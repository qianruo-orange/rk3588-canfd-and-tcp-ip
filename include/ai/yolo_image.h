#ifndef YOLO_IMAGE_H
#define YOLO_IMAGE_H

#include <stddef.h>

/**
 * ai/yolo_image.h — 图像处理模块：解码 / 编码 / 色彩空间 / 缩放。
 * 纯函数，无全局状态，可被任意线程并发调用。
 */

/* JPEG → RGB24（libjpeg-turbo）；成功返回 0，调用方 free *rgb_out */
int yolo_jpeg_to_rgb(const unsigned char *jpeg, size_t jpeg_len,
                     unsigned char **rgb_out, int *w_out, int *h_out);

/* RGB24 → JPEG；成功返回 0，调用方 free *out */
int yolo_rgb_to_jpeg(const unsigned char *rgb, int w, int h,
                     unsigned char **out, size_t *out_len);

/* YUYV（V4L2）→ RGB24（BT.601），dst 需 w*h*3 字节 */
void yolo_yuyv_to_rgb(const unsigned char *src, int w, int h, unsigned char *dst);

/* RGB24 双线性缩放（全图拉伸，非 letterbox）；dst 需 dw*dh*3 字节 */
void yolo_rgb_resize(const unsigned char *src, int sw, int sh,
                     unsigned char *dst, int dw, int dh);

/* RGB24 → NV12（BT.601 limited，Y 全分辨率 + UV 四分之一分辨率交错）；
   dst 需 w*h*3/2 字节。供录像硬件编码链路复用画框帧，免去二次 JPEG 解码 */
void yolo_rgb_to_nv12(const unsigned char *rgb, int w, int h, unsigned char *nv12);

/* RGB24 双线性缩放（OpenCV SIMD 实现，cv::resize INTER_LINEAR，NEON 加速）。
   替代 yolo_rgb_resize 用于 1080p→640 等大图缩小热路径（推理预处理）。 */
void yolo_rgb_resize_fast(const unsigned char *src, int sw, int sh,
                          unsigned char *dst, int dw, int dh);

/* RGB24 → NV12（OpenCV SIMD：cvtColor I420 + U/V 平面交错，NEON 加速）。
   替代 yolo_rgb_to_nv12 用于 1080p 渲染热路径（录像编码零拷贝快照）。 */
void yolo_rgb_to_nv12_fast(const unsigned char *rgb, int w, int h, unsigned char *nv12);

#endif /* YOLO_IMAGE_H */
