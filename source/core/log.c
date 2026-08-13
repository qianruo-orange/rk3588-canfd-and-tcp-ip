#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <limits.h>

#include "core/log.h"
#include "core/common.h"
#include "core/version.h"

typedef struct {
    FILE    *fp_info;
    FILE    *fp_error;
    char     dir[256];
    char     date[16];
    pthread_mutex_t mutex;
} log_ctx_t;

static log_ctx_t g_log = { .mutex = PTHREAD_MUTEX_INITIALIZER };

#define LOG_MAX_SIZE (10 * 1024 * 1024)  /* 10MB */

/* 若文件超过大小限制，重命名为 .1 备份 */
static void rotate_if_needed(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0 && st.st_size > LOG_MAX_SIZE) {
        char bak[PATH_MAX];
        snprintf(bak, sizeof(bak), "%s.1", path);
        rename(path, bak);
    }
}

/* 每次写入前检查当前文件大小，超限即轮转（解决同一天内不轮转的问题） */
static void rotate_check(int is_error)
{
    FILE **fpp = is_error ? &g_log.fp_error : &g_log.fp_info;
    if (!*fpp || *fpp == stderr) return;
    struct stat st;
    if (fstat(fileno(*fpp), &st) != 0 || st.st_size <= LOG_MAX_SIZE) return;

    char path[PATH_MAX], bak[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s_%s_%s.log",
             g_log.dir, APP_NAME, is_error ? "error" : "info", g_log.date);
    snprintf(bak, sizeof(bak), "%s.1", path);
    fclose(*fpp);
    rename(path, bak);
    *fpp = fopen(path, "a");
    if (!*fpp) *fpp = stderr;
}

static void ensure_date(void)
{
    time_t now = time(NULL);
    struct tm tm_buf, *tm = localtime_r(&now, &tm_buf);
    char cur[16]; strftime(cur, sizeof(cur), "%Y%m%d", tm);
    if (strcmp(cur, g_log.date) == 0) return;

    safe_strncpy(g_log.date, sizeof(g_log.date), cur);
    if (g_log.fp_info && g_log.fp_info != stderr)  fclose(g_log.fp_info);
    if (g_log.fp_error && g_log.fp_error != stderr) fclose(g_log.fp_error);

    char path[512];
    snprintf(path, sizeof(path), "%s/%s_info_%s.log", g_log.dir, APP_NAME, g_log.date);
    rotate_if_needed(path);
    g_log.fp_info = fopen(path, "a"); if (!g_log.fp_info) g_log.fp_info = stderr;
    snprintf(path, sizeof(path), "%s/%s_error_%s.log", g_log.dir, APP_NAME, g_log.date);
    rotate_if_needed(path);
    g_log.fp_error = fopen(path, "a"); if (!g_log.fp_error) g_log.fp_error = stderr;
}

void log_init(const char *dir)
{
    pthread_mutex_lock(&g_log.mutex);
    safe_strncpy(g_log.dir, sizeof(g_log.dir), dir);
    mkdir(dir, 0755);
    g_log.date[0] = '\0';
    ensure_date();
    pthread_mutex_unlock(&g_log.mutex);
}

void log_close(void)
{
    pthread_mutex_lock(&g_log.mutex);
    if (g_log.fp_info  && g_log.fp_info  != stderr) fclose(g_log.fp_info);
    if (g_log.fp_error && g_log.fp_error != stderr) fclose(g_log.fp_error);
    g_log.fp_info = g_log.fp_error = NULL;
    pthread_mutex_unlock(&g_log.mutex);
}

static void log_msg(FILE *fp, const char *file, int line,
                    const char *level, const char *fmt, va_list ap)
{
    struct timeval tv; gettimeofday(&tv, NULL);
    struct tm tm_buf, *tm = localtime_r(&tv.tv_sec, &tm_buf);
    char buf[4096];
    int off = snprintf(buf, sizeof(buf), "[%02d:%02d:%02d.%03ld] %-5s %s:%d ",
                       tm->tm_hour, tm->tm_min, tm->tm_sec, tv.tv_usec/1000, level, file, line);
    off += vsnprintf(buf + off, sizeof(buf) - off, fmt, ap);
    fprintf(fp, "%s\n", buf); fflush(fp);
}

void _log_info(const char *file, int line, const char *fmt, ...)
{
    pthread_mutex_lock(&g_log.mutex); ensure_date();
    rotate_check(0);
    va_list ap; va_start(ap, fmt);
    log_msg(g_log.fp_info, file, line, "INFO", fmt, ap);
    va_end(ap);
    pthread_mutex_unlock(&g_log.mutex);
}

void _log_error(const char *file, int line, const char *fmt, ...)
{
    pthread_mutex_lock(&g_log.mutex); ensure_date();
    rotate_check(1);
    va_list ap; va_start(ap, fmt);
    log_msg(g_log.fp_error, file, line, "ERROR", fmt, ap);
    va_end(ap);
    pthread_mutex_unlock(&g_log.mutex);
}
