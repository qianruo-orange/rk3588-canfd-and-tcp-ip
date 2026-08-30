/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * rknn_yolo.c — RKNN + YOLO26 检测主模块：模型管理 + 多线程推理池 + 按序渲染。
 *
 * 数据流（ai_task 采集线程 + 3 的倍数个推理 worker + 1 渲染 composer）：
 *   ai_task:  事件驱动等新帧（环条件变量，2s 超时仅用于退出检查）→ 按序
 *             claim 槽 raw（零拷贝引用 V4L2 mmap）→ 锁外解码 RGB24 进池缓冲 →
 *             投递任务队列。claim 随任务移交 worker：任务在途期间槽位被
 *             claims 钉住，不会被采集复用/退让清空
 *   worker i: 出队 → 双线性缩放 → NPU 推理（独立 context，绑定 NPU 核 i%3）→
 *             单输出后处理（NMS）→ rgb 写槽（rgb_done=1）→ unclaim；
 *             NMS 最终结果（目标名称 + 置信度 + 标注框四点坐标）入结果队列
 *   composer: 事件驱动消费结果队列，排空只留最新（seq 严格递增，绝不回退；
 *             旧结果条目直接丢弃）→ EMA 时间平滑 → 槽内 rgb 原地画框 →
 *             JPEG（含标签）+ NV12 写槽（display_done=1，display_seq 推进）；
 *             渲染节拍 ~30fps
 *
 * 帧数据全部走视频帧环形队列（video/frame_ring.h）：采集 raw（V4L2 mmap）、
 * 解码 rgb、画框 nv12/jpeg 四个阶段缓冲只在槽位间移动指针，零拷贝。
 * 推流/录像消费方锁内拷贝或窃取最新显示槽（rknn_yolo_get_frame*），
 * 快照缓冲与按 seq 索引的完成槽（AI_SLOT_N）已随环接入删除。
 *
 * 渲染 seq 单调不回退，画面不会抖动/闪烁；被跳过的旧结果直接回池。
 * 推流客户端在 AI 可用时只推标注帧，画面不再在标注帧/原始帧之间闪烁。
 *
 * 热重载：rknn_yolo_reload() 停止推理池（pool_running=0 → 线程退出/join，
 * 在途任务 rgb 回池 + 摘除槽 claim）→ 释放模型与 context → 重读配置
 * （ai_model/ai_names/ai_threads/ai_conf/ai_nms/ai_interval_ms）
 * → 重建池。由前端上传模型/标签文件与 AI 参数保存触发。
 *
 * 必要流程：AI 推理不可停用。无模型 / NPU 驱动未加载 / 推理失败 → 直接报错：
 * 启动路径整体失败退出；运行期热重载失败由 HTTP 接口报告 saved_reload_failed，
 * 画框流 /video/mjpeg_ai 返回 503（不回退原始帧）。所有失败路径只记日志不崩溃。
 *
 * 图像处理与后处理已拆分为独立模块（yolo_image / yolo_postprocess / yolo_draw），
 * 本文件仅保留：模型生命周期、推理线程池、任务队列、结果队列、
 * 环形队列槽消费、快照与对外 API。
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>

#include "ai/rknn_api.h"
#include "video/video_stream.h"

#include "ai/rknn_yolo.h"
#include "ai/yolo_image.h"
#include "ai/yolo_postprocess.h"
#include "ai/yolo_draw.h"
#include "core/common.h"
#include "core/cpu_affinity.h"
#include "core/log.h"
#include "core/config.h"
#include "watchdog/watchdog.h"
#include "video/video_stream.h"
#include "video/frame_ring.h"

#define AI_MAX_WORKERS 15    /* 推理工作线程上限（3 的倍数 3~15，每核 1~5 个） */
#define AI_QUEUE_CAP   4     /* 任务队列容量（帧），与 worker 数共同约束在途帧内存 */
#define AI_RESULT_CAP  8     /* 结果队列容量（帧），推理结果入队，composer 消费渲染 */

/* 推理任务：解码后的 RGB 帧（池内缓冲，由消费它的 worker 写槽/回池）。
   槽 claim 随任务移交：ai_task claim 后不摘除，worker 写槽时一并 unclaim */
typedef struct {
    unsigned char    *rgb;
    size_t            rgb_cap;
    int               w, h;
    unsigned long long seq;
} yolo_job_t;

/* 推理结果队列条目：NMS 后处理的最终检测结果，只携带目标名称、置信度与
   标注框四点坐标（seq 定位待渲染帧；渲染尺寸取自槽位）。纯元数据，无缓冲所有权 */
typedef struct {
    unsigned long long seq;
    int count;
    struct {
        char  name[YOLO_CLASS_NAME_LEN];
        float conf;
        float x1, y1, x2, y2;
    } dets[YOLO_MAX_DETS];
} yolo_result_item_t;

/* 单个推理工作线程：独立 rknn context，可与其它线程并行 run */
typedef struct {
    rknn_context    ctx;
    unsigned char  *in_buf;                 /* 缩放后输入缓冲 */
    rknn_tensor_attr out_attr;              /* 单输出张量属性 */
} yolo_worker_t;

/* 模块全局上下文 */
static struct {
    int            enabled;                  /* 推理池可用（画框流可用） */
    int            running;                  /* 模块生命周期（destroy 置 0） */
    int            pool_running;             /* 推理池生命周期（reload 置 0 停池） */
    app_ctx_t     *app;
    float          conf, nms;
    int            interval_ms;
    int            nthreads;                 /* 推理线程数（3 的倍数 3~15） */

    unsigned char *model;                    /* 模型文件内存（共享给所有 worker init） */
    size_t         model_len;
    int            in_w, in_h;

