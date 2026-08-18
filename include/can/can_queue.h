#ifndef CAN_QUEUE_H
#define CAN_QUEUE_H

#include <stdint.h>
#include <pthread.h>
#include <semaphore.h>
#include <linux/can.h>
#include <linux/can/raw.h>

#define CAN_QUEUE_MAX_SLOTS 64 /* 池最大槽位数 */

typedef struct{
    struct canfd_frame frame;
    char *ifname;
} can_queue_frame_t;

typedef struct {
    can_queue_frame_t frame[CAN_QUEUE_MAX_SLOTS]; /* 帧缓冲区 */
    sem_t used_sem;    /* 已用信号量（可弹出元素数） */
    sem_t free_sem;    /* 空闲信号量（可入队元素数） */
    uint32_t head;     /* 写指针（生产者专用） */
    uint32_t tail;     /* 读指针（消费者专用） */
    pthread_mutex_t push_mutex; /* 生产者入队互斥锁 */
} can_queue_t;

typedef enum {
    CAN_QUEUE_ERR_OK = 0,            /* 成功 */
    CAN_QUEUE_ERR_QUEUE_SEM_ERROR = -1, /* 队列信号量错误 */
    CAN_QUEUE_ERR_INVALID_PARAM = -2,   /* 无效参数 */
    CAN_QUEUE_ERR_QUEUE_FULL = -3,      /* 队列满 */
    CAN_QUEUE_ERR_QUEUE_EMPTY = -4,     /* 队列空 */
    CAN_QUEUE_ERR_MUTEX_LOCK = -5,      /* 互斥锁加锁失败 */
} can_queue_err_t;

//更新函数声明以匹配新的can_queue_frame_t结构体
int  can_queue_init(can_queue_t *q);
void can_queue_destroy(can_queue_t *q);
int  can_queue_push(can_queue_t *q, const can_queue_frame_t *frame);
int  can_queue_pop(can_queue_t *q, can_queue_frame_t *frame);
int  can_queue_count(can_queue_t *q);

#endif /* CAN_QUEUE_H */
