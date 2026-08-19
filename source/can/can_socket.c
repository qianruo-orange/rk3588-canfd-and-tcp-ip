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
#include "can/dbc_parser.h"
#include "http/http_api_dbc.h"
#include "http/http_api_can.h"
#include "core/common.h"
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
        LOG_INFO("CAN %s: data bitrate set to %u bps", ifname, dbitrate);
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
    if (!ifname_valid(ifname)) { LOG_ERROR("invalid CAN interface name: '%s'", ifname?ifname:"(null)"); return -1; }
    if (bitrate <= 0) bitrate = 500000;

    struct nl_sock *sk = nl_socket_alloc();
    if (!sk) { LOG_ERROR("nl_socket_alloc"); return -1; }
    if (nl_connect(sk, NETLINK_ROUTE) < 0) { LOG_ERROR("nl_connect"); nl_socket_free(sk); return -1; }

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
                LOG_ERROR("CAN %s: config via ip command failed", ifname);
                nl_socket_free(sk); return -1;
            }
            LOG_INFO("CAN %s: type can returned EEXIST, interface is up anyway", ifname);
        }
        LOG_INFO("CAN %s configured (existing), %s", ifname, bring_up ? "up" : "down");
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
        LOG_ERROR("CAN %s: configure failed: %s", ifname, nl_geterror(ret));
        rtnl_link_put(link); nl_socket_free(sk); return -1;
    }

    if (fd_mode && dbitrate > 0)
        apply_dbitrate(sk, ifname, (uint32_t)dbitrate);

    if (bring_up) {
        rtnl_link_set_flags(link, IFF_UP);
        if (rtnl_link_change(sk, link, link, 0) < 0)
            LOG_ERROR("CAN %s: bring up failed", ifname);
    }

    LOG_INFO("CAN %s configured (bitrate=%d%s%s) %s", ifname, bitrate,
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
    if (!ifname_valid(ifname)) { LOG_ERROR("invalid CAN interface name: '%s'", ifname?ifname:"(null)"); return -1; }
    int s = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (s < 0) { LOG_ERROR("socket(PF_CAN)"); return -1; }
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
    if (ioctl(s, SIOCGIFINDEX, &ifr) < 0) { LOG_ERROR("ioctl SIOCGIFINDEX for %s", ifname); close(s); return -1; }
    struct sockaddr_can addr;
    memset(&addr, 0, sizeof(addr));
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) { LOG_ERROR("bind CAN %s", ifname); close(s); return -1; }
    if (fd_mode) {
        int enable = 1;
        if (setsockopt(s, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &enable, sizeof(enable)) < 0)
            LOG_INFO("CAN_RAW_FD_FRAMES not available on %s, classic CAN only", ifname);
        else LOG_INFO("CAN FD socket enabled on %s", ifname);
    }

    /* 非阻塞：配合 epoll 就绪通知，read/write 在无数据/不可写时返回 EAGAIN 而不是阻塞 */
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0 || fcntl(s, F_SETFL, flags | O_NONBLOCK) < 0) {
        LOG_ERROR("CAN %s: fcntl O_NONBLOCK: %s", ifname, strerror(errno));
        close(s);
        return -1;
    }

    LOG_INFO("CAN socket opened on %s", ifname);
    return s;
}

/**
 * can_socket_close - 关闭已打开的 CAN socket fd。
 * @fd: socket 文件描述符。
 */
void can_socket_close(int fd) { if (fd >= 0) close(fd); }

/* ---- TX 队列 + eventfd 跨线程唤醒 ---- */

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
 * can_epoll_update_tx - 按该接口发送队列是否为空，动态切换 epoll 是否监听 EPOLLOUT。
 * 队列非空才监听 EPOLLOUT，避免 socket 一直可写导致 epoll 忙轮询。
 * @ctx: CAN 上下文。
 * @idx: 接口索引。
 */
static void can_epoll_update_tx(can_ctx_t *ctx, int idx)
{
    int fd = ctx->ifaces[idx].sock_fd;
    if (fd < 0) return;
    int want_out = can_queue_count(&ctx->ifaces[idx].txq) > 0;
    if (want_out) {
        struct epoll_event ev;
        ev.events = EPOLLOUT;
        ev.data.u32 = (uint32_t)(idx + 1);
        if (epoll_ctl(ctx->send_epfd, EPOLL_CTL_MOD, fd, &ev) < 0 && errno == ENOENT)
            epoll_ctl(ctx->send_epfd, EPOLL_CTL_ADD, fd, &ev);
    } else {
        epoll_ctl(ctx->send_epfd, EPOLL_CTL_DEL, fd, NULL);
    }
}

