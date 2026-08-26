#define _GNU_SOURCE   /* 暴露 MSG_NOSIGNAL / eventfd 等 Linux 扩展 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "tcp/tcp_server.h"
#include "tcp/tcp_queue.h"
#include "core/common.h"
#include "core/epoll_util.h"
#include "core/log.h"
#include "watchdog/watchdog.h"

/* epoll 事件 tag：listen_fd 用 0，TX eventfd 用高位标识，客户端用 0x80000000|(idx+1) */
#define TCP_TXEFD_TAG 0x40000000u
#define TCP_CLIENT_TAG(idx) (0x80000000u | (uint32_t)((idx) + 1))

int tcp_listen(int port, const char *bind_ifname)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { LOG_ERROR("tcp socket"); return -1; }

    /* 指定网卡时用 SO_BINDTODEVICE 绑定到该接口（否则 INADDR_ANY 绑定所有网卡） */
    if (bind_ifname && bind_ifname[0]) {
        struct ifreq ifr;
        memset(&ifr, 0, sizeof(ifr));
        ifreq_set_name(&ifr, bind_ifname);
        if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, &ifr, sizeof(ifr)) < 0) {
            LOG_ERROR("tcp bind device %s: %s", bind_ifname, strerror(errno));
            close(fd); return -1;
        }
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("tcp bind :%d: %s", port, strerror(errno));
        close(fd); return -1;
    }
    if (listen(fd, SOMAXCONN) < 0) {
        LOG_ERROR("tcp listen :%d: %s", port, strerror(errno));
        close(fd); return -1;
    }
    if (set_nonblock(fd) < 0) {
        LOG_ERROR("tcp fcntl O_NONBLOCK: %s", strerror(errno));
        close(fd); return -1;
    }
    if (bind_ifname && bind_ifname[0])
        LOG_INFO("TCP listening on %s:%d", bind_ifname, port);
    else
        LOG_INFO("TCP listening on port %d", port);
    return fd;
}

static int tcp_accept(int listen_fd)
{
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &addr_len);
    if (client_fd < 0) {
        /* 非阻塞监听 fd 下 EAGAIN/EWOULDBLOCK 属正常无连接，不刷错误日志 */
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            LOG_ERROR("tcp accept: %s", strerror(errno));
        return -1;
    }
    if (set_nonblock(client_fd) < 0) {
        LOG_ERROR("tcp client fcntl O_NONBLOCK: %s", strerror(errno));
        close(client_fd);
        return -1;
    }
    char ip_str[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str)) == NULL)
        safe_strncpy(ip_str, sizeof(ip_str), "?");
    LOG_INFO("TCP client connected: %s:%d  fd=%d", ip_str, ntohs(client_addr.sin_port), client_fd);
    return client_fd;
}

/* ---- eventfd 跨线程唤醒：TX 队列有数据压入时通知 tcp_task ----
   写/读 eventfd 的公共实现见 core/common.h（eventfd_signal / eventfd_consume） */

/*
 * tcp_tx_packet - 异步发送数据到客户端：压入 TX 队列，写 eventfd 唤醒 tcp_task。
 * client_idx < 0 表示广播所有已连接客户端。返回 0 成功，-1 失败。
 */
static int tcp_tx_packet(tcp_ctx_t *ctx, int client_idx, const void *buf, size_t len)
{
    if (!ctx || !buf || len == 0 || len > WBUF_SIZE) { errno = EINVAL; return -1; }

    int pushed = 0;

    if (client_idx < 0) {
        /* 广播：先快照已连接客户端索引，再统一压入 TX 队列 */
        int idxs[TCP_MAX_CLIENTS];
        int n = 0;
        pthread_mutex_lock(&ctx->client_mutex);
        for (int i = 0; i < ctx->client_count && n < TCP_MAX_CLIENTS; i++)
            if (ctx->clients[i].fd >= 0) idxs[n++] = i;
        pthread_mutex_unlock(&ctx->client_mutex);

        pthread_mutex_lock(&ctx->tx_mutex);
        for (int i = 0; i < n; i++)
            if (tcp_queue_push(&ctx->txq, idxs[i], buf, len) == 0) pushed++;
        pthread_mutex_unlock(&ctx->tx_mutex);
    } else {
        pthread_mutex_lock(&ctx->tx_mutex);
        if (tcp_queue_push(&ctx->txq, client_idx, buf, len) == 0) pushed++;
        pthread_mutex_unlock(&ctx->tx_mutex);
    }

    if (pushed > 0)
        eventfd_signal(ctx->tx_efd);

    return pushed > 0 ? 0 : -1;
}

