/**
 * rknn_yolo.c — RKNN + YOLO26 检测主模块：模型管理 + 多线程推理池 + 按序渲染。
 *
 * 数据流（ai_task 采集线程 + 3 的倍数个推理 worker + 1 渲染 composer）：
 *   ai_task:  video_stream_get_frame → 解码 RGB24 → 投递任务队列（seq 去重）
 *   worker i: 出队 → 双线性缩放 → NPU 推理（独立 context，绑定 NPU 核 i%3）→
 *             单输出后处理 → 提交到按 seq 索引的完成槽
 *   composer: 最新结果优先消费完成槽（seq 严格递增，绝不回退；旧结果释放）→
 *             EMA 时间平滑 → OpenCV 画框 JPEG（含标签）+ 同步 NV12 转换 →
 *             更新帧快照（JPEG 供 /video/mjpeg_ai 推流，NV12 供录像编码复用）；
 *             渲染节拍 ~30fps，与推流速率匹配并锁定渲染/编码 CPU
 *
 * 渲染 seq 单调不回退，画面不会抖动/闪烁；被跳过的旧结果直接释放。
 * 推流客户端在 AI 可用时只推标注帧，画面不再在标注帧/原始帧之间闪烁。
 *
 * 热重载：rknn_yolo_reload() 停止推理池（pool_running=0 → 线程退出/join）→
 * 释放模型与 context → 重读配置（ai_enable/ai_model/ai_names/ai_threads/
 * ai_conf/ai_nms/ai_interval_ms）→ 重建池。由前端上传模型/标签文件与
 * AI 参数保存触发。
 *
 * 优雅降级：无模型 / NPU 驱动未加载 / 推理失败 → enabled=0，原视频流照常，
 * 画框流客户端回退到原始帧。所有失败路径只记日志不崩溃。
 *
 * 图像处理与后处理已拆分为独立模块（yolo_image / yolo_postprocess / yolo_draw），
 * 本文件仅保留：模型生命周期、推理线程池、任务队列、重排槽、快照与对外 API。
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

#include "ai/rknn_yolo.h"
#include "ai/yolo_image.h"
#include "ai/yolo_postprocess.h"
#include "ai/yolo_draw.h"
#include "core/common.h"
#include "core/log.h"
#include "core/config.h"
#include "watchdog/watchdog.h"
#include "video/video_stream.h"

#define AI_MAX_WORKERS 15    /* 推理工作线程上限（3 的倍数 3~15，每核 1~5 个） */
#define AI_QUEUE_CAP   4     /* 任务队列容量（帧），与 worker 数共同约束在途帧内存 */
#define AI_SLOT_N      32    /* 完成槽数量（2 的幂；>= 队列 4 + 在途 worker 15） */

/* 与 ai/rknn_yolo.h 的解耦：画框帧快照类型在此定义（须在 g_ai 之前） */
typedef struct {
    unsigned char    *data;        /* 画框 JPEG（推流客户端按需拷贝） */
    size_t            len;
    size_t            cap;         /* data 分配容量（退役进 spare 复用时记录） */
    unsigned char    *nv12;        /* 渲染时同步转换的 NV12（录像编码链路零拷贝取用） */
    size_t            nv12_len;
    unsigned long long seq;
    int               w, h;
} yolo_frame_t;

/* 推理任务：解码后的 RGB 帧（由消费它的 worker 释放） */
typedef struct {
    unsigned char    *rgb;
    int               w, h;
    unsigned long long seq;
} yolo_job_t;

/* 完成槽：worker 提交推理结果与帧所有权（rgb 由 composer 消费释放），
   按 seq & (AI_SLOT_N-1) 索引，供 composer 按 seq 顺序重排 */
