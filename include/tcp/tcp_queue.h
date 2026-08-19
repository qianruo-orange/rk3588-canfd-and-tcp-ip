#ifndef TCP_QUEUE_H
#define TCP_QUEUE_H

#include <semaphore.h>  
#include <stddef.h>
#include <stdint.h>

/* 内存池参数（静态分配，禁止动态内存）：
 * 池划分为 TCP_QUEUE_MAX_SLOTS 个槽位，每槽 TCP_QUEUE_MAX_ELEM_BYTES 字节；
 * 帧文本最大 512 字节（FLOW_FRAME_TEXT_MAX），故 16×512=8192 字节。 */
#define TCP_QUEUE_MAX_SLOTS  1024 // 池最大槽位数
#define TCP_QUEUE_MAX_ELEM_BYTES 1024 // 每槽最大字节数
#define TCP_QUEUE_POOL_BYTES     (TCP_QUEUE_MAX_SLOTS * TCP_QUEUE_MAX_ELEM_BYTES)

typedef struct{
    uint8_t buf[TCP_QUEUE_POOL_BYTES];  // 数据缓冲区
    uint8_t len[TCP_QUEUE_MAX_SLOTS];   // 数据长度缓冲区
    sem_t used_sem;                     // 已用信号量
    sem_t free_sem;                     // 空闲信号量
    uint8_t head;                       // 头指针
    uint8_t tail;                       // 尾指针
    uint8_t count;                      // 元素计数         
    pthread_mutex_t mutex;             // 互斥锁    
    pthread_mutex_t mutex;             // 互斥锁
}tcp_queue_t;

int tcp_queue_count(const tcp_queue_t *q);

#endif /* TCP_QUEUE_H */