/* 读取单个客户端数据并丢弃（不再转发到 CAN）；返回 1 表示客户端已断开 */
static int handle_tcp_input(tcp_ctx_t *ctx, int client_idx)
{
    char buf[WBUF_SIZE];

    pthread_mutex_lock(&ctx->client_mutex);
    if (client_idx < 0 || client_idx >= TCP_MAX_CLIENTS) {
        pthread_mutex_unlock(&ctx->client_mutex);
        return 1;
    }
    client_t *c = &ctx->clients[client_idx];
    if (c->fd < 0) { pthread_mutex_unlock(&ctx->client_mutex); return 1; }
    ssize_t n = recv(c->fd, buf, sizeof(buf), 0);
    pthread_mutex_unlock(&ctx->client_mutex);

    if (n == 0) return 1;                                   /* 对端正常关闭 */
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return 1;
    return 0;
}

/* 弹出 TX 队列并写客户端 socket，处理部分写（用每客户端 wbuf 暂存未发完的字节） */
static void tcp_flush_tx(tcp_ctx_t *ctx)
{
    pthread_mutex_lock(&ctx->tx_mutex);
    pthread_mutex_lock(&ctx->client_mutex);

    /* 1) 先推进每个客户端尚未发完的 wbuf */
    for (int i = 0; i < ctx->client_count; i++) {
        client_t *c = &ctx->clients[i];
        if (c->fd < 0 || c->wlen <= 0) continue;
        ssize_t n = send(c->fd, c->wbuf, (size_t)c->wlen, MSG_NOSIGNAL);
        if (n > 0) {
            memmove(c->wbuf, c->wbuf + n, (size_t)(c->wlen - n));
            c->wlen -= (int)n;
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            c->wlen = 0; /* 连接异常，丢弃残留 */
        }
    }

    /* 2) 再把 TX 队列里的包搬到客户端 wbuf 并发送。
          队头包的目标客户端若暂时写不出去（wbuf 未排空），不阻塞其他客户端：
          把该包转回队尾稍后再试，继续处理后续包；每个目标客户端的包保持 FIFO。 */
    int remaining = ctx->txq.count;   /* 本次尝试处理的包数上限（旋转可能把包放回队尾） */
    int examined = 0;
    while (examined < remaining) {
        tcp_pkt_t p;
        if (tcp_queue_peek(&ctx->txq, &p) != 0) break;   /* 队列已空 */
        examined++;
        int idx = p.client_idx;
        if (idx < 0 || idx >= TCP_MAX_CLIENTS || idx >= ctx->client_count) { tcp_queue_pop(&ctx->txq, &p); continue; }
        client_t *c = &ctx->clients[idx];
        if (c->fd < 0) { tcp_queue_pop(&ctx->txq, &p); continue; }   /* 客户端已断开，丢弃 */
        if (c->wlen > 0) {
            /* 慢客户端：队头包转回队尾，避免队头阻塞拖垮整个广播 */
            tcp_queue_pop(&ctx->txq, &p);
            tcp_queue_push(&ctx->txq, p.client_idx, p.data, p.len);
            continue;
        }
        tcp_queue_pop(&ctx->txq, &p);
        memcpy(c->wbuf, p.data, p.len);
        c->wlen = (int)p.len;
        ssize_t n = send(c->fd, c->wbuf, (size_t)c->wlen, MSG_NOSIGNAL);
        if (n > 0) {
            memmove(c->wbuf, c->wbuf + n, (size_t)(c->wlen - n));
            c->wlen -= (int)n;
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            c->wlen = 0;
        }
    }

    /* 3) 按 wbuf 是否非空更新 EPOLLOUT 状态 */
    for (int i = 0; i < ctx->client_count; i++) {
        client_t *c = &ctx->clients[i];
        if (c->fd < 0) continue;
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLRDHUP | (c->wlen > 0 ? EPOLLOUT : 0);
        ev.data.u32 = TCP_CLIENT_TAG(i);
        epoll_ctl(ctx->epfd, EPOLL_CTL_MOD, c->fd, &ev);
    }

    pthread_mutex_unlock(&ctx->client_mutex);
    pthread_mutex_unlock(&ctx->tx_mutex);
}

