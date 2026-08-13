/**
 * http_api_config.c — 运行时配置读写（通过 app_ctx_t 指针访问运行时状态）
 */

#include "http/http_internal.h"
#include "can/can_socket.h"
#include "net/tcp_server.h"
#include "video/video_stream.h"
#include "watchdog/watchdog.h"
#include <sys/epoll.h>
#include <unistd.h>

void http_config_get(app_ctx_t *app, int fd, const char *method, const char *uri, const char *req)
{
    (void)method; (void)uri; (void)req;
    if (!app || !app->cfg || !app->can || !app->tcp) {
        http_send_response(fd, 500, "Error", "text/plain", "", 0);
        return;
    }
    struct app_config_t *cfg = app->cfg;
    can_ctx_t *can = app->can;
    tcp_ctx_t *tcp = app->tcp;

    char json[4096]; int off = 0;
#define J(fmt, ...) do { \
        int _n = snprintf(json + off, sizeof(json) - off, fmt, ##__VA_ARGS__); \
        if (_n < 0 || _n >= (int)(sizeof(json) - off)) break; \
        off += _n; \
    } while(0)

    J("{\"cans\":{");
    for (int i = 0; i < can->count; i++) {
        can_iface_t *iface = &can->ifaces[i];
        J("%s\"%s\":{\"bitrate\":%d,\"fd\":\"%s\",\"dbitrate\":%d,\"up\":\"%s\",\"filters\":[",
            i > 0 ? "," : "", iface->ifname,
            iface->bitrate, iface->fd_mode ? "on" : "off", iface->dbitrate,
            iface->up ? "on" : "off");
        for (int k = 0; k < iface->filter_count; k++)
            J("%s{\"id\":\"%X\",\"mask\":\"%X\"}",
                k > 0 ? "," : "", iface->filters[k].id, iface->filters[k].mask);
        J("]}");
    }
    J("},");

    J("\"tcp_port\":%d,\"max_clients\":%d", tcp->port, tcp->max_clients);
    if (cfg->video_device[0]) J(",\"video_device\":\"%s\"", cfg->video_device);
    J(",\"video_width\":%d,\"video_height\":%d", cfg->video_width, cfg->video_height);
    J("}");

#undef J
    http_send_response(fd, 200, "OK", "application/json", json, off);
}

/* 从 POST 表单中查找 key */
static const char *form_find(char keys[][64], char vals[][256], int count, const char *key)
{
    for (int i = 0; i < count; i++)
        if (strcmp(keys[i], key) == 0 && vals[i][0])
            return vals[i];
    return NULL;
}

static int find_iface(can_ctx_t *can, const char *name)
{
    for (int i = 0; i < can->count; i++)
        if (strcmp(can->ifaces[i].ifname, name) == 0) return i;
    return -1;
}

