/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef FRAME_RING_H
#define FRAME_RING_H

#include <stddef.h>
#include <pthread.h>

/**
 * video/frame_ring.h — 视频帧环形队列：采集/推理/显示/编码四阶段零拷贝。
 *
 * 32 个槽位描述符 + 四阶段指针（raw/rgb/nv12/jpeg）。各阶段只移动缓冲指针，
 * 缓冲本身在阶段间传递、游标越过后回池复用：
 *   采集(video worker)  V4L2 mmap 指针直接入槽，延迟 QBUF（真正零拷贝）
 *   推理(ai_task)       claims++ 后锁外解码槽内 raw → 池内 rgb（job 持有）；
 *                       claim 随任务移交 worker（任务在途期间槽位被钉住不可复用）
 *   推理(worker)        resize 读 job rgb，推理结果（名称+坐标）入结果队列，
 *                       rgb 写槽置 rgb_done，unclaim
 *   显示(composer)      消费结果队列最新结果，按 seq 取最新 rgb_done 槽原地画框
 *                       → nv12/jpeg 写槽，置 display_done
 *   编码(rec)           窃取槽内 nv12（所有权转移），置 encode_done（AI 必要流程，
 *                       停摆直接宕机，无原始帧回退）
 *   HTTP 推流            锁内拷贝最新显示槽 jpeg / 最新 raw（多读者，不钉槽）
 *
 * 槽位回收（采集复用条件）：raw/rgb/nv12/jpeg 四个缓冲全部释放。
 * 释放规则在 frame_ring_maintain_locked 统一维护（各提交点触发，遍历 32 槽开销可忽略）：
 *   raw  → QBUF(mmap)/free：非最新帧(raw 客户端钉最新) && claims==0 &&
 *          (ai_active ? infer_done : encode_done||!rec_active)
 *   rgb  → 池：seq <= 最新已渲染 seq（含被跳过的槽）
 *   jpeg → 池：seq != 最新已渲染 seq
 *   nv12 → 池：seq != display_seq 且（encode_done || !rec_active）；
 *             当前显示槽 nv12 始终钉住供 rec 窃取（显示推进后按本规则回收）
 *
 * 并发：单一互斥锁 + 条件变量。锁内：标志/指针交接/池/客户端拷贝（~百 µs）、
 * QBUF 回调（ioctl ~µs）；锁外：JPEG 解码、NPU 推理、渲染、H.264 编码。
 * 锁序约束：不得在持有本锁时获取其他模块锁（q_lock/slot_mutex 等）。
 *
 * 相机重启：quiesce_begin → quiesce_wait（等 claims 归零，上限 1s）→
 * STREAMOFF/munmap → 重新初始化。seq 单调递增永不重置（u64，~190 亿年无回绕）。
 */

#define FRAME_RING_N 32

/* 帧格式（采集侧写入；0=MJPEG，1=YUYV。与 video_stream 的 VIDEO_FMT_* 对齐） */
#define FRAME_RING_FMT_MJPEG 0
#define FRAME_RING_FMT_YUYV  1

/* 缓冲池编号（frame_ring_buf_take/put 的 which 参数） */
#define FRAME_RING_POOL_RGB  0
#define FRAME_RING_POOL_NV12 1
#define FRAME_RING_POOL_JPEG 2

/* 阶段缓冲：槽位只持有指针；cap 记录实际分配容量（回池复用时 len 可小于 cap） */
typedef struct {
    unsigned char *buf;   /* NULL = 该阶段尚未产生或已被消费 */
    size_t len, cap;
} ring_buf_t;

typedef struct pool_item {
    struct pool_item *next;
    unsigned char *buf;
    size_t cap;
} pool_item_t;

