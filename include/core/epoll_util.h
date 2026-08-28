/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CORE_EPOLL_UTIL_H
#define CORE_EPOLL_UTIL_H

/**
 * core/epoll_util.h — epoll 等待循环公共封装。
 *
 * can_recv_task / can_send_task / tcp_task / http_server_task 四个主循环
 * 共用同一段"epoll_wait → EINTR 重试并喂看门狗 → 喂狗 → 分发"骨架，
 * 收口为 epoll_wait_feed()：成功返回就绪数，真实错误打日志并返回 -1。
 */

#include <errno.h>
#include <sys/epoll.h>

#include "core/log.h"
#include "watchdog/watchdog.h"

/* 等待就绪事件：EINTR 时重试并喂狗（保持看门狗存活），真实错误打日志返回 -1 */
static inline int epoll_wait_feed(int epfd, struct epoll_event *events, int maxevents,
                                  int timeout_ms, const char *tag)
{
    for (;;) {
        int n = epoll_wait(epfd, events, maxevents, timeout_ms);
        if (n >= 0) {
            watchdog_feed_self(tag);
            return n;
        }
        if (errno == EINTR) {
            watchdog_feed_self(tag);
            continue;
        }
        LOG_ERROR("%s: epoll_wait", tag);
        return -1;
    }
}

#endif /* CORE_EPOLL_UTIL_H */
