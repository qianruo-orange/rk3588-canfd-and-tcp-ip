/**
 * yolo_image.c — 图像处理模块：JPEG 解码/编码（libjpeg-turbo）、
 * YUYV→RGB、RGB 双线性缩放。纯函数，线程安全。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jpeglib.h>
#include <jerror.h>   /* ERREXIT 宏（自定义输出管理器报错用） */

#include "ai/yolo_image.h"

#define YOLO_JPEG_QUALITY 95   /* 高画质：标签文字清晰无压缩伪影；流码率仍远低于原图 */

int yolo_jpeg_to_rgb(const unsigned char *jpeg, size_t jpeg_len,
                     unsigned char **rgb_out, int *w_out, int *h_out)
{
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, (unsigned char *)jpeg, jpeg_len);
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);
    int w = (int)cinfo.output_width, h = (int)cinfo.output_height;
    unsigned char *rgb = malloc((size_t)w * h * 3);
    if (!rgb) {
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }
    /* 行指针直接指向目标缓冲：免去行缓冲 malloc 与每行 w*3 的 memcpy */
    while (cinfo.output_scanline < cinfo.output_height) {
        unsigned int y = cinfo.output_scanline;   /* 读前保存：read_scanlines 返回后 output_scanline 已 +1 */
        JSAMPROW row_ptr = rgb + (size_t)y * w * 3;
        if (jpeg_read_scanlines(&cinfo, &row_ptr, 1) != 1) break;
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    *rgb_out = rgb;
    *w_out = w;
    *h_out = h;
    return 0;
}

/* 自定义 JPEG 输出管理器：写入调用方提供的缓冲，容量不足时 realloc 增长。
   渲染热路径（30fps）复用上一帧退役的 JPEG 缓冲，消除每帧 ~100-200KB
   malloc/free（该尺寸常走 mmap/munmap，syscall 与页表开销明显）。 */
typedef struct {
    struct jpeg_destination_mgr pub;
    unsigned char *buf;
    size_t cap;
} jpeg_reuse_dest_t;

static void reuse_init_dest(j_compress_ptr cinfo)
{
    jpeg_reuse_dest_t *d = (jpeg_reuse_dest_t *)cinfo->dest;
    /* libjpeg-turbo 的 marker 写入是"先写后查"（free_in_buffer 为 0 也直接
       落笔再回调 empty_output_buffer），init 阶段必须保证缓冲非空：
       无缓冲时先分配 64KB 起步容量，后续不足再由 empty_output 翻倍增长 */
    if (!d->buf) {
        d->cap = 65536;
        d->buf = (unsigned char *)malloc(d->cap);
        if (!d->buf) ERREXIT(cinfo, JERR_OUT_OF_MEMORY);
    }
    d->pub.next_output_byte = d->buf;
    d->pub.free_in_buffer = d->cap;
}

static boolean reuse_empty_output(j_compress_ptr cinfo)
{
    jpeg_reuse_dest_t *d = (jpeg_reuse_dest_t *)cinfo->dest;
    size_t used = (size_t)(d->pub.next_output_byte - d->buf);
    size_t ncap = d->cap ? d->cap * 2 : 65536;   /* 首帧无缓冲时给 64KB 起步 */
    unsigned char *nb = (unsigned char *)realloc(d->buf, ncap);
    if (!nb) ERREXIT(cinfo, JERR_OUT_OF_MEMORY);
    d->buf = nb;
    d->pub.next_output_byte = nb + used;
    d->pub.free_in_buffer = ncap - used;
    d->cap = ncap;
    return TRUE;
}

static void reuse_term_dest(j_compress_ptr cinfo)
{
    (void)cinfo;   /* 最终长度由调用方从 next_output_byte - buf 计算 */
}

int yolo_rgb_to_jpeg_reuse(const unsigned char *rgb, int w, int h,
                           unsigned char **buf, size_t *cap, size_t *out_len)
{
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);

    jpeg_reuse_dest_t dest;
    memset(&dest, 0, sizeof(dest));
    dest.buf = buf ? *buf : NULL;
    dest.cap = cap ? *cap : 0;
    dest.pub.init_destination    = reuse_init_dest;
    dest.pub.empty_output_buffer = reuse_empty_output;
    dest.pub.term_destination    = reuse_term_dest;
    cinfo.dest = &dest.pub;

    cinfo.image_width      = (JDIMENSION)w;
    cinfo.image_height     = (JDIMENSION)h;
    cinfo.input_components = 3;
    cinfo.in_color_space   = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, YOLO_JPEG_QUALITY, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    /* 行指针直接指向源缓冲（jpeg_write_scanlines 只读该行）：
       免去行缓冲 malloc 与每行 w*3 的 memcpy */
    while (cinfo.next_scanline < cinfo.image_height) {
        JSAMPROW row_ptr = (JSAMPROW)(rgb + (size_t)cinfo.next_scanline * w * 3);
        jpeg_write_scanlines(&cinfo, &row_ptr, 1);
    }
    jpeg_finish_compress(&cinfo);
    size_t len = (size_t)(dest.pub.next_output_byte - dest.buf);
    jpeg_destroy_compress(&cinfo);
    if (buf) *buf = dest.buf;
    if (cap) *cap = dest.cap;
    if (out_len) *out_len = len;
    return 0;
}

/* RGB24 → JPEG（malloc 输出）；成功返回 0，调用方 free *out。
   热路径（渲染任务 30fps）请用 yolo_rgb_to_jpeg_reuse 复用缓冲 */
int yolo_rgb_to_jpeg(const unsigned char *rgb, int w, int h,
                     unsigned char **out, size_t *out_len)
{
    unsigned char *buf = NULL;
    size_t cap = 0;
    if (yolo_rgb_to_jpeg_reuse(rgb, w, h, &buf, &cap, out_len) != 0)
        return -1;
    *out = buf;
    return 0;
}

void yolo_yuyv_to_rgb(const unsigned char *src, int w, int h, unsigned char *dst)
{
    int npix = w * h / 2;
    for (int i = 0; i < npix; i++) {
        int y0 = src[0], u = src[1] - 128, y1 = src[2], v = src[3] - 128;
        src += 4;
        for (int k = 0; k < 2; k++) {
            int y = k ? y1 : y0;
            /* BT.601 定点系数（×1/1024 ≈ 1.402/0.344/0.714/1.772）：免浮点 */
            int r = y + ((1436 * v) >> 10);
            int g = y - ((352 * u + 731 * v) >> 10);
            int b = y + ((1815 * u) >> 10);
            if (r < 0) r = 0; else if (r > 255) r = 255;
            if (g < 0) g = 0; else if (g > 255) g = 255;
            if (b < 0) b = 0; else if (b > 255) b = 255;
            *dst++ = (unsigned char)r;
            *dst++ = (unsigned char)g;
            *dst++ = (unsigned char)b;
        }
    }
}

void yolo_rgb_to_nv12(const unsigned char *rgb, int w, int h, unsigned char *nv12)
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
