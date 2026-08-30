/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * hevc_feed.c — 录制 H.265 编码输出扇出到命名管道（FIFO）。
 *
 * 数据流：录制线程 push_frame（所有权移交，队列满丢最旧）→ 写线程
 *   阻塞写 FIFO → ffmpeg 进程（systemd 管理）读 FIFO 推 RTSP →
 *   mediamtx WebRTC 桥 → 浏览器。
 *
 * 关键决策：
 *  - 队列吸收 ffmpeg/mediamtx 的短时背压，溢出丢最旧帧（直播语义）；
 *  - FIFO 打开/写入全部在独立线程：录制线程永不被管道阻塞；
 *  - ffmpeg 重启（systemd Restart=always）间隙写线程重开 FIFO，不丢会话。
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "video/hevc_feed.h"
#include "core/log.h"

#define FEED_FIFO     "/run/rk3588-edge-gateway/hevc.fifo"
#define FEED_DIR      "/run/rk3588-edge-gateway"
#define FEED_QUEUE    64u        /* ≈2s @30fps 的短时缓冲 */
#define FEED_MAX_FRAME (2u * 1024 * 1024)

typedef struct {
    unsigned char *data;   /* 所有权：录制线程移交，写线程写毕释放 */
    size_t len;
} feed_item_t;

static struct {
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    int      stop;
    int      fd;               /* FIFO 写端（-1 未开） */
    feed_item_t q[FEED_QUEUE];
    size_t   head, count;
    /* 参数集副本（关键帧前重插；录制线程更新，写线程读取——加锁下操作） */
    unsigned char vps[64], sps[64], pps[64];
    unsigned int  vps_len, sps_len, pps_len;
    unsigned long long pushed, dropped;
} g_feed;

/* 打开 FIFO 写端：阻塞等待 ffmpeg 读者（O_WRONLY 在无读者时挂起），
   可被 stop 唤醒提前放弃。返回 0 成功。 */
static int feed_open_fifo(void)
{
    while (!g_feed.stop) {
        int fd = open(FEED_FIFO, O_WRONLY | O_CLOEXEC);
        if (fd >= 0) {
            g_feed.fd = fd;
            return 0;
        }
        /* 仅对可重试错误等待；ENXIO/ENOENT 等则短暂退避 */
        if (errno == EINTR) continue;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;
        pthread_mutex_lock(&g_feed.lock);
        int rc = 0;
        if (!g_feed.stop)
            rc = pthread_cond_timedwait(&g_feed.cond, &g_feed.lock, &ts);
        pthread_mutex_unlock(&g_feed.lock);
        if (rc == ETIMEDOUT) continue;
        if (g_feed.stop) return -1;
    }
    return -1;
}

static void *feed_task(void *arg)
{
    (void)arg;
    while (!g_feed.stop) {
        /* 取一帧（无则等） */
        pthread_mutex_lock(&g_feed.lock);
        while (g_feed.count == 0 && !g_feed.stop)
            pthread_cond_wait(&g_feed.cond, &g_feed.lock);
        if (g_feed.stop) { pthread_mutex_unlock(&g_feed.lock); break; }
        feed_item_t it = g_feed.q[g_feed.head];
        g_feed.head = (g_feed.head + 1) % FEED_QUEUE;
        g_feed.count--;
        pthread_mutex_unlock(&g_feed.lock);

        /* 写 FIFO（阻塞直至整帧写完：天然背压，队列在上游吸收；
           读者消失 EPIPE 则关闭写端，下次重开） */
        if (g_feed.fd < 0 && feed_open_fifo() != 0) { free(it.data); continue; }
        size_t off = 0;
        while (off < it.len && !g_feed.stop) {
            ssize_t n = write(g_feed.fd, it.data + off, it.len - off);
            if (n < 0) {
                if (errno == EINTR) continue;
                /* EPIPE（读者关闭）/其他写错误：丢弃剩余并重开写端 */
                close(g_feed.fd);
                g_feed.fd = -1;
                break;
            }
            off += (size_t)n;
        }
        free(it.data);
    }
    return NULL;
}

