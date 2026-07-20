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
            cfg->max_points = (uint32_t)strtoul(value, NULL, 10);
        } else if (strcmp(key, "max_subscribers") == 0) {
            cfg->max_subscribers = (uint32_t)strtoul(value, NULL, 10);
        }
    }
    fclose(f);
    return 0;
}
