/**
 * video_rec.c — 网络录像模块：AI 画框帧优先（回退原始帧）+ MP4(MJPEG) 封装。
 *
 * 封装细节见 video/rec_mp4.c（ISO BMFF，moov 末尾回写，无需硬件编码器）。
 *
 * 线程模型：main 模块表 "rec" 线程，空闲时 100ms 轮询喂狗；
 *   HTTP 线程通过 video_rec_start/stop 下发命令，录制循环每轮检查
 *   recording 标志（≤1 帧间隔退出，帧快照轮询 20ms）。
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "video/video_rec.h"
#include "video/rec_mp4.h"
#include "video/video_stream.h"
#include "ai/rknn_yolo.h"
#include "ai/yolo_image.h"
#include "core/common.h"
#include "core/log.h"
#include "watchdog/watchdog.h"

#define REC_DIR        PATH_RECORDINGS

/* 命令（HTTP 线程 → 录制线程） */
#define REC_CMD_NONE   0
#define REC_CMD_START  1
#define REC_CMD_STOP   2

/* 模块全局上下文 */
static struct {
    pthread_mutex_t lock;
    int  cmd;                     /* REC_CMD_*（HTTP 线程下发） */
    int  recording;               /* 录制中标志（线程写，start 前读） */
    int  start_fail;              /* 最近一次 start 失败原因（0=无） */
    char file[128];               /* 当前录制文件名 */
    uint64_t start_ms, frames, bytes;
    uint64_t last_frame_ts;       /* 最近一帧单调时钟，用于实时 fps */
} g_rec;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

/* ---- 帧获取：AI 画框帧优先，回退原始帧（返回 malloc 的 JPEG，调用方 free） ---- */

static unsigned long long g_ai_seq = 0;   /* 已消费画框帧序号 */
static unsigned long long g_raw_seq = 0;  /* 已消费原始帧序号 */

/* 尝试取 AI 画框帧（JPEG）；无新帧返回 -1 */
static int rec_take_ai_frame(unsigned char **out, size_t *len)
{
    if (!rknn_yolo_enabled()) return -1;
    unsigned long long seq = rknn_yolo_get_frame_seq();
    if (seq == 0 || seq == g_ai_seq) return -1;
    unsigned long long aseq = 0;
    if (rknn_yolo_get_frame(out, len, &aseq) != 0) return -1;
    g_ai_seq = aseq;
    return 0;
}

/* 尝试取原始帧并转 JPEG；无新帧返回 -1 */
static int rec_take_raw_frame(unsigned char **out, size_t *len, int *w, int *h)
{
    unsigned char *raw = NULL;
    size_t raw_len = 0;
    int fmt = 0, fw = 0, fh = 0;
    unsigned long long seq = 0;
    if (video_stream_get_frame(&raw, &raw_len, &fmt, &fw, &fh, &seq) != 0)
        return -1;
    if (seq == 0 || seq == g_raw_seq) { free(raw); return -1; }
    g_raw_seq = seq;

    if (fmt == VIDEO_FMT_MJPEG) {
        *out = raw;               /* 直接复用 */
        *len = raw_len;
    } else {                      /* YUYV → RGB → JPEG */
        unsigned char *rgb = malloc((size_t)fw * fh * 3);
        if (!rgb) { free(raw); return -1; }
        yolo_yuyv_to_rgb(raw, fw, fh, rgb);
        free(raw);
        if (yolo_rgb_to_jpeg(rgb, fw, fh, out, len) != 0) {
            free(rgb);
            return -1;
        }
        free(rgb);
    }
    if (w) *w = fw;
    if (h) *h = fh;
    return 0;
}

/* ---- 录制线程 ---- */

