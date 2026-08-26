#ifndef CORE_RING_H
#define CORE_RING_H

/**
 * core/ring.h — 定长环形缓冲（最近 N 条快照）。
 *
 * 适用场景：生产者写入"最近记录"、消费者按"最新在前"顺序取快照展示，
 * 例如 CAN 原始报文环形缓冲（http_api_can.c）与 DBC 解码结果环形缓冲
 * （http_api_dbc.c）共用同一套槽位计算 / 原子计数 / 互斥写入逻辑。
 *
 * 存储数组由调用方静态分配（ring_t 零初始化后经 ring_init 绑定），
 * 读写两侧通过 ring_total()/ring_latest_pos() 计算游标，互不依赖各自结构。
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <string.h>

typedef struct {
    void      *buf;        /* entry_size * max 的数组（调用方静态分配） */
    size_t     entry_size;
    int        max;
    _Atomic int count;     /* 累计写入条数（只增不减，取模后落在槽位内） */
} ring_t;

/* 绑定存储数组并复位计数 */
static inline void ring_init(ring_t *r, void *buf, size_t entry_size, int max)
{
    if (!r) return;
    r->buf = buf;
    r->entry_size = entry_size;
    r->max = max;
    atomic_store_explicit(&r->count, 0, memory_order_relaxed);
}

/* 写入一条：互斥保护下取槽位并复制 entry（槽位 = 累计写入次数 % max） */
static inline void ring_record(ring_t *r, pthread_mutex_t *mtx, const void *entry)
{
    if (!r || !r->buf || !entry) return;
    pthread_mutex_lock(mtx);
    int pos = atomic_fetch_add_explicit(&r->count, 1, memory_order_relaxed) % r->max;
    memcpy((char *)r->buf + (size_t)pos * r->entry_size, entry, r->entry_size);
    pthread_mutex_unlock(mtx);
}

/* 最新在前的条目数（最多 max 条；读侧免锁取快照，再按需要加锁读内容） */
static inline int ring_total(const ring_t *r)
{
    if (!r) return 0;
    int t = atomic_load_explicit(&r->count, memory_order_relaxed);
    return t > r->max ? r->max : t;
}

/* 最新在前的第 i 条（i=0 为最新）所在槽位。
   槽位必须用"累计写入数"而不是 ring_total() 的截断值计算，
   否则环形回绕后定位会偏移（与旧版 (count-1-i) % max 语义一致）。 */
static inline int ring_latest_pos(const ring_t *r, int i)
{
    if (!r) return 0;
    int raw = atomic_load_explicit(&r->count, memory_order_relaxed);
    if (raw <= 0) return 0;
    int total = raw > r->max ? r->max : raw;
    if (i >= total) return 0;
    return (raw - 1 - i) % r->max;
}

#endif /* CORE_RING_H */