typedef struct {
    int               valid;
    unsigned long long seq;
    yolo_result_t     res;
    unsigned char    *rgb;
    int               w, h;
} yolo_slot_t;

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
    pthread_t       w_tid[AI_MAX_WORKERS];
    pthread_t       render_tid;
    int             n_created;               /* 实际创建成功的 worker 线程数（停池据此 join） */

    /* 完成槽（worker 提交 → composer 按序消费） */
    yolo_slot_t     slots[AI_SLOT_N];
    pthread_mutex_t slot_mutex;
    pthread_cond_t  slot_cond;

    long long       next_seq;                /* 渲染游标：下一个待渲染 seq（-1 = 未初始化） */

    /* EMA 平滑状态（仅 composer 线程访问） */
    yolo_result_t   prev;

    /* 类别名表（ai_names 文件，缺失回退内置 COCO 80） */
    yolo_classes_t  classes;
    char            names_path[256];

    unsigned long long last_seq;             /* 采集去重 */

    /* 快照：composer 按序写入，仅当新 seq 大于当前时更新 */
    pthread_mutex_t result_mutex;
    yolo_result_t   result;
    pthread_mutex_t frame_mutex;
    yolo_frame_t   *frame;

    /* NV12 缓冲复用池（仅 composer 线程访问）：替换快照时回收未被录像线程
       取走的旧 NV12 缓冲，下一帧转换直接复用，消除每帧 w*h*3/2 malloc/free
       （1080p 30fps ≈ 90MB/s 堆churn）。被录像线程取走所有权的缓冲由其释放，
       不进池，无跨线程复用风险 */
    unsigned char  *nv12_spare;
    size_t          nv12_spare_len;

    /* JPEG 编码缓冲复用池（仅 composer 线程访问）：替换快照时把旧 JPEG 缓冲
       退役进 spare，下一帧编码直接复用（yolo_rgb_to_jpeg_reuse realloc 增长），
       消除每帧 ~100-200KB malloc/free（该尺寸常走 mmap，30fps 下开销明显）。
       与 nv12_spare 同模式：缓冲从未跨线程共享，无并发风险 */
    unsigned char  *jpeg_spare;
    size_t          jpeg_spare_cap;
} g_ai;

static pthread_mutex_t g_reload_mutex = PTHREAD_MUTEX_INITIALIZER;

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
    yolo_worker_t *wk = &g_ai.workers[idx];

    while (g_ai.running && g_ai.pool_running) {
        yolo_job_t job;
        if (yolo_queue_pop(&job) < 0) break;      /* 停池/退出且队列空 */
        if (!g_ai.running || !g_ai.pool_running) { free(job.rgb); break; }

        yolo_rgb_resize_fast(job.rgb, job.w, job.h, wk->in_buf, g_ai.in_w, g_ai.in_h);

        rknn_input in;
        memset(&in, 0, sizeof(in));
        in.index = 0;
        in.type  = RKNN_TENSOR_UINT8;
        in.fmt   = RKNN_TENSOR_NHWC;
        in.size  = (uint32_t)(g_ai.in_w * g_ai.in_h * 3);
        in.buf   = wk->in_buf;
        if (rknn_inputs_set(wk->ctx, 1, &in) != RKNN_SUCC) { free(job.rgb); continue; }
        if (rknn_run(wk->ctx, NULL) != RKNN_SUCC) { free(job.rgb); continue; }

        rknn_output output;
        memset(&output, 0, sizeof(output));
        output.want_float = 1;
        if (rknn_outputs_get(wk->ctx, 1, &output, NULL) != RKNN_SUCC) { free(job.rgb); continue; }

        const float *out_buf[1] = { (const float *)output.buf };
        yolo_result_t res;
        memset(&res, 0, sizeof(res));
        res.seq = job.seq;
        res.w   = job.w;
        res.h   = job.h;
        res.count = yolo_postprocess(out_buf, &wk->out_attr, 1,
                                     g_ai.in_w, g_ai.in_h, job.w, job.h,
                                     res.dets, YOLO_MAX_DETS, g_ai.conf, g_ai.nms, NULL);
        rknn_outputs_release(wk->ctx, 1, &output);

        /* 提交到完成槽：rgb 所有权移交 composer；槽满（同余旧帧未消费）则等待 */
        int s = (int)(job.seq & (AI_SLOT_N - 1));
        pthread_mutex_lock(&g_ai.slot_mutex);
        while (g_ai.slots[s].valid && g_ai.running && g_ai.pool_running)
            pthread_cond_wait(&g_ai.slot_cond, &g_ai.slot_mutex);
        if (!g_ai.running || !g_ai.pool_running) {
            pthread_mutex_unlock(&g_ai.slot_mutex);
            free(job.rgb);
            break;
        }
        g_ai.slots[s].valid = 1;
        g_ai.slots[s].seq   = job.seq;
        g_ai.slots[s].res   = res;
        g_ai.slots[s].rgb   = job.rgb;
        g_ai.slots[s].w     = job.w;
        g_ai.slots[s].h     = job.h;
        pthread_cond_signal(&g_ai.slot_cond);
        pthread_mutex_unlock(&g_ai.slot_mutex);
    }
    return NULL;
}

