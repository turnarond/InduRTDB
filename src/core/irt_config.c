/**
 * @file irt_config.c
 * @brief 配置加载器实现
 * @version 3.0.0
 * @date 2026-07-15
 * @copyright MIT License
 */

#include "core/irt_config.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

void irt_config_init_defaults(irt_config_t* cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->instance_id, IRT_CONFIG_ID_MAX, "default");
    cfg->max_points      = 10000;
    cfg->max_subscribers = 32;
}

int irt_config_load_file(irt_config_t* cfg, const char* path) {
    if (!cfg || !path) return -1;
    FILE* f = fopen(path, "r");
    if (!f) return -1;

    char line[IRT_CONFIG_LINE_MAX];
    while (fgets(line, sizeof(line), f)) {
        /* 去掉末尾换行 */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';

        /* 跳过空行和注释 */
        if (len == 0 || line[0] == '#') continue;

        /* 查找 '=' */
        char* eq = strchr(line, '=');
        if (!eq) continue;

        *eq = '\0';
        const char* key   = line;
        const char* value = eq + 1;

        if (strcmp(key, "instance_id") == 0) {
            snprintf(cfg->instance_id, IRT_CONFIG_ID_MAX, "%s", value);
        } else if (strcmp(key, "max_points") == 0) {
            char* end = NULL;
            cfg->max_points = (uint32_t)strtoul(value, &end, 10);
            if (end == value || *end != '\0') continue;
        } else if (strcmp(key, "max_subscribers") == 0) {
            char* end = NULL;
            cfg->max_subscribers = (uint32_t)strtoul(value, &end, 10);
            if (end == value || *end != '\0') continue;
        }
    }
    fclose(f);
    return 0;
}

/* ================================================================
 * YAML 点位元数据解析 (SRS §3.3, 直译自 v2.x config_loader.cpp)
 * ================================================================ */

#include <ctype.h>

static char* yaml_trim(char* s) {
    while (isspace((unsigned char)*s)) s++;
    if (*s == '\0') return s;
    char* end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return s;
}

static void yaml_strip_quotes(char* s) {
    size_t len = strlen(s);
    if (len >= 2 && ((s[0] == '"' && s[len-1] == '"')
                  || (s[0] == '\'' && s[len-1] == '\''))) {
        memmove(s, s + 1, len - 2);
        s[len - 2] = '\0';
    }
}

static bool yaml_parse_kv(const char* line, char* key_out, size_t key_sz,
                          char* val_out, size_t val_sz) {
    const char* colon = strchr(line, ':');
    if (!colon) return false;
    size_t klen = (size_t)(colon - line);
    if (klen >= key_sz) klen = key_sz - 1;
    memcpy(key_out, line, klen); key_out[klen] = '\0';
    (void)yaml_trim(key_out);   /* key trim in-place: key starts at key_out[0] */
    strncpy(val_out, colon + 1, val_sz - 1);
    val_out[val_sz - 1] = '\0';
    {
        char* v = yaml_trim(val_out);
        yaml_strip_quotes(v);
        if (v != val_out) {
            memmove(val_out, v, strlen(v) + 1);
        }
    }
    return true;
}

uint8_t irt_config_parse_type(const char* s) {
    if (!s) return 0xff;
    if (strcmp(s, "bool") == 0)   return INDURTDB_TYPE_BOOL;
    if (strcmp(s, "int") == 0 || strcmp(s, "int32") == 0)
        return INDURTDB_TYPE_INT32;
    if (strcmp(s, "double") == 0 || strcmp(s, "float") == 0)
        return INDURTDB_TYPE_DOUBLE;
    if (strcmp(s, "str") == 0 || strcmp(s, "string") == 0)
        return INDURTDB_TYPE_STRING;
    return 0xff;
}

int irt_point_config_parse_yaml(const char* path, irt_point_meta_batch_t* out) {
    if (!path || !out) return -1;
    FILE* f = fopen(path, "r");
    if (!f) return -1;

    out->capacity = 64;
    out->count = 0;
    out->points = (irt_point_meta_t*)calloc(out->capacity, sizeof(irt_point_meta_t));
    if (!out->points) { fclose(f); return -1; }

    char line[512];
    bool in_points = false;
    irt_point_meta_t cur;
    memset(&cur, 0, sizeof(cur));

    while (fgets(line, sizeof(line), f)) {
        char* t = yaml_trim(line);
        if (t[0] == '\0' || t[0] == '#') continue;

        if (strncmp(t, "points:", 7) == 0) {
            in_points = true;
            continue;
        }
        if (!in_points) continue;

        /* 列表项 "- id: ..." 开始新点位, 同时解析此行内的 key:value */
        if (t[0] == '-' && t[1] == ' ') {
            if (cur.id != 0 || cur.name[0] != '\0') {
                if (out->count >= out->capacity) {
                    size_t nc = out->capacity * 2;
                    void* np = realloc(out->points, nc * sizeof(irt_point_meta_t));
                    if (!np) { free(out->points); out->points = NULL; fclose(f); return -1; }
                    out->points = (irt_point_meta_t*)np;
                    out->capacity = nc;
                }
                memcpy(&out->points[out->count], &cur, sizeof(cur));
                out->count++;
            }
            memset(&cur, 0, sizeof(cur));
            /* 不从 continue 跳过 —— 此行可能包含 id 字段:
             * "- id: 50" → 截断前缀 "- " 后得到 "id: 50" */
            t += 2;
            while (*t == ' ') t++;
        }

        char key[64], val[128];
        if (!yaml_parse_kv(t, key, sizeof(key), val, sizeof(val))) continue;

        if (strcmp(key, "id") == 0) {
            char* end = NULL;
            cur.id = (uint32_t)strtoul(val, &end, 10);
            if (end == val || *end != '\0') continue;  /* 非数字, 跳过此条目 */
        } else if (strcmp(key, "name") == 0) {
            strncpy(cur.name, val, sizeof(cur.name) - 1);
            cur.name[sizeof(cur.name) - 1] = '\0';
        } else if (strcmp(key, "type") == 0)
            cur.type = irt_config_parse_type(val);
        else if (strcmp(key, "unit") == 0)
            cur.unit = (uint16_t)strtoul(val, NULL, 10);
        else if (strcmp(key, "access") == 0)
            cur.access = (uint8_t)strtoul(val, NULL, 10);
    }

    /* 保存最后一个条目 */
    if (cur.id != 0 || cur.name[0] != '\0') {
        if (out->count >= out->capacity) {
            size_t nc = out->capacity * 2;
            void* np = realloc(out->points, nc * sizeof(irt_point_meta_t));
            if (!np) { free(out->points); out->points = NULL; fclose(f); return -1; }
            out->points = (irt_point_meta_t*)np;
            out->capacity = nc;
        }
        memcpy(&out->points[out->count], &cur, sizeof(cur));
        out->count++;
    }

    fclose(f);
    return (int)out->count;
}

void irt_point_config_free(irt_point_meta_batch_t* batch) {
    if (!batch) return;
    free(batch->points);
    batch->points = NULL;
    batch->count = 0;
    batch->capacity = 0;
}
