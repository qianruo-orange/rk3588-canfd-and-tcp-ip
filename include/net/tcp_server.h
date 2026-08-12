#ifndef TCP_SERVER_H
#define TCP_SERVER_H

#include <pthread.h>
#include "core/config.h"

#define TCP_MAX_CLIENTS  32
#define WBUF_SIZE        4096

/* ---- TCP 客户端状态 ---- */
typedef struct {
    int  fd;
    char wbuf[WBUF_SIZE];
    int  wlen;
} client_t;

/* ---- TCP 子系统上下文 ---- */
typedef struct tcp_ctx {
    int           listen_fd;
    int           port;
    int           max_clients;
    client_t      clients[TCP_MAX_CLIENTS];
    int           client_count;
    pthread_mutex_t client_mutex;
    int           epfd;   /* TCP 数据收发专用 epoll */
} tcp_ctx_t;

int tcp_listen(int port);
int tcp_accept(int listen_fd);

/* 创建 TCP 监听套接字。必须在 can_init() 之后调用。 */
int  tcp_init(void *arg);
void tcp_cleanup(void *ctx);

/* TCP 数据收发线程（独立 epoll 管理 listen + 客户端读写） */
void *tcp_task(void *arg);

/* 客户端管理 */
void tcp_client_add(tcp_ctx_t *ctx, int fd);
void tcp_client_del(tcp_ctx_t *ctx, int idx);

#endif /* TCP_SERVER_H */
