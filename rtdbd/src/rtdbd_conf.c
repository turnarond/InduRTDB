#include "rtdbd_conf.h"

#include <stdio.h>
#include <string.h>

/* 行缓冲上限：分区白名单/别名都是短内联键，512 足够；
 * 超长行（含注释头）按截断处理，只有 partitions/alias 两键超长才报错。 */
#define RTDBD_CONF_LINE_MAX 512

void rtdbd_conf_init(rtdbd_conf_t* c)
{
    if (!c) return;
    memset(c, 0, sizeof(*c));
    c->max_clients          = RTDBD_CONF_DEFAULT_MAX_CLIENTS;
    c->initial_clients      = RTDBD_CONF_DEFAULT_INITIAL_CLIENTS;
    c->max_subs_per_conn    = RTDBD_CONF_DEFAULT_MAX_SUBS;
    c->initial_subs_per_conn = RTDBD_CONF_DEFAULT_INITIAL_SUBS;
    c->max_batch            = RTDBD_CONF_DEFAULT_MAX_BATCH;
}

static int is_ws(char ch) { return ch == ' ' || ch == '\t'; }

static char* trim(char* s)
{
    while (is_ws(*s)) s++;
    char* e = s + strlen(s);
    while (e > s && (e[-1] == '\r' || is_ws(e[-1]))) *--e = '\0';
    return s;
}

/* 跳过空白 */
static void skip_ws(const char** pp)
{
    while (is_ws(**pp)) (*pp)++;
}

/* 严格十进制 uint32：至少一位数字、无符号、不溢出。
 * 成功 0 并前进 *pp；失败 -1（不改 *pp 的语义位置）。 */
static int parse_u32(const char** pp, uint32_t* out)
{
    const char* p = *pp;
    if (*p < '0' || *p > '9') return -1;
    unsigned long v = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10ul + (unsigned long)(*p - '0');
        if (v > 0xFFFFFFFFul) return -1;
        p++;
    }
    *out = (uint32_t)v;
    *pp  = p;
    return 0;
}

/* 标量容量值：只允许十进制数字，可带行尾注释。 */
static int parse_u32_scalar(const char* v, uint32_t* out)
{
    const char* p = v;
    skip_ws(&p);
    if (parse_u32(&p, out) != 0) return -1;
    skip_ws(&p);
    if (*p == '#') return 0;
    return (*p == '\0') ? 0 : -1;
}

static int parse_capacity_key(const char* key, const char* val, rtdbd_conf_t* c)
{
    uint32_t v;
    if (parse_u32_scalar(val, &v) != 0) return -1;

    if (strcmp(key, "max_clients") == 0) {
        c->max_clients = v;
        c->caps_present |= RTDBD_CONF_HAS_MAX_CLIENTS;
    } else if (strcmp(key, "initial_clients") == 0) {
        c->initial_clients = v;
        c->caps_present |= RTDBD_CONF_HAS_INITIAL_CLIENTS;
    } else if (strcmp(key, "max_subs_per_conn") == 0) {
        c->max_subs_per_conn = v;
        c->caps_present |= RTDBD_CONF_HAS_MAX_SUBS_PER_CONN;
    } else if (strcmp(key, "initial_subs_per_conn") == 0) {
        c->initial_subs_per_conn = v;
        c->caps_present |= RTDBD_CONF_HAS_INITIAL_SUBS;
    } else if (strcmp(key, "max_batch") == 0) {
        c->max_batch = v;
        c->caps_present |= RTDBD_CONF_HAS_MAX_BATCH;
    } else {
        return -1;
    }
    return 0;
}

/* partitions: [0, 1, 2] —— 内联十进制列表。空 [] 视为「未配置」（宽松）。 */
static int parse_partitions(const char* v, rtdbd_conf_t* c)
{
    const char* p = v;
    skip_ws(&p);
    if (*p != '[') return -1;
    p++;
    c->n_partitions = 0;

    skip_ws(&p);
    if (*p == ']') return 0;   /* 空列表 = 任意分区合法 */

    for (;;) {
        uint32_t val;
        skip_ws(&p);
        if (parse_u32(&p, &val) != 0) return -1;
        if (c->n_partitions >= RTDBD_CONF_MAX_PARTITIONS) return -1;
        c->partitions[c->n_partitions++] = val;

        skip_ws(&p);
        if (*p == ',') { p++; continue; }
        if (*p == ']') break;
        return -1;
    }
    return 0;
}

/* partition_alias: {0: "default", 1: "unitA"} —— 内联 uint32 → 引号字符串映射。
 * 字符串可单/双引号；逗号与花括号在引号内不参与分隔。 */