/* tcp_client_add / tcp_client_del 定义在文件末尾，此处提前声明供 tcp_task 使用 */
static void tcp_client_add(tcp_ctx_t *ctx, int fd);
static void tcp_client_del(tcp_ctx_t *ctx, int idx);

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

    ev.events = EPOLLIN;
    ev.data.u32 = TCP_TXEFD_TAG;
    if (ctx->tx_efd >= 0)
        epoll_ctl(ctx->epfd, EPOLL_CTL_ADD, ctx->tx_efd, &ev);

    pthread_mutex_lock(&ctx->client_mutex);
    for (int i = 0; i < TCP_MAX_CLIENTS; i++) {
        if (ctx->clients[i].fd < 0) continue;
        ev.events = EPOLLIN | EPOLLRDHUP;
        ev.data.u32 = TCP_CLIENT_TAG(i);
        epoll_ctl(ctx->epfd, EPOLL_CTL_ADD, ctx->clients[i].fd, &ev);
    }
    pthread_mutex_unlock(&ctx->client_mutex);

    LOG_INFO("tcp_task started");
    while (app->running) {
        struct epoll_event events[64];
        int nfds = epoll_wait_feed(ctx->epfd, events, 64, 500, "tcp");
        if (nfds < 0) break;
        for (int i = 0; i < nfds; i++) {
            uint32_t tag = events[i].data.u32;
            if (tag == 0) {
                while (1) { int fd = tcp_accept(ctx->listen_fd); if (fd < 0) break; tcp_client_add(ctx, fd); }
                continue;
            }
            if (tag == TCP_TXEFD_TAG) {
                eventfd_consume(ctx->tx_efd);
                tcp_flush_tx(ctx);
                continue;
            }
            int client_idx = (int)(tag & 0x7FFFFFFFu) - 1;
            if (client_idx < 0 || client_idx >= TCP_MAX_CLIENTS) continue;
            uint32_t e = events[i].events;
            int dead = 0;
            if (e & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR))
                dead = handle_tcp_input(ctx, client_idx);
            if (!dead && (e & EPOLLOUT))
                tcp_flush_tx(ctx);
            if (dead) tcp_client_del(ctx, client_idx);
        }
    }
    LOG_INFO("tcp_task stopped");
    return NULL;
}

int tcp_init(void *arg)
{
    app_ctx_t *app = (app_ctx_t *)arg;
    tcp_ctx_t *ctx = app->tcp;
    if (!ctx) return -1;
    memset(ctx, 0, sizeof(*ctx));
    ctx->epfd = -1;
    ctx->tx_efd = -1;
    pthread_mutex_init(&ctx->client_mutex, NULL);
    pthread_mutex_init(&ctx->tx_mutex, NULL);
    ctx->port        = app->cfg->args.tcp_port;
    ctx->max_clients = app->cfg->args.max_clients;
    safe_strncpy(ctx->bind_ifname, sizeof(ctx->bind_ifname), app->cfg->args.tcp_bind);
    ctx->listen_fd = tcp_listen(ctx->port, ctx->bind_ifname);
    if (ctx->listen_fd < 0) return -1;
    /* TCP 数据收发专用 epoll（listen + 客户端 + TX eventfd，与 CAN 分开管理） */
    ctx->epfd = epoll_create1(0);
    if (ctx->epfd < 0) { LOG_ERROR("tcp: epoll_create1 failed"); return -1; }
    ctx->tx_efd = eventfd(0, EFD_NONBLOCK);
    if (ctx->tx_efd < 0) { LOG_ERROR("tcp: eventfd failed"); return -1; }
    for (int i = 0; i < TCP_MAX_CLIENTS; i++) ctx->clients[i].fd = -1;
    tcp_queue_reset(&ctx->txq);
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
    if (ctx->tx_efd >= 0) close(ctx->tx_efd);
    if (ctx->epfd >= 0) close(ctx->epfd);
    pthread_mutex_destroy(&ctx->client_mutex);
    pthread_mutex_destroy(&ctx->tx_mutex);
}

static void tcp_client_add(tcp_ctx_t *ctx, int fd)
{
    pthread_mutex_lock(&ctx->client_mutex);
    int idx = -1;
    int limit = ctx->max_clients > 0 ? ctx->max_clients : TCP_MAX_CLIENTS;
    if (limit > TCP_MAX_CLIENTS) limit = TCP_MAX_CLIENTS;   /* clamp，防止数组越界 */
    for (int i = 0; i < limit; i++) { if (ctx->clients[i].fd == -1) { idx = i; break; } }
    if (idx < 0 && ctx->client_count < limit) idx = ctx->client_count;
    if (idx < 0) { LOG_ERROR("too many clients, rejecting fd=%d", fd); close(fd); pthread_mutex_unlock(&ctx->client_mutex); return; }
    client_t *c = &ctx->clients[idx]; memset(c, 0, sizeof(*c)); c->fd = fd;
    if (idx >= ctx->client_count) ctx->client_count = idx + 1;
    struct epoll_event ev; ev.events = EPOLLIN | EPOLLRDHUP; ev.data.u32 = TCP_CLIENT_TAG(idx);
    epoll_ctl(ctx->epfd, EPOLL_CTL_ADD, fd, &ev);
    LOG_INFO("client added fd=%d idx=%d", fd, idx);
    pthread_mutex_unlock(&ctx->client_mutex);
}

static void tcp_client_del(tcp_ctx_t *ctx, int idx)
{
    pthread_mutex_lock(&ctx->client_mutex);
    if (idx < 0 || idx >= TCP_MAX_CLIENTS) { pthread_mutex_unlock(&ctx->client_mutex); return; }
    client_t *c = &ctx->clients[idx];
    if (c->fd < 0) { pthread_mutex_unlock(&ctx->client_mutex); return; }
    epoll_ctl(ctx->epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd); LOG_INFO("client removed fd=%d idx=%d", c->fd, idx);
    c->fd = -1;
    pthread_mutex_unlock(&ctx->client_mutex);
}
