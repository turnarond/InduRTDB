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
    size_t vn = strlen(value);
    if (vn >= S5_CAPTURE_VALUE_LEN) return false; /* S5b：超长报错，不静默截断成假值 */
    if (reason && strlen(reason) >= S5_CAPTURE_REASON_LEN) return false;
    s5_field_t *f = find_or_add(c, key);
    if (!f) return false;
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
        "machine_restart_pull_full", "deny_non_loopback",
        "join_triggers_full", "rbe_gate_hits", "rbe_gate_misses",
        "snapshot_done_caps_only", "subs_restored", "epoch_before",
        "epoch_after", "partition_duration_s", "stale_epoch_rejected"
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

bool s5_field_set_status(s5_capture_t *c, const char *key, uint8_t status, s5_source_t src)
{
    char buf[16];
    (void)snprintf(buf, sizeof(buf), "%u", (unsigned)status);
    return set_raw(c, key, buf, src, NULL);
}

/* 从 emit JSON 文本中取某字段的 value 串（简单查找，不引入 JSON 解析器）。 */
static bool json_field_value(const char *json, const char *key, char *out, size_t cap)
{
    if (!json || !key || !out || cap == 0u) return false;
    char pat[96];
    (void)snprintf(pat, sizeof(pat), "\"%s\":{\"value\":", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);
    size_t n = 0u;
    if (*p == '"') { /* 字符串值：取到下一个 '"' */
        ++p;
        while (*p && *p != '"' && n + 1u < cap) out[n++] = *p++;
    } else { /* 数字/null：取到 ',' 或 '}' */
        while (*p && *p != ',' && *p != '}' && n + 1u < cap) out[n++] = *p++;
    }
    out[n] = '\0';
    return n > 0u;
}

bool s5_f4_compare(const char *before_json, const char *after_json,
                   char *out, size_t cap)
{
    char bd[1024], ad[1024], bp[1024], ap[1024];
    bool d_ok = json_field_value(before_json, "digest", bd, sizeof(bd)) &&
                json_field_value(after_json, "digest", ad, sizeof(ad)) &&
                strcmp(bd, ad) == 0;
    bool p_ok = json_field_value(before_json, "peers", bp, sizeof(bp)) &&
                json_field_value(after_json, "peers", ap, sizeof(ap)) &&
                strcmp(bp, ap) == 0;
    int n = snprintf(out, cap,
        "{\"digest_match\":%s,\"peers_restored\":%s}",
        d_ok ? "true" : "false", p_ok ? "true" : "false");
    return n > 0 && (size_t)n < cap;
}
