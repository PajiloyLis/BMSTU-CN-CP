#include "http.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define READ_BUFFER_SIZE 4096
#define IO_BUFFER_SIZE 16384
#define MAX_FILE_SIZE (128 * 1024 * 1024)

static const char k_response_400[] =
    "HTTP/1.1 400 Bad Request\r\n"
    "Content-Length: 0\r\n"
    "Connection: close\r\n"
    "\r\n";

static const char k_response_405[] =
    "HTTP/1.1 405 Method Not Allowed\r\n"
    "Allow: GET, HEAD\r\n"
    "Content-Length: 0\r\n"
    "Connection: close\r\n"
    "\r\n";

static const char k_response_403[] =
    "HTTP/1.1 403 Forbidden\r\n"
    "Content-Length: 0\r\n"
    "Connection: close\r\n"
    "\r\n";

static const char k_response_404[] =
    "HTTP/1.1 404 Not Found\r\n"
    "Content-Length: 0\r\n"
    "Connection: close\r\n"
    "\r\n";

static const char k_response_200_prefix[] =
    "HTTP/1.1 200 OK\r\n"
    "Connection: close\r\n";

static void copy_trunc(char *dst, size_t dst_sz, const char *src) {
    if (dst_sz == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '-';
        if (dst_sz > 1) {
            dst[1] = '\0';
        }
        return;
    }
    size_t len = strlen(src);
    if (len >= dst_sz) {
        len = dst_sz - 1;
    }
    if (len > 0) {
        memcpy(dst, src, len);
    }
    dst[len] = '\0';
}

static void set_result(http_result_t *result, const char *method, const char *uri, int status_code,
                       long long bytes_sent) {
    if (result == NULL) {
        return;
    }
    copy_trunc(result->method, sizeof(result->method), method);
    copy_trunc(result->uri, sizeof(result->uri), uri);
    result->status_code = status_code;
    result->bytes_sent = bytes_sent;
}

static int send_all(int fd, const char *buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t n = send(fd, buf + total, len - total, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        total += (size_t)n;
    }
    return 0;
}

static bool is_supported_version(const char *version) {
    return strcmp(version, "HTTP/1.0") == 0 || strcmp(version, "HTTP/1.1") == 0;
}

static int parse_request_line(const char *raw, char *method, size_t method_sz, char *uri,
                              size_t uri_sz, char *version, size_t version_sz) {
    const char *line_end = strstr(raw, "\r\n");
    if (line_end == NULL) {
        line_end = strchr(raw, '\n');
    }
    if (line_end == NULL) {
        return -1;
    }

    size_t line_len = (size_t)(line_end - raw);
    if (line_len == 0 || line_len >= READ_BUFFER_SIZE) {
        return -1;
    }

    char line[READ_BUFFER_SIZE];
    memcpy(line, raw, line_len);
    line[line_len] = '\0';

    int matched = sscanf(line, "%31s %2047s %31s", method, uri, version);
    if (matched != 3) {
        return -1;
    }
    if (!is_supported_version(version)) {
        return -1;
    }

    (void)method_sz;
    (void)uri_sz;
    (void)version_sz;
    return 0;
}

static const char *guess_content_type(const char *path) {
    const char *dot = strrchr(path, '.');
    if (dot == NULL) {
        return "application/octet-stream";
    }
    if (strcmp(dot, ".html") == 0 || strcmp(dot, ".htm") == 0) {
        return "text/html; charset=utf-8";
    }
    if (strcmp(dot, ".css") == 0) {
        return "text/css; charset=utf-8";
    }
    if (strcmp(dot, ".js") == 0) {
        return "application/javascript; charset=utf-8";
    }
    if (strcmp(dot, ".txt") == 0) {
        return "text/plain; charset=utf-8";
    }
    if (strcmp(dot, ".png") == 0) {
        return "image/png";
    }
    if (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jpeg") == 0) {
        return "image/jpeg";
    }
    if (strcmp(dot, ".gif") == 0) {
        return "image/gif";
    }
    if (strcmp(dot, ".svg") == 0) {
        return "image/svg+xml";
    }
    return "application/octet-stream";
}

