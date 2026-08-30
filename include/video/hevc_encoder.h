/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HEVC_ENCODER_H
#define HEVC_ENCODER_H

#include <stddef.h>
#include <stdint.h>

/**
 * video/hevc_encoder.h — RK3588 硬件 H.265(HEVC) 编码器封装。
 *
 * 后端自动选择：优先 FFmpeg hevc_rkmpp（Rockchip MPP，编译启用 HAVE_AVCODEC
 * 且运行时可用），否则 V4L2 M2M rkvenc（/dev/video-enc0 或探测到的节点）。
 *
 * 输入 NV12（Y 平面 w*h + 交错 UV w*h/2），输出 H.265 Annex-B 码流
 * （含 VPS/SPS/PPS/IRAP/slice，以 00 00 00 01 分隔）。编码器为同步接口：
 * 一次调用编码一帧。
 *
 * 单线程使用（由录制线程独占），无线程安全问题。
 */

typedef struct hevc_encoder_s hevc_encoder_t;

/* 创建编码器（优先 FFmpeg hevc_rkmpp，回退 V4L2 rkvenc；
   设置码率/帧率/GOP）。
   @param w/h          帧宽高（需偶数）
   @param fps          期望帧率（用于设置 GOP 与时间戳换算，不写死）
   @param bitrate_bps  目标码率（如 4000000）
   @return 非 NULL 成功；NULL 表示设备不可用（无编码器/参数不支持） */
hevc_encoder_t *hevc_encoder_create(int w, int h, int fps, int bitrate_bps);

/* 释放编码器（STREAMOFF + 关闭） */
void hevc_encoder_destroy(hevc_encoder_t *e);

/* 编码一帧 NV12（w*h*3/2 字节）。成功返回 0，*out 为 malloc 的 H.265
   Annex-B 码流（调用方 free），*keyframe=1 表示该帧为 IRAP（随机访问点）。
   失败返回 -1。 */
int hevc_encoder_encode(hevc_encoder_t *e, const unsigned char *nv12,
                        unsigned char **out, size_t *out_len, int *keyframe);

/* 提取最近一次 VPS/SPS/PPS（供 MP4 hvcC 写入）；全部存在返回 0 */
int hevc_encoder_sps_pps(hevc_encoder_t *e,
                         const unsigned char **vps, unsigned int *vps_len,
                         const unsigned char **sps, unsigned int *sps_len,
                         const unsigned char **pps, unsigned int *pps_len);

#endif /* HEVC_ENCODER_H */