    yolo_worker_t  workers[AI_MAX_WORKERS];

    /* 有界任务队列（采集线程生产 / worker 消费） */
    yolo_job_t     jobs[AI_QUEUE_CAP];
    int            q_head, q_tail, q_count;
    pthread_mutex_t q_lock;
    pthread_cond_t  q_not_empty;
    pthread_cond_t  q_not_full;

    /* 有界结果队列（worker 生产推理结果 / composer 消费渲染） */
    yolo_result_item_t results[AI_RESULT_CAP];
    int            r_head, r_tail, r_count;
    pthread_mutex_t r_lock;
    pthread_cond_t  r_not_empty;
    pthread_cond_t  r_not_full;

    pthread_t       w_tid[AI_MAX_WORKERS];
    pthread_t       render_tid;
    int             n_created;               /* 实际创建成功的 worker 线程数（停池据此 join） */

    unsigned long long next_seq;             /* 渲染游标：下一个待渲染 seq（环 seq 单调，0 起步，
                                               跨 reload 保留不重复渲染） */

    /* EMA 平滑状态（仅 composer 线程访问） */
    yolo_result_t   prev;

    /* 类别名表（ai_names 文件，缺失回退内置 COCO 80） */
    yolo_classes_t  classes;
    char            names_path[256];

    unsigned long long last_seq;             /* 采集游标：最近已解码/消费的帧 seq */

    /* 快照：composer 按序写入，仅当新 seq 大于当前时更新 */
    pthread_mutex_t result_mutex;
    yolo_result_t   result;
} g_ai;

static pthread_mutex_t g_reload_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ---- 环形队列访问 ---- */

/* 环句柄：video 模块先于 ai 初始化，正常非 NULL；防御性判空由调用方处理 */
static frame_ring_t *ai_ring(void)
{
    return video_stream_get_ring();
}

static frame_slot_t *ai_slot(frame_ring_t *r, unsigned long long seq)
{
    return &r->slots[seq & (FRAME_RING_N - 1)];
}

/* 任务作废（停池/推理失败/队列拒绝）：摘除槽 claim（raw 恢复可释放）
   并把 rgb 归还池。解码已成功，作废不计 infer_dropped */
static void yolo_job_abandon(const yolo_job_t *job)
{
    frame_ring_t *ring = ai_ring();
    if (!ring) return;
    frame_ring_lock(ring);
    frame_slot_t *s = ai_slot(ring, job->seq);
    if (s->seq == job->seq && s->raw_claims > 0)
        frame_ring_infer_unclaim_locked(ring, s, 1);
    if (job->rgb)
        frame_ring_buf_put_locked(ring, FRAME_RING_POOL_RGB, job->rgb, job->rgb_cap);
    frame_ring_unlock(ring);
}

/* ---- 任务队列 ---- */

static int yolo_queue_push(const yolo_job_t *job)
{
    pthread_mutex_lock(&g_ai.q_lock);
    while (g_ai.q_count >= AI_QUEUE_CAP && g_ai.running && g_ai.pool_running)
        pthread_cond_wait(&g_ai.q_not_full, &g_ai.q_lock);
    if (!g_ai.running || !g_ai.pool_running) {   /* 退出/停池：拒绝投递 */
        pthread_mutex_unlock(&g_ai.q_lock);
        return -1;
    }
    g_ai.jobs[g_ai.q_tail] = *job;
    g_ai.q_tail = (g_ai.q_tail + 1) % AI_QUEUE_CAP;
    g_ai.q_count++;
    pthread_cond_signal(&g_ai.q_not_empty);
    pthread_mutex_unlock(&g_ai.q_lock);
    return 0;
}

static int yolo_queue_pop(yolo_job_t *job)
{
    pthread_mutex_lock(&g_ai.q_lock);
    while (g_ai.q_count == 0 && g_ai.running && g_ai.pool_running)
        pthread_cond_wait(&g_ai.q_not_empty, &g_ai.q_lock);
    if (g_ai.q_count == 0) {   /* 退出/停池且队列空 */
        pthread_mutex_unlock(&g_ai.q_lock);
        return -1;
    }
    *job = g_ai.jobs[g_ai.q_head];
    g_ai.jobs[g_ai.q_head].rgb = NULL;   /* 所有权已移交，防止停池清理时 double-free */
    g_ai.q_head = (g_ai.q_head + 1) % AI_QUEUE_CAP;
    g_ai.q_count--;
    pthread_cond_signal(&g_ai.q_not_full);
    pthread_mutex_unlock(&g_ai.q_lock);
    return 0;
}

/* ---- 推理结果队列（worker 生产 / composer 消费）---- */

static int yolo_result_push(const yolo_result_item_t *item)
{
    pthread_mutex_lock(&g_ai.r_lock);
    while (g_ai.r_count >= AI_RESULT_CAP && g_ai.running && g_ai.pool_running)
        pthread_cond_wait(&g_ai.r_not_full, &g_ai.r_lock);
    if (!g_ai.running || !g_ai.pool_running) {   /* 退出/停池：拒绝入队 */
        pthread_mutex_unlock(&g_ai.r_lock);
        return -1;
    }
    g_ai.results[g_ai.r_tail] = *item;
    g_ai.r_tail = (g_ai.r_tail + 1) % AI_RESULT_CAP;
    g_ai.r_count++;
    pthread_cond_signal(&g_ai.r_not_empty);
    pthread_mutex_unlock(&g_ai.r_lock);
    return 0;
}

