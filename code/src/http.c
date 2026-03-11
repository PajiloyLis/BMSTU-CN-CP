#include "http.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define READ_BUFFER_SIZE 4096
#define MAX_FILE_SIZE (128 * 1024 * 1024)

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

static void plan_init(http_response_plan_t *plan) {
    memset(plan, 0, sizeof(*plan));
    copy_trunc(plan->method, sizeof(plan->method), "-");
    copy_trunc(plan->uri, sizeof(plan->uri), "-");
    plan->status_code = 500;
    plan->file_fd = -1;
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

static const char *status_text(int status_code) {
    switch (status_code) {
        case 200:
            return "OK";
        case 400:
            return "Bad Request";
        case 403:
            return "Forbidden";
        case 404:
            return "Not Found";
        case 405:
            return "Method Not Allowed";
        default:
            return "Internal Server Error";
    }
}

static int build_error_header(http_response_plan_t *plan, int status_code) {
    const char *extra = "";
    if (status_code == 405) {
        extra = "Allow: GET, HEAD\r\n";
    }
    int n = snprintf(plan->header, sizeof(plan->header),
                     "HTTP/1.1 %d %s\r\nConnection: close\r\n%sContent-Length: 0\r\n\r\n",
                     status_code, status_text(status_code), extra);
    if (n <= 0 || (size_t)n >= sizeof(plan->header)) {
        return -1;
    }
    plan->header_len = (size_t)n;
    plan->status_code = status_code;
    plan->send_body = false;
    return 0;
}

static int prepare_file_response(const char *path, bool is_head, http_response_plan_t *plan) {
    struct stat st;
    if (stat(path, &st) < 0) {
        if (errno == EACCES) {
            return build_error_header(plan, 403);
        }
        return build_error_header(plan, 404);
    }

    if (!S_ISREG(st.st_mode)) {
        return build_error_header(plan, 403);
    }

    if (access(path, R_OK) < 0) {
        return build_error_header(plan, 403);
    }

    if (st.st_size < 0 || st.st_size > MAX_FILE_SIZE) {
        return build_error_header(plan, 403);
    }

    const char *content_type = guess_content_type(path);
    int n = snprintf(plan->header, sizeof(plan->header),
                     "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: %s\r\n"
                     "Content-Length: %lld\r\n\r\n",
                     content_type, (long long)st.st_size);
    if (n <= 0 || (size_t)n >= sizeof(plan->header)) {
        return -1;
    }
    plan->header_len = (size_t)n;
    plan->status_code = 200;
    plan->file_size = st.st_size;

    if (is_head) {
        plan->send_body = false;
        plan->file_fd = -1;
        return 0;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return build_error_header(plan, 403);
    }
    plan->file_fd = fd;
    plan->send_body = true;
    return 0;
}

int http_prepare_response(const char *raw, const char *docroot, http_response_plan_t *plan) {
    char method[32];
    char uri[2048];
    char version[32];
    char path[PATH_MAX];
    bool is_head = false;

    if (plan == NULL) {
        return -1;
    }
    plan_init(plan);

    if (parse_request_line(raw, method, sizeof(method), uri, sizeof(uri), version,
                           sizeof(version)) < 0) {
        return build_error_header(plan, 400);
    }
    copy_trunc(plan->method, sizeof(plan->method), method);
    copy_trunc(plan->uri, sizeof(plan->uri), uri);

    if (strcmp(method, "GET") == 0) {
        is_head = false;
    } else if (strcmp(method, "HEAD") == 0) {
        is_head = true;
    } else {
        return build_error_header(plan, 405);
    }

    if (resolve_path(docroot, uri, path, sizeof(path)) < 0) {
        return build_error_header(plan, 403);
    }

    return prepare_file_response(path, is_head, plan);
}

void http_response_plan_close(http_response_plan_t *plan) {
    if (plan == NULL) {
        return;
    }
    if (plan->file_fd >= 0) {
        close(plan->file_fd);
        plan->file_fd = -1;
    }
}
