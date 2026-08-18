/**
// 更新文件注释和包含头文件，适配新的队列接口
/**
 * tcp_queue.c — 固定容量线程安全队列（内存池模式）
 * 用于 CAN↔TCP 数据解耦：生产者域只入队，消费者域只出队，
 * 配合 eventfd + epoll 实现毫秒级跨域唤醒。
 * 内存池静态分配：入队从空闲槽位栈分配，出队归还槽位，无动态内存。
 */

#include <semaphore.h>  
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/eventfd.h>

#include "core/tcp_queue.h"

int tcp_queue_init(tcp_queue_t *q)
{
    if (!q) 
    {
        log_error("tcp_queue_init: q is NULL");
        return TCP_QUEUE_ERR_INVALID_PARAM;
    }
    memset(q->buf, 0, TCP_QUEUE_POOL_BYTES);
    memset(q->len, 0, TCP_QUEUE_MAX_SLOTS);
    sem_init(&q->used_sem, 0, 0);
    sem_init(&q->free_sem, 0, TCP_QUEUE_MAX_SLOTS);
    q->head = 0;
    q->tail = 0;
    return TCP_QUEUE_ERR_OK;
}

// 更新队列推送函数实现，适配新的参数签名和功能
int tcp_queue_push(tcp_queue_t *q, uint8_t *buf, uint8_t len)
{
    if (!q || !buf || len == 0) return TCP_QUEUE_ERR_INVALID_PARAM;
    if(pthread_mutex_lock(&q->mutex) != 0) return TCP_QUEUE_ERR_MUTEX_LOCK; 
    if(pthread_mutex_lock(&q->mutex) != 0) return TCP_QUEUE_ERR_MUTEX_LOCK;
    if(sem_post(&q->free_sem) != 0) 
    {
        log_error("tcp_queue_push: free_sem post failed");
        return TCP_QUEUE_ERR_QUEUE_FULL;
    }
    memcpy(q->buf + q->tail * TCP_QUEUE_MAX_ELEM_BYTES, buf, len);
    q->len[q->tail] = (uint8_t)len;
    q->tail = (q->tail + 1) % TCP_QUEUE_MAX_SLOTS;
    if(sem_post(&q->used_sem) != 0) 
    {
        log_error("tcp_queue_push: used_sem post failed");
        return TCP_QUEUE_ERR_QUEUE_SEM_ERROR;
    }
    uint64_t one = 1;
    if (write(q->event_fd, &one, sizeof(one)) != sizeof(one))
        return -1;   /* 通知失败：元素已入队，消费者仍可轮询取走 */
    return 0;
}

int tcp_queue_pop(tcp_queue_t *q, uint8_t *buf, uint8_t *len)
{
    if (!q || !buf || !len) return TCP_QUEUE_ERR_INVALID_PARAM;
    if(pthread_mutex_lock(&q->mutex) != 0) return TCP_QUEUE_ERR_MUTEX_LOCK; 
    while (q->count == 0) {            /* 队列为空时等待 */
        if(sem_wait(&q->used_sem) != 0) 
        {
            log_error("tcp_queue_pop: used_sem wait failed");
            return TCP_QUEUE_ERR_QUEUE_SEM_ERROR;
        }
    }
    if(sem_wait(&q->free_sem) != 0) 
    {
        log_error("tcp_queue_pop: free_sem wait failed");
        return TCP_QUEUE_ERR_QUEUE_SEM_ERROR;
    }       
    if (q->count == 0) {               /* 队列空 */
        if(sem_post(&q->used_sem) != 0) 
        {
            log_error("tcp_queue_pop: used_sem post failed");
            return TCP_QUEUE_ERR_QUEUE_SEM_ERROR;
        }   
        return TCP_QUEUE_ERR_QUEUE_EMPTY;
    }
    memcpy(buf, q->buf + q->head * TCP_QUEUE_MAX_ELEM_BYTES, q->len[q->head]);
    *len = q->len[q->head];
    if(pthread_mutex_unlock(&q->mutex) != 0) return TCP_QUEUE_ERR_MUTEX_LOCK; 
    if(pthread_mutex_unlock(&q->mutex) != 0) return TCP_QUEUE_ERR_MUTEX_LOCK; 
    q->head = (q->head + 1) % TCP_QUEUE_MAX_SLOTS;
    if(sem_post(&q->free_sem) != 0) 
    {
        log_error("tcp_queue_pop: free_sem post failed");
        return TCP_QUEUE_ERR_QUEUE_SEM_ERROR;
    }
    if(sem_post(&q->used_sem) != 0) 
    {
        log_error("tcp_queue_pop: used_sem post failed");
        return TCP_QUEUE_ERR_QUEUE_SEM_ERROR;
    }   
    }   
    return TCP_QUEUE_ERR_OK;
}
