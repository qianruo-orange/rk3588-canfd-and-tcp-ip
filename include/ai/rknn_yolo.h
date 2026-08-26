#ifndef RKNN_YOLO_H
#define RKNN_YOLO_H

#include <stddef.h>

#include "ai/yolo_types.h"

/**
 * ai/rknn_yolo.h — RKNN + YOLO26 检测框架（主模块：模型管理 + 多线程推理池）。
 *
 * 模块组成（见 source/ai/ 下各文件）：
 *  - yolo_image.c      图像处理（JPEG 编解码 / YUYV→RGB / 缩放）
 *  - yolo_postprocess.c YOLO26 三输出模式后处理（sigmoid + stride + NMS）
 *  - yolo_draw.c       画框 + JPEG 编码
 *  - rknn_yolo.c       模型加载、推理线程池（每线程独立 rknn context，并行推理）、
 *                      任务队列、结果/画框帧快照
 *
 * 多线程推理：配置 ai_threads（1~4，默认 2）个推理工作线程，各自拥有独立的
 * rknn context 对队列中的不同帧并行推理；快照只保留最新 seq 的结果（乱序完成时
 * 按 seq 单调更新），供 /video/mjpeg_ai 推流客户端消费。
 *
 * 优雅降级：无模型 / NPU 驱动未加载 / 推理失败 → enabled=0，原视频流照常，
 * 画框流客户端回退到原始帧。所有失败路径只记日志不崩溃。
 */

/* 初始化：读配置加载模型并创建推理线程池；返回 0 成功（含"未启用"成功态）。
   模型缺失/NPU 不可用只置 enabled=0 并记日志，不阻断程序启动 */
int rknn_yolo_init(void *arg);
void rknn_yolo_destroy(void *arg);
int rknn_yolo_enabled(void);

/* AI 工作线程（main 模块框架 task）：采集帧 → 解码 → 投递推理队列 */
void *rknn_ai_task(void *arg);

/* 拷贝最新检测结果；@return 检测数 */
int rknn_yolo_get(yolo_result_t *out);

/* 拷贝最新画框 JPEG 帧（调用方 free *data）；@return 0 成功，-1 无可用帧 */
int rknn_yolo_get_frame(unsigned char **data, size_t *len, unsigned long long *seq);

/* 轻量查询最新画框帧序号（无新帧拷贝开销）；@return 帧序号，0 表示尚无画框帧 */
unsigned long long rknn_yolo_get_frame_seq(void);

#endif /* RKNN_YOLO_H */
