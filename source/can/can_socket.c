#define _GNU_SOURCE

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <linux/if.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <linux/can/netlink.h>
#include <netlink/netlink.h>
#include <netlink/socket.h>
#include <netlink/route/link.h>
#include <netlink/route/link/can.h>

#include "can/can_socket.h"
#include "core/common.h"
#include "core/data_flow.h"
#include "core/log.h"
#include "watchdog/watchdog.h"

/**
 * ifname_valid - 检查 CAN 接口名是否合法，避免无效名称导致 netlink 或 socket 配置失败。
 * @name: 待校验的接口名。
 * @return: 合法返回 1，否则返回 0。
 */
static int ifname_valid(const char *name)
{
    if (!name || !*name) return 0;
    size_t len = strlen(name);
    if (len == 0 || len >= IFNAMSIZ) return 0;
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        if (!isalnum((unsigned char)c) && c != '_' && c != '-') return 0;
    }
    return 1;
}

/**
 * apply_dbitrate - 使用 netlink 设置 CAN FD 数据位时钟参数，确保接口工作在指定数据速率。
 * @sk: netlink socket。
 * @ifname: 目标 CAN 接口名。
 * @dbitrate: 数据位速率（bps）。
 */
static void apply_dbitrate(struct nl_sock *sk, const char *ifname, uint32_t dbitrate)
{
    struct rtnl_link *live = NULL;
    if (rtnl_link_get_kernel(sk, 0, ifname, &live) != 0 || !live) return;
    struct can_bittiming_const dbtc;
    if (rtnl_link_can_get_data_bittiming_const(live, &dbtc) == 0) {
        struct can_bittiming dbt = { .bitrate = dbitrate };
        dbt.brp = dbtc.brp_min;
        dbt.prop_seg = dbtc.tseg1_min > 1 ? dbtc.tseg1_min - 1 : 1;
        dbt.phase_seg1 = 1;
        dbt.phase_seg2 = dbtc.tseg2_min < 1 ? 1 : dbtc.tseg2_min;
        dbt.sjw = 1;
        dbt.sample_point = 800;
        rtnl_link_can_set_ctrlmode(live, CAN_CTRLMODE_FD);
        rtnl_link_can_set_data_bittiming(live, &dbt);
        rtnl_link_change(sk, live, live, 0);
        log_info("CAN %s: data bitrate set to %u bps", ifname, dbitrate);
    }
    rtnl_link_put(live);
}

/**
 * can_socket_configure - 配置指定 CAN 设备的比特率、FD 模式和上电状态。
 * @ifname: CAN 接口名。
 * @bitrate: 普通 CAN 比特率。
 * @dbitrate: CAN FD 数据比特率。
 * @fd_mode: 是否启用 CAN FD。
 * @restart_ms: 自动重启等待时间。
 * @bring_up: 是否启动接口。
 * @return: 成功返回 0，失败返回 -1。
 */
