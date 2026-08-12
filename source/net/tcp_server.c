#define _GNU_SOURCE   /* 暴露 SO_REUSEPORT 等 GNU/Linux 扩展 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "net/tcp_server.h"
#include "core/common.h"
#include "core/log.h"

int tcp_listen(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { log_error("tcp socket"); return -1; }
    int optval = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { log_error("tcp bind :%d", port); close(fd); return -1; }
    if (listen(fd, SOMAXCONN) < 0) { log_error("tcp listen :%d", port); close(fd); return -1; }
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    log_info("TCP listening on port %d", port);
    return fd;
}

int tcp_accept(int listen_fd)
{
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &addr_len);
    if (client_fd < 0) return -1;
    int flags = fcntl(client_fd, F_GETFL, 0);
    fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));
    log_info("TCP client connected: %s:%d  fd=%d", ip_str, ntohs(client_addr.sin_port), client_fd);
    return client_fd;
}

int tcp_init(void *arg)
{
    app_ctx_t *app = (app_ctx_t *)arg;
    tcp_ctx_t *ctx = app->tcp;
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
