#ifndef YOLO_DRAW_H
#define YOLO_DRAW_H

#include <stddef.h>

#include "ai/yolo_types.h"

/**
 * ai/yolo_draw.h — 检测框渲染模块：RGB 画框 + JPEG 编码。
 * 纯函数，无全局状态，可被任意线程并发调用。
 */

/* 在 RGB24 帧上画 2px 边框（左上角色块标记类别，10 色轮转）；
   color 为 0xRRGGBB */
void yolo_draw_box(unsigned char *rgb, int w, int h,
                   int x1, int y1, int x2, int y2, unsigned int color);

/* 拷贝原帧 → 按 res 画框 → 编码 JPEG。
   成功返回 0 并输出 *jpeg_out（调用方 free）；失败返回 -1 */
int yolo_render_annotated(const unsigned char *rgb, int w, int h,
                          const yolo_result_t *res,
                          unsigned char **jpeg_out, size_t *jpeg_len);

#endif /* YOLO_DRAW_H */
