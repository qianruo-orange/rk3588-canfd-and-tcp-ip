#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stddef.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <errno.h>
#include <linux/videodev2.h>
#include <sys/epoll.h>
#include <poll.h>
#include <pthread.h>
#include "video/video_stream.h"
#include "video/frame_ring.h"
#include "core/log.h"
#include "core/common.h"
#include "core/cpu_affinity.h"
#include "core/epoll_util.h"
#include "watchdog/watchdog.h"
#include "ai/rknn_yolo.h"

/* 推流客户端空闲超时（秒）：客户端不读 / 网络黑洞时释放连接与线程 */
#define VIDEO_CLIENT_IDLE_TIMEOUT 30

typedef struct {
    _Atomic(int)       restart_req;  /* 请求 worker 用新参数重新初始化设备 */
    int                running;
    pthread_mutex_t    cfg_mutex;    /* 保护 device / width / height */
    frame_ring_t       ring;         /* 视频帧环形队列：采集侧零拷贝发布 */
    char               device[128];
    int                width, height;
    int                pixfmt;   /* 实际像素格式（V4L2_PIX_FMT_*） */
    int                fd;
    struct { void *start; size_t length; } *buffers;
    int                nbuffers;
    app_ctx_t         *app;          /* 运行上下文（读 running / cfg） */
} video_ctx_t;

static video_ctx_t *g_ctx = NULL;

/* 延迟 QBUF 释放回调：环在 raw 消费完成后归还驱动缓冲（零拷贝采集核心）。
   在环锁内调用（ioctl 微秒级），不得获取其他锁；持续失败时降频日志 */
static void raw_release_cb(void *arg, int vbuf_index)
{
    video_ctx_t *vs = (video_ctx_t *)arg;
    static int fail_cnt = 0;
    struct v4l2_buffer buf = {0};
    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index  = vbuf_index;
    if (ioctl(vs->fd, VIDIOC_QBUF, &buf) < 0 && (fail_cnt++ % 100) == 0)
        LOG_ERROR("video_stream: deferred QBUF failed: %s", strerror(errno));
}

/* ---- 内部函数 ---- */

