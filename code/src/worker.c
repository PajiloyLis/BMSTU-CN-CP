#define _POSIX_C_SOURCE 200809L

#include "worker.h"

#include "http.h"
#include "log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define READ_BUFFER_SIZE 4096
#define WRITE_BUFFER_SIZE 16384
#define DEFAULT_DOCROOT "./www"
#define DEFAULT_LOG_PATH "./server.log"

typedef enum connection_state {
    CONN_READING = 0,
    CONN_WRITING = 1
} connection_state_t;

typedef struct connection {
    bool active;
    connection_state_t state;
    struct sockaddr_storage addr;
    socklen_t addr_len;
    char request_buf[READ_BUFFER_SIZE];
    size_t request_len;
    http_response_plan_t plan;
    size_t header_sent;
    char write_buf[WRITE_BUFFER_SIZE];
    size_t write_buf_len;
    size_t write_buf_sent;
    bool body_done;
    bool timer_started;
    struct timespec started_at;
    long long body_bytes_sent;
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

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return -1;
    }
    return 0;
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

static void connection_init(connection_t *conn) {
    memset(conn, 0, sizeof(*conn));
    conn->plan.file_fd = -1;
}

static void copy_small(char *dst, size_t dst_sz, const char *src) {
    if (dst_sz == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, dst_sz, "%s", src);
}

static bool request_line_complete(const connection_t *conn) {
    return strstr(conn->request_buf, "\r\n") != NULL || strchr(conn->request_buf, '\n') != NULL;
}

static void close_connection(int fd, connection_t *conn) {
    http_response_plan_close(&conn->plan);
    close(fd);
    connection_init(conn);
}

static void finalize_and_close_connection(int fd, connection_t *conn) {
    struct timespec ended_at;
    clock_gettime(CLOCK_MONOTONIC, &ended_at);

    char client[128];
    format_client_addr(&conn->addr, conn->addr_len, client, sizeof(client));

    http_result_t result;
    memset(&result, 0, sizeof(result));
    copy_small(result.method, sizeof(result.method), conn->plan.method);
    copy_small(result.uri, sizeof(result.uri), conn->plan.uri);
    result.status_code = conn->plan.status_code;
    result.bytes_sent = conn->body_bytes_sent;

    if (logger_log_request(client, result.method, result.uri, result.status_code, result.bytes_sent,
                           diff_ms(&conn->started_at, &ended_at)) < 0) {
        logger_log_error("worker", "access log write failed", errno);
    }

    close_connection(fd, conn);
}

static int prepare_connection_response(connection_t *conn) {
    if (!conn->timer_started) {
        clock_gettime(CLOCK_MONOTONIC, &conn->started_at);
        conn->timer_started = true;
    }
    if (http_prepare_response(conn->request_buf, g_docroot, &conn->plan) < 0) {
        return -1;
    }
    conn->state = CONN_WRITING;
    conn->header_sent = 0;
    conn->write_buf_len = 0;
    conn->write_buf_sent = 0;
    conn->body_done = !conn->plan.send_body;
    conn->body_bytes_sent = 0;
    return 0;
}

