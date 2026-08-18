#ifndef HTTP_API_CAN_H
#define HTTP_API_CAN_H

#include <linux/can.h>

/* CAN 接收路径记录一帧原始报文到环形缓存（供 GET /api/can/frames 展示）。
 * 由 can_task 在收到帧后调用；ifname 为来源接口名，frame 为原始 CAN(FD) 帧。 */
void http_can_record_rx(const char *ifname, const struct canfd_frame *frame);

#endif /* HTTP_API_CAN_H */
