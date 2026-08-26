/**
 * http_util.c — HTTP 层公共工具实现（JSON 构建 / URL 解码 / 表单解析 /
 * 系统文件读取 / 便捷响应），供全部 http_api_*.c 复用。
 */

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "http/http_internal.h"
#include "http/http_util.h"

int http_json_append(char *json, size_t size, int off, const char *fmt, ...)
{
    if (!json || size == 0 || off < 0 || (size_t)off >= size) return -1;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(json + off, size - (size_t)off, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= size - (size_t)off) return -1;
    return off + n;
}

int http_json_escape(const char *in, char *out, size_t out_size)
{
    if (!in || !out || out_size == 0) return 0;
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 1 < out_size; i++) {
        char c = in[i];
        if (c == '"' || c == '\\') {
            if (o + 2 >= out_size) break;
            out[o++] = '\\';
            out[o++] = c;
        } else if (c >= 32 && c < 127) {
            out[o++] = c;
        }
    }
    out[o] = '\0';
    return (int)o;
}

/* URL 解码核心：只处理前 max_in 个字节（防止越过 '&' 等分隔符） */
static int url_decode_n(const char *src, size_t max_in, char *dst, size_t dst_size)
{
    if (!src || !dst || dst_size == 0) return 0;
    size_t i = 0, o = 0;
    while (i < max_in && src[i] && o + 1 < dst_size) {
        if (src[i] == '%' &&
            i + 2 < max_in &&
            isxdigit((unsigned char)src[i + 1]) &&
            isxdigit((unsigned char)src[i + 2])) {
            int hi = isdigit((unsigned char)src[i + 1])
                         ? src[i + 1] - '0'
                         : (tolower((unsigned char)src[i + 1]) - 'a' + 10);
            int lo = isdigit((unsigned char)src[i + 2])
                         ? src[i + 2] - '0'
                         : (tolower((unsigned char)src[i + 2]) - 'a' + 10);
            dst[o++] = (char)((hi << 4) | lo);
            i += 3;
        } else if (src[i] == '+') {
            dst[o++] = ' ';
            i++;
        } else {
            dst[o++] = src[i++];
        }
    }
    dst[o] = '\0';
    return (int)o;
}

int http_url_decode(const char *src, char *dst, size_t dst_size)
{
    return url_decode_n(src, (size_t)-1, dst, dst_size);
}

const char *http_body_start(const char *req)
{
    if (!req) return "";
    const char *sep = strstr(req, "\r\n\r\n");
    if (!sep) sep = strstr(req, "\n\n");
    if (sep) return sep + (sep[0] == '\r' ? 4 : 2);
    return req;
}

int http_form_parse(const char *req, http_form_field_t *fields, int max_fields)
{
    if (!req || !fields || max_fields <= 0) return 0;
    const char *p = http_body_start(req);
    int count = 0;
    while (*p && count < max_fields) {
        const char *eq = strchr(p, '=');
        const char *amp = strchr(p, '&');
        if (!eq) break;
        int klen = (int)(eq - p);
        if (klen > HTTP_FORM_KEY_MAX - 1) klen = HTTP_FORM_KEY_MAX - 1;
        memcpy(fields[count].key, p, (size_t)klen);
        fields[count].key[klen] = '\0';

        const char *vs = eq + 1;
        int vlen = amp ? (int)(amp - vs) : (int)strlen(vs);
        int dlen = url_decode_n(vs, (size_t)vlen, fields[count].val, HTTP_FORM_VAL_MAX);
        fields[count].val[dlen] = '\0';
        count++;
        p = amp ? amp + 1 : vs + vlen;
    }
    return count;
}

const char *http_form_find(const http_form_field_t *fields, int count, const char *key)
{
    for (int i = 0; i < count; i++)
        if (strcmp(fields[i].key, key) == 0 && fields[i].val[0])
            return fields[i].val;
    return NULL;
}

int http_form_get_param(const char *req, const char *key, char *out, size_t out_size)
{
    if (!req || !key || !out || out_size == 0) return 0;
    const char *p = http_body_start(req);
    size_t klen = strlen(key);
    while (*p) {
        const char *amp = strchr(p, '&');
        size_t seg = amp ? (size_t)(amp - p) : strlen(p);
        if (seg >= klen && strncmp(p, key, klen) == 0)
            return url_decode_n(p + klen, seg - klen, out, out_size);
        p = amp ? amp + 1 : p + seg;
    }
    return 0;
}

long http_read_key_long(const char *path, const char *key)
{
    if (!path || !key) return -1;
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    char line[256];
    long val = -1;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, key, strlen(key)) == 0) {
            sscanf(line + strlen(key), "%ld", &val);
            break;
        }
    }
    fclose(fp);
    return val;
}

long http_content_length(const char *req)
{
    if (!req) return -1;
    const char *cl = strstr(req, "Content-Length:");
    if (!cl) cl = strstr(req, "content-length:");
    if (!cl) return -1;
    char *end = NULL;
    long v = strtol(cl + 15, &end, 10);
    if (end == cl + 15 || v < 0) return -1;
    return v;
}

/* ---- 便捷响应封装 ---- */

void http_ok_json(int fd, const char *json, size_t len)
{
    http_send_response(fd, 200, "OK", "application/json", json, len);
}

void http_ok_text(int fd, const char *msg)
{
    http_send_response(fd, 200, "OK", "text/plain", msg ? msg : "", msg ? strlen(msg) : 0);
}

void http_err(int fd, int code, const char *status, const char *msg)
{
    http_send_response(fd, code, status, "text/plain",
                       msg ? msg : "", msg ? strlen(msg) : 0);
}
