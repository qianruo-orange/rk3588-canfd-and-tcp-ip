#ifndef COMMON_H
#define COMMON_H

#include <linux/can.h>
#include <linux/if.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>

#include "can/dbc_parser.h"

#define WD_MAX_SLOTS 8

/* 公共路径 */
#define PATH_CONFIG   "config/config.txt"
#define PATH_LOGS     "logs"
#define PATH_WEBROOT  "html"
#define PATH_RECORDINGS "recordings"

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

/* 校验网络接口名合法性：字母数字 / 下划线 / 连字符（防止非法名称进入系统命令） */
static inline int ifname_valid(const char *name)
{
    if (!name || !*name) return 0;
    size_t len = strlen(name);
    if (len >= IFNAMSIZ) return 0;
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        if (!isalnum((unsigned char)c) && c != '_' && c != '-') return 0;
    }
    return 1;
}

/* 设置 fd 为非阻塞；成功返回 0，失败返回 -1（保留 errno）。
   tcp/can/http 三层各自把 socket 设为非阻塞的同一套 fcntl 模式收口于此 */
static inline int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* 填充 struct ifreq 的接口名（4 处 ifr_name 赋值统一） */
static inline void ifreq_set_name(struct ifreq *ifr, const char *name)
{
    if (!ifr || !name) return;
    snprintf(ifr->ifr_name, sizeof(ifr->ifr_name), "%s", name);
}

/* 写 eventfd：通知对端“有数据压入队列”（can TX 队列 / tcp TX 队列共用） */
static inline void eventfd_signal(int efd)
{
    if (efd < 0) return;
    uint64_t one = 1;
    (void)write(efd, &one, sizeof(one));
}

/* 读 eventfd：清空通知计数，随后即可弹出队列 */
static inline void eventfd_consume(int efd)
{
    if (efd < 0) return;
    uint64_t v;
    while (read(efd, &v, sizeof(v)) == (ssize_t)sizeof(v)) { }
}

/* 完整写入 fd（阻塞语义，容忍非阻塞 fd）：处理 EINTR / EAGAIN / EWOULDBLOCK
   与部分写入，EAGAIN 时 poll 等待可写最多 3 秒。成功返回 0，失败返回 -1。
   http 非连接上下文的兜底发送与 video 推流线程原为逐字重复，收口于此 */
static inline int fd_write_all_blocking(int fd, const void *data, size_t len)
{
    const char *p = (const char *)data;
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, p + off, len - off);
        if (w > 0) { off += (size_t)w; continue; }
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pfd = { .fd = fd, .events = POLLOUT };
                if (poll(&pfd, 1, 3000) <= 0) return -1;
                continue;
            }
            return -1;
        }
        return -1;   /* write 返回 0：对端异常 */
    }
    return 0;
}

/* 安全追加格式化文本到数组缓冲：写满即把 off 置为缓冲上限并 break（调用方
   继续往下走，与 JSON_ADD 的 return 语义区分）。buf 必须是数组，不能是指针。 */
#define BUF_APPEND(buf, off, fmt, ...) do { \
        int _n = snprintf((buf) + (off), sizeof(buf) - (off), fmt, ##__VA_ARGS__); \
        if (_n < 0 || (size_t)_n >= sizeof(buf) - (off)) { (off) = sizeof(buf); break; } \
        (off) += (size_t)_n; \
    } while (0)

#endif /* COMMON_H */
