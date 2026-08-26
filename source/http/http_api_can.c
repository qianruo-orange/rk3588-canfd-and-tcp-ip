/**
 * http_api_can.c — CAN 接口状态查询与开关控制 API。
 */

#include <ctype.h>
#include <stdint.h>
#include <linux/if.h>
#include <linux/can.h>
#include <netlink/netlink.h>
#include <netlink/socket.h>
#include <netlink/route/link.h>
#include <netlink/route/link/can.h>
#include <linux/can/netlink.h>

#include "http/http_internal.h"
#include "http/http_api_can.h"
#include "can/can_socket.h"

void http_can_ifaces(app_ctx_t *app, int fd)
{
    (void)app;
    char names[CAN_MAX_IFACES][IFNAMSIZ];
    int n = can_enumerate_system(names, CAN_MAX_IFACES);

    char json[2048];
    int off = 0;
    JSON_ADD(json, off, "[");
    for (int i = 0; i < n; i++) {
        JSON_ADD(json, off, "%s{\"name\":\"%s\",\"fd\":%d}",
                 i ? "," : "", names[i], can_fd_supported(names[i]));
    }
    JSON_ADD(json, off, "]");
    http_ok_json(fd, json, (size_t)off);
}

void http_can_status(app_ctx_t *app, int fd)
{
    (void)app;
    char json[2048] = "[";
    int off = 1;

    struct nl_sock *sk = nl_socket_alloc();
    if (!sk) goto done;
    if (nl_connect(sk, NETLINK_ROUTE) < 0) { nl_socket_free(sk); goto done; }

    for (int idx = 1; ; idx++) {
        struct rtnl_link *link;
        if (rtnl_link_get_kernel(sk, idx, NULL, &link) != 0) break;
        if (!link) break;

        const char *kind = rtnl_link_get_type(link);
        if (!kind || strcmp(kind, "can") != 0) { rtnl_link_put(link); continue; }

        const char *name = rtnl_link_get_name(link);
        int flags = rtnl_link_get_flags(link);
        int up = (flags & IFF_UP) ? 1 : 0;
        uint32_t bitrate = 0;
        rtnl_link_can_get_bitrate(link, &bitrate);
        uint32_t ctrlmode = 0;
        rtnl_link_can_get_ctrlmode(link, &ctrlmode);
        int fd_mode = (ctrlmode & CAN_CTRLMODE_FD) ? 1 : 0;

        uint32_t dbitrate = 0;
        char sysfs[128];
        snprintf(sysfs, sizeof(sysfs), "/sys/class/net/%s/can_data_bitrate", name);
        FILE *sfp = fopen(sysfs, "r");
        if (sfp) { if (fscanf(sfp, "%u", &dbitrate) != 1) dbitrate = 0; fclose(sfp); }

        JSON_ADD(json, off, "%s{\"name\":\"%s\",\"up\":%d,\"bitrate\":%u,\"dbitrate\":%u,\"fd\":%d}",
             off > 1 ? "," : "", name, up, bitrate, dbitrate, fd_mode);
        rtnl_link_put(link);
    }
    nl_socket_free(sk);

done:
    JSON_ADD(json, off, "]");
    http_ok_json(fd, json, (size_t)off);
}

/* 校验 CAN 接口名：字母数字 / 下划线 / 连字符（公共实现见 core/common.h 的 ifname_valid） */

