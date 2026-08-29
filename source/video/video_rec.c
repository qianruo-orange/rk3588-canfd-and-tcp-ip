/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * video_rec.c — 网络录像模块：AI 画框帧（必要流程，无原始帧回退）+ H.264 硬件编码。
 *
 * 编码链路：帧（JPEG/YUYV）→ NV12 → /dev/video-enc0（rkvenc）→ H.264
 *   Annex-B → MP4(avc1) 封装（详见 video/rec_mp4.c）。
 *
 * 线程模型：main 模块表 "rec" 线程，空闲时 100ms 轮询喂狗；
 *   默认自动录制：任务启动即开始，按天分目录（recordings/YYYYMMDD），
 *   达到会话上限（帧数/体积）自动续录下一段；HTTP 线程可下发
 *   video_rec_start/stop 手动控制，手动停止后不再自动续录。
 * 录制分辨率取摄像头配置（cfg->video_width/height），未配置时退回首帧探测；
 * 帧率/码率不写死：会话开始前实测帧间隔，码率按分辨率动态计算。
 *
 * 帧来源为视频帧环形队列（video/frame_ring.h）编码游标：
 *   AI 画框帧 → encode_pick 锁内窃取槽内 nv12（零拷贝，用毕回池）。
 * AI 为必要流程：停摆（>1s 无标注帧）不回退原始帧，直接置 app->fatal、
 * 停 running 整体宕机（main 退出码 1 → systemd Restart=on-failure 拉起）。
 * 会话期间置 rec_active（槽位释放规则改走编码路径），结束即清除。
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
#include "video/h264_encoder.h"
#include "video/h264_stream.h"
#include "video/video_stream.h"
#include "video/frame_ring.h"
#include "ai/rknn_yolo.h"
#include "core/common.h"
#include "core/cpu_affinity.h"
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

/* ---- 帧获取：AI 画框帧（环形队列编码游标，必要流程无回退） ---- */

static unsigned long long g_ai_seq = 0;   /* 已消费画框帧序号 */
static unsigned long long g_raw_seq = 0;  /* 已读取原始帧序号（仅探测/测帧率用） */
static uint64_t g_last_ai_ms = 0;         /* 最近取到标注帧时刻（停摆宕机计时） */

/* 尝试从环形队列窃取 AI 画框帧的 NV12（渲染时已转换，免去二次 JPEG 解码）。
   成功后 *out 为池内缓冲，用毕 frame_ring_buf_put_locked 回池（勿 free）；
   无新帧或与编码器分辨率不符返回 -1（尺寸不符走 encode_skip，encode_skipped++） */
static int rec_take_ai_frame(frame_ring_t *ring, unsigned char **out, size_t *len,
                             size_t *cap, int *w, int *h, int pw, int ph)
{
    if (!ring || !rknn_yolo_enabled()) return -1;
    frame_ring_lock(ring);
    frame_slot_t *s = frame_ring_encode_pick_locked(ring, g_ai_seq);
    if (!s) { frame_ring_unlock(ring); return -1; }
    g_ai_seq = s->seq;
    if (s->w != pw || s->h != ph) {   /* 与编码器分辨率不符：跳过该帧 */
        frame_ring_encode_skip_locked(ring, s);
        frame_ring_unlock(ring);
        return -1;
    }
    *out = s->nv12.buf;   /* 锁内窃取：所有权移交 rec（槽内指针摘除，免每帧拷贝） */
    *len = s->nv12.len;
    *cap = s->nv12.cap;
    if (w) *w = s->w;
    if (h) *h = s->h;
    s->nv12.buf = NULL;
    s->nv12.len = s->nv12.cap = 0;
    frame_ring_encode_advance_locked(ring, s);
    frame_ring_unlock(ring);
    return 0;
}

/* 读取最新原始帧（锁内拷贝，*out 为 malloc，调用方 free）。
   仅供录制会话开始前的分辨率探测与帧率实测使用——录像本身不回退原始帧 */
