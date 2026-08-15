#ifndef CAN_SOCKET_H
#define CAN_SOCKET_H

#include <linux/can.h>
#include <linux/if.h>
#include "core/config.h"

/* ---- CAN 子系统上下文 ---- */
typedef struct can_ctx {
    can_iface_t      *ifaces;
    int               count;
    int               epfd;   /* CAN 数据接收专用 epoll */
} can_ctx_t;

/* CAN 数据接收线程（独立 epoll 管理所有 CAN 接口） */
void *can_task(void *arg);

/* CAN 收发接口预留：统一收发入口，供后续业务逻辑复用 */
ssize_t can_recv_frame(int fd, struct canfd_frame *frame, int timeout_ms);
ssize_t can_send_frame(int fd, const struct canfd_frame *frame, int timeout_ms);

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

/* 枚举系统中实际存在的 CAN 接口（netlink 路由，kind=="can"），返回写入的数量 */
int  can_enumerate_system(char names[][IFNAMSIZ], int max);

#endif /* CAN_SOCKET_H */