int can_socket_configure(const char *ifname, int bitrate, int dbitrate,
                         int fd_mode, int restart_ms, int bring_up)
{
    if (!ifname_valid(ifname)) { log_error("invalid CAN interface name: '%s'", ifname?ifname:"(null)"); return -1; }
    if (bitrate <= 0) bitrate = 500000;

    struct nl_sock *sk = nl_socket_alloc();
    if (!sk) { log_error("nl_socket_alloc"); return -1; }
    if (nl_connect(sk, NETLINK_ROUTE) < 0) { log_error("nl_connect"); nl_socket_free(sk); return -1; }

    /* 用 netlink 探测接口是否已存在；避免 if_nametoindex() 返回 0 后仍走
       rtnl_link_add(NLM_F_CREATE) 而对已存在接口报 "Object busy" */
    struct rtnl_link *existing = NULL;
    int exists = (rtnl_link_get_kernel(sk, 0, ifname, &existing) == 0 && existing != NULL);
    if (existing) rtnl_link_put(existing);

    if (exists) {
        /* 接口已存在，用 ip 命令配置（MCP251xFD 等芯片需要原子操作） */
        char cmd[512];
        size_t off = 0;
        /* 安全追加：缓冲区写满后安全截断，指针/长度均不越界 */
#define APPEND(...) do { \
            int _n = snprintf(cmd + off, sizeof(cmd) - off, __VA_ARGS__); \
            if (_n < 0 || (size_t)_n >= sizeof(cmd) - off) { off = sizeof(cmd); break; } \
            off += (size_t)_n; \
        } while (0)
        APPEND("ip link set %s down 2>/dev/null; ip link set %s type can bitrate %d",
               ifname, ifname, bitrate);
        if (fd_mode) APPEND(" fd on");
        if (fd_mode && dbitrate > 0) APPEND(" dbitrate %d", dbitrate);
        /* 用 ; 而非 &&：ip link set type can 对已有CAN接口可能返回 EEXIST */
        APPEND(" 2>/dev/null");
        if (bring_up) APPEND("; ip link set %s up 2>/dev/null", ifname);
#undef APPEND
        int rc = system(cmd);
        if (rc != 0 && bring_up) {
            /* type can 部分失败但 up 可能成功——验证接口是否真的 up */
            unsigned int flags = 0;
            struct ifreq ifr;
            memset(&ifr, 0, sizeof(ifr));
            snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
            int sock = socket(AF_INET, SOCK_DGRAM, 0);
            if (sock >= 0) {
                ioctl(sock, SIOCGIFFLAGS, &ifr);
                flags = (unsigned int)ifr.ifr_flags;
                close(sock);
            }
            if (!(flags & IFF_UP)) {
                log_error("CAN %s: config via ip command failed", ifname);
                nl_socket_free(sk); return -1;
            }
            log_info("CAN %s: type can returned EEXIST, interface is up anyway", ifname);
        }
        log_info("CAN %s configured (existing), %s", ifname, bring_up ? "up" : "down");
        nl_socket_free(sk);
        return 0;
    }

    /* 接口不存在：创建新 link */
    struct rtnl_link *link = rtnl_link_alloc();
    rtnl_link_set_name(link, ifname);
    rtnl_link_set_type(link, "can");

    rtnl_link_can_set_bitrate(link, (uint32_t)bitrate);
    if (fd_mode) rtnl_link_can_set_ctrlmode(link, CAN_CTRLMODE_FD);
    if (restart_ms > 0) rtnl_link_can_set_restart_ms(link, (uint32_t)restart_ms);

    int ret = rtnl_link_add(sk, link, NLM_F_CREATE);
    if (ret < 0) {
        log_error("CAN %s: configure failed: %s", ifname, nl_geterror(ret));
        rtnl_link_put(link); nl_socket_free(sk); return -1;
    }

    if (fd_mode && dbitrate > 0)
        apply_dbitrate(sk, ifname, (uint32_t)dbitrate);

    if (bring_up) {
        rtnl_link_set_flags(link, IFF_UP);
        if (rtnl_link_change(sk, link, link, 0) < 0)
            log_error("CAN %s: bring up failed", ifname);
    }

    log_info("CAN %s configured (bitrate=%d%s%s) %s", ifname, bitrate,
             fd_mode ? " FD" : "",
             (fd_mode && dbitrate > 0) ? "+dbitrate" : "",
             bring_up ? "up" : "down");
    rtnl_link_put(link);
    nl_socket_free(sk);
    return 0;
}

/**
 * can_socket_open - 打开一个原始 CAN socket，并绑定到指定接口。
 * @ifname: CAN 接口名。
 * @fd_mode: 是否启用 CAN FD 模式。
 * @return: 成功返回 socket fd，失败返回 -1。
 */
int can_socket_open(const char *ifname, int fd_mode)
{
    if (!ifname_valid(ifname)) { log_error("invalid CAN interface name: '%s'", ifname?ifname:"(null)"); return -1; }
    int s = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (s < 0) { log_error("socket(PF_CAN)"); return -1; }
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
    if (ioctl(s, SIOCGIFINDEX, &ifr) < 0) { log_error("ioctl SIOCGIFINDEX for %s", ifname); close(s); return -1; }
    struct sockaddr_can addr;
    memset(&addr, 0, sizeof(addr));
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) { log_error("bind CAN %s", ifname); close(s); return -1; }
    if (fd_mode) {
        int enable = 1;
        if (setsockopt(s, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &enable, sizeof(enable)) < 0)
            log_info("CAN_RAW_FD_FRAMES not available on %s, classic CAN only", ifname);
        else log_info("CAN FD socket enabled on %s", ifname);
    }

    /* 非阻塞：配合 epoll 就绪通知，read/write 在无数据/不可写时返回 EAGAIN 而不是阻塞 */
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0 || fcntl(s, F_SETFL, flags | O_NONBLOCK) < 0) {
        log_error("CAN %s: fcntl O_NONBLOCK: %s", ifname, strerror(errno));
        close(s);
        return -1;
    }

    log_info("CAN socket opened on %s", ifname);
    return s;
}

