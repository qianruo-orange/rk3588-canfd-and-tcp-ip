#define _GNU_SOURCE   /* 暴露 SO_REUSEPORT 等 GNU/Linux 扩展 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "net/tcp_server.h"
#include "core/common.h"
#include "core/data_flow.h"
#include "core/log.h"
#include "watchdog/watchdog.h"

int tcp_listen(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { log_error("tcp socket"); return -1; }
    int optval = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
        log_error("tcp setsockopt SO_REUSEADDR: %s", strerror(errno));
        close(fd);
        return -1;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval)) < 0) {
        log_error("tcp setsockopt SO_REUSEPORT: %s", strerror(errno));
        close(fd);
        return -1;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        log_error("tcp bind :%d: %s", port, strerror(errno));
        close(fd); return -1;
    }
    if (listen(fd, SOMAXCONN) < 0) {
        log_error("tcp listen :%d: %s", port, strerror(errno));
        close(fd); return -1;
    }
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        log_error("tcp fcntl O_NONBLOCK: %s", strerror(errno));
        close(fd); return -1;
    }
    log_info("TCP listening on port %d", port);
    return fd;
}

int tcp_accept(int listen_fd)
{
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &addr_len);
    if (client_fd < 0) {
        /* 非阻塞监听 fd 下 EAGAIN/EWOULDBLOCK 属正常无连接，不刷错误日志 */
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            log_error("tcp accept: %s", strerror(errno));
        return -1;
    }
    int flags = fcntl(client_fd, F_GETFL, 0);
    if (flags < 0 || fcntl(client_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        log_error("tcp client fcntl O_NONBLOCK: %s", strerror(errno));
        close(client_fd);
        return -1;
    }
    char ip_str[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str)) == NULL)
        safe_strncpy(ip_str, sizeof(ip_str), "?");
    log_info("TCP client connected: %s:%d  fd=%d", ip_str, ntohs(client_addr.sin_port), client_fd);
    return client_fd;
}

/* 超时收发共用的 epoll 实例：复用避免每次临时创建/销毁；
   互斥保护以支持多线程调用（当前主要被 tcp_task 在 client_mutex 下调用） */
static pthread_mutex_t g_io_epoll_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_io_epfd = -1;

static int tcp_io_epoll_wait(int fd, uint32_t events, int timeout_ms)
{
    pthread_mutex_lock(&g_io_epoll_mutex);

    if (g_io_epfd < 0) {
        g_io_epfd = epoll_create1(EPOLL_CLOEXEC);
        if (g_io_epfd < 0) {
            pthread_mutex_unlock(&g_io_epoll_mutex);
            return -1;
        }
    }

    struct epoll_event ev;
    ev.events = events;
    ev.data.fd = fd;

    if (epoll_ctl(g_io_epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        /* 已有残留注册（并发调用或 fd 号复用），改为 MOD 即可 */
        if (errno == EEXIST)
            epoll_ctl(g_io_epfd, EPOLL_CTL_MOD, fd, &ev);
    }

    int rc = epoll_wait(g_io_epfd, &ev, 1, timeout_ms < 0 ? 0 : timeout_ms);
    /* 用后即删，保持实例干净；fd 已关闭时 DEL 失败无害 */
    epoll_ctl(g_io_epfd, EPOLL_CTL_DEL, fd, NULL);
    pthread_mutex_unlock(&g_io_epoll_mutex);
    return rc;
}

ssize_t tcp_recv_data(int fd, void *buf, size_t len, int timeout_ms)
{
    if (fd < 0 || !buf || len == 0) {
        errno = EINVAL;
        return -1;
    }

    int rc = tcp_io_epoll_wait(fd, EPOLLIN, timeout_ms);
    if (rc < 0) return -1;
    if (rc == 0) return 0;

    return recv(fd, buf, len, 0);
}

ssize_t tcp_send_data(int fd, const void *buf, size_t len, int timeout_ms)
{
    if (fd < 0 || !buf || len == 0) {
        errno = EINVAL;
        return -1;
    }

    int rc = tcp_io_epoll_wait(fd, EPOLLOUT, timeout_ms);
    if (rc < 0) return -1;
    if (rc == 0) return 0;

    return send(fd, buf, len, 0);
}

static int handle_tcp_input(app_ctx_t *app, tcp_ctx_t *ctx, int client_idx)
{
    client_t *c = &ctx->clients[client_idx];
    if (c->fd < 0) return 1;
    char buf[WBUF_SIZE];
    ssize_t n = tcp_recv_data(c->fd, buf, sizeof(buf) - 1, 10);
    if (n == 0) return 1;
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return 1;
    /* 数据流：交给 TCP 域钩子（默认空实现，不转发 CAN；业务可覆盖处理） */
    if (n > 0) {
        buf[n] = '\0';
        if (app->flow && app->flow->on_tcp_rx)
            app->flow->on_tcp_rx(app, client_idx, buf, (size_t)n);
    }
    return 0;
}

static int flush_client(tcp_ctx_t *ctx, int client_idx)
{
    client_t *c = &ctx->clients[client_idx];
    if (c->fd < 0) return 1;
    if (c->wlen == 0) return 0;
    ssize_t ret = tcp_send_data(c->fd, c->wbuf, (size_t)c->wlen, 10);
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

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.u32 = 0;
    if (ctx->listen_fd >= 0)
        epoll_ctl(ctx->epfd, EPOLL_CTL_ADD, ctx->listen_fd, &ev);

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
        struct epoll_event events[64];
        int nfds = epoll_wait(ctx->epfd, events, 64, 500);
        if (nfds < 0) {
            if (errno == EINTR) { watchdog_feed_self("tcp"); continue; }
            log_error("tcp epoll_wait"); break;
        }
        watchdog_feed_self("tcp");
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
                dead = handle_tcp_input(app, ctx, client_idx);
            if (!dead && (events[i].events & EPOLLOUT))
                dead = flush_client(ctx, client_idx);
            pthread_mutex_unlock(&ctx->client_mutex);
            if (dead) tcp_client_del(ctx, client_idx);
        }
    }
    log_info("tcp_task stopped");
    return NULL;
}