/* 非阻塞出队：队列空返回 -1 */
static int yolo_result_try_pop(yolo_result_item_t *item)
{
    pthread_mutex_lock(&g_ai.r_lock);
    if (g_ai.r_count == 0) {
        pthread_mutex_unlock(&g_ai.r_lock);
        return -1;
    }
    *item = g_ai.results[g_ai.r_head];
    g_ai.r_head = (g_ai.r_head + 1) % AI_RESULT_CAP;
    g_ai.r_count--;
    pthread_cond_signal(&g_ai.r_not_full);
    pthread_mutex_unlock(&g_ai.r_lock);
    return 0;
}

/* 阻塞出队：timeout_ms 超时仅用于调用方周期性检查退出条件；
   超时/停池/退出且队列空返回 -1 */
static int yolo_result_pop(yolo_result_item_t *item, int timeout_ms)
{
    pthread_mutex_lock(&g_ai.r_lock);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    while (g_ai.r_count == 0 && g_ai.running && g_ai.pool_running) {
        if (pthread_cond_timedwait(&g_ai.r_not_empty, &g_ai.r_lock, &ts) != 0) {
            pthread_mutex_unlock(&g_ai.r_lock);
            return -1;
        }
    }
    if (g_ai.r_count == 0) {
        pthread_mutex_unlock(&g_ai.r_lock);
        return -1;
    }
    *item = g_ai.results[g_ai.r_head];
    g_ai.r_head = (g_ai.r_head + 1) % AI_RESULT_CAP;
    g_ai.r_count--;
    pthread_cond_signal(&g_ai.r_not_full);
    pthread_mutex_unlock(&g_ai.r_lock);
    return 0;
}

/* ---- 结果快照（composer 按序更新，单调递增） ---- */

static void snap_result(const yolo_result_t *res)
{
    pthread_mutex_lock(&g_ai.result_mutex);
    if (res->seq > g_ai.result.seq) g_ai.result = *res;
    pthread_mutex_unlock(&g_ai.result_mutex);
}

/* 时间平滑：cur 中与 prev 同类且 IoU 足够大的框，位置/置信度做 EMA
   （新帧权重 0.4，保留 0.6 历史），降低逐帧检测抖动 */
static void yolo_smooth(const yolo_result_t *prev, yolo_result_t *cur)
{
    if (prev->count <= 0 || cur->count <= 0) return;
    const float alpha = 0.4f;
    for (int i = 0; i < cur->count; i++) {
        yolo_det_t *d = &cur->dets[i];
        float best_iou = 0.3f;
        int bi = -1;
        for (int j = 0; j < prev->count; j++) {
            const yolo_det_t *p = &prev->dets[j];
            if (p->cls != d->cls) continue;
            float ix1 = d->x1 > p->x1 ? d->x1 : p->x1;
            float iy1 = d->y1 > p->y1 ? d->y1 : p->y1;
            float ix2 = d->x2 < p->x2 ? d->x2 : p->x2;
            float iy2 = d->y2 < p->y2 ? d->y2 : p->y2;
            if (ix2 <= ix1 || iy2 <= iy1) continue;
            float inter = (ix2 - ix1) * (iy2 - iy1);
            float uni = (d->x2-d->x1)*(d->y2-d->y1) + (p->x2-p->x1)*(p->y2-p->y1) - inter;
            if (uni <= 0.0f) continue;
            float iou = inter / uni;
            if (iou > best_iou) { best_iou = iou; bi = j; }
        }
        if (bi >= 0) {
            const yolo_det_t *p = &prev->dets[bi];
            d->x1 = alpha*d->x1 + (1.0f-alpha)*p->x1;
            d->y1 = alpha*d->y1 + (1.0f-alpha)*p->y1;
            d->x2 = alpha*d->x2 + (1.0f-alpha)*p->x2;
            d->y2 = alpha*d->y2 + (1.0f-alpha)*p->y2;
            d->conf = alpha*d->conf + (1.0f-alpha)*p->conf;
        }
    }
}

/* ---- 推理工作线程 ---- */

