/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RKNN_YOLO_H
#define RKNN_YOLO_H

#include <stddef.h>

#include "ai/yolo_types.h"

/**
 * ai/rknn_yolo.h — RKNN + YOLO26 检测框架（主模块：模型管理 + 多线程推理池）。
 *
 * 模块组成（见 source/ai/ 下各文件）：
 *  - yolo_image.c      图像处理（JPEG 编解码 / YUYV→RGB / 缩放）
 *  - yolo_postprocess.c YOLO26 单输出模式后处理（sigmoid + stride + NMS）
 *  - yolo_draw.c       画框 + 标签 + JPEG 编码
 *  - rknn_yolo.c       模型加载、推理线程池（每线程独立 rknn context，并行推理）、
 *                      任务队列、结果队列（NMS 后处理结果：目标名称 + 置信度 +
 *                      标注框四点坐标）、渲染 composer、结果/画框帧快照
 *
 * 多线程推理：ai_threads 个推理工作线程（3 的倍数 3~15，各绑定 NPU 核 i%3），
 * 独立 rknn context 并行推理。推理后数据流分两路：rgb 写槽（画框显示链路），
 * NMS 结果入结果队列；composer 消费队列最新结果（乱序完成的旧结果丢弃），
 * 按 seq 严格递增做 EMA 平滑 + 渲染，每个视频帧都会产出标注画面。
 *
 * 热重载：rknn_yolo_reload() 停池 → 重读配置（模型/标签/线程数/阈值等）→ 重建。
 *
 * 必要流程：AI 推理不可停用。无模型 / NPU 驱动未加载 / 推理失败 → 直接报错：
 * 启动路径整体失败退出；运行期热重载失败由 HTTP 接口报告，画框流返回 503。
 * 所有失败路径只记日志不崩溃。
 */

/* 初始化：读配置加载模型并创建推理线程池；返回 0 成功。
   模型缺失/NPU 不可用返回 -1（无 AI 直接报错，服务启动失败） */
int rknn_yolo_init(void *arg);
void rknn_yolo_destroy(void *arg);
int rknn_yolo_enabled(void);

/* 热重载推理池（停止 → 重读配置 → 重建）；返回 0 = 新池可用，-1 = 未运行/降级 */
int rknn_yolo_reload(void);

/* AI 工作线程（main 模块框架 task）：采集帧 → 解码 → 投递推理队列 */
void *rknn_ai_task(void *arg);

/* 拷贝最新检测结果；@return 检测数 */
int rknn_yolo_get(yolo_result_t *out);

/* 拷贝最新画框 JPEG 帧（调用方 free *data）；@return 0 成功，-1 无可用帧 */
int rknn_yolo_get_frame(unsigned char **data, size_t *len, unsigned long long *seq);

/* 轻量查询最新画框帧序号（无新帧拷贝开销）；@return 帧序号，0 表示尚无画框帧 */
unsigned long long rknn_yolo_get_frame_seq(void);

#endif /* RKNN_YOLO_H */