typedef struct {
    unsigned long long seq;   /* 帧绝对序号（u64 单调递增，永不回绕） */
    int w, h, fmt;            /* fmt: FRAME_RING_FMT_* */

    /* ---- 采集阶段 ---- */
    ring_buf_t raw;
    int raw_present;          /* 槽内持有 raw（采集已写入、尚未释放） */
    int raw_is_mmap;          /* raw 指向 V4L2 mmap：释放走回调 QBUF，不能 free */
    int raw_vbuf_index;       /* mmap 时对应的驱动缓冲索引（释放回调参数） */
    int raw_claims;           /* 锁外引用 raw 的计数（ai_task 解码至 worker 写槽全程/退让期间） */

    /* ---- 推理阶段 ---- */
    ring_buf_t rgb;           /* JPEG/YUYV 解码 RGB24（池内缓冲） */
    int infer_done;           /* ai_task 解码完成（raw 可释放） */
    int rgb_done;             /* worker 解码 rgb 写入完成（可渲染）；检测结果经结果队列传递 */

    /* ---- 显示阶段 ---- */
    ring_buf_t nv12;          /* 画框 NV12（池内缓冲，供录像编码） */
    ring_buf_t jpeg;          /* 画框 JPEG（池内缓冲，供 HTTP 推流） */
    int display_done;         /* composer 渲染完成 */

    /* ---- 编码阶段 ---- */
    int encode_done;          /* rec 已消费（窃取/尺寸不符跳过） */
} frame_slot_t;

typedef struct frame_ring {
    frame_slot_t slots[FRAME_RING_N];
    unsigned long long produce_seq;   /* 最新已发布 seq（0 = 尚无帧） */
    unsigned long long display_seq;   /* 最新已渲染 seq（钉住最新显示帧，维护规则用） */

    /* 运行模式（消费者侧在锁内更新，维护释放规则用） */
    int ai_active;            /* AI 池运行中（停用时 infer_done 不再推进） */
    int rec_active;           /* 录像进行中 */
    int quiescing;            /* 相机重启退让：禁止新 claim，等待 claims 归零 */

    /* 丢帧/停摆计数器（暴露 /api/video/caps 与定期日志） */
    unsigned long long capture_dropped;   /* 环无空闲槽丢弃 */
    unsigned long long driver_dropped;    /* V4L2 buf.sequence 缺口（驱动侧丢失） */
    unsigned long long capture_stall_ms;  /* 驱动缓冲全被占用时的等待累计 */
    unsigned long long infer_dropped;     /* 解码失败/槽已释放跳过 */
    unsigned long long render_skipped;    /* 最新结果优先被跳过的帧 */
    unsigned long long encode_skipped;    /* 录像尺寸不符等丢弃 */

    /* 阶段缓冲池（spare 列表，跨帧复用免 malloc/mmap churn） */
    pool_item_t *pool_rgb, *pool_nv12, *pool_jpeg;

    pthread_mutex_t lock;
    pthread_cond_t  cond;

    /* mmap 释放回调（video_stream 注册：QBUF 驱动缓冲）。
       锁内调用，ioctl 微秒级，勿在其中获取其他锁 */
    void (*raw_release_cb)(void *arg, int vbuf_index);
    void *raw_release_arg;
} frame_ring_t;

typedef struct {
    unsigned long long produce_seq;
    unsigned long long capture_dropped, driver_dropped, capture_stall_ms;
    unsigned long long infer_dropped, render_skipped, encode_skipped;
    int raw_pinned;            /* 当前被 raw 占用的槽数 */
    int pool_rgb, pool_nv12, pool_jpeg;   /* 池内缓冲个数 */
} frame_ring_stats_t;

/* ---- 生命周期 ---- */
void frame_ring_init(frame_ring_t *r);
void frame_ring_destroy(frame_ring_t *r);
void frame_ring_set_raw_release_cb(frame_ring_t *r,
                                   void (*cb)(void *arg, int vbuf_index),
                                   void *arg);

/* ---- 锁（所有 *_locked 接口须在持锁下调用） ---- */
void frame_ring_lock(frame_ring_t *r);
void frame_ring_unlock(frame_ring_t *r);
void frame_ring_signal(frame_ring_t *r);   /* 广播唤醒（如停止线程时） */

/* ---- 采集（video worker） ---- */
/* 为下一帧分配槽位（seq = produce_seq+1）。调用方填 raw 各字段后 commit。
   无空闲槽返回 NULL：应立即 QBUF 该驱动缓冲并 frame_ring_produce_drop_locked */
frame_slot_t *frame_ring_produce_slot_locked(frame_ring_t *r);
void frame_ring_produce_commit_locked(frame_ring_t *r);
void frame_ring_produce_drop_locked(frame_ring_t *r);   /* capture_dropped++ */
void frame_ring_add_driver_dropped_locked(frame_ring_t *r, unsigned long long n);

