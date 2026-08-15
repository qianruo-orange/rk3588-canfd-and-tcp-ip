/**
 * http_api_can.c — CAN 接口状态查询与开关控制 API。
 */

#include <ctype.h>
#include <linux/if.h>
#include <netlink/netlink.h>
#include <netlink/socket.h>
#include <netlink/route/link.h>
#include <netlink/route/link/can.h>
#include <linux/can/netlink.h>

#include "http/http_internal.h"
#include "core/data_flow.h"
#include "can/can_socket.h"

void http_can_decoded(app_ctx_t *app, int fd)
{
    (void)app;
    char json[8192];
    int n = data_flow_recent_decoded_json(json, sizeof(json));
    if (n < 0) n = 0;
    http_send_response(fd, 200, "OK", "application/json", json, (size_t)n);
}

void http_can_status(app_ctx_t *app, int fd)
{
    (void)app;
    char json[2048] = "[";
    int off = 1;
    /* 带边界检查的 JSON 追加：缓冲区写满后安全截断，不越界 */
#define JADD(fmt, ...) do { \
        int _n = snprintf(json + off, sizeof(json) - off, fmt, ##__VA_ARGS__); \
        if (_n < 0) { off = (int)sizeof(json); } \
        else if (_n >= (int)(sizeof(json) - off)) { off = (int)sizeof(json); } \
        else { off += _n; } \
    } while (0)

    struct nl_sock *sk = nl_socket_alloc();
    if (!sk) goto done;
    if (nl_connect(sk, NETLINK_ROUTE) < 0) { nl_socket_free(sk); goto done; }

    for (int idx = 1; ; idx++) {
        struct rtnl_link *link;
        if (rtnl_link_get_kernel(sk, idx, NULL, &link) != 0) break;
        if (!link) break;

        const char *kind = rtnl_link_get_type(link);
        if (!kind || strcmp(kind, "can") != 0) { rtnl_link_put(link); continue; }

        const char *name = rtnl_link_get_name(link);
        int flags = rtnl_link_get_flags(link);
        int up = (flags & IFF_UP) ? 1 : 0;
        uint32_t bitrate = 0;
        rtnl_link_can_get_bitrate(link, &bitrate);
        uint32_t ctrlmode = 0;
        rtnl_link_can_get_ctrlmode(link, &ctrlmode);
        int fd_mode = (ctrlmode & CAN_CTRLMODE_FD) ? 1 : 0;

        uint32_t dbitrate = 0;
        char sysfs[128];
        snprintf(sysfs, sizeof(sysfs), "/sys/class/net/%s/can_data_bitrate", name);
        FILE *sfp = fopen(sysfs, "r");
        if (sfp) { if (fscanf(sfp, "%u", &dbitrate) != 1) dbitrate = 0; fclose(sfp); }

        JADD("%s{\"name\":\"%s\",\"up\":%d,\"bitrate\":%u,\"dbitrate\":%u,\"fd\":%d}",
             off > 1 ? "," : "", name, up, bitrate, dbitrate, fd_mode);
        rtnl_link_put(link);
    }
    nl_socket_free(sk);

done:
    JADD("]");
#undef JADD
    http_send_response(fd, 200, "OK", "application/json", json, off);
}

/* 从 URL 编码 body 中提取 key 的值，并进行 %XX / '+' 解码 */
static int url_get_param(const char *body, const char *key, char *out, int out_size)
{
    int klen = (int)strlen(key);
    const char *p = strstr(body, key);
    if (!p) return 0;
    p += klen;
    int o = 0;
    while (*p && *p != '&' && o < out_size - 1) {
        if (*p == '%' && p[1] && p[2]) {
            unsigned int h = 0;
            if (sscanf(p + 1, "%2x", &h) == 1) out[o++] = (char)h;
            else out[o++] = *p;
            p += 3;
        } else if (*p == '+') { out[o++] = ' '; p++; }
        else { out[o++] = *p; p++; }
    }
    out[o] = '\0';
    return o;
}

/* 校验 CAN 接口名：字母数字 / 下划线 / 连字符 */
static int ifname_valid(const char *name)
{
    if (!name || !*name) return 0;
    size_t len = strlen(name);
    if (len >= IFNAMSIZ) return 0;
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        if (!isalnum((unsigned char)c) && c != '_' && c != '-') return 0;
    }
    return 1;
}

