/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HEVC_FEED_H
#define HEVC_FEED_H

#include <stddef.h>
#include <stdint.h>

/**
 * video/hevc_feed.h — 录制 H.265 编码输出扇出到命名管道（FIFO）。
 *
 * 录制线程（唯一生产者）把每帧 Annex-B 编码结果所有权移交本模块的
 * 内部队列；独立写线程以阻塞写送入 FIFO，供 ffmpeg 进程读取并推
 * RTSP 到 mediamtx（浏览器 WebRTC 播放链）。
 *
 * 背压策略：队列满时丢弃最旧帧（直播优先新鲜度，不阻塞录制线程）；
 * FIFO 无读者（ffmpeg 未启动/重启中）时写线程等待重开，帧按队列容量丢弃。
 */

/* 模块初始化（video_rec_init 调用）：创建 FIFO 并启动写线程 */
int hevc_feed_init(void);

/* 录制线程调用（每帧，编码成功后）：参数集副本（供关键帧前重插）。
   仅在空位时写入（首个参数集锁定）：rkmpp 首帧与后续的 SPS 存在字节级
   差异，重插值必须会话内恒定，否则 ffmpeg/mediamtx 会看到参数集变化
   而生成多值 sprop（mediamtx 拒绝）。 */
void hevc_feed_set_ps(const unsigned char *vps, unsigned int vps_len,
                      const unsigned char *sps, unsigned int sps_len,
                      const unsigned char *pps, unsigned int pps_len);

/* 录制线程调用（新会话开始时）：清空锁定，重新采集本会话参数集 */
void hevc_feed_reset_ps(void);

/* 录制线程调用（每帧）：annexb 所有权移交本模块（勿再 free）。
   失败/队列满时内部直接丢弃（含释放），不阻塞 */
void hevc_feed_push(unsigned char *annexb, size_t len, int keyframe);

/* 模块停止（video_rec_destroy 调用）：置停止标志、唤醒写线程并回收 */
void hevc_feed_destroy(void);

#endif /* HEVC_FEED_H */
