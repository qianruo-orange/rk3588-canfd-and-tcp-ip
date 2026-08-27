/**
 * video_rec.c — 网络录像模块：AI 画框帧优先（回退原始帧）+ H.264 硬件编码。
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
static uint64_t g_last_ai_ms = 0;         /* 最近取到标注帧时刻（原始帧回退的退避计时） */

/* 尝试取 AI 画框帧的 NV12（渲染时已转换，免去二次 JPEG 解码）；无新帧返回 -1 */
static int rec_take_ai_frame(unsigned char **out, size_t *len, int *w, int *h)
{
    if (!rknn_yolo_enabled()) return -1;
    unsigned long long seq = rknn_yolo_get_frame_seq();
    if (seq == 0 || seq == g_ai_seq) return -1;
    unsigned long long aseq = 0;
    if (rknn_yolo_get_frame_nv12(out, len, &aseq, w, h) != 0) return -1;
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

/* ---- 色彩转换：统一转 NV12（编码器输入） ---- */

/* RGB24 → NV12（BT.601 limited，整数定点） */
static void rgb_to_nv12(const unsigned char *rgb, int w, int h,
                        unsigned char *nv12)
{
    unsigned char *Y = nv12;
    unsigned char *UV = nv12 + (size_t)w * h;
    for (int y = 0; y < h; y++) {
        const unsigned char *row = rgb + (size_t)y * w * 3;
        unsigned char *Yrow = Y + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            const unsigned char *p = row + x * 3;
            int r = p[0], g = p[1], b = p[2];
            Yrow[x] = (unsigned char)((66 * r + 129 * g + 25 * b + 128) / 256 + 16);
            if ((y & 1) == 0 && (x & 1) == 0) {
                unsigned char *uv = UV + (size_t)(y / 2) * w + x;
                uv[0] = (unsigned char)((-38 * r - 74 * g + 112 * b + 128) / 256 + 128);
                uv[1] = (unsigned char)((112 * r - 94 * g - 18 * b + 128) / 256 + 128);
            }
        }
    }
}

/* YUYV → NV12（4:2:2 → 4:2:0，垂直抽样偶数行） */
static void yuyv_to_nv12(const unsigned char *src, int w, int h,
                         unsigned char *nv12)
{
    unsigned char *Y = nv12;
    unsigned char *UV = nv12 + (size_t)w * h;
    for (int y = 0; y < h; y++) {
        const unsigned char *row = src + (size_t)y * w * 2;
        unsigned char *Yrow = Y + (size_t)y * w;
        for (int x = 0; x < w; x++)
            Yrow[x] = row[x * 2];
        if ((y & 1) == 0) {
            unsigned char *uv = UV + (size_t)(y / 2) * w;
            for (int x = 0; x < w; x += 2) {
                uv[x]     = row[x * 2 + 1];   /* U */
                uv[x + 1] = row[x * 2 + 3];   /* V */
            }
        }
    }
}

/* 把一帧（JPEG / YUYV）转为 NV12；成功返回 0，*nv12 为 malloc（调用方 free），
   输出实际宽高（JPEG 以解码尺寸为准；YUYV 即输入 w/h）。 */
static int rec_to_nv12(const unsigned char *data, size_t len, int fmt,
                       int w, int h, unsigned char **nv12, size_t *nv12_len,
                       int *out_w, int *out_h)
{
    if (fmt == VIDEO_FMT_MJPEG) {
        unsigned char *rgb = NULL;
        int rw = 0, rh = 0;
        if (yolo_jpeg_to_rgb(data, len, &rgb, &rw, &rh) != 0 || !rgb)
            return -1;
        unsigned char *out = malloc((size_t)rw * rh * 3 / 2);
        if (!out) { free(rgb); return -1; }
        rgb_to_nv12(rgb, rw, rh, out);
        free(rgb);
        *nv12 = out;
        *nv12_len = (size_t)rw * rh * 3 / 2;
        if (out_w) *out_w = rw;
        if (out_h) *out_h = rh;
        return 0;
    }
    /* YUYV */
    unsigned char *out = malloc((size_t)w * h * 3 / 2);
    if (!out) return -1;
    yuyv_to_nv12(data, w, h, out);
    *nv12 = out;
    *nv12_len = (size_t)w * h * 3 / 2;
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return 0;
}