int hevc_feed_init(void)
{
    pthread_mutex_init(&g_feed.lock, NULL);
    pthread_cond_init(&g_feed.cond, NULL);
    g_feed.fd = -1;
    mkdir(FEED_DIR, 0755);
    unlink(FEED_FIFO);
    if (mkfifo(FEED_FIFO, 0666) != 0) {
        LOG_ERROR("feed: mkfifo %s failed: %s", FEED_FIFO, strerror(errno));
        return -1;
    }
    pthread_t tid;
    if (pthread_create(&tid, NULL, feed_task, NULL) != 0) {
        LOG_ERROR("feed: thread create failed");
        return -1;
    }
    pthread_detach(tid);
    LOG_INFO("feed: fifo %s ready (queue %u)", FEED_FIFO, FEED_QUEUE);
    return 0;
}

void hevc_feed_set_ps(const unsigned char *vps, unsigned int vps_len,
                      const unsigned char *sps, unsigned int sps_len,
                      const unsigned char *pps, unsigned int pps_len)
{
    pthread_mutex_lock(&g_feed.lock);
    /* 首个参数集锁定（会话内恒定，见头文件说明） */
    if (g_feed.vps_len == 0 && vps && vps_len > 0 && vps_len <= sizeof(g_feed.vps)) {
        memcpy(g_feed.vps, vps, vps_len);
        g_feed.vps_len = vps_len;
    }
    if (g_feed.sps_len == 0 && sps && sps_len > 0 && sps_len <= sizeof(g_feed.sps)) {
        memcpy(g_feed.sps, sps, sps_len);
        g_feed.sps_len = sps_len;
    }
    if (g_feed.pps_len == 0 && pps && pps_len > 0 && pps_len <= sizeof(g_feed.pps)) {
        memcpy(g_feed.pps, pps, pps_len);
        g_feed.pps_len = pps_len;
    }
    pthread_mutex_unlock(&g_feed.lock);
}

void hevc_feed_reset_ps(void)
{
    pthread_mutex_lock(&g_feed.lock);
    g_feed.vps_len = g_feed.sps_len = g_feed.pps_len = 0;
    pthread_mutex_unlock(&g_feed.lock);
}

/* 帧内是否有参数集 NAL（VPS 32 / SPS 33 / PPS 34） */
/* 从 from 起找下一个 start code：*sc_off 记录 start code 起点，
   返回 NAL 数据起点（跳过 start code），无则 -1 */
static long nal_find(const unsigned char *d, size_t len, long from, size_t *sc_off)
{
    size_t i = (from > 0) ? (size_t)from : 0;
    for (; i + 3 <= len; i++) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            if (sc_off) *sc_off = i;
            return (long)(i + 3);
        }
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 0 &&
            i + 3 < len && d[i + 3] == 1) {
            if (sc_off) *sc_off = i;
            return (long)(i + 4);
        }
    }
    return -1;
}

static int frame_has_ps(const unsigned char *d, size_t len)
{
    size_t from = 0;
    while (from + 3 <= len) {
        long p = nal_find(d, len, (long)from, NULL);
        if (p < 0) break;
        unsigned t = ((d[p] >> 1) & 0x3Fu);
        if (t == 32 || t == 33 || t == 34) return 1;
        from = (size_t)p;
    }
    return 0;
}

/* 重组帧：剥离原内嵌参数集（VPS/SPS/PPS），关键帧前重插锁定参数集。
   这样 RTSP 流里参数集每次出现都是同一份，ffmpeg 的 sprop-* 只有单值
   （in-band 原始参数集 + 重插副本会让 ffmpeg 登记两次 → mediamtx 拒绝
   多值 sprop）。非关键帧且无参数集时零拷贝直通。 */