/**
 * can_socket_close - 关闭已打开的 CAN socket fd。
 * @fd: socket 文件描述符。
 */
void can_socket_close(int fd) { if (fd >= 0) close(fd); }

/* ---- RX / TX 双队列 + eventfd 跨线程唤醒 ---- */

static void rxq_reset(can_rx_queue_t *q)
{
    q->head = q->tail = q->count = 0;
}

static int rxq_push(can_rx_queue_t *q, const char *ifname,
                    const struct canfd_frame *frame)
{
    if (q->count >= CAN_RX_QUEUE_DEPTH) return -1;
    can_rx_item_t *it = &q->items[q->tail];
    safe_strncpy(it->ifname, sizeof(it->ifname), ifname);
    it->frame = *frame;
    q->tail = (q->tail + 1) % CAN_RX_QUEUE_DEPTH;
    q->count++;
    return 0;
}

static int rxq_pop(can_rx_queue_t *q, can_rx_item_t *out)
{
    if (q->count <= 0) return -1;
    *out = q->items[q->head];
    q->head = (q->head + 1) % CAN_RX_QUEUE_DEPTH;
    q->count--;
    return 0;
}

static void txq_reset(can_tx_queue_t *q)
{
    q->head = q->tail = q->count = 0;
}

static int txq_push(can_tx_queue_t *q, const struct canfd_frame *frame)
{
    if (q->count >= CAN_TX_QUEUE_DEPTH) return -1;
    q->frames[q->tail] = *frame;
    q->tail = (q->tail + 1) % CAN_TX_QUEUE_DEPTH;
    q->count++;
    return 0;
}

static int txq_front(can_tx_queue_t *q, struct canfd_frame *frame)
{
    if (q->count <= 0) return -1;
    *frame = q->frames[q->head];
    return 0;
}

static void txq_pop(can_tx_queue_t *q)
{
    if (q->count <= 0) return;
    q->head = (q->head + 1) % CAN_TX_QUEUE_DEPTH;
    q->count--;
}

/* 写 eventfd：通知对端“有数据压入队列” */
static void eventfd_signal(int efd)
{
    if (efd < 0) return;
    uint64_t one = 1;
    (void)write(efd, &one, sizeof(one));
}

/* 读 eventfd：清空通知计数，随后即可弹出队列 */
static void eventfd_consume(int efd)
{
    if (efd < 0) return;
    uint64_t v;
    while (read(efd, &v, sizeof(v)) == (ssize_t)sizeof(v)) { }
}

/**
 * can_epoll_update_tx - 按接口发送队列是否为空，动态切换 epoll 是否监听 EPOLLOUT。
 * 队列非空才监听 EPOLLOUT，避免 socket 一直可写导致 epoll 忙轮询。
 * @ctx: CAN 上下文。
 * @idx: 接口索引。
 */
static void can_epoll_update_tx(can_ctx_t *ctx, int idx)
{
    int fd = ctx->ifaces[idx].sock_fd;
    if (fd < 0) return;
    struct epoll_event ev;
    ev.events = EPOLLIN | (ctx->txq[idx].count > 0 ? EPOLLOUT : 0);
    ev.data.u32 = (uint32_t)(idx + 1);
    if (epoll_ctl(ctx->epfd, EPOLL_CTL_MOD, fd, &ev) < 0 && errno == ENOENT)
        epoll_ctl(ctx->epfd, EPOLL_CTL_ADD, fd, &ev);
}

/**
 * can_tx_frame - 异步发送一帧 CAN 数据。
 * 立即可写则直接发送；不可写则入队，由 can_task 在 EPOLLOUT 事件时发送。
 * @app: 全局应用上下文。
 * @ifname: 目标 CAN 接口名。
 * @frame: 待发送帧。
 * @return: 立即发送返回写入字节数；入队返回 0；失败返回 -1。
 */
