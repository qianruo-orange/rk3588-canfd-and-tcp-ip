#ifndef QUEUE_H
#define QUEUE_H

#include <pthread.h>
#include <stddef.h>

/* 固定容量环形队列（线程安全）：
 * - push 时队列已满则丢弃新元素并返回 -1；
 * - 每次成功 push 会向 eventfd 写通知，供 epoll 即时唤醒消费者；
 * - 单消费者模式（每个队列仅一个消费线程）。 */
typedef struct queue {
    void            *buf;        /* 环形缓冲 */
    size_t           cap;        /* 容量（元素个数） */
    size_t           elem_size;  /* 元素字节数 */
    size_t           head;       /* 队首下标 */
    size_t           tail;       /* 队尾下标 */
    size_t           count;      /* 当前元素数 */
    pthread_mutex_t  mutex;
    int              event_fd;   /* 数据到达通知（EFD_NONBLOCK） */
} queue_t;

/* 初始化队列；cap/elem_size 必须大于 0 */
int  queue_init(queue_t *q, size_t cap, size_t elem_size);

/* 销毁队列并释放资源 */
void queue_destroy(queue_t *q);

/* 入队：成功返回 0；满返回 -1（丢弃） */
int  queue_push(queue_t *q, const void *item);

/* 出队：成功返回 1；空返回 0 */
int  queue_pop(queue_t *q, void *item);

/* 获取通知用 eventfd（供 epoll 注册） */
int  queue_event_fd(const queue_t *q);

/* 读空 eventfd 计数器（消费唤醒事件后调用） */
void queue_drain_event(queue_t *q);

#endif /* QUEUE_H */