/* ---- 渲染 composer：最新结果优先（seq 单调不回退）+ ~30fps 渲染节拍 ----

   严格按 seq 顺序消费会因 worker 乱序完成而等待缺口（实测卡顿 100~700ms）。
   改为消费最新完成结果：渲染 seq 严格递增（绝不回退，画面不抖动），
   被跳过的旧结果直接释放；EMA 平滑本身即可容忍帧间小跳跃。
   渲染节拍与推流/显示速率匹配（~30fps），同时把 OpenCV 渲染 + JPEG 编码 +
   NV12 转换的 CPU 锁定在上限内。 */

static void *yolo_render_task(void *arg)
{
    (void)arg;
    struct timespec last_render = { 0, 0 };
    while (g_ai.running && g_ai.pool_running) {
        yolo_result_t res;
        unsigned char *rgb = NULL;
        int w = 0, h = 0;

        pthread_mutex_lock(&g_ai.slot_mutex);
        for (;;) {
            /* 选槽：有效槽中取 seq > next_seq 的最新者（MAX seq）；
               游标未初始化时取最小 seq 起步 */
            int pick = -1;
            unsigned long long sel = 0;
            int found = 0;
            for (int s = 0; s < AI_SLOT_N; s++) {
                if (!g_ai.slots[s].valid) continue;
                if (g_ai.next_seq < 0) {
                    if (!found || g_ai.slots[s].seq < sel) { sel = g_ai.slots[s].seq; pick = s; found = 1; }
                } else if ((long long)g_ai.slots[s].seq > g_ai.next_seq) {
                    if (!found || g_ai.slots[s].seq > sel) { sel = g_ai.slots[s].seq; pick = s; found = 1; }
                }
            }
            if (found) {
                yolo_slot_t *sl = &g_ai.slots[pick];
                res = sl->res;
                rgb = sl->rgb; sl->rgb = NULL;
                w = sl->w; h = sl->h;
                g_ai.next_seq = (long long)sl->seq;
                /* 释放所有 ≤ 已选 seq 的旧槽（不会被渲染），解除对应 worker 的槽等待 */
                for (int s = 0; s < AI_SLOT_N; s++) {
                    if (g_ai.slots[s].valid && (long long)g_ai.slots[s].seq <= g_ai.next_seq) {
                        free(g_ai.slots[s].rgb);
                        g_ai.slots[s].rgb = NULL;
                        g_ai.slots[s].valid = 0;
                    }
                }
                sl->valid = 0;
                pthread_cond_broadcast(&g_ai.slot_cond);
                break;
            }
            /* 无更新结果：等待 40ms；超时且所有有效槽都落后于游标 → 视为
               seq 回绕（相机重启），重置游标重新起步 */
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 40 * 1000000;
            if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
            int rc = 0;
            while (g_ai.running && g_ai.pool_running && rc == 0)
                rc = pthread_cond_timedwait(&g_ai.slot_cond, &g_ai.slot_mutex, &ts);
            if (!g_ai.running || !g_ai.pool_running) {
                pthread_mutex_unlock(&g_ai.slot_mutex);
                goto out;
            }
            if (rc == ETIMEDOUT && g_ai.next_seq >= 0) {
                unsigned long long mx = 0;
                int any = 0;
                for (int s = 0; s < AI_SLOT_N; s++) {
                    if (g_ai.slots[s].valid &&
                        (!any || g_ai.slots[s].seq > mx)) { mx = g_ai.slots[s].seq; any = 1; }
                }
                if (any && (unsigned long long)g_ai.next_seq - mx > 1000)
                    g_ai.next_seq = -1;
            }
        }
        pthread_mutex_unlock(&g_ai.slot_mutex);

        /* 渲染：EMA 平滑（prev 仅本线程访问）+ OpenCV 画框标签 → JPEG + NV12 双快照 */
        yolo_result_t disp = res;
        yolo_smooth(&g_ai.prev, &disp);

        /* JPEG 编码缓冲：优先复用 spare（上一帧替换时退役的旧快照缓冲，
           编码器 realloc 增长） */
        unsigned char *jpeg = g_ai.jpeg_spare;
        size_t jcap = g_ai.jpeg_spare_cap;
        g_ai.jpeg_spare = NULL;
        g_ai.jpeg_spare_cap = 0;
        size_t jlen = 0;
        if (yolo_render_annotated(rgb, w, h, &disp, &g_ai.classes,
                                  &jpeg, &jcap, &jlen) == 0) {
            /* NV12 目标缓冲：优先复用 spare（上一帧替换时回收） */
            size_t nv12_len = (size_t)w * h * 3 / 2;
            unsigned char *nv12 = NULL;
            if (g_ai.nv12_spare && g_ai.nv12_spare_len >= nv12_len) {
                nv12 = g_ai.nv12_spare;
                g_ai.nv12_spare = NULL;
            } else {
                free(g_ai.nv12_spare);
                g_ai.nv12_spare = NULL;
                nv12 = malloc(nv12_len);
            }
            if (nv12) yolo_rgb_to_nv12_fast(rgb, w, h, nv12);
            g_ai.prev = disp;
            snap_result(&disp);
            pthread_mutex_lock(&g_ai.frame_mutex);
            if (!g_ai.frame) g_ai.frame = calloc(1, sizeof(*g_ai.frame));
            if (g_ai.frame) {
                /* 旧 JPEG 所有权一直在快照（客户端只拷贝）：退役进 spare 供
                   下一帧编码复用（spare 空槽 = 本帧编码前刚取走）；旧 NV12
                   若未被录像线程取走则回收进 spare，结构体本身原地复用 */
                free(g_ai.jpeg_spare);
                g_ai.jpeg_spare = g_ai.frame->data;
                g_ai.jpeg_spare_cap = g_ai.frame->cap;
                if (g_ai.frame->nv12) {
                    free(g_ai.nv12_spare);
                    g_ai.nv12_spare = g_ai.frame->nv12;
                    g_ai.nv12_spare_len = g_ai.frame->nv12_len;
                }
                g_ai.frame->data = jpeg; jpeg = NULL;   /* 所有权移交快照 */
                g_ai.frame->cap  = jcap;
                g_ai.frame->len  = jlen;
                g_ai.frame->nv12 = nv12; nv12 = NULL;
                g_ai.frame->nv12_len = nv12_len;
                g_ai.frame->seq  = disp.seq;
                g_ai.frame->w    = w;
                g_ai.frame->h    = h;
            }
            pthread_mutex_unlock(&g_ai.frame_mutex);
            free(nv12);
        }
        free(jpeg);   /* 编码失败/快照 calloc 失败时释放本次取走的缓冲 */
        free(rgb);

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
out:
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

/* 停止推理池：置 pool_running=0 → 唤醒各线程 → join → 清理队列/槽/快照/模型。
   可安全重入（池未启动时 n_created=0，各清理为幂等操作） */
static void ai_stop_pool(void)
{
    g_ai.pool_running = 0;
    /* 唤醒可能阻塞在队列/槽上的采集线程、工作线程与渲染线程 */
    pthread_cond_broadcast(&g_ai.q_not_empty);
    pthread_cond_broadcast(&g_ai.q_not_full);
    pthread_cond_broadcast(&g_ai.slot_cond);

    for (int i = 0; i < g_ai.n_created; i++)
        pthread_join(g_ai.w_tid[i], NULL);
    g_ai.n_created = 0;
    if (g_ai.render_tid) {
        pthread_join(g_ai.render_tid, NULL);
        g_ai.render_tid = 0;
    }

    /* 释放队列中未被消费的任务（已消费槽位 rgb 已置 NULL） */
    for (int i = 0; i < AI_QUEUE_CAP; i++) {
        if (g_ai.jobs[i].rgb) { free(g_ai.jobs[i].rgb); g_ai.jobs[i].rgb = NULL; }
    }
    g_ai.q_head = g_ai.q_tail = g_ai.q_count = 0;

    /* 释放未被 composer 消费的槽内存 */
    for (int s = 0; s < AI_SLOT_N; s++) {
        if (g_ai.slots[s].rgb) { free(g_ai.slots[s].rgb); g_ai.slots[s].rgb = NULL; }
        g_ai.slots[s].valid = 0;
    }

    /* 清空快照与渲染状态（新池从干净状态开始） */
    pthread_mutex_lock(&g_ai.frame_mutex);
    if (g_ai.frame) {
        free(g_ai.frame->data);
        free(g_ai.frame->nv12);   /* 录像线程可能已取走（置 NULL），双重释放安全 */
        free(g_ai.frame);
        g_ai.frame = NULL;
    }
    pthread_mutex_unlock(&g_ai.frame_mutex);
    free(g_ai.nv12_spare);        /* 渲染线程已 join，spare 无并发访问 */
    g_ai.nv12_spare = NULL;
    g_ai.nv12_spare_len = 0;
    free(g_ai.jpeg_spare);
    g_ai.jpeg_spare = NULL;
    g_ai.jpeg_spare_cap = 0;
    pthread_mutex_lock(&g_ai.result_mutex);
    memset(&g_ai.result, 0, sizeof(g_ai.result));
    pthread_mutex_unlock(&g_ai.result_mutex);
    memset(&g_ai.prev, 0, sizeof(g_ai.prev));
    g_ai.next_seq = -1;
    g_ai.last_seq = 0;

    ai_free_workers();
    g_ai.enabled = 0;
}

/* 启动推理池：重读配置 → 加载模型 → 建 worker → 启动线程。
   失败/禁用时 enabled=0（画框流自动回退原始帧），不阻断调用方 */
static void ai_start_pool(void)
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

    if (!cfg->ai_enable) {
        g_ai.enabled = 0;
        LOG_INFO("ai: disabled by config");
        return;
    }
    safe_strncpy(g_ai.names_path, sizeof(g_ai.names_path),
                 cfg->ai_names[0] ? cfg->ai_names : "config/coco.names");

    char model_path[256];
    safe_strncpy(model_path, sizeof(model_path),
                 cfg->ai_model[0] ? cfg->ai_model : "config/yolo26.rknn");
    if (load_model_file(model_path, &g_ai.model, &g_ai.model_len) < 0) {
        LOG_ERROR("ai: model '%s' not found, AI disabled (stream continues raw)", model_path);
        g_ai.enabled = 0;
        return;
    }

    /* 用第一个上下文获取输入尺寸（静态模型）与输出数校验 */
    rknn_context probe = 0;
    if (rknn_init(&probe, g_ai.model, (uint32_t)g_ai.model_len, 0, NULL) != RKNN_SUCC) {
        LOG_ERROR("ai: rknn_init failed: model incompatible or NPU unavailable, AI disabled");
        ai_free_workers();
        g_ai.enabled = 0;
        return;
    }
    rknn_input_output_num io_num;
    memset(&io_num, 0, sizeof(io_num));
    if (rknn_query(probe, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num)) != RKNN_SUCC ||
        io_num.n_input < 1 || io_num.n_output != 1) {
        LOG_ERROR("ai: outputs=%u != 1, 需官方 rknn 单输出模型 (yolo export format=rknn), AI disabled",
                  io_num.n_output);
        rknn_destroy(probe);
        ai_free_workers();
        g_ai.enabled = 0;
        return;
    }
    rknn_tensor_attr in_attr;
    memset(&in_attr, 0, sizeof(in_attr));
    in_attr.index = 0;
    if (rknn_query(probe, RKNN_QUERY_INPUT_ATTR, &in_attr, sizeof(in_attr)) != RKNN_SUCC) {
        LOG_ERROR("ai: rknn_query INPUT_ATTR failed");
        rknn_destroy(probe);
        ai_free_workers();
        g_ai.enabled = 0;
        return;
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
            LOG_ERROR("ai: pool start failed, AI disabled (stream continues raw)");
            return;
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
    LOG_INFO("ai: model '%s' loaded, classes '%s' (%d), input %dx%d, "
             "single output, threads %d (NPU core i%%3), conf %.2f nms %.2f",
             model_path, g_ai.names_path, ncls, g_ai.in_w, g_ai.in_h,
             g_ai.nthreads, g_ai.conf, g_ai.nms);
}

/* 热重载：停止旧池 → 重读配置重建。由前端上传模型/标签文件、修改 AI 参数触发 */
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
    g_ai.next_seq = -1;
    pthread_mutex_init(&g_ai.result_mutex, NULL);
    pthread_mutex_init(&g_ai.frame_mutex, NULL);
    pthread_mutex_init(&g_ai.q_lock, NULL);
    pthread_mutex_init(&g_ai.slot_mutex, NULL);
    pthread_cond_init(&g_ai.q_not_empty, NULL);
    pthread_cond_init(&g_ai.q_not_full, NULL);
    pthread_cond_init(&g_ai.slot_cond, NULL);

    if (!app || !app->cfg) {
        LOG_ERROR("ai: init without config");
        return 0;
    }
    ai_start_pool();   /* ai_enable=0 或失败时 enabled=0，ai_task 空转喂狗 */
    return 0;
}

