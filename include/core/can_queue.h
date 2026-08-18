#ifndef CAN_QUEUE_H
#define CAN_QUEUE_H
#include <linux/can.h>
#include <linux/can/raw.h>
#include <linux/semaphore.h>
#include <linux/pthread.h>

#define CAN_QUEUE_MAX_SLOTS 65535 // 池最大槽位数

typedef struct{
    struct canfd_frame frame[CAN_QUEUE_MAX_SLOTS];     //帧缓冲区
    sem_t used_sem;                                     //已用信号量
    sem_t free_sem;                                     //空闲信号量
    uint32_t head;                                      //头指针
    uint32_t tail;                                      //尾指针 
    pthread_mutex_t push_mutex;                              //互斥锁
}can_queue_t;

typedef enum{
    CAN_QUEUE_ERR_OK = 0,                               //成功
    CAN_QUEUE_ERR_QUEUE_SEM_ERROR = -1,                 //队列信号量错误
    CAN_QUEUE_ERR_INVALID_PARAM = -2,                   //无效参数
    CAN_QUEUE_ERR_QUEUE_FULL = -3,                      //队列满
    CAN_QUEUE_ERR_INVALID_PARAM = -4,                   //无效参数
    CAN_QUEUE_ERR_QUEUE_FULL = -5,                      //队列满
    CAN_QUEUE_ERR_QUEUE_EMPTY = -6,                     //队列空
    CAN_QUEUE_ERR_MUTEX_LOCK = -7,                      //互斥锁加锁失败
}can_queue_err_t;

int can_queue_init(can_queue_t *q);
int can_queue_push(can_queue_t *q, struct canfd_frame *frame);
int can_queue_pop(can_queue_t *q, struct canfd_frame *frame);

#endif /* CAN_QUEUE_H */