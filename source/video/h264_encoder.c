/**
 * h264_encoder.c — RK3588 硬件 H.264 编码器封装。
 *
 * 后端自动选择（编译 + 运行时）：
 *   1) FFmpeg h264_rkmpp（Rockchip MPP 硬件编码，HAVE_AVCODEC 且运行时存在）
 *   2) V4L2 M2M rkvenc（/dev/video-enc0 或扫描到的编码器节点）
 *
 * 输入 NV12（Y 平面 w*h + 交错 UV w*h/2），输出 H.264 Annex-B
 * （含 SPS/PPS/IDR/slice，以 00 00 00 01 分隔）。同步接口：一次 encode 一帧。
 * 关键帧（IDR）与 SPS/PPS 自动识别，供 MP4 封装写 avcC/stss。
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
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <linux/v4l2-controls.h>

#ifdef HAVE_AVCODEC
#include <libavcodec/avcodec.h>
#include <libavutil/pixfmt.h>
#include <libavutil/error.h>   /* av_err2str */
#endif

#include "video/h264_encoder.h"
#include "core/log.h"
#include "core/common.h"   /* safe_strncpy */

#define ENC_DEV   "/dev/video-enc0"
#define ENC_BUFS  3      /* 每队列缓冲数 */

#define V4L2_CID_MPEG_VIDEO_H264_SPS_PPS_BEFORE_IDR \
    0x00980919u  /* Rockchip 扩展 control（V4L2_CID_MPEG_VIDEO_BASE+24） */

/* 编码器创建失败的日志限频：同一错误 10s 内只打一条，
   避免录制线程 0.5s 重试时无限刷屏 */
static struct timespec g_last_err_ts = { 0, 0 };
static int errlog_spam_ok(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long dt_ms = (now.tv_sec - g_last_err_ts.tv_sec) * 1000L +
                 (now.tv_nsec - g_last_err_ts.tv_nsec) / 1000000L;
    if (g_last_err_ts.tv_sec == 0 || dt_ms > 10000) {
        g_last_err_ts = now;
        return 1;
    }
    return 0;
}

/* 判断某节点是否为 V4L2 M2M 编码器：
   QUERYCAP 通过（M2M/M2M_MPLANE）且 output 队列支持 NV12（编码器特征，
   解码器 output 只接受压缩格式） */
static int node_is_v4l2_encoder(const char *path)
{
    int fd = open(path, O_RDWR | O_NONBLOCK, 0);
    if (fd < 0) return 0;

    struct v4l2_capability cap;
    int ok = 0;
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
        uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS)
                            ? cap.device_caps : cap.capabilities;
        if (caps & (V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_VIDEO_M2M)) {
            for (int i = 0; i < 32; i++) {
                struct v4l2_fmtdesc fdsc;
                memset(&fdsc, 0, sizeof(fdsc));
                fdsc.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
                fdsc.index = (uint32_t)i;
                if (ioctl(fd, VIDIOC_ENUM_FMT, &fdsc) < 0) break;
                if (fdsc.pixelformat == V4L2_PIX_FMT_NV12 ||
                    fdsc.pixelformat == V4L2_PIX_FMT_NV12M) { ok = 1; break; }
            }
        }
    }
    close(fd);
    return ok;
}

/* 探测可用的 V4L2 H.264 编码器节点：优先 /dev/video-enc0，其次扫描 /dev/video0..15。
   返回 0 成功并把节点路径写入 dev（调用方提供缓冲区）。 */
int h264_encoder_probe(char *dev, size_t dev_size)
{
    if (node_is_v4l2_encoder(ENC_DEV)) {
        safe_strncpy(dev, dev_size, ENC_DEV);
        return 0;
    }
    for (int i = 0; i < 16; i++) {
        char p[32];
        snprintf(p, sizeof(p), "/dev/video%d", i);
        if (node_is_v4l2_encoder(p)) {
            safe_strncpy(dev, dev_size, p);
            return 0;
        }
    }
    dev[0] = '\0';
    return -1;
}

