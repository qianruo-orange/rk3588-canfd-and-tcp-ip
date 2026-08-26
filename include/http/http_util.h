#ifndef HTTP_UTIL_H
#define HTTP_UTIL_H

#include <stddef.h>
#include <stdint.h>

/**
 * http_util.h — HTTP 层公共工具：JSON 构建 / URL 解码 / 表单解析 /
 * 系统文件读取等复用函数（消除各 http_api_*.c 中的重复实现）。
 */

/* 表单字段键/值容量（与 http_api_config.c 原 MAX_FORM_FIELDS 一致） */
#define HTTP_FORM_KEY_MAX 64
#define HTTP_FORM_VAL_MAX 256

typedef struct {
    char key[HTTP_FORM_KEY_MAX];
    char val[HTTP_FORM_VAL_MAX];
} http_form_field_t;

/* 带边界检查的 JSON 追加（vsnprintf 语义）。成功返回新的 off，溢出/非法返回 -1。
   用于需要"返回值判断"的构建场景；简单场景直接用 http_internal.h 的 JSON_ADD 宏。 */
int http_json_append(char *json, size_t size, int off, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

/* JSON 字符串转义：把 '"' 与 '\' 转义、过滤不可见字符（0x20~0x7E 之外）。
   返回写入字节数（不含结尾 NUL），out 恒以 NUL 结尾。 */
int http_json_escape(const char *in, char *out, size_t out_size);

/* URL 解码：%XX 十六进制转义还原 + '+' 转空格。返回写入字节数，未匹配返回 0。 */
int http_url_decode(const char *src, char *dst, size_t dst_size);

/* 跳过 HTTP 头（\r\n\r\n / \n\n），返回 body 起始；无头则返回原串。 */
const char *http_body_start(const char *req);

/* 解析 URL 编码表单（application/x-www-form-urlencoded）到字段数组。
   req 可含 HTTP 头（内部自动跳过）。返回字段数。 */
int http_form_parse(const char *req, http_form_field_t *fields, int max_fields);

/* 从已解析的字段数组中查找 key，返回其值指针（空值视为未找到）。 */
const char *http_form_find(const http_form_field_t *fields, int count, const char *key);

/* 从 URL 编码请求体/查询串中提取单个 key 的值并解码（自动跳过 HTTP 头）。
   返回写入字节数（含 '+'/ '%' 解码）；未找到返回 0。 */
int http_form_get_param(const char *req, const char *key, char *out, size_t out_size);

/* 读取 proc/sysfs 文本文件中以 key 前缀开头的行的整数值；文件不可读或未匹配返回 -1。 */
long http_read_key_long(const char *path, const char *key);

/* 便捷响应封装（内部走 http_send_response，自动计算长度） */
void http_ok_json(int fd, const char *json, size_t len);
void http_ok_text(int fd, const char *msg);
void http_err(int fd, int code, const char *status, const char *msg);

#endif /* HTTP_UTIL_H */