static int parse_aliases(const char* v, rtdbd_conf_t* c, uint32_t line)
{
    const char* p = v;
    skip_ws(&p);
    if (*p != '{') return -1;
    p++;

    skip_ws(&p);
    if (*p == '}') return 0;   /* 空映射 */

    for (;;) {
        uint32_t key;
        skip_ws(&p);
        if (parse_u32(&p, &key) != 0) return -1;

        skip_ws(&p);
        if (*p != ':') return -1;
        p++;
        skip_ws(&p);
        if (*p != '"' && *p != '\'') return -1;

        char quote = *p;
        p++;
        char name[RTDBD_CONF_ALIAS_MAX];
        size_t n = 0;
        while (*p && *p != quote) {
            if (n < sizeof(name) - 1u) name[n++] = *p;
            p++;
        }
        if (*p != quote) return -1;   /* 未闭合引号 */
        name[n] = '\0';
        p++;

        if (c->n_aliases >= RTDBD_CONF_MAX_ALIASES) return -1;
        c->aliases[c->n_aliases].key = key;
        memcpy(c->aliases[c->n_aliases].name, name, sizeof(name));
        c->alias_line[c->n_aliases] = line;
        c->n_aliases++;

        skip_ws(&p);
        if (*p == ',') { p++; continue; }
        if (*p == '}') break;
        return -1;
    }
    return 0;
}

int rtdbd_conf_load(rtdbd_conf_t* c, const char* path, uint32_t* err_line)
{
    if (!c) return -2;
    rtdbd_conf_init(c);
    if (err_line) *err_line = 0;
    if (!path) return 0;

    FILE* f = fopen(path, "r");
    if (!f) return -2;

    char line[RTDBD_CONF_LINE_MAX];
    uint32_t lineno = 0;
    int in_rtdbd = 0;

    while (fgets(line, sizeof(line), f)) {
        lineno++;

        /* 行超缓冲（无换行且非 EOF）：吞掉本行剩余字节，避免残段被当作下一行解析。 */
        int truncated = 0;
        if (strchr(line, '\n') == NULL && !feof(f)) {
            truncated = 1;
            int ch;
            while ((ch = fgetc(f)) != EOF && ch != '\n') { /* discard */ }
        }
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        char* t = trim(line);
        if (*t == '\0' || *t == '#') continue;

        /* 只读 `points:` 之前的顶层键；之后的内容归核心解析器。 */
        if (strncmp(t, "points:", 7) == 0) break;

        char* first = line;
        while (is_ws(*first)) first++;
        int indented = (first != line);

        char* colon = strchr(t, ':');
        if (!colon) {
            if (in_rtdbd && indented) {
                if (err_line) *err_line = lineno;
                fclose(f);
                return -1;
            }
            continue;   /* 非键值行忽略（与核心解析器一致的宽松） */
        }
        *colon = '\0';
        char* key = trim(t);
        char* val = trim(colon + 1);

        if (in_rtdbd && !indented) in_rtdbd = 0;

        if (strcmp(key, "rtdbd") == 0 && !indented) {
            if (truncated || *val != '\0') {
                if (err_line) *err_line = lineno;
                fclose(f);
                return -1;
            }
            in_rtdbd = 1;
            continue;
        }

        if (in_rtdbd) {
            if (!indented || parse_capacity_key(key, val, c) != 0) {
                if (err_line) *err_line = lineno;
                fclose(f);
                return -1;
            }
            continue;
        }

        if (strcmp(key, "partitions") == 0) {
            if (truncated || parse_partitions(val, c) != 0) {
                if (err_line) *err_line = lineno;
                fclose(f);
                return -1;
            }
        } else if (strcmp(key, "partition_alias") == 0) {
            if (truncated || parse_aliases(val, c, lineno) != 0) {
                if (err_line) *err_line = lineno;
                fclose(f);
                return -1;
            }
        }
        /* 其它键（instance_id / max_points 等）静默忽略 —— 与既有约定一致。 */
    }

    fclose(f);

    /* 存在白名单时，别名键必须都在白名单内，否则配置错误（fail-fast）。 */
    if (c->n_partitions > 0) {
        for (uint32_t i = 0; i < c->n_aliases; ++i) {
            uint32_t k = c->aliases[i].key;
            int found = 0;
            for (uint32_t j = 0; j < c->n_partitions; ++j) {
                if (c->partitions[j] == k) { found = 1; break; }
            }
            if (!found) {
                if (err_line) *err_line = c->alias_line[i];
                return -1;
            }
        }
    }
    return 0;
}

int rtdbd_conf_partition_allowed(const rtdbd_conf_t* c, uint32_t p)
{
    if (!c || c->n_partitions == 0) return 1;   /* 无白名单 ⇒ 宽松 */
    for (uint32_t i = 0; i < c->n_partitions; ++i) {
        if (c->partitions[i] == p) return 1;
    }
    return 0;
}
