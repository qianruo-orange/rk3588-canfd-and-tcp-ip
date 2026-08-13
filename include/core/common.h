#ifndef COMMON_H
#define COMMON_H

#include <linux/can.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#define WD_MAX_SLOTS 8

/* 公共路径 */
#define PATH_CONFIG   "config/config.txt"
#define PATH_LOGS     "logs"
#define PATH_WEBROOT  "html"

/* 网络监控接口 */
#define NET_IFACE_ETH  "eth0"
#define NET_IFACE_WLAN "wlan0"

struct can_ctx;
struct tcp_ctx;
struct app_config_t;
struct app_ctx;

/* 数据流收发虚函数表（C 风格虚函数：可整体替换或逐项覆盖）
 * 默认实现见 data_flow.c：CAN/TCP 两域经解耦队列交互，互不直接调用；
 * 通过 data_flow_register() 注册，ops 为 NULL 时恢复默认实现。 */
typedef struct data_flow_ops {
    /* CAN 域钩子：can_task 每读到一帧调用一次（can_mutex 持有下）。
       默认实现为空（CAN 数据流独立，不转发到 TCP）；业务可覆盖自行处理。 */
    int (*on_can_rx)(struct app_ctx *app, const char *ifname,
                     const struct canfd_frame *frame);

    /* TCP 域钩子：tcp_task 每收到客户端数据调用一次（client_mutex 持有下）。
       默认实现为空（TCP 数据流独立，不发送到 CAN）；业务可覆盖自行处理。 */
    int (*on_tcp_rx)(struct app_ctx *app, int client_idx,
                     const void *buf, size_t len);

    /* 各域发送原语（供业务主动收发使用）：
       tx_can   CAN 域发送一帧到指定接口；
       tx_tcp   TCP 域发送数据，client_idx < 0 表示广播所有客户端。 */
    ssize_t (*tx_can)(struct app_ctx *app, const char *ifname,
                      const struct canfd_frame *frame, int timeout_ms);
    ssize_t (*tx_tcp)(struct app_ctx *app, int client_idx,
                      const void *buf, size_t len, int timeout_ms);
} data_flow_ops_t;

/* 应用运行时上下文：唯一的共享状态容器，通过指针传递给各线程 / 各模块 */
typedef struct app_ctx {
    volatile sig_atomic_t running;    /* 运行标志（替代 g_running） */
    pthread_mutex_t        can_mutex; /* CAN sock_fd 并发修改互斥锁 */
    struct can_ctx        *can;
    struct tcp_ctx        *tcp;
    struct app_config_t   *cfg;
    const data_flow_ops_t *flow;      /* 数据流虚函数实现（默认各域独立，不做桥接） */
    _Atomic int            threads_running; /* 活跃工作线程数（线程退出时递减） */
} app_ctx_t;

static inline void safe_strncpy(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    snprintf(dst, dst_size, "%s", src);
}

static inline int parse_int_clamped(const char *s, int min_v, int max_v, int def_v)
{
    if (!s || !*s) return def_v;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s) return def_v;
    if (v < min_v) return min_v;
    if (v > max_v) return max_v;
    return (int)v;
}

#endif /* COMMON_H */