void http_can_toggle(app_ctx_t *app, int fd, const char *body)
{
    char ifname[32] = {0}, action[8] = {0};
    if (!body) { http_err(fd, 400, "Bad Request", NULL); return; }

    http_form_get_param(body, "ifname=", ifname, sizeof(ifname));
    http_form_get_param(body, "action=", action, sizeof(action));

    if (!ifname_valid(ifname)) {
        http_err(fd, 400, "Bad Request", "bad ifname");
        return;
    }
    if (strcmp(action, "up") != 0 && strcmp(action, "down") != 0) {
        http_err(fd, 400, "Bad Request", "bad action");
        return;
    }

    struct nl_sock *sk = nl_socket_alloc();
    if (!sk) { http_err(fd, 500, "Error", NULL); return; }
    if (nl_connect(sk, NETLINK_ROUTE) < 0) {
        nl_socket_free(sk);
        http_err(fd, 500, "Error", NULL);
        return;
    }

    struct rtnl_link *ln = rtnl_link_alloc();
    if (!ln) { nl_socket_free(sk); http_err(fd, 500, "Error", NULL); return; }
    rtnl_link_set_name(ln, ifname);

    /* 串行化接口状态操作与 CAN 配置热更新 / CAN 重连 */
    pthread_mutex_lock(&app->can_mutex);
    if (strcmp(action, "up") == 0)
        rtnl_link_set_flags(ln, IFF_UP);
    else
        rtnl_link_set_flags(ln, 0);

    int rc = rtnl_link_change(sk, ln, ln, 0);
    pthread_mutex_unlock(&app->can_mutex);
    rtnl_link_put(ln);
    nl_socket_free(sk);

    if (rc < 0) {
        http_err(fd, 500, "Error", "link change failed");
        return;
    }

    char msg[64];
    snprintf(msg, sizeof(msg), "{\"result\":\"%s %s\"}", ifname, action);
    http_ok_json(fd, msg, strlen(msg));
}

/* DBC 文件上传：POST /api/can/dbc?ifname=can0，body 为 DBC 文本内容 */
void http_can_dbc_upload(app_ctx_t *app, int fd, const char *method, const char *uri, const char *body)
{
    (void)method;
    if (!app || !app->cfg || !app->can || !uri || !body) {
        http_err(fd, 400, "Bad Request", NULL);
        return;
    }

    /* 从 query string 解析目标通道名 */
    char ifname[32] = {0};
    const char *q = strchr(uri, '?');
    if (!q || !http_form_get_param(q, "ifname=", ifname, sizeof(ifname)) || !ifname_valid(ifname)) {
        http_err(fd, 400, "Bad Request", "bad ifname");
        return;
    }

    can_ctx_t *can = app->can;
    int idx = -1;
    for (int i = 0; i < can->count; i++)
        if (strcmp(can->ifaces[i].ifname, ifname) == 0) { idx = i; break; }
    if (idx < 0) {
        http_err(fd, 404, "Not Found", "iface not found");
        return;
    }

    /* 定位 body 与长度 */
    long cl = 0;
    const char *cl_hdr = strstr(body, "Content-Length:");
    if (!cl_hdr) cl_hdr = strstr(body, "content-length:");
    if (cl_hdr) {
        char *end = NULL;
        cl = strtol(cl_hdr + 15, &end, 10);
        if (end == cl_hdr + 15 || cl < 0) cl = 0;
    }
    const char *content = http_body_start(body);
    /* 实际接收到的 body 长度（HTTP 层已把读取到的数据 NUL 结尾）。
       不盲信 Content-Length：即使客户端头声称值大于实际数据，也以实际长度为上限，
       避免 fwrite 读到未接收的缓冲区 */
    size_t real_len = strlen(content);
    long content_len = (cl > 0 && (unsigned long)cl <= real_len) ? cl : (long)real_len;
    if (content_len <= 0 || content_len > 256 * 1024) {
        http_err(fd, 413, "Payload Too Large", "empty or too large");
        return;
    }

    /* 先写临时文件，解析验证成功后再原子替换正式文件，避免残留损坏的 DBC */
    char path[320], tmp_path[336];
    snprintf(path, sizeof(path), "config/dbc_%s.dbc", ifname);
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    FILE *fp = fopen(tmp_path, "w");
    if (!fp) {
        http_err(fd, 500, "Error", "cannot write file");
        return;
    }
    size_t w = fwrite(content, 1, (size_t)content_len, fp);
    fclose(fp);
    if (w != (size_t)content_len) {
        remove(tmp_path);
        http_err(fd, 500, "Error", "write failed");
        return;
    }

    /* 重新加载该通道 DBC（与解码互斥） */
    pthread_mutex_lock(&app->dbc_mutex);
    int rc = dbc_load(&app->dbcs[idx], tmp_path);
    int msg_count = app->dbcs[idx].msg_count;
    int sig_count = app->dbcs[idx].sig_count;
    pthread_mutex_unlock(&app->dbc_mutex);

    if (rc < 0) {
        remove(tmp_path);   /* 解析失败：不残留临时文件 */
        http_err(fd, 400, "Bad Request", "invalid dbc");
        return;
    }

    /* 验证通过：替换为正式文件 */
    if (rename(tmp_path, path) != 0) {
        remove(tmp_path);
        http_err(fd, 500, "Error", "cannot write file");
        return;
    }

    /* 更新运行时配置并落盘 */
    safe_strncpy(can->ifaces[idx].dbc_path, sizeof(can->ifaces[idx].dbc_path), path);
    config_save(app);

    char msg[160];
    snprintf(msg, sizeof(msg),
             "{\"result\":\"ok\",\"ifname\":\"%s\",\"messages\":%d,\"signals\":%d}",
             ifname, msg_count, sig_count);
    http_ok_json(fd, msg, strlen(msg));
}

