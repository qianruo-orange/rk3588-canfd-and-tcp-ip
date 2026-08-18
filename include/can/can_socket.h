#ifndef CAN_SOCKET_H
#define CAN_SOCKET_H

#include <pthread.h>
#include <linux/can.h>
#include <linux/if.h>
#include "core/config.h"
#include "can/can_queue.h"

/* ---- CAN 收发队列 ----
 * 每个接口各占一个发送队列、一个接收队列（txq[i]/rxq[i] 与 ifaces[i] 一一对应）。
 * RX：can_task 从 socket 读到帧先入 rxq[i]，随后在本轮内排空做 DBC 解码（无独立消费线程）。
 * TX：业务线程压入待发送帧（can_queue_t 接口），can_task 弹出并写 socket；用 tx_efd 唤醒 epoll。 */

/* ---- CAN 子系统上下文 ---- */
typedef struct can_ctx {
    can_iface_t     *ifaces;
    int              count;
    int              epfd;      /* CAN socket 收发 epoll（can_task 线程） */
    can_queue_t      txq[CAN_MAX_IFACES]; /* 每个接口一个发送队列 */
    can_queue_t      rxq[CAN_MAX_IFACES]; /* 每个接口一个接收队列 */
    int              tx_efd;    /* TX eventfd：压入 TX 队列后 write，唤醒 can_task */
} can_ctx_t;

/* CAN 数据收发线程（独立 epoll 管理所有 CAN 接口 + TX eventfd） */
void *can_task(void *arg);

/* 异步发送一帧：立即可写则直接发送；否则入对应接口队列，由 can_task 在 EPOLLOUT 时发送。
 * ifname 为目标接口名。返回 0 表示已入队（稍后发送），>0 表示立即发送字节数，-1 失败。 */
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

#endif /* CAN_SOCKET_H */
