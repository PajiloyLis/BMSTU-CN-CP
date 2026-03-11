#ifndef HTTP_H
#define HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

typedef struct http_result {
    char method[16];
    char uri[2048];
    int status_code;
    long long bytes_sent;
} http_result_t;

typedef struct http_response_plan {
    char method[16];
    char uri[2048];
    int status_code;
    char header[1024];
    size_t header_len;
    bool send_body;
    int file_fd;
    off_t file_size;
} http_response_plan_t;

int http_prepare_response(const char *raw, const char *docroot, http_response_plan_t *plan);
void http_response_plan_close(http_response_plan_t *plan);

#endif
