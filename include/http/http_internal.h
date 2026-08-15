#ifndef HTTP_INTERNAL_H
#define HTTP_INTERNAL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http/http.h"
#include "core/common.h"
#include "core/log.h"
#include "core/config.h"

#define HTTP_BUF_SIZE 262144  /* 请求缓冲：需容纳 header + DBC 文件上传 body */
#define HTTP_ROOT     PATH_WEBROOT
#define HTTP_URI_MAX  256

#define JSON_ADD(json, off, fmt, ...) do {     int _n = snprintf((json) + (off), sizeof(json) - (off), fmt, ##__VA_ARGS__);     if (_n < 0 || _n >= (int)(sizeof(json) - (off))) return;     (off) += _n; } while(0)

const char *http_mime_type(const char *path);
void http_send_response(int fd, int code, const char *status, const char *mime, const void *body, size_t len);
void http_handle_404(int fd, const char *path);
int  http_check_auth_user(const char *req, int fd);
int  http_check_auth_root(const char *req, int fd);
void http_serve_file(int fd, const char *uri);

void http_logs_handler(app_ctx_t *app, int fd, const char *method, const char *uri, const char *req_buf);
void http_system_api(app_ctx_t *app, int fd);
void http_can_status(app_ctx_t *app, int fd);
void http_can_toggle(app_ctx_t *app, int fd, const char *body);
void http_can_decoded(app_ctx_t *app, int fd);
void http_can_dbc_upload(app_ctx_t *app, int fd, const char *method, const char *uri, const char *body);
void http_config_get(app_ctx_t *app, int fd, const char *method, const char *uri, const char *req);
void http_config_post(app_ctx_t *app, int fd, const char *method, const char *uri, const char *body);
void http_reboot(app_ctx_t *app, int fd);
void http_shutdown(app_ctx_t *app, int fd);
void http_network_api(app_ctx_t *app, int fd);
void http_video_devices(app_ctx_t *app, int fd, const char *method, const char *uri, const char *req_buf);
void http_video_caps(app_ctx_t *app, int fd, const char *method, const char *uri, const char *req_buf);

#endif