void rknn_yolo_destroy(void *arg)
{
    (void)arg;
    g_ai.running = 0;
    ai_stop_pool();

    pthread_mutex_destroy(&g_ai.q_lock);
    pthread_mutex_destroy(&g_ai.slot_mutex);
    pthread_cond_destroy(&g_ai.q_not_empty);
    pthread_cond_destroy(&g_ai.q_not_full);
    pthread_cond_destroy(&g_ai.slot_cond);
    pthread_mutex_destroy(&g_ai.result_mutex);
    pthread_mutex_destroy(&g_ai.frame_mutex);
    g_ai.enabled = 0;
}

int rknn_yolo_enabled(void)
{
    return g_ai.enabled;
}

/* ---- 快照访问（供推流客户端）---- */

int rknn_yolo_get(yolo_result_t *out)
{
    if (!out || !g_ai.enabled) return 0;
    pthread_mutex_lock(&g_ai.result_mutex);
    *out = g_ai.result;
    pthread_mutex_unlock(&g_ai.result_mutex);
    return out->count;
}

int rknn_yolo_get_frame(unsigned char **data, size_t *len, unsigned long long *seq)
{
    if (!g_ai.enabled || !data || !len || !seq) return -1;
    pthread_mutex_lock(&g_ai.frame_mutex);
    if (!g_ai.frame || g_ai.frame->len == 0) {
        pthread_mutex_unlock(&g_ai.frame_mutex);
        return -1;
    }
    unsigned char *c = malloc(g_ai.frame->len);
    if (!c) {
        pthread_mutex_unlock(&g_ai.frame_mutex);
        return -1;
    }
    memcpy(c, g_ai.frame->data, g_ai.frame->len);
    *len = g_ai.frame->len;
    *seq = g_ai.frame->seq;
    pthread_mutex_unlock(&g_ai.frame_mutex);
    *data = c;
    return 0;
}

