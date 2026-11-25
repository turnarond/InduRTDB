/**
 * @file s5_capture.c
 * @brief S5 取证 JSON 纯函数实现（见 s5_capture.h）。
 */
#include "s5_capture.h"

#include <stdio.h>
#include <string.h>

void s5_capture_init(s5_capture_t *c)
{
    if (!c) return;
    memset(c, 0, sizeof(*c));
}

static s5_field_t *find_or_add(s5_capture_t *c, const char *key)
{
    if (!c || !key || key[0] == '\0') return NULL;
    for (uint32_t i = 0; i < c->n_fields; ++i) {
        if (strcmp(c->fields[i].key, key) == 0) return &c->fields[i];
    }
    if (c->n_fields >= S5_CAPTURE_MAX_FIELDS) return NULL;
    s5_field_t *f = &c->fields[c->n_fields++];
    memset(f, 0, sizeof(*f));
    size_t n = strlen(key);
    if (n >= sizeof(f->key)) n = sizeof(f->key) - 1u;
    memcpy(f->key, key, n);
    f->key[n] = '\0';
    return f;
}

static bool set_raw(s5_capture_t *c, const char *key, const char *value,
                    s5_source_t src, const char *reason)
{
    s5_field_t *f = find_or_add(c, key);
    if (!f) return false;
    size_t vn = strlen(value);
    if (vn >= sizeof(f->value)) vn = sizeof(f->value) - 1u;
    memcpy(f->value, value, vn);
    f->value[vn] = '\0';
    f->source = src;
    if (reason) {
        size_t rn = strlen(reason);
        if (rn >= sizeof(f->reason)) rn = sizeof(f->reason) - 1u;
        memcpy(f->reason, reason, rn);
        f->reason[rn] = '\0';
    } else {
        f->reason[0] = '\0';
    }
    return true;
}

bool s5_field_set_u64(s5_capture_t *c, const char *key, uint64_t v, s5_source_t src)
{
    char buf[32];
    (void)snprintf(buf, sizeof(buf), "%llu", (unsigned long long)v);
    return set_raw(c, key, buf, src, NULL);
}

bool s5_field_set_on_site(s5_capture_t *c, const char *key, const char *json_value)
{
    return set_raw(c, key, json_value, S5_SRC_ON_SITE, NULL);
}

bool s5_field_set_logic_only(s5_capture_t *c, const char *key,
                             const char *json_value, const char *reason)
{
    return set_raw(c, key, json_value, S5_SRC_LOGIC_ONLY, reason);
}

bool s5_field_set_unavailable(s5_capture_t *c, const char *key, const char *reason)
{
    return set_raw(c, key, "null", S5_SRC_UNAVAILABLE, reason);
}

s5_source_t s5_field_source(const s5_capture_t *c, const char *key)
{
    if (!c || !key) return S5_SRC_NONE;
    for (uint32_t i = 0; i < c->n_fields; ++i) {
        if (strcmp(c->fields[i].key, key) == 0) return c->fields[i].source;
    }
    return S5_SRC_NONE;
}

static const char *source_name(s5_source_t s)
{
    switch (s) {
    case S5_SRC_ON_SITE:      return "on-site";
    case S5_SRC_LOGIC_ONLY:   return "logic-only";
    case S5_SRC_UNAVAILABLE:  return "unavailable";
    default:                  return "none";
    }
}

bool s5_capture_emit_json(const s5_capture_t *c, char *buf, size_t cap)
{
    if (!c || !buf || cap < 256u) return false;
    int n = snprintf(buf, cap, "{\"mode\":\"%s\",\"node_id\":\"%s\",\"fields\":{",
                     c->mode, c->node_id);
    if (n <= 0 || (size_t)n >= cap) return false;
    size_t used = (size_t)n;
    for (uint32_t i = 0; i < c->n_fields; ++i) {
        const s5_field_t *f = &c->fields[i];
        int k = snprintf(buf + used, cap - used,
                         "%s\"%s\":{\"value\":%s,\"_source\":\"%s\",\"reason\":\"%s\"}",
                         i ? "," : "", f->key, f->value, source_name(f->source), f->reason);
        if (k <= 0 || (size_t)k >= cap - used) return false;
        used += (size_t)k;
    }
    int tail = snprintf(buf + used, cap - used, "}}");
    return tail > 0 && (size_t)tail < cap - used;
}

/* 产品语义不足的字段禁止 on-site/logic-only 伪装现场真值。 */
static bool product_unavailable_field(const char *key)
{
    static const char *const forbidden[] = {
        "nodes_converged", "terminal_hash", "dict_max_snapshot_match",
        "machine_restart_pull_full"
    };
    for (size_t i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); ++i) {
        if (strcmp(key, forbidden[i]) == 0) return true;
    }
    return false;
}

bool s5_capture_validate(const s5_capture_t *c)
{
    if (!c || c->n_fields > S5_CAPTURE_MAX_FIELDS) return false;
    if (c->mode[0] == '\0') return false;
    for (uint32_t i = 0; i < c->n_fields; ++i) {
        const s5_field_t *f = &c->fields[i];
        if (f->key[0] == '\0' || f->value[0] == '\0') return false;
        if (f->source == S5_SRC_NONE) return false;
        if (f->source == S5_SRC_UNAVAILABLE && f->reason[0] == '\0') return false;
        if (product_unavailable_field(f->key) && f->source != S5_SRC_UNAVAILABLE) return false;
    }
    return true;
}
