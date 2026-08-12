#include <string.h>
/**
 * http_api_network.c — 网络接口流量统计 API。
 *   读取 /proc/net/dev，返回 eth0 / wlan0 的收发字节数。
 */

#include "http/http_internal.h"

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