unsigned long long rknn_yolo_get_frame_seq(void)
{
    if (!g_ai.enabled) return 0;
    pthread_mutex_lock(&g_ai.frame_mutex);
    unsigned long long s = g_ai.frame ? g_ai.frame->seq : 0;
    pthread_mutex_unlock(&g_ai.frame_mutex);
    return s;
}

int rknn_yolo_get_frame_nv12(unsigned char **data, size_t *len,
                             unsigned long long *seq, int *w, int *h)
{
    if (!g_ai.enabled || !data || !len || !seq) return -1;
    pthread_mutex_lock(&g_ai.frame_mutex);
    if (!g_ai.frame || !g_ai.frame->nv12 || g_ai.frame->nv12_len == 0) {
        pthread_mutex_unlock(&g_ai.frame_mutex);
        return -1;
    }
    /* 所有权移交（单消费者：录像编码线程），免去每帧 3MB 拷贝 */
    *data = g_ai.frame->nv12;
    g_ai.frame->nv12 = NULL;
    *len = g_ai.frame->nv12_len;
    *seq = g_ai.frame->seq;
    if (w) *w = g_ai.frame->w;
    if (h) *h = g_ai.frame->h;
    pthread_mutex_unlock(&g_ai.frame_mutex);
    return 0;
}

