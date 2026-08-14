/**
 * queue.c — 固定容量线程安全队列（内存池模式）
 * 用于 CAN↔TCP 数据解耦：生产者域只入队，消费者域只出队，
 * 配合 eventfd + epoll 实现毫秒级跨域唤醒。
 * 内存池静态分配：入队从空闲槽位栈分配，出队归还槽位，无动态内存。
 */

#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/eventfd.h>

#include "core/queue.h"

int queue_init(queue_t *q, size_t cap, size_t elem_size)
{
    if (!q || cap == 0 || elem_size == 0) return -1;
    if (cap > QUEUE_MAX_SLOTS) return -1;
    if (elem_size > QUEUE_MAX_ELEM_BYTES) return -1;

    memset(q, 0, sizeof(*q));
    q->cap = cap;
    q->elem_size = elem_size;

    /* 初始化空闲槽位栈：槽位 0..cap-1 全部空闲 */
    for (size_t i = 0; i < cap; i++)
        q->free_slots[i] = (uint8_t)i;
    q->free_count = cap;

    pthread_mutex_init(&q->mutex, NULL);
    q->event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (q->event_fd < 0) return -1;
    return 0;
}

void queue_destroy(queue_t *q)
{
    if (!q) return;
    if (q->event_fd >= 0) close(q->event_fd);
    pthread_mutex_destroy(&q->mutex);
    memset(q, 0, sizeof(*q));
    q->event_fd = -1;
}

int queue_push(queue_t *q, const void *item)
{
    if (!q || !item) return -1;
    pthread_mutex_lock(&q->mutex);
    if (q->free_count == 0) {          /* 池空 */
        pthread_mutex_unlock(&q->mutex);
        return -1;   /* 满：丢弃新元素 */
    }
    size_t slot = q->free_slots[--q->free_count];   /* 从空闲池分配槽位 */
    memcpy(q->pool + slot * q->elem_size, item, q->elem_size);
    q->fifo[q->tail] = (uint8_t)slot;               /* 记录 FIFO 顺序 */
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
    if (q->count == 0) {               /* 队列空 */
        pthread_mutex_unlock(&q->mutex);
        return 0;
    }
    size_t slot = q->fifo[q->head];    /* 按 FIFO 顺序取最早入队的槽位 */
    q->head = (q->head + 1) % q->cap;
    q->count--;
    memcpy(item, q->pool + slot * q->elem_size, q->elem_size);
    q->free_slots[q->free_count++] = (uint8_t)slot;  /* 归还槽位到空闲池 */
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
