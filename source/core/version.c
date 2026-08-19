/**
 * version.c — 版本信息辅助函数。
 *
 * 版本号 / git 提交 / 构建信息由 CMake 直接生成到
 * include/core/version.h，本文件仅做格式化输出。
 */

#include <stdio.h>

#include "core/version.h"

/* 短版本号：如 "1.0.0" */
static const char *app_version_short(void)
{
    return APP_VERSION;
}

/* 完整版本串：如
 *   data_transport_test 1.0.0 (git:dd072de+ branch:main build:Release 2026-08-12 10:00:00)
 * '+' 后缀表示工作区存在未提交修改（git dirty）。 */
static const char *app_version_full(void)
{
    static char buf[256];
    snprintf(buf, sizeof(buf),
             "%s %s (git:%s%s branch:%s build:%s %s)",
             APP_NAME, APP_VERSION,
             APP_GIT_COMMIT, APP_GIT_DIRTY ? "+" : "",
             APP_GIT_BRANCH, APP_BUILD_TYPE, APP_BUILD_DATE);
    return buf;
}
