/**
 * @file rtdbd_select_source.c
 * @brief S3 逻辑选源实现（见 rtdbd_select_source.h）。
 */
#include "rtdbd_select_source.h"

#include <stdio.h>
#include <string.h>

/* 归一化二段比较：epoch → node_id；boot_id 不参与（内部静态，不导出公共 API）。 */
static int compare_epoch_node(const rtdbd_source_candidate_t *a,
                              const rtdbd_source_candidate_t *b)
{
    if (a->epoch < b->epoch) return -1;
    if (a->epoch > b->epoch) return 1;
    int c = strncmp(a->node_id, b->node_id, sizeof(a->node_id));
    if (c != 0) return c < 0 ? -1 : 1;
    return 0;
}

static void digest_hex(uint64_t sh, uint64_t vd, uint32_t n, char *out, size_t cap)
{
    (void)snprintf(out, cap, "fnv1a64-v1:%016llx:%016llx:%u",
                   (unsigned long long)sh, (unsigned long long)vd, (unsigned)n);
}

bool rtdbd_select_source(const rtdbd_source_candidate_t *cands, uint32_t n,
                         rtdbd_source_result_t *out, const char *local_digest_hex)
{
    if (!cands || n == 0u || !out) return false;
    memset(out, 0, sizeof(*out));
    const rtdbd_source_candidate_t *win = NULL;
    for (uint32_t i = 0; i < n; ++i) {
        if (!cands[i].valid) continue;
        out->candidate_count++;
        if (!win || compare_epoch_node(&cands[i], win) > 0) win = &cands[i];
    }
    if (!win) return false; /* 全部 invalid：拒绝裁决（不制造伪胜者） */
    out->valid_candidate_count = out->candidate_count;
    out->winner_epoch = win->epoch;
    size_t nlen = strlen(win->node_id);
    if (nlen >= sizeof(out->winner_node_id)) nlen = sizeof(out->winner_node_id) - 1u;
    memcpy(out->winner_node_id, win->node_id, nlen);
    out->winner_node_id[nlen] = '\0';
    digest_hex(win->struct_hash, win->value_digest, win->n_points,
               out->winner_digest_hex, sizeof(out->winner_digest_hex));
    if (local_digest_hex) {
        size_t dlen = strlen(local_digest_hex);
        if (dlen >= sizeof(out->selected_source_digest_hex)) dlen = sizeof(out->selected_source_digest_hex) - 1u;
        memcpy(out->selected_source_digest_hex, local_digest_hex, dlen);
        out->selected_source_digest_hex[dlen] = '\0';
    } else {
        memcpy(out->selected_source_digest_hex, out->winner_digest_hex,
               sizeof(out->selected_source_digest_hex));
    }
    return true;
}

bool rtdbd_select_source_json(const rtdbd_source_result_t *res, char *buf, size_t cap)
{
    if (!res || !buf || cap < 256u) return false;
    int k = snprintf(buf, cap,
        "{\"winner_epoch\":%llu,\"winner_node_id\":\"%s\","
        "\"winner_digest_hex\":\"%s\",\"selected_source_digest_hex\":\"%s\","
        "\"valid_candidate_count\":%u,\"candidate_count\":%u,"
        "\"duplicate_dropped\":%u,\"truncated\":%u,"
        "\"evidence_kind\":\"logic-only\"}",
        (unsigned long long)res->winner_epoch, res->winner_node_id,
        res->winner_digest_hex, res->selected_source_digest_hex,
        res->valid_candidate_count, res->candidate_count,
        res->duplicate_dropped, res->truncated);
    return k > 0 && (size_t)k < cap;
}
