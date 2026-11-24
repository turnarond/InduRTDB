/**
 * @file rtdbd_agent_rbe.c
 * @brief h-C 设备侧代理 RBE 适配纯逻辑（无 SDK 依赖，OFF 单测可直测）。
 * 从 rtdbd_device_agent.c 拆出，使 OFF 构建无需 VSOA SDK 即可测 RBE 三例。
 */
#include "rtdbd_device_agent.h"

#include <stdlib.h>
#include <string.h>

void agent_rbe_init(agent_rbe_state_t *s)
{
    if (!s) return;
    memset(s, 0, sizeof(*s));
}

static agent_point_t *rbe_find_point(agent_rbe_state_t *s, uint32_t pid)
{
    if (!s) return NULL;
    for (uint32_t i = 0; i < s->n_points; ++i) {
        if (s->points[i].point_id == pid) return &s->points[i];
    }
    return NULL;
}

static agent_point_t *rbe_ensure_point(agent_rbe_state_t *s, uint32_t pid)
{
    agent_point_t *p = rbe_find_point(s, pid);
    if (p) return p;
    if (s->n_points >= s->cap_points) {
        uint32_t newcap = s->cap_points ? s->cap_points * 2u : 16u;
        if (newcap > RTDBD_AGENT_FIRST_CACHE_MAX) newcap = RTDBD_AGENT_FIRST_CACHE_MAX;
        if (s->n_points >= newcap) { s->evicted++; return NULL; } /* 缓存满，计淘汰 */
        agent_point_t *np = (agent_point_t *)realloc(s->points, (size_t)newcap * sizeof(*np));
        if (!np) return NULL;
        s->points = np;
        s->cap_points = newcap;
    }
    p = &s->points[s->n_points++];
    memset(p, 0, sizeof(*p));
    p->point_id = pid;
    return p;
}

bool agent_rbe_subscribe(agent_rbe_state_t *s, uint32_t listener, uint32_t point_id)
{
    if (!s || listener >= RTDBD_AGENT_LISTENER_MAX || point_id >= RTDBD_AGENT_FIRST_CACHE_MAX) {
        return false;
    }
    uint32_t *bmp = &s->listener_bmp[listener][point_id / 32u];
    uint32_t bit = 1u << (point_id % 32u);
    if (*bmp & bit) return false;       /* 该 listener 已订阅，非首个 */
    *bmp |= bit;
    agent_point_t *p = rbe_ensure_point(s, point_id);
    if (!p) return false;
    p->refcnt++;
    return p->refcnt == 1u;             /* 首个 listener → 需向上游订阅 */
}

bool agent_rbe_unsubscribe(agent_rbe_state_t *s, uint32_t listener, uint32_t point_id)
{
    if (!s || listener >= RTDBD_AGENT_LISTENER_MAX || point_id >= RTDBD_AGENT_FIRST_CACHE_MAX) {
        return false;
    }
    uint32_t *bmp = &s->listener_bmp[listener][point_id / 32u];
    uint32_t bit = 1u << (point_id % 32u);
    if (!(*bmp & bit)) return false;
    *bmp &= ~bit;
    agent_point_t *p = rbe_find_point(s, point_id);
    if (!p || p->refcnt == 0u) return false;
    p->refcnt--;
    return p->refcnt == 0u;             /* 最后 listener → 需向上游退订 */
}

bool agent_rbe_listener_has(const agent_rbe_state_t *s, uint32_t listener, uint32_t point_id)
{
    if (!s || listener >= RTDBD_AGENT_LISTENER_MAX || point_id >= RTDBD_AGENT_FIRST_CACHE_MAX) {
        return false;
    }
    return (s->listener_bmp[listener][point_id / 32u] & (1u << (point_id % 32u))) != 0u;
}

void agent_rbe_cache(agent_rbe_state_t *s, uint32_t point_id, const uint8_t notify[32])
{
    if (!s || !notify) return;
    agent_point_t *p = rbe_ensure_point(s, point_id);
    if (!p) { s->evicted++; return; }
    memcpy(p->cached, notify, sizeof(p->cached));
    p->has_cached = true;
}

bool agent_rbe_get_cached(const agent_rbe_state_t *s, uint32_t point_id, uint8_t out[32])
{
    if (!s || !out) return false;
    agent_point_t *p = rbe_find_point((agent_rbe_state_t *)s, point_id);
    if (!p || !p->has_cached) return false;
    memcpy(out, p->cached, sizeof(p->cached));
    return true;
}
