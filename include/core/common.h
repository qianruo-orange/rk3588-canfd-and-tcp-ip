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

#include "can/dbc_parser.h"

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

/* 应用运行时上下文：唯一的共享状态容器，通过指针传递给各线程 / 各模块 */
typedef struct app_ctx {
    atomic_int            running;    /* 运行标志（C11 原子，信号处理器与各线程安全读写） */
    pthread_mutex_t        can_mutex; /* CAN sock_fd 并发修改互斥锁 */
    struct can_ctx        *can;
    struct tcp_ctx        *tcp;
    struct app_config_t   *cfg;
    dbc_t                 *dbcs;      /* DBC 数据库数组（按 CAN 通道索引；空则该通道不解码） */
    pthread_mutex_t        dbc_mutex; /* DBC 数组并发访问互斥锁 */
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
