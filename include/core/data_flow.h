#ifndef DATA_FLOW_H
#define DATA_FLOW_H

#include <stddef.h>
#include "core/common.h"

/* 单帧文本最大长度：ifname(16) + id(8) + dlc(2) + 64×3 + 分隔符，留足余量 */
#define FLOW_FRAME_TEXT_MAX 512

/* 注册数据流虚函数实现；ops 为 NULL 时恢复默认实现 */
void data_flow_register(app_ctx_t *app, const data_flow_ops_t *ops);

/* 获取默认实现（各域数据流独立、不做任何桥接；业务可整体替换或逐项覆盖） */
const data_flow_ops_t *data_flow_default_ops(void);

/* 帧文本编解码工具
 * 帧格式：<ifname> <can_id(hex)> <dlc> <b0(hex)> <b1(hex)> ... */
int data_flow_encode_frame(const char *ifname, const struct canfd_frame *frame,
                           char *out, size_t out_size);
int data_flow_decode_frame(const char *text, char *ifname, size_t ifname_size,
                           struct canfd_frame *frame);

/* 获取最近 DBC 解码结果（JSON 数组，最新在前）；返回写入字节数 */
int data_flow_recent_decoded_json(char *out, size_t out_size);

#endif /* DATA_FLOW_H */
