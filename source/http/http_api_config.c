/**
 * http_api_config.c — 运行时配置读写（通过 app_ctx_t 指针访问运行时状态）
 */

#include "http/http_internal.h"
#include "can/can_socket.h"
#include "tcp/tcp_server.h"
#include "video/video_stream.h"
#include "watchdog/watchdog.h"
#include <sys/epoll.h>
#include <unistd.h>
#include <ctype.h>

void http_config_get(app_ctx_t *app, int fd, const char *method, const char *uri, const char *req)
{
    (void)method; (void)uri; (void)req;
    if (!app || !app->cfg || !app->can || !app->tcp) {
        http_err(fd, 500, "Error", NULL);
        return;
    }
    struct app_config_t *cfg = app->cfg;
    can_ctx_t *can = app->can;
    tcp_ctx_t *tcp = app->tcp;

    char json[4096]; int off = 0;

    JSON_ADD(json, off, "{\"cans\":{");
    for (int i = 0; i < can->count; i++) {
        can_iface_t *iface = &can->ifaces[i];
        JSON_ADD(json, off, "%s\"%s\":{\"bitrate\":%d,\"fd\":\"%s\",\"dbitrate\":%d,\"up\":\"%s\",\"filters\":[",
            i > 0 ? "," : "", iface->ifname,
            iface->bitrate, iface->fd_mode ? "on" : "off", iface->dbitrate,
            iface->up ? "on" : "off");
        for (int k = 0; k < iface->filter_count; k++)
            JSON_ADD(json, off, "%s{\"id\":\"%X\",\"mask\":\"%X\"}",
                k > 0 ? "," : "", iface->filters[k].id, iface->filters[k].mask);
        JSON_ADD(json, off, "]}");
    }
    JSON_ADD(json, off, "},");

    JSON_ADD(json, off, "\"tcp_port\":%d,\"max_clients\":%d,\"tcp_bind\":\"%s\"", tcp->port, tcp->max_clients, tcp->bind_ifname);
    if (cfg->video_device[0]) JSON_ADD(json, off, ",\"video_device\":\"%s\"", cfg->video_device);
    JSON_ADD(json, off, ",\"video_width\":%d,\"video_height\":%d", cfg->video_width, cfg->video_height);
    JSON_ADD(json, off, "}");

    http_ok_json(fd, json, (size_t)off);
}

/* 表单字段容量：8 接口 × 5 基础项 + 8×16 过滤器 × 2 + 6 全局项 = 302，留余量 */
#define MAX_FORM_FIELDS 320

