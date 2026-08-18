/**
 * http_api_dbc.c — DBC 解码结果的 HTTP 展示模块
 *
 * 维护一个最近解码结果的环形缓存，供 /api/can/decoded 接口以 JSON 返回。
 * 解码在 can_task 接收路径调用 dbc_decode_frame() 完成后，由调用方通过
 * http_dbc_record() 记录；HTTP 层调用 http_dbc_recent_json() 读取。
 */

#include <stdint.h>
#include <string.h>
#include <pthread.h>

#include "http/http_api_dbc.h"
#include "http/http_internal.h"   /* http_send_response */
#include "core/common.h"          /* safe_strncpy */
#include "can/dbc_parser.h"       /* DBC_MAX_NAME_LEN */

#define DECODE_RING_MAX 32
#define DECODE_TEXT_MAX 512

typedef struct {
    char    ifname[16];
    canid_t can_id;
    char    name[DBC_MAX_NAME_LEN];
    char    text[DECODE_TEXT_MAX];
} decode_entry_t;

static decode_entry_t  g_decode_ring[DECODE_RING_MAX];
static _Atomic int     g_decode_count = 0;
static pthread_mutex_t g_decode_mutex = PTHREAD_MUTEX_INITIALIZER;

void http_dbc_record(const char *ifname, canid_t can_id,
                     const char *name, const char *text)
{
    if (!ifname || !name || !text) return;
    pthread_mutex_lock(&g_decode_mutex);
    int pos = (int)(__atomic_fetch_add(&g_decode_count, 1, __ATOMIC_RELAXED) % DECODE_RING_MAX);
    decode_entry_t *e = &g_decode_ring[pos];
    safe_strncpy(e->ifname, sizeof(e->ifname), ifname);
    e->can_id = can_id;
    safe_strncpy(e->name, sizeof(e->name), name);
    safe_strncpy(e->text, sizeof(e->text), text);
    pthread_mutex_unlock(&g_decode_mutex);
}

int http_dbc_recent_json(char *out, size_t out_size)
{
    if (!out || out_size == 0) return -1;

    int total = __atomic_load_n(&g_decode_count, __ATOMIC_RELAXED);
    int count = total > DECODE_RING_MAX ? DECODE_RING_MAX : total;

    int off = snprintf(out, out_size, "[");

    pthread_mutex_lock(&g_decode_mutex);
    for (int i = 0; i < count; i++) {
        int pos = (total - 1 - i) % DECODE_RING_MAX;   /* 最新在前 */
        decode_entry_t *e = &g_decode_ring[pos];
        int n = snprintf(out + off, out_size - (size_t)off,
                         "%s{\"ifname\":\"%s\",\"id\":\"0x%X\",\"name\":\"%s\",\"text\":\"%s\"}",
                         i > 0 ? "," : "", e->ifname, e->can_id, e->name, e->text);
        if (n < 0 || (size_t)n >= out_size - (size_t)off) { off = (int)out_size - 1; break; }
        off += n;
    }
    pthread_mutex_unlock(&g_decode_mutex);

    if (off < (int)out_size - 1)
        off += snprintf(out + off, out_size - (size_t)off, "]");
    return off;
}

void http_can_decoded(app_ctx_t *app, int fd)
{
    (void)app;
    char json[8192];
    int n = http_dbc_recent_json(json, sizeof(json));
    if (n < 0) n = 0;
    http_send_response(fd, 200, "OK", "application/json", json, (size_t)n);
}
