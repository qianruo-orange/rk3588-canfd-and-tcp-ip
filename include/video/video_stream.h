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

/* 拷贝当前最新采集帧（MJPEG 或 YUYV 原始数据），调用方 free *out。
   @fmt 输出 VIDEO_FMT_*；@w/@h 为采集分辨率；@seq 为帧序号（从 1 递增）。
   @return 0 成功，-1 尚无帧或参数非法 */
int video_stream_get_frame(unsigned char **out, size_t *out_len, int *fmt,
                           int *w, int *h, unsigned long long *seq);

/* 无拷贝读取当前最新帧序号（从 1 递增；0 = 模块未初始化/尚无帧）。
   轮询消费方（AI 采集 10ms、录像 10-20ms）先用它判断是否有新帧，有变化
   再调 video_stream_get_frame 整帧拷贝，避免每轮轮询 malloc+memcpy
   （720p MJPEG ~100-200KB）后因序号未变而丢弃 */
unsigned long long video_stream_get_frame_seq(void);

/* 启动一个 MJPEG 推流线程（每个 HTTP 连接一个，detached）；成功返回 0。
   on_close 在推流线程退出时回调，由调用方负责关闭 fd 与释放资源 */
typedef void (*video_stream_client_close_cb)(int fd);
int video_stream_client_start(int fd, video_stream_client_close_cb on_close);

/* 画框流版本：AI（RKNN YOLO）可用时优先推画框帧，无新画框帧或 AI 降级时
   回退原始帧；接口与 video_stream_client_start 一致 */
int video_stream_client_start_ai(int fd, video_stream_client_close_cb on_close);

#endif /* VIDEO_STREAM_H */
