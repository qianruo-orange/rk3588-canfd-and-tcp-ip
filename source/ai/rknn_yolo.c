/**
 * rknn_yolo.c — RKNN + YOLO26 检测主模块：模型管理 + 多线程推理池。
 *
 * 数据流（rknn_ai_task 采集线程 + N 个推理工作线程）：
 *   ai_task:   video_stream_get_frame → 解码/转换 RGB24 → 投递任务队列
 *   worker i:  出队 → 双线性缩放 → NPU 推理（独立 rknn context）→
 *              YOLO26 三头后处理（yolo_postprocess）→ 画框 JPEG（yolo_draw）→
 *              按 seq 单调更新结果/画框帧快照（供 /video/mjpeg_ai 推流）
 *
 * 优雅降级：无模型 / NPU 驱动未加载 / 推理失败 → enabled=0，原视频流照常，
 * 画框流客户端回退到原始帧。所有失败路径只记日志不崩溃。
 *
 * 图像处理与后处理已拆分为独立模块（yolo_image / yolo_postprocess / yolo_draw），
 * 本文件仅保留：模型生命周期、推理线程池、任务队列、快照与对外 API。
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <unistd.h>

#include "rknn/rknn_api.h"

#include "ai/rknn_yolo.h"
#include "ai/yolo_image.h"
#include "ai/yolo_postprocess.h"
#include "ai/yolo_draw.h"
#include "core/common.h"
#include "core/log.h"
#include "core/config.h"
#include "watchdog/watchdog.h"
#include "video/video_stream.h"

#define AI_MAX_WORKERS 4     /* 推理工作线程上限（对应 ai_threads 1~4） */
#define AI_QUEUE_CAP   2     /* 任务队列容量（帧），与 worker 数共同约束在途帧内存 */

/* 与 ai/rknn_yolo.h 的解耦：画框帧快照类型在此定义（须在 g_ai 之前） */
typedef struct {
    unsigned char    *data;
    size_t            len;
    unsigned long long seq;
    int               w, h;
} yolo_frame_t;

/* 推理任务：解码后的 RGB 帧（由消费它的 worker 释放） */
typedef struct {
    unsigned char    *rgb;
    int               w, h;
    unsigned long long seq;
} yolo_job_t;

/* 单个推理工作线程：独立 rknn context，可与其它线程并行 run */
typedef struct {
    rknn_context    ctx;
    unsigned char  *in_buf;                 /* 缩放后输入缓冲 */
    rknn_tensor_attr out_attr[3];
} yolo_worker_t;

/* 模块全局上下文 */
static struct {
    int            enabled;
    int            running;
    char           model_path[256];
    app_ctx_t     *app;
    float          conf, nms;
    int            interval_ms;
    int            nthreads;                /* 推理线程数（ai_threads） */

    unsigned char *model;                   /* 模型文件内存（共享给所有 worker init） */
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

    unsigned long long last_seq;            /* 采集去重 */

    /* 快照：仅当新 seq 大于当前时更新（worker 乱序完成场景） */
    pthread_mutex_t result_mutex;
    yolo_result_t   result;
    pthread_mutex_t frame_mutex;
    yolo_frame_t   *frame;
} g_ai;

/* ---- 任务队列 ---- */

