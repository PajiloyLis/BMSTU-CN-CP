#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "log.h"
#include "worker.h"

#define DEFAULT_PORT 8080
#define DEFAULT_WORKERS 4
#define LISTEN_BACKLOG 128
#define DEFAULT_DOCROOT "./www"
#define DEFAULT_LOG_PATH "./server.log"
#define DEFAULT_ERROR_LOG_PATH "./server.error.log"

volatile sig_atomic_t g_stop = 0;
static volatile sig_atomic_t g_child_changed = 0;

static void on_stop_signal(int signo) {
    (void)signo;
    g_stop = 1;
}

static void on_sigchld(int signo) {
    (void)signo;
    g_child_changed = 1;
}

static int parse_positive_number(const char *value, int *out) {
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (value[0] == '\0' || end == value || *end != '\0') {
        return -1;
    }
    if (parsed <= 0 || parsed > 65535) {
        return -1;
    }
    *out = (int)parsed;
    return 0;
}

static int create_listen_socket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        logger_log_error("master", "socket failed", errno);
        return -1;
    }

    int reuse = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        logger_log_error("master", "setsockopt(SO_REUSEADDR) failed", errno);
        close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        logger_log_error("master", "bind failed", errno);
        close(fd);
        return -1;
    }

    if (listen(fd, LISTEN_BACKLOG) < 0) {
        logger_log_error("master", "listen failed", errno);
        close(fd);
        return -1;
    }

    return fd;
}

static pid_t create_worker(int listen_fd) {
    pid_t pid = fork();
    if (pid < 0) {
        logger_log_error("master", "fork failed", errno);
        return -1;
    }
    if (pid == 0) {
        worker_loop(listen_fd);
        close(listen_fd);
        _exit(0);
    }
    return pid;
}

static int find_worker_slot(pid_t *workers, int worker_count, pid_t pid) {
    for (int i = 0; i < worker_count; ++i) {
        if (workers[i] == pid) {
            return i;
        }
    }
    return -1;
}

static void install_signal_handlers(void) {
    struct sigaction sa_stop;
    memset(&sa_stop, 0, sizeof(sa_stop));
    sa_stop.sa_handler = on_stop_signal;
    sigemptyset(&sa_stop.sa_mask);
    sigaction(SIGINT, &sa_stop, NULL);
    sigaction(SIGTERM, &sa_stop, NULL);

    struct sigaction sa_chld;
    memset(&sa_chld, 0, sizeof(sa_chld));
    sa_chld.sa_handler = on_sigchld;
    sigemptyset(&sa_chld.sa_mask);
    sa_chld.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa_chld, NULL);
}

int main(int argc, char **argv) {
    int port = DEFAULT_PORT;
    int worker_count = DEFAULT_WORKERS;
    const char *docroot = DEFAULT_DOCROOT;
    const char *log_path = DEFAULT_LOG_PATH;
    const char *error_log_path = DEFAULT_ERROR_LOG_PATH;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            if (parse_positive_number(argv[++i], &port) < 0 || port > 65535) {
                fprintf(stderr, "Invalid port value\n");
                return 1;
            }
        } else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            if (parse_positive_number(argv[++i], &worker_count) < 0) {
                fprintf(stderr, "Invalid workers value\n");
                return 1;
            }
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            docroot = argv[++i];
        } else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
            log_path = argv[++i];
        } else if (strcmp(argv[i], "-e") == 0 && i + 1 < argc) {
            error_log_path = argv[++i];
        } else {
            fprintf(stderr,
                    "Usage: %s [-p port] [-w workers] [-d docroot] [-l log_path] "
                    "[-e error_log_path]\n",
                    argv[0]);
            return 1;
        }
    }

    if (logger_set_error_path(error_log_path) < 0) {
        fprintf(stderr, "Invalid error log path\n");
        return 1;
    }
    if (logger_set_access_path(log_path) < 0) {
        logger_log_error("master", "invalid access log path", EINVAL);
        fprintf(stderr, "Invalid log path\n");
        return 1;
    }
    if (worker_set_docroot(docroot) < 0) {
        logger_log_error("master", "invalid docroot", EINVAL);
        fprintf(stderr, "Invalid docroot\n");
        return 1;
    }
    if (worker_set_log_path(log_path) < 0) {
        logger_log_error("master", "failed to set access log path in worker", EINVAL);
        fprintf(stderr, "Invalid log path\n");
        return 1;
    }
    if (worker_set_error_log_path(error_log_path) < 0) {
        logger_log_error("master", "failed to set error log path in worker", EINVAL);
        fprintf(stderr, "Invalid error log path\n");
        return 1;
    }

    int listen_fd = create_listen_socket(port);
    if (listen_fd < 0) {
        return 1;
    }

    pid_t *workers = calloc((size_t)worker_count, sizeof(pid_t));
    if (workers == NULL) {
        logger_log_error("master", "calloc failed", errno);
        close(listen_fd);
        return 1;
    }

    install_signal_handlers();

    printf("[master %d] listening on port %d, workers=%d, docroot=%s, log=%s, error_log=%s\n",
           getpid(), port, worker_count, docroot, log_path, error_log_path);
    fflush(stdout);

    for (int i = 0; i < worker_count; ++i) {
        pid_t pid = create_worker(listen_fd);
        if (pid < 0) {
            g_stop = 1;
            worker_count = i;
            break;
        }
        workers[i] = pid;
    }

    while (!g_stop) {
        pause();

        if (g_child_changed) {
            g_child_changed = 0;
            while (1) {
                int status = 0;
                pid_t dead = waitpid(-1, &status, WNOHANG);
                if (dead <= 0) {
                    break;
                }

                int slot = find_worker_slot(workers, worker_count, dead);
                if (slot >= 0) {
                    workers[slot] = -1;
                    if (!g_stop) {
                        pid_t replacement = create_worker(listen_fd);
                        if (replacement > 0) {
                            workers[slot] = replacement;
                            printf("[master] respawn worker: %d -> %d\n", dead, replacement);
                            fflush(stdout);
                        } else {
                            logger_log_error("master", "failed to respawn worker", errno);
                            g_stop = 1;
                        }
                    }
                }
            }
        }
    }

    printf("[master] stopping\n");
    fflush(stdout);

    for (int i = 0; i < worker_count; ++i) {
        if (workers[i] > 0) {
            kill(workers[i], SIGTERM);
        }
    }
    while (waitpid(-1, NULL, 0) > 0) {}

    close(listen_fd);
    free(workers);
    return 0;
}
