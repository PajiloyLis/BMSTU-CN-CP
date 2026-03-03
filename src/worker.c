#define _POSIX_C_SOURCE 200809L

#include "worker.h"

#include "http.h"
#include "log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define READ_BUFFER_SIZE 4096
#define DEFAULT_DOCROOT "./www"
#define DEFAULT_LOG_PATH "./server.log"

typedef struct connection {
    bool active;
    struct sockaddr_storage addr;
    socklen_t addr_len;
} connection_t;

static char g_docroot[PATH_MAX] = DEFAULT_DOCROOT;

int worker_set_docroot(const char *docroot) {
    if (docroot == NULL || docroot[0] == '\0') {
        return -1;
    }
    if (strlen(docroot) >= sizeof(g_docroot)) {
        return -1;
    }
    strcpy(g_docroot, docroot);
    return 0;
}

int worker_set_log_path(const char *path) {
    return logger_set_access_path(path);
}

int worker_set_error_log_path(const char *path) {
    return logger_set_error_path(path);
}

static long long diff_ms(const struct timespec *start, const struct timespec *end) {
    long long sec = (long long)(end->tv_sec - start->tv_sec);
    long long nsec = (long long)(end->tv_nsec - start->tv_nsec);
    return sec * 1000LL + nsec / 1000000LL;
}

static void format_client_addr(const struct sockaddr_storage *addr, socklen_t addr_len, char *out,
                               size_t out_sz) {
    (void)addr_len;
    if (addr->ss_family == AF_INET) {
        const struct sockaddr_in *a4 = (const struct sockaddr_in *)addr;
        char ip[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &a4->sin_addr, ip, sizeof(ip)) == NULL) {
            snprintf(out, out_sz, "-");
            return;
        }
        snprintf(out, out_sz, "%s:%u", ip, (unsigned)ntohs(a4->sin_port));
        return;
    }

    if (addr->ss_family == AF_INET6) {
        const struct sockaddr_in6 *a6 = (const struct sockaddr_in6 *)addr;
        char ip[INET6_ADDRSTRLEN];
        if (inet_ntop(AF_INET6, &a6->sin6_addr, ip, sizeof(ip)) == NULL) {
            snprintf(out, out_sz, "-");
            return;
        }
        snprintf(out, out_sz, "[%s]:%u", ip, (unsigned)ntohs(a6->sin6_port));
        return;
    }

    snprintf(out, out_sz, "-");
}

void worker_loop(int listen_fd) {
    printf("[worker %d] started\n", getpid());
    fflush(stdout);

    connection_t conns[FD_SETSIZE];
    memset(conns, 0, sizeof(conns));

    int max_fd = listen_fd;

    while (!g_stop) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(listen_fd, &readfds);

        for (int fd = 0; fd <= max_fd; ++fd) {
            if (conns[fd].active) {
                FD_SET(fd, &readfds);
            }
        }

        int ready = select(max_fd + 1, &readfds, NULL, NULL, NULL);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            logger_log_error("worker", "select failed", errno);
            break;
        }

        if (FD_ISSET(listen_fd, &readfds)) {
            struct sockaddr_storage client_addr;
            socklen_t client_len = sizeof(client_addr);
            int client_fd =
                accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
            if (client_fd >= 0) {
                if (client_fd >= FD_SETSIZE) {
                    close(client_fd);
                } else {
                    conns[client_fd].active = true;
                    conns[client_fd].addr = client_addr;
                    conns[client_fd].addr_len = client_len;
                    if (client_fd > max_fd) {
                        max_fd = client_fd;
                    }
                }
            }
        }

        for (int fd = 0; fd <= max_fd; ++fd) {
            if (!conns[fd].active) {
                continue;
            }
            if (!FD_ISSET(fd, &readfds)) {
                continue;
            }

            char buf[READ_BUFFER_SIZE];
            ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
            if (n <= 0) {
                close(fd);
                conns[fd].active = false;
                continue;
            }
            buf[n] = '\0';

            struct timespec t_start;
            struct timespec t_end;
            clock_gettime(CLOCK_MONOTONIC, &t_start);

            http_result_t result;
            memset(&result, 0, sizeof(result));

            if (handle_http_request(fd, buf, g_docroot, &result) < 0) {
                logger_log_error("worker", "handle_http_request failed", errno);
            }
            clock_gettime(CLOCK_MONOTONIC, &t_end);

            char client[128];
            format_client_addr(&conns[fd].addr, conns[fd].addr_len, client, sizeof(client));

            if (result.status_code == 0) {
                snprintf(result.method, sizeof(result.method), "-");
                snprintf(result.uri, sizeof(result.uri), "-");
                result.status_code = 500;
                result.bytes_sent = 0;
            }

            if (logger_log_request(client, result.method, result.uri, result.status_code,
                                   result.bytes_sent, diff_ms(&t_start, &t_end)) < 0) {
                logger_log_error("worker", "access log write failed", errno);
            }

            close(fd);
            conns[fd].active = false;
        }

        while (max_fd > listen_fd && !conns[max_fd].active) {
            --max_fd;
        }
    }

    for (int fd = 0; fd <= max_fd; ++fd) {
        if (conns[fd].active) {
            close(fd);
            conns[fd].active = false;
        }
    }

    printf("[worker %d] stopping\n", getpid());
    fflush(stdout);
}
