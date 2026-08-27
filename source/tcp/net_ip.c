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

/* 校验网卡名：只允许字母/数字/./_/:-，防命令注入（名称会拼进 system() 命令） */
static int valid_ifname(const char *ifname)
{
    if (!ifname || !ifname[0] || strlen(ifname) >= IFNAMSIZ) return 0;
    for (const char *p = ifname; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '.' || *p == '_' ||
              *p == ':' || *p == '-'))
            return 0;
    }
    return 1;
}

int net_ip_apply(const char *ifname, const char *mode,
                 const char *addr, const char *mask, const char *gw)
{
    if (!valid_ifname(ifname)) {
        LOG_ERROR("net_ip: invalid ifname rejected");
        return -1;
    }
    int is_static = mode && strcmp(mode, "static") == 0;
    int is_dhcp   = mode && strcmp(mode, "dhcp") == 0;
    /* mode off/空：完全不碰接口（此前无条件 flush 会把管理口地址清掉导致断连） */
    if (!is_static && !is_dhcp) {
        LOG_INFO("net_ip: %s ip mode off, left unchanged", ifname);
        return 0;
    }

    /* 先校验静态参数，再动接口：地址非法时不再 flush 后裸奔（接口无地址） */
    struct in_addr check;
    int has_addr = addr && addr[0] && inet_pton(AF_INET, addr, &check) == 1;
    int has_mask = mask && mask[0];
    int has_gw   = gw && gw[0] && inet_pton(AF_INET, gw, &check) == 1;
    if (is_static && (!has_addr || !has_mask)) {
        LOG_INFO("net_ip: %s mode '%s' not applied (no valid address given)",
                 ifname, mode ? mode : "");
        return -1;
    }
    if (gw && gw[0] && !has_gw)   /* gw 会拼进 system() 命令，非法则忽略 */
        LOG_ERROR("net_ip: invalid gateway '%s' ignored", gw);

    char cmd[512];
    /* 使接口 up 并清空旧地址（避免 add 冲突）——仅在确实要配置时执行 */
    snprintf(cmd, sizeof(cmd), "ip link set %s up 2>/dev/null; ip addr flush dev %s 2>/dev/null",
             ifname, ifname);
    system(cmd);

    if (is_static && has_addr) {
        int prefix = mask_to_prefix(mask);
        snprintf(cmd, sizeof(cmd), "ip addr add %s/%d dev %s 2>/dev/null", addr, prefix, ifname);
        int rc = system(cmd);
        LOG_INFO("net_ip: %s -> %s/%d (%s)", ifname, addr, prefix, rc == 0 ? "ok" : "failed");
        if (has_gw) {
            snprintf(cmd, sizeof(cmd), "ip route replace default via %s dev %s 2>/dev/null", gw, ifname);
            system(cmd);
        }
        return rc == 0 ? 0 : -1;
    }

    if (is_dhcp) {
        /* busybox udhcpc 优先，失败回退 dhclient */
        snprintf(cmd, sizeof(cmd), "udhcpc -i %s -b -q 2>/dev/null || dhclient %s 2>/dev/null",
                 ifname, ifname);
        int rc = system(cmd);
        LOG_INFO("net_ip: %s -> dhcp (%s)", ifname, rc == 0 ? "ok" : "failed");
        return rc == 0 ? 0 : -1;
    }

    LOG_INFO("net_ip: %s mode '%s' not applied (no address given)", ifname, mode ? mode : "");
    return -1;
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

/* 串行化所有后台 IP 应用：连续保存会起多个 detached 线程，
   不串行时各线程的 system() 命令交错执行（flush/add 竞态），接口状态不可预期 */
static pthread_mutex_t g_apply_lock = PTHREAD_MUTEX_INITIALIZER;

static void *net_ip_worker(void *p)
{
    net_ip_job_t *j = p;
    usleep(300000);   /* 等响应发出后再断网 */
    pthread_mutex_lock(&g_apply_lock);
    net_ip_apply(j->ifname, j->mode, j->addr, j->mask, j->gw);
    pthread_mutex_unlock(&g_apply_lock);
    free(j);
    return NULL;
}

void net_ip_apply_async(const char *ifname, const char *mode,
                        const char *addr, const char *mask, const char *gw)
{
    net_ip_job_t *j = calloc(1, sizeof(*j));
    if (!j) return;
    if (!valid_ifname(ifname)) {   /* 异步入口同样校验，不把恶意串放进线程 */
        LOG_ERROR("net_ip: invalid ifname rejected (async)");
        free(j);
        return;
    }
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