int can_tx_frame(app_ctx_t *app, const char *ifname, const struct canfd_frame *frame)
{
    if (!app || !app->can || !ifname || !frame) { errno = EINVAL; return -1; }
    can_ctx_t *ctx = app->can;

    int idx = -1;
    for (int i = 0; i < ctx->count; i++) {
        if (strcmp(ctx->ifaces[i].ifname, ifname) == 0) { idx = i; break; }
    }
    if (idx < 0) { log_error("can_tx: unknown interface '%s'", ifname); errno = ENODEV; return -1; }

    pthread_mutex_lock(&app->can_mutex);
    int fd = ctx->ifaces[idx].sock_fd;
    if (fd < 0) { pthread_mutex_unlock(&app->can_mutex); errno = ENODEV; return -1; }

    ssize_t n = write(fd, frame, sizeof(*frame));
    if (n == (ssize_t)sizeof(*frame)) {
        pthread_mutex_unlock(&app->can_mutex);
        return (int)n;
    }
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        int saved = errno;
        pthread_mutex_unlock(&app->can_mutex);
        errno = saved;
        return -1;
    }

    /* 不可写：压入 TX 队列，写 eventfd 唤醒 can_task 排空 */
    if (txq_push(&ctx->txq[idx], frame) < 0) {
        pthread_mutex_unlock(&app->can_mutex);
        errno = ENOBUFS;
        return -1;
    }
    eventfd_signal(ctx->tx_efd);
    pthread_mutex_unlock(&app->can_mutex);
    return 0;
}

/**
 * handle_can_output - 处理单个 CAN 接口的 EPOLLOUT 事件，尽力排空发送队列。
 * @app: 全局应用上下文。
 * @can_idx: CAN 接口索引。
 */
static void handle_can_output(app_ctx_t *app, int can_idx)
{
    can_ctx_t *ctx = app->can;
    pthread_mutex_lock(&app->can_mutex);
    int fd = ctx->ifaces[can_idx].sock_fd;
    can_tx_queue_t *q = &ctx->txq[can_idx];

    while (fd >= 0 && q->count > 0) {
        struct canfd_frame frame;
        txq_front(q, &frame);
        ssize_t n = write(fd, &frame, sizeof(frame));
        if (n == (ssize_t)sizeof(frame)) { txq_pop(q); continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        txq_pop(q); /* 写异常：丢弃该帧，避免卡死 */
    }

    /* 让 epoll 的 EPOLLOUT 状态与队列是否非空保持一致：
       队列非空才监听 EPOLLOUT，排空则回到仅监听 EPOLLIN */
    can_epoll_update_tx(ctx, can_idx);
    pthread_mutex_unlock(&app->can_mutex);
}

/**
 * handle_can_input - 处理单个 CAN 接口上的输入事件，读取帧并在失败时自动重建 socket。
 * @app: 全局应用上下文。
 * @can_idx: CAN 接口索引。
 */
static void handle_can_input(app_ctx_t *app, int can_idx)
{
    can_ctx_t *ctx = app->can;
    pthread_mutex_lock(&app->can_mutex);
    int fd = ctx->ifaces[can_idx].sock_fd;
    /* 每次 epoll 事件最多排空 64 帧就返回：避免 CAN 总线持续有数据时无限排水，
       导致 can_task 无法回到下一次 epoll_wait 前的 watchdog_feed_self("can")，
       从而被看门狗误判为卡死。 */
    for (int i = 0; i < 64; i++) {
        struct canfd_frame frame;
        ssize_t n = read(fd, &frame, sizeof(frame));
        if (n > 0) {
            /* 压入 RX 队列并写 eventfd 唤醒独立消费线程；由消费线程回调 on_can_rx */
            pthread_mutex_lock(&ctx->rx_mutex);
            if (rxq_push(&ctx->rxq, ctx->ifaces[can_idx].ifname, &frame) == 0)
                eventfd_signal(ctx->rx_efd);
            pthread_mutex_unlock(&ctx->rx_mutex);
        } else if (n == 0) {
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            log_error("can read %s: %s", ctx->ifaces[can_idx].ifname, strerror(errno));
            can_socket_close(fd);
            int new_fd = can_socket_open(ctx->ifaces[can_idx].ifname,
                                          ctx->ifaces[can_idx].fd_mode);
            if (new_fd >= 0) {
                ctx->ifaces[can_idx].sock_fd = new_fd;
                /* 重建 socket 后清空待发队列：旧链路已断，遗留帧不再有意义 */
                txq_reset(&ctx->txq[can_idx]);
                for (int k = 0; k < ctx->ifaces[can_idx].filter_count; k++)
                    can_socket_set_filter(new_fd,
                        ctx->ifaces[can_idx].filters[k].id,
                        ctx->ifaces[can_idx].filters[k].mask);
                can_epoll_update_tx(ctx, can_idx);
                log_info("CAN %s reconnected", ctx->ifaces[can_idx].ifname);
            }
            break;
        }
    }
    pthread_mutex_unlock(&app->can_mutex);
}

/* CAN RX 独立消费线程：epoll 监听 rx_efd，弹出 RX 队列并回调数据流钩子 */
static void *can_rx_consumer(void *arg)
{
    app_ctx_t *app = (app_ctx_t *)arg;
    can_ctx_t *ctx = app->can;
    if (!ctx) return NULL;

    int epfd = epoll_create1(0);
    if (epfd < 0) { log_error("can rx: epoll_create1 failed"); return NULL; }
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.u32 = 0;
    epoll_ctl(epfd, EPOLL_CTL_ADD, ctx->rx_efd, &ev);

    log_info("can rx consumer started");
    while (app->running && !ctx->rx_stop) {
        struct epoll_event evs[8];
        int n = epoll_wait(epfd, evs, 8, 500);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < n; i++) {
            eventfd_consume(ctx->rx_efd);
            /* 排空 RX 队列并逐个回调数据流钩子 */
            for (;;) {
                can_rx_item_t item;
                pthread_mutex_lock(&ctx->rx_mutex);
                int ok = (rxq_pop(&ctx->rxq, &item) == 0);
                pthread_mutex_unlock(&ctx->rx_mutex);
                if (!ok) break;
                if (app->flow && app->flow->on_can_rx)
                    app->flow->on_can_rx(app, item.ifname, &item.frame);
            }
        }
    }
    close(epfd);
    log_info("can rx consumer stopped");
    return NULL;
}

