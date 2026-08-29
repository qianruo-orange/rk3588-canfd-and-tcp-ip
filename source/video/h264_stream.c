/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * h264_stream.c — 录制 H.264 编码扇出 + fragmented MP4 实时推流（浏览器 MSE）。
 *
 * 数据流：录制线程（唯一生产者）h264_encoder_encode → rec_mp4_write_frame
 *   （录像不变）→ h264_stream_push_frame 入环形队列（256 帧 ≈ 8.5s，
 *   ≥2×GOP，新客户端总能接合最新 IDR）→ 每个 /video/stream 连接一个
 *   detached 推流线程（消费者）：
 *     接合最新 IDR → HTTP 200 + ftyp/moov 初始化段（avcC 取 SPS/PPS）→
 *     逐帧 moof/mdat 片段（样本 = 4 字节长度前缀 VCL NAL），按实时节奏发送。
 *
 * 关键决策：
 *  - 直播与录像共享同一份编码输出，不创建第二个编码器实例
 *    （V4L2 回退路径仅单节点 /dev/video-enc0，编码器为录制线程独占）；
 *  - push_config（录制会话开始）epoch++ 并清空全环 → 环内帧必属当前 epoch；
 *  - 会话轮转/手动重启（新编码器 → 新 SPS/PPS）推流线程断连，前端自动重连
 *    （连接内跨 init 段续接在 Chrome SourceBuffer 上不可靠，不做）；
 *  - 客户端落后被环淘汰时断连而非跳帧（跳帧会在 MSE 时间轴留缺口，卡死播放）；
 *  - 锁：单 mutex + condvar 保护环与配置，叶子锁（持锁期间不做 IO/其他加锁）；
 *    片段在锁内组好（两次 NAL 遍历，微秒级，零每帧 malloc/拷贝），锁外只做网络写；
 *  - 锁/条件变量进程生命周期不销毁（同 frame_ring 先例），stop 标志广播唤醒。
 */
#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>

#include "ai/rknn_yolo.h"
#include "core/common.h"
#include "core/log.h"
#include "video/h264_stream.h"

#define H264S_RING_N           256u              /* ≈8.5s @30fps，≥2×GOP(60帧) */
#define H264S_MAX_FRAME        (2u * 1024 * 1024)   /* 单帧合理性上限 */
#define H264S_JOIN_TIMEOUT_MS  3000u             /* 配置等待/接合超时 */
#define H264S_CLIENT_IDLE_TIMEOUT 30             /* 客户端空闲超时（秒），同 MJPEG 推流 */
#define H264S_POLL_MS          200u              /* 无事件等待节拍 */

typedef struct {
    unsigned char *data;     /* malloc 精确大小（push 时拷贝，随环淘汰释放） */
    size_t len;
    uint64_t seq;            /* 生产者单调序号（跨会话不回绕，游标寻址） */
    uint32_t ts_ms;          /* 录制线程 CLOCK_MONOTONIC 毫秒低 32 位（差运算回绕安全） */
    int keyframe;
} h264s_entry_t;

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    int             stop;    /* shutdown 置位（锁内读写） */

    h264s_entry_t ring[H264S_RING_N];
    size_t head, count;      /* 有效项 [head, head+count) mod N，seq 连续递增 */
    uint64_t next_seq;

    uint32_t epoch;          /* 0 = 无会话；push_config ++ / push_end 归零 */
    int w, h, fps;
    unsigned char sps[64], pps[64];
    unsigned int  sps_len, pps_len;

    unsigned long long pushed, dropped;
} h264s_ctx_t;

static h264s_ctx_t g_h264s = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
};

/* 每连接推流线程上下文（线程私有，退出时释放） */
typedef struct {
    int fd;
    h264_stream_client_close_cb on_close;

    uint64_t cursor;         /* 已发送帧 seq（接合帧的前一位） */
    uint32_t epoch;          /* 本连接所属会话 */
    uint32_t mfhd_seq;       /* 片段序号（每连接独立，从 1 起） */
    uint32_t origin_ms;      /* 时间轴原点 = 接合 IDR 帧 ts（tfdt 归零） */
    uint32_t last_fts;       /* 上一消费帧采集 ts（样本时长取真实帧间隔） */
    uint64_t join_wall_ms;   /* 接合时刻墙钟（实时节流基准） */
    time_t   last_write;
    int      oom;            /* 片段缓冲分配失败（致命，断连） */

    unsigned char *init; size_t init_len, init_cap;   /* 本 epoch 的 ftyp+moov */
    unsigned char *frag; size_t frag_cap;             /* 片段缓冲（复用） */
} h264s_client_t;

/* ---- 时间与等待 ---- */

static uint64_t h264s_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

/* 全局 condvar 定时等待（须已持有 g_h264s.lock；REALTIME 超时） */
static int h264s_cond_timedwait_ms(unsigned ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec  += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    return pthread_cond_timedwait(&g_h264s.cond, &g_h264s.lock, &ts);
}

/* ---- 字节缓冲写入器（全大端；oom 后全部变 no-op） ---- */

typedef struct {
    unsigned char *p;
    size_t len, cap;
    int oom;
} h264s_buf_t;

