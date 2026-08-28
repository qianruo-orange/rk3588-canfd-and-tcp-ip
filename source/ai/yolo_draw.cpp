/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * yolo_draw.cpp — 检测框渲染模块：RGB 画框 + 类别标签 + JPEG 编码。
 *
 * 画框与标签直接调用 OpenCV 图像库（cv::rectangle / cv::getTextSize /
 * cv::putText），与官方 cv2 标注脚本同款方案：
 *  - 框：3px 实色边框，LINE_AA 抗锯齿（cv2.rectangle 默认画法）
 *  - 标签：Hershey Simplex 白色粗体字（scale 1.4 / thickness 2 / LINE_AA）
 *    + 实色填充底块（cv2 惯例 4px 内边距），默认挂在框顶外侧，顶部放不下
 *    时落在框内
 *
 * 类别名加载自官方 COCO 格式文件（config/coco.names，每行一个类名），
 * 文件缺失时回退内置 COCO 80 类。纯函数，线程安全（不持有全局状态）。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include "ai/yolo_draw.h"
#include "ai/yolo_image.h"
}

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

/* ---- 内置回退类别名（COCO 80 类，官方顺序） ---- */
static const char *const kCoco80[] = {
    "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck",
    "boat", "traffic light", "fire hydrant", "stop sign", "parking meter", "bench",
    "bird", "cat", "dog", "horse", "sheep", "cow", "elephant", "bear", "zebra",
    "giraffe", "backpack", "umbrella", "handbag", "tie", "suitcase", "frisbee",
    "skis", "snowboard", "sports ball", "kite", "baseball bat", "baseball glove",
    "skateboard", "surfboard", "tennis racket", "bottle", "wine glass", "cup",
    "fork", "knife", "spoon", "bowl", "banana", "apple", "sandwich", "orange",
    "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair", "couch",
    "potted plant", "bed", "dining table", "toilet", "tv", "laptop", "mouse",
    "remote", "keyboard", "cell phone", "microwave", "oven", "toaster", "sink",
    "refrigerator", "book", "clock", "vase", "scissors", "teddy bear",
    "hair drier", "toothbrush",
};