/* 把接口 socket 以 EPOLLIN 注册到接收 epoll（can_recv_task 使用） */
static void can_epoll_add_recv(can_ctx_t *ctx, int idx)
{
    int fd = ctx->ifaces[idx].sock_fd;
    if (fd < 0) return;
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.u32 = (uint32_t)(idx + 1);
    epoll_ctl(ctx->recv_epfd, EPOLL_CTL_ADD, fd, &ev);
}

/**
 * can_tx_frame - 异步发送一帧 CAN 数据。
 * 压入目标接口 TX 队列，由 can_send_task 排空写 socket 并做发送方向 DBC 解析。
 * @app: 全局应用上下文。
 * @ifname: 目标接口名。
 * @frame: 待发送 CAN(FD) 帧。
 * @return: 入队成功返回 0；失败返回 -1。
 */
int can_tx_frame(app_ctx_t *app, const char *ifname, const struct canfd_frame *frame)
{
    if (!app || !app->can || !ifname || !frame) { errno = EINVAL; return -1; }
    can_ctx_t *ctx = app->can;

    int idx = -1;
    for (int i = 0; i < ctx->count; i++) {
        if (strcmp(ctx->ifaces[i].ifname, ifname) == 0) { idx = i; break; }
    }
    if (idx < 0) { LOG_ERROR("can_tx: unknown interface '%s'", ifname); errno = ENODEV; return -1; }

    /* 发送统一走 TX 队列：由 can_send_task 弹出写 socket 并做发送方向 DBC 解析 */
    can_queue_t *txq = &ctx->ifaces[idx].txq;
    if (can_queue_push(txq, frame) != CAN_QUEUE_ERR_OK) {
        errno = ENOBUFS;
        return -1;
    }
    eventfd_signal(ctx->tx_efd);
    return 0;
}

/**
 * handle_can_output - 排空各接口发送队列，把待发帧写入对应 socket。
 * @app: 全局应用上下文。
 */