static int rec_take_raw_frame(frame_ring_t *ring, unsigned char **out, size_t *len,
                              int *fmt, int *w, int *h)
{
    if (!ring) return -1;
    frame_ring_lock(ring);
    frame_slot_t *s = frame_ring_raw_newest_locked(ring);
    if (!s || s->seq == g_raw_seq) { frame_ring_unlock(ring); return -1; }
    unsigned char *raw = malloc(s->raw.len ? s->raw.len : 1);
    if (!raw) { frame_ring_unlock(ring); return -1; }
    memcpy(raw, s->raw.buf, s->raw.len);
    frame_ring_unlock(ring);
    g_raw_seq = s->seq;
    *out = raw;
    *len = s->raw.len;
    if (fmt) *fmt = s->fmt;
    if (w) *w = s->w;
    if (h) *h = s->h;
    return 0;
}

/* 实测帧率（不写死）：连续抓若干新帧取平均间隔，fps = 1000/avg_ms。
   失败返回 0（无视频帧）。 */
static int rec_measure_fps(app_ctx_t *app, frame_ring_t *ring)
{
    enum { N = 8 };
    uint64_t ts[N];
    int n = 0;
    uint64_t last = 0;
    uint64_t t0 = now_ms();
    while (n < N && app->running) {
        watchdog_feed_self("rec");
        unsigned char *f = NULL; size_t flen = 0;
        int fmt = 0;
        if (rec_take_raw_frame(ring, &f, &flen, &fmt, NULL, NULL) == 0) {
            uint64_t t = now_ms();
            if (t != last) { ts[n++] = t; last = t; }
            free(f);
        } else {
            usleep(20000);
        }
        if (now_ms() - t0 > 2000) break;   /* 2s 量测超时 */
    }
    if (n < 3) return 0;
    uint64_t span = ts[n - 1] - ts[0];
    uint64_t avg = span / (n - 1);
    if (avg == 0) avg = 1;
    int fps = (int)(1000 / avg);
    if (fps < 1) fps = 1;
    if (fps > 120) fps = 120;
    return fps;
}

/* ---- 录制线程 ---- */

