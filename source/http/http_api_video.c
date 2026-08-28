/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * http_api_video.c — V4L2 设备列表 & 格式/分辨率查询 + 环形队列运行统计
 */

#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include "http/http_internal.h"
#include "video/frame_ring.h"
#include "video/video_stream.h"

/* ---- /api/video/devices —— 枚举可用的摄像头 ---- */
void http_video_devices(app_ctx_t *app, int fd, const char *method, const char *uri, const char *req_buf)
{
    (void)app; (void)method; (void)uri; (void)req_buf;
    char json[2048];
    int off = 0, first = 1;
    JSON_ADD(json, off, "[");

    for (int i = 0; i < 10; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/video%d", i);
        int dev_fd = open(path, O_RDONLY | O_NONBLOCK);
        if (dev_fd < 0) continue;

        struct v4l2_capability cap;
        if (ioctl(dev_fd, VIDIOC_QUERYCAP, &cap) == 0 &&
            (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
            char card[64];
            http_json_escape((const char *)cap.card, card, sizeof(card));
            JSON_ADD(json, off, "%s{\"path\":\"%s\",\"card\":\"%s\"}",
                     first ? "" : ",", path, card);
            first = 0;
        }
        close(dev_fd);
    }
    JSON_ADD(json, off, "]");
    http_ok_json(fd, json, (size_t)off);
}

/* ---- /api/video/caps —— 枚举真实格式与分辨率 ---- */

/* 常见分辨率档位：STEPWISE/CONTINUOUS 区间设备按这些档位筛选 */
static const struct { int w, h; } kStdSizes[] = {
    { 320,240 },{ 640,480 },{ 800,600 },{ 1024,768 },
    { 1280,720 },{ 1920,1080 },{ 2560,1440 },{ 3840,2160 },
};
#define kStdN ((int)(sizeof(kStdSizes)/sizeof(kStdSizes[0])))

static void caps_add_size(int *sw, int *sh, int *sn, int w, int h)
{
    if (w <= 0 || h <= 0 || *sn >= 64) return;
    for (int i = 0; i < *sn; i++)
        if (sw[i] == w && sh[i] == h) return;   /* 去重 */
    sw[*sn] = w; sh[*sn] = h; (*sn)++;
}

/* 在 STEPWISE/CONTINUOUS 区间内挑选标准档位，并附带区间极值 */
static void caps_add_std_sizes(int *sw, int *sh, int *sn,
                               int minw, int minh, int maxw, int maxh,
                               int step_w, int step_h)
{
    for (int i = 0; i < kStdN; i++) {
        int w = kStdSizes[i].w, h = kStdSizes[i].h;
        if (w < minw || w > maxw || h < minh || h > maxh) continue;
        if (step_w > 0 && (w - minw) % step_w != 0) continue;
        if (step_h > 0 && (h - minh) % step_h != 0) continue;
        caps_add_size(sw, sh, sn, w, h);
    }
    caps_add_size(sw, sh, sn, minw, minh);
    caps_add_size(sw, sh, sn, maxw, maxh);
}

/* 枚举某格式/分辨率支持的帧率（fps = 分母/分子，四舍五入），写入 fps[] 返回数量。
   驱动不支持枚举时回退常见帧率档位 */
static int caps_enum_fps(int dev_fd, unsigned int pixfmt, int w, int h,
                         int *fps, int maxfps)
{
    static const int kStdFps[] = { 60, 50, 30, 25, 20, 15, 10, 5 };
#define kStdFpsN ((int)(sizeof(kStdFps) / sizeof(kStdFps[0])))
    int n = 0;
    for (int ii = 0; ii < 32; ii++) {
        struct v4l2_frmivalenum fv;
        memset(&fv, 0, sizeof(fv));
        fv.index = ii;
        fv.pixel_format = pixfmt;
        fv.width = w;
        fv.height = h;
        if (ioctl(dev_fd, VIDIOC_ENUM_FRAMEINTERVALS, &fv) < 0) break;
        if (fv.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
            int f = (int)((fv.discrete.denominator + fv.discrete.numerator / 2)
                          / fv.discrete.numerator);
            if (f < 1 || f > 120) continue;
            int dup = 0;
            for (int k = 0; k < n; k++) if (fps[k] == f) { dup = 1; break; }
            if (!dup && n < maxfps) fps[n++] = f;
        } else if (fv.type == V4L2_FRMIVAL_TYPE_STEPWISE) {
            /* 区间帧率：min fps 对应 min.denominator/min.numerator */
            int fmin = (int)(fv.stepwise.min.denominator / fv.stepwise.min.numerator);
            int fmax = (int)(fv.stepwise.max.denominator / fv.stepwise.max.numerator);
            for (int k = 0; k < kStdFpsN; k++)
                if (kStdFps[k] >= fmin && kStdFps[k] <= fmax && n < maxfps)
                    fps[n++] = kStdFps[k];
            break;
        }
    }
    if (n == 0) {   /* 驱动不支持枚举：回退常见档位 */
        for (int k = 0; k < kStdFpsN && n < maxfps; k++) fps[n++] = kStdFps[k];
    }
    return n;
#undef kStdFpsN
}

/* 枚举设备真实格式/分辨率写入 json。全部写入成功且有可用项返回 1，否则 0 */
static int video_caps_fill(int dev_fd, char *json, int json_size,
                           const char *device, const char *card)
{
    int off = 0;
    off = http_json_append(json, (size_t)json_size, off,
                      "{\"device\":\"%s\",\"card\":\"%s\",\"formats\":[", device, card);
    if (off < 0) return 0;

    int fmt_first = 1, emitted = 0;
    for (int fi = 0; fi < 64; fi++) {
        struct v4l2_fmtdesc fmtd;
        memset(&fmtd, 0, sizeof(fmtd));
        fmtd.index = fi;
        fmtd.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(dev_fd, VIDIOC_ENUM_FMT, &fmtd) < 0) break;
        /* 推流编码只支持 MJPEG / YUYV，只上报这两种格式 */
        if (fmtd.pixelformat != V4L2_PIX_FMT_MJPEG && fmtd.pixelformat != V4L2_PIX_FMT_YUYV) continue;

        int sw[64], sh[64], sn = 0;
        for (int si = 0; si < 64; si++) {
            struct v4l2_frmsizeenum fr;
            memset(&fr, 0, sizeof(fr));
            fr.index = si;
            fr.pixel_format = fmtd.pixelformat;
            if (ioctl(dev_fd, VIDIOC_ENUM_FRAMESIZES, &fr) < 0) break;
            if (fr.type == V4L2_FRMSIZE_TYPE_DISCRETE)
                caps_add_size(sw, sh, &sn, fr.discrete.width, fr.discrete.height);
            else if (fr.type == V4L2_FRMSIZE_TYPE_STEPWISE)
                caps_add_std_sizes(sw, sh, &sn,
                    fr.stepwise.min_width, fr.stepwise.min_height,
                    fr.stepwise.max_width, fr.stepwise.max_height,
                    fr.stepwise.step_width, fr.stepwise.step_height);
            else if (fr.type == V4L2_FRMSIZE_TYPE_CONTINUOUS)
                caps_add_std_sizes(sw, sh, &sn,
                    fr.stepwise.min_width, fr.stepwise.min_height,
                    fr.stepwise.max_width, fr.stepwise.max_height, 0, 0);
        }
        if (sn == 0) continue;   /* 驱动无该格式的分辨率信息 */

        off = http_json_append(json, (size_t)json_size, off, "%s{\"fmt\":\"%s\",\"desc\":\"%s\",\"sizes\":[",
                          fmt_first ? "" : ",",
                          fmtd.pixelformat == V4L2_PIX_FMT_MJPEG ? "MJPG" : "YUYV",
                          fmtd.pixelformat == V4L2_PIX_FMT_MJPEG ? "MJPEG" : "YUYV 4:2:2");
        if (off < 0) return 0;
        for (int k = 0; k < sn; k++) {
            int fps[16], fn = caps_enum_fps(dev_fd, fmtd.pixelformat,
                                            sw[k], sh[k], fps, 16);
            char fps_str[128];
            int foff = 0;
            for (int m = 0; m < fn; m++)
                foff += snprintf(fps_str + foff, sizeof(fps_str) - (size_t)foff,
                                 "%s%d", m > 0 ? "," : "", fps[m]);
            off = http_json_append(json, (size_t)json_size, off,
                              "%s{\"w\":%d,\"h\":%d,\"fps\":[%s]}",
                              k > 0 ? "," : "", sw[k], sh[k], fps_str);
            if (off < 0) return 0;
        }
        off = http_json_append(json, (size_t)json_size, off, "]}");
        if (off < 0) return 0;
        fmt_first = 0;
        emitted = 1;
    }

    if (!emitted) return 0;
    off = http_json_append(json, (size_t)json_size, off, "]}");
    return off < 0 ? 0 : 1;
}