extern "C" {

int yolo_classes_load(const char *path, yolo_classes_t *out)
{
    memset(out, 0, sizeof(*out));
    int n = 0;
    FILE *fp = (path && path[0]) ? fopen(path, "r") : NULL;
    if (fp) {
        char line[YOLO_CLASS_NAME_LEN + 4];
        while (n < YOLO_MAX_CLASSES && fgets(line, sizeof(line), fp)) {
            char *s = line, *e = line + strlen(line);
            while (s < e && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')) s++;
            char *t = e;
            while (t > s && (t[-1] == ' ' || t[-1] == '\t' || t[-1] == '\r' || t[-1] == '\n')) t--;
            if (t == s) continue;   /* 空行 */
            size_t len = (size_t)(t - s);
            if (len >= YOLO_CLASS_NAME_LEN) len = YOLO_CLASS_NAME_LEN - 1;
            memcpy(out->names[n], s, len);
            out->names[n][len] = '\0';
            n++;
        }
        fclose(fp);
    }
    if (n == 0) {   /* 文件缺失或为空：回退内置 COCO 80 类 */
        int m = (int)(sizeof(kCoco80) / sizeof(kCoco80[0]));
        if (m > YOLO_MAX_CLASSES) m = YOLO_MAX_CLASSES;
        for (int i = 0; i < m; i++)
            snprintf(out->names[i], YOLO_CLASS_NAME_LEN, "%s", kCoco80[i]);
        n = m;
    }
    out->count = n;
    return n;
}

/* ---- OpenCV 画框：3px 实色 + LINE_AA 抗锯齿 ---- */

void yolo_draw_box(unsigned char *rgb, int w, int h,
                   int x1, int y1, int x2, int y2, unsigned int color)
{
    /* 注意：缓冲区为 RGB 字节序，OpenCV 按 BGR 解释，
       故 Scalar 分量用 (r, g, b) 顺序构造 */
    cv::Scalar col((int)((color >> 16) & 0xFF),
                   (int)((color >> 8) & 0xFF),
                   (int)(color & 0xFF));
    cv::Mat img(h, w, CV_8UC3, rgb);
    cv::rectangle(img, cv::Point(x1, y1), cv::Point(x2, y2), col, 3, cv::LINE_AA);
}

/* ---- OpenCV SIMD 缩放：推理预处理热路径（替代浮点逐像素实现） ---- */

void yolo_rgb_resize_fast(const unsigned char *src, int sw, int sh,
                          unsigned char *dst, int dw, int dh)
{
    /* 逐通道双线性，通道顺序无关；NEON 加速 */
    cv::Mat s(sh, sw, CV_8UC3, (void *)src);
    cv::Mat d(dh, dw, CV_8UC3, dst);
    cv::resize(s, d, cv::Size(dw, dh), 0, 0, cv::INTER_LINEAR);
}

/* ---- OpenCV SIMD NV12 转换：录像编码链路热路径 ---- */

void yolo_rgb_to_nv12_fast(const unsigned char *rgb, int w, int h, unsigned char *nv12)
{
    /* RGB → I420（NEON 加速）。目标 Mat 行布局：
       Y 0..h-1，U h..h+h/2-1，V h+h/2..h*3/2-1 */
    cv::Mat src(h, w, CV_8UC3, (void *)rgb);
    cv::Mat i420(h * 3 / 2, w, CV_8UC1, nv12);
    cv::cvtColor(src, i420, cv::COLOR_RGB2YUV_I420);

    /* I420 → NV12：U/V 四分之一行交错合并（Y 已就位）。
       OpenCV 单 Mat I420 布局（已实测）：U 行 k 在 Mat 行 h+k/2、行内偏移 (k%2)*hw；
       V 行 k 在 Mat 行 h+hh/2+k/2、行内偏移 (k%2)*hw。NV12 UV 行 r 偏移 w*r。
       自后向前原地交错安全；首行 U 与写入区重叠，先暂存 */
    const int hw = w / 2, hh = h / 2;
    unsigned char *t = (unsigned char *)malloc((size_t)hw);
    if (!t) { yolo_rgb_to_nv12(rgb, w, h, nv12); return; }   /* 兜底：标量实现 */
    for (int r = hh - 1; r >= 0; r--) {
        const unsigned char *up = nv12 + (size_t)w * (h + r / 2) + (size_t)(r % 2) * hw;
        const unsigned char *vp = nv12 + (size_t)w * (h + hh / 2 + r / 2) + (size_t)(r % 2) * hw;
        if (r == 0) { memcpy(t, up, (size_t)hw); up = t; }
        unsigned char *o = nv12 + (size_t)w * h + (size_t)w * r;
        for (int c = 0; c < hw; c++) { o[2 * c] = up[c]; o[2 * c + 1] = vp[c]; }
    }
    free(t);
}

/* ---- OpenCV cv2.putText 风格标签 ---- */

static void yolo_draw_label(cv::Mat &img, int x1, int y1,
                            const char *text, unsigned int color)
{
    const int w = img.cols, h = img.rows;
    cv::Scalar col((int)((color >> 16) & 0xFF),
                   (int)((color >> 8) & 0xFF),
                   (int)(color & 0xFF));

    /* 测量文本尺寸（含下行），按 cv2 惯例 4px 内边距算底块 */
    int baseline = 0;
    cv::Size ts = cv::getTextSize(text, cv::FONT_HERSHEY_SIMPLEX, 1.4, 2, &baseline);
    const int pad = 4;
    int bw = ts.width + pad * 2;
    int bh = ts.height + pad * 2;

    int lx = x1;
    int inside = 0;
    int ly = y1 - bh;                           /* 挂在框顶外侧 */
    if (ly < 0) { ly = y1 + 4; inside = 1; }    /* 顶部空间不足：画在框内 */
    if (inside) {
        /* 框内标签始终让开左边框（3px + AA 光晕），防止底块与边框融合变宽；
           块放不下时整体左移，但绝不越过框左边框（宁超右缘被画面裁剪） */
        lx = x1 + 4;
        if (lx + bw > w && w - bw > x1 + 4) lx = w - bw;
    } else if (lx + bw > w) {
        lx = w - bw;                            /* 块在框上方，不遮竖直边框，可左移 */
    }
    if (lx < 0) lx = 0;
    if (ly + bh > h) ly = h - bh;
    if (ly < 0) { ly = 0; bh = h; }

    /* 实色填充底块 + 白色粗体字（putText 原点即首字符基线） */
    cv::rectangle(img, cv::Rect(lx, ly, bw, bh), col, cv::FILLED);
    cv::putText(img, text, cv::Point(lx + pad, ly + pad + ts.height),
                cv::FONT_HERSHEY_SIMPLEX, 1.4, cv::Scalar(255, 255, 255),
                2, cv::LINE_AA);
}

int yolo_render_annotated(unsigned char *rgb, int w, int h,
                          const yolo_result_t *res,
                          const yolo_classes_t *classes,
                          unsigned char **jpeg_buf, size_t *jpeg_cap,
                          size_t *jpeg_len)
{
    static const unsigned int kColors[] = {
        0xFF3B30, 0x34C759, 0x007AFF, 0xFFCC00,
        0xAF52DE, 0xFF9500, 0x5AC8FA, 0xFF2D55,
        0x00C7BE, 0x8E8E93,
    };
    int ncolors = (int)(sizeof(kColors) / sizeof(kColors[0]));

    /* 原地绘制：调用方（composer）独占 rgb 所有权，无需拷贝整帧
       （1080p 每帧省 6MB 拷贝带宽 + malloc/free） */
    cv::Mat mat(h, w, CV_8UC3, rgb);    /* 包装 RGB 缓冲，不拷贝 */
    for (int i = 0; i < res->count; i++) {
        const yolo_det_t *d = &res->dets[i];
        unsigned int color = kColors[d->cls % ncolors];
        yolo_draw_box(rgb, w, h, (int)d->x1, (int)d->y1, (int)d->x2, (int)d->y2, color);

        /* 标签：类别名 + 置信度（cv2 风格：实色底块 + 白色粗体字） */
        const char *name = "obj";
        if (classes && d->cls >= 0 && d->cls < classes->count)
            name = classes->names[d->cls];
        char label[YOLO_CLASS_NAME_LEN + 16];
        snprintf(label, sizeof(label), "%s %.2f", name, d->conf);
        yolo_draw_label(mat, (int)d->x1, (int)d->y1, label, color);
    }

    size_t jlen = 0;
    if (yolo_rgb_to_jpeg_reuse(rgb, w, h, jpeg_buf, jpeg_cap, &jlen) != 0)
        return -1;
    *jpeg_len = jlen;
    return 0;
}

} /* extern "C" */
