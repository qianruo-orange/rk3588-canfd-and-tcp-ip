#ifndef DBC_PARSER_H
#define DBC_PARSER_H

#include <stddef.h>
#include <stdint.h>
#include <linux/can.h>

struct app_ctx;   /* 前置声明，供 dbc_decode_frame 引用 */

/* 编译期上限（静态数组，禁止动态内存）：可按实际 DBC 规模调整 */
#define DBC_MAX_MESSAGES 256
#define DBC_MAX_SIGNALS  1024
#define DBC_MAX_NAME_LEN 64

/* 解码整帧文本最大长度（name=value 形式，供解码结果缓存/展示使用） */
#define DBC_DECODE_TEXT_MAX 512

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

/* 从文件加载 DBC；成功返回 0 */
int dbc_load(dbc_t *dbc, const char *path);

/* 收到一帧 CAN 数据后按通道 DBC 解码整帧并记录日志；解码成功返回 0，否则 -1。
   通过 out 参数返回 can_id(不含标志位)、报文名与解码文本。 */
int dbc_decode_frame(struct app_ctx *app, int can_idx, const char *ifname,
                     const struct canfd_frame *frame,
                     canid_t *id_out, char *name_out, size_t name_size,
                     char *text_out, size_t text_size);

#endif /* DBC_PARSER_H */
