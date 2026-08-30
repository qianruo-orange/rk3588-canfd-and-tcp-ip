/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef YOLO_IMAGE_H
#define YOLO_IMAGE_H

#include <stddef.h>

/**
 * ai/yolo_image.h — 图像处理模块：解码 / 编码 / 色彩空间 / 缩放。
 * 纯函数，无全局状态，可被任意线程并发调用。
 */

/* JPEG → RGB24（解码进调用方缓冲，dst 需 w*h*3 字节；实际尺寸与 w/h 不符
   返回 -1）。零拷贝解码路径：ai_task 把采集帧直接解码进环形队列池缓冲，
   免去中间 malloc + 整帧 memcpy */
int yolo_jpeg_to_rgb_buf(const unsigned char *jpeg, size_t jpeg_len,
                        unsigned char *dst, int w, int h);

/* RGB24 → JPEG（复用输出缓冲）：写入 *buf（容量 *cap），不足时 realloc 增长，
   成功后回写新的 *buf/*cap。*buf 可为 NULL（首次 malloc）。
   渲染热路径（30fps）复用上一帧退役的 JPEG 缓冲，免每帧 malloc/free */
int yolo_rgb_to_jpeg_reuse(const unsigned char *rgb, int w, int h,
                           unsigned char **buf, size_t *cap, size_t *out_len);

/* YUYV（V4L2）→ RGB24（BT.601），dst 需 w*h*3 字节 */
void yolo_yuyv_to_rgb(const unsigned char *src, int w, int h, unsigned char *dst);

/* RGB24 → NV12（BT.601 limited，Y 全分辨率 + UV 四分之一分辨率交错）；
   dst 需 w*h*3/2 字节。供录像硬件编码链路复用画框帧，免去二次 JPEG 解码 */
void yolo_rgb_to_nv12(const unsigned char *rgb, int w, int h, unsigned char *nv12);

/* RGB24 letterbox+padding（OpenCV SIMD）：等比例缩放到 dw×dh 内后居中，
   四周填 114 灰边（ultralytics 训练惯例）。输出实际缩放系数与左上偏移，
   供后处理把模型坐标逆映射回原图坐标系（x = (x_m - pad) / scale） */
void yolo_rgb_letterbox(const unsigned char *src, int sw, int sh,
                        unsigned char *dst, int dw, int dh,
                        float *scale_out, int *pad_x_out, int *pad_y_out);

/* RGB24 → NV12（OpenCV SIMD：cvtColor I420 + U/V 平面交错，NEON 加速）。
   替代 yolo_rgb_to_nv12 用于渲染热路径（录像编码零拷贝快照）。 */
void yolo_rgb_to_nv12_fast(const unsigned char *rgb, int w, int h, unsigned char *nv12);

#endif /* YOLO_IMAGE_H */
