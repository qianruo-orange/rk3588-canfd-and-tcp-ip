#ifndef YOLO_DRAW_H
#define YOLO_DRAW_H

#include <stddef.h>

#include "ai/yolo_types.h"

/**
 * ai/yolo_draw.h — 检测框渲染模块：RGB 画框 + 类别标签 + JPEG 编码。
 * 纯函数，无全局状态，可被任意线程并发调用。
 */

/* 加载类别名文件（官方 COCO 格式，每行一个类名，忽略空行）。
   文件缺失或为空时回退内置 COCO 80 类。返回加载到的类别数 */
int yolo_classes_load(const char *path, yolo_classes_t *out);

/* 在 RGB24 帧上画 3px 实色边框（OpenCV cv::rectangle，LINE_AA 抗锯齿）；
   color 为 0xRRGGBB */
void yolo_draw_box(unsigned char *rgb, int w, int h,
                   int x1, int y1, int x2, int y2, unsigned int color);

/* 原地在 rgb 帧上按 res 画框 + 类别名/置信度标签 → 编码 JPEG。
   rgb 缓冲会被原地修改（调用方必须独占所有权；composer 的推理帧满足此条件）。
   classes 为类别名表（可为 NULL，此时标签显示 "obj"）。
   JPEG 编码复用 *jpeg_buf 缓冲（容量 *jpeg_cap，不足 realloc 增长；可为 NULL），
   成功后回写新的 *jpeg_buf/*jpeg_cap 并输出 *jpeg_len；
   失败返回 -1（*jpeg_buf 可能已被 realloc，由调用方持有释放）。 */
int yolo_render_annotated(unsigned char *rgb, int w, int h,
                          const yolo_result_t *res,
                          const yolo_classes_t *classes,
                          unsigned char **jpeg_buf, size_t *jpeg_cap,
                          size_t *jpeg_len);

#endif /* YOLO_DRAW_H */