static void *yolo_worker_task(void *arg)
{
    int idx = (int)(intptr_t)arg;
    cpu_bind_big();
    yolo_worker_t *wk = &g_ai.workers[idx];
    frame_ring_t *ring = ai_ring();

    while (g_ai.running && g_ai.pool_running) {
        if (!ring) { usleep(200000); continue; }   /* 防御：video 未初始化 */

        yolo_job_t job;
        if (yolo_queue_pop(&job) < 0) break;      /* 停池/退出且队列空 */
        if (!g_ai.running || !g_ai.pool_running) { yolo_job_abandon(&job); break; }

        /* letterbox+padding 预处理（等比例 + 114 灰边）；几何参数供后处理逆映射 */
        float lb_scale; int lb_pad_x, lb_pad_y;
        yolo_rgb_letterbox(job.rgb, job.w, job.h, wk->in_buf, g_ai.in_w, g_ai.in_h,
                           &lb_scale, &lb_pad_x, &lb_pad_y);

        rknn_input in;
        memset(&in, 0, sizeof(in));
        in.index = 0;
        in.type  = RKNN_TENSOR_UINT8;
        in.fmt   = RKNN_TENSOR_NHWC;
        in.size  = (uint32_t)(g_ai.in_w * g_ai.in_h * 3);
        in.buf   = wk->in_buf;
        if (rknn_inputs_set(wk->ctx, 1, &in) != RKNN_SUCC) { yolo_job_abandon(&job); continue; }
        if (rknn_run(wk->ctx, NULL) != RKNN_SUCC) { yolo_job_abandon(&job); continue; }

        rknn_output output;
        memset(&output, 0, sizeof(output));
        output.want_float = 1;
        if (rknn_outputs_get(wk->ctx, 1, &output, NULL) != RKNN_SUCC) {
            yolo_job_abandon(&job);
            continue;
        }

        const float *out_buf[1] = { (const float *)output.buf };
        yolo_result_t res;
        memset(&res, 0, sizeof(res));
        res.seq = job.seq;
        res.w   = job.w;
        res.h   = job.h;
        res.count = yolo_postprocess(out_buf, &wk->out_attr, 1,
                                     lb_scale, lb_pad_x, lb_pad_y, job.w, job.h,
                                     res.dets, YOLO_MAX_DETS, g_ai.conf, g_ai.nms, NULL);
        rknn_outputs_release(wk->ctx, 1, &output);

        /* 停池竞态：推理期间 pool_running 被清 → 作废任务（不写槽，
           避免停池后槽内滞留 rgb 阻碍该槽复用） */
        if (!g_ai.running || !g_ai.pool_running) { yolo_job_abandon(&job); continue; }

        /* rgb 写槽（claim 钉住槽位：任务在途期间不会被复用/清空，
           故 s->seq == job.seq 必然成立；防御分支防退让强制清空的迟到写） */
        frame_ring_lock(ring);
        frame_slot_t *s = ai_slot(ring, job.seq);
        if (s->seq == job.seq && s->raw_claims > 0) {
            s->rgb.buf  = job.rgb;
            s->rgb.cap  = job.rgb_cap;
            s->rgb.len  = (size_t)job.w * job.h * 3;
            s->rgb_done = 1;
            frame_ring_infer_unclaim_locked(ring, s, 1);
        } else {
            frame_ring_buf_put_locked(ring, FRAME_RING_POOL_RGB, job.rgb, job.rgb_cap);
            if (s->seq == job.seq && s->raw_claims > 0)
                frame_ring_infer_unclaim_locked(ring, s, 1);
        }
        frame_ring_unlock(ring);

        /* NMS 后处理结果入结果队列：条目只携带目标名称、置信度与标注框
           四点坐标（纯元数据；rgb 已入槽，随显示推进由维护规则释放） */
        yolo_result_item_t item;
        memset(&item, 0, sizeof(item));
        item.seq = job.seq;
        for (int i = 0; i < res.count && i < YOLO_MAX_DETS; i++) {
            const yolo_det_t *d = &res.dets[i];
            const char *nm = (d->cls >= 0 && d->cls < g_ai.classes.count)
                             ? g_ai.classes.names[d->cls] : "obj";
            snprintf(item.dets[i].name, sizeof(item.dets[i].name), "%s", nm);
            item.dets[i].conf = d->conf;
            item.dets[i].x1 = d->x1; item.dets[i].y1 = d->y1;
            item.dets[i].x2 = d->x2; item.dets[i].y2 = d->y2;
        }
        item.count = res.count;
        yolo_result_push(&item);   /* 停池/退出拒绝入队时条目直接丢弃（无资源持有） */
    }
    return NULL;
}

/* ---- 渲染 composer：结果队列最新结果优先（seq 单调不回退）+ ~30fps 节拍 ----

   严格按 seq 顺序消费会因 worker 乱序完成而等待缺口（实测卡顿 100~700ms）。
   改为消费结果队列中最新完成结果：渲染 seq 严格递增（绝不回退，画面不抖动），
   被跳过的旧结果条目直接丢弃（纯元数据，无资源）；EMA 平滑本身即可容忍帧间小跳跃。
   渲染节拍与推流/显示速率匹配（~30fps），同时把 OpenCV 渲染 + JPEG 编码 +
   NV12 转换的 CPU 锁定在上限内。 */

/* 名称回查类别下标（条目只携带名称；正常必命中，未命中兜底 0 保着色合法） */
static int yolo_class_index(const char *name)
{
    for (int i = 0; i < g_ai.classes.count; i++)
        if (strcmp(g_ai.classes.names[i], name) == 0) return i;
    return 0;
}