static int init_video_device(video_ctx_t *vs, int verbose)
{
    /* 设备初始化期间持续喂狗：部分 V4L2 驱动的 open/ioctl 可能较慢，
       整段初始化若耗时过长会被看门狗误判为 video 线程卡死。 */
    watchdog_feed_self("video");

    vs->fd = open(vs->device, O_RDWR | O_NONBLOCK, 0);
    if (vs->fd < 0) {
        /* 相机未连接是常见场景：重试时静默，仅在状态切换时详细记录，避免刷屏 */
        if (verbose)
            LOG_ERROR("video_stream: open %s failed: %s", vs->device, strerror(errno));
        return -1;
    }
    watchdog_feed_self("video");

    struct v4l2_capability cap;
    if (ioctl(vs->fd, VIDIOC_QUERYCAP, &cap) < 0) {
        LOG_ERROR("video_stream: VIDIOC_QUERYCAP failed");
        goto fail_close;
    }
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        LOG_ERROR("video_stream: device %s is not video capture", vs->device);
        goto fail_close;
    }
    watchdog_feed_self("video");

    struct v4l2_format fmt = {0};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width       = vs->width;
    fmt.fmt.pix.height      = vs->height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field       = V4L2_FIELD_ANY;

    if (ioctl(vs->fd, VIDIOC_S_FMT, &fmt) < 0) {
        /* MJPEG 失败，尝试 YUYV */
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        if (ioctl(vs->fd, VIDIOC_S_FMT, &fmt) < 0) {
            /* 仍失败则用 G_FMT 取驱动默认值 */
            LOG_ERROR("video_stream: S_FMT MJPG+YUYV failed, using driver default");
            memset(&fmt, 0, sizeof(fmt));
            fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            if (ioctl(vs->fd, VIDIOC_G_FMT, &fmt) < 0)
                goto fail_close;
        }
    }
    watchdog_feed_self("video");

    /* 记录实际使用的参数 */
    vs->width  = fmt.fmt.pix.width;
    vs->height = fmt.fmt.pix.height;
    vs->pixfmt = (int)fmt.fmt.pix.pixelformat;
    LOG_INFO("video_stream: fmt=%c%c%c%c %dx%d",
        (char)(fmt.fmt.pix.pixelformat & 0xFF),
        (char)((fmt.fmt.pix.pixelformat >> 8) & 0xFF),
        (char)((fmt.fmt.pix.pixelformat >> 16) & 0xFF),
        (char)((fmt.fmt.pix.pixelformat >> 24) & 0xFF),
        vs->width, vs->height);

    /* 帧率：配置 video_fps > 0 时设置驱动帧间隔（S_PARM），否则保持驱动默认 */
    if (vs->app && vs->app->cfg && vs->app->cfg->video_fps > 0) {
        struct v4l2_streamparm sp;
        memset(&sp, 0, sizeof(sp));
        sp.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        sp.parm.capture.timeperframe.numerator = 1;
        sp.parm.capture.timeperframe.denominator = vs->app->cfg->video_fps;
        if (ioctl(vs->fd, VIDIOC_S_PARM, &sp) == 0) {
            LOG_INFO("video_stream: fps set to %d", vs->app->cfg->video_fps);
        } else {
            LOG_ERROR("video_stream: S_PARM fps=%d failed, keeping driver default",
                      vs->app->cfg->video_fps);
        }
    }

    struct v4l2_requestbuffers req = {0};
    /* 8 个驱动缓冲：零拷贝延迟 QBUF 模式下，缓冲占用 = 推理解码在途 +
       最新帧钉住 + 帧间隔，4 个缓冲不足会导致驱动侧静默丢帧 */
    req.count  = 8;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(vs->fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) {
        LOG_ERROR("video_stream: VIDIOC_REQBUFS failed");
        goto fail_close;
    }
    watchdog_feed_self("video");

    vs->buffers = calloc(req.count, sizeof(*vs->buffers));
    if (!vs->buffers) {
        LOG_ERROR("video_stream: calloc buffers failed");
        goto fail_close;
    }
    vs->nbuffers = req.count;

    for (int i = 0; i < vs->nbuffers; i++) {
        struct v4l2_buffer buf = {0};
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        if (ioctl(vs->fd, VIDIOC_QUERYBUF, &buf) < 0) {
            LOG_ERROR("video_stream: VIDIOC_QUERYBUF failed");
            goto fail_buffers;
        }
        vs->buffers[i].length = buf.length;
        vs->buffers[i].start  = mmap(NULL, buf.length,
                                     PROT_READ | PROT_WRITE,
                                     MAP_SHARED, vs->fd, buf.m.offset);
        if (vs->buffers[i].start == MAP_FAILED) {
            LOG_ERROR("video_stream: mmap failed");
            goto fail_buffers;
        }
        if (ioctl(vs->fd, VIDIOC_QBUF, &buf) < 0) {
            LOG_ERROR("video_stream: VIDIOC_QBUF failed");
            goto fail_buffers;
        }
        watchdog_feed_self("video");
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(vs->fd, VIDIOC_STREAMON, &type) < 0) {
        LOG_ERROR("video_stream: VIDIOC_STREAMON failed");
        goto fail_buffers;
    }
    watchdog_feed_self("video");
    return 0;

fail_buffers:
    for (int i = 0; i < vs->nbuffers; i++)
        if (vs->buffers[i].start && vs->buffers[i].start != MAP_FAILED)
            munmap(vs->buffers[i].start, vs->buffers[i].length);
    free(vs->buffers);
    vs->buffers  = NULL;
    vs->nbuffers = 0;
fail_close:
    close(vs->fd);
    vs->fd = -1;
    return -1;
}

static void deinit_video_device(video_ctx_t *vs)
{
    if (vs->fd < 0) return;
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(vs->fd, VIDIOC_STREAMOFF, &type);
    for (int i = 0; i < vs->nbuffers; i++)
        if (vs->buffers[i].start && vs->buffers[i].start != MAP_FAILED)
            munmap(vs->buffers[i].start, vs->buffers[i].length);
    free(vs->buffers);
    vs->buffers  = NULL;
    vs->nbuffers = 0;
    close(vs->fd);
    vs->fd = -1;
}

/* ---- 公共接口 ---- */

int video_stream_init(void *arg)
{
    app_ctx_t *app = (app_ctx_t *)arg;

    video_ctx_t *vs = calloc(1, sizeof(*vs));
    if (!vs) return -1;
    pthread_mutex_init(&vs->cfg_mutex, NULL);
    frame_ring_init(&vs->ring);
    frame_ring_set_raw_release_cb(&vs->ring, raw_release_cb, vs);
    snprintf(vs->device, sizeof(vs->device), "%s", "/dev/video0");
    vs->width  = 640;
    vs->height = 480;
    vs->fd     = -1;
    vs->app    = app;

    if (app->cfg->video_device[0])
        snprintf(vs->device, sizeof(vs->device), "%s", app->cfg->video_device);
    if (app->cfg->video_width  > 0) vs->width  = app->cfg->video_width;
    if (app->cfg->video_height > 0) vs->height = app->cfg->video_height;
    vs->running = 1;
    g_ctx = vs;
    return 0;
}

