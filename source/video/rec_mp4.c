/**
 * rec_mp4.c — 最小 MP4(H.264 avc1 track) 封装器（ISO BMFF，moov 末尾回写）。
 *
 * 文件布局：ftyp(28) + mdat(8+N 帧 H.264 length-prefixed) + moov
 *   stbl 记录每帧绝对偏移（stco）/ 大小（stsz）/ 平均帧间隔（stts），
 *   stss 记录关键帧（IDR），stsd 用 avc1+avcC（SPS/PPS 取自编码器）。
 *   mdat 内样本为 4 字节大端长度前缀的 NAL（avc1 规范格式），
 *   Chrome / VLC / ffplay 均可直接播放。
 *
 * 纯封装，无线程；调用方保证并发安全。
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "video/rec_mp4.h"

#define REC_MAX_SAMPLES 200000u      /* 帧数上限（≈1.8h @30fps），防内存无限增长 */
#define REC_MAX_MDAT   (3500u * 1024 * 1024)  /* mdat 数据上限 3.5GB（32 位 size） */
#define REC_INIT_CAP   1024u         /* 样本表初始容量 */

/* 单帧记录（stbl 用） */
typedef struct {
    uint32_t offset;   /* 帧数据在文件中的绝对偏移 */
    uint32_t size;
} rec_sample_t;

struct rec_mp4_s {
    FILE       *fp;
    char        path[512];
    char        name[128];
    uint32_t    mdat_start;           /* mdat 头文件偏移（= ftyp 总大小） */
    uint32_t    data_len;             /* mdat 数据字节数 */
    int         w, h;
    rec_sample_t *samples;
    uint32_t    n_samples, cap_samples;
    uint32_t    *keyframes;           /* 关键帧样本序号（1-based，stss 用） */
    uint32_t    n_key, cap_key;
    unsigned char sps[64], pps[64];   /* avcC：SPS/PPS（首帧写入） */
    unsigned int  sps_len, pps_len;
    int         avcC_written;
    uint64_t    first_ts_ms, last_ts_ms;   /* 首/末帧单调时钟 */
    uint64_t    start_ms;
};

/* ---- 小端/大端写入 ---- */

static void be32(FILE *fp, uint32_t v)
{
    unsigned char b[4] = { (unsigned char)(v >> 24), (unsigned char)(v >> 16),
                           (unsigned char)(v >> 8),  (unsigned char)v };
    fwrite(b, 1, 4, fp);
}

static void be64(FILE *fp, uint64_t v)
{
    unsigned char b[8] = { (unsigned char)(v >> 56), (unsigned char)(v >> 48),
                           (unsigned char)(v >> 40), (unsigned char)(v >> 32),
                           (unsigned char)(v >> 24), (unsigned char)(v >> 16),
                           (unsigned char)(v >> 8),  (unsigned char)v };
    fwrite(b, 1, 8, fp);
}

static void be16(FILE *fp, uint16_t v)
{
    unsigned char b[2] = { (unsigned char)(v >> 8), (unsigned char)v };
    fwrite(b, 1, 2, fp);
}

