/**
 * @file irt_subscription.c
 * @brief 订阅管理器实现
 * @version 3.0.0
 * @date 2026-07-15
 * @copyright MIT License
 */

#include "core/irt_subscription.h"
#include "osal/irt_osal.h"
#include <string.h>

void irt_sub_init(irt_sub_t* sub, irt_shm_t* shm) {
    if (!sub || !shm) return;
    memset(sub, 0, sizeof(*sub));
    sub->shm_table       = irt_shm_subscribers(shm);
    sub->max_subscribers = shm->max_subscribers;
}

int irt_sub_subscribe(irt_sub_t* sub, uint32_t id,
                      indurtdb_callback_t cb, void* user_data) {
    if (!sub || !cb) return -1;
    if (sub->slot_count >= IRT_SUB_MAX_CALLBACKS) return -1;

    irt_sub_slot_t* slot = &sub->slots[sub->slot_count];
    slot->point_id          = id;
    slot->callback          = cb;
    slot->user_data         = user_data;
    slot->last_heartbeat_ns = irt_time_now_ns();
    slot->active            = true;
    sub->slot_count++;
    return 0;
}

int irt_sub_unsubscribe(irt_sub_t* sub, uint32_t id) {
    if (!sub) return -1;
    uint32_t removed = 0;
    for (uint32_t i = 0; i < sub->slot_count; ) {
        if (sub->slots[i].active && sub->slots[i].point_id == id) {
            /* 与末尾元素交换后缩减, 无需移动所有后续元素 */
            sub->slots[i] = sub->slots[sub->slot_count - 1];
            memset(&sub->slots[sub->slot_count - 1], 0,
                   sizeof(irt_sub_slot_t));
            sub->slot_count--;
            removed++;
        } else {
            i++;
        }
    }
    return removed > 0 ? 0 : -1;
}

void irt_sub_notify(irt_sub_t* sub, uint32_t id,
                    const indurtdb_point_t* data) {
    if (!sub || !data) return;
    for (uint32_t i = 0; i < sub->slot_count; i++) {
        irt_sub_slot_t* slot = &sub->slots[i];
        if (slot->active && slot->point_id == id && slot->callback) {
            slot->callback(id, data, slot->user_data);
        }
    }
}

void irt_sub_update_heartbeat(irt_sub_t* sub, int32_t pid) {
    if (!sub || !sub->shm_table) return;
    for (uint32_t i = 0; i < sub->max_subscribers; i++) {
        if (sub->shm_table[i].pid == pid) {
            sub->shm_table[i].last_heartbeat_ns = irt_time_now_ns();
            return;
        }
    }
    /* pid 未找到: 尝试占用第一个空槽位 */
    for (uint32_t i = 0; i < sub->max_subscribers; i++) {
        if (sub->shm_table[i].pid == 0) {
            sub->shm_table[i].pid = pid;
            sub->shm_table[i].last_heartbeat_ns = irt_time_now_ns();
            return;
        }
    }
}

void irt_sub_cleanup_zombies(irt_sub_t* sub, uint64_t timeout_ns) {
    if (!sub || !sub->shm_table || timeout_ns == 0) return;
    uint64_t now = irt_time_now_ns();
    for (uint32_t i = 0; i < sub->max_subscribers; i++) {
        if (sub->shm_table[i].pid != 0
            && now - sub->shm_table[i].last_heartbeat_ns > timeout_ns) {
            memset(&sub->shm_table[i], 0, sizeof(irt_subscriber_entry_t));
        }
    }
}

uint32_t irt_sub_count(const irt_sub_t* sub) {
    if (!sub) return 0;
    uint32_t cnt = 0;
    for (uint32_t i = 0; i < sub->slot_count; i++) {
        if (sub->slots[i].active) cnt++;
    }
    return cnt;
}
