/**
 * h264_encoder.c — RK3588 硬件 H.264 编码器（V4L2 M2M，/dev/video-enc0 rkvenc）。
 *
 * 输入 NV12（单 plane），输出 H.264 Annex-B（含 SPS/PPS/IDR/slice）。
 * 同步接口：一次 encode 一帧，内部 poll 等待 M2M 队列就绪。
 * 关键帧（IDR）与 SPS/PPS 通过 NAL 扫描自动识别，供 MP4 封装写 avcC/stss。
 *
 * 单线程使用（录制线程独占）。
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <linux/v4l2-controls.h>

#include "video/h264_encoder.h"
#include "core/log.h"

#define ENC_DEV   "/dev/video-enc0"
#define ENC_BUFS  3      /* 每队列缓冲数 */

#define V4L2_CID_MPEG_VIDEO_H264_SPS_PPS_BEFORE_IDR \
    0x00980919u  /* Rockchip 扩展 control（V4L2_CID_MPEG_VIDEO_BASE+24） */

struct h264_encoder_s {
    int  fd;
    int  w, h;
    int  sizeimage_in;              /* output 单 plane 容量 */
    int  sizeimage_out;             /* capture 单 plane 容量 */
    void *buf_in[ENC_BUFS];         /* mmap output 缓冲 */
    void *buf_out[ENC_BUFS];        /* mmap capture 缓冲 */
    /* SPS/PPS（最近一次，供 avcC） */
    unsigned char sps[64];
    unsigned int  sps_len;
    unsigned char pps[64];
    unsigned int  pps_len;
};

/* ---- NAL 扫描（Annex-B） ---- */

/* 返回 NAL 起始偏移（相对 data），-1 无 */
static long nal_find(const unsigned char *d, size_t len, long from)
{
    size_t i = (from > 0) ? (size_t)from : 0;
    for (; i + 3 <= len; i++) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) return (long)(i + 3);          /* 3 字节 */
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 0 && i + 3 < len && d[i + 3] == 1)
            return (long)(i + 4);                                                        /* 4 字节 */
    }
    return -1;
}

/* ---- 设备控制 ---- */

static int enc_set_ctrl(h264_encoder_t *e, uint32_t id, int value)
{
    struct v4l2_control c;
    memset(&c, 0, sizeof(c));
    c.id = id;
    c.value = value;
    if (ioctl(e->fd, VIDIOC_S_CTRL, &c) < 0) {
        /* 部分固件缺该 control（如 SPS_PPS_BEFORE_IDR），静默跳过 */
        return -1;
    }
    return 0;
}

