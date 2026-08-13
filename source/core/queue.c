/**
 * queue.c — 固定容量线程安全环形队列
 * 用于 CAN↔TCP 数据解耦：生产者域只入队，消费者域只出队，
 * 配合 eventfd + epoll 实现毫秒级跨域唤醒。
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/eventfd.h>

#include "core/queue.h"

int queue_init(queue_t *q, size_t cap, size_t elem_size)
{
    if (!q || cap == 0 || elem_size == 0) return -1;
    memset(q, 0, sizeof(*q));
    q->cap = cap;
    q->elem_size = elem_size;
    q->buf = calloc(cap, elem_size);
    if (!q->buf) return -1;
    pthread_mutex_init(&q->mutex, NULL);
    q->event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (q->event_fd < 0) {
        free(q->buf);
        q->buf = NULL;
        return -1;
    }
    return 0;
}

void queue_destroy(queue_t *q)
{
    if (!q) return;
    if (q->event_fd >= 0) close(q->event_fd);
    free(q->buf);
    pthread_mutex_destroy(&q->mutex);
    memset(q, 0, sizeof(*q));
    q->event_fd = -1;
}

int queue_push(queue_t *q, const void *item)
{
    if (!q || !item) return -1;
    pthread_mutex_lock(&q->mutex);
    if (q->count == q->cap) {
        pthread_mutex_unlock(&q->mutex);
        return -1;   /* 满：丢弃新元素 */
    }
    memcpy((char *)q->buf + q->tail * q->elem_size, item, q->elem_size);
    q->tail = (q->tail + 1) % q->cap;
    q->count++;
    pthread_mutex_unlock(&q->mutex);

    uint64_t one = 1;
    if (write(q->event_fd, &one, sizeof(one)) != sizeof(one))
        return -1;   /* 通知失败：元素已入队，消费者仍可轮询取走 */
    return 0;
}

int queue_pop(queue_t *q, void *item)
{
    if (!q || !item) return -1;
    pthread_mutex_lock(&q->mutex);
    if (q->count == 0) {
        pthread_mutex_unlock(&q->mutex);
        return 0;
    }
    memcpy(item, (char *)q->buf + q->head * q->elem_size, q->elem_size);
    q->head = (q->head + 1) % q->cap;
    q->count--;
    pthread_mutex_unlock(&q->mutex);
    return 1;
}

int queue_event_fd(const queue_t *q)
{
    return q ? q->event_fd : -1;
}

void queue_drain_event(queue_t *q)
{
    if (!q || q->event_fd < 0) return;
    uint64_t val;
    while (read(q->event_fd, &val, sizeof(val)) == sizeof(val)) { }
}
