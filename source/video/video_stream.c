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
#include <pthread.h>
#include "video/video_stream.h"
#include "core/log.h"
#include "core/common.h"
#include "watchdog/watchdog.h"

typedef struct { unsigned char *data; size_t len; } frame_t;

typedef struct {
    _Atomic(frame_t *) frame_obj;
    _Atomic(int)       seq;
    _Atomic(int)       worker_done;  /* worker 退出标志 */
    _Atomic(int)       restart_req;  /* 请求 worker 用新参数重新初始化设备 */
    int                running;
    pthread_mutex_t    cfg_mutex;    /* 保护 device / width / height */
    pthread_mutex_t    frame_mutex;  /* 保护 frame_obj 指针生命周期（防 use-after-free） */
    char               device[128];
    int                width, height;
    int                fd;
    struct { void *start; size_t length; } *buffers;
    int                nbuffers;
    app_ctx_t         *app;          /* 运行上下文（读 running / cfg） */
} video_ctx_t;

static video_ctx_t *g_ctx = NULL;

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
            log_error("video_stream: open %s failed: %s", vs->device, strerror(errno));
        return -1;
    }
    watchdog_feed_self("video");

    struct v4l2_capability cap;
    if (ioctl(vs->fd, VIDIOC_QUERYCAP, &cap) < 0) {
        log_error("video_stream: VIDIOC_QUERYCAP failed");
        goto fail_close;
    }
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        log_error("video_stream: device %s is not video capture", vs->device);
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
            log_error("video_stream: S_FMT MJPG+YUYV failed, using driver default");
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
    log_info("video_stream: fmt=%c%c%c%c %dx%d",
        (char)(fmt.fmt.pix.pixelformat & 0xFF),
        (char)((fmt.fmt.pix.pixelformat >> 8) & 0xFF),
        (char)((fmt.fmt.pix.pixelformat >> 16) & 0xFF),
        (char)((fmt.fmt.pix.pixelformat >> 24) & 0xFF),
        vs->width, vs->height);

    struct v4l2_requestbuffers req = {0};
    req.count  = 4;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(vs->fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) {
        log_error("video_stream: VIDIOC_REQBUFS failed");
        goto fail_close;
    }
    watchdog_feed_self("video");

    vs->buffers = calloc(req.count, sizeof(*vs->buffers));
    if (!vs->buffers) {
        log_error("video_stream: calloc buffers failed");
        goto fail_close;
    }
    vs->nbuffers = req.count;

    for (int i = 0; i < vs->nbuffers; i++) {
        struct v4l2_buffer buf = {0};
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        if (ioctl(vs->fd, VIDIOC_QUERYBUF, &buf) < 0) {
            log_error("video_stream: VIDIOC_QUERYBUF failed");
            goto fail_buffers;
        }
        vs->buffers[i].length = buf.length;
        vs->buffers[i].start  = mmap(NULL, buf.length,
                                     PROT_READ | PROT_WRITE,
                                     MAP_SHARED, vs->fd, buf.m.offset);
        if (vs->buffers[i].start == MAP_FAILED) {
            log_error("video_stream: mmap failed");
            goto fail_buffers;
        }
        if (ioctl(vs->fd, VIDIOC_QBUF, &buf) < 0) {
            log_error("video_stream: VIDIOC_QBUF failed");
            goto fail_buffers;
        }
        watchdog_feed_self("video");
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(vs->fd, VIDIOC_STREAMON, &type) < 0) {
        log_error("video_stream: VIDIOC_STREAMON failed");
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
    pthread_mutex_init(&vs->frame_mutex, NULL);
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
    video_ctx_t *vs = g_ctx;
    vs->running = 0;
    pthread_mutex_lock(&vs->frame_mutex);
    frame_t *old = __atomic_exchange_n(&vs->frame_obj, NULL, __ATOMIC_SEQ_CST);
    if (old) { free(old->data); free(old); }
    pthread_mutex_unlock(&vs->frame_mutex);
    __atomic_store_n(&vs->seq, 0, __ATOMIC_SEQ_CST);
}

