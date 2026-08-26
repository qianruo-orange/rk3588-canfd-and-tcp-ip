/**
 * http_api_dbc.c — DBC 解析结果的 HTTP 展示模块
 *
 * 按解析方向（接收/发送）各维护一个最近解析结果的环形缓存，
 * 分别供 /api/can/decoded（接收）与 /api/can/decoded/tx（发送）以 JSON 返回。
 * 解析在 can_recv_task（接收方向）与 can_send_task（发送方向）调用
 * dbc_decode_frame() 完成后，由调用方通过 http_dbc_record(dir, ...) 记录；
 * HTTP 层调用 http_dbc_recent_json(dir, ...) 读取。
 */

#include <stdint.h>
#include <string.h>
#include <pthread.h>

#include "http/http_api_dbc.h"
#include "http/http_internal.h"   /* http_send_response */
#include "core/common.h"          /* safe_strncpy */
#include "core/ring.h"            /* ring_t / ring_record / ring_total / ring_latest_pos */
#include "can/dbc_parser.h"       /* DBC_MAX_NAME_LEN */

#define DECODE_RING_MAX 32
#define DECODE_TEXT_MAX 512
#define DBC_DIR_COUNT   2

typedef struct {
    char    ifname[16];
    canid_t can_id;
    char    name[DBC_MAX_NAME_LEN];
    char    text[DECODE_TEXT_MAX];
} decode_entry_t;

static decode_entry_t  g_decode_buf[DBC_DIR_COUNT][DECODE_RING_MAX];
static ring_t          g_decode_ring[DBC_DIR_COUNT] = {
    { .buf = g_decode_buf[0], .entry_size = sizeof(decode_entry_t), .max = DECODE_RING_MAX },
    { .buf = g_decode_buf[1], .entry_size = sizeof(decode_entry_t), .max = DECODE_RING_MAX },
};
static pthread_mutex_t g_decode_mutex = PTHREAD_MUTEX_INITIALIZER;

static int dbc_dir_index(dbc_dir_t dir)
{
    return (dir == DBC_DIR_TX) ? 1 : 0;
}

void http_dbc_record(dbc_dir_t dir, const char *ifname, canid_t can_id,
                     const char *name, const char *text)
{
    if (!ifname || !name || !text) return;
    decode_entry_t e;
    safe_strncpy(e.ifname, sizeof(e.ifname), ifname);
    e.can_id = can_id;
    safe_strncpy(e.name, sizeof(e.name), name);
    safe_strncpy(e.text, sizeof(e.text), text);
    ring_record(&g_decode_ring[dbc_dir_index(dir)], &g_decode_mutex, &e);
}

static int http_dbc_recent_json(dbc_dir_t dir, char *out, size_t out_size)
{
    if (!out || out_size == 0) return -1;

    ring_t *r = &g_decode_ring[dbc_dir_index(dir)];
    int total = ring_total(r);

    int off = snprintf(out, out_size, "[");

    pthread_mutex_lock(&g_decode_mutex);
    for (int i = 0; i < total; i++) {
        int pos = ring_latest_pos(r, i);   /* 最新在前 */
        decode_entry_t *e = &g_decode_buf[dbc_dir_index(dir)][pos];
        int n = http_json_append(out, out_size, off,
                         "%s{\"ifname\":\"%s\",\"id\":\"0x%X\",\"name\":\"%s\",\"text\":\"%s\"}",
                         i > 0 ? "," : "", e->ifname, e->can_id, e->name, e->text);
        if (n < 0) { off = (int)out_size - 1; break; }
        off = n;
    }
    pthread_mutex_unlock(&g_decode_mutex);

    if (off < (int)out_size - 1)
        off = http_json_append(out, out_size, off, "]");
    return off;
}

void http_can_decoded(app_ctx_t *app, int fd)
{
    (void)app;
    char json[8192];
    int n = http_dbc_recent_json(DBC_DIR_RX, json, sizeof(json));
    if (n < 0) n = 0;
    http_ok_json(fd, json, (size_t)n);
}

void http_can_decoded_tx(app_ctx_t *app, int fd)
{
    (void)app;
    char json[8192];
    int n = http_dbc_recent_json(DBC_DIR_TX, json, sizeof(json));
    if (n < 0) n = 0;
    http_ok_json(fd, json, (size_t)n);
}