void video_stream_shutdown(void *arg)
{
    (void)arg;
    if (!g_ctx) return;
    /* 仅置停止标志：环形队列随进程退出（推流客户端线程为 detached，
       可能仍在退出检查前短暂访问环，不能在此销毁） */
    g_ctx->running = 0;
}

void *video_stream_task(void *arg)
{
    (void)arg;
    cpu_bind_big();
    video_ctx_t *vs = g_ctx;
    if (!vs) return NULL;

    int failed_before = 0;   /* 连续失败标记：只在状态切换时输出详细错误 */

    while (vs->app->running) {
        /* 会话开始：初始化设备（参数变更由 restart_req 触发重新进入本循环） */
        pthread_mutex_lock(&vs->cfg_mutex);
        int ok = (init_video_device(vs, !failed_before) == 0);
        pthread_mutex_unlock(&vs->cfg_mutex);

        if (!ok) {
            if (!failed_before) {
                LOG_ERROR("video_stream: device init failed, entering idle retry loop (watchdog still fed)");
                failed_before = 1;
            }
            /* 相机未连接：线程保持存活并持续喂狗，程序正常运行；
               每 5 秒重试一次初始化，便于热插拔后自动恢复 */
            int idle_ticks = 0;
            while (vs->app->running && !__atomic_load_n(&vs->restart_req, __ATOMIC_ACQUIRE)) {
                usleep(500000);
                watchdog_feed_self("video");
                if (++idle_ticks >= 10) break;
            }
            if (__atomic_load_n(&vs->restart_req, __ATOMIC_ACQUIRE))
                failed_before = 0;   /* 配置变更触发：下次失败重新详细记录 */
            __atomic_store_n(&vs->restart_req, 0, __ATOMIC_RELEASE);
            continue;
        }
        failed_before = 0;

        int epfd = epoll_create1(0);
        if (epfd < 0) {
            LOG_ERROR("video_stream: epoll_create1 failed");
            deinit_video_device(vs);
            __atomic_store_n(&vs->restart_req, 0, __ATOMIC_RELEASE);
            continue;
        }
        struct epoll_event vev = { .events = EPOLLIN | EPOLLERR, .data.fd = vs->fd };
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, vs->fd, &vev) < 0) {
            LOG_ERROR("video_stream: epoll_ctl add device failed");
            close(epfd);
            deinit_video_device(vs);
            __atomic_store_n(&vs->restart_req, 0, __ATOMIC_RELEASE);
            continue;
        }
        LOG_INFO("video_stream: capturing from %s", vs->device);

        unsigned int last_vseq = 0;
        int have_vseq = 0;   /* 驱动侧丢帧统计（buf.sequence 缺口；会话级状态） */
        while (vs->app->running && !__atomic_load_n(&vs->restart_req, __ATOMIC_ACQUIRE)) {
            struct epoll_event out;
            /* epoll_wait_feed：EINTR 重试并喂狗；无论是否有帧都保持喂狗
               （相机空闲/无数据时线程仍存活，避免误判卡死） */
            int ret = epoll_wait_feed(epfd, &out, 1, 500, "video");
            if (ret < 0) break;
            if (ret == 0) continue;
            if (!(out.events & EPOLLIN)) continue;

            struct v4l2_buffer buf = {0};
            buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            if (ioctl(vs->fd, VIDIOC_DQBUF, &buf) < 0) {
                if (errno == EAGAIN || errno == EINTR) continue;
                LOG_ERROR("video_stream: VIDIOC_DQBUF failed");
                break;
            }

            /* 驱动侧丢帧：相邻 buf.sequence 的缺口（u32 回绕安全）。
               部分驱动不填 sequence（恒 0）时不计数 */
            if (have_vseq) {
                unsigned int gap = buf.sequence - last_vseq;
                if (gap > 1) {
                    frame_ring_lock(&vs->ring);
                    frame_ring_add_driver_dropped_locked(&vs->ring, gap - 1);
                    frame_ring_unlock(&vs->ring);
                }
            }
            last_vseq = buf.sequence;
            have_vseq = 1;

            if (buf.bytesused == 0 || buf.index >= vs->nbuffers) {
                /* 空帧：立即归还，不进环 */
                if (ioctl(vs->fd, VIDIOC_QBUF, &buf) < 0) {
                    LOG_ERROR("video_stream: VIDIOC_QBUF failed");
                    break;
                }
                continue;
            }

            /* 零拷贝入环：mmap 指针直接进槽（不 memcpy），消费完成前不 QBUF。
               环无空闲槽（消费严重滞后）时丢弃本帧并立即归还驱动缓冲 */
            frame_ring_lock(&vs->ring);
            frame_slot_t *s = frame_ring_produce_slot_locked(&vs->ring);
            if (!s) {
                frame_ring_produce_drop_locked(&vs->ring);
                frame_ring_unlock(&vs->ring);
                if (ioctl(vs->fd, VIDIOC_QBUF, &buf) < 0) {
                    LOG_ERROR("video_stream: VIDIOC_QBUF failed");
                    break;
                }
                continue;
            }
            s->raw.buf        = vs->buffers[buf.index].start;
            s->raw.len        = buf.bytesused;
            s->raw.cap        = vs->buffers[buf.index].length;
            s->raw_present    = 1;
            s->raw_is_mmap    = 1;
            s->raw_vbuf_index = buf.index;
            s->w = vs->width;
            s->h = vs->height;
            s->fmt = (vs->pixfmt == V4L2_PIX_FMT_MJPEG) ? FRAME_RING_FMT_MJPEG
                                                        : FRAME_RING_FMT_YUYV;
            frame_ring_produce_commit_locked(&vs->ring);
            frame_ring_unlock(&vs->ring);
            watchdog_feed_self("video");
        }

        /* 会话结束（重启/退出/错误）：退让等待锁外解码者释放引用（推理侧
           任务在途期间持有 claims），随后摘除全部 mmap 指针，
           STREAMOFF/munmap 才安全 */
        frame_ring_quiesce_begin(&vs->ring);
        int q_timeout = frame_ring_quiesce_wait(&vs->ring, 1000) != 0;
        if (q_timeout)
            LOG_ERROR("video_stream: ring quiesce timeout (claims not drained)");

        close(epfd);
        deinit_video_device(vs);
        /* 超时兜底：STREAMOFF/munmap 后 claim 持有者只剩推理 worker（读的是
           job rgb 而非 mmap），通常随即写槽摘除 claim——再等一次；仍不归零
           （推理线程卡死）则强制清空槽，迟到写槽/摘除走防御分支无副作用 */
        if (q_timeout && frame_ring_quiesce_wait(&vs->ring, 1000) != 0) {
            LOG_ERROR("video_stream: quiesce retry timeout, force clearing slots");
            frame_ring_quiesce_force_clear(&vs->ring);
        }
        __atomic_store_n(&vs->restart_req, 0, __ATOMIC_RELEASE);
    }

    LOG_INFO("video_stream: worker exiting");
    return NULL;
}

