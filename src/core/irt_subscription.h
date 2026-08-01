/**
 * @file irt_subscription.h
 * @brief 订阅管理器 (直译自 v2.x SubscriptionManager)
 * @version 3.0.0
 * @date 2026-07-15
 * @copyright MIT License
 */

#ifndef IRT_CORE_IRT_SUBSCRIPTION_H_
#define IRT_CORE_IRT_SUBSCRIPTION_H_

#include "core/irt_shm.h"

#define IRT_SUB_MAX_CALLBACKS  256

/* 本地订阅槽位 (进程私有) */
typedef struct {
    uint32_t            point_id;
    indurtdb_callback_t callback;
    void*               user_data;
    bool                active;
} irt_sub_slot_t;

typedef struct {
    irt_sub_slot_t        slots[IRT_SUB_MAX_CALLBACKS];
    uint32_t              slot_count;
    irt_subscriber_entry_t* shm_table;
    uint32_t              max_subscribers;
} irt_sub_t;

/* 绑定到已初始化的共享内存段 */
void irt_sub_init(irt_sub_t* sub, irt_shm_t* shm);

/* 订阅/取消订阅 (订阅: 成功 0, 槽位满/null cb -1; 取消: 成功 0, null sub -1, 未找到 -2) */
int  irt_sub_subscribe(irt_sub_t* sub, uint32_t id,
                       indurtdb_callback_t cb, void* user_data);
int  irt_sub_unsubscribe(irt_sub_t* sub, uint32_t id);

/* 通知所有订阅了 id 的回调 */
void irt_sub_notify(irt_sub_t* sub, uint32_t id, const indurtdb_point_t* data);

/* 更新心跳 (在共享内存表中查找 pid 并记录时间戳) */
void irt_sub_update_heartbeat(irt_sub_t* sub, int32_t pid);

/* 清理僵尸进程 (心跳超时 > timeout_ns 的条目; timeout_ns=0 则跳过) */
void irt_sub_cleanup_zombies(irt_sub_t* sub, uint64_t timeout_ns);

/* 活跃订阅数 */
uint32_t irt_sub_count(const irt_sub_t* sub);

#endif /* IRT_CORE_IRT_SUBSCRIPTION_H_ */