void http_video_caps(app_ctx_t *app, int fd, const char *method, const char *uri, const char *req_buf)
{
    (void)app; (void)method; (void)req_buf;

    const char *q = strchr(uri, '?');
    const char *device = "/dev/video0";
    if (q) {
        const char *d = strstr(q + 1, "device=");
        if (d) {
            const char *val = d + 7;
            const char *p = val;
            while (*p && *p != '&') p++;
            size_t len = (size_t)(p - val);
            /* 仅允许 /dev/videoN（N 为十进制数字），防止 open 任意路径 */
            if (len > 10 && strncmp(val, "/dev/video", 10) == 0) {
                int valid = 1;
                for (size_t k = 10; k < len; k++)
                    if (val[k] < '0' || val[k] > '9') { valid = 0; break; }
                if (valid) device = val;
            }
        }
    }

    char card[64] = "Unknown Camera";
    char json[16384];   /* 尺寸 × 格式 × FPS 列表可能较大 */
    int off = 0;

    int dev_fd = open(device, O_RDONLY | O_NONBLOCK);
    if (dev_fd >= 0) {
        struct v4l2_capability cap;
        if (ioctl(dev_fd, VIDIOC_QUERYCAP, &cap) == 0)
            http_json_escape((const char *)cap.card, card, sizeof(card));
        /* 真实枚举成功即用真实能力返回 */
        if (video_caps_fill(dev_fd, json, sizeof(json), device, card)) {
            close(dev_fd);
            http_ok_json(fd, json, strlen(json));
            return;
        }
        close(dev_fd);
    }

    /* 设备不可用 / 驱动不支持枚举：回退预设档位，保证前端仍有可选分辨率 */
    static const struct { int w, h; const char *label; } presets[] = {
        { 320,  240, "QVGA" },
        { 640,  480, "VGA" },
        { 800,  600, "SVGA" },
        { 1024, 768, "XGA" },
        { 1280, 720, "HD" },
        { 1920,1080, "FHD" },
        { 2560,1440, "2K" },
        { 3840,2160, "4K" },
    };
#define NP (int)(sizeof(presets)/sizeof(presets[0]))

    JSON_ADD(json, off,
        "{\"device\":\"%s\",\"card\":\"%s\",\"formats\":["
        "{\"fmt\":\"MJPG\",\"desc\":\"MJPEG\",\"sizes\":[", device, card);

    for (int i = 0; i < NP; i++)
        JSON_ADD(json, off, "%s{\"w\":%d,\"h\":%d,\"label\":\"%s\",\"fps\":[30,25,20,15,10,5]}",
                 i > 0 ? "," : "", presets[i].w, presets[i].h, presets[i].label);

    JSON_ADD(json, off, "]},"
        "{\"fmt\":\"YUYV\",\"desc\":\"YUYV 4:2:2\",\"sizes\":[");

    for (int i = 0; i < NP; i++)
        JSON_ADD(json, off, "%s{\"w\":%d,\"h\":%d,\"label\":\"%s\",\"fps\":[30,25,20,15,10,5]}",
                 i > 0 ? "," : "", presets[i].w, presets[i].h, presets[i].label);

    JSON_ADD(json, off, "]}]}");
    http_ok_json(fd, json, (size_t)off);
#undef NP
}

