#ifndef CAN_SOCKET_H
#define CAN_SOCKET_H

#include <linux/can.h>
#include "core/config.h"

/* ---- CAN 子系统上下文 ---- */
typedef struct can_ctx {
    can_iface_t      *ifaces;
    int               count;
    int               epfd;   /* CAN 数据接收专用 epoll */
} can_ctx_t;

/* CAN 数据接收线程（独立 epoll 管理所有 CAN 接口） */
void *can_task(void *arg);

/* 打开/关闭单个 CAN 套接字（can_init 内部使用） */
int  can_socket_open(const char *ifname, int fd_mode);
void can_socket_close(int fd);

/* 设置 CAN 硬件接收过滤器 */
int  can_socket_set_filter(int fd, canid_t id, canid_t mask);

/* 通过 ip link 命令配置 CAN 接口 */
int  can_socket_configure(const char *ifname, int bitrate, int dbitrate,
                          int fd, int restart_ms, int up);

/* 根据配置初始化所有 CAN 接口 */
int  can_init(void *arg);
void can_cleanup(void *ctx);

#endif /* CAN_SOCKET_H */