void *video_rec_task(void *arg)
{
    app_ctx_t *app = (app_ctx_t *)arg;

    while (app->running) {
        watchdog_feed_self("rec");

        /* 取命令（无命令则 100ms 空转喂狗） */
        pthread_mutex_lock(&g_rec.lock);
        int cmd = g_rec.cmd;
        g_rec.cmd = REC_CMD_NONE;
        pthread_mutex_unlock(&g_rec.lock);

        if (cmd == REC_CMD_START && !g_rec.recording) {
            /* 开新会话（需先有视频帧可用，用其宽高定会话尺寸） */
            unsigned char *probe = NULL; size_t plen = 0;
            int pw = 0, ph = 0;
            if (rec_take_raw_frame(&probe, &plen, &pw, &ph) != 0 ||
                pw <= 0 || ph <= 0) {
                if (probe) free(probe);
                pthread_mutex_lock(&g_rec.lock);
                g_rec.start_fail = 1;   /* 无视频帧：拒绝录制 */
                pthread_mutex_unlock(&g_rec.lock);
            } else {
                free(probe);
                char fname[128] = "";
                rec_mp4_t *s = rec_mp4_create(REC_DIR, "rec", pw, ph,
                                              fname, sizeof(fname));
                if (!s) {
                    pthread_mutex_lock(&g_rec.lock);
                    g_rec.start_fail = 2;
                    pthread_mutex_unlock(&g_rec.lock);
                    LOG_ERROR("rec: create recording file failed");
                } else {
                    LOG_INFO("rec: recording started -> %s (%dx%d)", fname, pw, ph);
                    pthread_mutex_lock(&g_rec.lock);
                    g_rec.recording = 1;
                    g_rec.start_fail = 0;
                    g_rec.start_ms = rec_mp4_start_ms(s);
                    g_rec.frames = 0;
                    g_rec.bytes = 0;
                    g_rec.last_frame_ts = 0;
                    safe_strncpy(g_rec.file, sizeof(g_rec.file), fname);
                    pthread_mutex_unlock(&g_rec.lock);

                    /* ---- 录制内层循环：快照轮询 20ms ---- */
                    int done = 0;
                    while (app->running && !done) {
                        watchdog_feed_self("rec");
                        pthread_mutex_lock(&g_rec.lock);
                        int still = g_rec.recording;
                        pthread_mutex_unlock(&g_rec.lock);
                        if (!still) break;

                        unsigned char *jpeg = NULL; size_t jlen = 0;
                        int fw = 0, fh = 0;
                        if (rec_take_ai_frame(&jpeg, &jlen) == 0) {
                            /* 画框帧：宽高沿用会话首帧尺寸 */
                        } else if (rec_take_raw_frame(&jpeg, &jlen, &fw, &fh) == 0) {
                            if (jpeg && (fw != pw || fh != ph)) { free(jpeg); jpeg = NULL; }
                        }
                        if (!jpeg) { usleep(20000); continue; }

                        uint64_t ts = now_ms();
                        if (rec_mp4_write_frame(s, jpeg, jlen, ts) != 0) {
                            free(jpeg);
                            done = 1;   /* 达上限或写失败：自动停止 */
                            break;
                        }
                        free(jpeg);

                        pthread_mutex_lock(&g_rec.lock);
                        g_rec.frames = rec_mp4_frames(s);
                        g_rec.bytes  = rec_mp4_bytes(s);
                        g_rec.last_frame_ts = ts;
                        pthread_mutex_unlock(&g_rec.lock);
                    }

                    /* ---- 结束会话（finalize 内部释放对象） ---- */
                    uint32_t n_frames = rec_mp4_frames(s);
                    uint32_t n_bytes  = rec_mp4_bytes(s);
                    rec_mp4_finalize(s);
                    pthread_mutex_lock(&g_rec.lock);
                    g_rec.recording = 0;
                    g_rec.start_fail = 0;
                    g_rec.file[0] = '\0';
                    pthread_mutex_unlock(&g_rec.lock);
                    if (n_frames > 0)
                        LOG_INFO("rec: recording finished -> %s (%u frames, %u bytes)",
                                 fname, n_frames, n_bytes);
                    else
                        LOG_INFO("rec: recording aborted (no frames): %s", fname);
                }
            }
        } else if (cmd == REC_CMD_STOP) {
            pthread_mutex_lock(&g_rec.lock);
            g_rec.recording = 0;
            pthread_mutex_unlock(&g_rec.lock);
        }

        usleep(100000);   /* 空转：响应命令与喂狗 */
    }
    return NULL;
}

/* ---- 对外 API（HTTP Reactor 线程调用） ---- */

int video_rec_start(void)
{
    pthread_mutex_lock(&g_rec.lock);
    if (g_rec.recording) {
        pthread_mutex_unlock(&g_rec.lock);
        return -1;   /* 已在录制 */
    }
    g_rec.cmd = REC_CMD_START;
    g_rec.start_fail = 0;
    pthread_mutex_unlock(&g_rec.lock);
    return 0;
}

int video_rec_stop(void)
{
    pthread_mutex_lock(&g_rec.lock);
    if (!g_rec.recording) {
        pthread_mutex_unlock(&g_rec.lock);
        return -1;
    }
    g_rec.cmd = REC_CMD_STOP;
    pthread_mutex_unlock(&g_rec.lock);
    return 0;
}

int video_rec_status(video_rec_status_t *out)
{
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    pthread_mutex_lock(&g_rec.lock);
    out->recording = g_rec.recording;
    out->start_ms  = g_rec.start_ms;
    out->frames    = g_rec.frames;
    out->bytes     = g_rec.bytes;
    safe_strncpy(out->file, sizeof(out->file),
                 g_rec.file[0] ? g_rec.file : "");
    /* 实时帧率：最近一帧与前若干帧的间隔由调用方按 start_ms 粗估 */
    pthread_mutex_unlock(&g_rec.lock);
    if (out->recording && out->frames > 1) {
        uint64_t el = now_ms() - out->start_ms;
        if (el > 0) out->fps = (double)out->frames * 1000.0 / (double)el;
    }
    return 0;
}

int video_rec_init(void *arg)
{
    (void)arg;
    memset(&g_rec, 0, sizeof(g_rec));
    pthread_mutex_init(&g_rec.lock, NULL);
    mkdir(REC_DIR, 0755);
    LOG_INFO("rec: recordings dir: %s", REC_DIR);
    return 0;
}

void video_rec_destroy(void *arg)
{
    (void)arg;
    /* 停止录制并等待线程退出（录制循环 ≤1 帧间隔退出，finalize 同步完成） */
    pthread_mutex_lock(&g_rec.lock);
    g_rec.recording = 0;
    g_rec.cmd = REC_CMD_NONE;
    pthread_mutex_unlock(&g_rec.lock);
    pthread_mutex_destroy(&g_rec.lock);
}
