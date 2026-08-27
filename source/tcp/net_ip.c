/**
 * net_ip.c — 网卡 IP 配置：静态/DHCP 应用（ip 命令）、运行时地址读取。
 * 供配置页 IP 设置卡片（target=ip）与开机启动应用使用。
 */

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "core/common.h"
#include "core/config.h"
#include "core/log.h"
#include "tcp/net_ip.h"

/* 点分掩码 → 前缀长度（如 255.255.255.0 → 24） */
static int mask_to_prefix(const char *mask)
{
    struct in_addr a;
    if (inet_pton(AF_INET, mask, &a) != 1) return 24;
    uint32_t m = ntohl(a.s_addr);
    int p = 0;
    while (m & 0x80000000u) { p++; m <<= 1; }
    return p;
}

int net_ip_apply(const char *ifname, const char *mode,
                 const char *addr, const char *mask, const char *gw)
{
    if (!ifname || !ifname[0]) return -1;
    char cmd[512];

    /* 先使接口 up 并清空旧地址（避免 add 冲突） */
    snprintf(cmd, sizeof(cmd), "ip link set %s up 2>/dev/null; ip addr flush dev %s 2>/dev/null",
             ifname, ifname);
    system(cmd);

    if (mode && strcmp(mode, "static") == 0 && addr && addr[0] && mask && mask[0]) {
        struct in_addr check;
        if (inet_pton(AF_INET, addr, &check) != 1) {
            LOG_ERROR("net_ip: invalid static address '%s'", addr);
            return -1;
        }
        int prefix = mask_to_prefix(mask);
        snprintf(cmd, sizeof(cmd), "ip addr add %s/%d dev %s 2>/dev/null", addr, prefix, ifname);
        int rc = system(cmd);
        LOG_INFO("net_ip: %s -> %s/%d (%s)", ifname, addr, prefix, rc == 0 ? "ok" : "failed");
        if (gw && gw[0]) {
            snprintf(cmd, sizeof(cmd), "ip route replace default via %s dev %s 2>/dev/null", gw, ifname);
            system(cmd);
        }
        return rc == 0 ? 0 : -1;
    }

    if (mode && strcmp(mode, "dhcp") == 0) {
        /* busybox udhcpc 优先，失败回退 dhclient */
        snprintf(cmd, sizeof(cmd), "udhcpc -i %s -b -q 2>/dev/null || dhclient %s 2>/dev/null",
                 ifname, ifname);
        int rc = system(cmd);
        LOG_INFO("net_ip: %s -> dhcp (%s)", ifname, rc == 0 ? "ok" : "failed");
        return rc == 0 ? 0 : -1;
    }

    LOG_INFO("net_ip: %s ip mode off, left unchanged", ifname);
    return 0;
}

int net_ip_get_current(const char *ifname,
                       char *addr, size_t addr_sz,
                       char *mask, size_t mask_sz,
                       char *gw,   size_t gw_sz)
{
    if (!ifname || !ifname[0]) return -1;
    if (addr) addr[0] = '\0';
    if (mask) mask[0] = '\0';
    if (gw)   gw[0]   = '\0';

    struct ifaddrs *ifas = NULL;
    if (getifaddrs(&ifas) == 0) {
        for (struct ifaddrs *ifa = ifas; ifa; ifa = ifa->ifa_next) {
            if (!ifa->ifa_name || !ifa->ifa_addr || !ifa->ifa_netmask) continue;
            if (ifa->ifa_addr->sa_family != AF_INET) continue;
            if (strcmp(ifa->ifa_name, ifname) != 0) continue;
            struct sockaddr_in *a4 = (struct sockaddr_in *)ifa->ifa_addr;
            struct sockaddr_in *m4 = (struct sockaddr_in *)ifa->ifa_netmask;
            if (addr) inet_ntop(AF_INET, &a4->sin_addr, addr, addr_sz);
            if (mask) inet_ntop(AF_INET, &m4->sin_addr, mask, mask_sz);
        }
        freeifaddrs(ifas);
    }

    /* 网关：/proc/net/route 中目标 0.0.0.0 且 dev == ifname 的表项 */
    FILE *fp = fopen("/proc/net/route", "r");
    if (fp && gw) {
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            char dev[32];
            unsigned int dest = 0, route_gw = 0;
            if (sscanf(line, "%31s %x %x", dev, &dest, &route_gw) == 3 &&
                dest == 0 && strcmp(dev, ifname) == 0 && route_gw) {
                struct in_addr ga;
                ga.s_addr = route_gw;   /* /proc/net/route 字段为主机字节序（小端平台） */
                inet_ntop(AF_INET, &ga, gw, gw_sz);
                break;
            }
        }
        fclose(fp);
    }
    return 0;
}

/* ---- 后台延迟应用（先回 HTTP 响应，再改 IP） ---- */
typedef struct {
    char ifname[IFNAMSIZ];
    char mode[8];
    char addr[32];
    char mask[32];
    char gw[32];
} net_ip_job_t;

static void *net_ip_worker(void *p)
{
    net_ip_job_t *j = p;
    usleep(300000);   /* 等响应发出后再断网 */
    net_ip_apply(j->ifname, j->mode, j->addr, j->mask, j->gw);
    free(j);
    return NULL;
}

void net_ip_apply_async(const char *ifname, const char *mode,
                        const char *addr, const char *mask, const char *gw)
{
    net_ip_job_t *j = calloc(1, sizeof(*j));
    if (!j) return;
    safe_strncpy(j->ifname, sizeof(j->ifname), ifname);
    safe_strncpy(j->mode,   sizeof(j->mode),   mode);
    safe_strncpy(j->addr,   sizeof(j->addr),   addr);
    safe_strncpy(j->mask,   sizeof(j->mask),   mask);
    safe_strncpy(j->gw,     sizeof(j->gw),     gw);

    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &attr, net_ip_worker, j) != 0) free(j);
    pthread_attr_destroy(&attr);
}

void net_ip_apply_cfg(const struct app_config_t *cfg)
{
    const app_args_t *a = &cfg->args;
    /* IP 配置针对 TCP 绑定网卡：未单独配置接口时回退用 tcp_bind */
    const char *ifn = a->ip_ifname[0] ? a->ip_ifname
                    : (a->tcp_bind[0] ? a->tcp_bind : "");
    if (!ifn[0] || !a->ip_mode[0]) return;
    LOG_INFO("net_ip: applying boot config on %s (mode=%s)", ifn, a->ip_mode);
    net_ip_apply(ifn, a->ip_mode, a->ip_addr, a->ip_mask, a->ip_gw);
}
