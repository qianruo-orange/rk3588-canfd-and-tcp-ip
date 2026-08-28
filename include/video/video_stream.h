#ifndef VIDEO_STREAM_H
#define VIDEO_STREAM_H

#include <stddef.h>

/* 帧像素格式（对应 V4L2_PIX_FMT_*，供 rknn_yolo 等消费方识别） */
#define VIDEO_FMT_MJPEG 0
#define VIDEO_FMT_YUYV  1

/* 初始化 video stream 模块（从 cfg 读取设备和分辨率），不创建线程 */
struct app_config_t;
int video_stream_init(void *arg);
/* 工作线程函数，由 main 创建线程并传入 NULL */
void *video_stream_task(void *arg);
/* 请求模块停止（由 main 在退出前调用） */
void video_stream_shutdown(void *arg);

void video_stream_restart(void);

/* 视频帧环形队列句柄（供 AI 推理/录像消费方直接访问槽位，见 video/frame_ring.h）；
   模块未初始化返回 NULL */
struct frame_ring;
struct frame_ring *video_stream_get_ring(void);

/* 启动一个 MJPEG 推流线程（每个 HTTP 连接一个，detached）；成功返回 0。
   on_close 在推流线程退出时回调，由调用方负责关闭 fd 与释放资源 */
typedef void (*video_stream_client_close_cb)(int fd);
int video_stream_client_start(int fd, video_stream_client_close_cb on_close);

/* 画框流版本：AI 为必要流程，推流线程直接从环形队列读标注帧；
   AI 不可用时返回 503 不回退原始帧。接口与 video_stream_client_start 一致 */
int video_stream_client_start_ai(int fd, video_stream_client_close_cb on_close);

#endif /* VIDEO_STREAM_H */