static void handle_can_output(app_ctx_t *app)
{
    can_ctx_t *ctx = app->can;
    pthread_mutex_lock(&app->can_mutex);

    for (int idx = 0; idx < ctx->count; idx++) {
        int fd = ctx->ifaces[idx].sock_fd;
        if (fd < 0) continue;

        can_queue_t *q = &ctx->ifaces[idx].txq;
        /* 一轮最多处理当前队列长度，避免 EAGAIN 帧重排入队尾导致死循环 */
        int limit = can_queue_count(q);
        while (limit-- > 0 && can_queue_count(q) > 0) {
            struct canfd_frame f;
            if (can_queue_pop(q, &f) != CAN_QUEUE_ERR_OK) break;

            ssize_t n = write(fd, &f, sizeof(f));
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                can_queue_push(q, &f); /* 该接口暂不可写：放回队尾，等下一轮 */
                break;
            }
            if (n != (ssize_t)sizeof(f))
                continue; /* 写异常：该帧已弹出，直接丢弃 */

            /* 写成功后做发送方向 DBC 解析并记录 */
            char name[DBC_MAX_NAME_LEN];
            char text[DBC_DECODE_TEXT_MAX];
            canid_t id = 0;
            if (dbc_decode_frame(app, idx, ctx->ifaces[idx].ifname, &f,
                                 &id, name, sizeof(name), text, sizeof(text)) == 0)
                http_dbc_record(DBC_DIR_TX, ctx->ifaces[idx].ifname, id, name, text);
        }
    }

    /* 队列非空才监听 EPOLLOUT，排空则回到仅监听 TX eventfd */
    for (int i = 0; i < ctx->count; i++)
        can_epoll_update_tx(ctx, i);
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
    can_queue_t *rxq = &ctx->ifaces[can_idx].rxq;
    const char *ifname = ctx->ifaces[can_idx].ifname;

    /* 每次 epoll 事件最多排空 64 帧就返回：避免 CAN 总线持续有数据时无限排水，
       导致 can_recv_task 无法回到下一次 epoll_wait 前的 watchdog_feed_self("can_recv")，
       从而被看门狗误判为卡死。 */
    for (int i = 0; i < 64; i++) {
        struct canfd_frame frame;
        ssize_t n = read(fd, &frame, sizeof(frame));
        if (n > 0) {
            /* 记录原始帧到 HTTP 报文缓存（与 DBC 解码队列相互独立，队列满也不影响展示） */
            http_can_record_rx(ifname, &frame);

            /* 入队前完成接收方向 DBC 解析并记录 */
            char name[DBC_MAX_NAME_LEN];
            char text[DBC_DECODE_TEXT_MAX];
            canid_t id = 0;
            if (dbc_decode_frame(app, can_idx, ifname, &frame,
                                 &id, name, sizeof(name), text, sizeof(text)) == 0)
                http_dbc_record(DBC_DIR_RX, ifname, id, name, text);

            /* 解析后入 rxq；接收线程只入队、不出队（由其他线程 pop） */
            if (can_queue_push(rxq, &frame) != CAN_QUEUE_ERR_OK)
                LOG_ERROR("can rx queue full on %s, drop frame", ifname);
        } else if (n == 0) {
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            LOG_ERROR("can read %s: %s", ifname, strerror(errno));
            can_socket_close(fd);
            int new_fd = can_socket_open(ifname, ctx->ifaces[can_idx].fd_mode);
            if (new_fd >= 0) {
                ctx->ifaces[can_idx].sock_fd = new_fd;
                for (int k = 0; k < ctx->ifaces[can_idx].filter_count; k++)
                    can_socket_set_filter(new_fd,
                        ctx->ifaces[can_idx].filters[k].id,
                        ctx->ifaces[can_idx].filters[k].mask);
                /* 重新注册到接收 epoll；并同步发送 epoll 的 EPOLLOUT 状态 */
                can_epoll_add_recv(ctx, can_idx);
                can_epoll_update_tx(ctx, can_idx);
                LOG_INFO("CAN %s reconnected", ifname);
            }
            break;
        }
    }

    pthread_mutex_unlock(&app->can_mutex);
}

void *can_recv_task(void *arg)
{
    app_ctx_t *app = (app_ctx_t *)arg;
    can_ctx_t *ctx = app->can;
    if (!ctx) return NULL;

    LOG_INFO("can_recv_task started (%d iface(s))", ctx->count);
    while (app->running) {
        struct epoll_event events[64];
        int nfds = epoll_wait(ctx->recv_epfd, events, 64, 500);
        if (nfds < 0) {
            if (errno == EINTR) { watchdog_feed_self("can_recv"); continue; }
            LOG_ERROR("can recv epoll_wait"); break;
        }
        watchdog_feed_self("can_recv");
        for (int i = 0; i < nfds; i++) {
            uint32_t tag = events[i].data.u32;
            if (tag < 1 || tag > (uint32_t)ctx->count) continue;
            int idx = (int)(tag - 1);
            uint32_t ev = events[i].events;
            if (ev & (EPOLLIN | EPOLLERR | EPOLLHUP))
                handle_can_input(app, idx);
        }
    }
    LOG_INFO("can_recv_task stopped");
    return NULL;
}

void *can_send_task(void *arg)
{
    app_ctx_t *app = (app_ctx_t *)arg;
    can_ctx_t *ctx = app->can;
    if (!ctx) return NULL;

    for (int i = 0; i < ctx->count; i++)
        can_epoll_update_tx(ctx, i);
    LOG_INFO("can_send_task started (%d iface(s))", ctx->count);
    while (app->running) {
        struct epoll_event events[64];
        int nfds = epoll_wait(ctx->send_epfd, events, 64, 500);
        if (nfds < 0) {
            if (errno == EINTR) { watchdog_feed_self("can_send"); continue; }
            LOG_ERROR("can send epoll_wait"); break;
        }
        watchdog_feed_self("can_send");
        for (int i = 0; i < nfds; i++) {
            uint32_t tag = events[i].data.u32;
            if (tag == 0) {
                /* TX eventfd：有帧压入 TX 队列，排空各接口发送队列 */
                eventfd_consume(ctx->tx_efd);
                handle_can_output(app);
                continue;
            }
            if (tag < 1 || tag > (uint32_t)ctx->count) continue;
            uint32_t ev = events[i].events;
            if (ev & EPOLLOUT)
                handle_can_output(app);
        }
    }
    LOG_INFO("can_send_task stopped");
    return NULL;
}

