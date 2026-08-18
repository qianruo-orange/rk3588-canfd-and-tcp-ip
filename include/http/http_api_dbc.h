#ifndef HTTP_API_DBC_H
#define HTTP_API_DBC_H

#include <stddef.h>
#include <linux/can.h>

/* 记录一条 DBC 解码结果到环形缓存（供 /api/can/decoded 展示，最新在前） */
void http_dbc_record(const char *ifname, canid_t can_id,
                     const char *name, const char *text);

/* 将最近解码结果序列化为 JSON 数组（最新在前）；返回写入字节数 */
int http_dbc_recent_json(char *out, size_t out_size);

#endif /* HTTP_API_DBC_H */
