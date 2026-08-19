#ifndef VIDEO_STREAM_H
#define VIDEO_STREAM_H

#include <stddef.h>

/* 初始化 video stream 模块（从 cfg 读取设备和分辨率），不创建线程 */
struct app_config_t;
int video_stream_init(void *arg);
/* 工作线程函数，由 main 创建线程并传入 NULL */
void *video_stream_task(void *arg);
/* 请求模块停止（由 main 在退出前调用） */
void video_stream_shutdown(void *arg);

void video_stream_restart(void);

/* 启动一个 MJPEG 推流线程（每个 HTTP 连接一个，detached）；成功返回 0。
   on_close 在推流线程退出时回调，由调用方负责关闭 fd 与释放资源 */
typedef void (*video_stream_client_close_cb)(int fd);
int video_stream_client_start(int fd, video_stream_client_close_cb on_close);

#endif /* VIDEO_STREAM_H */
