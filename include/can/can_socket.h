/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CAN_SOCKET_H
#define CAN_SOCKET_H

#include <pthread.h>
#include <linux/can.h>
#include <linux/if.h>
#include "core/config.h"
#include "can/can_queue.h"

/* ---- CAN 收发队列 ----
 * 每个接口各占一个发送队列、一个接收队列（txq[i]/rxq[i] 与 ifaces[i] 一一对应）。
 * RX：can_recv_task 从 socket 读到帧先做接收方向 DBC 解析，再入 rxq[i]（只入队、不出队）。
 * TX：业务线程压入待发送帧（can_queue_t 接口），can_send_task 弹出写 socket 后做发送方向 DBC 解析；
 *     用 tx_efd 唤醒 can_send_task。 */

/* ---- CAN 子系统上下文 ---- */
typedef struct can_ctx {
    can_iface_t     *ifaces;
    int              count;
    int              recv_epfd; /* 接收 epoll（can_recv_task） */
    int              send_epfd; /* 发送 epoll（can_send_task，监听 tx_efd + 各接口 EPOLLOUT） */
    int              tx_efd;    /* TX eventfd：压入 TX 队列后 write，唤醒 can_send_task */
} can_ctx_t;

/* CAN 接收线程：读 socket 入 rxq 并做接收方向 DBC 解析 */
void *can_recv_task(void *arg);

/* CAN 发送线程：排空 txq 写 socket 并做发送方向 DBC 解析 */
void *can_send_task(void *arg);

/* 异步发送一帧：入对应接口 TX 队列，由 can_send_task 排空写 socket 并做发送方向 DBC 解析。
 * ifname 为目标接口名。返回 0 表示已入队，-1 失败。 */
int can_tx_frame(app_ctx_t *app, const char *ifname, const struct canfd_frame *frame);

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

/* 查询指定 CAN 接口是否支持 CAN FD，支持返回 1，不支持或查询失败返回 0 */
int  can_fd_supported(const char *ifname);

#endif /* CAN_SOCKET_H */
