#ifndef WATCHDOG_H
#define WATCHDOG_H

#include "core/config.h"

/* 看门狗槽位索引：与各工作线程一一对应 */
enum {
    WD_CAN = 0,    /* can_task：CAN 数据接收 */
    WD_TCP,        /* tcp_task：TCP 数据收发 */
    WD_HTTP,       /* http_server_task：HTTP 服务 */
    WD_VIDEO,      /* video_stream_task：视频采集 */
    WD_MAIN,       /* 主线程主循环 */
};

int watchdog_init(void *arg);
void *watchdog_task(void *arg);
void watchdog_feed(int slot);

#endif /* WATCHDOG_H */