static int enc_reqbufs(h264_encoder_t *e, uint32_t type, void **bufs)
{
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count  = ENC_BUFS;
    req.type   = type;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(e->fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) {
        LOG_ERROR("h264: VIDIOC_REQBUFS type=%u failed", type);
        return -1;
    }
    for (uint32_t i = 0; i < req.count; i++) {
        struct v4l2_buffer b;
        struct v4l2_plane  p;
        memset(&b, 0, sizeof(b));
        memset(&p, 0, sizeof(p));
        b.type     = type;
        b.memory   = V4L2_MEMORY_MMAP;
        b.index    = i;
        b.length   = 1;
        b.m.planes = &p;
        if (ioctl(e->fd, VIDIOC_QUERYBUF, &b) < 0) {
            LOG_ERROR("h264: VIDIOC_QUERYBUF type=%u idx=%u failed", type, i);
            return -1;
        }
        bufs[i] = mmap(NULL, p.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                       e->fd, p.m.mem_offset);
        if (bufs[i] == MAP_FAILED) {
            LOG_ERROR("h264: mmap type=%u failed", type);
            bufs[i] = NULL;
            return -1;
        }
        if (type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
            e->sizeimage_in = (int)p.length;
        else
            e->sizeimage_out = (int)p.length;

        memset(&b, 0, sizeof(b));
        memset(&p, 0, sizeof(p));
        b.type     = type;
        b.memory   = V4L2_MEMORY_MMAP;
        b.index    = i;
        b.length   = 1;
        b.m.planes = &p;
        if (ioctl(e->fd, VIDIOC_QBUF, &b) < 0) {
            LOG_ERROR("h264: VIDIOC_QBUF type=%u idx=%u failed", type, i);
            return -1;
        }
    }
    return (int)req.count;
}

h264_encoder_t *h264_encoder_create(int w, int h, int fps, int bitrate_bps)
{
    if (w <= 0 || h <= 0 || (w & 1) || (h & 1)) return NULL;

    h264_encoder_t *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->fd = -1;
    e->w = w; e->h = h;

    e->fd = open(ENC_DEV, O_RDWR | O_NONBLOCK, 0);
    if (e->fd < 0) {
        LOG_ERROR("h264: open %s failed: %s", ENC_DEV, strerror(errno));
        free(e);
        return NULL;
    }

    struct v4l2_capability cap;
    if (ioctl(e->fd, VIDIOC_QUERYCAP, &cap) < 0 ||
        !(cap.capabilities & V4L2_CAP_VIDEO_M2M_MPLANE)) {
        LOG_ERROR("h264: %s not M2M MPLANE", ENC_DEV);
        goto fail;
    }

    /* ---- output 队列：NV12 ---- */
    struct v4l2_format ofmt;
    memset(&ofmt, 0, sizeof(ofmt));
    ofmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    ofmt.fmt.pix_mp.width       = (uint32_t)w;
    ofmt.fmt.pix_mp.height      = (uint32_t)h;
    ofmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    ofmt.fmt.pix_mp.field       = V4L2_FIELD_ANY;
    ofmt.fmt.pix_mp.num_planes  = 1;
    if (ioctl(e->fd, VIDIOC_S_FMT, &ofmt) < 0) {
        LOG_ERROR("h264: S_FMT NV12 %dx%d failed: %s", w, h, strerror(errno));
        goto fail;
    }

    /* ---- capture 队列：H264 ---- */
    struct v4l2_format cfmt;
    memset(&cfmt, 0, sizeof(cfmt));
    cfmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    cfmt.fmt.pix_mp.width       = (uint32_t)w;
    cfmt.fmt.pix_mp.height      = (uint32_t)h;
    cfmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264;
    cfmt.fmt.pix_mp.field       = V4L2_FIELD_ANY;
    cfmt.fmt.pix_mp.num_planes  = 1;
    if (ioctl(e->fd, VIDIOC_S_FMT, &cfmt) < 0) {
        LOG_ERROR("h264: S_FMT H264 failed: %s", strerror(errno));
        goto fail;
    }

    /* ---- 编码参数（帧率驱动 GOP，码率按分辨率动态，不写死） ---- */
    if (fps <= 0) fps = 30;
    enc_set_ctrl(e, V4L2_CID_MPEG_VIDEO_BITRATE, bitrate_bps);
    enc_set_ctrl(e, V4L2_CID_MPEG_VIDEO_BITRATE_MODE, V4L2_MPEG_VIDEO_BITRATE_MODE_CBR);
    enc_set_ctrl(e, V4L2_CID_MPEG_VIDEO_H264_PROFILE, V4L2_MPEG_VIDEO_H264_PROFILE_MAIN);
    enc_set_ctrl(e, V4L2_CID_MPEG_VIDEO_H264_I_PERIOD, fps * 2);      /* GOP=2s */
    enc_set_ctrl(e, V4L2_CID_MPEG_VIDEO_H264_SPS_PPS_BEFORE_IDR, 1);  /* 尽力，失败忽略 */

    /* ---- 申请缓冲 ---- */
    if (enc_reqbufs(e, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, e->buf_in) < 0 ||
        enc_reqbufs(e, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, e->buf_out) < 0)
        goto fail;

    uint32_t ot = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    uint32_t ct = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (ioctl(e->fd, VIDIOC_STREAMON, &ot) < 0 || ioctl(e->fd, VIDIOC_STREAMON, &ct) < 0) {
        LOG_ERROR("h264: STREAMON failed: %s", strerror(errno));
        goto fail;
    }

    LOG_INFO("h264: encoder ready %dx%d fps=%d bitrate=%d", w, h, fps, bitrate_bps);
    return e;

fail:
    h264_encoder_destroy(e);
    return NULL;
}

void h264_encoder_destroy(h264_encoder_t *e)
{
    if (!e) return;
    if (e->fd >= 0) {
        uint32_t ot = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        uint32_t ct = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        ioctl(e->fd, VIDIOC_STREAMOFF, &ot);
        ioctl(e->fd, VIDIOC_STREAMOFF, &ct);
        for (int i = 0; i < ENC_BUFS; i++) {
            if (e->buf_in[i])  munmap(e->buf_in[i],  (size_t)e->sizeimage_in);
            if (e->buf_out[i]) munmap(e->buf_out[i], (size_t)e->sizeimage_out);
        }
        close(e->fd);
    }
    free(e);
}

/* 逐行拷贝 NV12 到 output 缓冲（bytesperline 可能大于宽度，逐行复制最稳） */
static void nv12_copy(h264_encoder_t *e, const unsigned char *nv12)
{
    int w = e->w, h = e->h;
    unsigned char *dst = e->buf_in[0];
    for (int y = 0; y < h; y++) {
        memcpy(dst + (size_t)y * w, nv12 + (size_t)y * w, (size_t)w);
    }
    const unsigned char *uv = nv12 + (size_t)w * h;
    unsigned char *dut = dst + (size_t)w * h;
    for (int y = 0; y < h / 2; y++) {
        memcpy(dut + (size_t)y * w, uv + (size_t)y * w, (size_t)w);
    }
}

/* 从 Annex-B 码流提取 SPS/PPS，判断关键帧（含 IDR） */
static void h264_scan(h264_encoder_t *e, const unsigned char *d, size_t len,
                      int *keyframe)
{
    long pos = nal_find(d, len, 0);
    while (pos >= 0 && (size_t)pos < len) {
        unsigned char type = d[pos] & 0x1F;
        long next = nal_find(d, len, pos + 1);
        size_t nlen = (next < 0) ? (len - (size_t)pos) : (size_t)(next - pos);
        if (type == 7 && nlen <= sizeof(e->sps)) {       /* SPS */
            memcpy(e->sps, d + pos, nlen);
            e->sps_len = (unsigned int)nlen;
        } else if (type == 8 && nlen <= sizeof(e->pps)) { /* PPS */
            memcpy(e->pps, d + pos, nlen);
            e->pps_len = (unsigned int)nlen;
        } else if (type == 5) {
            *keyframe = 1;
        }
        pos = next;
    }
}

int h264_encoder_encode(h264_encoder_t *e, const unsigned char *nv12,
                        unsigned char **out, size_t *out_len, int *keyframe)
{
    if (!e || !nv12 || !out || !out_len) return -1;
    *out = NULL;
    *out_len = 0;
    if (keyframe) *keyframe = 0;

    /* 1. 取一个已排队的 output 缓冲并填数据 */
    struct v4l2_buffer ob;
    struct v4l2_plane  op;
    memset(&ob, 0, sizeof(ob));
    memset(&op, 0, sizeof(op));
    ob.type     = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    ob.memory   = V4L2_MEMORY_MMAP;
    ob.length   = 1;
    ob.m.planes = &op;
    if (ioctl(e->fd, VIDIOC_DQBUF, &ob) < 0) {
        LOG_ERROR("h264: DQBUF out failed: %s", strerror(errno));
        return -1;
    }
    nv12_copy(e, nv12);
    op.bytesused = (uint32_t)((size_t)e->w * e->h * 3 / 2);
    if (ioctl(e->fd, VIDIOC_QBUF, &ob) < 0) {
        LOG_ERROR("h264: QBUF out failed: %s", strerror(errno));
        return -1;
    }

    /* 2. 等待并读取编码结果（可能包含纯 SPS/PPS 配置帧，无 VCL 则丢弃重读） */
    for (int tries = 0; tries < 4; tries++) {
        struct pollfd pfd = { .fd = e->fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 2000);
        if (pr <= 0) {
            LOG_ERROR("h264: poll capture timeout");
            return -1;
        }

        struct v4l2_buffer cb;
        struct v4l2_plane  cp;
        memset(&cb, 0, sizeof(cb));
        memset(&cp, 0, sizeof(cp));
        cb.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        cb.memory   = V4L2_MEMORY_MMAP;
        cb.length   = 1;
        cb.m.planes = &cp;
        if (ioctl(e->fd, VIDIOC_DQBUF, &cb) < 0) {
            if (errno == EAGAIN) continue;
            LOG_ERROR("h264: DQBUF cap failed: %s", strerror(errno));
            return -1;
        }
        /* 归还 capture 缓冲 */
        ioctl(e->fd, VIDIOC_QBUF, &cb);

        unsigned char *data = e->buf_out[cb.index];
        size_t len = (size_t)cp.bytesused;

        int kf = 0;
        h264_scan(e, data, len, &kf);
        if (!kf && len >= 4) {
            /* 无 IDR：仅 SPS/PPS 配置帧，丢弃并继续等 VCL 帧 */
            long pos = nal_find(data, len, 0);
            while (pos >= 0 && (size_t)pos < len) {
                unsigned char t = data[pos] & 0x1F;
                if (t == 1 || t == 5) { kf = 1; break; }
                long next = nal_find(data, len, pos + 1);
                pos = next;
            }
            if (!kf) {
                /* 纯配置帧：丢弃，等下一轮 */
                continue;
            }
        }

        unsigned char *copy = malloc(len);
        if (!copy) { LOG_ERROR("h264: out alloc failed"); return -1; }
        memcpy(copy, data, len);
        *out = copy;
        *out_len = len;
        if (keyframe) *keyframe = kf;
        return 0;
    }
    LOG_ERROR("h264: no VCL frame after %d tries", 4);
    return -1;
}

/* SPS/PPS 提取（供 avcC 写入） */
int h264_encoder_sps_pps(h264_encoder_t *e,
                         const unsigned char **sps, unsigned int *sps_len,
                         const unsigned char **pps, unsigned int *pps_len)
{
    if (!e) return -1;
    if (sps)     *sps     = e->sps;
    if (sps_len) *sps_len = e->sps_len;
    if (pps)     *pps     = e->pps;
    if (pps_len) *pps_len = e->pps_len;
    return (e->sps_len > 0 && e->pps_len > 0) ? 0 : -1;
}
