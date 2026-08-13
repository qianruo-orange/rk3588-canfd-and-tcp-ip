#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <ctype.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/epoll.h>
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
#include "core/log.h"
#include "watchdog/watchdog.h"

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
    log_info("CAN socket opened on %s", ifname);
    return s;
}

void can_socket_close(int fd) { if (fd >= 0) close(fd); }

ssize_t can_recv_frame(int fd, struct canfd_frame *frame, int timeout_ms)
{
    if (fd < 0 || !frame) {
        errno = EINVAL;
        return -1;
    }

    int wait_ms = timeout_ms < 0 ? 0 : timeout_ms;
    int efd = epoll_create1(EPOLL_CLOEXEC);
    if (efd < 0) return -1;

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    if (epoll_ctl(efd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        close(efd);
        return -1;
    }

    int nfds = epoll_wait(efd, &ev, 1, wait_ms);
    close(efd);
    if (nfds < 0) return -1;
    if (nfds == 0) return 0;

    ssize_t n = read(fd, frame, sizeof(*frame));
    if (n == (ssize_t)sizeof(*frame) || n == (ssize_t)CAN_MTU)
        return n;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return 0;
    return n;
}

ssize_t can_send_frame(int fd, const struct canfd_frame *frame, int timeout_ms)
{
    if (fd < 0 || !frame) {
        errno = EINVAL;
        return -1;
    }

    int wait_ms = timeout_ms < 0 ? 0 : timeout_ms;
    int efd = epoll_create1(EPOLL_CLOEXEC);
    if (efd < 0) return -1;

    struct epoll_event ev;
    ev.events = EPOLLOUT;
    ev.data.fd = fd;
    if (epoll_ctl(efd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        close(efd);
        return -1;
    }

    int nfds = epoll_wait(efd, &ev, 1, wait_ms);
    close(efd);
    if (nfds < 0) return -1;
    if (nfds == 0) return 0;

    return write(fd, frame, sizeof(*frame));
}

static void handle_can_input(app_ctx_t *app, int can_idx)
{
    can_ctx_t *ctx = app->can;
    pthread_mutex_lock(&app->can_mutex);
    int fd = ctx->ifaces[can_idx].sock_fd;
    while (1) {
        struct canfd_frame frame;
        ssize_t n = can_recv_frame(fd, &frame, 10);
        if (n > 0) {
            log_info("CAN recv: %s id=%X len=%d",
                     ctx->ifaces[can_idx].ifname,
                     frame.can_id & CAN_EFF_MASK, frame.len);
        } else if (n == 0) {
            break;
        } else {
            if (errno != 0)
                log_error("can read %s: %s", ctx->ifaces[can_idx].ifname, strerror(errno));
            can_socket_close(fd);
            int new_fd = can_socket_open(ctx->ifaces[can_idx].ifname,
                                          ctx->ifaces[can_idx].fd_mode);
            if (new_fd >= 0) {
                ctx->ifaces[can_idx].sock_fd = new_fd;
                for (int k = 0; k < ctx->ifaces[can_idx].filter_count; k++)
                    can_socket_set_filter(new_fd,
                        ctx->ifaces[can_idx].filters[k].id,
                        ctx->ifaces[can_idx].filters[k].mask);
                struct epoll_event nev;
                nev.events = EPOLLIN;
                nev.data.u32 = (uint32_t)(can_idx + 1);
                epoll_ctl(ctx->epfd, EPOLL_CTL_ADD, new_fd, &nev);
                log_info("CAN %s reconnected", ctx->ifaces[can_idx].ifname);
            }
            break;
        }
    }
    pthread_mutex_unlock(&app->can_mutex);
}

void *can_task(void *arg)
{
    app_ctx_t *app = (app_ctx_t *)arg;
    can_ctx_t *ctx = app->can;
    if (!ctx) return NULL;

    for (int i = 0; i < ctx->count; i++) {
        struct epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.u32 = (uint32_t)(i + 1);
        epoll_ctl(ctx->epfd, EPOLL_CTL_ADD, ctx->ifaces[i].sock_fd, &ev);
    }
    log_info("can_task started (%d iface(s))", ctx->count);
    while (app->running) {
        struct epoll_event events[64];
        int nfds = epoll_wait(ctx->epfd, events, 64, 500);
        if (nfds < 0) {
            if (errno == EINTR) { watchdog_feed_thread(pthread_self()); continue; }
            log_error("can epoll_wait"); break;
        }
        watchdog_feed_thread(pthread_self());
        for (int i = 0; i < nfds; i++) {
            uint32_t tag = events[i].data.u32;
            if (tag < 1 || tag > (uint32_t)ctx->count) continue;
            handle_can_input(app, (int)(tag - 1));
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

int can_init(void *arg)
{
    app_ctx_t *app = (app_ctx_t *)arg;
    gateway_args_t *args = &app->cfg->gw_args;
    can_ctx_t *ctx = app->can;
    ctx->ifaces = args->can_ifaces;
    ctx->count  = args->can_count;
    /* CAN 数据接收专用 epoll（与 TCP 分开管理） */
    ctx->epfd = epoll_create1(0);
    if (ctx->epfd < 0) { log_error("can: epoll_create1 failed"); return -1; }
    for (int i = 0; i < args->can_count; i++) {
        can_iface_t *iface = &args->can_ifaces[i];
        if (can_socket_configure(iface->ifname, iface->bitrate, iface->dbitrate, iface->fd_mode, iface->restart_ms, iface->up) < 0) return -1;
        int fd = can_socket_open(iface->ifname, iface->fd_mode);
        if (fd < 0) return -1;
        iface->sock_fd = fd;
        for (int k = 0; k < iface->filter_count; k++)
            can_socket_set_filter(fd, iface->filters[k].id, iface->filters[k].mask);
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
    for (int i = 0; i < ctx->count; i++)
        can_socket_close(ctx->ifaces[i].sock_fd);
    if (ctx->epfd >= 0) close(ctx->epfd);
}