void *can_task(void *arg)
{
    app_ctx_t *app = (app_ctx_t *)arg;
    can_ctx_t *ctx = app->can;
    if (!ctx) return NULL;

    for (int i = 0; i < ctx->count; i++)
        can_epoll_update_tx(ctx, i);
    log_info("can_task started (%d iface(s))", ctx->count);
    while (app->running) {
        struct epoll_event events[64];
        int nfds = epoll_wait(ctx->epfd, events, 64, 500);
        if (nfds < 0) {
            if (errno == EINTR) { watchdog_feed_self("can"); continue; }
            log_error("can epoll_wait"); break;
        }
        watchdog_feed_self("can");
        for (int i = 0; i < nfds; i++) {
            uint32_t tag = events[i].data.u32;
            if (tag == 0) {
                /* TX eventfd：有帧压入 TX 队列，尽力排空所有接口 */
                eventfd_consume(ctx->tx_efd);
                for (int k = 0; k < ctx->count; k++)
                    handle_can_output(app, k);
                continue;
            }
            if (tag < 1 || tag > (uint32_t)ctx->count) continue;
            int idx = (int)(tag - 1);
            uint32_t ev = events[i].events;
            if (ev & (EPOLLIN | EPOLLERR | EPOLLHUP))
                handle_can_input(app, idx);
            if (ev & EPOLLOUT)
                handle_can_output(app, idx);
        }
    }
    log_info("can_task stopped");
    return NULL;
}

int can_socket_set_filter(int fd, canid_t id, canid_t mask)
{
    if ((id & ~CAN_SFF_MASK) || (mask & ~CAN_SFF_MASK)) { id |= CAN_EFF_FLAG; mask |= CAN_EFF_FLAG; }
    struct can_filter filter;
    filter.can_id = id;
    filter.can_mask = mask;
    if (setsockopt(fd, SOL_CAN_RAW, CAN_RAW_FILTER, &filter, sizeof(filter)) < 0) {
        log_error("setsockopt CAN_RAW_FILTER id=%08X mask=%08X", id, mask); return -1;
    }
    log_info("CAN filter set: id=%08X mask=%08X", id, mask);
    return 0;
}

