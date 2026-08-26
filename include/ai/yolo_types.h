#ifndef YOLO_TYPES_H
#define YOLO_TYPES_H

#include <stddef.h>

/* 一帧最多保留的检测框数 */
#define YOLO_MAX_DETS 32

/* 帧像素格式（对应 V4L2_PIX_FMT_*，供解码模块识别） */
#define RKNN_FMT_MJPEG 0
#define RKNN_FMT_YUYV  1

/* 单条检测结果（坐标在原图坐标系，x1<x2, y1<y2） */
typedef struct {
    float x1, y1, x2, y2;
    float conf;
    int   cls;
} yolo_det_t;

/* 一帧的检测结果快照 */
typedef struct {
    int               count;
    unsigned long long seq;   /* 对应的视频采集帧序号 */
    int               w, h;   /* 检测原图尺寸（解码后） */
    yolo_det_t        dets[YOLO_MAX_DETS];
} yolo_result_t;

#endif /* YOLO_TYPES_H */