struct h264_encoder_s {
    int  w, h;
    int  using_ff;              /* 1=FFmpeg h264_rkmpp 后端，0=V4L2 后端 */
#ifdef HAVE_AVCODEC
    AVCodecContext *avctx;
    AVFrame *frame;
    AVPacket *pkt;                  /* 复用同一 packet，避免每帧 alloc/free */
    int64_t pts;
#endif
    /* V4L2 后端状态 */
    int  fd;
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

/* ---- FFmpeg h264_rkmpp 后端 ---- */

#ifdef HAVE_AVCODEC

/* 从 avcC extradata 解析 SPS/PPS（NAL 无起始码，与 MP4 avcC 内嵌格式一致）。
   布局：1(ver) profile constraint level | 0xFF(lengthSizeMinusOne) numSPS
        [sps_len_be16 sps]*  numPPS  [pps_len_be16 pps]* */
static void parse_avcc(h264_encoder_t *e, const unsigned char *d, int size)
{
    if (size < 8 || d[0] != 1) return;
    int i = 5;
    int num_sps = d[i++] & 0x1F;
    for (int s = 0; s < num_sps && i + 2 <= size; s++) {
        int len = (d[i] << 8) | d[i + 1]; i += 2;
        if (i + len > size) return;
        if (len <= (int)sizeof(e->sps)) { memcpy(e->sps, d + i, (size_t)len); e->sps_len = (unsigned int)len; }
        i += len;
    }
    if (i < size) {
        int num_pps = d[i++] & 0x1F;
        for (int s = 0; s < num_pps && i + 2 <= size; s++) {
            int len = (d[i] << 8) | d[i + 1]; i += 2;
            if (i + len > size) return;
            if (len <= (int)sizeof(e->pps)) { memcpy(e->pps, d + i, (size_t)len); e->pps_len = (unsigned int)len; }
            i += len;
        }
    }
}

/* FFmpeg packet → Annex-B（00 00 00 01 + NAL）。返回 malloc 缓冲，*out_len 为实际长度。
   自动识别包内格式：
   - 已含起始码（MPP 编码器原生输出，h264e_slice.c/h264e_sps.c 写 00 00 00 01）
     → 原样拷贝；
   - AVCC 4 字节大端长度前缀（个别后端）→ 逐 NAL 加起始码。 */
static unsigned char *avcc_to_annexb(const unsigned char *d, int size, size_t *out_len)
{
    *out_len = 0;
    if (size <= 0) return NULL;

    /* 仅当起始码后的 NAL 头合法（forbidden_zero_bit=0）才认 Annex-B：
       否则 AVCC 首 NAL 长度前缀恰为 00 00 01 xx 时会被误判 */
    int has_sc =
        (size >= 5 && d[0] == 0 && d[1] == 0 && d[2] == 0 && d[3] == 1 && !(d[4] & 0x80)) ||
        (size >= 4 && d[0] == 0 && d[1] == 0 && d[2] == 1 && !(d[3] & 0x80));
    if (has_sc) {
        unsigned char *out = malloc((size_t)size);
        if (!out) return NULL;
        memcpy(out, d, (size_t)size);
        *out_len = (size_t)size;
        return out;
    }

    size_t total = 0, i = 0;
    while (i + 4 <= (size_t)size) {
        uint32_t len = ((uint32_t)d[i] << 24) | ((uint32_t)d[i + 1] << 16) |
                       ((uint32_t)d[i + 2] << 8) | d[i + 3];
        if (len == 0 || i + 4 + len > (size_t)size) break;
        total += 4 + len;
        i += 4 + len;
    }
    if (total == 0) return NULL;

    unsigned char *out = malloc(total);
    if (!out) return NULL;
    size_t o = 0; i = 0;
    while (i + 4 <= (size_t)size) {
        uint32_t len = ((uint32_t)d[i] << 24) | ((uint32_t)d[i + 1] << 16) |
                       ((uint32_t)d[i + 2] << 8) | d[i + 3];
        if (len == 0 || i + 4 + len > (size_t)size) break;
        out[o++] = 0; out[o++] = 0; out[o++] = 0; out[o++] = 1;
        memcpy(out + o, d + i + 4, len);
        o += len;
        i += 4 + len;
    }
    *out_len = o;
    return out;
}

/* 打开 FFmpeg h264_rkmpp 编码器；成功返回 0（e->using_ff=1） */
static int ff_encoder_open(h264_encoder_t *e, int fps, int bitrate_bps)
{
    const AVCodec *codec = avcodec_find_encoder_by_name("h264_rkmpp");
    if (!codec) return -1;                       /* 当前 FFmpeg 未编译 rkmpp */

    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (!ctx) return -1;
    ctx->width       = e->w;
    ctx->height      = e->h;
    ctx->pix_fmt     = AV_PIX_FMT_NV12;
    ctx->time_base   = (AVRational){ 1, fps > 0 ? fps : 30 };
    ctx->framerate   = (AVRational){ fps > 0 ? fps : 30, 1 };
    ctx->bit_rate    = bitrate_bps;
    ctx->gop_size    = (fps > 0 ? fps : 30) * 2;  /* GOP=2s */
    ctx->max_b_frames = 0;
    ctx->thread_count = 1;
    ctx->flags      |= AV_CODEC_FLAG_LOW_DELAY;
    /* 不用 GLOBAL_HEADER：SPS/PPS 随 IDR 内嵌输出，rec_mp4 封装从码流
       扫描 SPS/PPS 写 avcC（与 V4L2 后端一致）；否则 avcC 会缺 SPS/PPS */

    if (avcodec_open2(ctx, codec, NULL) < 0) {
        avcodec_free_context(&ctx);
        return -1;
    }

    if (ctx->extradata && ctx->extradata_size > 0)
        parse_avcc(e, ctx->extradata, ctx->extradata_size);

    e->frame = av_frame_alloc();
    e->pkt   = av_packet_alloc();
    if (!e->frame || !e->pkt) {
        if (e->frame) av_frame_free(&e->frame);
        if (e->pkt)   av_packet_free(&e->pkt);
        avcodec_free_context(&ctx);
        return -1;
    }
    e->frame->format     = AV_PIX_FMT_NV12;
    e->frame->width      = e->w;
    e->frame->height     = e->h;
    e->frame->linesize[0] = e->w;
    e->frame->linesize[1] = e->w;

    e->avctx = ctx;
    e->pts = 0;
    e->using_ff = 1;
    return 0;
}

#endif /* HAVE_AVCODEC */

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

#ifdef HAVE_AVCODEC
    /* 优先 FFmpeg h264_rkmpp（Rockchip MPP 硬件编码） */
    if (ff_encoder_open(e, fps, bitrate_bps) == 0) {
        LOG_INFO("h264: FFmpeg rkmpp encoder ready %dx%d fps=%d bitrate=%d",
                 w, h, fps, bitrate_bps);
        return e;
    }
#endif