/* ---- /api/video/stats —— 环形队列分阶段计数器（回答"摄像头 vs 推理"丢帧问题） ---- */
void http_video_stats(app_ctx_t *app, int fd, const char *method, const char *uri, const char *req_buf)
{
    (void)app; (void)method; (void)uri; (void)req_buf;
    frame_ring_t *ring = video_stream_get_ring();
    if (!ring) {
        http_ok_json(fd, "{\"error\":\"video not running\"}", 28);
        return;
    }
    frame_ring_stats_t st;
    frame_ring_stats(ring, &st);
    char json[512];
    int off = 0;
    JSON_ADD(json, off, "{\"produce_seq\":%llu,\"capture_dropped\":%llu,"
        "\"driver_dropped\":%llu,\"capture_stall_ms\":%llu,\"infer_dropped\":%llu,"
        "\"render_skipped\":%llu,\"encode_skipped\":%llu,\"raw_pinned\":%d,"
        "\"pool_rgb\":%d,\"pool_nv12\":%d,\"pool_jpeg\":%d}",
        st.produce_seq, st.capture_dropped, st.driver_dropped, st.capture_stall_ms,
        st.infer_dropped, st.render_skipped, st.encode_skipped,
        st.raw_pinned, st.pool_rgb, st.pool_nv12, st.pool_jpeg);
    http_ok_json(fd, json, (size_t)off);
}