static void h264s_bb_ensure(h264s_buf_t *b, size_t extra)
{
    if (b->oom || b->len + extra <= b->cap) return;
    size_t ncap = b->cap ? b->cap * 2 : 1024;
    while (ncap < b->len + extra) ncap *= 2;
    unsigned char *np = (unsigned char *)realloc(b->p, ncap);
    if (!np) { b->oom = 1; return; }
    b->p = np;
    b->cap = ncap;
}

static void h264s_bb_u8(h264s_buf_t *b, unsigned v)
{
    h264s_bb_ensure(b, 1);
    if (b->oom) return;
    b->p[b->len++] = (unsigned char)v;
}

static void h264s_bb_u16(h264s_buf_t *b, uint16_t v)
{
    h264s_bb_ensure(b, 2);
    if (b->oom) return;
    b->p[b->len++] = (unsigned char)(v >> 8);
    b->p[b->len++] = (unsigned char)v;
}

static void h264s_bb_u32(h264s_buf_t *b, uint32_t v)
{
    h264s_bb_ensure(b, 4);
    if (b->oom) return;
    b->p[b->len++] = (unsigned char)(v >> 24);
    b->p[b->len++] = (unsigned char)(v >> 16);
    b->p[b->len++] = (unsigned char)(v >> 8);
    b->p[b->len++] = (unsigned char)v;
}

static void h264s_bb_u64(h264s_buf_t *b, uint64_t v)
{
    h264s_bb_ensure(b, 8);
    if (b->oom) return;
    b->p[b->len++] = (unsigned char)(v >> 56);
    b->p[b->len++] = (unsigned char)(v >> 48);
    b->p[b->len++] = (unsigned char)(v >> 40);
    b->p[b->len++] = (unsigned char)(v >> 32);
    b->p[b->len++] = (unsigned char)(v >> 24);
    b->p[b->len++] = (unsigned char)(v >> 16);
    b->p[b->len++] = (unsigned char)(v >> 8);
    b->p[b->len++] = (unsigned char)v;
}

static void h264s_bb_raw(h264s_buf_t *b, const void *d, size_t n)
{
    h264s_bb_ensure(b, n);
    if (b->oom || n == 0) return;
    memcpy(b->p + b->len, d, n);
    b->len += n;
}

/* 单位矩阵 36 字节（同 rec_mp4.c write_matrix） */
static void h264s_bb_matrix(h264s_buf_t *b)
{
    h264s_bb_u32(b, 0x00010000u); h264s_bb_u32(b, 0); h264s_bb_u32(b, 0);
    h264s_bb_u32(b, 0); h264s_bb_u32(b, 0x00010000u); h264s_bb_u32(b, 0);
    h264s_bb_u32(b, 0); h264s_bb_u32(b, 0); h264s_bb_u32(b, 0x40000000u);
}

/* ---- Annex-B 扫描（复制自 rec_mp4.c，不跨模块重构） ---- */

/* 从 from 起找下一个 start code：*sc_off 记录 start code 起点，
   返回 NAL 数据起点（跳过 start code），无则 -1 */