int can_enumerate_system(char names[][IFNAMSIZ], int max)
{
    int count = 0;
    struct nl_sock *sk = nl_socket_alloc();
    if (!sk) return 0;
    if (nl_connect(sk, NETLINK_ROUTE) < 0) { nl_socket_free(sk); return 0; }

    for (int idx = 1; ; idx++) {
        struct rtnl_link *link = NULL;
        if (rtnl_link_get_kernel(sk, idx, NULL, &link) != 0) break;
        if (!link) break;

        const char *kind = rtnl_link_get_type(link);
        if (!kind || strcmp(kind, "can") != 0) { rtnl_link_put(link); continue; }

        const char *name = rtnl_link_get_name(link);
        if (name && ifname_valid(name) && count < max)
            safe_strncpy(names[count++], IFNAMSIZ, name);
        rtnl_link_put(link);
    }

    nl_socket_free(sk);
    return count;
}

int can_init(void *arg)
{
    app_ctx_t *app = (app_ctx_t *)arg;
    gateway_args_t *args = &app->cfg->gw_args;
    can_ctx_t *ctx = app->can;
    memset(ctx, 0, sizeof(*ctx));
    ctx->epfd  = -1;
    ctx->rx_efd = -1;
    ctx->tx_efd = -1;
    ctx->ifaces = args->can_ifaces;
    ctx->count  = args->can_count;
    /* 收发队列清零 */
    rxq_reset(&ctx->rxq);
    for (int i = 0; i < CAN_MAX_IFACES; i++)
        txq_reset(&ctx->txq[i]);
    pthread_mutex_init(&ctx->rx_mutex, NULL);
    /* CAN 数据收发专用 epoll（socket + TX eventfd，与 TCP 分开管理） */
    ctx->epfd = epoll_create1(0);
    if (ctx->epfd < 0) { log_error("can: epoll_create1 failed"); return -1; }
    /* RX/TX 两个 eventfd：跨线程唤醒，让 epoll 检测“有数据压入/弹出” */
    ctx->rx_efd = eventfd(0, EFD_NONBLOCK);
    ctx->tx_efd = eventfd(0, EFD_NONBLOCK);
    if (ctx->rx_efd < 0 || ctx->tx_efd < 0) { log_error("can: eventfd failed"); return -1; }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.u32 = 0;
    if (epoll_ctl(ctx->epfd, EPOLL_CTL_ADD, ctx->tx_efd, &ev) < 0) {
        log_error("can: epoll_ctl(tx_efd) failed"); return -1;
    }

    for (int i = 0; i < args->can_count; i++) {
        can_iface_t *iface = &args->can_ifaces[i];
        if (can_socket_configure(iface->ifname, iface->bitrate, iface->dbitrate, iface->fd_mode, iface->restart_ms, iface->up) < 0) return -1;
        int fd = can_socket_open(iface->ifname, iface->fd_mode);
        if (fd < 0) return -1;
        iface->sock_fd = fd;
        for (int k = 0; k < iface->filter_count; k++)
            can_socket_set_filter(fd, iface->filters[k].id, iface->filters[k].mask);
    }

    /* 独立 RX 消费线程：epoll 监听 rx_efd，弹出 RX 队列回调 on_can_rx */
    if (pthread_create(&ctx->rx_tid, NULL, can_rx_consumer, app) != 0) {
        log_error("can: create rx consumer thread failed"); return -1;
    }

    log_info("%d CAN interface(s) initialized", args->can_count);
    for (int i = 0; i < args->can_count; i++)
        log_info("  CAN[%d]=%s filters=%d", i, args->can_ifaces[i].ifname, args->can_ifaces[i].filter_count);
    return 0;
}

void can_cleanup(void *arg)
{
    app_ctx_t *app = (app_ctx_t *)arg;
    can_ctx_t *ctx = app->can;
    if (!ctx) return;

    /* 停止并回收 RX 消费线程 */
    ctx->rx_stop = 1;
    eventfd_signal(ctx->rx_efd);
    if (ctx->rx_tid) pthread_join(ctx->rx_tid, NULL);

    for (int i = 0; i < ctx->count; i++)
        can_socket_close(ctx->ifaces[i].sock_fd);
    if (ctx->rx_efd >= 0) close(ctx->rx_efd);
    if (ctx->tx_efd >= 0) close(ctx->tx_efd);
    if (ctx->epfd >= 0) close(ctx->epfd);
    pthread_mutex_destroy(&ctx->rx_mutex);
}
