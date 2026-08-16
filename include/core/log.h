#ifndef LOG_H
#define LOG_H

/**
 * 初始化日志系统。
 * @param dir  日志目录路径
 *
 * info/error 分别写入按天命名的文件，存放在按日期建立的二级目录下：
 *   dir/YYYYMMDD/socketcan_info_YYYYMMDD.log
 *   dir/YYYYMMDD/socketcan_error_YYYYMMDD.log
 */
void log_init(const char *dir);

/* 关闭日志文件 */
void log_close(void);

void _log_info(const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
void _log_error(const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#define log_info(fmt, ...)  _log_info(__FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define log_error(fmt, ...) _log_error(__FILE__, __LINE__, fmt, ##__VA_ARGS__)

#endif /* LOG_H */