static bool has_parent_segment(const char *uri_path) {
    const char *p = uri_path;
    while (*p != '\0') {
        while (*p == '/') {
            ++p;
        }
        const char *seg_start = p;
        while (*p != '/' && *p != '\0') {
            ++p;
        }
        size_t seg_len = (size_t)(p - seg_start);
        if (seg_len == 2 && seg_start[0] == '.' && seg_start[1] == '.') {
            return true;
        }
    }
    return false;
}

static int resolve_path(const char *docroot, const char *uri, char *out_path, size_t out_sz) {
    if (uri[0] != '/') {
        return -1;
    }

    char clean_uri[2048];
    strncpy(clean_uri, uri, sizeof(clean_uri) - 1);
    clean_uri[sizeof(clean_uri) - 1] = '\0';

    char *query = strchr(clean_uri, '?');
    if (query != NULL) {
        *query = '\0';
    }

    if (strchr(clean_uri, '\\') != NULL || has_parent_segment(clean_uri)) {
        return -1;
    }

    const char *resource = clean_uri;
    if (strcmp(clean_uri, "/") == 0) {
        resource = "/index.html";
    }

    int written = snprintf(out_path, out_sz, "%s%s", docroot, resource);
    if (written <= 0 || (size_t)written >= out_sz) {
        return -1;
    }
    return 0;
}

static int send_file_response(int fd, const char *path, bool is_head, http_result_t *result,
                              const char *method, const char *uri) {
    struct stat st;
    if (stat(path, &st) < 0) {
        if (errno == EACCES) {
            set_result(result, method, uri, 403, 0);
            return send_all(fd, k_response_403, strlen(k_response_403));
        }
        set_result(result, method, uri, 404, 0);
        return send_all(fd, k_response_404, strlen(k_response_404));
    }

    if (!S_ISREG(st.st_mode)) {
        set_result(result, method, uri, 403, 0);
        return send_all(fd, k_response_403, strlen(k_response_403));
    }

    if (access(path, R_OK) < 0) {
        set_result(result, method, uri, 403, 0);
        return send_all(fd, k_response_403, strlen(k_response_403));
    }

    if (st.st_size < 0 || st.st_size > MAX_FILE_SIZE) {
        set_result(result, method, uri, 403, 0);
        return send_all(fd, k_response_403, strlen(k_response_403));
    }

    const char *content_type = guess_content_type(path);
    char headers[512];
    int header_len =
        snprintf(headers, sizeof(headers), "%sContent-Type: %s\r\nContent-Length: %lld\r\n\r\n",
                 k_response_200_prefix, content_type, (long long)st.st_size);
    if (header_len <= 0 || (size_t)header_len >= sizeof(headers)) {
        return -1;
    }

    if (send_all(fd, headers, (size_t)header_len) < 0) {
        return -1;
    }

    if (is_head) {
        set_result(result, method, uri, 200, 0);
        return 0;
    }

    int file_fd = open(path, O_RDONLY);
    if (file_fd < 0) {
        return send_all(fd, k_response_403, strlen(k_response_403));
    }

    char io_buf[IO_BUFFER_SIZE];
    while (1) {
        ssize_t nread = read(file_fd, io_buf, sizeof(io_buf));
        if (nread < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(file_fd);
            return -1;
        }
        if (nread == 0) {
            break;
        }
        if (send_all(fd, io_buf, (size_t)nread) < 0) {
            close(file_fd);
            return -1;
        }
    }

    close(file_fd);
    set_result(result, method, uri, 200, (long long)st.st_size);
    return 0;
}

int handle_http_request(int fd, const char *raw, const char *docroot, http_result_t *result) {
    char method[32];
    char uri[2048];
    char version[32];
    char path[PATH_MAX];

    if (parse_request_line(raw, method, sizeof(method), uri, sizeof(uri), version,
                           sizeof(version)) < 0) {
        set_result(result, "-", "-", 400, 0);
        return send_all(fd, k_response_400, strlen(k_response_400));
    }

    if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0) {
        set_result(result, method, uri, 405, 0);
        return send_all(fd, k_response_405, strlen(k_response_405));
    }

    if (resolve_path(docroot, uri, path, sizeof(path)) < 0) {
        set_result(result, method, uri, 403, 0);
        return send_all(fd, k_response_403, strlen(k_response_403));
    }

    return send_file_response(fd, path, strcmp(method, "HEAD") == 0, result, method, uri);
}