void *video_rec_task(void *arg)
{
    app_ctx_t *app = (app_ctx_t *)arg;
    cpu_bind_big();
    frame_ring_t *ring = video_stream_get_ring();
    int auto_rec = 1;                    /* 默认自动开启录制 */
    int init_fail_cnt = 0;               /* 编码器连续初始化失败计数（日志退避） */

    while (app->running) {
        watchdog_feed_self("rec");

        /* 取命令（无命令则空转喂狗） */
        pthread_mutex_lock(&g_rec.lock);
        int cmd = g_rec.cmd;
        g_rec.cmd = REC_CMD_NONE;
        pthread_mutex_unlock(&g_rec.lock);

        if (cmd == REC_CMD_START) {
            auto_rec = 1;
        } else if (cmd == REC_CMD_STOP) {
            pthread_mutex_lock(&g_rec.lock);
            g_rec.recording = 0;
            pthread_mutex_unlock(&g_rec.lock);
            auto_rec = 0;                /* 手动停止后不再自动续录 */
        }

        if (auto_rec && !g_rec.recording) {
            /* ---- 开始一个录制会话（自动开启/续录或命令启动） ---- */
            int pw = app->cfg->video_width, ph = app->cfg->video_height;
            if (pw <= 0 || ph <= 0) {    /* 配置未设分辨率：首帧探测 */
                unsigned char *probe = NULL; size_t plen = 0;
                int fmt = 0;
                if (rec_take_raw_frame(ring, &probe, &plen, &fmt, &pw, &ph) != 0 ||
                    pw <= 0 || ph <= 0) {
                    if (probe) free(probe);
                    pthread_mutex_lock(&g_rec.lock);
                    g_rec.start_fail = 1;   /* 无视频帧且无配置分辨率 */
                    pthread_mutex_unlock(&g_rec.lock);
                    usleep(100000);
                    continue;
                }
                free(probe);
            }

            /* 实测帧率（不写死）→ 按分辨率动态码率 → 创建 H.264 编码器 */
            int fps = rec_measure_fps(app, ring);
            if (fps <= 0) {
                pthread_mutex_lock(&g_rec.lock);
                g_rec.start_fail = 1;   /* 无视频帧 */
                pthread_mutex_unlock(&g_rec.lock);
                usleep(100000);
                continue;
            }
            int bitrate = (int)((uint64_t)pw * ph * 2u);   /* 每像素 ~2bps */
            if (bitrate < 300000)  bitrate = 300000;
            if (bitrate > 16000000) bitrate = 16000000;
            h264_encoder_t *enc = h264_encoder_create(pw, ph, fps, bitrate);
            if (!enc) {
                init_fail_cnt++;
                pthread_mutex_lock(&g_rec.lock);
                g_rec.start_fail = 2;   /* 编码器不可用 */
                pthread_mutex_unlock(&g_rec.lock);
                /* 日志退避：首次与每 25 次失败打一条，避免设备不可用时刷屏 */
                if (init_fail_cnt == 1 || init_fail_cnt % 25 == 0)
                    LOG_ERROR("rec: h264 encoder init failed (%dx%d), retrying...", pw, ph);
                usleep(500000);
                continue;
            }
            init_fail_cnt = 0;

            /* 直播扇出：新会话（新编码器 → 新 SPS/PPS）告知推流模块清环换 epoch。
               rkmpp 首帧前 extradata 可能为空，sps/pps 传空由首 IDR 内嵌补齐 */
            {
                const unsigned char *sps = NULL, *pps = NULL;
                unsigned int slen = 0, plen = 0;
                h264_encoder_sps_pps(enc, &sps, &slen, &pps, &plen);
                h264_stream_push_config(pw, ph, fps, sps, slen, pps, plen);
            }

            /* 按天分目录：recordings/YYYYMMDD */
            time_t t = time(NULL);
            struct tm tm;
            localtime_r(&t, &tm);
            char day[16];
            snprintf(day, sizeof(day), "%04d%02d%02d",
                     tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
            char dir[512];
            snprintf(dir, sizeof(dir), "%s/%s", REC_DIR, day);

            char fname[128] = "";
            rec_mp4_t *s = rec_mp4_create(dir, "rec", pw, ph,
                                          fname, sizeof(fname));
            if (!s) {
                h264_encoder_destroy(enc);
                pthread_mutex_lock(&g_rec.lock);
                g_rec.start_fail = 3;
                pthread_mutex_unlock(&g_rec.lock);
                LOG_ERROR("rec: create recording file failed");
                usleep(100000);
                continue;
            }
            char rel[192];
            snprintf(rel, sizeof(rel), "%s/%s", day, fname);
            LOG_INFO("rec: recording started -> %s (%dx%d @%dfps)", rel, pw, ph, fps);
            pthread_mutex_lock(&g_rec.lock);
            g_rec.recording = 1;
            g_rec.start_fail = 0;
            g_rec.start_ms = rec_mp4_start_ms(s);
            g_rec.frames = 0;
            g_rec.bytes = 0;
            g_rec.last_frame_ts = 0;
            safe_strncpy(g_rec.file, sizeof(g_rec.file), rel);
            pthread_mutex_unlock(&g_rec.lock);

            /* ---- 录制内层循环：快照轮询 20ms ---- */
            if (ring) frame_ring_set_rec_active(ring, 1);   /* 会话开始：释放规则走编码路径 */
            g_last_ai_ms = now_ms();   /* 会话起点计时，避免启动初期误判停摆 */
            int done = 0;
            while (app->running && !done) {
                watchdog_feed_self("rec");
                pthread_mutex_lock(&g_rec.lock);
                int still = g_rec.recording;
                pthread_mutex_unlock(&g_rec.lock);
                if (!still) break;

                /* 取帧：AI 画框帧（渲染时已转 NV12，锁内窃取零拷贝），无原始帧回退 */
                unsigned char *nv12 = NULL;
                size_t nlen = 0, ncap = 0;
                int fw = 0, fh = 0;
                if (rec_take_ai_frame(ring, &nv12, &nlen, &ncap, &fw, &fh,
                                      pw, ph) == 0) {
                    g_last_ai_ms = now_ms();
                } else {
                    /* AI 必要流程：停摆 >1s（无新标注帧）直接宕机——置 fatal
                       使 main 以退出码 1 结束，systemd Restart=on-failure 拉起 */
                    if (now_ms() - g_last_ai_ms >= 1000) {
                        LOG_ERROR("rec: AI inference stalled >1s (mandatory AI, "
                                  "no raw fallback) — aborting service");
                        app->fatal = 1;
                        app->running = 0;
                        sync();
                        done = 1;
                        break;
                    }
                    usleep(10000);
                    continue;
                }

                /* H.264 编码 */
                unsigned char *h264 = NULL; size_t hlen = 0;
                int keyframe = 0;
                if (h264_encoder_encode(enc, nv12, &h264, &hlen, &keyframe) != 0) {
                    frame_ring_lock(ring);
                    frame_ring_buf_put_locked(ring, FRAME_RING_POOL_NV12, nv12, ncap);
                    frame_ring_unlock(ring);
                    done = 1;   /* 编码失败：结束本段（自动续录下一段） */
                    break;
                }
                /* AI 窃取的池内缓冲：用毕回池复用 */
                frame_ring_lock(ring);
                frame_ring_buf_put_locked(ring, FRAME_RING_POOL_NV12, nv12, ncap);
                frame_ring_unlock(ring);

                uint64_t ts = now_ms();
                if (rec_mp4_write_frame(s, h264, hlen, keyframe, ts) != 0) {
                    free(h264);
                    done = 1;   /* 达上限或写失败：结束本段（自动续录下一段） */
                    break;
                }
                /* 直播扇出：MP4 写成功后同一编码帧入推流环（此时无锁持有，叶子锁安全） */
                h264_stream_push_frame(h264, hlen, keyframe, ts);
                free(h264);

                pthread_mutex_lock(&g_rec.lock);
                g_rec.frames = rec_mp4_frames(s);
                g_rec.bytes  = rec_mp4_bytes(s);
                g_rec.last_frame_ts = ts;
                pthread_mutex_unlock(&g_rec.lock);
            }

            /* ---- 结束会话（finalize 内部释放对象） ---- */
            if (ring) frame_ring_set_rec_active(ring, 0);   /* 会话结束：回池 nv12 等 */
            uint32_t n_frames = rec_mp4_frames(s);
            uint32_t n_bytes  = rec_mp4_bytes(s);
            rec_mp4_finalize(s);
            h264_encoder_destroy(enc);
            h264_stream_push_end();   /* 直播扇出：会话结束断流（新连接 503，同 mjpeg_ai 无 AI 行为） */
            pthread_mutex_lock(&g_rec.lock);
            g_rec.recording = 0;
            g_rec.start_fail = 0;
            g_rec.file[0] = '\0';
            pthread_mutex_unlock(&g_rec.lock);
            if (n_frames > 0)
                LOG_INFO("rec: recording finished -> %s (%u frames, %u bytes)",
                         rel, n_frames, n_bytes);
            else
                LOG_INFO("rec: recording aborted (no frames): %s", rel);

            if (n_frames > 0 && auto_rec) continue;  /* 达上限：立即续录下一段 */
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
    /* 兜底：异常退出未走会话收尾时也解除 rec_active（rec 先于 video 析构，环仍有效） */
    frame_ring_t *ring = video_stream_get_ring();
    if (ring) frame_ring_set_rec_active(ring, 0);
    pthread_mutex_destroy(&g_rec.lock);
    /* 直播扇出：置停止标志并广播，唤醒全部推流线程收尾 */
    h264_stream_shutdown();
}
