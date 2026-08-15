/**
 * dbc_parser.c — DBC（CAN 数据库）解析与信号解码
 *
 * 仅解析 BO_（报文）与 SG_（信号）两种定义行，其余行（VERSION/NS_/BS_/
 * BU_/VAL_/CM_/BA_ 等）忽略。解析结果存入静态上限数组，全程无动态内存。
 *
 * 信号位提取支持两种字节序：
 *   - Intel(1)：LSB first，bit n = byte(n/8).bit(n%8)；
 *   - Motorola(0)：MSB first，bit n = byte(n/8).bit(7 - n%8)，跨字节蛇形。
 */

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "can/dbc_parser.h"
#include "core/common.h"   /* safe_strncpy */

/* ---- 位提取 ---- */

static uint64_t extract_intel(const uint8_t *data, int start, int len)
{
    uint64_t v = 0;
    for (int i = 0; i < len; i++) {
        int pos = start + i;
        if (data[pos / 8] & (1u << (pos % 8)))
            v |= (1ULL << i);
    }
    return v;
}

static uint64_t extract_motorola(const uint8_t *data, int start, int len)
{
    uint64_t v = 0;
    for (int i = 0; i < len; i++) {
        int pos = start - i;           /* 自 MSB 向下 */
        if (data[pos / 8] & (1u << (7 - (pos % 8))))
            v |= (1ULL << (len - 1 - i));
    }
    return v;
}

/* ---- DBC 行解析 ---- */

static int parse_message(dbc_t *dbc, const char *line)
{
    if (dbc->msg_count >= DBC_MAX_MESSAGES) return -1;

    char name[DBC_MAX_NAME_LEN];
    int id, dlc;
    /* BO_ <id(十进制)> <name>: <dlc> <transmitter> */
    if (sscanf(line, "BO_ %d %63[^:] : %d", &id, name, &dlc) != 3)
        return -1;

    dbc_message_t *msg = &dbc->messages[dbc->msg_count];
    memset(msg, 0, sizeof(*msg));
    msg->can_id = (canid_t)id;
    if (id > CAN_SFF_MASK) msg->can_id |= CAN_EFF_FLAG;
    safe_strncpy(msg->name, sizeof(msg->name), name);
    msg->dlc = dlc;
    msg->sig_start = dbc->sig_count;
    msg->sig_count = 0;

    dbc->msg_count++;
    return 0;
}

static int parse_signal(dbc_t *dbc, const char *line)
{
    if (dbc->msg_count == 0) return -1;   /* SG_ 之前必须有 BO_ */
    if (dbc->sig_count >= DBC_MAX_SIGNALS) return -1;

    char name[DBC_MAX_NAME_LEN];
    int start, len;
    char order, type;
    double factor, offset;
    int consumed = 0;

    /* 必选部分：SG_ <name> : <start>|<len>@<order><type> (<factor>,<offset>) */
    int n = sscanf(line, "SG_ %63s : %d|%d@%c%c (%lf,%lf)%n",
                   name, &start, &len, &order, &type, &factor, &offset, &consumed);
    if (n < 7) return -1;

    /* 可选部分：[<min>|<max>] "<unit>" */
    double min = 0.0, max = 0.0;
    char unit[DBC_MAX_NAME_LEN] = "";
    sscanf(line + consumed, " [%lf|%lf] \"%63[^\"]\"", &min, &max, unit);

    dbc_message_t *msg = &dbc->messages[dbc->msg_count - 1];
    dbc_signal_t *sig = &dbc->signals[dbc->sig_count];
    memset(sig, 0, sizeof(*sig));

    safe_strncpy(sig->name, sizeof(sig->name), name);
    sig->start_bit = start;
    sig->length = len;
    sig->byte_order = (order == '0') ? DBC_BYTE_ORDER_MOTOROLA : DBC_BYTE_ORDER_INTEL;
    sig->value_type = (type == '-') ? DBC_VALUE_SIGNED : DBC_VALUE_UNSIGNED;
    sig->factor = factor;
    sig->offset = offset;
    sig->min = min;
    sig->max = max;
    safe_strncpy(sig->unit, sizeof(sig->unit), unit);
    sig->msg_idx = dbc->msg_count - 1;

    msg->sig_count++;
    dbc->sig_count++;
    return 0;
}

static void parse_line(dbc_t *dbc, const char *line)
{
    while (*line && isspace((unsigned char)*line)) line++;
    if (strncmp(line, "BO_ ", 4) == 0) {
        parse_message(dbc, line);
    } else if (strncmp(line, "SG_ ", 4) == 0) {
        parse_signal(dbc, line);
    }
    /* 其余行忽略 */
}

/* ---- 对外 API ---- */

