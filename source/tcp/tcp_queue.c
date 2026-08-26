#include <string.h>

#include "tcp/tcp_queue.h"

/* 重置队列（清空）。须在无并发访问时调用（如 tcp_init）。 */
void tcp_queue_reset(tcp_queue_t *q)
{
    if (!q) return;
    q->head = q->tail = q->count = 0;
}

/* 压入一个数据包（拷贝 data）。返回 0 成功，-1 队列满或参数非法。 */
int tcp_queue_push(tcp_queue_t *q, int client_idx, const void *data, size_t len)
{
    if (!q || !data || len == 0 || len > WBUF_SIZE) return -1;
    if (q->count >= TCP_QUEUE_DEPTH) return -1;
    tcp_pkt_t *p = &q->items[q->tail];
    p->client_idx = client_idx;
    p->len = len;
    memcpy(p->data, data, len);
    q->tail = (q->tail + 1) % TCP_QUEUE_DEPTH;
    q->count++;
    return 0;
}

/* 查看队头数据包（不出队）。返回 0 成功，-1 队列空。 */
int tcp_queue_peek(tcp_queue_t *q, tcp_pkt_t *out)
{
    if (!q || !out) return -1;
    if (q->count <= 0) return -1;
    *out = q->items[q->head];
    return 0;
}

/* 弹出队头数据包。返回 0 成功，-1 队列空。 */
int tcp_queue_pop(tcp_queue_t *q, tcp_pkt_t *out)
{
    if (!q || !out) return -1;
    if (q->count <= 0) return -1;
    *out = q->items[q->head];
    q->head = (q->head + 1) % TCP_QUEUE_DEPTH;
    q->count--;
    return 0;
}