static int video_stream_wait_next(int last_seq, unsigned char **out, size_t *out_len)
{
    video_ctx_t *vs = g_ctx;
    if (!vs) return -1;
    unsigned long long last = (unsigned long long)(unsigned int)last_seq;

    /* 阻塞等新帧（环条件变量，帧到即醒；200ms 超时用于退出检查） */
    while (vs->app->running && vs->running) {
        if (frame_ring_wait_new(&vs->ring, last, 200)) break;
    }
    if (!vs->app->running || !vs->running) return -1;

    /* 持锁拷贝最新槽 raw：防止 worker 在复制期间释放该槽 */
    frame_ring_lock(&vs->ring);
    frame_slot_t *s = frame_ring_raw_newest_locked(&vs->ring);
    unsigned long long seq = s ? s->seq : last;
    if (s) {
        unsigned char *copy = malloc(s->raw.len);
        if (!copy) {
            LOG_ERROR("video_stream: frame alloc failed (%zu bytes)", s->raw.len);
            *out     = NULL;
            *out_len = 0;
        } else {
            memcpy(copy, s->raw.buf, s->raw.len);
            *out     = copy;
            *out_len = s->raw.len;
        }
    }
    frame_ring_unlock(&vs->ring);
    return (int)(unsigned int)seq;   /* 客户端 pacing 兼容 32 位序号 */
}

