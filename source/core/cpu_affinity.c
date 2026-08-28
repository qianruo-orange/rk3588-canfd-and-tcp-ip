#define _GNU_SOURCE

/**
 * cpu_affinity.c — 计算密集线程绑核（详见 include/core/cpu_affinity.h）。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <errno.h>

#include "core/cpu_affinity.h"
#include "core/log.h"

/* Cortex-A76 的 CPU part 编号（RK3588 大核；0xd05 为 Cortex-A55 小核） */
#define CPU_PART_A76 0xd0b

static pthread_once_t s_once = PTHREAD_ONCE_INIT;
static cpu_set_t      s_big_set;
static int            s_big_count = 0;
static int            s_logged = 0;

/* 从 /proc/cpuinfo 解析大核集合。cpuinfo 行形如：
     processor : 4
     CPU part  : 0xd0b
   分隔符为空格/Tab 混排，统一从首个 ':' 之后取数值。 */
static void cpu_detect_big(void)
{
    FILE *fp = fopen("/proc/cpuinfo", "r");
    if (!fp) return;
    CPU_ZERO(&s_big_set);
    char line[256];
    int cur = -1;
    while (fgets(line, sizeof(line), fp)) {
        const char *colon = strchr(line, ':');
        if (!colon) continue;
        if (strncmp(line, "processor", 9) == 0) {
            cur = atoi(colon + 1);
        } else if (strncmp(line, "CPU part", 8) == 0) {
            if ((unsigned int)strtoul(colon + 1, NULL, 0) == CPU_PART_A76 &&
                cur >= 0 && cur < CPU_SETSIZE) {
                CPU_SET(cur, &s_big_set);
                s_big_count++;
            }
            cur = -1;
        }
    }
    fclose(fp);
}

int cpu_bind_big(void)
{
    pthread_once(&s_once, cpu_detect_big);
    if (s_big_count <= 0) return -1;   /* 非异构平台：不绑核，交给默认调度 */

    if (pthread_setaffinity_np(pthread_self(), sizeof(s_big_set), &s_big_set) != 0) {
        if (!s_logged)
            LOG_ERROR("cpu: bind big cores failed: %s", strerror(errno));
        s_logged = 1;
        return -1;
    }
    if (!s_logged) {
        char buf[128] = "";
        for (int i = 0; i < CPU_SETSIZE && i < 64; i++) {
            if (CPU_ISSET(i, &s_big_set)) {
                char t[8];
                snprintf(t, sizeof(t), "%s%d", buf[0] ? "," : "", i);
                strncat(buf, t, sizeof(buf) - strlen(buf) - 1);
            }
        }
        LOG_INFO("cpu: heavy threads bound to big cores [%s] (%d core(s))",
                 buf, s_big_count);
    }
    s_logged = 1;
    return 0;
}
