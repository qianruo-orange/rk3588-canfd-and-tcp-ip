#include "core/can_queue.h"
#include "log.h"

int can_queue_init(can_queue_t *q)
{
    if (!q) 
    {
        LOG_ERROR("can_queue_init: q is NULL");
        return CAN_QUEUE_ERR_INVALID_PARAM;
    }
    memset(q, 0, sizeof(*q));
    if(pthread_mutex_init(&q->push_mutex, NULL) != 0) 
    {
        LOG_ERROR("can_queue_init: push_mutex init failed");    
        return CAN_QUEUE_ERR_MUTEX_LOCK;
    }
    if(sem_init(&q->used_sem, 0, 0) != 0) 
    {
        LOG_ERROR("can_queue_init: used_sem init failed");
        pthread_mutex_destroy(&q->push_mutex);
        return CAN_QUEUE_ERR_MUTEX_LOCK;
    }
    if(sem_init(&q->free_sem, 0, CAN_QUEUE_MAX_SLOTS) != 0) 
    {
        sem_destroy(&q->used_sem);
        pthread_mutex_destroy(&q->push_mutex);
        return CAN_QUEUE_ERR_MUTEX_LOCK;
    }
    return CAN_QUEUE_ERR_OK;
}

int can_queue_push(can_queue_t *q,const struct canfd_frame *frame)
{
    if (!q || !frame) 
    {
        LOG_ERROR("can_queue_push: frame is NULL");
        return CAN_QUEUE_ERR_INVALID_PARAM;
    }
    if(pthread_mutex_trylock(&q->push_mutex) != 0) 
    {
        LOG_ERROR("can_queue_push: push_mutex trylock failed");
        return CAN_QUEUE_ERR_MUTEX_LOCK;
    }
    if(sem_trywait(&q->free_sem) != 0) 
    {
        if(errno == EAGAIN)
        {
            LOG_INFO("can_queue_push: queue is full");
            pthread_mutex_unlock(&q->push_mutex);
            return CAN_QUEUE_ERR_QUEUE_FULL;
        }
        LOG_ERROR("can_queue_push: free_sem trywait failed");
        pthread_mutex_unlock(&q->push_mutex);
        return CAN_QUEUE_ERR_QUEUE_SEM_ERROR;   
    }
    q->frame[q->head] = *frame;
    q->head++;
    if(q->head >= CAN_QUEUE_MAX_SLOTS) q->head = 0;
    if(sem_post(&q->used_sem) != 0) 
    {
        LOG_ERROR("can_queue_push: used_sem post failed");
        pthread_mutex_unlock(&q->push_mutex);
        return CAN_QUEUE_ERR_QUEUE_SEM_ERROR;
    }
    pthread_mutex_unlock(&q->push_mutex);
    return CAN_QUEUE_ERR_OK;
}

int can_queue_pop(can_queue_t *q, struct canfd_frame *frame)
{
    if (!q || !frame) 
    {
        LOG_ERROR("can_queue_pop: frame is NULL");
        return CAN_QUEUE_ERR_INVALID_PARAM;
    }
    if(sem_trywait(&q->used_sem) != 0) 
    {
        if(errno == EAGAIN)
        {
            LOG_INFO("can_queue_pop: queue is empty");
            return CAN_QUEUE_ERR_QUEUE_EMPTY;
        }
        LOG_ERROR("can_queue_pop: used_sem trywait failed");
        return CAN_QUEUE_ERR_QUEUE_SEM_ERROR;
    }
    *frame = q->frame[q->tail];
    q->tail++;
    if(q->tail >= CAN_QUEUE_MAX_SLOTS) q->tail = 0;
    if(sem_post(&q->free_sem) != 0) 
    {
        LOG_ERROR("can_queue_pop: free_sem post failed");
        return CAN_QUEUE_ERR_QUEUE_SEM_ERROR;
    }
    return CAN_QUEUE_ERR_OK;
}