static void *yolo_render_task(void *arg)
{
    (void)arg;
    cpu_bind_big();
    frame_ring_t *ring = ai_ring();
    struct timespec last_render = { 0, 0 };
    while (g_ai.running && g_ai.pool_running) {
        if (!ring) { usleep(200000); continue; }   /* 防御：video 未初始化 */

        /* 事件驱动：等推理结果入队（40ms 超时仅用于退出检查） */
        yolo_result_item_t item;
        if (yolo_result_pop(&item, 40) != 0) continue;

        /* 最新结果优先：排空队列只留最新（旧结果条目直接丢弃） */
        yolo_result_item_t newer;
        while (yolo_result_try_pop(&newer) == 0) item = newer;
        /* 结果须晚于已渲染 seq（渲染 seq 严格递增，绝不回退） */
        if (item.seq <= g_ai.next_seq) continue;

        unsigned long long seq = item.seq;
        int w = 0, h = 0;
        ring_buf_t nv12b = { 0 }, jpegb = { 0 };
        /* 无 MJPEG 观看者时跳过 JPEG 编码与池占用（省 CPU） */
        int want_jpeg = video_stream_client_count() > 0;
        frame_ring_lock(ring);
        /* rgb_done 钉住槽位（维护规则在显示推进后才释放 rgb）；
           防御分支防退让强制清空的迟到渲染 */
        frame_slot_t *s = ai_slot(ring, seq);
        if (s->seq != seq || !s->rgb_done || !s->rgb.buf) {
            frame_ring_unlock(ring);
            continue;
        }
        w = s->w; h = s->h;
        /* 渲染输出缓冲：池内 NV12（供录像窃取）与 JPEG（供推流拷贝）。
           s->rgb 由 rgb_done && seq > display_seq 钉住，锁外渲染期间无人释放 */
        frame_ring_buf_take_locked(ring, FRAME_RING_POOL_NV12, (size_t)w * h * 3 / 2, &nv12b);
        if (want_jpeg)
            frame_ring_buf_take_locked(ring, FRAME_RING_POOL_JPEG, 64 * 1024, &jpegb);
        g_ai.next_seq = seq;
        frame_ring_unlock(ring);

        /* 结果条目 → 渲染结果：名称回查类别下标（标签含置信度） */
        yolo_result_t disp;
        memset(&disp, 0, sizeof(disp));
        disp.seq = seq;
        disp.w = w; disp.h = h;
        disp.count = item.count;
        for (int i = 0; i < item.count && i < YOLO_MAX_DETS; i++) {
            disp.dets[i].cls  = yolo_class_index(item.dets[i].name);
            disp.dets[i].conf = item.dets[i].conf;
            disp.dets[i].x1 = item.dets[i].x1;
            disp.dets[i].y1 = item.dets[i].y1;
            disp.dets[i].x2 = item.dets[i].x2;
            disp.dets[i].y2 = item.dets[i].y2;
        }

        /* 锁外渲染：EMA 平滑（prev 仅本线程访问）+ 槽内 rgb 原地画框标签。
           无 MJPEG 观看者时跳过 JPEG 编码（省 CPU）；观看者接入后下一帧
           （≤33ms）恢复编码，get_frame 对 len==0 帧返回 -1 由客户端等待 */
        yolo_smooth(&g_ai.prev, &disp);
        unsigned char *jbuf = jpegb.buf;
        size_t jcap = jpegb.cap, jlen = 0;
        int ok = (nv12b.buf &&
                  yolo_render_annotated(s->rgb.buf, w, h, &disp, &g_ai.classes,
                                        want_jpeg ? &jbuf : NULL,
                                        want_jpeg ? &jcap : NULL,
                                        want_jpeg ? &jlen : NULL) == 0);
        jpegb.buf = jbuf; jpegb.cap = jcap;   /* realloc 可能更换指针 */
        if (ok) {
            yolo_rgb_to_nv12_fast(s->rgb.buf, w, h, nv12b.buf);
            jpegb.len = jlen;
            g_ai.prev = disp;
            snap_result(&disp);
        }

        frame_ring_lock(ring);
        /* 槽可能已被退让清空（相机重启）：校验 seq 后提交，否则缓冲回池 */
        if (s->seq == seq && s->rgb_done && s->rgb.buf) {
            if (ok) {
                s->nv12     = nv12b;
                s->nv12.len = (size_t)w * h * 3 / 2;
                s->jpeg     = jpegb;
            } else {
                /* 渲染失败（编码 OOM 等）：仍推进 display（释放 rgb 防槽卡死），
                   推流/录像客户端随 get_frame 失败回退原始帧 */
                frame_ring_buf_put_locked(ring, FRAME_RING_POOL_NV12, nv12b.buf, nv12b.cap);
                frame_ring_buf_put_locked(ring, FRAME_RING_POOL_JPEG, jpegb.buf, jpegb.cap);
            }
            frame_ring_display_commit_locked(ring, s);
        } else {
            frame_ring_buf_put_locked(ring, FRAME_RING_POOL_NV12, nv12b.buf, nv12b.cap);
            frame_ring_buf_put_locked(ring, FRAME_RING_POOL_JPEG, jpegb.buf, jpegb.cap);
        }
        frame_ring_unlock(ring);

        /* 渲染节拍 ~30fps：超出部分在下一次渲染前补眠，平滑帧间隔 */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed_ms = (now.tv_sec - last_render.tv_sec) * 1000 +
                          (now.tv_nsec - last_render.tv_nsec) / 1000000;
        if (last_render.tv_sec && elapsed_ms < 33) {
            struct timespec slp = { 0, (33 - elapsed_ms) * 1000000L };
            nanosleep(&slp, NULL);
        }
        clock_gettime(CLOCK_MONOTONIC, &last_render);
    }
    return NULL;
}

/* ---- 模型加载与生命周期 ---- */

/* 模型文件读入内存；失败返回 -1 */
static int load_model_file(const char *path, unsigned char **buf_out, size_t *len_out)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return -1; }
    long sz = ftell(fp);
    if (sz <= 0 || fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return -1; }
    unsigned char *buf = malloc((size_t)sz);
    if (!buf) { fclose(fp); return -1; }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) { free(buf); fclose(fp); return -1; }
    fclose(fp);
    *buf_out = buf;
    *len_out = (size_t)sz;
    return 0;
}

/* 创建一个推理上下文：rknn_init + 绑定 NPU 核 + 查输出属性 + 分配输入缓冲 */
static int yolo_worker_setup(yolo_worker_t *wk, int idx)
{
    wk->ctx = 0;
    wk->in_buf = NULL;
    memset(&wk->out_attr, 0, sizeof(wk->out_attr));

    int ret = rknn_init(&wk->ctx, g_ai.model, (uint32_t)g_ai.model_len, 0, NULL);
    if (ret != RKNN_SUCC) {
        LOG_ERROR("ai: worker rknn_init failed (%d)", ret);
        wk->ctx = 0;
        return -1;
    }
    /* 绑定 NPU 核：worker i 固定用核 i%3，三核等量分摊（线程数为 3 的倍数） */
    static const rknn_core_mask kCoreMask[3] = {
        RKNN_NPU_CORE_0, RKNN_NPU_CORE_1, RKNN_NPU_CORE_2,
    };
    if (rknn_set_core_mask(wk->ctx, kCoreMask[idx % 3]) != RKNN_SUCC)
        LOG_ERROR("ai: worker %d set_core_mask failed", idx);

    wk->out_attr.index = 0;
    if (rknn_query(wk->ctx, RKNN_QUERY_OUTPUT_ATTR, &wk->out_attr,
                   sizeof(wk->out_attr)) != RKNN_SUCC) {
        LOG_ERROR("ai: worker rknn_query OUTPUT_ATTR failed");
        return -1;
    }
    wk->in_buf = malloc((size_t)g_ai.in_w * g_ai.in_h * 3);
    if (!wk->in_buf) {
        LOG_ERROR("ai: worker input buffer alloc failed");
        return -1;
    }
    return 0;
}

