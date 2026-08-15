#ifndef DBC_PARSER_H
#define DBC_PARSER_H

#include <stddef.h>
#include <stdint.h>
#include <linux/can.h>

/* 编译期上限（静态数组，禁止动态内存）：可按实际 DBC 规模调整 */
#define DBC_MAX_MESSAGES 256
#define DBC_MAX_SIGNALS  1024
#define DBC_MAX_NAME_LEN 64

/* 字节序：DBC 中 @0=Motorola(大端/MSB first)，@1=Intel(小端/LSB first) */
typedef enum {
    DBC_BYTE_ORDER_INTEL = 0,
    DBC_BYTE_ORDER_MOTOROLA = 1
} dbc_byte_order_t;

/* 数值类型：+ 无符号，- 有符号 */
typedef enum {
    DBC_VALUE_UNSIGNED = 0,
    DBC_VALUE_SIGNED = 1
} dbc_value_type_t;

/* 单条信号定义 */
typedef struct {
    char              name[DBC_MAX_NAME_LEN];
    int               start_bit;   /* DBC 起始位 */
    int               length;      /* 位宽（1~64） */
    dbc_byte_order_t  byte_order;
    dbc_value_type_t  value_type;
    double            factor;      /* 物理值 = raw * factor + offset */
    double            offset;
    double            min;
    double            max;
    char              unit[DBC_MAX_NAME_LEN];
    int               msg_idx;     /* 所属消息索引 */
} dbc_signal_t;

/* 单条报文定义 */
typedef struct {
    canid_t  can_id;    /* 已含 CAN_EFF_FLAG */
    char     name[DBC_MAX_NAME_LEN];
    int      dlc;
    int      sig_start; /* 信号在 signals[] 中的起始索引 */
    int      sig_count; /* 信号数量 */
} dbc_message_t;

/* DBC 数据库（静态分配，调用方宜用 static 或全局，勿在栈上大量创建） */
typedef struct {
    dbc_message_t messages[DBC_MAX_MESSAGES];
    int           msg_count;
    dbc_signal_t  signals[DBC_MAX_SIGNALS];
    int           sig_count;
} dbc_t;

/* 解析 DBC 文本内容（逐行解析 BO_/SG_，忽略其余行）；成功返回 0 */
int dbc_parse(dbc_t *dbc, const char *content, size_t len);

/* 从文件加载 DBC；成功返回 0 */
int dbc_load(dbc_t *dbc, const char *path);

/* 按 CAN ID 查找消息索引；找不到返回 -1 */
int dbc_find_message(const dbc_t *dbc, canid_t can_id);

/* 按名字查找消息索引；找不到返回 -1 */
int dbc_find_message_by_name(const dbc_t *dbc, const char *name);

/* 在指定消息内按名字查找信号索引（全局 signals[] 下标）；找不到返回 -1 */
int dbc_find_signal(const dbc_t *dbc, int msg_idx, const char *name);

/* 从帧中解码信号原始值（未缩放，含符号扩展）；成功返回 0 */
int dbc_decode_raw(const dbc_t *dbc, int sig_idx,
                   const struct canfd_frame *frame, uint64_t *raw);

/* 从帧中解码信号物理值（raw * factor + offset）；成功返回 0 */
int dbc_decode_physical(const dbc_t *dbc, int sig_idx,
                        const struct canfd_frame *frame, double *value);

/* 解码整帧消息为 "name=value name=value ..." 文本；返回写入字节数 */
int dbc_decode_message(const dbc_t *dbc, int msg_idx,
                       const struct canfd_frame *frame, char *out, size_t out_size);

#endif /* DBC_PARSER_H */
