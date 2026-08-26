#ifndef YOLO_POSTPROCESS_H
#define YOLO_POSTPROCESS_H

#include <stdint.h>

#include "rknn/rknn_api.h"
#include "ai/yolo_types.h"

/**
 * ai/yolo_postprocess.h — YOLO26 三输出模式后处理（YOLOv8 风格）。
 * 纯函数，无全局状态，可被任意线程并发调用。
 */

/* 三输出头模型后处理：sigmoid + stride 解码 + 类别内 NMS。
   @out_buf  各输出的 float 数据（rknn_outputs_get 的 buf）
   @attrs    各输出的张量属性（决定布局 NCHW/NHWC 与尺寸）
   @n_output 必须为 3（P3/P4/P5），否则视为模型不匹配返回 0
   @in_w/@in_h 模型输入尺寸（用于 stride 推导与坐标缩放）
   @frame_w/@frame_h 原图尺寸（检测框坐标缩放回原图坐标系）
   @nc_out   可选：输出推断出的类别数
   @return 检测数（已按 conf 过滤、NMS 抑制并缩放回原图坐标） */
int yolo_postprocess(const float *const *out_buf, const rknn_tensor_attr *attrs,
                     uint32_t n_output, int in_w, int in_h,
                     int frame_w, int frame_h,
                     yolo_det_t *dets, int max_dets,
                     float conf, float nms, int *nc_out);

#endif /* YOLO_POSTPROCESS_H */