/* 释放模型与全部 worker 资源（停池后调用） */
static void ai_free_workers(void)
{
    for (int i = 0; i < g_ai.nthreads; i++) {
        yolo_worker_t *wk = &g_ai.workers[i];
        if (wk->in_buf) { free(wk->in_buf); wk->in_buf = NULL; }
        if (wk->ctx) { rknn_destroy(wk->ctx); wk->ctx = 0; }
    }
    free(g_ai.model);
    g_ai.model = NULL;
}

/* 停止推理池：置 pool_running=0 → 唤醒各线程 → join → 清理队列/模型。
   可安全重入（池未启动时 n_created=0，各清理为幂等操作）。
   在途任务：worker 推理中检出停池后作废（rgb 回池 + 摘除 claim），
   队列残余任务由本函数统一作废 */
static void ai_stop_pool(void)
{
    frame_ring_t *ring = ai_ring();
    g_ai.pool_running = 0;
    /* raw 释放规则切回编码路径（ai_active=0），并唤醒等待中的
       ai_task（wait_new）与 composer（wait_render） */
    if (ring) {
        frame_ring_set_ai_active(ring, 0);
        frame_ring_signal(ring);
    }
    pthread_cond_broadcast(&g_ai.q_not_empty);
    pthread_cond_broadcast(&g_ai.q_not_full);
    pthread_cond_broadcast(&g_ai.r_not_empty);
    pthread_cond_broadcast(&g_ai.r_not_full);

    for (int i = 0; i < g_ai.n_created; i++)
        pthread_join(g_ai.w_tid[i], NULL);
    g_ai.n_created = 0;
    if (g_ai.render_tid) {
        pthread_join(g_ai.render_tid, NULL);
        g_ai.render_tid = 0;
    }

    /* 释放队列中未被消费的任务（已消费槽位 rgb 已置 NULL） */
    for (int i = 0; i < AI_QUEUE_CAP; i++) {
        if (g_ai.jobs[i].rgb) {
            yolo_job_t job = g_ai.jobs[i];
            g_ai.jobs[i].rgb = NULL;
            yolo_job_abandon(&job);
        }
    }
    g_ai.q_head = g_ai.q_tail = g_ai.q_count = 0;

    /* 结果队列条目为纯元数据（无缓冲/claim），直接清空即可 */
    g_ai.r_head = g_ai.r_tail = g_ai.r_count = 0;

    /* 清空快照与渲染状态（新池从干净状态开始；next_seq 保留跨 reload 连续） */
    pthread_mutex_lock(&g_ai.result_mutex);
    memset(&g_ai.result, 0, sizeof(g_ai.result));
    pthread_mutex_unlock(&g_ai.result_mutex);
    memset(&g_ai.prev, 0, sizeof(g_ai.prev));
    g_ai.last_seq = 0;

    ai_free_workers();
    g_ai.enabled = 0;
}

/* 启动推理池：重读配置 → 加载模型 → 建 worker → 启动线程。
   AI 为必要流程：任一失败即返回 -1 直接报错（启动路径整体失败退出；
   热重载路径由 HTTP 接口向调用方报告），enabled=0 仅供失败后运行态空转 */