static void box_hdr(FILE *fp, uint32_t size, const char type[4])
{
    be32(fp, size);
    fwrite(type, 1, 4, fp);
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

/* 单位矩阵 36 字节 */
static void write_matrix(FILE *fp)
{
    be32(fp, 0x00010000u); be32(fp, 0); be32(fp, 0);
    be32(fp, 0); be32(fp, 0x00010000u); be32(fp, 0);
    be32(fp, 0); be32(fp, 0); be32(fp, 0x40000000u);
}

/* ---- Annex-B 扫描辅助 ---- */

/* 从 from 起找下一个 start code：*sc_off 记录 start code 起点，
   返回 NAL 数据起点（跳过 start code），无则 -1 */
static long nal_find(const unsigned char *d, size_t len, size_t from, size_t *sc_off)
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
typedef void (*nal_cb_t)(void *ud, const unsigned char *start, size_t nlen);
static void nal_walk(const unsigned char *d, size_t len, nal_cb_t cb, void *ud)
{
    size_t from = 0;
    while (from + 3 <= len) {
        size_t sc = 0;
        long pos = nal_find(d, len, from, &sc);
        if (pos < 0) break;
        /* 从当前 NAL 数据起点找下一个 start code：H.264 帧内经防竞争字节
           保证不含 00 00 00/00 00 01，直接扫描数据区是安全的 */
        size_t sc2 = 0;
        long next = nal_find(d, len, (size_t)pos, &sc2);
        size_t nlen = (next < 0) ? (len - (size_t)pos) : ((size_t)sc2 - (size_t)pos);
        cb(ud, d + pos, nlen);
        if (next < 0) break;
        from = (size_t)pos;
    }
}

/* ---- MP4 封装 ---- */

/* 写 ftyp：mp42 品牌，兼容 mp42/mp41/isom（28 字节） */
static void mp4_write_ftyp(FILE *fp)
{
    box_hdr(fp, 28, "ftyp");
    fwrite("mp42", 1, 4, fp);
    be32(fp, 0);
    fwrite("mp42mp41isom", 1, 12, fp);
}

/* 写 moov（文件末尾）：mvhd + trak(tkhd, mdia(mdhd, hdlr, minf(vmhd, dinf, stbl))）。
   所有子盒大小动态计算（含 avcC 与 stss），@delta 为平均帧间隔（timescale 90000） */
static void mp4_write_moov(FILE *fp, const rec_mp4_t *s, uint32_t n,
                           uint32_t delta, uint32_t n_key)
{
    uint32_t N = n, K = n_key;

    /* 子盒尺寸（全部动态推导） */
    uint32_t avcC_size = 18u + s->sps_len + s->pps_len;        /* 8+7+3+sps+pps */
    uint32_t avc1_size = 86u + avcC_size;                      /* 8+78+avcC */
    uint32_t stsd_size = 16u + avc1_size;                      /* 8+8+entry */
    uint32_t stss_size = 16u + 4u * K;
    uint32_t stts_size = 24u;
    uint32_t stsc_size = 28u;
    uint32_t stsz_size = 20u + 4u * N;
    uint32_t stco_size = 16u + 4u * N;
    uint32_t stbl_size = 8u + stsd_size + stts_size + stsc_size + stsz_size
                         + stco_size + stss_size;
    uint32_t vmhd_size = 20u;
    uint32_t dinf_size = 36u;
    uint32_t minf_size = 8u + vmhd_size + dinf_size + stbl_size;
    uint32_t mdhd_size = 32u;
    uint32_t hdlr_size = 48u;
    uint32_t mdia_size = 8u + mdhd_size + hdlr_size + minf_size;
    uint32_t tkhd_size = 92u;
    uint32_t trak_size = 8u + tkhd_size + mdia_size;
    uint32_t mvhd_size = 108u;
    uint32_t moov_size = 8u + mvhd_size + trak_size;

    uint32_t duration  = delta * N;

    box_hdr(fp, moov_size, "moov");

    /* ---- mvhd ---- */
    box_hdr(fp, mvhd_size, "mvhd");
    be32(fp, 0);                    /* version=0, flags=0 */
    be32(fp, 0); be32(fp, 0);       /* creation/modification */
    be32(fp, 1000);                 /* timescale */
    be32(fp, (uint32_t)(s->last_ts_ms - s->first_ts_ms));  /* duration ms */
    be32(fp, 0x00010000u);          /* rate 1.0 */
    be16(fp, 0x0100);               /* volume */
    be16(fp, 0);                    /* reserved */
    be64(fp, 0);                    /* reserved */
    write_matrix(fp);
    be64(fp, 0); be64(fp, 0); be64(fp, 0);   /* pre_defined 24 字节 */
    be32(fp, 2);                    /* next_track_id */

    /* ---- trak ---- */
    box_hdr(fp, trak_size, "trak");

    /* ---- tkhd ---- */
    box_hdr(fp, tkhd_size, "tkhd");
    be32(fp, 7);                    /* version=0, flags=enabled|in_movie|in_preview */
    be32(fp, 0); be32(fp, 0);
    be32(fp, 1);                    /* track_ID */
    be32(fp, 0);                    /* reserved */
    be32(fp, duration);
    be64(fp, 0);                    /* reserved */
    be16(fp, 0); be16(fp, 0);       /* layer, alternate_group */
    be16(fp, 0x0100);               /* volume */
    be16(fp, 0);                    /* reserved */
    write_matrix(fp);
    be32(fp, (uint32_t)((unsigned)s->w << 16));
    be32(fp, (uint32_t)((unsigned)s->h << 16));

    /* ---- mdia ---- */
    box_hdr(fp, mdia_size, "mdia");

    /* ---- mdhd ---- */
    box_hdr(fp, mdhd_size, "mdhd");
    be32(fp, 0);
    be32(fp, 0); be32(fp, 0);
    be32(fp, 90000);                /* video timescale */
    be32(fp, duration);
    be16(fp, 0x55C4);               /* language = und */
    be16(fp, 0);

    /* ---- hdlr ---- */
    box_hdr(fp, hdlr_size, "hdlr");
    be32(fp, 0);
    be32(fp, 0);                    /* pre_defined */
    fwrite("vide", 1, 4, fp);       /* handler_type */
    be32(fp, 0); be32(fp, 0); be32(fp, 0);   /* reserved 12 */
    fwrite("VideoHandler", 1, 12, fp);
    be32(fp, 0);

    /* ---- minf ---- */
    box_hdr(fp, minf_size, "minf");

    /* ---- vmhd ---- */
    box_hdr(fp, vmhd_size, "vmhd");
    be32(fp, 1);                    /* version=0, flags=1 */
    be16(fp, 0);                    /* graphicsmode */
    be16(fp, 0); be16(fp, 0); be16(fp, 0);   /* opcolor */

    /* ---- dinf / dref ---- */
    box_hdr(fp, dinf_size, "dinf");
    box_hdr(fp, 28, "dref");
    be32(fp, 0);
    be32(fp, 1);                    /* entry_count */
    box_hdr(fp, 12, "url ");
    be32(fp, 1);                    /* flags=1 (self-contained) */

    /* ---- stbl ---- */
    box_hdr(fp, stbl_size, "stbl");

    /* ---- stsd：H.264 VisualSampleEntry（'avc1'）+ avcC ---- */
    box_hdr(fp, stsd_size, "stsd");
    be32(fp, 0);
    be32(fp, 1);                    /* entry_count */
    box_hdr(fp, avc1_size, "avc1");
    be32(fp, 0); be16(fp, 1);       /* reserved 6 + data_reference_index */
    be16(fp, 0); be16(fp, 0);       /* pre_defined + reserved */
    be32(fp, 0); be32(fp, 0); be32(fp, 0);   /* pre_defined 12 */
    be16(fp, (uint16_t)s->w);
    be16(fp, (uint16_t)s->h);
    be32(fp, 0x00480000u);          /* horizresolution 72dpi */
    be32(fp, 0x00480000u);          /* vertresolution */
    be32(fp, 0);                    /* reserved */
    be16(fp, 1);                    /* frame_count */
    fwrite("rkvenc\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 1, 32, fp);
    be16(fp, 24);                   /* depth */
    be16(fp, 0xFFFFu);              /* pre_defined */

    /* avcC */
    box_hdr(fp, avcC_size, "avcC");
    fputc(1, fp);                               /* configurationVersion */
    fputc((int)(s->sps_len > 0 ? s->sps[1] : 66), fp);      /* AVCProfileIndication */
    fputc((int)(s->sps_len > 0 ? s->sps[2] : 0), fp);       /* profile_compatibility */
    fputc((int)(s->sps_len > 0 ? s->sps[3] : 30), fp);      /* AVCLevelIndication */
    fputc(0xFF, fp);                            /* 0xFC|lengthSizeMinusOne=3（4 字节） */
    fputc(0xE1, fp);                            /* 0xE0|numSPS=1 */
    be16(fp, (uint16_t)s->sps_len);
    fwrite(s->sps, 1, s->sps_len, fp);
    fputc(1, fp);                               /* numPPS */
    be16(fp, (uint16_t)s->pps_len);
    fwrite(s->pps, 1, s->pps_len, fp);

    /* ---- stts：一个 entry，平均帧间隔 ---- */
    box_hdr(fp, stts_size, "stts");
    be32(fp, 0);
    be32(fp, 1);
    be32(fp, N);
    be32(fp, delta);

    /* ---- stsc：每 chunk 1 样本 ---- */
    box_hdr(fp, stsc_size, "stsc");
    be32(fp, 0);
    be32(fp, 1);
    be32(fp, 1);                    /* first_chunk */
    be32(fp, 1);                    /* samples_per_chunk */
    be32(fp, 1);                    /* sample_description_index */

    /* ---- stsz ---- */
    box_hdr(fp, stsz_size, "stsz");
    be32(fp, 0);
    be32(fp, 0);                    /* sample_size = 0（逐样本） */
    be32(fp, N);
    for (uint32_t i = 0; i < N; i++) be32(fp, s->samples[i].size);

    /* ---- stco：绝对文件偏移 ---- */
    box_hdr(fp, stco_size, "stco");
    be32(fp, 0);
    be32(fp, N);
    for (uint32_t i = 0; i < N; i++) be32(fp, s->samples[i].offset);

    /* ---- stss：关键帧样本序号 ---- */
    box_hdr(fp, stss_size, "stss");
    be32(fp, 0);
    be32(fp, K);
    for (uint32_t i = 0; i < K; i++) be32(fp, s->keyframes[i]);
}

/* Annex-B → length-prefixed 写入回调：be32(nlen) + NAL 数据 */
static void nal_write_cb(void *ud, const unsigned char *start, size_t nlen)
{
    struct { FILE *fp; uint32_t written; int ok; } *c = ud;
    if (!c->ok) return;
    be32(c->fp, (uint32_t)nlen);
    if (fwrite(start, 1, nlen, c->fp) == nlen)
        c->written += (uint32_t)nlen + 4u;
    else
        c->ok = 0;
}

/* ---- 对外 API ---- */

rec_mp4_t *rec_mp4_create(const char *dir, const char *prefix, int w, int h,
                          char *name_out, size_t name_size)
{
    if (!dir || !prefix || w <= 0 || h <= 0) return NULL;

    rec_mp4_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    mkdir(dir, 0755);
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    /* 规范化命名：日期已体现在按天目录（dir = .../YYYYMMDD），文件名只保留时间 */
    int idx = 0;
    do {
        if (idx == 0)
            snprintf(s->name, sizeof(s->name), "%s_%02d%02d%02d.mp4",
                     prefix, tm.tm_hour, tm.tm_min, tm.tm_sec);
        else
            snprintf(s->name, sizeof(s->name), "%s_%02d%02d%02d_%d.mp4",
                     prefix, tm.tm_hour, tm.tm_min, tm.tm_sec, idx);
        snprintf(s->path, sizeof(s->path), "%s/%s", dir, s->name);
    } while (access(s->path, F_OK) == 0 && ++idx < 1000);  /* 同秒会话冲突追加序号 */

    s->fp = fopen(s->path, "wb");
    if (!s->fp) { free(s); return NULL; }
    s->w = w; s->h = h;
    s->start_ms = now_ms();
    s->cap_samples = REC_INIT_CAP;
    s->samples = malloc(s->cap_samples * sizeof(rec_sample_t));
    if (!s->samples) { fclose(s->fp); unlink(s->path); free(s); return NULL; }
    s->cap_key = 64;
    s->keyframes = malloc(s->cap_key * sizeof(uint32_t));
    if (!s->keyframes) { fclose(s->fp); unlink(s->path); free(s->samples); free(s); return NULL; }

    mp4_write_ftyp(s->fp);
    /* mdat header：size 占位（finalize 时回填） */
    be32(s->fp, 0);
    fwrite("mdat", 1, 4, s->fp);
    s->mdat_start = 28u;   /* = ftyp 总大小 */

    if (name_out && name_size) snprintf(name_out, name_size, "%s", s->name);
    return s;
}

/* SPS/PPS 收集回调：type 7 → sps，type 8 → pps */
static void sps_pps_cb(void *ud, const unsigned char *start, size_t nlen)
{
    rec_mp4_t *s = ud;
    if (s->avcC_written) return;
    unsigned char type = start[0] & 0x1F;
    if (type == 7 && nlen <= sizeof(s->sps) && s->sps_len == 0) {
        memcpy(s->sps, start, nlen);
        s->sps_len = (unsigned int)nlen;
    } else if (type == 8 && nlen <= sizeof(s->pps) && s->pps_len == 0) {
        memcpy(s->pps, start, nlen);
        s->pps_len = (unsigned int)nlen;
    }
    if (s->sps_len && s->pps_len) s->avcC_written = 1;
}

/* 首次遇到 SPS/PPS 时保存（供 avcC） */
static void rec_save_sps_pps(rec_mp4_t *s, const unsigned char *d, size_t len)
{
    nal_walk(d, len, sps_pps_cb, s);
}

int rec_mp4_write_frame(rec_mp4_t *s, const unsigned char *h264, size_t len,
                        int keyframe, uint64_t ts_ms)
{
    if (!s || !s->fp || !h264) return -1;
    if (s->n_samples >= REC_MAX_SAMPLES ||
        s->data_len + (uint32_t)len > REC_MAX_MDAT)
        return -1;

    rec_save_sps_pps(s, h264, len);

    if (s->n_samples >= s->cap_samples) {
        uint32_t ncap = s->cap_samples * 2;
        rec_sample_t *ns = realloc(s->samples, ncap * sizeof(rec_sample_t));
        if (!ns) return -1;
        s->samples = ns;
        s->cap_samples = ncap;
    }
    if (keyframe) {
        if (s->n_key >= s->cap_key) {
            uint32_t ncap = s->cap_key * 2;
            uint32_t *nk = realloc(s->keyframes, ncap * sizeof(uint32_t));
            if (!nk) return -1;
            s->keyframes = nk;
            s->cap_key = ncap;
        }
        s->keyframes[s->n_key++] = s->n_samples + 1;   /* 1-based */
    }

    /* Annex-B → 4 字节长度前缀（avc1 规范），逐 NAL 写入并统计实际大小 */
    long off = ftell(s->fp);
    if (off < 0) return -1;
    struct { FILE *fp; uint32_t written; int ok; } w = { s->fp, 0, 1 };
    nal_walk(h264, len, nal_write_cb, &w);
    if (!w.ok || w.written == 0) return -1;   /* 写失败或无有效 NAL，丢弃 */
    uint32_t written = w.written;

    s->samples[s->n_samples].offset = (uint32_t)off;
    s->samples[s->n_samples].size   = written;
    s->n_samples++;
    s->data_len += written;
    if (s->n_samples == 1) s->first_ts_ms = ts_ms;
    s->last_ts_ms = ts_ms;
    return 0;
}

int rec_mp4_finalize(rec_mp4_t *s)
{
    if (!s) return -1;
    int rc = -1;
    if (s->fp) {
        /* 回填 mdat 总大小（ftyp 之后） */
        if (s->n_samples > 0 && s->sps_len && s->pps_len) {
            long cur = ftell(s->fp);
            fseek(s->fp, (long)s->mdat_start, SEEK_SET);
            be32(s->fp, 8u + s->data_len);
            fseek(s->fp, cur, SEEK_SET);

            /* 平均帧间隔：timescale 90000，1ms = 90 ticks（不写死，按实际帧间隔） */
            uint32_t delta = 3000;   /* 兜底 30fps */
            if (s->n_samples > 1) {
                uint64_t span = s->last_ts_ms - s->first_ts_ms;
                uint64_t avg  = span / (s->n_samples - 1);
                if (avg > 0) {
                    delta = (uint32_t)(avg * 90u);   /* avg ms × 90 ticks/ms */
                    if (delta == 0) delta = 1;
                }
            }
            mp4_write_moov(s->fp, s, s->n_samples, delta, s->n_key);
            rc = 0;
        }
        fclose(s->fp);
        s->fp = NULL;
    }
    free(s->samples);
    free(s->keyframes);
    s->samples = NULL;
    s->keyframes = NULL;
    if (rc != 0) unlink(s->path);   /* 空录制/缺 SPS/PPS/失败：删除残缺文件 */
    free(s);
    return rc;
}

const char *rec_mp4_name(const rec_mp4_t *s)
{
    return s ? s->name : "";
}

uint32_t rec_mp4_frames(const rec_mp4_t *s)
{
    return s ? s->n_samples : 0;
}

uint32_t rec_mp4_bytes(const rec_mp4_t *s)
{
    return s ? s->data_len : 0;
}

uint64_t rec_mp4_start_ms(const rec_mp4_t *s)
{
    return s ? s->start_ms : 0;
}
