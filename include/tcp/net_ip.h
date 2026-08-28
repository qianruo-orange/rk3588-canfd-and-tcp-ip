/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NET_IP_H
#define NET_IP_H

/* net_ip.h — 网卡 IP 配置：静态/DHCP 应用、运行时信息读取 */

struct app_config_t;

/* 立即对 ifname 应用 IP 配置：
   mode 为 "" 不处理；"static" 使用 addr/mask（gw 可选）；"dhcp" 自动获取。
   返回 0 成功。 */
int  net_ip_apply(const char *ifname, const char *mode,
                  const char *addr, const char *mask, const char *gw);

/* 后台线程延迟 ~300ms 应用（先让 HTTP 响应发出，避免改 IP 切断当前连接）；
   参数会被拷贝，调用方可复用栈变量。 */
void net_ip_apply_async(const char *ifname, const char *mode,
                        const char *addr, const char *mask, const char *gw);

/* 读取 ifname 当前 IPv4 地址/掩码/网关（无则置空串）。返回 0 成功 */
int  net_ip_get_current(const char *ifname,
                        char *addr, size_t addr_sz,
                        char *mask, size_t mask_sz,
                        char *gw,   size_t gw_sz);

/* 启动时按配置应用（未配置 ip_mode 则跳过）；失败仅记日志 */
void net_ip_apply_cfg(const struct app_config_t *cfg);

#endif /* NET_IP_H */
