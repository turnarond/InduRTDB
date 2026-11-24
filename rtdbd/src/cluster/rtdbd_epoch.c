/**
 * @file rtdbd_epoch.c
 * @brief S1：进程级全局 epoch 实现（见 rtdbd_epoch.h）。
 */
#include "rtdbd_epoch.h"

#include <string.h>

static uint64_t g_current_epoch = 0u;

uint64_t rtdbd_epoch_next(void)
{
    g_current_epoch++;
    if (g_current_epoch == 0u) g_current_epoch = 1u; /* 跳过回绕 0（非零约束） */
    return g_current_epoch;
}

uint64_t rtdbd_epoch_current(void)
{
    return g_current_epoch;
}

bool rtdbd_epoch_is_stale(uint64_t frame_epoch)
{
    /* 13 号 §8.3：epoch < current 为陈旧帧拒绝；未来帧（> current）由 S2
     * backlog 暂存/重放处理，不在本原语层判 stale。 */
    return frame_epoch < g_current_epoch;
}

void rtdbd_epoch_advance_to(uint64_t epoch)
{
    if (epoch > g_current_epoch) g_current_epoch = epoch;
}

rtdbd_epoch_key_status_t rtdbd_epoch_key(uint64_t epoch, uint64_t boot_id,
                                          const char *node_id, rtdbd_epoch_key_t *out)
{
    if (!node_id || !out) return RTDBD_EPOCH_KEY_ARG;
    memset(out, 0, sizeof(*out));
    out->epoch = epoch;
    out->boot_id = boot_id;
    size_t n = strlen(node_id);
    if (n >= sizeof(out->node_id)) n = sizeof(out->node_id) - 1u;
    memcpy(out->node_id, node_id, n);
    return RTDBD_EPOCH_KEY_OK;
}

int rtdbd_epoch_key_compare(const rtdbd_epoch_key_t *a, const rtdbd_epoch_key_t *b)
{
    if (!a || !b) return 0;
    if (a->epoch < b->epoch) return -1;
    if (a->epoch > b->epoch) return 1;
    int c = strncmp(a->node_id, b->node_id, sizeof(a->node_id));
    if (c != 0) return c < 0 ? -1 : 1;
    if (a->boot_id < b->boot_id) return -1;
    if (a->boot_id > b->boot_id) return 1;
    return 0;
}

void rtdbd_epoch_reset_for_test(void)
{
    g_current_epoch = 0u;
}
