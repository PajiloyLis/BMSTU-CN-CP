#define _POSIX_C_SOURCE 200809L

#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define DEFAULT_ACCESS_LOG_PATH "./server.log"
#define DEFAULT_ERROR_LOG_PATH "./server.error.log"

static char g_access_log_path[PATH_MAX] = DEFAULT_ACCESS_LOG_PATH;
static char g_error_log_path[PATH_MAX] = DEFAULT_ERROR_LOG_PATH;

static int build_timestamp(char *out, size_t out_sz) {
    time_t now = time(NULL);
    struct tm tm_utc;
    if (gmtime_r(&now, &tm_utc) == NULL) {
        return -1;
    }
    if (strftime(out, out_sz, "%Y-%m-%dT%H:%M:%SZ", &tm_utc) == 0) {
        return -1;
    }
    return 0;
}

static int set_path(char *dst, size_t dst_sz, const char *src) {
    if (src == NULL || src[0] == '\0') {
        return -1;
    }
    if (strlen(src) >= dst_sz) {
        return -1;
    }
    strcpy(dst, src);
    return 0;
}

int logger_set_access_path(const char *path) {
    return set_path(g_access_log_path, sizeof(g_access_log_path), path);
}

int logger_set_error_path(const char *path) {
    return set_path(g_error_log_path, sizeof(g_error_log_path), path);
}

static int append_line(const char *path, const char *line, size_t line_len) {
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        return -1;
    }

    size_t total = 0;
    while (total < (size_t)line_len) {
        ssize_t n = write(fd, line + total, (size_t)line_len - total);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            return -1;
        }
        total += (size_t)n;
    }

    close(fd);
    return 0;
}

int logger_log_request(const char *client, const char *method, const char *uri, int status_code,
                       long long bytes_sent, long long duration_ms) {
    char ts[32];
    if (build_timestamp(ts, sizeof(ts)) < 0) {
        return -1;
    }

    char line[1024];
    int line_len = snprintf(
        line, sizeof(line), "%s pid=%ld client=%s method=%s uri=%s status=%d bytes=%lld duration_ms=%lld\n",
        ts, (long)getpid(), client, method, uri, status_code, bytes_sent, duration_ms);
    if (line_len <= 0 || (size_t)line_len >= sizeof(line)) {
        return -1;
    }

    return append_line(g_access_log_path, line, (size_t)line_len);
}

int logger_log_error(const char *component, const char *message, int errnum) {
    char ts[32];
    if (build_timestamp(ts, sizeof(ts)) < 0) {
        return -1;
    }

    const char *comp = component != NULL ? component : "-";
    const char *msg = message != NULL ? message : "-";
    const char *errtext = strerror(errnum);

    char line[1024];
    int line_len = snprintf(line, sizeof(line),
                            "%s pid=%ld component=%s message=%s errno=%d error=%s\n", ts,
                            (long)getpid(), comp, msg, errnum, errtext);
    if (line_len <= 0 || (size_t)line_len >= sizeof(line)) {
        return -1;
    }

    return append_line(g_error_log_path, line, (size_t)line_len);
}