/* 实测帧率（不写死）：连续抓若干新帧取平均间隔，fps = 1000/avg_ms。
   失败返回 0（无视频帧）。 */
static int rec_measure_fps(app_ctx_t *app)
{
    enum { N = 8 };
    uint64_t ts[N];
    int n = 0;
    uint64_t last = 0;
    uint64_t t0 = now_ms();
    while (n < N && app->running) {
        watchdog_feed_self("rec");
        unsigned char *f = NULL; size_t flen = 0;
        if (rec_take_raw_frame(&f, &flen, NULL, NULL) == 0) {
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
                if (rec_take_raw_frame(&probe, &plen, &pw, &ph) != 0 ||
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
            int fps = rec_measure_fps(app);
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
            int done = 0;
            while (app->running && !done) {
                watchdog_feed_self("rec");
                pthread_mutex_lock(&g_rec.lock);
                int still = g_rec.recording;
                pthread_mutex_unlock(&g_rec.lock);
                if (!still) break;

                /* 取帧：AI 画框帧（渲染时已转 NV12）优先，回退原始帧（JPEG/YUYV） */
                unsigned char *nv12 = NULL;
                size_t nlen = 0;
                int fw = 0, fh = 0, took = -1;
                if (rec_take_ai_frame(&nv12, &nlen, &fw, &fh) == 0) {
                    if (fw == pw && fh == ph) {
                        took = 0;
                        g_last_ai_ms = now_ms();
                    } else {   /* AI 帧分辨率与编码器不符：丢弃，走原始帧 */
                        free(nv12);
                        nv12 = NULL;
                    }
                }
                if (took < 0) {
                    /* AI 可用时优先等下一帧标注帧，不逐 tick 回退原始帧：
                       回退会使录像混入未标注帧，且原始帧 JPEG 解码白白烧 CPU
                       （rec 线程实测 ~75%）。仅当标注帧 >1s 未更新（推理卡死
                       降级）才回退原始帧保证录像不断流 */
                    if (rknn_yolo_enabled() && now_ms() - g_last_ai_ms < 1000) {
                        usleep(10000);
                        continue;
                    }
                    int fmt = 0; unsigned long long seq = 0;
                    unsigned char *raw = NULL; size_t raw_len = 0;
                    if (video_stream_get_frame(&raw, &raw_len, &fmt, &fw, &fh, &seq) == 0 &&
                        seq != g_raw_seq) {
                        g_raw_seq = seq;
                        if (fw == pw && fh == ph &&
                            rec_to_nv12(raw, raw_len, fmt, fw, fh,
                                        &nv12, &nlen, NULL, NULL) == 0)
                            took = 0;
                    }
                    if (raw) free(raw);
                }
                if (took < 0) { usleep(20000); continue; }

                /* H.264 编码 */
                unsigned char *h264 = NULL; size_t hlen = 0;
                int keyframe = 0;
                if (h264_encoder_encode(enc, nv12, &h264, &hlen, &keyframe) != 0) {
                    free(nv12);
                    done = 1;   /* 编码失败：结束本段（自动续录下一段） */
                    break;
                }
                free(nv12);

                uint64_t ts = now_ms();
                if (rec_mp4_write_frame(s, h264, hlen, keyframe, ts) != 0) {
                    free(h264);
                    done = 1;   /* 达上限或写失败：结束本段（自动续录下一段） */
                    break;
                }
                free(h264);

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
            h264_encoder_destroy(enc);
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
    pthread_mutex_destroy(&g_rec.lock);
}
