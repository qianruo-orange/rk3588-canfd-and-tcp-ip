/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TCP_QUEUE_H
#define TCP_QUEUE_H

#include <stddef.h>

#define WBUF_SIZE         4096
#define TCP_QUEUE_DEPTH   256

/* ---- TCP 数据包（RX / TX 队列共用条目） ----
 * client_idx：RX 表示来源客户端，TX 表示目标客户端。 */
typedef struct {
    int    client_idx;
    size_t len;
    char   data[WBUF_SIZE];
} tcp_pkt_t;

/* 定长环形缓冲区，存放待收发 TCP 数据包 */
typedef struct {
    tcp_pkt_t items[TCP_QUEUE_DEPTH];
    int head;
    int tail;
    int count;
} tcp_queue_t;

/* 队列操作均为非线程安全，调用方需自行加锁串行化访问（当前由 tcp_ctx.tx_mutex 保护） */

/* 重置队列（清空）。须在无并发访问时调用（如 tcp_init）。 */
void tcp_queue_reset(tcp_queue_t *q);

/* 压入一个数据包（拷贝 data）。返回 0 成功，-1 队列满或参数非法。 */
int tcp_queue_push(tcp_queue_t *q, int client_idx, const void *data, size_t len);

/* 查看队头数据包（不出队）。返回 0 成功，-1 队列空。 */
int tcp_queue_peek(tcp_queue_t *q, tcp_pkt_t *out);

/* 弹出队头数据包。返回 0 成功，-1 队列空。 */
int tcp_queue_pop(tcp_queue_t *q, tcp_pkt_t *out);

#endif /* TCP_QUEUE_H */