/* ---- CAN 原始报文收发接口 ---- */

#define CAN_RX_RING_MAX 32

typedef struct {
    char               ifname[16];
    struct canfd_frame frame;
} can_rx_entry_t;

static can_rx_entry_t  g_can_rx_ring[CAN_RX_RING_MAX];
static _Atomic int     g_can_rx_count = 0;
static pthread_mutex_t g_can_rx_mutex = PTHREAD_MUTEX_INITIALIZER;

void http_can_record_rx(const char *ifname, const struct canfd_frame *frame)
{
    if (!ifname || !frame) return;
    pthread_mutex_lock(&g_can_rx_mutex);
    int pos = (int)(__atomic_fetch_add(&g_can_rx_count, 1, __ATOMIC_RELAXED) % CAN_RX_RING_MAX);
    can_rx_entry_t *e = &g_can_rx_ring[pos];
    safe_strncpy(e->ifname, sizeof(e->ifname), ifname);
    e->frame = *frame;
    pthread_mutex_unlock(&g_can_rx_mutex);
}

/* 单帧序列化为 JSON 对象；返回写入字节数（溢出返回 -1，复用 http_json_append） */
static int can_rx_frame_json(const can_rx_entry_t *e, char *out, size_t out_size)
{
    const struct canfd_frame *f = &e->frame;
    int off = http_json_append(out, out_size, 0,
                    "{\"ifname\":\"%s\",\"id\":\"0x%X\",\"len\":%u,\"data\":\"",
                    e->ifname, f->can_id & CAN_EFF_MASK, (unsigned)f->len);
    if (off < 0) return -1;

    for (unsigned i = 0; i < f->len; i++) {
        off = http_json_append(out, out_size, off, "%02X", f->data[i]);
        if (off < 0) return -1;
    }

    off = http_json_append(out, out_size, off, "\",\"flags\":\"%s%s%s\"}",
                    (f->can_id & CAN_EFF_FLAG) ? "EFF" : "SFF",
                    (f->len > 8) ? " FD" : "",
                    (f->can_id & CAN_RTR_FLAG) ? " RTR" : "");
    return off;
}

void http_can_rx(app_ctx_t *app, int fd)
{
    (void)app;
    char json[16384];
    int total = __atomic_load_n(&g_can_rx_count, __ATOMIC_RELAXED);
    int count = total > CAN_RX_RING_MAX ? CAN_RX_RING_MAX : total;
    int off = snprintf(json, sizeof(json), "[");

    pthread_mutex_lock(&g_can_rx_mutex);
    for (int i = 0; i < count; i++) {
        int pos = (total - 1 - i) % CAN_RX_RING_MAX;   /* 最新在前 */
        if (i > 0 && (size_t)off < sizeof(json) - 1) json[off++] = ',';
        int n = can_rx_frame_json(&g_can_rx_ring[pos], json + off, sizeof(json) - (size_t)off);
        if (n < 0 || (size_t)n >= (int)(sizeof(json) - (size_t)off)) { off = (int)sizeof(json) - 1; break; }
        off += n;
    }
    pthread_mutex_unlock(&g_can_rx_mutex);

    if ((size_t)off < sizeof(json) - 1) json[off++] = ']';
    http_ok_json(fd, json, (size_t)off);
}

