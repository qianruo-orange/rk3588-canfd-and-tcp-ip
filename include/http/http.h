/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HTTP_H
#define HTTP_H

#define HTTP_DEFAULT_PORT 80

typedef struct { int tcp_port; int wd_sec; char log_dir[256]; } http_runtime_t;

struct app_config_t;
int  http_server_start(void *arg);
void http_server_stop(void *arg);
void *http_server_task(void *arg);

#endif
