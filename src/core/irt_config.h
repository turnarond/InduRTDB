/**
 * @file irt_config.h
 * @brief 配置加载器 (简单 key=value 格式, 无 JSON 依赖)
 * @version 3.0.0
 * @date 2026-07-15
 * @copyright MIT License
 */

#ifndef IRT_CORE_IRT_CONFIG_H_
#define IRT_CORE_IRT_CONFIG_H_

#include <stdint.h>

#define IRT_CONFIG_ID_MAX   64
#define IRT_CONFIG_LINE_MAX 256

typedef struct {
    char     instance_id[IRT_CONFIG_ID_MAX];
    uint32_t max_points;
    uint32_t max_subscribers;
} irt_config_t;

/* 设置内置默认值 */
void irt_config_init_defaults(irt_config_t* cfg);

/* 从 key=value 文件加载 (成功 0, 失败 -1). 空行和 # 注释跳过 */
int irt_config_load_file(irt_config_t* cfg, const char* path);

#endif /* IRT_CORE_IRT_CONFIG_H_ */
