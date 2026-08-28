/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef YOLO_POSTPROCESS_H
#define YOLO_POSTPROCESS_H

#include <stdint.h>

#include "ai/rknn_api.h"
#include "ai/yolo_types.h"

/**
 * ai/yolo_postprocess.h — YOLO26 单输出模式后处理（官方 ultralytics rknn 导出格式）。
 * 纯函数，无全局状态，可被任意线程并发调用。
 */

/* 单输出模型后处理：框已解码（像素坐标）、分数已 sigmoid，仅做阈值过滤 + 类别内 NMS。
   @out_buf  输出的 float 数据（rknn_outputs_get 的 buf）
   @attrs    输出张量属性（决定布局 NCHW/NHWC 与尺寸）
   @n_output 必须为 1（官方 rknn 单输出），否则视为模型不匹配返回 0
   @lb_scale/@lb_pad_x/@lb_pad_y 预处理 letterbox 几何：模型坐标经
              (x - pad) / scale 逆映射回原图坐标系
   @frame_w/@frame_h 原图尺寸（结果坐标所属坐标系）
   @nc_out   可选：输出推断出的类别数
   @return 检测数（已按 conf 过滤、NMS 抑制并映射回原图坐标） */
int yolo_postprocess(const float *const *out_buf, const rknn_tensor_attr *attrs,
                     uint32_t n_output,
                     float lb_scale, int lb_pad_x, int lb_pad_y,
                     int frame_w, int frame_h,
                     yolo_det_t *dets, int max_dets,
                     float conf, float nms, int *nc_out);

#endif /* YOLO_POSTPROCESS_H */
