/**
 * data_flow.c — 数据流虚函数默认实现（各域数据流独立，不做任何桥接）
 *
 * CAN 域与 TCP 域的数据流完全独立：
 *   - on_can_rx 默认空实现：can_task 读到的帧仅记录日志，不转发；
 *   - on_tcp_rx 默认空实现：tcp_task 收到的数据仅记录日志，不转发；
 *   - tx_can / tx_tcp 提供各域发送原语，供业务主动收发使用。
 *
 * 虚函数表 data_flow_ops_t 可整体替换或逐项覆盖，
 * 业务通过 data_flow_register() 注册自定义实现即可接管数据流。
 *
 * 帧文本格式（空格分隔，每行一帧）：
 *   <ifname> <can_id(hex)> <dlc> <b0(hex)> <b1(hex)> ...
 */

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "core/data_flow.h"
#include "core/log.h"
#include "can/can_socket.h"
#include "net/tcp_server.h"

/* ---- 默认发送原语 ---- */

static ssize_t default_tx_can(app_ctx_t *app, const char *ifname,
                              const struct canfd_frame *frame, int timeout_ms)
{
    if (!app || !app->can || !ifname || !frame) { errno = EINVAL; return -1; }
    can_ctx_t *can = app->can;
    for (int i = 0; i < can->count; i++) {
        if (strcmp(can->ifaces[i].ifname, ifname) != 0) continue;
        /* CAN 域发送原语：供业务经虚函数 tx_can 调用；
           发送失败由返回值体现，调用方自行重试 */
        int fd = can->ifaces[i].sock_fd;
        if (fd < 0) { errno = ENODEV; return -1; }
        return can_send_frame(fd, frame, timeout_ms);
    }
    log_error("tx_can: unknown interface '%s'", ifname);
    errno = ENODEV;
    return -1;
}

static ssize_t default_tx_tcp(app_ctx_t *app, int client_idx,
                              const void *buf, size_t len, int timeout_ms)
{
    if (!app || !app->tcp || !buf || len == 0) { errno = EINVAL; return -1; }
    tcp_ctx_t *tcp = app->tcp;
    ssize_t total = 0;
    pthread_mutex_lock(&tcp->client_mutex);
    if (client_idx < 0) {
        /* 广播所有已连接客户端 */
        for (int i = 0; i < tcp->client_count; i++) {
            if (tcp->clients[i].fd < 0) continue;
            ssize_t r = tcp_send_data(tcp->clients[i].fd, buf, len, timeout_ms);
            if (r > 0) total += r;
        }
    } else if (client_idx < tcp->client_count && tcp->clients[client_idx].fd >= 0) {
        total = tcp_send_data(tcp->clients[client_idx].fd, buf, len, timeout_ms);
    }
    pthread_mutex_unlock(&tcp->client_mutex);
    return total;
}

/* ---- 帧文本编解码 ---- */

int data_flow_encode_frame(const char *ifname, const struct canfd_frame *frame,
                           char *out, size_t out_size)
{
    if (!ifname || !frame || !out || out_size == 0) return -1;
    int off = snprintf(out, out_size, "%s %X %u", ifname,
                       frame->can_id & CAN_EFF_MASK, frame->len);
    if (off < 0 || (size_t)off >= out_size) return -1;
    for (unsigned int i = 0; i < frame->len; i++) {
        int n = snprintf(out + off, out_size - (size_t)off, " %02X", frame->data[i]);
        if (n < 0 || (size_t)n >= out_size - (size_t)off) return -1;
        off += n;
    }
    return off;
}

int data_flow_decode_frame(const char *text, char *ifname, size_t ifname_size,
                           struct canfd_frame *frame)
{
    if (!text || !ifname || !frame || ifname_size == 0) return -1;
    const char *p = text;

    /* ifname */
    while (*p && isspace((unsigned char)*p)) p++;
    const char *s = p;
    while (*p && !isspace((unsigned char)*p)) p++;
    size_t nlen = (size_t)(p - s);
    if (nlen == 0 || nlen >= ifname_size) return -1;
    memcpy(ifname, s, nlen);
    ifname[nlen] = '\0';

    /* can_id（hex，不含 EFF 标志位） */
    while (*p && isspace((unsigned char)*p)) p++;
    char *end = NULL;
    unsigned long id = strtoul(p, &end, 16);
    if (end == p || id > (unsigned long)CAN_EFF_MASK) return -1;
    p = end;

    /* dlc */
    while (*p && isspace((unsigned char)*p)) p++;
    unsigned long dlen = strtoul(p, &end, 10);
    if (end == p || dlen > CANFD_MAX_DLEN) return -1;
    p = end;

    memset(frame, 0, sizeof(*frame));
    frame->can_id = (canid_t)id | (id > (unsigned long)CAN_SFF_MASK ? CAN_EFF_FLAG : 0);
    frame->len = (uint8_t)dlen;

    /* data */
    for (unsigned long i = 0; i < dlen; i++) {
        while (*p && isspace((unsigned char)*p)) p++;
        unsigned long b = strtoul(p, &end, 16);
        if (end == p || b > 0xFFUL) return -1;
        frame->data[i] = (uint8_t)b;
        p = end;
    }
    return 0;
}

/* ---- 默认回调：域内钩子 ---- */

static int default_on_can_rx(app_ctx_t *app, const char *ifname,
                             const struct canfd_frame *frame)
{
    (void)app; (void)ifname; (void)frame;
    /* CAN 数据流独立：默认不转发到 TCP（帧内容已由 can_task 记录日志）。
       业务可覆盖此钩子自行处理，如落盘、告警或转发到自定义通道。 */
    return 0;
}

static int default_on_tcp_rx(app_ctx_t *app, int client_idx,
                             const void *buf, size_t len)
{
    (void)app; (void)buf;
    /* TCP 数据流独立：默认不发送到 CAN，仅记录来源。
       业务可覆盖此钩子自行处理，如协议解析、命令响应或转发到 CAN。 */
    log_info("tcp rx: %zu byte(s) from client=%d ignored (flow independent)",
             len, client_idx);
    return 0;
}

/* ---- 虚函数注册 ---- */

static const data_flow_ops_t g_default_ops = {
    .on_can_rx = default_on_can_rx,
    .on_tcp_rx = default_on_tcp_rx,
    .tx_can    = default_tx_can,
    .tx_tcp    = default_tx_tcp,
};

void data_flow_register(app_ctx_t *app, const data_flow_ops_t *ops)
{
    if (!app) return;
    app->flow = ops ? ops : data_flow_default_ops();
}

const data_flow_ops_t *data_flow_default_ops(void)
{
    return &g_default_ops;
}
