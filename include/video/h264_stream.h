/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef H264_STREAM_H
#define H264_STREAM_H

#include <stddef.h>
#include <stdint.h>

/**
 * video/h264_stream.h — 录制 H.264 编码扇出 + fragmented MP4 实时推流（浏览器 MSE）。
 *
 * 录制线程（唯一生产者）把每帧 Annex-B 编码结果推入环形队列；
 * 每个 /video/stream HTTP 连接一个 detached 推流线程（消费者），
 * 从队列接合最新 IDR，发送 ftyp/moov 初始化段 + 逐帧 moof/mdat 片段。
 * 不创建第二个编码器实例：直播与录像共享同一份编码输出
 * （V4L2 回退路径仅单节点 /dev/video-enc0，编码器为录制线程独占）。
 */

/* 推流线程退出时回调（调用方关闭 fd、释放连接资源），恰好调用一次 */
typedef void (*h264_stream_client_close_cb)(int fd);

/* 录制线程调用（会话开始、编码器已重建）：清空环形队列、epoch++、保存新配置。
   sps/pps 可为空（V4L2 后端首 IDR 前未知），由 push_frame 从首帧内嵌
   SPS/PPS 扫描补齐，补齐前客户端无法接合 */
void h264_stream_push_config(int w, int h, int fps,
                             const unsigned char *sps, unsigned int sps_len,
                             const unsigned char *pps, unsigned int pps_len);

/* 录制线程调用（每帧，rec_mp4_write_frame 成功后）：Annex-B 帧拷贝入环 */
void h264_stream_push_frame(const unsigned char *annexb, size_t len,
                            int keyframe, uint64_t ts_ms);

/* 录制线程调用（会话结束/录像停止）：清空环形队列、epoch 归零。
   在流客户端断连，新客户端得到 503（与 mjpeg_ai 依赖 AI 的行为对齐） */
void h264_stream_push_end(void);

/* HTTP 线程调用：为连接创建推流线程（detached）；失败返回 -1 */
int h264_stream_client_start(int fd, h264_stream_client_close_cb on_close);

/* 模块停止（video_rec_destroy 调用）：置停止标志并广播唤醒所有推流线程。
   锁/条件变量随进程存活，不销毁（同 frame_ring 先例） */
void h264_stream_shutdown(void);

#endif /* H264_STREAM_H */
