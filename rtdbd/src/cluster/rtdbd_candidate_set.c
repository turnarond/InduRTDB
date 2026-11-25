/**
 * @file rtdbd_candidate_set.c
 * @brief S3 有界去重候选集实现（见 rtdbd_candidate_set.h）。
 */
#include "rtdbd_candidate_set.h"

#include <string.h>

void rtdbd_candidate_set_init(rtdbd_candidate_set_t *s)
{
    if (s) memset(s, 0, sizeof(*s));
}

static bool same_key(const rtdbd_source_candidate_t *a, const rtdbd_source_candidate_t *b)
{
    return a->epoch == b->epoch && a->offset == b->offset &&
           strncmp(a->node_id, b->node_id, sizeof(a->node_id)) == 0;
}

bool rtdbd_candidate_set_add(rtdbd_candidate_set_t *s, const rtdbd_source_candidate_t *c)
{
    if (!s || !c) return false;
    for (uint32_t i = 0; i < s->n; ++i) {
        if (same_key(&s->items[i], c)) {
            s->duplicate_dropped++;
            return false;
        }
    }
    if (s->n >= RTDBD_CANDIDATE_SET_CAP) {
        s->truncated++;
        return false;
    }
    memcpy(&s->items[s->n++], c, sizeof(*c));
    return true;
}

uint32_t rtdbd_candidate_set_snapshot(const rtdbd_candidate_set_t *s,
                                      rtdbd_source_candidate_t *out, uint32_t cap)
{
    if (!s || !out) return 0u;
    uint32_t n = s->n < cap ? s->n : cap;
    memcpy(out, s->items, (size_t)n * sizeof(*out));
    return n;
}
