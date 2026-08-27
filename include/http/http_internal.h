#ifndef HTTP_INTERNAL_H
#define HTTP_INTERNAL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http/http.h"
#include "core/common.h"
#include "core/log.h"
#include "core/config.h"
#include "http/http_util.h"

#define HTTP_BUF_SIZE 262144  /* 请求缓冲：需容纳 header + DBC 文件上传 body */
#define HTTP_ROOT     PATH_WEBROOT
#define HTTP_URI_MAX  256

#define JSON_ADD(json, off, fmt, ...) do {     int _n = snprintf((json) + (off), sizeof(json) - (off), fmt, ##__VA_ARGS__);     if (_n < 0 || _n >= (int)(sizeof(json) - (off))) return;     (off) += _n; } while(0)

void http_send_response(int fd, int code, const char *status, const char *mime, const void *body, size_t len);
void http_handle_404(int fd, const char *path);
void http_serve_file(int fd, const char *uri);
/* 流式发送数据源（文件）到客户端：静态文件/日志下载/日志打包共用。
   服务器接管 src 所有权，发送完毕或连接关闭时负责 fclose；unlink_after
   非空时发送完成后删除该文件（用于临时打包文件）。 */
void http_serve_stream(int fd, const char *mime, const char *extra_hdr,
                       FILE *src, size_t size, const char *unlink_after);
/* 完整写入客户端 socket：连接上下文内追加到输出缓冲（非阻塞），否则阻塞写 */
int http_write_all(int fd, const void *data, size_t len);

void http_logs_handler(app_ctx_t *app, int fd, const char *method, const char *uri, const char *req_buf);
void http_system_api(app_ctx_t *app, int fd);
void http_can_status(app_ctx_t *app, int fd);
void http_can_ifaces(app_ctx_t *app, int fd);
void http_can_toggle(app_ctx_t *app, int fd, const char *body);
void http_can_decoded(app_ctx_t *app, int fd);
void http_can_decoded_tx(app_ctx_t *app, int fd);
void http_can_send(app_ctx_t *app, int fd, const char *method, const char *uri, const char *body);
void http_can_rx(app_ctx_t *app, int fd);
void http_can_dbc_upload(app_ctx_t *app, int fd, const char *method, const char *uri, const char *body);
/* AI 文件上传（/api/ai/upload?type=model|names）：body 已由 HTTP 层落盘到 tmp_path，
   本接口负责校验、原子替换 config/ 下正式文件、更新配置并热重载推理池 */
void http_ai_upload(app_ctx_t *app, int fd, const char *uri, const char *tmp_path);
void http_config_get(app_ctx_t *app, int fd, const char *method, const char *uri, const char *req);
void http_config_post(app_ctx_t *app, int fd, const char *method, const char *uri, const char *body);
void http_reboot(app_ctx_t *app, int fd);
void http_shutdown(app_ctx_t *app, int fd);
void http_network_api(app_ctx_t *app, int fd);
void http_network_ifaces(app_ctx_t *app, int fd, const char *method, const char *uri, const char *req_buf);
void http_video_devices(app_ctx_t *app, int fd, const char *method, const char *uri, const char *req_buf);
void http_video_caps(app_ctx_t *app, int fd, const char *method, const char *uri, const char *req_buf);
void http_rec_handler(app_ctx_t *app, int fd, const char *method, const char *uri, const char *req_buf);

#endif
