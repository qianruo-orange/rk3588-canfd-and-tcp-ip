/**
 * rec_mp4.c — 最小 MP4(MJPEG track) 封装器（ISO BMFF，moov 末尾回写）。
 *
 * 文件布局：
 *   ftyp(16) + mdat(8+N 帧 JPEG) + moov(574+8N 字节)
 *   stbl 用 stco 记录每帧绝对偏移 / stsz 记录大小 / stts 平均帧间隔，
 *   无需硬件编码器，VLC / ffplay / 浏览器均按 MJPEG 解码。
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

/* ---- MP4 封装 ---- */

/* 写 ftyp：mp42 品牌，兼容 mp42/mp41/isom。
   盒大小 = 8 头 + major 4 + minor 4 + compatible 12 = 28 字节 */
static void mp4_write_ftyp(FILE *fp)
{
    box_hdr(fp, 28, "ftyp");
    fwrite("mp42", 1, 4, fp);
    be32(fp, 0);
    fwrite("mp42mp41isom", 1, 12, fp);
}

/* 写 moov（文件末尾）：mvhd + trak(tkhd, mdia(mdhd, hdlr, minf(...)))，
   stbl 记录全部样本。@delta 为平均帧间隔（timescale 90000） */
static void mp4_write_moov(FILE *fp, const rec_mp4_t *s, uint32_t n,
                           uint32_t delta)
{
    uint32_t N = n;
    /* moov 总大小 = 564 + 8N（各子盒实际写入字节数推导） */
    uint32_t moov_size = 564u + 8u * N;
    uint32_t duration  = delta * N;

    box_hdr(fp, moov_size, "moov");

    /* ---- mvhd ---- */
    box_hdr(fp, 108, "mvhd");
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
    box_hdr(fp, 448u + 8u * N, "trak");

    /* ---- tkhd ---- */
    box_hdr(fp, 92, "tkhd");
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
    box_hdr(fp, 348u + 8u * N, "mdia");

    /* ---- mdhd ---- */
    box_hdr(fp, 32, "mdhd");
    be32(fp, 0);
    be32(fp, 0); be32(fp, 0);
    be32(fp, 90000);                /* video timescale */
    be32(fp, duration);
    be16(fp, 0x55C4);               /* language = und */
    be16(fp, 0);

    /* ---- hdlr ---- */
    box_hdr(fp, 48, "hdlr");
    be32(fp, 0);
    be32(fp, 0);                    /* pre_defined */
    fwrite("vide", 1, 4, fp);       /* handler_type */
    be32(fp, 0); be32(fp, 0); be32(fp, 0);   /* reserved 12 */
    fwrite("VideoHandler", 1, 12, fp);
    be32(fp, 0);

    /* ---- minf ---- */
    box_hdr(fp, 260u + 8u * N, "minf");

    /* ---- vmhd ---- */
    box_hdr(fp, 20, "vmhd");
    be32(fp, 1);                    /* version=0, flags=1 */
    be16(fp, 0);                    /* graphicsmode */
    be16(fp, 0); be16(fp, 0); be16(fp, 0);   /* opcolor */

    /* ---- dinf / dref ---- */
    box_hdr(fp, 36, "dinf");
    box_hdr(fp, 28, "dref");
    be32(fp, 0);
    be32(fp, 1);                    /* entry_count */
    box_hdr(fp, 12, "url ");
    be32(fp, 1);                    /* flags=1 (self-contained) */

    /* ---- stbl ---- */
    box_hdr(fp, 196u + 8u * N, "stbl");

    /* ---- stsd：MJPEG VisualSampleEntry（'jpeg'） ---- */
    box_hdr(fp, 100, "stsd");
    be32(fp, 0);
    be32(fp, 1);                    /* entry_count */
    box_hdr(fp, 84, "jpeg");        /* VisualSampleEntry 84 字节 */
    be32(fp, 0); be16(fp, 1);       /* reserved 6 + data_reference_index */
    be16(fp, 0); be16(fp, 0);       /* pre_defined + reserved */
    be32(fp, 0); be32(fp, 0); be32(fp, 0);   /* pre_defined 12 */
    be16(fp, (uint16_t)s->w);
    be16(fp, (uint16_t)s->h);
    be32(fp, 0x00480000u);          /* horizresolution 72dpi */
    be32(fp, 0x00480000u);          /* vertresolution */
    be32(fp, 0);                    /* reserved */
    be16(fp, 1);                    /* frame_count */
    fwrite("JPG ", 1, 4, fp);       /* compressorname */
    be64(fp, 0); be64(fp, 0); be64(fp, 0); be32(fp, 0);  /* 剩 28 字节填 0 */
    be16(fp, 24);                   /* depth */
    be16(fp, 0xFFFFu);              /* pre_defined */

    /* ---- stts：一个 entry，平均帧间隔 ---- */
    box_hdr(fp, 24, "stts");
    be32(fp, 0);
    be32(fp, 1);
    be32(fp, N);
    be32(fp, delta);

    /* ---- stsc：每 chunk 1 样本 ---- */
    box_hdr(fp, 28, "stsc");
    be32(fp, 0);
    be32(fp, 1);
    be32(fp, 1);                    /* first_chunk */
    be32(fp, 1);                    /* samples_per_chunk */
    be32(fp, 1);                    /* sample_description_index */

    /* ---- stsz ---- */
    box_hdr(fp, 20u + 4u * N, "stsz");
    be32(fp, 0);
    be32(fp, 0);                    /* sample_size = 0（逐样本） */
    be32(fp, N);
    for (uint32_t i = 0; i < N; i++) be32(fp, s->samples[i].size);

    /* ---- stco：绝对文件偏移 ---- */
    box_hdr(fp, 16u + 4u * N, "stco");
    be32(fp, 0);
    be32(fp, N);
    for (uint32_t i = 0; i < N; i++) be32(fp, s->samples[i].offset);
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
    snprintf(s->name, sizeof(s->name), "%s_%04d%02d%02d_%02d%02d%02d.mp4",
             prefix, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
    snprintf(s->path, sizeof(s->path), "%s/%s", dir, s->name);

    s->fp = fopen(s->path, "wb");
    if (!s->fp) { free(s); return NULL; }
    s->w = w; s->h = h;
    s->start_ms = now_ms();
    s->cap_samples = REC_INIT_CAP;
    s->samples = malloc(s->cap_samples * sizeof(rec_sample_t));
    if (!s->samples) { fclose(s->fp); unlink(s->path); free(s); return NULL; }

    mp4_write_ftyp(s->fp);
    /* mdat header：size 占位（finalize 时回填） */
    be32(s->fp, 0);
    fwrite("mdat", 1, 4, s->fp);
    s->mdat_start = 28u;   /* = ftyp 总大小 */

    if (name_out && name_size) snprintf(name_out, name_size, "%s", s->name);
    return s;
}

int rec_mp4_write_frame(rec_mp4_t *s, const unsigned char *jpeg, size_t len,
                        uint64_t ts_ms)
{
    if (!s || !s->fp) return -1;
    if (s->n_samples >= REC_MAX_SAMPLES ||
        s->data_len + (uint32_t)len > REC_MAX_MDAT)
        return -1;

    if (s->n_samples >= s->cap_samples) {
        uint32_t ncap = s->cap_samples * 2;
        rec_sample_t *ns = realloc(s->samples, ncap * sizeof(rec_sample_t));
        if (!ns) return -1;
        s->samples = ns;
        s->cap_samples = ncap;
    }

    /* 帧数据写入位置 = 文件当前偏移 */
    long off = ftell(s->fp);
    if (off < 0) return -1;
    if (fwrite(jpeg, 1, len, s->fp) != len) return -1;

    s->samples[s->n_samples].offset = (uint32_t)off;
    s->samples[s->n_samples].size   = (uint32_t)len;
    s->n_samples++;
    s->data_len += (uint32_t)len;
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
        if (s->n_samples > 0) {
            long cur = ftell(s->fp);
            fseek(s->fp, (long)s->mdat_start, SEEK_SET);
            be32(s->fp, 8u + s->data_len);
            fseek(s->fp, cur, SEEK_SET);

            /* 平均帧间隔：timescale 90000 */
            uint32_t delta = 3000;   /* 兜底 30fps */
            if (s->n_samples > 1) {
                uint64_t span = s->last_ts_ms - s->first_ts_ms;
                uint64_t avg  = span / (s->n_samples - 1);
                if (avg > 0) {
                    delta = (uint32_t)((90u * avg + 50u) / 100u);
                    if (delta == 0) delta = 1;
                }
            }
            mp4_write_moov(s->fp, s, s->n_samples, delta);
            rc = 0;
        }
        fclose(s->fp);
        s->fp = NULL;
    }
    free(s->samples);
    s->samples = NULL;
    if (rc != 0) unlink(s->path);   /* 空录制/失败：删除残缺文件 */
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
