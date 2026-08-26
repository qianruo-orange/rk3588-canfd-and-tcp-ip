#ifndef H264_ENCODER_H
#define H264_ENCODER_H

#include <stddef.h>
#include <stdint.h>

/**
 * video/h264_encoder.h — RK3588 硬件 H.264 编码器封装（V4L2 M2M，rkvenc）。
 *
 * 输入 NV12（Y 平面 w*h + 交错 UV w*h/2），输出 H.264 Annex-B 码流
 * （含 SPS/PPS/IDR/slice，以 00 00 00 01 分隔）。编码器为同步接口：
 * 一次调用编码一帧，内部通过 poll 等待 M2M 队列就绪。
 *
 * 单线程使用（由录制线程独占），无线程安全问题。
 */

typedef struct h264_encoder_s h264_encoder_t;

/* 创建编码器（open /dev/video-enc0，配置 NV12→H264，设置码率 / 帧率 / GOP）。
   @param w/h          帧宽高（需偶数）
   @param fps          期望帧率（用于设置 GOP 与时间戳换算，不写死）
   @param bitrate_bps  目标码率（如 4000000）
   @return 非 NULL 成功；NULL 表示设备不可用（无编码器/参数不支持） */
h264_encoder_t *h264_encoder_create(int w, int h, int fps, int bitrate_bps);

/* 释放编码器（STREAMOFF + 关闭） */
void h264_encoder_destroy(h264_encoder_t *e);

/* 编码一帧 NV12（w*h*3/2 字节）。成功返回 0，*out 为 malloc 的 H.264
   Annex-B 码流（调用方 free），*keyframe=1 表示该帧为 IDR（含 SPS/PPS）。
   失败返回 -1。 */
int h264_encoder_encode(h264_encoder_t *e, const unsigned char *nv12,
                        unsigned char **out, size_t *out_len, int *keyframe);

#endif /* H264_ENCODER_H */
