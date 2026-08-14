#define _GNU_SOURCE

/**
 * main.c — 模块总表框架：
 *   每个模块一个 module_t：
 *     name 模块名，tid 运行时线程句柄
 *     ops  模块生命周期函数（init/dtor/task）
 *     wd   看门狗参数（timeout/max_miss）
 *     thr  线程参数（stack_size/priority/cpu）
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>

#include "core/config.h"
#include "core/data_flow.h"
#include "can/can_socket.h"
#include "net/tcp_server.h"
#include "http/http.h"
#include "watchdog/watchdog.h"
#include "core/log.h"
#include "core/common.h"
#include "video/video_stream.h"

/* 模块生命周期函数 */
typedef struct {
    int  (*init)(void *arg);
    void (*dtor)(void *arg);
    void *(*task)(void *);
} module_ops_t;

/* 看门狗参数 */
typedef struct {
    int timeout;
    int max_miss;
} module_wd_t;

/* 线程参数（创建属性） */
typedef struct {
    size_t stack_size;   /* 线程栈大小（字节），0 = 系统默认 */
    int    priority;     /* 调度优先级，0 = 默认调度，>0 = SCHED_FIFO 实时优先级(1-99) */
    int    cpu;          /* 绑定核心，-1 = 不绑定，>=0 = 绑定到指定 CPU */
} module_thread_cfg_t;

/* 模块总表条目：一个模块的完整描述 */
typedef struct {
    pthread_t           tid;
    const char         *name;
    module_ops_t        ops;
    module_wd_t         wd;
    module_thread_cfg_t thr;
} module_t;

static void sig_handler(int sig);

static void set_current_thread_name(const char *name)
{
    if (!name || !*name) return;
    char short_name[16];
    safe_strncpy(short_name, sizeof(short_name), name);
    (void)pthread_setname_np(pthread_self(), short_name);
}

static int signal_setup(void *arg)
{
    (void)arg;
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    /* 写已断开 socket 会触发 SIGPIPE，默认会终止进程；统一忽略，
       由 write() 的返回值处理错误（见 tcp_server.c / http.c） */
    signal(SIGPIPE, SIG_IGN);
    return 0;
}

/* 文件级应用上下文（仅本文件直接访问；其他模块通过指针参数获得） */
static app_ctx_t g_app;

static module_t g_modules[] = {
    /* can */
    {
        .tid  = 0,
        .name = "can",
        .ops  = { .init = can_init, .dtor = can_cleanup, .task = can_task },
        .wd   = { .timeout = 3, .max_miss = 3 },
        .thr  = { .stack_size = 0, .priority = 0, .cpu = -1 },
    },
    /* tcp */
    {
        .tid  = 0,
        .name = "tcp",
        .ops  = { .init = tcp_init, .dtor = tcp_cleanup, .task = tcp_task },
        .wd   = { .timeout = 5, .max_miss = 3 },
        .thr  = { .stack_size = 0, .priority = 0, .cpu = -1 },
    },
    /* http */
    {
        .tid  = 0,
        .name = "http",
        .ops  = { .init = http_server_start, .dtor = http_server_stop, .task = http_server_task },
        .wd   = { .timeout = 5, .max_miss = 3 },
        .thr  = { .stack_size = 0, .priority = 0, .cpu = -1 },
    },
    /* video */
    {
        .tid  = 0,
        .name = "video",
        .ops  = { .init = video_stream_init, .dtor = video_stream_shutdown, .task = video_stream_task },
        .wd   = { .timeout = 5, .max_miss = 3 },
        .thr  = { .stack_size = 0, .priority = 0, .cpu = -1 },
    },
    /* watchdog —— 监控者，不能监督自己，故 timeout=0（不注册自身） */
    {
        .tid  = 0,
        .name = "watchdog",
        .ops  = { .init = watchdog_init, .dtor = NULL, .task = watchdog_task },
        .wd   = { .timeout = 0, .max_miss = 0 },
        .thr  = { .stack_size = 0, .priority = 0, .cpu = -1 },
    },
    /* signal */
    {
        .tid  = 0,
        .name = "signal",
        .ops  = { .init = signal_setup, .dtor = NULL, .task = NULL },
        .wd   = { .timeout = 0, .max_miss = 0 },
        .thr  = { .stack_size = 0, .priority = 0, .cpu = -1 },
    },
};
#define MOD_COUNT (int)(sizeof(g_modules)/sizeof(g_modules[0]))

/**
 * sig_handler - 处理退出信号，通知主循环停止服务。
 * @sig: 接收到的信号编号。
 */
static void sig_handler(int sig) { (void)sig; g_app.running = 0; }

/**
 * thread_wrapper - 线程入口函数，执行模块线程并在退出时递减活跃线程计数。
 * @arg: 模块下标（intptr_t 转 void*）。
 */
