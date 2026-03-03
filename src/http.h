#ifndef HTTP_H
#define HTTP_H

typedef struct http_result {
    char method[16];
    char uri[2048];
    int status_code;
    long long bytes_sent;
} http_result_t;

int handle_http_request(int fd, const char *raw, const char *docroot, http_result_t *result);

#endif