static int ai_start_pool(void)
{
    app_ctx_t *app = g_ai.app;
    struct app_config_t *cfg = app->cfg;

    /* 重读运行时配置（支持热重载） */
    g_ai.conf = cfg->ai_conf > 0 ? cfg->ai_conf : 0.25f;
    g_ai.nms  = cfg->ai_nms  > 0 ? cfg->ai_nms  : 0.45f;
    g_ai.interval_ms = cfg->ai_interval_ms > 0 ? cfg->ai_interval_ms : 10;
    int t = cfg->ai_threads > 0 ? cfg->ai_threads : 3;
    if (t > AI_MAX_WORKERS) t = AI_MAX_WORKERS;
    if (t < 3) t = 3;
    g_ai.nthreads = t - (t % 3);   /* 必须是 3 的倍数：3~15，每核 1~5 个 worker */

    safe_strncpy(g_ai.names_path, sizeof(g_ai.names_path),
                 cfg->ai_names[0] ? cfg->ai_names : "config/coco.names");

    char model_path[256];
    safe_strncpy(model_path, sizeof(model_path),
                 cfg->ai_model[0] ? cfg->ai_model : "config/yolo26.rknn");
    if (load_model_file(model_path, &g_ai.model, &g_ai.model_len) < 0) {
        LOG_ERROR("ai: model '%s' not found — AI unavailable (no AI)", model_path);
        g_ai.enabled = 0;
        return -1;
    }

    /* 用第一个上下文获取输入尺寸（静态模型）与输出数校验 */
    rknn_context probe = 0;
    if (rknn_init(&probe, g_ai.model, (uint32_t)g_ai.model_len, 0, NULL) != RKNN_SUCC) {
        LOG_ERROR("ai: rknn_init failed: model incompatible or NPU unavailable — AI unavailable");
        ai_free_workers();
        g_ai.enabled = 0;
        return -1;
    }
    rknn_input_output_num io_num;
    memset(&io_num, 0, sizeof(io_num));
    if (rknn_query(probe, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num)) != RKNN_SUCC ||
        io_num.n_input < 1 || io_num.n_output != 1) {
        LOG_ERROR("ai: outputs=%u != 1, 需官方 rknn 单输出模型 (yolo export format=rknn) — AI unavailable",
                  io_num.n_output);
        rknn_destroy(probe);
        ai_free_workers();
        g_ai.enabled = 0;
        return -1;
    }
    rknn_tensor_attr in_attr;
    memset(&in_attr, 0, sizeof(in_attr));
    in_attr.index = 0;
    if (rknn_query(probe, RKNN_QUERY_INPUT_ATTR, &in_attr, sizeof(in_attr)) != RKNN_SUCC) {
        LOG_ERROR("ai: rknn_query INPUT_ATTR failed — AI unavailable");
        rknn_destroy(probe);
        ai_free_workers();
        g_ai.enabled = 0;
        return -1;
    }
    rknn_destroy(probe);

    /* 输入尺寸：优先用模型静态尺寸；dims 全 0（动态模型）时用配置值 */
    if (in_attr.fmt == RKNN_TENSOR_NCHW && in_attr.n_dims >= 4) {
        g_ai.in_w = (int)in_attr.dims[3];
        g_ai.in_h = (int)in_attr.dims[2];
    } else if (in_attr.fmt == RKNN_TENSOR_NHWC && in_attr.n_dims >= 4) {
        g_ai.in_w = (int)in_attr.dims[2];
        g_ai.in_h = (int)in_attr.dims[1];
    }
    if (g_ai.in_w <= 0 || g_ai.in_h <= 0) {
        int s = cfg->ai_input_size > 0 ? cfg->ai_input_size : 640;
        g_ai.in_w = s;
        g_ai.in_h = s;
    }

    /* 类别名表：官方 COCO 格式文件（缺失/为空时回退内置 80 类） */
    int ncls = yolo_classes_load(g_ai.names_path, &g_ai.classes);

    /* 每个工作线程独立上下文，绑定独立 NPU 核 */
    for (int i = 0; i < g_ai.nthreads; i++) {
        if (yolo_worker_setup(&g_ai.workers[i], i) < 0) {
            ai_free_workers();
            g_ai.enabled = 0;
            LOG_ERROR("ai: pool start failed — AI unavailable (no AI)");
            return -1;
        }
    }

    /* 启动推理工作线程池 + 按序渲染线程 */
    g_ai.pool_running = 1;
    for (int i = 0; i < g_ai.nthreads; i++) {
        if (pthread_create(&g_ai.w_tid[i], NULL, yolo_worker_task,
                           (void *)(intptr_t)i) != 0) {
            LOG_ERROR("ai: worker thread %d create failed", i);
            break;
        }
        g_ai.n_created = i + 1;
    }
    if (pthread_create(&g_ai.render_tid, NULL, yolo_render_task, NULL) != 0) {
        LOG_ERROR("ai: render thread create failed (annotated stream stalled)");
        g_ai.render_tid = 0;
    }
    g_ai.enabled = 1;
    /* 环内 raw 释放规则切回推理路径（infer_done 门槛） */
    if (ai_ring()) frame_ring_set_ai_active(ai_ring(), 1);
    LOG_INFO("ai: model '%s' loaded, classes '%s' (%d), input %dx%d, "
             "single output, threads %d (NPU core i%%3), conf %.2f nms %.2f",
             model_path, g_ai.names_path, ncls, g_ai.in_w, g_ai.in_h,
             g_ai.nthreads, g_ai.conf, g_ai.nms);
    return 0;
}

/* 热重载：停止旧池 → 重读配置重建。由前端上传模型/标签文件、修改 AI 参数触发。
   失败返回 -1（enabled=0）：调用方（HTTP 接口）向客户端直接报告 reload 失败 */
int rknn_yolo_reload(void)
{
    pthread_mutex_lock(&g_reload_mutex);
    if (!g_ai.running) {
        pthread_mutex_unlock(&g_reload_mutex);
        return -1;
    }
    LOG_INFO("ai: reloading inference pool...");
    ai_stop_pool();
    ai_start_pool();
    pthread_mutex_unlock(&g_reload_mutex);
    return g_ai.enabled ? 0 : -1;
}

int rknn_yolo_init(void *arg)
{
    memset(&g_ai, 0, sizeof(g_ai));
    app_ctx_t *app = (app_ctx_t *)arg;
    g_ai.app = app;
    g_ai.running = 1;
    pthread_mutex_init(&g_ai.result_mutex, NULL);
    pthread_mutex_init(&g_ai.q_lock, NULL);
    pthread_cond_init(&g_ai.q_not_empty, NULL);
    pthread_cond_init(&g_ai.q_not_full, NULL);
    pthread_mutex_init(&g_ai.r_lock, NULL);
    pthread_cond_init(&g_ai.r_not_empty, NULL);
    pthread_cond_init(&g_ai.r_not_full, NULL);

    if (!app || !app->cfg) {
        LOG_ERROR("ai: init without config");
        return -1;
    }
    /* AI 为必要流程：无 AI（模型缺失/NPU 不可用）直接报错，服务启动失败 */
    if (ai_start_pool() < 0) {
        LOG_ERROR("ai: mandatory inference pool failed to start — no AI, aborting startup");
        return -1;
    }
    return 0;
}