static int yolo_queue_push(const yolo_job_t *job)
{
    pthread_mutex_lock(&g_ai.q_lock);
    while (g_ai.q_count >= AI_QUEUE_CAP && g_ai.running)
        pthread_cond_wait(&g_ai.q_not_full, &g_ai.q_lock);
    if (!g_ai.running) {
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
    while (g_ai.q_count == 0 && g_ai.running)
        pthread_cond_wait(&g_ai.q_not_empty, &g_ai.q_lock);
    if (g_ai.q_count == 0) {
        pthread_mutex_unlock(&g_ai.q_lock);
        return -1;
    }
    *job = g_ai.jobs[g_ai.q_head];
    g_ai.q_head = (g_ai.q_head + 1) % AI_QUEUE_CAP;
    g_ai.q_count--;
    pthread_cond_signal(&g_ai.q_not_full);
    pthread_mutex_unlock(&g_ai.q_lock);
    return 0;
}

/* ---- 快照（seq 单调更新）---- */

static void snap_result(const yolo_result_t *res)
{
    pthread_mutex_lock(&g_ai.result_mutex);
    if (res->seq > g_ai.result.seq) g_ai.result = *res;
    pthread_mutex_unlock(&g_ai.result_mutex);
}

static void snap_frame(const unsigned char *jpeg, size_t jlen,
                       unsigned long long seq, int w, int h)
{
    pthread_mutex_lock(&g_ai.frame_mutex);
    if (g_ai.frame && seq <= g_ai.frame->seq) {   /* 乱序完成的旧帧：丢弃 */
        pthread_mutex_unlock(&g_ai.frame_mutex);
        return;
    }
    if (g_ai.frame) { free(g_ai.frame->data); free(g_ai.frame); }
    g_ai.frame = malloc(sizeof(*g_ai.frame));
    if (g_ai.frame) {
        g_ai.frame->data = malloc(jlen);
        if (g_ai.frame->data) {
            memcpy(g_ai.frame->data, jpeg, jlen);
            g_ai.frame->len  = jlen;
            g_ai.frame->seq  = seq;
            g_ai.frame->w    = w;
            g_ai.frame->h    = h;
        } else {
            free(g_ai.frame);
            g_ai.frame = NULL;
        }
    }
    pthread_mutex_unlock(&g_ai.frame_mutex);
}

/* ---- 推理工作线程 ---- */

static void *yolo_worker_task(void *arg)
{
    int idx = (int)(intptr_t)arg;
    yolo_worker_t *wk = &g_ai.workers[idx];

    while (g_ai.running) {
        yolo_job_t job;
        if (yolo_queue_pop(&job) < 0) break;      /* running=0 且队列空 */
        if (!g_ai.running) { free(job.rgb); break; }

        yolo_rgb_resize(job.rgb, job.w, job.h, wk->in_buf, g_ai.in_w, g_ai.in_h);

        rknn_input in;
        memset(&in, 0, sizeof(in));
        in.index = 0;
        in.type  = RKNN_TENSOR_UINT8;
        in.fmt   = RKNN_TENSOR_NHWC;
        in.size  = (uint32_t)(g_ai.in_w * g_ai.in_h * 3);
        in.buf   = wk->in_buf;
        if (rknn_inputs_set(wk->ctx, 1, &in) != RKNN_SUCC) { free(job.rgb); continue; }
        if (rknn_run(wk->ctx, NULL) != RKNN_SUCC) { free(job.rgb); continue; }

        rknn_output outputs[3];
        memset(outputs, 0, sizeof(outputs));
        for (int i = 0; i < 3; i++) outputs[i].want_float = 1;
        if (rknn_outputs_get(wk->ctx, 3, outputs, NULL) != RKNN_SUCC) { free(job.rgb); continue; }

        const float *out_buf[3] = { (const float *)outputs[0].buf,
                                    (const float *)outputs[1].buf,
                                    (const float *)outputs[2].buf };
        yolo_result_t res;
        memset(&res, 0, sizeof(res));
        res.seq = job.seq;
        res.w   = job.w;
        res.h   = job.h;
        res.count = yolo_postprocess(out_buf, wk->out_attr, 3,
                                     g_ai.in_w, g_ai.in_h, job.w, job.h,
                                     res.dets, YOLO_MAX_DETS, g_ai.conf, g_ai.nms, NULL);
        rknn_outputs_release(wk->ctx, 3, outputs);

        /* 结果快照（仅新 seq 覆盖） */
        snap_result(&res);

        /* 画框帧快照（仅新 seq 才做 JPEG 编码，避免乱序覆盖与无效开销） */
        unsigned long long fseq = 0;
        pthread_mutex_lock(&g_ai.frame_mutex);
        fseq = g_ai.frame ? g_ai.frame->seq : 0;
        pthread_mutex_unlock(&g_ai.frame_mutex);
        if (job.seq > fseq) {
            unsigned char *jpeg = NULL;
            size_t jlen = 0;
            if (yolo_render_annotated(job.rgb, job.w, job.h, &res, &jpeg, &jlen) == 0) {
                snap_frame(jpeg, jlen, job.seq, job.w, job.h);
                free(jpeg);
            }
        }
        free(job.rgb);
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

/* 创建一个推理上下文：rknn_init + 查输出属性 + 分配输入缓冲 */
static int yolo_worker_setup(yolo_worker_t *wk)
{
    wk->ctx = 0;
    wk->in_buf = NULL;
    memset(wk->out_attr, 0, sizeof(wk->out_attr));

    int ret = rknn_init(&wk->ctx, g_ai.model, (uint32_t)g_ai.model_len, 0, NULL);
    if (ret != RKNN_SUCC) {
        LOG_ERROR("ai: worker rknn_init failed (%d)", ret);
        wk->ctx = 0;
        return -1;
    }
    for (int i = 0; i < 3; i++) {
        wk->out_attr[i].index = (uint32_t)i;
        if (rknn_query(wk->ctx, RKNN_QUERY_OUTPUT_ATTR, &wk->out_attr[i],
                       sizeof(wk->out_attr[i])) != RKNN_SUCC) {
            LOG_ERROR("ai: worker rknn_query OUTPUT_ATTR[%d] failed", i);
            return -1;
        }
    }
    wk->in_buf = malloc((size_t)g_ai.in_w * g_ai.in_h * 3);
    if (!wk->in_buf) {
        LOG_ERROR("ai: worker input buffer alloc failed");
        return -1;
    }
    return 0;
}

int rknn_yolo_init(void *arg)
{
    memset(&g_ai, 0, sizeof(g_ai));
    app_ctx_t *app = (app_ctx_t *)arg;
    g_ai.app = app;
    g_ai.conf = 0.25f;
    g_ai.nms = 0.45f;
    g_ai.interval_ms = 200;
    g_ai.nthreads = 2;
    g_ai.running = 1;
    pthread_mutex_init(&g_ai.result_mutex, NULL);
    pthread_mutex_init(&g_ai.frame_mutex, NULL);
    pthread_mutex_init(&g_ai.q_lock, NULL);
    pthread_cond_init(&g_ai.q_not_empty, NULL);
    pthread_cond_init(&g_ai.q_not_full, NULL);

    if (!app || !app->cfg || !app->cfg->ai_enable) {
        LOG_INFO("ai: disabled by config");
        return 0;   /* enabled=0：task 休眠空转 */
    }
    safe_strncpy(g_ai.model_path, sizeof(g_ai.model_path),
                 app->cfg->ai_model[0] ? app->cfg->ai_model : "config/yolo26.rknn");
    g_ai.conf = app->cfg->ai_conf > 0 ? app->cfg->ai_conf : 0.25f;
    g_ai.nms  = app->cfg->ai_nms  > 0 ? app->cfg->ai_nms  : 0.45f;
    g_ai.interval_ms = app->cfg->ai_interval_ms > 0 ? app->cfg->ai_interval_ms : 200;
    g_ai.nthreads = app->cfg->ai_threads > 0 ? app->cfg->ai_threads : 2;
    if (g_ai.nthreads > AI_MAX_WORKERS) g_ai.nthreads = AI_MAX_WORKERS;

    if (load_model_file(g_ai.model_path, &g_ai.model, &g_ai.model_len) < 0) {
        LOG_ERROR("ai: model '%s' not found, AI disabled (stream continues raw)",
                  g_ai.model_path);
        return 0;
    }

    /* 用第一个上下文获取输入尺寸（静态模型）与输出数校验 */
    rknn_context probe = 0;
    if (rknn_init(&probe, g_ai.model, (uint32_t)g_ai.model_len, 0, NULL) != RKNN_SUCC) {
        LOG_ERROR("ai: rknn_init failed: model incompatible or NPU unavailable, AI disabled");
        return 0;
    }
    rknn_input_output_num io_num;
    memset(&io_num, 0, sizeof(io_num));
    if (rknn_query(probe, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num)) != RKNN_SUCC ||
        io_num.n_input < 1 || io_num.n_output != 3) {
        LOG_ERROR("ai: outputs=%u != 3, 需要 YOLO26 三输出模式（P3/P4/P5 检测头）模型, AI disabled",
                  io_num.n_output);
        rknn_destroy(probe);
        return 0;
    }
    rknn_tensor_attr in_attr;
    memset(&in_attr, 0, sizeof(in_attr));
    in_attr.index = 0;
    if (rknn_query(probe, RKNN_QUERY_INPUT_ATTR, &in_attr, sizeof(in_attr)) != RKNN_SUCC) {
        LOG_ERROR("ai: rknn_query INPUT_ATTR failed");
        rknn_destroy(probe);
        return 0;
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
        int s = app->cfg->ai_input_size > 0 ? app->cfg->ai_input_size : 640;
        g_ai.in_w = s;
        g_ai.in_h = s;
    }

    /* 每个工作线程独立上下文 */
    for (int i = 0; i < g_ai.nthreads; i++) {
        if (yolo_worker_setup(&g_ai.workers[i]) < 0)
            goto fail_workers;
    }
    g_ai.enabled = 1;
    LOG_INFO("ai: model '%s' loaded, input %dx%d, 3 outputs, threads %d, conf %.2f nms %.2f",
             g_ai.model_path, g_ai.in_w, g_ai.in_h, g_ai.nthreads, g_ai.conf, g_ai.nms);
    return 0;

fail_workers:
    for (int i = 0; i < g_ai.nthreads; i++) {
        yolo_worker_t *wk = &g_ai.workers[i];
        if (wk->in_buf) { free(wk->in_buf); wk->in_buf = NULL; }
        if (wk->ctx) { rknn_destroy(wk->ctx); wk->ctx = 0; }
    }
    LOG_ERROR("ai: init failed, AI disabled (stream continues raw)");
    return 0;
}

void rknn_yolo_destroy(void *arg)
{
    (void)arg;
    g_ai.running = 0;
    /* 唤醒可能阻塞在队列上的采集线程与工作线程 */
    pthread_cond_broadcast(&g_ai.q_not_empty);
    pthread_cond_broadcast(&g_ai.q_not_full);

    for (int i = 0; i < g_ai.nthreads; i++)
        pthread_join(g_ai.w_tid[i], NULL);   /* 在途任务完成或退出后返回 */
    for (int i = 0; i < g_ai.nthreads; i++) {
        yolo_worker_t *wk = &g_ai.workers[i];
        if (wk->in_buf) { free(wk->in_buf); wk->in_buf = NULL; }
        if (wk->ctx) { rknn_destroy(wk->ctx); wk->ctx = 0; }
    }
    free(g_ai.model);
    g_ai.model = NULL;

    pthread_mutex_lock(&g_ai.frame_mutex);
    if (g_ai.frame) { free(g_ai.frame->data); free(g_ai.frame); g_ai.frame = NULL; }
    pthread_mutex_unlock(&g_ai.frame_mutex);

    pthread_mutex_destroy(&g_ai.q_lock);
    pthread_cond_destroy(&g_ai.q_not_empty);
    pthread_cond_destroy(&g_ai.q_not_full);
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

/* ---- AI 采集线程（main 模块框架 task）：取帧 → 解码 → 投递队列 ---- */
void *rknn_ai_task(void *arg)
{
    app_ctx_t *app = (app_ctx_t *)arg;
    if (!g_ai.enabled) {
        /* 未启用/降级：线程保持存活并持续喂狗（模块一致管理，无 watchdog 注册竞态），
           不占推理资源 */
        while (app->running && g_ai.running) {
            watchdog_feed_self("ai");
            usleep(200000);
        }
        return NULL;
    }

    /* 启动推理工作线程池 */
    for (int i = 0; i < g_ai.nthreads; i++) {
        if (pthread_create(&g_ai.w_tid[i], NULL, yolo_worker_task,
                           (void *)(intptr_t)i) != 0) {
            LOG_ERROR("ai: worker thread %d create failed", i);
            g_ai.nthreads = i;
            break;
        }
    }
    LOG_INFO("ai: inference pool started (%d threads, interval %d ms)",
             g_ai.nthreads, g_ai.interval_ms);

    while (app->running && g_ai.running) {
        watchdog_feed_self("ai");

        unsigned char *raw = NULL;
        size_t raw_len = 0;
        int fmt = 0, w = 0, h = 0;
        unsigned long long seq = 0;
        if (video_stream_get_frame(&raw, &raw_len, &fmt, &w, &h, &seq) != 0) {
            usleep(g_ai.interval_ms * 1000);
            continue;
        }
        if (seq == g_ai.last_seq || raw_len == 0 || w <= 0 || h <= 0) {
            free(raw);
            usleep(g_ai.interval_ms * 1000);
            continue;
        }
        g_ai.last_seq = seq;

        unsigned char *rgb = NULL;
        int rw = 0, rh = 0;
        if (fmt == RKNN_FMT_MJPEG) {
            if (yolo_jpeg_to_rgb(raw, raw_len, &rgb, &rw, &rh) != 0) { free(raw); continue; }
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
        if (yolo_queue_push(&job) != 0) free(rgb);   /* 退出中：释放 */

        usleep(g_ai.interval_ms * 1000);
    }
    return NULL;
}
