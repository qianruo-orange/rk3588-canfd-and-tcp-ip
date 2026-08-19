#ifndef TCP_SERVER_H
#define TCP_SERVER_H

#include <pthread.h>
#include "core/config.h"

#define TCP_MAX_CLIENTS   32
#define WBUF_SIZE         4096
#define TCP_QUEUE_DEPTH   256

/* ---- TCP 数据包（RX / TX 队列共用条目） ----
 * client_idx：RX 表示来源客户端，TX 表示目标客户端。 */
typedef struct {
    int    client_idx;
    size_t len;
    char   data[WBUF_SIZE];
} tcp_pkt_t;

typedef struct {
    tcp_pkt_t items[TCP_QUEUE_DEPTH];
    int head;
    int tail;
    int count;
} tcp_queue_t;

/* ---- TCP 客户端状态 ---- */
typedef struct {
    int  fd;
    char wbuf[WBUF_SIZE];   /* 单客户端正在发送的部分写缓冲 */
    int  wlen;
} client_t;

/* ---- TCP 子系统上下文 ----
 * TX 队列：业务线程压入待发送包，tcp_task 弹出并写客户端 socket。
 * 用 eventfd 唤醒，epoll 检测"有数据压入"。 */
typedef struct tcp_ctx {
    int           listen_fd;
    int           port;
    int           max_clients;
    char          bind_ifname[IFNAMSIZ]; /* TCP 监听绑定网卡名（空 = 绑定所有网卡） */
    client_t      clients[TCP_MAX_CLIENTS];
    int           client_count;
    pthread_mutex_t client_mutex;
    int           epfd;      /* TCP 数据收发 epoll（tcp_task 线程） */

    pthread_mutex_t tx_mutex;
    tcp_queue_t     txq;
    int             tx_efd;
} tcp_ctx_t;

int tcp_listen(int port, const char *bind_ifname);

/* 创建 TCP 监听套接字。必须在 can_init() 之后调用。 */
int  tcp_init(void *arg);
void tcp_cleanup(void *ctx);

/* TCP 数据收发线程（独立 epoll 管理 listen + 客户端读写 + TX eventfd） */
void *tcp_task(void *arg);

#endif /* TCP_SERVER_H */
