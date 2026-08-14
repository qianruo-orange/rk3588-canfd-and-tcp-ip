#ifndef QUEUE_H
#define QUEUE_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

/* 内存池参数（静态分配，禁止动态内存）：
 * 池划分为 QUEUE_MAX_SLOTS 个槽位，每槽 QUEUE_MAX_ELEM_BYTES 字节；
 * 帧文本最大 512 字节（FLOW_FRAME_TEXT_MAX），故 16×512=8192 字节。 */
#define QUEUE_MAX_SLOTS      16
#define QUEUE_MAX_ELEM_BYTES 512
#define QUEUE_POOL_BYTES     (QUEUE_MAX_SLOTS * QUEUE_MAX_ELEM_BYTES)

/* 固定容量线程安全队列（内存池模式）：
 * - 预分配静态内存池，入队从空闲槽位栈分配，出队归还；
 * - push 时池已空则丢弃新元素并返回 -1；
 * - 每次成功 push 会向 eventfd 写通知，供 epoll 即时唤醒消费者；
 * - 单消费者模式（每个队列仅一个消费线程）。 */
typedef struct queue {
    unsigned char    pool[QUEUE_POOL_BYTES];      /* 内存池（静态分配） */
    uint8_t          free_slots[QUEUE_MAX_SLOTS]; /* 空闲槽位索引栈 */
    uint8_t          fifo[QUEUE_MAX_SLOTS];       /* FIFO 出队顺序（存槽位索引） */
    size_t           cap;        /* 实际槽位数量（<= QUEUE_MAX_SLOTS） */
    size_t           elem_size;  /* 单槽字节数（<= QUEUE_MAX_ELEM_BYTES） */
    size_t           free_count; /* 当前空闲槽位数 */
    size_t           head;       /* FIFO 队首下标 */
    size_t           tail;       /* FIFO 队尾下标 */
    size_t           count;      /* 当前已入队元素数 */
    pthread_mutex_t  mutex;
    int              event_fd;   /* 数据到达通知（EFD_NONBLOCK） */
} queue_t;

/* 初始化队列；0 < cap <= QUEUE_MAX_SLOTS，0 < elem_size <= QUEUE_MAX_ELEM_BYTES */
int  queue_init(queue_t *q, size_t cap, size_t elem_size);

/* 销毁队列并释放资源 */
void queue_destroy(queue_t *q);

/* 入队：成功返回 0；池满返回 -1（丢弃） */
int  queue_push(queue_t *q, const void *item);

/* 出队：成功返回 1；空返回 0 */
int  queue_pop(queue_t *q, void *item);

/* 获取通知用 eventfd（供 epoll 注册） */
int  queue_event_fd(const queue_t *q);

/* 读空 eventfd 计数器（消费唤醒事件后调用） */
void queue_drain_event(queue_t *q);

#endif /* QUEUE_H */
