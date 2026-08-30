/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef VIDEO_REC_H
#define VIDEO_REC_H

#include <stddef.h>

/**
 * video/video_rec.h — 网络录像模块（Web/HTTP 控制，AI 画框帧优先）。
 *
 * 数据流：
 *   HTTP API（/api/rec/start|stop）→ 录制线程（main 模块表 "rec"）
 *     拉取 AI 画框帧（rknn_yolo_get_frame，JPEG）；AI 未启用或尚无画框帧时
 *     回退原始视频帧（MJPEG 直接用，YUYV 转 JPEG）→ 封装为 MP4（MJPEG track）
 *     写入 recordings/ 目录，停止时 finalize（写 moov）。
 *
 * MP4 采用标准 ISO BMFF：ftyp + mdat（全部 JPEG 帧）+ moov（末尾回写，
 * stbl 记录每帧偏移/大小/时长），无需硬件编码器，通用播放器可播。
 */

/* 录制状态快照（供 HTTP status 接口 / 前端轮询） */
typedef struct {
    int               recording;    /* 1 = 录制中 */
    char              file[128];    /* 当前录制文件名（仅文件名，空 = 无） */
    unsigned long long start_ms;    /* 开始时刻（CLOCK_MONOTONIC ms） */
    unsigned long long frames;      /* 已写入帧数 */
    unsigned long long bytes;       /* 已写入数据字节数 */
    double             fps;         /* 实时平均帧率（最后若干帧） */
} video_rec_status_t;

struct app_config_t;

/* 模块生命周期（main 模块表调用）：init 建目录；task 为录制线程（空闲空转喂狗） */
int  video_rec_init(void *arg);
void video_rec_destroy(void *arg);
void *video_rec_task(void *arg);

/* HTTP 控制入口（Reactor 线程调用，线程安全）：
   @return 0 成功，-1 失败（已在录制 / 未在录制 / 参数错误） */
int video_rec_start(void);
int video_rec_stop(void);

/* 结束当前录制段，让线程按最新配置续录下一段（不改变自动续录开关）。
   编码器随会话创建，码率系数等编码参数变更后须调用本函数才会生效；
   已手动停录时为空操作 */
void video_rec_cycle_session(void);

/* 拷贝当前录制状态 */
int video_rec_status(video_rec_status_t *out);

#endif /* VIDEO_REC_H */