void http_can_toggle(app_ctx_t *app, int fd, const char *body)
{
    char ifname[32] = {0}, action[8] = {0};
    if (!body) { http_send_response(fd, 400, "Bad Request", "text/plain", "", 0); return; }

    url_get_param(body, "ifname=", ifname, sizeof(ifname));
    url_get_param(body, "action=", action, sizeof(action));

    if (!ifname_valid(ifname)) {
        http_send_response(fd, 400, "Bad Request", "text/plain", "bad ifname", 10);
        return;
    }
    if (strcmp(action, "up") != 0 && strcmp(action, "down") != 0) {
        http_send_response(fd, 400, "Bad Request", "text/plain", "bad action", 10);
        return;
    }

    struct nl_sock *sk = nl_socket_alloc();
    if (!sk) { http_send_response(fd, 500, "Error", "text/plain", "", 0); return; }
    if (nl_connect(sk, NETLINK_ROUTE) < 0) {
        nl_socket_free(sk);
        http_send_response(fd, 500, "Error", "text/plain", "", 0);
        return;
    }

    struct rtnl_link *ln = rtnl_link_alloc();
    if (!ln) { nl_socket_free(sk); http_send_response(fd, 500, "Error", "text/plain", "", 0); return; }
    rtnl_link_set_name(ln, ifname);

    /* 串行化接口状态操作与 CAN 配置热更新 / can_task 重连 */
    pthread_mutex_lock(&app->can_mutex);
    if (strcmp(action, "up") == 0)
        rtnl_link_set_flags(ln, IFF_UP);
    else
        rtnl_link_set_flags(ln, 0);

    int rc = rtnl_link_change(sk, ln, ln, 0);
    pthread_mutex_unlock(&app->can_mutex);
    rtnl_link_put(ln);
    nl_socket_free(sk);

    if (rc < 0) {
        http_send_response(fd, 500, "Error", "text/plain", "link change failed", 19);
        return;
    }

    char msg[64];
    snprintf(msg, sizeof(msg), "{\"result\":\"%s %s\"}", ifname, action);
    http_send_response(fd, 200, "OK", "application/json", msg, strlen(msg));
}

/* DBC 文件上传：POST /api/can/dbc?ifname=can0，body 为 DBC 文本内容 */
void http_can_dbc_upload(app_ctx_t *app, int fd, const char *method, const char *uri, const char *body)
{
    (void)method;
    if (!app || !app->cfg || !app->can || !uri || !body) {
        http_send_response(fd, 400, "Bad Request", "text/plain", "", 0);
        return;
    }

    /* 从 query string 解析目标通道名 */
    char ifname[32] = {0};
    const char *q = strchr(uri, '?');
    if (!q || !url_get_param(q, "ifname=", ifname, sizeof(ifname)) || !ifname_valid(ifname)) {
        http_send_response(fd, 400, "Bad Request", "text/plain", "bad ifname", 10);
        return;
    }

    can_ctx_t *can = app->can;
    int idx = -1;
    for (int i = 0; i < can->count; i++)
        if (strcmp(can->ifaces[i].ifname, ifname) == 0) { idx = i; break; }
    if (idx < 0) {
        http_send_response(fd, 404, "Not Found", "text/plain", "iface not found", 15);
        return;
    }

    /* 定位 body 与长度 */
    long cl = 0;
    const char *cl_hdr = strstr(body, "Content-Length:");
    if (!cl_hdr) cl_hdr = strstr(body, "content-length:");
    if (cl_hdr) {
        char *end = NULL;
        cl = strtol(cl_hdr + 15, &end, 10);
        if (end == cl_hdr + 15 || cl < 0) cl = 0;
    }
    const char *sep = strstr(body, "\r\n\r\n");
    if (!sep) sep = strstr(body, "\n\n");
    const char *content = sep ? sep + (sep[0] == '\r' ? 4 : 2) : body;
    long content_len = cl > 0 ? cl : (long)strlen(content);
    if (content_len <= 0 || content_len > 256 * 1024) {
        http_send_response(fd, 413, "Payload Too Large", "text/plain", "empty or too large", 19);
        return;
    }

    /* 落盘到 config/dbc_<ifname>.dbc */
    char path[320];
    snprintf(path, sizeof(path), "config/dbc_%s.dbc", ifname);
    FILE *fp = fopen(path, "w");
    if (!fp) {
        http_send_response(fd, 500, "Error", "text/plain", "cannot write file", 17);
        return;
    }
    size_t w = fwrite(content, 1, (size_t)content_len, fp);
    fclose(fp);
    if (w != (size_t)content_len) {
        http_send_response(fd, 500, "Error", "text/plain", "write failed", 12);
        return;
    }

    /* 重新加载该通道 DBC（与解码互斥） */
    pthread_mutex_lock(&app->dbc_mutex);
    int rc = dbc_load(&app->dbcs[idx], path);
    int msg_count = app->dbcs[idx].msg_count;
    int sig_count = app->dbcs[idx].sig_count;
    pthread_mutex_unlock(&app->dbc_mutex);

    if (rc < 0) {
        http_send_response(fd, 400, "Bad Request", "text/plain", "invalid dbc", 12);
        return;
    }

    /* 更新运行时配置并落盘 */
    safe_strncpy(can->ifaces[idx].dbc_path, sizeof(can->ifaces[idx].dbc_path), path);
    config_save(app);

    char msg[160];
    snprintf(msg, sizeof(msg),
             "{\"result\":\"ok\",\"ifname\":\"%s\",\"messages\":%d,\"signals\":%d}",
             ifname, msg_count, sig_count);
    http_send_response(fd, 200, "OK", "application/json", msg, strlen(msg));
}
