#ifndef CAN_SOCKET_H
#define CAN_SOCKET_H

#include <pthread.h>
#include <linux/can.h>
#include <linux/if.h>
#include "core/config.h"

/* ---- CAN 双队列（RX / TX） ----
 * RX 队列：can_task 从 socket 读到数据后压入，独立消费线程弹出并回调 on_can_rx。
 * TX 队列：业务线程压入待发送帧，can_task 弹出并写 socket。
 * 两侧都用 eventfd 唤醒，epoll 检测“有数据压入 / 弹出”。 */
#define CAN_RX_QUEUE_DEPTH 256
#define CAN_TX_QUEUE_DEPTH 64

/* RX 队列条目：保存来源接口名 + 一帧 */
typedef struct {
    char               ifname[IFNAMSIZ];
    struct canfd_frame frame;
} can_rx_item_t;

typedef struct {
    can_rx_item_t items[CAN_RX_QUEUE_DEPTH];
    int head;
    int tail;
    int count;
} can_rx_queue_t;

/* TX 队列（每接口一个，配合 epoll 的 EPOLLOUT 按需发送） */
typedef struct {
    struct canfd_frame frames[CAN_TX_QUEUE_DEPTH];
    int head;
    int tail;
    int count;
} can_tx_queue_t;

/* ---- CAN 子系统上下文 ---- */
typedef struct can_ctx {
    can_iface_t     *ifaces;
    int              count;
    int              epfd;      /* CAN socket 收发 epoll（can_task 线程） */
    can_tx_queue_t   txq[CAN_MAX_IFACES];

    pthread_mutex_t  rx_mutex;  /* RX 队列互斥锁 */
    can_rx_queue_t   rxq;
    int              rx_efd;    /* RX eventfd：压入 RX 队列后 write，唤醒消费线程 */
    int              tx_efd;    /* TX eventfd：压入 TX 队列后 write，唤醒 can_task */
    volatile sig_atomic_t rx_stop; /* RX 消费线程停止标志 */
    pthread_t        rx_tid;    /* RX 消费线程句柄 */
} can_ctx_t;

/* CAN 数据收发线程（独立 epoll 管理所有 CAN 接口 + TX eventfd） */
void *can_task(void *arg);

/* 异步发送一帧：立即可写则直接发送；否则入队，由 can_task 在 EPOLLOUT 时发送。
 * 返回 0 表示已入队（稍后发送），>0 表示立即发送字节数，-1 失败。 */
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
