#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <linux/can.h>
#include "net/tcp_server.h"
#include "watchdog/watchdog.h"
#include "core/common.h"
#include "core/log.h"

#define MAX_EVENTS 64

/* TCP 数据收发：使用独立 tcp_epfd 管理 listen + 客户端读写 */

static int write_all(int fd, const void *buf, int len)
{
    int total = 0;
    const char *p = (const char *)buf;
    while (total < len) {
        ssize_t n = write(fd, p + total, len - total);
        if (n > 0) { total += (int)n; }
        else if (n == 0) { return -1; }
        else { if (errno == EINTR) continue; if (errno == EAGAIN || errno == EWOULDBLOCK) return total; return -1; }
    }
    return total;
}

static int handle_tcp_input(tcp_ctx_t *ctx, int client_idx)
{
    client_t *c = &ctx->clients[client_idx];
    if (c->fd < 0) return 1;
    char dummy[256];
    int n = read(c->fd, dummy, sizeof(dummy));
    if (n == 0) return 1;
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return 1;
    return 0;
}

static int flush_client(tcp_ctx_t *ctx, int client_idx)
{
    client_t *c = &ctx->clients[client_idx];
    if (c->fd < 0) return 1;
    if (c->wlen == 0) return 0;   /* 无待发数据；写事件由下行数据入口按需启用 */
    int ret = write_all(c->fd, c->wbuf, c->wlen);
    if (ret < 0) return 1;
    if (ret == c->wlen) { c->wlen = 0; }
    else if (ret > 0) { memmove(c->wbuf, c->wbuf + ret, c->wlen - ret); c->wlen -= ret; }
    return 0;
}

void *tcp_task(void *arg)
{
    app_ctx_t *app = (app_ctx_t *)arg;
    tcp_ctx_t *ctx = app->tcp;
    if (!ctx) return NULL;

    /* listen fd：EPOLLIN 触发 accept */
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.u32 = 0;
    if (ctx->listen_fd >= 0)
        epoll_ctl(ctx->epfd, EPOLL_CTL_ADD, ctx->listen_fd, &ev);

    /* 已有客户端：注册读事件 */
    pthread_mutex_lock(&ctx->client_mutex);
    for (int i = 0; i < TCP_MAX_CLIENTS; i++) {
        if (ctx->clients[i].fd < 0) continue;
        ev.events = EPOLLIN;
        ev.data.u32 = (uint32_t)(0x80000000 | (i + 1));
        epoll_ctl(ctx->epfd, EPOLL_CTL_ADD, ctx->clients[i].fd, &ev);
    }
    pthread_mutex_unlock(&ctx->client_mutex);

    log_info("tcp_task started");
    while (app->running) {
        struct epoll_event events[MAX_EVENTS];
        int nfds = epoll_wait(ctx->epfd, events, MAX_EVENTS, 500);
        if (nfds < 0) {
            if (errno == EINTR) { watchdog_feed_thread(pthread_self()); continue; }
            log_error("tcp epoll_wait"); break;
        }
        watchdog_feed_thread(pthread_self());
        for (int i = 0; i < nfds; i++) {
            uint32_t tag = events[i].data.u32;
            if (tag == 0) {
                while (1) { int fd = tcp_accept(ctx->listen_fd); if (fd < 0) break; tcp_client_add(ctx, fd); }
                continue;
            }
            int client_idx = (int)(tag & 0x7FFFFFFF) - 1;
            if (client_idx < 0 || client_idx >= TCP_MAX_CLIENTS) continue;
            pthread_mutex_lock(&ctx->client_mutex);
            int dead = 0;
            if (events[i].events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR))
                dead = handle_tcp_input(ctx, client_idx);
            if (!dead && (events[i].events & EPOLLOUT))
                dead = flush_client(ctx, client_idx);
            pthread_mutex_unlock(&ctx->client_mutex);
            if (dead) tcp_client_del(ctx, client_idx);
        }
    }
    log_info("tcp_task stopped");
    return NULL;
}