void rknn_yolo_destroy(void *arg)
{
    (void)arg;
    g_ai.running = 0;
    ai_stop_pool();

    pthread_mutex_destroy(&g_ai.q_lock);
    pthread_cond_destroy(&g_ai.q_not_empty);
    pthread_cond_destroy(&g_ai.q_not_full);
    pthread_mutex_destroy(&g_ai.r_lock);
    pthread_cond_destroy(&g_ai.r_not_empty);
    pthread_cond_destroy(&g_ai.r_not_full);
    pthread_mutex_destroy(&g_ai.result_mutex);
    g_ai.enabled = 0;
}

int rknn_yolo_enabled(void)
{
    return g_ai.enabled;
}

/* ---- 显示槽快照访问（供推流客户端/录像线程）---- */

int rknn_yolo_get_frame(unsigned char **data, size_t *len, unsigned long long *seq)
{
    if (!g_ai.enabled || !data || !len || !seq) return -1;
    frame_ring_t *ring = ai_ring();
    if (!ring) return -1;
    frame_ring_lock(ring);
    frame_slot_t *s = ai_slot(ring, ring->display_seq);
    if (s->seq != ring->display_seq || !s->jpeg.buf || s->jpeg.len == 0) {
        frame_ring_unlock(ring);
        return -1;
    }
    unsigned char *c = malloc(s->jpeg.len);
    if (!c) {
        frame_ring_unlock(ring);
        return -1;
    }
    memcpy(c, s->jpeg.buf, s->jpeg.len);
    *len = s->jpeg.len;
    *seq = s->seq;
    frame_ring_unlock(ring);
    *data = c;
    return 0;
}

unsigned long long rknn_yolo_get_frame_seq(void)
{
    if (!g_ai.enabled) return 0;
    frame_ring_t *ring = ai_ring();
    if (!ring) return 0;
    frame_ring_lock(ring);
    unsigned long long s = ring->display_seq;
    frame_ring_unlock(ring);
    return s;
}

/* ---- AI 采集线程（main 模块框架 task）：等新帧 → claim → 解码 → 投递队列 ---- */
void *rknn_ai_task(void *arg)
{
    app_ctx_t *app = (app_ctx_t *)arg;
    if (!app) return NULL;

    cpu_bind_big();
    frame_ring_t *ring = ai_ring();

    while (app->running && g_ai.running) {
        watchdog_feed_self("ai");

        /* 未启用 / 池未运行（重载中）/ video 未初始化：空转喂狗，不占推理资源。
           重载后 enabled 恢复时自动继续采集 */
        if (!ring || !g_ai.enabled || !g_ai.pool_running) { usleep(200000); continue; }

        /* 事件驱动：阻塞等新帧（2s 超时仅用于周期性检查退出/热插拔），
           替代旧版 10ms 轮询，唤醒即处理、无帧即休眠 */
        if (!frame_ring_wait_new(ring, g_ai.last_seq, 2000)) continue;

        /* 按序 claim 下一个未处理帧的 raw（与旧轮询版语义一致：每帧都解码，
           队列背压即限速）。槽已被编码回退消费/退让清空时顺延跳过。
           claim 随任务移交 worker（写槽时 unclaim），任务在途期间槽位
           被 claims 钉住，不会被采集复用，raw 也不会被释放 */
        frame_ring_lock(ring);
        frame_slot_t *s = NULL;
        while (g_ai.last_seq < ring->produce_seq) {
            s = frame_ring_infer_claim_locked(ring, g_ai.last_seq + 1);
            if (s) break;
            g_ai.last_seq++;
        }
        if (!s) { frame_ring_unlock(ring); continue; }
        unsigned long long seq = s->seq;
        int fmt = s->fmt, w = s->w, h = s->h;
        unsigned char *raw = s->raw.buf;
        size_t raw_len = s->raw.len;
        /* 解码目标：池内 RGB（job 持有，worker 写槽时移交槽） */
        ring_buf_t rgbb;
        frame_ring_buf_take_locked(ring, FRAME_RING_POOL_RGB, (size_t)w * h * 3, &rgbb);
        frame_ring_unlock(ring);

        /* 锁外解码：槽 raw 由 claim 保护（零拷贝读 mmap，不再整帧拷贝） */
        int decode_ok = 0;
        if (raw && rgbb.buf && w > 0 && h > 0) {
            if (fmt == FRAME_RING_FMT_MJPEG)
                decode_ok = yolo_jpeg_to_rgb_buf(raw, raw_len, rgbb.buf, w, h) == 0;
            else   /* YUYV */
                { yolo_yuyv_to_rgb(raw, w, h, rgbb.buf); decode_ok = 1; }
        }
        g_ai.last_seq = seq;
        if (!decode_ok) {
            frame_ring_lock(ring);
            if (rgbb.buf)
                frame_ring_buf_put_locked(ring, FRAME_RING_POOL_RGB, rgbb.buf, rgbb.cap);
            frame_ring_infer_unclaim_locked(ring, s, 0);   /* 解码失败：infer_dropped++ */
            frame_ring_unlock(ring);
            usleep(g_ai.interval_ms * 1000);   /* 坏帧退避，避免忙转 */
            continue;
        }

        yolo_job_t job = { .rgb = rgbb.buf, .rgb_cap = rgbb.cap,
                           .w = w, .h = h, .seq = seq };
        if (yolo_queue_push(&job) != 0)
            yolo_job_abandon(&job);   /* 停池/退出中：claim 摘除 + rgb 回池 */

        /* 帧间隔下限（ai_interval_ms 语义）：事件驱动下默认 10ms 无感，
           调大即可限制推理帧率，不再与相机帧率耦合 */
        usleep(g_ai.interval_ms * 1000);
    }
    return NULL;
}