int dbc_parse(dbc_t *dbc, const char *content, size_t len)
{
    if (!dbc || (!content && len != 0)) return -1;
    memset(dbc, 0, sizeof(*dbc));

    const char *p = content;
    const char *end = content + len;
    char line[1024];

    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t line_len = nl ? (size_t)(nl - p) : (size_t)(end - p);
        if (line_len >= sizeof(line)) line_len = sizeof(line) - 1;
        memcpy(line, p, line_len);
        line[line_len] = '\0';
        parse_line(dbc, line);
        p += line_len + (nl ? 1 : 0);
    }
    return 0;
}

int dbc_load(dbc_t *dbc, const char *path)
{
    if (!dbc || !path) return -1;
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    memset(dbc, 0, sizeof(*dbc));
    char line[1024];
    while (fgets(line, sizeof(line), f))
        parse_line(dbc, line);
    fclose(f);
    return 0;
}

int dbc_find_message(const dbc_t *dbc, canid_t can_id)
{
    if (!dbc) return -1;
    canid_t id = can_id & CAN_EFF_MASK;
    for (int i = 0; i < dbc->msg_count; i++)
        if ((dbc->messages[i].can_id & CAN_EFF_MASK) == id) return i;
    return -1;
}

int dbc_find_message_by_name(const dbc_t *dbc, const char *name)
{
    if (!dbc || !name) return -1;
    for (int i = 0; i < dbc->msg_count; i++)
        if (strcmp(dbc->messages[i].name, name) == 0) return i;
    return -1;
}

int dbc_find_signal(const dbc_t *dbc, int msg_idx, const char *name)
{
    if (!dbc || !name || msg_idx < 0 || msg_idx >= dbc->msg_count) return -1;
    const dbc_message_t *msg = &dbc->messages[msg_idx];
    for (int i = 0; i < msg->sig_count; i++) {
        int idx = msg->sig_start + i;
        if (strcmp(dbc->signals[idx].name, name) == 0) return idx;
    }
    return -1;
}

int dbc_decode_raw(const dbc_t *dbc, int sig_idx,
                   const struct canfd_frame *frame, uint64_t *raw)
{
    if (!dbc || !frame || !raw) return -1;
    if (sig_idx < 0 || sig_idx >= dbc->sig_count) return -1;

    const dbc_signal_t *sig = &dbc->signals[sig_idx];
    if (sig->length <= 0 || sig->length > 64) return -1;

    /* 校验信号覆盖的字节是否在帧数据范围内 */
    int max_byte;
    if (sig->byte_order == DBC_BYTE_ORDER_MOTOROLA) {
        if (sig->start_bit - sig->length + 1 < 0) return -1;
        max_byte = sig->start_bit / 8;
    } else {
        max_byte = (sig->start_bit + sig->length - 1) / 8;
    }
    if (max_byte >= (int)frame->len) return -1;

    uint64_t value = (sig->byte_order == DBC_BYTE_ORDER_MOTOROLA)
        ? extract_motorola(frame->data, sig->start_bit, sig->length)
        : extract_intel(frame->data, sig->start_bit, sig->length);

    /* 符号扩展 */
    if (sig->value_type == DBC_VALUE_SIGNED && sig->length < 64) {
        if (value & (1ULL << (sig->length - 1)))
            value |= ~((1ULL << sig->length) - 1);
    }
    *raw = value;
    return 0;
}

int dbc_decode_physical(const dbc_t *dbc, int sig_idx,
                        const struct canfd_frame *frame, double *value)
{
    uint64_t raw;
    if (dbc_decode_raw(dbc, sig_idx, frame, &raw) < 0) return -1;
    const dbc_signal_t *sig = &dbc->signals[sig_idx];
    *value = (double)(int64_t)raw * sig->factor + sig->offset;
    return 0;
}

int dbc_decode_message(const dbc_t *dbc, int msg_idx,
                       const struct canfd_frame *frame, char *out, size_t out_size)
{
    if (!dbc || !frame || !out || out_size == 0) return -1;
    if (msg_idx < 0 || msg_idx >= dbc->msg_count) return -1;

    const dbc_message_t *msg = &dbc->messages[msg_idx];
    int off = 0;
    for (int i = 0; i < msg->sig_count; i++) {
        int sig_idx = msg->sig_start + i;
        double v;
        if (dbc_decode_physical(dbc, sig_idx, frame, &v) < 0) continue;
        const dbc_signal_t *sig = &dbc->signals[sig_idx];
        int n = snprintf(out + off, out_size - (size_t)off,
                         "%s%s=%g", off ? " " : "", sig->name, v);
        if (n < 0 || (size_t)n >= out_size - (size_t)off) break;
        off += n;
    }
    return off;
}
