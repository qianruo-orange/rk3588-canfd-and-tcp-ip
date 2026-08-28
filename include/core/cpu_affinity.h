#ifndef CPU_AFFINITY_H
#define CPU_AFFINITY_H

/**
 * core/cpu_affinity.h — 计算密集线程绑核：
 *   将耗时线程（视频采集/JPEG 解码/推理预处理/渲染编码/录像编码）绑定到
 *   大核（Cortex-A76）集合运行，小核（Cortex-A55）留给 CAN/TCP/HTTP 等
 *   I/O 线程与系统后台任务。
 *
 * 大核从 /proc/cpuinfo 的 "CPU part" 字段检测（0xd0b = Cortex-A76）；
 * 非异构平台（无大核）时自动降级为不绑核，不影响任何平台运行。
 */

/* 将当前线程绑定到大核集合。成功返回 0；无大核（降级不绑）或绑定失败
   返回 -1。可被任意线程在入口处调用，检测只执行一次。 */
int cpu_bind_big(void);

#endif /* CPU_AFFINITY_H */
