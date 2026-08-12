#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <linux/can.h>
#include "can/can_socket.h"
#include "watchdog/watchdog.h"
#include "core/common.h"
#include "core/log.h"

#define MAX_EVENTS 64

/* CAN 数据接收：使用独立 can_epfd 管理所有 CAN 接口 */

static void handle_can_input(app_ctx_t *app, int can_idx)
{
    can_ctx_t *ctx = app->can;
    /* 整个读 + 重连过程加锁，与 HTTP 配置热更新互斥，消除 fd 竞态 */
    pthread_mutex_lock(&app->can_mutex);
    int fd = ctx->ifaces[can_idx].sock_fd;
    while (1) {
        struct canfd_frame frame;
        int n = read(fd, &frame, sizeof(frame));
        if (n == (int)sizeof(frame) || n == (int)CAN_MTU) {
            log_info("CAN recv: %s id=%X len=%d",
                     ctx->ifaces[can_idx].ifname,
                     frame.can_id & CAN_EFF_MASK, frame.len);
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        } else {
            if (n < 0)
                log_error("can read %s: %s", ctx->ifaces[can_idx].ifname, strerror(errno));
            /* 尝试重连（已持锁，与 HTTP 配置热更新互斥） */
            can_socket_close(fd);
            int new_fd = can_socket_open(ctx->ifaces[can_idx].ifname,
                                          ctx->ifaces[can_idx].fd_mode);
            if (new_fd >= 0) {
                ctx->ifaces[can_idx].sock_fd = new_fd;
                for (int k = 0; k < ctx->ifaces[can_idx].filter_count; k++)
                    can_socket_set_filter(new_fd,
                        ctx->ifaces[can_idx].filters[k].id,
                        ctx->ifaces[can_idx].filters[k].mask);
                /* 重新注册到 CAN epoll，否则重连后该接口收不到事件 */
                struct epoll_event nev;
                nev.events = EPOLLIN;
                nev.data.u32 = (uint32_t)(can_idx + 1);
                epoll_ctl(ctx->epfd, EPOLL_CTL_ADD, new_fd, &nev);
                log_info("CAN %s reconnected", ctx->ifaces[can_idx].ifname);
            }
            break;
        }
    }
    pthread_mutex_unlock(&app->can_mutex);
}

void *can_task(void *arg)
{
    app_ctx_t *app = (app_ctx_t *)arg;
    can_ctx_t *ctx = app->can;
    if (!ctx) return NULL;

    for (int i = 0; i < ctx->count; i++) {
        struct epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.u32 = (uint32_t)(i + 1);
        epoll_ctl(ctx->epfd, EPOLL_CTL_ADD, ctx->ifaces[i].sock_fd, &ev);
    }
    log_info("can_task started (%d iface(s))", ctx->count);
    while (app->running) {
        struct epoll_event events[MAX_EVENTS];
        int nfds = epoll_wait(ctx->epfd, events, MAX_EVENTS, 500);
        if (nfds < 0) {
            if (errno == EINTR) { watchdog_feed(WD_CAN); continue; }
            log_error("can epoll_wait"); break;
        }
        watchdog_feed(WD_CAN);
        for (int i = 0; i < nfds; i++) {
            uint32_t tag = events[i].data.u32;
            if (tag < 1 || tag > (uint32_t)ctx->count) continue;
            handle_can_input(app, (int)(tag - 1));
        }
    }
    log_info("can_task stopped");
    return NULL;
}
