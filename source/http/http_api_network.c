#include <string.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netpacket/packet.h>
/**
 * http_api_network.c — 网络接口流量统计 & 接口列表 API。
 *   /api/network       读取 /proc/net/dev，返回 eth0 / wlan0 的收发字节数。
 *   /api/network/ifaces 枚举系统全部网络接口（排除回环），用于配置页"绑定网卡"下拉框。
 */

#include "http/http_internal.h"

/* ---- /api/network/ifaces —— 枚举系统网络接口（排除回环） ---- */
void http_network_ifaces(app_ctx_t *app, int fd, const char *method, const char *uri, const char *req_buf)
{
    (void)app; (void)method; (void)uri; (void)req_buf;
    char json[1024];
    int off = 0, first = 1;
    JSON_ADD(json, off, "[");

    struct ifaddrs *ifas = NULL;
    if (getifaddrs(&ifas) == 0) {
        for (struct ifaddrs *ifa = ifas; ifa; ifa = ifa->ifa_next) {
            if (!ifa->ifa_name || !ifa->ifa_addr) continue;
            if (ifa->ifa_addr->sa_family != AF_PACKET) continue;  /* 每个接口只计一次 */
            if (ifa->ifa_flags & IFF_LOOPBACK) continue;
            JSON_ADD(json, off, "%s\"%s\"", first ? "" : ",", ifa->ifa_name);
            first = 0;
        }
        freeifaddrs(ifas);
    }

    JSON_ADD(json, off, "]");
    http_send_response(fd, 200, "OK", "application/json", json, off);
}

void http_network_api(app_ctx_t *app, int fd)
{
    (void)app;
    const char *ifaces[] = { NET_IFACE_ETH, NET_IFACE_WLAN };
    char json[512];
    int off = snprintf(json, sizeof(json), "{");
    /* 边界检查宏：缓冲区写满后安全截断 */
#define JADD(fmt, ...) do { \
        int _n = snprintf(json + off, sizeof(json) - off, fmt, ##__VA_ARGS__); \
        if (_n < 0) { off = (int)sizeof(json); } \
        else if (_n >= (int)(sizeof(json) - off)) { off = (int)sizeof(json); } \
        else { off += _n; } \
    } while (0)

    FILE *fp = fopen("/proc/net/dev", "r");
    if (!fp) {
        JADD("\"%s\":{\"rx\":0,\"tx\":0},\"%s\":{\"rx\":0,\"tx\":0}}",
             ifaces[0], ifaces[1]);
        http_send_response(fd, 200, "OK", "application/json", json, off);
        return;
    }

    char line[256];
    int found = 0;
    while (fgets(line, sizeof(line), fp)) {
        char ifname[32];
        unsigned long rx_bytes, tx_bytes;
        if (sscanf(line, " %31[^:]: %lu %*u %*u %*u %*u %*u %*u %*u %lu",
                   ifname, &rx_bytes, &tx_bytes) == 3) {
            int match = 0;
            for (int j = 0; j < 2; j++)
                if (strcmp(ifname, ifaces[j]) == 0) { match = 1; break; }
            if (!match) continue;
            JADD("%s\"%s\":{\"rx\":%lu,\"tx\":%lu}",
                 found > 0 ? "," : "", ifname, rx_bytes, tx_bytes);
            found++;
        }
    }
    fclose(fp);

    /* 补全缺失接口 */
    for (int i = 0, need = (found > 0); i < 2; i++) {
        if (strstr(json, ifaces[i])) continue;
        JADD("%s\"%s\":{\"rx\":0,\"tx\":0}",
             need ? "," : "", ifaces[i]);
        need = 1;
    }

    JADD("}");
    http_send_response(fd, 200, "OK", "application/json", json, off);
}