/* 拷贝当前最新采集帧（AI 推理线程消费用：取快照，不等待新帧） */
int video_stream_get_frame(unsigned char **out, size_t *out_len, int *fmt,
                           int *w, int *h, unsigned long long *seq)
{
    video_ctx_t *vs = g_ctx;
    if (!vs || !out || !out_len || !fmt || !w || !h || !seq) return -1;
    frame_ring_lock(&vs->ring);
    frame_slot_t *s = frame_ring_raw_newest_locked(&vs->ring);
    if (!s || !s->raw.buf) {
        frame_ring_unlock(&vs->ring);
        return -1;   /* 尚无采集帧 */
    }
    unsigned char *copy = malloc(s->raw.len);
    if (!copy) {
        frame_ring_unlock(&vs->ring);
        return -1;
    }
    memcpy(copy, s->raw.buf, s->raw.len);
    size_t len = s->raw.len;
    unsigned long long sseq = s->seq;
    frame_ring_unlock(&vs->ring);
    *out     = copy;
    *out_len = len;
    *fmt     = (vs->pixfmt == V4L2_PIX_FMT_MJPEG) ? VIDEO_FMT_MJPEG : VIDEO_FMT_YUYV;
    *w       = vs->width;
    *h       = vs->height;
    *seq     = sseq;
    return 0;
}

/* 无拷贝窥探最新帧序号：轮询方先判新帧，有新帧才整帧拷贝（见 video_stream.h） */
unsigned long long video_stream_get_frame_seq(void)
{
    video_ctx_t *vs = g_ctx;
    if (!vs) return 0;
    frame_ring_lock(&vs->ring);
    unsigned long long seq = vs->ring.produce_seq;
    frame_ring_unlock(&vs->ring);
    return seq;
}

/* 环形队列句柄：AI/录像消费方直接操作槽位（零拷贝路径），模块未初始化返回 NULL */
frame_ring_t *video_stream_get_ring(void)
{
    video_ctx_t *vs = g_ctx;
    return vs ? &vs->ring : NULL;
}

/* 运行时重启视频流（切换设备/分辨率，不重启进程） */
void video_stream_restart(void)
{
    video_ctx_t *vs = g_ctx;
    if (!vs) return;

    /* 更新参数（worker 加锁读取，避免与设备初始化并发） */
    pthread_mutex_lock(&vs->cfg_mutex);
    if (vs->app && vs->app->cfg) {
        if (vs->app->cfg->video_device[0])
            snprintf(vs->device, sizeof(vs->device), "%s", vs->app->cfg->video_device);
        if (vs->app->cfg->video_width > 0)
            vs->width = vs->app->cfg->video_width > 4096 ? 4096 : vs->app->cfg->video_width;
        if (vs->app->cfg->video_height > 0)
            vs->height = vs->app->cfg->video_height > 4096 ? 4096 : vs->app->cfg->video_height;
    }
    pthread_mutex_unlock(&vs->cfg_mutex);

    /* 请求 worker 用新参数重新初始化；旧设备由 worker 自行 deinit，
       避免 restart 与 worker 并发操作同一 fd 造成双重释放/崩溃 */
    __atomic_store_n(&vs->restart_req, 1, __ATOMIC_RELEASE);
    LOG_INFO("video_stream: restart requested (%s %dx%d)",
             vs->device, vs->width, vs->height);
}

/* ---- MJPEG 推流线程（每连接一个，detached，由 video 模块创建） ---- */
/* 完整写入非阻塞推流 socket 统一走 core/common.h 的 fd_write_all_blocking（处理
   EAGAIN/EWOULDBLOCK 与部分写入，避免慢客户端导致 MJPEG 流数据错位） */

typedef struct {
    int fd;
    int ai_mode;   /* 1: 画框流（AI 为必要流程：不可用时直接 503/断流，不回退原始帧） */
    video_stream_client_close_cb on_close;
} video_client_ctx_t;