void http_config_post(app_ctx_t *app, int fd, const char *method, const char *uri, const char *body)
{
    (void)method; (void)uri;
    if (!body || !app || !app->cfg || !app->can || !app->tcp) {
        http_send_response(fd, 400, "Bad Request", "text/plain", "", 0);
        return;
    }

    /* 整个配置应用过程加锁：串行化并发 POST，并与 can_task 的 CAN 重连互斥 */
    pthread_mutex_lock(&app->can_mutex);

    /* 跳过 HTTP 头部，定位 POST body */
    const char *p = strstr(body, "\r\n\r\n");
    if (!p) p = strstr(body, "\n\n");
    if (p) p += (p[0] == '\r') ? 4 : 2;
    else p = body;

    char keys[64][64], vals[64][256]; int count = 0;
    while (*p && count < 64) {
        const char *eq = strchr(p, '='), *amp = strchr(p, '&');
        if (!eq) break;
        int klen = (int)(eq - p); if (klen > 63) klen = 63;
        memcpy(keys[count], p, klen); keys[count][klen] = '\0';
        const char *vs = eq + 1;
        int vlen = amp ? (int)(amp - vs) : (int)strlen(vs);
        if (vlen > 254) vlen = 254;
        int vi = 0;
        for (int i = 0; i < vlen && vi < 254; i++) {
            if (vs[i] == '%' && i+2 < vlen) {
                unsigned int h = 0;
                if (sscanf(vs+i+1, "%2x", &h) == 1) vals[count][vi++] = (char)h;
                i += 2;
            } else if (vs[i] == '+') vals[count][vi++]=' ';
            else vals[count][vi++]=vs[i];
        }
        vals[count][vi]='\0'; count++;
        p = amp ? amp + 1 : vs + vlen;
    }

    struct app_config_t *cfg = app->cfg;
    can_ctx_t *can = app->can;
    tcp_ctx_t *tcp = app->tcp;

    log_info("config POST: parsed %d fields", count);

    for (int i = 0; i < CAN_MAX_IFACES; i++) {
        char kn[32];
        snprintf(kn, sizeof(kn), "ifname%d", i);
        const char *ifn = form_find(keys, vals, count, kn);
        if (!ifn) continue;
        int idx = find_iface(can, ifn);
        if (idx < 0) continue;

        snprintf(kn, sizeof(kn), "bitrate%d", i);
        const char *v = form_find(keys, vals, count, kn);
        if (v) can->ifaces[idx].bitrate = atoi(v);

        snprintf(kn, sizeof(kn), "fd%d", i);
        v = form_find(keys, vals, count, kn);
        if (v) can->ifaces[idx].fd_mode = (strcmp(v, "on") == 0) ? 1 : 0;

        snprintf(kn, sizeof(kn), "dbitrate%d", i);
        v = form_find(keys, vals, count, kn);
        if (v) can->ifaces[idx].dbitrate = atoi(v);

        snprintf(kn, sizeof(kn), "up%d", i);
        v = form_find(keys, vals, count, kn);
        if (v) can->ifaces[idx].up = (strcmp(v, "on") == 0) ? 1 : 0;

        can->ifaces[idx].filter_count = 0;
        for (int f = 0; f < 16; f++) {
            char kn_id[32], kn_mask[32];
            snprintf(kn_id,   sizeof(kn_id),   "filter_id_%d_%d", i, f);
            snprintf(kn_mask, sizeof(kn_mask), "filter_mask_%d_%d", i, f);
            const char *fid  = form_find(keys, vals, count, kn_id);
            const char *fmsk = form_find(keys, vals, count, kn_mask);
            if (fid && fmsk) {
                unsigned int id, mask;
                if (sscanf(fid, "%x", &id) == 1 && sscanf(fmsk, "%x", &mask) == 1) {
                    can_filter_t *flt = &can->ifaces[idx].filters[can->ifaces[idx].filter_count++];
                    flt->id = (canid_t)id;
                    flt->mask = (canid_t)mask;
                }
            }
        }
    }

    /* -- 应用 CAN 参数到系统接口 -- */
    for (int i = 0; i < can->count; i++) {
        can_iface_t *iface = &can->ifaces[i];
        int old_fd = iface->sock_fd;

        watchdog_feed_self("http");   /* 配置可能执行 ip 命令较慢，避免 HTTP 看门狗误杀 */

        /* 从 CAN epoll 移除旧 fd */
        if (old_fd >= 0 && can->epfd >= 0)
            epoll_ctl(can->epfd, EPOLL_CTL_DEL, old_fd, NULL);

        /* 重新配置并打开 */
        if (can_socket_configure(iface->ifname, iface->bitrate, iface->dbitrate,
                                  iface->fd_mode, iface->restart_ms, iface->up) < 0) {
            log_error("config: CAN %s reconfigure failed", iface->ifname);
            if (old_fd >= 0 && can->epfd >= 0)
                epoll_ctl(can->epfd, EPOLL_CTL_ADD, old_fd, NULL);
            continue;
        }

        int new_fd = can_socket_open(iface->ifname, iface->fd_mode);
        if (new_fd < 0) {
            log_error("config: CAN %s reopen failed", iface->ifname);
            continue;
        }

        /* 关闭旧 socket，更新为新 fd */
        if (old_fd >= 0) can_socket_close(old_fd);
        iface->sock_fd = new_fd;

        /* 重新设置过滤器 */
        for (int k = 0; k < iface->filter_count; k++)
            can_socket_set_filter(new_fd, iface->filters[k].id, iface->filters[k].mask);

        /* 重新加入 CAN epoll */
        if (can->epfd >= 0) {
            struct epoll_event ev;
            ev.events = EPOLLIN;
            ev.data.u32 = (uint32_t)(i + 1);
            epoll_ctl(can->epfd, EPOLL_CTL_ADD, new_fd, &ev);
        }
        log_info("config: CAN %s reconfigured (bitrate=%d, fd=%s, up=%s)",
                 iface->ifname, iface->bitrate,
                 iface->fd_mode ? "on" : "off",
                 iface->up ? "on" : "off");
    }

    /* -- 应用 TCP 端口 -- */
    const char *v = form_find(keys, vals, count, "tcp_port");
    if (v) {
        int new_port = atoi(v);
        if (new_port > 0 && new_port != tcp->port) {
            /* 加锁串行化与 tcp_task 的 listen 事件管理，避免 fd/epoll 竞态 */
            pthread_mutex_lock(&tcp->client_mutex);
            if (tcp->listen_fd >= 0 && tcp->epfd >= 0)
                epoll_ctl(tcp->epfd, EPOLL_CTL_DEL, tcp->listen_fd, NULL);
            if (tcp->listen_fd >= 0) close(tcp->listen_fd);

            tcp->listen_fd = tcp_listen(new_port);
            tcp->port = new_port;

            if (tcp->listen_fd >= 0 && tcp->epfd >= 0) {
                struct epoll_event ev;
                ev.events = EPOLLIN;
                ev.data.u32 = 0;
                if (epoll_ctl(tcp->epfd, EPOLL_CTL_ADD, tcp->listen_fd, &ev) < 0)
                    log_error("config: epoll add listen failed");
            }
            pthread_mutex_unlock(&tcp->client_mutex);
            log_info("config: TCP port changed to %d", new_port);
        }
    }

    v = form_find(keys, vals, count, "max_clients");
    if (v) {
        int mc = atoi(v);
        if (mc < 1) mc = 1;
        if (mc > TCP_MAX_CLIENTS) mc = TCP_MAX_CLIENTS;   /* clamp，防越界 */
        tcp->max_clients = mc;
    }

    v = form_find(keys, vals, count, "video_device");
    int vid_changed = (v != NULL);
    if (v) safe_strncpy(cfg->video_device, sizeof(cfg->video_device), v);
    v = form_find(keys, vals, count, "video_width");
    if (v) { vid_changed = 1; cfg->video_width = parse_int_clamped(v, 1, 4096, cfg->video_width > 0 ? cfg->video_width : 640); }
    v = form_find(keys, vals, count, "video_height");
    if (v) { vid_changed = 1; cfg->video_height = parse_int_clamped(v, 1, 4096, cfg->video_height > 0 ? cfg->video_height : 480); }

    log_info("config: applied to runtime, saving to file");
    config_save(app);
    http_send_response(fd, 200, "OK", "text/plain", "saved", 5);

    /* 视频参数变更后重启视频流 */
    if (vid_changed) video_stream_restart();

    pthread_mutex_unlock(&app->can_mutex);
}