/* ---- 推理（ai_task） ---- */
/* 按序 claim 槽 seq 的 raw（claims++）。quiescing / 槽已释放 / 已被复用返回 NULL。
   成功后调用方可在锁外解码 s->raw（mmap 由 claims 保护），解码完 unclaim */
frame_slot_t *frame_ring_infer_claim_locked(frame_ring_t *r, unsigned long long seq);
/* 解码完成：claims--，infer_done=1。decode_ok=0 时额外 infer_dropped++ */
void frame_ring_infer_unclaim_locked(frame_ring_t *r, frame_slot_t *s, int decode_ok);

/* ---- 显示（composer） ---- */
/* 取 seq > next_seq 的最新 rgb_done 槽（最新结果优先）；无则 NULL。
   调用方持锁拷贝 res、取出 rgb 指针并 take 池缓冲，解锁后渲染，再 commit */
frame_slot_t *frame_ring_display_pick_locked(frame_ring_t *r, unsigned long long next_seq);
/* 渲染完成：display_done=1，释放被跳过槽与旧槽的 rgb/jpeg/nv12 */
void frame_ring_display_commit_locked(frame_ring_t *r, frame_slot_t *s);

/* ---- 编码（rec） ---- */
/* 取 seq > next_seq 的最新 display_done 槽（AI 录像）；无则 NULL。
   调用方锁内窃取 nv12（s->nv12.buf 置 NULL，所有权转移给 rec，
   用毕 frame_ring_buf_put_locked 回池），再 encode_advance */
frame_slot_t *frame_ring_encode_pick_locked(frame_ring_t *r, unsigned long long next_seq);
/* rec 已消费（窃取或尺寸不符跳过）：encode_done=1 */
void frame_ring_encode_advance_locked(frame_ring_t *r, frame_slot_t *s);
/* 尺寸不符等丢弃：与 advance 相同的消费标记，另计 encode_skipped++
   （槽内 nv12 留待显示推进后按维护规则回池） */
void frame_ring_encode_skip_locked(frame_ring_t *r, frame_slot_t *s);
/* 最新 raw 槽（仅供录像会话开始前的分辨率探测/帧率实测锁内拷贝读取） */
frame_slot_t *frame_ring_raw_newest_locked(frame_ring_t *r);

/* ---- 模式标志 ---- */
void frame_ring_set_ai_active(frame_ring_t *r, int on);
void frame_ring_set_rec_active(frame_ring_t *r, int on);

/* ---- 事件等待（内部加锁；超时返回 0） ---- */
int frame_ring_wait_new(frame_ring_t *r, unsigned long long last_seq, int timeout_ms);
int frame_ring_wait_render(frame_ring_t *r, unsigned long long next_seq, int timeout_ms);

/* ---- 缓冲池（须持锁） ---- */
/* 取容量 >= min_cap 的池内缓冲（不足时 malloc），回写 ring_buf_t（cap 为实际容量） */
void frame_ring_buf_take_locked(frame_ring_t *r, int which, size_t min_cap, ring_buf_t *out);
/* 归还缓冲（cap 为实际分配容量，来自 take 或槽内 ring_buf_t.cap） */
void frame_ring_buf_put_locked(frame_ring_t *r, int which, unsigned char *buf, size_t cap);

/* ---- 相机重启退让 ---- */
void frame_ring_quiesce_begin(frame_ring_t *r);
/* 等待所有 claims 归零后清空槽（mmap 指针直接摘除不回调——STREAMOFF 后驱动自收，
   非 mmap raw free，阶段缓冲回池），复位完成标志，解除 quiescing。
   成功 0，超时 -1（调用方记 ERROR 后仍须执行 STREAMOFF/munmap） */
int frame_ring_quiesce_wait(frame_ring_t *r, int timeout_ms);
/* 超时兜底：mmap 已 unmap 后 claims 仍不归零（推理线程卡死）时强制清空槽并
   解除 quiescing。迟到写槽因 seq 不匹配走调用方防御分支（rgb 回池），
   迟到 unclaim 有计数守卫，均无副作用 */
void frame_ring_quiesce_force_clear(frame_ring_t *r);

/* ---- 诊断 ---- */
void frame_ring_stats(frame_ring_t *r, frame_ring_stats_t *out);

#endif /* FRAME_RING_H */