int tcp_init(void *arg)
{
    app_ctx_t *app = (app_ctx_t *)arg;
    tcp_ctx_t *ctx = app->tcp;
    if (!ctx) return -1;
    memset(ctx, 0, sizeof(*ctx));             /* 栈上创建，显式清零 */
    pthread_mutex_init(&ctx->client_mutex, NULL);
    ctx->port        = app->cfg->gw_args.tcp_port;
    ctx->max_clients = app->cfg->gw_args.max_clients;
    ctx->listen_fd = tcp_listen(ctx->port);
    if (ctx->listen_fd < 0) return -1;
    /* TCP 数据收发专用 epoll（与 CAN 分开管理） */
    ctx->epfd = epoll_create1(0);
    if (ctx->epfd < 0) { log_error("tcp: epoll_create1 failed"); return -1; }
    for (int i = 0; i < TCP_MAX_CLIENTS; i++) ctx->clients[i].fd = -1;
    return 0;
}

void tcp_cleanup(void *arg)
{
    app_ctx_t *app = (app_ctx_t *)arg;
    tcp_ctx_t *ctx = app->tcp;
    if (!ctx) return;
    for (int i = 0; i < TCP_MAX_CLIENTS; i++)
        if (ctx->clients[i].fd >= 0) { close(ctx->clients[i].fd); ctx->clients[i].fd = -1; }
    if (ctx->listen_fd >= 0) close(ctx->listen_fd);
    if (ctx->epfd >= 0) close(ctx->epfd);
}

void tcp_client_add(tcp_ctx_t *ctx, int fd)
{
    pthread_mutex_lock(&ctx->client_mutex);
    int idx = -1;
    int limit = ctx->max_clients > 0 ? ctx->max_clients : TCP_MAX_CLIENTS;
    if (limit > TCP_MAX_CLIENTS) limit = TCP_MAX_CLIENTS;   /* clamp，防止数组越界 */
    for (int i = 0; i < limit; i++) { if (ctx->clients[i].fd == -1) { idx = i; break; } }
    if (idx < 0 && ctx->client_count < limit) idx = ctx->client_count;
    if (idx < 0) { log_error("too many clients, rejecting fd=%d", fd); close(fd); pthread_mutex_unlock(&ctx->client_mutex); return; }
    client_t *c = &ctx->clients[idx]; memset(c, 0, sizeof(*c)); c->fd = fd;
    if (idx >= ctx->client_count) ctx->client_count = idx + 1;
    /* 只注册读事件；写事件由下行数据写入方按需 EPOLL_CTL_MOD 启用（当前无下行数据） */
    struct epoll_event ev; ev.events = EPOLLIN; ev.data.u32 = (uint32_t)(0x80000000 | (idx + 1));
    epoll_ctl(ctx->epfd, EPOLL_CTL_ADD, fd, &ev);
    log_info("client added fd=%d idx=%d", fd, idx);
    pthread_mutex_unlock(&ctx->client_mutex);
}

void tcp_client_del(tcp_ctx_t *ctx, int idx)
{
    pthread_mutex_lock(&ctx->client_mutex);
    if (idx < 0 || idx >= TCP_MAX_CLIENTS) { pthread_mutex_unlock(&ctx->client_mutex); return; }
    client_t *c = &ctx->clients[idx];
    if (c->fd < 0) { pthread_mutex_unlock(&ctx->client_mutex); return; }
    epoll_ctl(ctx->epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd); log_info("client removed fd=%d idx=%d", c->fd, idx);
    c->fd = -1;
    pthread_mutex_unlock(&ctx->client_mutex);
}
