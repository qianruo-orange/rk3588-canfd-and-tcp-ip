/**
 * yolo_image.c — 图像处理模块：JPEG 解码/编码（libjpeg-turbo）、
 * YUYV→RGB、RGB 双线性缩放。纯函数，线程安全。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jpeglib.h>

#include "ai/yolo_image.h"

#define YOLO_JPEG_QUALITY 85

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
    unsigned char *row = malloc((size_t)w * 3);
    if (!row) {
        free(rgb);
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }
    while (cinfo.output_scanline < cinfo.output_height) {
        unsigned int y = cinfo.output_scanline;   /* 读前保存：read_scanlines 返回后 output_scanline 已 +1 */
        if (jpeg_read_scanlines(&cinfo, &row, 1) != 1) break;
        memcpy(rgb + (size_t)y * w * 3, row, (size_t)w * 3);
    }
    free(row);
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    *rgb_out = rgb;
    *w_out = w;
    *h_out = h;
    return 0;
}

int yolo_rgb_to_jpeg(const unsigned char *rgb, int w, int h,
                     unsigned char **out, size_t *out_len)
{
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);

    unsigned char *mem = NULL;
    unsigned long memlen = 0;
    jpeg_mem_dest(&cinfo, &mem, &memlen);

    cinfo.image_width      = (JDIMENSION)w;
    cinfo.image_height     = (JDIMENSION)h;
    cinfo.input_components = 3;
    cinfo.in_color_space   = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, YOLO_JPEG_QUALITY, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    unsigned char *row = malloc((size_t)w * 3);
    if (!row) {
        jpeg_destroy_compress(&cinfo);
        if (mem) free(mem);
        return -1;
    }
    while (cinfo.next_scanline < cinfo.image_height) {
        memcpy(row, rgb + (size_t)cinfo.next_scanline * w * 3, (size_t)w * 3);
        jpeg_write_scanlines(&cinfo, &row, 1);
    }
    free(row);
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    *out = mem;
    *out_len = (size_t)memlen;
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
            int r = (int)(y + 1.402f * v);
            int g = (int)(y - 0.344f * u - 0.714f * v);
            int b = (int)(y + 1.772f * u);
            if (r < 0) r = 0; else if (r > 255) r = 255;
            if (g < 0) g = 0; else if (g > 255) g = 255;
            if (b < 0) b = 0; else if (b > 255) b = 255;
            *dst++ = (unsigned char)r;
            *dst++ = (unsigned char)g;
            *dst++ = (unsigned char)b;
        }
    }
}

void yolo_rgb_resize(const unsigned char *src, int sw, int sh,
                     unsigned char *dst, int dw, int dh)
{
    for (int y = 0; y < dh; y++) {
        float sy = (y + 0.5f) * sh / dh - 0.5f;
        if (sy < 0) sy = 0;
        int y0 = (int)sy;
        int y1 = y0 + 1 < sh ? y0 + 1 : y0;
        float fy = sy - y0;
        for (int x = 0; x < dw; x++) {
            float sx = (x + 0.5f) * sw / dw - 0.5f;
            if (sx < 0) sx = 0;
            int x0 = (int)sx;
            int x1 = x0 + 1 < sw ? x0 + 1 : x0;
            float fx = sx - x0;
            const unsigned char *p00 = src + ((size_t)y0 * sw + x0) * 3;
            const unsigned char *p01 = src + ((size_t)y0 * sw + x1) * 3;
            const unsigned char *p10 = src + ((size_t)y1 * sw + x0) * 3;
            const unsigned char *p11 = src + ((size_t)y1 * sw + x1) * 3;
            for (int c = 0; c < 3; c++) {
                float v = p00[c] * (1 - fx) * (1 - fy) + p01[c] * fx * (1 - fy) +
                          p10[c] * (1 - fx) * fy + p11[c] * fx * fy;
                *dst++ = (unsigned char)(v + 0.5f);
            }
        }
    }
}
