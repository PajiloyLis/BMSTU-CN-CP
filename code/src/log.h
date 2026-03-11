#ifndef LOG_H
#define LOG_H

int logger_set_access_path(const char *path);
int logger_set_error_path(const char *path);
int logger_log_request(const char *client, const char *method, const char *uri, int status_code,
                       long long bytes_sent, long long duration_ms);
int logger_log_error(const char *component, const char *message, int errnum);

#endif