static void *video_stream_client_task(void *arg)
{
    video_client_ctx_t *a = (video_client_ctx_t *)arg;
    int fd = a->fd;
    int ai_mode = a->ai_mode;
    video_stream_client_close_cb on_close = a->on_close;
    free(a);

    /* AI 为必要流程：画框流在 AI 不可用时直接报错（503），不回退原始帧 */
    if (ai_mode && !rknn_yolo_enabled()) {
        const char *err = "HTTP/1.1 503 Service Unavailable\r\n"
                          "Content-Type: text/plain; charset=utf-8\r\n"
                          "Connection: close\r\n\r\n"
                          "AI 推理不可用（无 AI）\n";
        fd_write_all_blocking(fd, err, strlen(err));
        if (on_close) on_close(fd);
        return NULL;
    }

    const char *hdr = "HTTP/1.1 200 OK\r\n"
                      "Content-Type: multipart/x-mixed-replace; boundary=--frame\r\n"
                      "Connection: close\r\n\r\n";
    fd_write_all_blocking(fd, hdr, strlen(hdr));
    int last_raw = 0;   /* 原始帧 pacing：seq 单调递增，wait_next 依赖此不变量 */
    unsigned long long last_ann = (unsigned long long)-1; /* 已推的标注帧 seq（-1 表示尚未推过） */
    time_t last_write = time(NULL);   /* 最近一次成功写帧的时间，用于空闲超时 */
    while (g_ctx && g_ctx->app->running) {
        /* 空闲超时：长时间未成功写出任何帧（客户端不读/网络黑洞），释放连接，
           避免推流线程与 fd 永久占用 */
        if (time(NULL) - last_write >= VIDEO_CLIENT_IDLE_TIMEOUT) break;

        /* 对端断开检测：非阻塞 MSG_PEEK，FIN 已到（返回 0）或连接异常即退出 */
        char peek;
        ssize_t pr = recv(fd, &peek, 1, MSG_PEEK | MSG_DONTWAIT);
        if (pr == 0) break;
        if (pr < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) break;

        /* 推流线程基于非阻塞写 + 轮询，不会长期阻塞，无需独立看门狗监督 */
        unsigned char *frame = NULL; size_t flen = 0;
        if (ai_mode) {
            /* AI 必要流程：运行期 AI 失效（热重载失败等）→ 直接断流报错，不回退原始帧 */
            if (!rknn_yolo_enabled()) break;
            /* 画框流：推流的每一帧都必须是推理标注后的画面。
               新标注帧未就绪（推理进行中 / 首帧未生成）时短暂等待，绝不混入原始帧，
               否则画面会在标注帧与原始帧之间来回闪烁（抖动）。
               标注帧用独立的 last_ann 去重：不能与原始帧共用 last，
               否则标注帧 seq（推理时刻，必然滞后于最新原始帧）会把 last 拉回过去，
               wait_next 永不阻塞，同一对帧被无限重复发送（死循环刷流）。 */
            unsigned long long aseq_now = rknn_yolo_get_frame_seq();
            if (aseq_now != last_ann) {
                unsigned long long aseq = 0;
                if (rknn_yolo_get_frame(&frame, &flen, &aseq) == 0)
                    last_ann = aseq;
            }
            if (!frame) { usleep(10000); continue; }   /* 等标注帧，不回退原始帧 */
        } else {
            int seq = video_stream_wait_next(last_raw, &frame, &flen);
            if (seq < 0) break;
            if (seq == last_raw) { usleep(20000); continue; }
            last_raw = seq;
        }
        if (!frame || flen == 0) { usleep(20000); continue; }   /* 分配失败等：跳过 */
        char mhdr[128];
        int hlen = snprintf(mhdr, sizeof(mhdr),
                            "--frame\r\nContent-Type: image/jpeg\r\n"
                            "Content-Length: %zu\r\n\r\n", flen);
        if (fd_write_all_blocking(fd, mhdr, (size_t)hlen) < 0) { free(frame); break; }
        if (fd_write_all_blocking(fd, frame, flen) < 0) { free(frame); break; }
        if (fd_write_all_blocking(fd, "\r\n", 2) < 0) { free(frame); break; }
        free(frame);
        last_write = time(NULL);
    }
    if (on_close) on_close(fd);
    return NULL;
}

int video_stream_client_start(int fd, video_stream_client_close_cb on_close)
{
    video_client_ctx_t *a = malloc(sizeof(*a));
    if (!a) return -1;
    a->fd       = fd;
    a->ai_mode  = 0;
    a->on_close = on_close;
    pthread_t tid;
    if (pthread_create(&tid, NULL, video_stream_client_task, a) != 0) {
        free(a);
        return -1;
    }
    pthread_detach(tid);
    return 0;
}

int video_stream_client_start_ai(int fd, video_stream_client_close_cb on_close)
{
    video_client_ctx_t *a = malloc(sizeof(*a));
    if (!a) return -1;
    a->fd       = fd;
    a->ai_mode  = 1;
    a->on_close = on_close;
    pthread_t tid;
    if (pthread_create(&tid, NULL, video_stream_client_task, a) != 0) {
        free(a);
        return -1;
    }
    pthread_detach(tid);
    return 0;
}