int can_socket_set_filter(int fd, canid_t id, canid_t mask)
{
    if ((id & ~CAN_SFF_MASK) || (mask & ~CAN_SFF_MASK)) { id |= CAN_EFF_FLAG; mask |= CAN_EFF_FLAG; }
    struct can_filter filter;
    filter.can_id = id;
    filter.can_mask = mask;
    if (setsockopt(fd, SOL_CAN_RAW, CAN_RAW_FILTER, &filter, sizeof(filter)) < 0) {
        LOG_ERROR("setsockopt CAN_RAW_FILTER id=%08X mask=%08X", id, mask); return -1;
    }
    LOG_INFO("CAN filter set: id=%08X mask=%08X", id, mask);
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
    app_args_t *args = &app->cfg->args;
    can_ctx_t *ctx = app->can;
    memset(ctx, 0, sizeof(*ctx));
    ctx->recv_epfd = -1;
    ctx->send_epfd = -1;
    ctx->tx_efd    = -1;
    ctx->ifaces = args->can_ifaces;
    ctx->count  = args->can_count;
    /* 每个接口各初始化一个发送队列和一个接收队列 */
    for (int i = 0; i < ctx->count; i++) {
        can_queue_init(&ctx->ifaces[i].txq);
        can_queue_init(&ctx->ifaces[i].rxq);
    }
    /* 接收 epoll（can_recv_task，监听各接口 EPOLLIN） */
    ctx->recv_epfd = epoll_create1(0);
    if (ctx->recv_epfd < 0) { LOG_ERROR("can: recv epoll_create1 failed"); return -1; }
    /* 发送 epoll（can_send_task，监听 TX eventfd + 各接口 EPOLLOUT） */
    ctx->send_epfd = epoll_create1(0);
    if (ctx->send_epfd < 0) { LOG_ERROR("can: send epoll_create1 failed"); return -1; }
    /* TX eventfd：跨线程唤醒 can_send_task，让其检测“有帧压入 TX 队列” */
    ctx->tx_efd = eventfd(0, EFD_NONBLOCK);
    if (ctx->tx_efd < 0) { LOG_ERROR("can: eventfd failed"); return -1; }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.u32 = 0;
    if (epoll_ctl(ctx->send_epfd, EPOLL_CTL_ADD, ctx->tx_efd, &ev) < 0) {
        LOG_ERROR("can: epoll_ctl(tx_efd) failed"); return -1;
    }

    for (int i = 0; i < args->can_count; i++) {
        can_iface_t *iface = &args->can_ifaces[i];
        if (can_socket_configure(iface->ifname, iface->bitrate, iface->dbitrate, iface->fd_mode, iface->restart_ms, iface->up) < 0) return -1;
        int fd = can_socket_open(iface->ifname, iface->fd_mode);
        if (fd < 0) return -1;
        iface->sock_fd = fd;
        for (int k = 0; k < iface->filter_count; k++)
            can_socket_set_filter(fd, iface->filters[k].id, iface->filters[k].mask);
        can_epoll_add_recv(ctx, i);
    }

    LOG_INFO("%d CAN interface(s) initialized", args->can_count);
    for (int i = 0; i < args->can_count; i++)
        LOG_INFO("  CAN[%d]=%s filters=%d", i, args->can_ifaces[i].ifname, args->can_ifaces[i].filter_count);
    return 0;
}

void can_cleanup(void *arg)
{
    app_ctx_t *app = (app_ctx_t *)arg;
    can_ctx_t *ctx = app->can;
    if (!ctx) return;

    for (int i = 0; i < ctx->count; i++) {
        can_socket_close(ctx->ifaces[i].sock_fd);
        can_queue_destroy(&ctx->ifaces[i].txq);
        can_queue_destroy(&ctx->ifaces[i].rxq);
    }
    if (ctx->tx_efd >= 0) close(ctx->tx_efd);
    if (ctx->recv_epfd >= 0) close(ctx->recv_epfd);
    if (ctx->send_epfd >= 0) close(ctx->send_epfd);
}