    char dev_path[64];
    if (h264_encoder_probe(dev_path, sizeof(dev_path)) != 0) {
        if (errlog_spam_ok())
            LOG_ERROR("h264: no V4L2 encoder node (tried %s and /dev/video0-15)", ENC_DEV);
        free(e);
        return NULL;
    }

    e->fd = open(dev_path, O_RDWR | O_NONBLOCK, 0);
    if (e->fd < 0) {
        if (errlog_spam_ok())
            LOG_ERROR("h264: open %s failed: %s", dev_path, strerror(errno));
        free(e);
        return NULL;
    }

    struct v4l2_capability cap;
    if (ioctl(e->fd, VIDIOC_QUERYCAP, &cap) < 0) {
        if (errlog_spam_ok())
            LOG_ERROR("h264: QUERYCAP failed on %s: %s", dev_path, strerror(errno));
        goto fail;
    }
    /* Rockchip 编码器固件差异：有的只在 device_caps 报 M2M，有的仅报 M2M 不带 MPLANE，
       统一按 device_caps 优先，并放宽容忍输出/捕获 MPLANE 组合 */
    uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS)
                        ? cap.device_caps : cap.capabilities;
    if (!(caps & (V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_VIDEO_M2M |
                  V4L2_CAP_VIDEO_OUTPUT_MPLANE | V4L2_CAP_VIDEO_CAPTURE_MPLANE))) {
        if (errlog_spam_ok())
            LOG_ERROR("h264: %s not M2M (caps=0x%x device_caps=0x%x)",
                      dev_path, cap.capabilities, cap.device_caps);
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

#ifdef HAVE_AVCODEC
    if (e->using_ff) {
        /* 冲刷编码器：NULL 帧触发 flush，收走残余包（录制已结束，丢弃），
           避免 MPP 硬件编码器留未输出帧 */
        avcodec_send_frame(e->avctx, NULL);
        while (avcodec_receive_packet(e->avctx, e->pkt) == 0)
            av_packet_unref(e->pkt);
        av_packet_free(&e->pkt);
        avcodec_free_context(&e->avctx);
        if (e->frame) av_frame_free(&e->frame);
        free(e);
        return;
    }
#endif

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

/* 逐行拷贝 NV12 到 output 缓冲（bytesperline 可能大于宽度，逐行复制最稳）。
   @idx 为 VIDIOC_DQBUF 归还的缓冲索引：必须写回该缓冲，不能固定 [0]。 */
static void nv12_copy(h264_encoder_t *e, const unsigned char *nv12, int idx)
{
    int w = e->w, h = e->h;
    unsigned char *dst = e->buf_in[idx];
    for (int y = 0; y < h; y++) {
        memcpy(dst + (size_t)y * w, nv12 + (size_t)y * w, (size_t)w);
    }
    const unsigned char *uv = nv12 + (size_t)w * h;
    unsigned char *dut = dst + (size_t)w * h;
    for (int y = 0; y < h / 2; y++) {
        memcpy(dut + (size_t)y * w, uv + (size_t)y * w, (size_t)w);
    }
}

/* 从 Annex-B 码流单遍提取 SPS/PPS、判断关键帧（含 IDR）与 VCL 存在性
   （type 1 非 IDR slice / type 5 IDR）。@has_vcl 可为 NULL（不关心时） */
static void h264_scan(h264_encoder_t *e, const unsigned char *d, size_t len,
                      int *keyframe, int *has_vcl)
{
    if (has_vcl) *has_vcl = 0;
    long pos = nal_find(d, len, 0);
    while (pos >= 0 && (size_t)pos < len) {
        unsigned char type = d[pos] & 0x1F;
        long next = nal_find(d, len, pos + 1);
        size_t nlen = (next < 0) ? (len - (size_t)pos) : (size_t)(next - pos);
        /* next 指向下一 NAL 头，[pos, next) 内含其后置起始码字节（3-4 个 0x00…01），
           必须裁掉，否则 avcC 里的 SPS/PPS 末尾带起始码 → MP4 无法解码。
           仅当确实找到下一 NAL（next >= 0）才裁：末段 NAL 无后置起始码，
           裁剪会误删其合法尾字节（如 rbsp 停止位恰好落在最低位的 0x01） */
        if (next >= 0) {
            while (nlen > 1 && d[pos + nlen - 1] == 0) nlen--;
            if (nlen > 1 && d[pos + nlen - 1] == 1) nlen--;
        }
        if (type == 7 && nlen >= 4 && nlen <= sizeof(e->sps)) {       /* SPS */
            memcpy(e->sps, d + pos, nlen);
            e->sps_len = (unsigned int)nlen;
        } else if (type == 8 && nlen >= 4 && nlen <= sizeof(e->pps)) { /* PPS */
            memcpy(e->pps, d + pos, nlen);
            e->pps_len = (unsigned int)nlen;
        } else if (type == 5) {
            *keyframe = 1;
            if (has_vcl) *has_vcl = 1;
        } else if (type == 1 && has_vcl) {
            *has_vcl = 1;
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

#ifdef HAVE_AVCODEC
    if (e->using_ff) {
        e->frame->data[0] = (unsigned char *)nv12;
        e->frame->data[1] = (unsigned char *)nv12 + (size_t)e->w * e->h;
        e->frame->pts = e->pts++;
        int rc = avcodec_send_frame(e->avctx, e->frame);
        if (rc == AVERROR(EAGAIN)) {
            /* 编码器积压：先收走已产出的包，再重发一次 */
            av_packet_unref(e->pkt);
            avcodec_receive_packet(e->avctx, e->pkt);
            rc = avcodec_send_frame(e->avctx, e->frame);
        }
        if (rc < 0) {
            LOG_ERROR("h264: avcodec_send_frame failed: %s", av_err2str(rc));
            return -1;
        }
        av_packet_unref(e->pkt);
        rc = avcodec_receive_packet(e->avctx, e->pkt);
        if (rc < 0) {
            LOG_ERROR("h264: avcodec_receive_packet failed (%s)",
                      rc == AVERROR(EAGAIN) ? "EAGAIN, no packet yet" : av_err2str(rc));
            return -1;
        }
        /* rkmpp 原生输出 Annex-B（含起始码）；若是 AVCC 前缀则自动转换 */
        size_t alen = 0;
        unsigned char *copy = avcc_to_annexb(e->pkt->data, e->pkt->size, &alen);
        int kf = (e->pkt->flags & AV_PKT_FLAG_KEY) ? 1 : 0;
        if (!copy) {
            LOG_ERROR("h264: rkmpp packet malformed");
            return -1;
        }
        /* 与 V4L2 后端一致：从输出码流补扫内嵌 SPS/PPS 与 IDR */
        h264_scan(e, copy, alen, &kf, NULL);
        *out = copy;
        *out_len = alen;
        if (keyframe) *keyframe = kf;
        return 0;
    }
#endif

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
    nv12_copy(e, nv12, (int)ob.index);
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

        /* 先扫描/拷贝再归还缓冲：归还后驱动可能立即填充覆盖。
           单遍扫描：提取 SPS/PPS、识别 IDR、检测 VCL（type 1/5）。
           纯 SPS/PPS 配置帧（无 VCL）丢弃并继续等 VCL 帧；
           含非 IDR slice 的帧接受，但不算关键帧 */
        unsigned char *data = e->buf_out[cb.index];
        size_t len = (size_t)cp.bytesused;

        int kf = 0, has_vcl = 0;
        h264_scan(e, data, len, &kf, &has_vcl);
        if (!has_vcl) {
            ioctl(e->fd, VIDIOC_QBUF, &cb);   /* 纯配置帧：归还后丢弃 */
            continue;
        }

        unsigned char *copy = malloc(len);
        if (!copy) {
            ioctl(e->fd, VIDIOC_QBUF, &cb);
            LOG_ERROR("h264: out alloc failed");
            return -1;
        }
        memcpy(copy, data, len);
        ioctl(e->fd, VIDIOC_QBUF, &cb);   /* 拷贝完成后再归还 */
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
