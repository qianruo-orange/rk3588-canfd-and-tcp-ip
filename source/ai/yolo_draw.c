/**
 * yolo_draw.c — 检测框渲染模块：RGB 画框 + JPEG 编码。
 * 纯函数，线程安全（不持有全局状态）。
 */

#include <stdlib.h>
#include <string.h>

#include "ai/yolo_draw.h"
#include "ai/yolo_image.h"

void yolo_draw_box(unsigned char *rgb, int w, int h,
                   int x1, int y1, int x2, int y2, unsigned int color)
{
    int r = (int)((color >> 16) & 0xFF), g = (int)((color >> 8) & 0xFF), b = (int)(color & 0xFF);
    if (x1 < 0) x1 = 0; if (y1 < 0) y1 = 0;
    if (x2 >= w) x2 = w - 1; if (y2 >= h) y2 = h - 1;
    if (x2 <= x1 || y2 <= y1) return;

    for (int y = y1; y <= y2; y++) {
        for (int x = x1; x <= x2; x++) {
            int edge = (x - x1 < 2 || x2 - x < 2 || y - y1 < 2 || y2 - y < 2);
            if (!edge) continue;
            unsigned char *p = rgb + ((size_t)y * w + x) * 3;
            p[0] = (unsigned char)r; p[1] = (unsigned char)g; p[2] = (unsigned char)b;
        }
    }
    /* 左上角色块：无字体时用色块标记类别 */
    int tw = x2 - x1 < 16 ? x2 - x1 : 16;
    int th = y2 - y1 < 8  ? y2 - y1 : 8;
    for (int y = y1; y < y1 + th; y++)
        for (int x = x1; x < x1 + tw; x++) {
            unsigned char *p = rgb + ((size_t)y * w + x) * 3;
            p[0] = (unsigned char)r; p[1] = (unsigned char)g; p[2] = (unsigned char)b;
        }
}

int yolo_render_annotated(const unsigned char *rgb, int w, int h,
                          const yolo_result_t *res,
                          unsigned char **jpeg_out, size_t *jpeg_len)
{
    static const unsigned int kColors[] = {
        0xFF3B30, 0x34C759, 0x007AFF, 0xFFCC00,
        0xAF52DE, 0xFF9500, 0x5AC8FA, 0xFF2D55,
        0x00C7BE, 0x8E8E93,
    };
    int ncolors = (int)(sizeof(kColors) / sizeof(kColors[0]));
    unsigned char *img = malloc((size_t)w * h * 3);
    if (!img) return -1;
    memcpy(img, rgb, (size_t)w * h * 3);
    for (int i = 0; i < res->count; i++) {
        const yolo_det_t *d = &res->dets[i];
        yolo_draw_box(img, w, h, (int)d->x1, (int)d->y1, (int)d->x2, (int)d->y2,
                      kColors[d->cls % ncolors]);
    }

    unsigned char *jpeg = NULL;
    size_t jlen = 0;
    if (yolo_rgb_to_jpeg(img, w, h, &jpeg, &jlen) != 0 || !jpeg) {
        free(img);
        return -1;
    }
    free(img);
    *jpeg_out = jpeg;
    *jpeg_len = jlen;
    return 0;
}