void *video_stream_task(void *arg)
{
    (void)arg;
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
                log_error("video_stream: device init failed, entering idle retry loop (watchdog still fed)");
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
            log_error("video_stream: epoll_create1 failed");
            deinit_video_device(vs);
            __atomic_store_n(&vs->restart_req, 0, __ATOMIC_RELEASE);
            continue;
        }
        struct epoll_event vev = { .events = EPOLLIN | EPOLLERR, .data.fd = vs->fd };
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, vs->fd, &vev) < 0) {
            log_error("video_stream: epoll_ctl add device failed");
            close(epfd);
            deinit_video_device(vs);
            __atomic_store_n(&vs->restart_req, 0, __ATOMIC_RELEASE);
            continue;
        }
        log_info("video_stream: capturing from %s", vs->device);

        while (vs->app->running && !__atomic_load_n(&vs->restart_req, __ATOMIC_ACQUIRE)) {
            struct epoll_event out;
            int ret = epoll_wait(epfd, &out, 1, 500);
            if (ret < 0) {
                if (errno == EINTR) { watchdog_feed_self("video"); continue; }
                log_error("video_stream: epoll_wait failed");
                break;
            }
            /* 无论是否有帧都保持喂狗：相机空闲/无数据时线程仍存活，避免误判卡死 */
            watchdog_feed_self("video");
            if (ret == 0) continue;
            if (!(out.events & EPOLLIN)) continue;

            struct v4l2_buffer buf = {0};
            buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            if (ioctl(vs->fd, VIDIOC_DQBUF, &buf) < 0) {
                if (errno == EAGAIN || errno == EINTR) continue;
                log_error("video_stream: VIDIOC_DQBUF failed");
                break;
            }

            if (buf.bytesused > 0 && buf.index < vs->nbuffers) {
                frame_t *f = malloc(sizeof(frame_t));
                if (f) {
                    f->len  = buf.bytesused;
                    f->data = malloc(f->len);
                    if (f->data)
                        memcpy(f->data, vs->buffers[buf.index].start, f->len);
                    else { free(f); f = NULL; }
                }
                if (f) {
                    /* 持锁替换旧帧：防止推流线程正在复制时旧帧被释放 */
                    pthread_mutex_lock(&vs->frame_mutex);
                    frame_t *old = __atomic_exchange_n(&vs->frame_obj, f, __ATOMIC_SEQ_CST);
                    if (old) { free(old->data); free(old); }
                    pthread_mutex_unlock(&vs->frame_mutex);
                    __atomic_fetch_add(&vs->seq, 1, __ATOMIC_SEQ_CST);
                    watchdog_feed_self("video");
                }
            }

            if (ioctl(vs->fd, VIDIOC_QBUF, &buf) < 0) {
                log_error("video_stream: VIDIOC_QBUF failed");
                break;
            }
        }

        close(epfd);
        deinit_video_device(vs);
        __atomic_store_n(&vs->restart_req, 0, __ATOMIC_RELEASE);
    }

    log_info("video_stream: worker exiting");
    return NULL;
}

int video_stream_wait_next(int last_seq, unsigned char **out, size_t *out_len)
{
    video_ctx_t *vs = g_ctx;
    if (!vs) return -1;

    while (__atomic_load_n(&vs->seq, __ATOMIC_SEQ_CST) == last_seq) {
        if (!vs->app->running || !vs->running) return -1;
        usleep(20000);
    }
    int seq = __atomic_load_n(&vs->seq, __ATOMIC_SEQ_CST);
    /* 持锁读取帧并复制：防止 worker 在复制期间释放该帧 */
    pthread_mutex_lock(&vs->frame_mutex);
    frame_t *f = __atomic_load_n(&vs->frame_obj, __ATOMIC_SEQ_CST);
    if (!f) { pthread_mutex_unlock(&vs->frame_mutex); return seq; }

    unsigned char *copy = malloc(f->len);
    if (!copy) {
        log_error("video_stream: frame alloc failed (%zu bytes)", f->len);
        pthread_mutex_unlock(&vs->frame_mutex);
        *out     = NULL;
        *out_len = 0;
        return seq;   /* 调用方应跳过该帧，避免 write(NULL) */
    }
    memcpy(copy, f->data, f->len);
    pthread_mutex_unlock(&vs->frame_mutex);
    *out     = copy;
    *out_len = f->len;
    return seq;
}

int video_stream_is_running(void)
{
    return g_ctx ? g_ctx->running : 0;
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
    log_info("video_stream: restart requested (%s %dx%d)",
             vs->device, vs->width, vs->height);
}

/* ---- MJPEG 推流线程（每连接一个，detached，由 video 模块创建） ---- */

typedef struct {
    int fd;
    video_stream_client_close_cb on_close;
} video_client_ctx_t;

static void *video_stream_client_task(void *arg)
{
    video_client_ctx_t *a = (video_client_ctx_t *)arg;
    int fd = a->fd;
    video_stream_client_close_cb on_close = a->on_close;
    free(a);

    const char *hdr = "HTTP/1.1 200 OK\r\n"
                      "Content-Type: multipart/x-mixed-replace; boundary=--frame\r\n"
                      "Connection: close\r\n\r\n";
    write(fd, hdr, strlen(hdr));
    int last = 0;
    while (g_ctx && g_ctx->app->running) {
        /* 推流线程基于非阻塞写 + 轮询，不会长期阻塞，无需独立看门狗监督 */
        unsigned char *frame = NULL; size_t flen = 0;
        int seq = video_stream_wait_next(last, &frame, &flen);
        if (seq < 0) break;
        if (seq == last) { usleep(20000); continue; }
        last = seq;
        if (!frame || flen == 0) { usleep(20000); continue; }   /* 分配失败等：跳过 */
        char mhdr[128];
        int hlen = snprintf(mhdr, sizeof(mhdr),
                            "--frame\r\nContent-Type: image/jpeg\r\n"
                            "Content-Length: %zu\r\n\r\n", flen);
        if (write(fd, mhdr, hlen) < 0) { free(frame); break; }
        if (write(fd, frame, flen) < 0) { free(frame); break; }
        if (write(fd, "\r\n", 2) < 0) { free(frame); break; }
        free(frame);
    }
    if (on_close) on_close(fd);
    return NULL;
}

int video_stream_client_start(int fd, video_stream_client_close_cb on_close)
{
    video_client_ctx_t *a = malloc(sizeof(*a));
    if (!a) return -1;
    a->fd       = fd;
    a->on_close = on_close;
    pthread_t tid;
    if (pthread_create(&tid, NULL, video_stream_client_task, a) != 0) {
        free(a);
        return -1;
    }
    pthread_detach(tid);
    return 0;
}