static long h264s_nal_find(const unsigned char *d, size_t len, size_t from, size_t *sc_off)
{
    for (size_t i = from; i + 3 <= len; i++) {
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

/* 遍历 Annex-B 码流，逐 NAL 回调：@start 数据起点，@nlen 数据长度 */
typedef void (*h264s_nal_cb_t)(void *ud, const unsigned char *start, size_t nlen);
static void h264s_nal_walk(const unsigned char *d, size_t len, h264s_nal_cb_t cb, void *ud)
{
    size_t from = 0;
    while (from + 3 <= len) {
        size_t sc = 0;
        long pos = h264s_nal_find(d, len, from, &sc);
        if (pos < 0) break;
        /* 从当前 NAL 数据起点找下一个 start code：H.264 帧内经防竞争字节
           保证不含 00 00 00/00 00 01，直接扫描数据区是安全的 */
        size_t sc2 = 0;
        long next = h264s_nal_find(d, len, (size_t)pos, &sc2);
        size_t nlen = (next < 0) ? (len - (size_t)pos) : ((size_t)sc2 - (size_t)pos);
        cb(ud, d + pos, nlen);
        if (next < 0) break;
        from = (size_t)pos;
    }
}

/* 配置补齐回调：从入环帧内嵌码流扫描 SPS/PPS
   （V4L2 后端首 IDR 前 extradata 为空，push_config 收到空配置的场景） */
static void h264s_collect_sps_pps(void *ud, const unsigned char *start, size_t nlen)
{
    h264s_ctx_t *x = (h264s_ctx_t *)ud;
    if (nlen == 0) return;
    int type = start[0] & 0x1F;
    if (type == 7 && x->sps_len == 0 && nlen <= sizeof(x->sps)) {
        memcpy(x->sps, start, nlen);
        x->sps_len = (unsigned int)nlen;
    } else if (type == 8 && x->pps_len == 0 && nlen <= sizeof(x->pps)) {
        memcpy(x->pps, start, nlen);
        x->pps_len = (unsigned int)nlen;
    }
}

/* ---- Annex-B → AVCC 样本（仅 VCL NAL type 1/5，4 字节长度前缀） ---- */

static void h264s_avcc_size_cb(void *ud, const unsigned char *start, size_t nlen)
{
    if (nlen == 0) return;
    int type = start[0] & 0x1F;
    if (type == 1 || type == 5) *(size_t *)ud += 4 + nlen;
}

static size_t h264s_avcc_size(const unsigned char *d, size_t len)
{
    size_t n = 0;
    h264s_nal_walk(d, len, h264s_avcc_size_cb, &n);
    return n;
}

static void h264s_avcc_write_cb(void *ud, const unsigned char *start, size_t nlen)
{
    if (nlen == 0) return;
    int type = start[0] & 0x1F;
    if (type != 1 && type != 5) return;   /* SPS/PPS/SEI/AUD 由 avcC 承载或省略 */
    unsigned char **pp = (unsigned char **)ud;
    unsigned char *p = *pp;
    p[0] = (unsigned char)(nlen >> 24);
    p[1] = (unsigned char)(nlen >> 16);
    p[2] = (unsigned char)(nlen >> 8);
    p[3] = (unsigned char)nlen;
    memcpy(p + 4, start, nlen);
    *pp = p + 4 + nlen;
}

/* 写入恰好 avcc_size 字节到 dst（调用方已保证容量） */
static void h264s_avcc_write(unsigned char *dst, const unsigned char *d, size_t len)
{
    h264s_nal_walk(d, len, h264s_avcc_write_cb, &dst);
}

/* ---- 初始化段：ftyp + moov（live 形态：duration 0 + 空样本表，
   ffmpeg empty_moov+frag_keyframe 同款，Chrome MSE 接受） ---- */

static void h264s_build_init(h264s_buf_t *b, int w, int h,
                             const unsigned char *sps, unsigned int sps_len,
                             const unsigned char *pps, unsigned int pps_len)
{
    /* 子盒尺寸（全部动态推导，参照 rec_mp4.c mp4_write_moov；
       live 流无 stss，同步信息由 trun first_sample_flags 承载） */
    uint32_t avcC_size = 19u + sps_len + pps_len;        /* 8+6+2+sps+1+2+pps */
    uint32_t avc1_size = 86u + avcC_size;                /* 8 hdr + 78 body + avcC 盒 */
    uint32_t stsd_size = 16u + avc1_size;                /* 8+8+entry */
    uint32_t stts_size = 16u;                            /* 空表 */
    uint32_t stsc_size = 16u;
    uint32_t stsz_size = 20u;                            /* 8+4+4(sample_size 0)+4(count 0) */
    uint32_t stco_size = 16u;
    uint32_t stbl_size = 8u + stsd_size + stts_size + stsc_size
                         + stsz_size + stco_size;
    uint32_t vmhd_size = 20u;
    uint32_t dinf_size = 36u;
    uint32_t minf_size = 8u + vmhd_size + dinf_size + stbl_size;
    uint32_t mdhd_size = 32u;
    uint32_t hdlr_size = 48u;
    uint32_t mdia_size = 8u + mdhd_size + hdlr_size + minf_size;
    uint32_t tkhd_size = 92u;
    uint32_t trak_size = 8u + tkhd_size + mdia_size;
    uint32_t mvhd_size = 108u;
    uint32_t mehd_size = 16u;              /* fragment_duration 0（live） */
    uint32_t trex_size = 32u;
    uint32_t mvex_size = 8u + mehd_size + trex_size;   /* 分片流必备标记 */
    uint32_t moov_size = 8u + mvhd_size + trak_size + mvex_size;

    static const unsigned char cname[32] = "rkvenc";   /* 其余为零 */

    b->len = 0;

    /* ---- ftyp（28）：mp42 品牌，兼容 mp42/mp41/isom ---- */
    h264s_bb_u32(b, 28); h264s_bb_raw(b, "ftyp", 4);
    h264s_bb_raw(b, "mp42", 4);
    h264s_bb_u32(b, 0);
    h264s_bb_raw(b, "mp42mp41isom", 12);

    /* ---- moov ---- */
    h264s_bb_u32(b, moov_size); h264s_bb_raw(b, "moov", 4);

    /* ---- mvhd ---- */
    h264s_bb_u32(b, mvhd_size); h264s_bb_raw(b, "mvhd", 4);
    h264s_bb_u32(b, 0);                      /* version=0, flags=0 */
    h264s_bb_u32(b, 0); h264s_bb_u32(b, 0);  /* creation/modification */
    h264s_bb_u32(b, 1000);                   /* timescale */
    h264s_bb_u32(b, 0);                      /* duration = 0（live 流） */
    h264s_bb_u32(b, 0x00010000u);            /* rate 1.0 */
    h264s_bb_u16(b, 0x0100);                 /* volume */
    h264s_bb_u16(b, 0);                      /* reserved */
    h264s_bb_u64(b, 0);                      /* reserved */
    h264s_bb_matrix(b);
    h264s_bb_u64(b, 0); h264s_bb_u64(b, 0); h264s_bb_u64(b, 0);   /* pre_defined 24 字节 */
    h264s_bb_u32(b, 2);                      /* next_track_id */

    /* ---- trak ---- */
    h264s_bb_u32(b, trak_size); h264s_bb_raw(b, "trak", 4);

    /* ---- tkhd ---- */
    h264s_bb_u32(b, tkhd_size); h264s_bb_raw(b, "tkhd", 4);
    h264s_bb_u32(b, 7);                      /* version=0, flags=enabled|in_movie|in_preview */
    h264s_bb_u32(b, 0); h264s_bb_u32(b, 0);
    h264s_bb_u32(b, 1);                      /* track_ID */
    h264s_bb_u32(b, 0);                      /* reserved */
    h264s_bb_u32(b, 0);                      /* duration = 0（live 流） */
    h264s_bb_u64(b, 0);                      /* reserved */
    h264s_bb_u16(b, 0); h264s_bb_u16(b, 0);  /* layer, alternate_group */
    h264s_bb_u16(b, 0x0100);                 /* volume */
    h264s_bb_u16(b, 0);                      /* reserved */
    h264s_bb_matrix(b);
    h264s_bb_u32(b, (uint32_t)((unsigned)w << 16));
    h264s_bb_u32(b, (uint32_t)((unsigned)h << 16));

    /* ---- mdia ---- */
    h264s_bb_u32(b, mdia_size); h264s_bb_raw(b, "mdia", 4);

    /* ---- mdhd ---- */
    h264s_bb_u32(b, mdhd_size); h264s_bb_raw(b, "mdhd", 4);
    h264s_bb_u32(b, 0);
    h264s_bb_u32(b, 0); h264s_bb_u32(b, 0);
    h264s_bb_u32(b, 90000);                  /* video timescale */
    h264s_bb_u32(b, 0);                      /* duration = 0（live 流） */
    h264s_bb_u16(b, 0x55C4);                 /* language = und */
    h264s_bb_u16(b, 0);

    /* ---- hdlr ---- */
    h264s_bb_u32(b, hdlr_size); h264s_bb_raw(b, "hdlr", 4);
    h264s_bb_u32(b, 0);
    h264s_bb_u32(b, 0);                      /* pre_defined */
    h264s_bb_raw(b, "vide", 4);              /* handler_type */
    h264s_bb_u32(b, 0); h264s_bb_u32(b, 0); h264s_bb_u32(b, 0);   /* reserved 12 */
    h264s_bb_raw(b, "VideoHandler", 12);
    h264s_bb_u32(b, 0);

    /* ---- minf ---- */
    h264s_bb_u32(b, minf_size); h264s_bb_raw(b, "minf", 4);

    /* ---- vmhd ---- */
    h264s_bb_u32(b, vmhd_size); h264s_bb_raw(b, "vmhd", 4);
    h264s_bb_u32(b, 1);                      /* version=0, flags=1 */
    h264s_bb_u16(b, 0);                      /* graphicsmode */
    h264s_bb_u16(b, 0); h264s_bb_u16(b, 0); h264s_bb_u16(b, 0);   /* opcolor */

    /* ---- dinf / dref ---- */
    h264s_bb_u32(b, dinf_size); h264s_bb_raw(b, "dinf", 4);
    h264s_bb_u32(b, 28); h264s_bb_raw(b, "dref", 4);
    h264s_bb_u32(b, 0);
    h264s_bb_u32(b, 1);                      /* entry_count */
    h264s_bb_u32(b, 12); h264s_bb_raw(b, "url ", 4);
    h264s_bb_u32(b, 1);                      /* flags=1 (self-contained) */

    /* ---- stbl ---- */
    h264s_bb_u32(b, stbl_size); h264s_bb_raw(b, "stbl", 4);

    /* ---- stsd：H.264 VisualSampleEntry（'avc1'）+ avcC ---- */
    h264s_bb_u32(b, stsd_size); h264s_bb_raw(b, "stsd", 4);
    h264s_bb_u32(b, 0);
    h264s_bb_u32(b, 1);                      /* entry_count */
    h264s_bb_u32(b, avc1_size); h264s_bb_raw(b, "avc1", 4);
    h264s_bb_u32(b, 0); h264s_bb_u16(b, 0);  /* reserved 6 */
    h264s_bb_u16(b, 1);                      /* data_reference_index */
    h264s_bb_u16(b, 0); h264s_bb_u16(b, 0);  /* pre_defined + reserved */
    h264s_bb_u32(b, 0); h264s_bb_u32(b, 0); h264s_bb_u32(b, 0);   /* pre_defined 12 */
    h264s_bb_u16(b, (uint16_t)w);
    h264s_bb_u16(b, (uint16_t)h);
    h264s_bb_u32(b, 0x00480000u);            /* horizresolution 72dpi */
    h264s_bb_u32(b, 0x00480000u);            /* vertresolution */
    h264s_bb_u32(b, 0);                      /* reserved */
    h264s_bb_u16(b, 1);                      /* frame_count */
    h264s_bb_raw(b, cname, 32);
    h264s_bb_u16(b, 24);                     /* depth */
    h264s_bb_u16(b, 0xFFFFu);                /* pre_defined */

    /* avcC */
    h264s_bb_u32(b, avcC_size); h264s_bb_raw(b, "avcC", 4);
    h264s_bb_u8(b, 1);                                    /* configurationVersion */
    h264s_bb_u8(b, sps_len > 0 ? sps[1] : 66);            /* AVCProfileIndication */
    h264s_bb_u8(b, sps_len > 0 ? sps[2] : 0);             /* profile_compatibility */
    h264s_bb_u8(b, sps_len > 0 ? sps[3] : 30);            /* AVCLevelIndication */
    h264s_bb_u8(b, 0xFF);                                 /* 0xFC|lengthSizeMinusOne=3（4 字节） */
    h264s_bb_u8(b, 0xE1);                                 /* 0xE0|numSPS=1 */
    h264s_bb_u16(b, (uint16_t)sps_len);
    h264s_bb_raw(b, sps, sps_len);
    h264s_bb_u8(b, 1);                                    /* numPPS */
    h264s_bb_u16(b, (uint16_t)pps_len);
    h264s_bb_raw(b, pps, pps_len);

    /* ---- 空样本表（live 流）：stts(16)/stsc(16)/stsz(20)/stco(16) ---- */
    h264s_bb_u32(b, stts_size); h264s_bb_raw(b, "stts", 4);
    h264s_bb_u32(b, 0); h264s_bb_u32(b, 0);               /* entry_count 0 */
    h264s_bb_u32(b, stsc_size); h264s_bb_raw(b, "stsc", 4);
    h264s_bb_u32(b, 0); h264s_bb_u32(b, 0);
    h264s_bb_u32(b, stsz_size); h264s_bb_raw(b, "stsz", 4);
    h264s_bb_u32(b, 0); h264s_bb_u32(b, 0); h264s_bb_u32(b, 0);
    h264s_bb_u32(b, stco_size); h264s_bb_raw(b, "stco", 4);
    h264s_bb_u32(b, 0); h264s_bb_u32(b, 0);

    /* ---- mvex（fragmented MP4 必备：ffmpeg/Chrome 依此识别分片流，
       moov 之后、紧跟 mvhd/trak，duration 0 + 空表 + mvex = 标准 live-fMP4 形态） ---- */
    h264s_bb_u32(b, mvex_size); h264s_bb_raw(b, "mvex", 4);
    h264s_bb_u32(b, mehd_size); h264s_bb_raw(b, "mehd", 4);
    h264s_bb_u32(b, 0);                      /* version=0, flags=0 */
    h264s_bb_u32(b, 0);                      /* fragment_duration = 0（live） */
    h264s_bb_u32(b, trex_size); h264s_bb_raw(b, "trex", 4);
    h264s_bb_u32(b, 0);                      /* version=0, flags=0 */
    h264s_bb_u32(b, 1);                      /* track_ID */
    h264s_bb_u32(b, 1);                      /* default_sample_description_index */
    h264s_bb_u32(b, 0);                      /* default_sample_duration（由 trun 给出） */
    h264s_bb_u32(b, 0);                      /* default_sample_size */
    h264s_bb_u32(b, 0);                      /* default_sample_flags */
}

/* codec 串：avc1.剖面兼容级别（供 MSE addSourceBuffer 精确匹配） */
static void h264s_codec_string(char *out, size_t outsz,
                               const unsigned char *sps, unsigned int sps_len)
{
    if (sps_len >= 4)
        snprintf(out, outsz, "avc1.%02x%02x%02x", sps[1], sps[2], sps[3]);
    else
        snprintf(out, outsz, "avc1.4d401f");
}

/* ---- 单帧片段：moof(mfhd/tfhd/tfdt/trun) + mdat(AVCC 样本) ----
   返回片段字节数；0 = 无 VCL 数据（跳过）或缓冲分配失败（c->oom 置位） */
static size_t h264s_build_fragment(h264s_client_t *c,
                                   const unsigned char *annexb, size_t alen,
                                   int keyframe, uint32_t mfhd_seq,
                                   uint64_t bmdt90, uint32_t dur90)
{
    size_t avcc = h264s_avcc_size(annexb, alen);
    if (avcc == 0) return 0;   /* 纯 SEI/AUD 等无 VCL 帧：跳过 */

    h264s_buf_t b = { .p = c->frag, .len = 0, .cap = c->frag_cap, .oom = 0 };

    /* moof 恒 100（mfhd 16 + traf 76），mdat 头 8 → trun data_offset 恒 108 */
    h264s_bb_u32(&b, 100); h264s_bb_raw(&b, "moof", 4);

    h264s_bb_u32(&b, 16); h264s_bb_raw(&b, "mfhd", 4);
    h264s_bb_u32(&b, 0);
    h264s_bb_u32(&b, mfhd_seq);

    /* traf：tfhd/tfdt/trun 必须包在 TrackFragmentBox 内（ISO 14496-12；
       平铺在 moof 下会令容器计数偏差 8 字节，解析器错位） */
    h264s_bb_u32(&b, 76); h264s_bb_raw(&b, "traf", 4);

    h264s_bb_u32(&b, 16); h264s_bb_raw(&b, "tfhd", 4);
    h264s_bb_u32(&b, 0x020000u);             /* version 0 + default-base-is-moof */
    h264s_bb_u32(&b, 1);                     /* track_ID */

    h264s_bb_u32(&b, 20); h264s_bb_raw(&b, "tfdt", 4);
    h264s_bb_u32(&b, 1);                     /* version 1 → 64 位（32 位 90000 刻度 ~13.3h 回绕） */
    h264s_bb_u64(&b, bmdt90);

    h264s_bb_u32(&b, 32); h264s_bb_raw(&b, "trun", 4);
    h264s_bb_u32(&b, 0x000305u);             /* data-offset|first-sample-flags|sample-duration|sample-size */
    h264s_bb_u32(&b, 1);                     /* sample_count */
    h264s_bb_u32(&b, 108);
    h264s_bb_u32(&b, keyframe ? 0x02000000u : 0x01010000u);
    h264s_bb_u32(&b, dur90);
    h264s_bb_u32(&b, (uint32_t)avcc);

    h264s_bb_u32(&b, 8u + (uint32_t)avcc); h264s_bb_raw(&b, "mdat", 4);
    h264s_bb_ensure(&b, avcc);
    if (!b.oom) {
        h264s_avcc_write(b.p + b.len, annexb, alen);
        b.len += avcc;
    }

    c->frag = b.p;
    c->frag_cap = b.cap;
    c->oom = b.oom;
    return b.oom ? 0 : b.len;
}

/* ---- 环形队列（生产者：录制线程；消费者：推流线程） ---- */

void h264_stream_push_config(int w, int h, int fps,
                             const unsigned char *sps, unsigned int sps_len,
                             const unsigned char *pps, unsigned int pps_len)
{
    unsigned int sl = 0, pl = 0;
    pthread_mutex_lock(&g_h264s.lock);
    g_h264s.epoch++;
    while (g_h264s.count > 0) {   /* 清空全环：环内帧必属当前 epoch */
        free(g_h264s.ring[g_h264s.head].data);
        g_h264s.head = (g_h264s.head + 1) % H264S_RING_N;
        g_h264s.count--;
    }
    g_h264s.w = w; g_h264s.h = h; g_h264s.fps = fps;
    g_h264s.sps_len = 0; g_h264s.pps_len = 0;
    if (sps && sps_len > 0 && sps_len <= sizeof(g_h264s.sps)) {
        memcpy(g_h264s.sps, sps, sps_len);
        g_h264s.sps_len = sps_len;
        sl = sps_len;
    }
    if (pps && pps_len > 0 && pps_len <= sizeof(g_h264s.pps)) {
        memcpy(g_h264s.pps, pps, pps_len);
        g_h264s.pps_len = pps_len;
        pl = pps_len;
    }
    pthread_cond_broadcast(&g_h264s.cond);
    pthread_mutex_unlock(&g_h264s.lock);
    LOG_INFO("h264s: session %u start %dx%d @%dfps (sps %uB, pps %uB)",
             g_h264s.epoch, w, h, fps, sl, pl);
}

void h264_stream_push_frame(const unsigned char *annexb, size_t len,
                            int keyframe, uint64_t ts_ms)
{
    if (!annexb || len == 0 || len > H264S_MAX_FRAME) return;
    pthread_mutex_lock(&g_h264s.lock);
    if (g_h264s.stop || g_h264s.epoch == 0) {
        pthread_mutex_unlock(&g_h264s.lock);
        return;
    }
    /* 配置缺 SPS/PPS（V4L2 后端首 IDR 前）：从入环帧内嵌 SPS/PPS 扫描补齐
       （补齐后本帧尾部的 broadcast 会唤醒等待配置的客户端） */
    if (g_h264s.sps_len == 0 || g_h264s.pps_len == 0)
        h264s_nal_walk(annexb, len, h264s_collect_sps_pps, &g_h264s);
    if (g_h264s.count == H264S_RING_N) {   /* 满：淘汰最旧 */
        free(g_h264s.ring[g_h264s.head].data);
        g_h264s.head = (g_h264s.head + 1) % H264S_RING_N;
        g_h264s.count--;
    }
    unsigned char *copy = (unsigned char *)malloc(len);
    if (!copy) {
        g_h264s.dropped++;
        pthread_mutex_unlock(&g_h264s.lock);
        return;
    }
    memcpy(copy, annexb, len);
    size_t i = (g_h264s.head + g_h264s.count) % H264S_RING_N;
    g_h264s.ring[i].data     = copy;
    g_h264s.ring[i].len      = len;
    g_h264s.ring[i].seq      = g_h264s.next_seq++;
    g_h264s.ring[i].ts_ms    = (uint32_t)ts_ms;
    g_h264s.ring[i].keyframe = keyframe ? 1 : 0;
    g_h264s.count++;
    g_h264s.pushed++;
    pthread_cond_broadcast(&g_h264s.cond);
    pthread_mutex_unlock(&g_h264s.lock);
}

void h264_stream_push_end(void)
{
    pthread_mutex_lock(&g_h264s.lock);
    g_h264s.epoch = 0;
    while (g_h264s.count > 0) {
        free(g_h264s.ring[g_h264s.head].data);
        g_h264s.head = (g_h264s.head + 1) % H264S_RING_N;
        g_h264s.count--;
    }
    pthread_cond_broadcast(&g_h264s.cond);
    pthread_mutex_unlock(&g_h264s.lock);
    LOG_INFO("h264s: session end (pushed %llu, dropped %llu)",
             g_h264s.pushed, g_h264s.dropped);
}

void h264_stream_shutdown(void)
{
    pthread_mutex_lock(&g_h264s.lock);
    g_h264s.stop = 1;
    pthread_cond_broadcast(&g_h264s.cond);
    pthread_mutex_unlock(&g_h264s.lock);
}

/* ---- 推流线程 ---- */

static void h264s_fail(h264s_client_t *c, int code, const char *reason,
                       const char *msg)
{
    char buf[256];
    int n = snprintf(buf, sizeof(buf),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: text/plain; charset=utf-8\r\n"
                     "Connection: close\r\n\r\n%s",
                     code, reason, msg);
    fd_write_all_blocking(c->fd, buf, (size_t)n);
}

static void h264s_client_destroy(h264s_client_t *c)
{
    free(c->init);
    free(c->frag);
    free(c);
}

static void *h264_stream_client_task(void *arg)
{
    h264s_client_t *c = (h264s_client_t *)arg;
    int fd = c->fd;
    h264_stream_client_close_cb on_close = c->on_close;

    /* AI 为必要流程（与录像同源）：无 AI 直接 503（与 mjpeg_ai 行为对齐） */
    if (!rknn_yolo_enabled()) {
        h264s_fail(c, 503, "Service Unavailable", "AI 推理不可用（无 AI）\n");
        on_close(fd);
        h264s_client_destroy(c);
        return NULL;
    }

    /* 1) 等配置有效（epoch>0 且 SPS/PPS 齐全），≤3s 超时 503 */
    int w = 0, h = 0, fps = 0;
    unsigned char sps[64], pps[64];
    unsigned int sl = 0, pl = 0;
    int ok = 0;
    pthread_mutex_lock(&g_h264s.lock);
    for (unsigned waited = 0; waited < H264S_JOIN_TIMEOUT_MS && !g_h264s.stop; ) {
        if (g_h264s.epoch != 0 && g_h264s.sps_len > 0 && g_h264s.pps_len > 0) {
            c->epoch = g_h264s.epoch;
            w = g_h264s.w; h = g_h264s.h; fps = g_h264s.fps;
            sl = g_h264s.sps_len; pl = g_h264s.pps_len;
            memcpy(sps, g_h264s.sps, sl);
            memcpy(pps, g_h264s.pps, pl);
            ok = 1;
            break;
        }
        h264s_cond_timedwait_ms(H264S_POLL_MS);
        waited += H264S_POLL_MS;
    }
    pthread_mutex_unlock(&g_h264s.lock);
    if (!ok) {
        h264s_fail(c, 503, "Service Unavailable", "推流暂不可用（无编码会话）\n");
        on_close(fd);
        h264s_client_destroy(c);
        return NULL;
    }

    /* 2) 初始化段（ftyp+moov，按本会话 SPS/PPS 构建） */
    h264s_buf_t ib = { .p = c->init, .len = 0, .cap = c->init_cap, .oom = 0 };
    h264s_build_init(&ib, w, h, sps, sl, pps, pl);
    c->init = ib.p;
    c->init_len = ib.len;
    c->init_cap = ib.cap;
    if (ib.oom) {
        h264s_fail(c, 503, "Service Unavailable", "内存不足\n");
        on_close(fd);
        h264s_client_destroy(c);
        return NULL;
    }

    /* 3) 接合最新 IDR（GOP≈2s ≤ 环 8.5s，环内必有；新会话则等首 IDR ≤3s） */
    ok = 0;
    pthread_mutex_lock(&g_h264s.lock);
    for (unsigned waited = 0; waited < H264S_JOIN_TIMEOUT_MS && !g_h264s.stop; ) {
        if (g_h264s.epoch != c->epoch) break;   /* 会话轮转：重连走新配置 */
        for (size_t k = g_h264s.count; k-- > 0; ) {   /* 环尾（最新）向前扫 */
            const h264s_entry_t *e =
                &g_h264s.ring[(g_h264s.head + k) % H264S_RING_N];
            if (e->keyframe) {
                c->cursor = e->seq - 1;
                c->origin_ms = e->ts_ms;
                c->join_wall_ms = h264s_now_ms();
                ok = 1;
                break;
            }
        }
        if (ok) break;
        h264s_cond_timedwait_ms(H264S_POLL_MS);
        waited += H264S_POLL_MS;
    }
    pthread_mutex_unlock(&g_h264s.lock);
    if (!ok) {
        h264s_fail(c, 503, "Service Unavailable", "推流暂不可用（无关键帧）\n");
        on_close(fd);
        h264s_client_destroy(c);
        return NULL;
    }

    /* 4) 响应头 + 初始化段（close-delimited；X-Codec 给前端精确 codec 串） */
    char codec[32];
    h264s_codec_string(codec, sizeof(codec), sps, sl);
    char hdr[256];
    int hl = snprintf(hdr, sizeof(hdr),
                      "HTTP/1.1 200 OK\r\n"
                      "Content-Type: video/mp4\r\n"
                      "Cache-Control: no-cache\r\n"
                      "X-Codec: %s\r\n"
                      "Connection: close\r\n\r\n", codec);
    if (fd_write_all_blocking(fd, hdr, (size_t)hl) < 0 ||
        fd_write_all_blocking(fd, c->init, c->init_len) < 0) {
        on_close(fd);
        h264s_client_destroy(c);
        return NULL;
    }
    c->last_write = time(NULL);
    LOG_INFO("h264s: client joined (epoch %u, %s)", c->epoch, codec);

    /* 样本时长兜底（首帧/间隔异常时用）：实测 fps 与源速率可能不符
       （相机各次启动帧率不同），正常路径取相邻帧真实间隔 */
    uint32_t dur90_def = fps > 0 ? (uint32_t)(90000u / (unsigned)fps) : 3000u;

    /* 5) 主循环：按序逐帧组片段（锁内）→ 网络写（锁外）→ 实时节流 */
    while (1) {
        if (time(NULL) - c->last_write >= H264S_CLIENT_IDLE_TIMEOUT) break;

        /* 对端断开探测（MSG_PEEK 不消费字节；克隆 MJPEG 推流线程做法） */
        char peek;
        ssize_t pr = recv(fd, &peek, 1, MSG_PEEK | MSG_DONTWAIT);
        if (pr == 0) break;
        if (pr < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) break;

        uint64_t expected = c->cursor + 1;
        uint32_t fts = 0;
        size_t flen = 0;
        int have = 0;

        pthread_mutex_lock(&g_h264s.lock);
        if (g_h264s.stop || g_h264s.epoch != c->epoch) {
            pthread_mutex_unlock(&g_h264s.lock);
            break;   /* 停止/会话轮转：断连（前端自动重连取新 init 段） */
        }
        if (g_h264s.count > 0) {
            uint64_t oldest = g_h264s.next_seq - g_h264s.count;
            if (expected < oldest) {
                /* 游标已被环淘汰（落后 >8.5s）：跳帧会在 MSE 时间轴留缺口，断连重连 */
                pthread_mutex_unlock(&g_h264s.lock);
                break;
            }
            if (expected <= g_h264s.next_seq - 1) {
                const h264s_entry_t *e =
                    &g_h264s.ring[(g_h264s.head + (size_t)(expected - oldest)) % H264S_RING_N];
                /* 样本时长取相邻帧真实间隔（源速率与实测 fps 不符时时间轴仍正确）；
                   首帧/间隔异常（>500ms）回退实测 fps */
                uint32_t d90 = 0;
                if (c->last_fts) {
                    uint32_t gap = (uint32_t)(e->ts_ms - c->last_fts);
                    if (gap > 0 && gap < 500u) d90 = gap * 90u;
                }
                if (d90 == 0) d90 = dur90_def;
                flen = h264s_build_fragment(c, e->data, e->len, e->keyframe,
                                            c->mfhd_seq + 1,
                                            (uint64_t)(e->ts_ms - c->origin_ms) * 90u,
                                            d90);
                if (flen > 0) c->mfhd_seq++;
                fts = e->ts_ms;
                c->last_fts = e->ts_ms;   /* 跳过帧也推进：下一帧时长跨真实间隔 */
                have = 1;
            }
        }
        pthread_mutex_unlock(&g_h264s.lock);

        if (c->oom) break;   /* 片段缓冲分配失败：致命，断连 */
        if (!have) {
            /* 无新帧：等待（被 stop/epoch 变更/新帧广播唤醒） */
            pthread_mutex_lock(&g_h264s.lock);
            if (!g_h264s.stop && g_h264s.epoch == c->epoch)
                h264s_cond_timedwait_ms(H264S_POLL_MS);
            pthread_mutex_unlock(&g_h264s.lock);
            continue;
        }
        if (flen == 0) {   /* 纯 SEI/AUD 等无 VCL 帧：游标推进跳过，不占 mfhd 序号 */
            c->cursor = expected;
            continue;
        }

        if (fd_write_all_blocking(fd, c->frag, flen) < 0) break;
        c->cursor = expected;
        c->last_write = time(NULL);

        /* 实时节流：目标墙钟 = 接合时刻 + (帧采集时间 − 接合帧时间)。
           落后（接合滞后 ≤2s / 拥塞恢复）时连续追赶，随后自动回归实时 */
        uint64_t target = c->join_wall_ms + (uint64_t)(fts - c->origin_ms);
        uint64_t now = h264s_now_ms();
        if (target > now) {
            pthread_mutex_lock(&g_h264s.lock);
            if (!g_h264s.stop && g_h264s.epoch == c->epoch)
                h264s_cond_timedwait_ms((unsigned)(target - now));
            pthread_mutex_unlock(&g_h264s.lock);
        }
    }

    on_close(fd);
    h264s_client_destroy(c);
    return NULL;
}

int h264_stream_client_start(int fd, h264_stream_client_close_cb on_close)
{
    h264s_client_t *c = (h264s_client_t *)calloc(1, sizeof(*c));
    if (!c) return -1;
    c->fd = fd;
    c->on_close = on_close;
    pthread_t tid;
    if (pthread_create(&tid, NULL, h264_stream_client_task, c) != 0) {
        free(c);
        return -1;
    }
    pthread_detach(tid);
    return 0;
}