/* ---- AI 采集线程（main 模块框架 task）：取帧 → 解码 → 投递队列 ---- */
void *rknn_ai_task(void *arg)
{
    app_ctx_t *app = (app_ctx_t *)arg;
    if (!app) return NULL;

    while (app->running && g_ai.running) {
        watchdog_feed_self("ai");

        /* 未启用 / 池未运行（重载中）：空转喂狗，不占推理资源。
           重载后 enabled 恢复时自动继续采集 */
        if (!g_ai.enabled || !g_ai.pool_running) { usleep(200000); continue; }

        /* 先无拷贝窥探帧序号：10ms 轮询大多时刻无新帧（30fps 相机约 70% 轮询
           落空），直接跳过整帧 malloc+memcpy（720p MJPEG ~100-200KB/次） */
        unsigned long long seq = video_stream_get_frame_seq();
        if (seq == 0 || seq == g_ai.last_seq) {
            usleep(g_ai.interval_ms * 1000);
            continue;
        }

        unsigned char *raw = NULL;
        size_t raw_len = 0;
        int fmt = 0, w = 0, h = 0;
        if (video_stream_get_frame(&raw, &raw_len, &fmt, &w, &h, &seq) != 0) {
            usleep(g_ai.interval_ms * 1000);
            continue;
        }
        /* 窥探与拷贝间隙恰有新帧发布时，返回帧可能新于返回的 seq：
           拷贝后仍按 seq 去重，防止同一帧被重复解码 */
        if (seq == g_ai.last_seq || raw_len == 0 || w <= 0 || h <= 0) {
            free(raw);
            usleep(g_ai.interval_ms * 1000);
            continue;
        }
        g_ai.last_seq = seq;

        unsigned char *rgb = NULL;
        int rw = 0, rh = 0;
        if (fmt == RKNN_FMT_MJPEG) {
            if (yolo_jpeg_to_rgb(raw, raw_len, &rgb, &rw, &rh) != 0) {
                free(raw);
                usleep(g_ai.interval_ms * 1000);   /* 坏帧退避，避免忙转 */
                continue;
            }
        } else {   /* YUYV */
            rgb = malloc((size_t)w * h * 3);
            if (rgb) { yolo_yuyv_to_rgb(raw, w, h, rgb); rw = w; rh = h; }
        }
        free(raw);
        if (!rgb) {
            usleep(g_ai.interval_ms * 1000);
            continue;
        }

        yolo_job_t job = { .rgb = rgb, .w = rw, .h = rh, .seq = seq };
        if (yolo_queue_push(&job) != 0) free(rgb);   /* 停池/退出中：释放 */

        usleep(g_ai.interval_ms * 1000);
    }
    return NULL;
}
