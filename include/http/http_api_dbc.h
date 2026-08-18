#ifndef HTTP_API_DBC_H
#define HTTP_API_DBC_H

#include <stddef.h>
#include <linux/can.h>

/* DBC 解析方向：接收数据解析 / 发送数据解析 */
typedef enum {
    DBC_DIR_RX = 0,   /* 接收方向解析 */
    DBC_DIR_TX = 1    /* 发送方向解析 */
} dbc_dir_t;

/* 记录一条 DBC 解析结果到指定方向的环形缓存（最新在前） */
void http_dbc_record(dbc_dir_t dir, const char *ifname, canid_t can_id,
                     const char *name, const char *text);

/* 将指定方向的最近解析结果序列化为 JSON 数组（最新在前）；返回写入字节数 */
int http_dbc_recent_json(dbc_dir_t dir, char *out, size_t out_size);

#endif /* HTTP_API_DBC_H */
