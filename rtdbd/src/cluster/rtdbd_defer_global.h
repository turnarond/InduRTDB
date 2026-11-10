/**
 * @file rtdbd_defer_global.h
 * @brief v3.12 M3.3 切片 C：延迟动作队列**进程级单例**（UDS / VSOA 共用）。
 *
 * 与 rtdbd_peers_global 同理：VSOA 回调里入队的动作，由 rtdbd 主循环循环尾
 * 的 drain_deferred_actions() 统一执行（§8.3-3），两前端不各持一份队列。
 */
#ifndef RTDBD_DEFER_GLOBAL_H_
#define RTDBD_DEFER_GLOBAL_H_

#include "session/rtdbd_session.h"

#ifdef __cplusplus
extern "C" {
#endif

rtdbd_defer_queue_t *rtdbd_defer_global(void);

#ifdef __cplusplus
}
#endif
#endif /* RTDBD_DEFER_GLOBAL_H_ */