static void *thread_wrapper(void *arg)
{
    int idx = (int)(intptr_t)arg;
    const char *name = g_modules[idx].name;
    set_current_thread_name(name);
    if (g_modules[idx].ops.task) g_modules[idx].ops.task(&g_app);
    watchdog_unregister_self(name);
    __atomic_fetch_sub(&g_app.threads_running, 1, __ATOMIC_RELEASE);
    return NULL;
}

/**
 * wait_threads_exit - 在有限时间内等待所有工作线程退出，避免析构与线程并发访问资源。
 */
static void wait_threads_exit(void)
{
    for (int i = 0; i < 50; i++) {
        if (__atomic_load_n(&g_app.threads_running, __ATOMIC_ACQUIRE) <= 0) break;
        usleep(100000);   /* 每次 100ms，最多 5s */
    }
}

/**
 * shutdown_modules_safe - 安全关闭各模块并等待线程退出，避免在关闭时出现竞争或卡死。
 */
static void shutdown_modules_safe(void)
{
    log_info("Shutting down modules...");
    for (int i = 0; i < MOD_COUNT; i++) {
        if (g_modules[i].ops.dtor) {
            log_info("Shutting down module %s...", g_modules[i].name);
            g_modules[i].ops.dtor(&g_app);
        }
    }
    log_info("Modules shut down successfully.");
    g_app.running = 0;
    http_server_stop(&g_app);
    video_stream_shutdown(&g_app);
    wait_threads_exit();
}

/**
 * main - 初始化所有模块、创建线程、进入主循环，并在退出时统一清理资源。
 */
int main(void)
{
    can_ctx_t           can_ctx;
    tcp_ctx_t           tcp_ctx;
    struct app_config_t cfg;

    memset(&g_app, 0, sizeof(g_app));
    g_app.running = 1;
    pthread_mutex_init(&g_app.can_mutex, NULL);
    g_app.can = &can_ctx;
    g_app.tcp = &tcp_ctx;
    g_app.cfg = &cfg;

    log_init(PATH_LOGS);
    config_load(&cfg);
    log_close(); log_init(cfg.log_dir);

    signal(SIGINT, sig_handler); signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    /* 注册数据流虚函数实现（默认各域数据流独立、不做桥接；业务可整体替换或逐项覆盖） */
    data_flow_register(&g_app, data_flow_default_ops());
    set_current_thread_name("main");

    for (int i = 0; i < MOD_COUNT; i++) {
        module_t *m = &g_modules[i];

        if (m->ops.init && m->ops.init(&g_app) < 0) goto fail;
        if (!m->ops.task) continue;

        /* 按模块线程属性创建 detached 线程 */
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

        if (m->thr.stack_size > 0)
            pthread_attr_setstacksize(&attr, m->thr.stack_size);

        if (m->thr.priority > 0) {
            struct sched_param sp;
            memset(&sp, 0, sizeof(sp));
            sp.sched_priority = m->thr.priority;
            pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
            pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
            pthread_attr_setschedparam(&attr, &sp);
        }

        __atomic_fetch_add(&g_app.threads_running, 1, __ATOMIC_RELAXED);
        int rc = pthread_create(&m->tid, &attr, thread_wrapper, (void *)(intptr_t)i);
        pthread_attr_destroy(&attr);

        if (rc != 0) {
            __atomic_fetch_sub(&g_app.threads_running, 1, __ATOMIC_RELAXED);
            log_error("create thread '%s' failed: %s", m->name, strerror(rc));
            goto fail;
        }

        /* 绑核（可选） */
        if (m->thr.cpu >= 0) {
            cpu_set_t set;
            CPU_ZERO(&set);
            CPU_SET(m->thr.cpu, &set);
            if (pthread_setaffinity_np(m->tid, sizeof(set), &set) != 0)
                log_error("bind thread '%s' to cpu %d failed: %s",
                          m->name, m->thr.cpu, strerror(errno));
        }

        /* 注册看门狗（timeout>0 才纳入监督） */
        if (m->wd.timeout > 0)
            watchdog_register_thread(m->tid, m->name, m->wd.timeout, m->wd.max_miss);
    }

    watchdog_register_thread(pthread_self(), "main", 15, 1);
    while (g_app.running) { watchdog_feed_self("main"); sleep(1); }
    watchdog_unregister_self("main");

    /* 通知各模块停止，让工作线程尽快退出 */
    shutdown_modules_safe();

    for (int i = MOD_COUNT-1; i >= 0; i--)
        if (g_modules[i].ops.dtor) g_modules[i].ops.dtor(&g_app);
    log_close(); return 0;

fail:
    watchdog_unregister_self("main");
    shutdown_modules_safe();
    for (int i = MOD_COUNT-1; i >= 0; i--)
        if (g_modules[i].ops.dtor) g_modules[i].ops.dtor(&g_app);
    log_close(); return 1;
}