void http_config_post(app_ctx_t *app, int fd, const char *method, const char *uri, const char *body)
{
    (void)method; (void)uri;
    if (!body || !app || !app->cfg || !app->can || !app->tcp) {
        http_err(fd, 400, "Bad Request", NULL);
        return;
    }

    /* 整个配置应用过程加锁：串行化并发 POST，并与 CAN 重连互斥 */
    pthread_mutex_lock(&app->can_mutex);

    /* 解析 URL 编码表单（公共实现，自动跳过 HTTP 头） */
    http_form_field_t fields[MAX_FORM_FIELDS];
    int count = http_form_parse(body, fields, MAX_FORM_FIELDS);
    if (count == MAX_FORM_FIELDS)
        LOG_ERROR("config POST: fields exceed %d, tail ignored", MAX_FORM_FIELDS);

    struct app_config_t *cfg = app->cfg;
    can_ctx_t *can = app->can;
    tcp_ctx_t *tcp = app->tcp;

    LOG_INFO("config POST: parsed %d fields", count);

    for (int i = 0; i < CAN_MAX_IFACES; i++) {
        char kn[32];
        snprintf(kn, sizeof(kn), "ifname%d", i);
        const char *ifn = http_form_find(fields, count, kn);
        if (!ifn) continue;
        if (!ifname_valid(ifn)) { LOG_ERROR("config: invalid CAN ifname '%s'", ifn); continue; }
        int idx = can_iface_index(can->ifaces, can->count, ifn);
        if (idx < 0) {
            /* 表单新增了运行态中没有的 CAN 接口：动态加入，并在下方统一重配置 */
            if (can->count >= CAN_MAX_IFACES) {
                LOG_ERROR("config: cannot add CAN '%s', max %d ifaces", ifn, CAN_MAX_IFACES);
                continue;
            }
            idx = can->count++;
            can_iface_t *ni = &can->ifaces[idx];
            memset(ni, 0, sizeof(*ni));
            ni->sock_fd = -1;
            safe_strncpy(ni->ifname, sizeof(ni->ifname), ifn);
            ni->bitrate = 500000; ni->dbitrate = 2000000;
            ni->fd_mode = 1; ni->up = 1; ni->restart_ms = 0;
            /* 新增接口的收发队列必须初始化，否则 can_send/can_recv 任务访问未初始化队列 */
            can_queue_init(&ni->txq);
            can_queue_init(&ni->rxq);
            LOG_INFO("config: adding new CAN interface '%s' (runtime idx %d)", ifn, idx);
        }

        snprintf(kn, sizeof(kn), "bitrate%d", i);
        const char *v = http_form_find(fields, count, kn);
        if (v) can->ifaces[idx].bitrate = atoi(v);

        snprintf(kn, sizeof(kn), "fd%d", i);
        v = http_form_find(fields, count, kn);
        if (v) can->ifaces[idx].fd_mode = (strcmp(v, "on") == 0) ? 1 : 0;

        snprintf(kn, sizeof(kn), "dbitrate%d", i);
        v = http_form_find(fields, count, kn);
        if (v) can->ifaces[idx].dbitrate = atoi(v);

        snprintf(kn, sizeof(kn), "up%d", i);
        v = http_form_find(fields, count, kn);
        if (v) can->ifaces[idx].up = (strcmp(v, "on") == 0) ? 1 : 0;

        can->ifaces[idx].filter_count = 0;
        for (int f = 0; f < 16; f++) {
            char kn_id[32], kn_mask[32];
            snprintf(kn_id,   sizeof(kn_id),   "filter_id_%d_%d", i, f);
            snprintf(kn_mask, sizeof(kn_mask), "filter_mask_%d_%d", i, f);
            const char *fid  = http_form_find(fields, count, kn_id);
            const char *fmsk = http_form_find(fields, count, kn_mask);
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

        /* 从 CAN 接收 epoll 移除旧 fd */
        if (old_fd >= 0 && can->recv_epfd >= 0)
            epoll_ctl(can->recv_epfd, EPOLL_CTL_DEL, old_fd, NULL);

        /* 重新配置并打开 */
        if (can_socket_configure(iface->ifname, iface->bitrate, iface->dbitrate,
                                  iface->fd_mode, iface->restart_ms, iface->up) < 0) {
            LOG_ERROR("config: CAN %s reconfigure failed", iface->ifname);
            if (old_fd >= 0 && can->recv_epfd >= 0)
                epoll_ctl(can->recv_epfd, EPOLL_CTL_ADD, old_fd, NULL);
            continue;
        }

        int new_fd = can_socket_open(iface->ifname, iface->fd_mode);
        if (new_fd < 0) {
            LOG_ERROR("config: CAN %s reopen failed", iface->ifname);
            /* 保持旧 socket 继续工作：重新注册到接收 epoll，
               否则该通道会静默失联（fd 有效但已不在 epoll 中） */
            if (old_fd >= 0 && can->recv_epfd >= 0) {
                struct epoll_event ev;
                ev.events = EPOLLIN;
                ev.data.u32 = (uint32_t)(i + 1);
                epoll_ctl(can->recv_epfd, EPOLL_CTL_ADD, old_fd, &ev);
            }
            continue;
        }

        /* 关闭旧 socket，更新为新 fd */
        if (old_fd >= 0) can_socket_close(old_fd);
        iface->sock_fd = new_fd;

        /* 重新设置过滤器 */
        for (int k = 0; k < iface->filter_count; k++)
            can_socket_set_filter(new_fd, iface->filters[k].id, iface->filters[k].mask);

        /* 重新加入 CAN 接收 epoll */
        if (can->recv_epfd >= 0) {
            struct epoll_event ev;
            ev.events = EPOLLIN;
            ev.data.u32 = (uint32_t)(i + 1);
            epoll_ctl(can->recv_epfd, EPOLL_CTL_ADD, new_fd, &ev);
        }
        LOG_INFO("config: CAN %s reconfigured (bitrate=%d, fd=%s, up=%s)",
                 iface->ifname, iface->bitrate,
                 iface->fd_mode ? "on" : "off",
                 iface->up ? "on" : "off");
    }

    /* -- 应用 TCP 监听配置（端口 / 绑定网卡） -- */
    const char *pv = http_form_find(fields, count, "tcp_port");
    const char *bv = http_form_find(fields, count, "tcp_bind");

    int  want_port = pv ? atoi(pv) : tcp->port;
    char want_bind[IFNAMSIZ];
    if (bv) safe_strncpy(want_bind, sizeof(want_bind), bv);
    else   safe_strncpy(want_bind, sizeof(want_bind), tcp->bind_ifname);

    if (want_port <= 0) want_port = tcp->port;
    int changed = (want_port != tcp->port) || (strcmp(want_bind, tcp->bind_ifname) != 0);

    if (changed) {
        /* 加锁串行化与 tcp_task 的 listen 事件管理，避免 fd/epoll 竞态 */
        pthread_mutex_lock(&tcp->client_mutex);
        if (tcp->listen_fd >= 0 && tcp->epfd >= 0)
            epoll_ctl(tcp->epfd, EPOLL_CTL_DEL, tcp->listen_fd, NULL);
        if (tcp->listen_fd >= 0) close(tcp->listen_fd);

        tcp->listen_fd = tcp_listen(want_port, want_bind);
        if (tcp->listen_fd >= 0) {
            tcp->port = want_port;
            safe_strncpy(tcp->bind_ifname, sizeof(tcp->bind_ifname), want_bind);
        }

        if (tcp->listen_fd >= 0 && tcp->epfd >= 0) {
            struct epoll_event ev;
            ev.events = EPOLLIN;
            ev.data.u32 = 0;
            if (epoll_ctl(tcp->epfd, EPOLL_CTL_ADD, tcp->listen_fd, &ev) < 0)
                LOG_ERROR("config: epoll add listen failed");
        }
        pthread_mutex_unlock(&tcp->client_mutex);
        LOG_INFO("config: TCP listen changed to %s:%d", want_bind[0] ? want_bind : "*", want_port);
    }

    const char *v = http_form_find(fields, count, "max_clients");
    if (v) {
        int mc = atoi(v);
        if (mc < 1) mc = 1;
        if (mc > TCP_MAX_CLIENTS) mc = TCP_MAX_CLIENTS;   /* clamp，防越界 */
        tcp->max_clients = mc;
    }

    int vid_changed = 0;
    v = http_form_find(fields, count, "video_device");
    if (v && strcmp(v, cfg->video_device) != 0) {
        vid_changed = 1;
        safe_strncpy(cfg->video_device, sizeof(cfg->video_device), v);
    }
    v = http_form_find(fields, count, "video_width");
    if (v) {
        int nw = parse_int_clamped(v, 1, 4096, cfg->video_width > 0 ? cfg->video_width : 640);
        if (nw != cfg->video_width) { vid_changed = 1; cfg->video_width = nw; }
    }
    v = http_form_find(fields, count, "video_height");
    if (v) {
        int nh = parse_int_clamped(v, 1, 4096, cfg->video_height > 0 ? cfg->video_height : 480);
        if (nh != cfg->video_height) { vid_changed = 1; cfg->video_height = nh; }
    }

    LOG_INFO("config: applied to runtime, saving to file");
    config_save(app);
    http_ok_text(fd, "saved");

    /* 视频参数变更后重启视频流 */
    if (vid_changed) video_stream_restart();

    pthread_mutex_unlock(&app->can_mutex);
}