void worker_loop(int listen_fd) {
    printf("[worker %d] started\n", getpid());
    fflush(stdout);

    connection_t *conns = calloc(FD_SETSIZE, sizeof(connection_t));
    if (conns == NULL) {
        logger_log_error("worker", "calloc connections failed", errno);
        return;
    }
    for (int i = 0; i < FD_SETSIZE; ++i) {
        connection_init(&conns[i]);
    }

    if (set_nonblocking(listen_fd) < 0) {
        logger_log_error("worker", "failed to set listen socket nonblocking", errno);
    }

    int max_fd = listen_fd;

    while (!g_stop) {
        fd_set readfds;
        fd_set writefds;
        FD_ZERO(&readfds);
        FD_ZERO(&writefds);
        FD_SET(listen_fd, &readfds);

        for (int fd = 0; fd <= max_fd; ++fd) {
            if (conns[fd].active) {
                if (conns[fd].state == CONN_READING) {
                    FD_SET(fd, &readfds);
                } else {
                    FD_SET(fd, &writefds);
                }
            }
        }

        int ready = select(max_fd + 1, &readfds, &writefds, NULL, NULL);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            logger_log_error("worker", "select failed", errno);
            break;
        }

        if (FD_ISSET(listen_fd, &readfds)) {
            while (1) {
                struct sockaddr_storage client_addr;
                socklen_t client_len = sizeof(client_addr);
                int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
                if (client_fd < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    }
                    if (errno != EINTR) {
                        logger_log_error("worker", "accept failed", errno);
                    }
                    break;
                }

                if (client_fd >= FD_SETSIZE) {
                    close(client_fd);
                    continue;
                }
                if (set_nonblocking(client_fd) < 0) {
                    logger_log_error("worker", "failed to set client socket nonblocking", errno);
                    close(client_fd);
                    continue;
                }

                connection_init(&conns[client_fd]);
                conns[client_fd].active = true;
                conns[client_fd].state = CONN_READING;
                conns[client_fd].addr = client_addr;
                conns[client_fd].addr_len = client_len;
                if (client_fd > max_fd) {
                    max_fd = client_fd;
                }
            }
        }

        for (int fd = 0; fd <= max_fd; ++fd) {
            if (!conns[fd].active) {
                continue;
            }
            if (conns[fd].state == CONN_READING && FD_ISSET(fd, &readfds)) {
                size_t left = sizeof(conns[fd].request_buf) - 1 - conns[fd].request_len;
                if (left == 0) {
                    if (prepare_connection_response(&conns[fd]) < 0) {
                        logger_log_error("worker", "prepare response failed for full request buffer",
                                         errno);
                        close_connection(fd, &conns[fd]);
                    }
                    continue;
                }

                ssize_t n = recv(fd, conns[fd].request_buf + conns[fd].request_len, left, 0);
                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                        continue;
                    }
                    logger_log_error("worker", "recv failed", errno);
                    close_connection(fd, &conns[fd]);
                    continue;
                }
                if (n == 0) {
                    close_connection(fd, &conns[fd]);
                    continue;
                }

                conns[fd].request_len += (size_t)n;
                conns[fd].request_buf[conns[fd].request_len] = '\0';
                if (request_line_complete(&conns[fd])) {
                    if (prepare_connection_response(&conns[fd]) < 0) {
                        logger_log_error("worker", "http_prepare_response failed", errno);
                        close_connection(fd, &conns[fd]);
                    }
                }
            }

            if (conns[fd].active && conns[fd].state == CONN_WRITING && FD_ISSET(fd, &writefds)) {
                if (conns[fd].header_sent < conns[fd].plan.header_len) {
                    const char *h = conns[fd].plan.header + conns[fd].header_sent;
                    size_t len = conns[fd].plan.header_len - conns[fd].header_sent;
                    ssize_t n = send(fd, h, len, MSG_NOSIGNAL);
                    if (n < 0) {
                        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                            logger_log_error("worker", "send header failed", errno);
                            close_connection(fd, &conns[fd]);
                        }
                        continue;
                    }
                    conns[fd].header_sent += (size_t)n;
                }

                if (conns[fd].header_sent == conns[fd].plan.header_len && !conns[fd].body_done) {
                    if (conns[fd].write_buf_sent == conns[fd].write_buf_len) {
                        ssize_t nread = read(conns[fd].plan.file_fd, conns[fd].write_buf,
                                             sizeof(conns[fd].write_buf));
                        if (nread < 0) {
                            if (errno == EINTR) {
                                continue;
                            }
                            logger_log_error("worker", "read file chunk failed", errno);
                            close_connection(fd, &conns[fd]);
                            continue;
                        }
                        if (nread == 0) {
                            conns[fd].body_done = true;
                        } else {
                            conns[fd].write_buf_len = (size_t)nread;
                            conns[fd].write_buf_sent = 0;
                        }
                    }

                    if (!conns[fd].body_done && conns[fd].write_buf_sent < conns[fd].write_buf_len) {
                        const char *p = conns[fd].write_buf + conns[fd].write_buf_sent;
                        size_t len = conns[fd].write_buf_len - conns[fd].write_buf_sent;
                        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
                        if (n < 0) {
                            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                                logger_log_error("worker", "send body chunk failed", errno);
                                close_connection(fd, &conns[fd]);
                            }
                            continue;
                        }
                        conns[fd].write_buf_sent += (size_t)n;
                        conns[fd].body_bytes_sent += n;
                    }
                }

                if (conns[fd].active && conns[fd].header_sent == conns[fd].plan.header_len &&
                    conns[fd].body_done) {
                    finalize_and_close_connection(fd, &conns[fd]);
                }
            }
        }

        while (max_fd > listen_fd && !conns[max_fd].active) {
            --max_fd;
        }
    }

    for (int fd = 0; fd <= max_fd; ++fd) {
        if (conns[fd].active) {
            close_connection(fd, &conns[fd]);
        }
    }
    free(conns);

    printf("[worker %d] stopping\n", getpid());
    fflush(stdout);
}