static int feed_rebuild_frame(unsigned char **annexb, size_t *len, int keyframe)
{
    size_t ps = 0;
    if (keyframe && g_feed.vps_len && g_feed.sps_len && g_feed.pps_len)
        ps = 4 + g_feed.vps_len + 4 + g_feed.sps_len + 4 + g_feed.pps_len;

    if (ps == 0 && !frame_has_ps(*annexb, *len)) return 0;   /* 无需重组 */

    /* 第一遍：统计保留 NAL 总长（统一 4 字节起始码） */
    size_t keep = 0;
    size_t from = 0;
    while (from + 3 <= (*len)) {
        size_t sc = 0;
        long p = nal_find(*annexb, *len, (long)from, &sc);
        if (p < 0) break;
        unsigned t = ((*annexb)[p] >> 1) & 0x3Fu;
        size_t sc2 = 0;
        long next = nal_find(*annexb, *len, p + 1, &sc2);
        size_t nlen = (next < 0) ? (*len - (size_t)p) : (sc2 - (size_t)p);
        if (t != 32 && t != 33 && t != 34) keep += 4 + nlen;
        if (next < 0) break;
        from = (size_t)p;
    }

    unsigned char *nb = malloc(ps + keep);
    if (!nb) return 0;   /* 分配失败：保留原帧（可能含双份参数集，可接受） */
    unsigned char *out = nb;
    if (ps) {
        *out++ = 0; *out++ = 0; *out++ = 0; *out++ = 1;
        memcpy(out, g_feed.vps, g_feed.vps_len); out += g_feed.vps_len;
        *out++ = 0; *out++ = 0; *out++ = 0; *out++ = 1;
        memcpy(out, g_feed.sps, g_feed.sps_len); out += g_feed.sps_len;
        *out++ = 0; *out++ = 0; *out++ = 0; *out++ = 1;
        memcpy(out, g_feed.pps, g_feed.pps_len); out += g_feed.pps_len;
    }
    from = 0;
    while (from + 3 <= (*len)) {
        long p = nal_find(*annexb, *len, (long)from, NULL);
        if (p < 0) break;
        unsigned t = ((*annexb)[p] >> 1) & 0x3Fu;
        size_t sc2 = 0;
        long next = nal_find(*annexb, *len, p + 1, &sc2);
        size_t nlen = (next < 0) ? (*len - (size_t)p) : (sc2 - (size_t)p);
        if (t != 32 && t != 33 && t != 34) {
            *out++ = 0; *out++ = 0; *out++ = 0; *out++ = 1;
            memcpy(out, *annexb + p, nlen);
            out += nlen;
        }
        if (next < 0) break;
        from = (size_t)p;
    }
    free(*annexb);
    *annexb = nb;
    *len = (size_t)(out - nb);
    return 1;
}

void hevc_feed_push(unsigned char *annexb, size_t len, int keyframe)
{
    if (!annexb || len == 0 || len > FEED_MAX_FRAME) { free(annexb); return; }
    pthread_mutex_lock(&g_feed.lock);
    if (g_feed.stop) { pthread_mutex_unlock(&g_feed.lock); free(annexb); return; }

    feed_rebuild_frame(&annexb, &len, keyframe);   /* 参数集去重/重插（锁内，纯内存操作） */
    if (g_feed.count == FEED_QUEUE) {   /* 满：丢最旧，保最新 */
        free(g_feed.q[g_feed.head].data);
        g_feed.head = (g_feed.head + 1) % FEED_QUEUE;
        g_feed.count--;
        g_feed.dropped++;
    }
    size_t i = (g_feed.head + g_feed.count) % FEED_QUEUE;
    g_feed.q[i].data = annexb;
    g_feed.q[i].len  = len;
    g_feed.count++;
    g_feed.pushed++;
    pthread_cond_signal(&g_feed.cond);
    pthread_mutex_unlock(&g_feed.lock);
}

void hevc_feed_destroy(void)
{
    pthread_mutex_lock(&g_feed.lock);
    g_feed.stop = 1;
    pthread_cond_broadcast(&g_feed.cond);
    pthread_mutex_unlock(&g_feed.lock);
    /* 写线程退出后队列残留帧不再有意义；直接清空（写线程已 stop 不再取） */
    usleep(50000);   /* 给写线程一个退出窗口（detached，无 join） */
    pthread_mutex_lock(&g_feed.lock);
    while (g_feed.count > 0) {
        free(g_feed.q[g_feed.head].data);
        g_feed.head = (g_feed.head + 1) % FEED_QUEUE;
        g_feed.count--;
    }
    pthread_mutex_unlock(&g_feed.lock);
    if (g_feed.fd >= 0) close(g_feed.fd);
    LOG_INFO("feed: shutdown (pushed %llu, dropped %llu)",
             g_feed.pushed, g_feed.dropped);
}
