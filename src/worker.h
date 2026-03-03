#ifndef WORKER_H
#define WORKER_H

#include <signal.h>

extern volatile sig_atomic_t g_stop;

int worker_set_docroot(const char *docroot);
int worker_set_log_path(const char *path);
int worker_set_error_log_path(const char *path);
void worker_loop(int listen_fd);

#endif
