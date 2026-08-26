#ifndef RKNN_YOLO_H
#define RKNN_YOLO_H

#include <stddef.h>

/**
 * ai/rknn_yolo.h — RKNN + YOLO26 检测框架。
 *
 * 模块职责（无独立线程职责，由 rknn_ai_task 驱动）：
 *  - 加载 .rknn 模型（配置 ai_model），查询输入/输出张量属性；
 *  - 对 RGB24 帧做缩放 + NPU 推理 + YOLO26 后处理（无 NMS 端到端单头
 *    为主，兼容经典三头布局，经典布局带 NMS）；
 *  - 把检测框画回 RGB 帧并编码为 JPEG，保存最新"画框帧"快照供推流客户端使用；
 *  - 无模型 / NPU 不可用 / 推理失败：优雅降级（enabled=0，原视频流照常）。
 *
 * 生命周期：rknn_yolo_init（main 模块框架 init）→ rknn_ai_task（线程）
 * → rknn_yolo_destroy（main 模块框架 dtor）。
 */

#define YOLO_MAX_DETS 32

/* 单条检测结果（坐标在原图坐标系，x1<y2 等） */
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

/* 帧像素格式（对应 V4L2_PIX_FMT_*） */
#define RKNN_FMT_MJPEG 0
#define RKNN_FMT_YUYV  1

/* 初始化：读配置加载模型；返回 0 成功（含"未启用"成功态），-1 系统错误。
   模型缺失/NPU 不可用只置 enabled=0 并记日志，不阻断程序启动 */
int rknn_yolo_init(void *arg);
void rknn_yolo_destroy(void *arg);
int rknn_yolo_enabled(void);

/* AI 工作线程（main 模块框架 task）；无模型时立即退出 */
void *rknn_ai_task(void *arg);

/* 对一帧 RGB24 做检测，结果存为最新快照；@return 0 成功（可能 0 个目标），-1 失败 */
int rknn_yolo_detect(const unsigned char *rgb, int w, int h, unsigned long long seq);

/* 拷贝最新检测结果；@return 检测数 */
int rknn_yolo_get(yolo_result_t *out);

/* 拷贝最新画框 JPEG 帧（调用方 free *data）；@return 0 成功，-1 无可用帧 */
int rknn_yolo_get_frame(unsigned char **data, size_t *len, unsigned long long *seq);

/* 轻量查询最新画框帧序号（无新帧拷贝开销）；@return 帧序号，0 表示尚无画框帧 */
unsigned long long rknn_yolo_get_frame_seq(void);

#endif /* RKNN_YOLO_H */