/* 发送 CAN 报文：POST /api/can/send，body 形如 ifname=can0&id=0x123&data=01 02 03 */
void http_can_send(app_ctx_t *app, int fd, const char *method, const char *uri, const char *body)
{
    (void)method; (void)uri;
    if (!app || !body) { http_err(fd, 400, "Bad Request", NULL); return; }

    char ifname[32] = {0}, idstr[32] = {0}, datastr[512] = {0};
    http_form_get_param(body, "ifname=", ifname, sizeof(ifname));
    http_form_get_param(body, "id=", idstr, sizeof(idstr));
    http_form_get_param(body, "data=", datastr, sizeof(datastr));

    if (!ifname_valid(ifname)) {
        http_err(fd, 400, "Bad Request", "bad ifname");
        return;
    }
    if (!idstr[0]) {
        http_err(fd, 400, "Bad Request", "bad id");
        return;
    }

    char *end = NULL;
    unsigned long id = strtoul(idstr, &end, 0);   /* base=0：支持 0x 前缀十六进制与十进制 */
    if (!end || *end != '\0' || id > 0x1FFFFFFFUL) {
        http_err(fd, 400, "Bad Request", "bad id");
        return;
    }

    /* 解析 data 十六进制串：空格/逗号/连字符作为分隔符，允许省略分隔符的连续 hex */
    uint8_t data[64];
    int len = 0, hi = -1;
    for (const char *p = datastr; *p; p++) {
        if (*p == ' ' || *p == ',' || *p == '-') continue;
        int v;
        if (*p >= '0' && *p <= '9') v = *p - '0';
        else if (*p >= 'a' && *p <= 'f') v = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F') v = *p - 'A' + 10;
        else { len = -1; break; }
        if (hi < 0) hi = v;
        else {
            if (len >= 64) { len = -1; break; }
            data[len++] = (uint8_t)((hi << 4) | v);
            hi = -1;
        }
    }
    if (len < 0 || hi >= 0) {   /* 非法字符 / 奇数个 hex 字符 / 超长 */
        http_err(fd, 400, "Bad Request", "bad data");
        return;
    }

    /* 校验目标接口存在及其模式：经典 CAN 单帧最多 8 字节，
       否则 send 必然失败且前端无提示 */
    int iface_idx = -1;
    if (app && app->can) {
        for (int i = 0; i < app->can->count; i++)
            if (strcmp(app->can->ifaces[i].ifname, ifname) == 0) { iface_idx = i; break; }
    }
    if (iface_idx < 0) {
        http_err(fd, 404, "Not Found", "iface not found");
        return;
    }
    if (!app->can->ifaces[iface_idx].fd_mode && len > 8) {
        http_err(fd, 400, "Bad Request", "classic CAN max 8 bytes");
        return;
    }

    struct canfd_frame frame;
    memset(&frame, 0, sizeof(frame));
    frame.can_id = (canid_t)id;
    if (id > CAN_SFF_MASK) frame.can_id |= CAN_EFF_FLAG;
    frame.len = (uint8_t)len;
    memcpy(frame.data, data, (size_t)len);

    int rc = can_tx_frame(app, ifname, &frame);
    if (rc < 0) {
        http_err(fd, 502, "Bad Gateway", NULL);
        return;
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "{\"result\":\"%s\",\"bytes\":%d}",
             rc == 0 ? "queued" : "sent", rc);
    http_ok_json(fd, msg, strlen(msg));
}
