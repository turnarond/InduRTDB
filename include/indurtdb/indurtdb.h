/**
 * @file indurtdb.h
 * @brief InduRTDB 纯 C 公共 API（单例风格）
 * @version 3.0.0
 * @date 2026-07-15
 * @copyright MIT License
 */

#ifndef INDURTDB_INDURTDB_H_
#define INDURTDB_INDURTDB_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==== 点位类型/质量/权限常量 (与 v2.x 枚举值一致) ==== */
#define INDURTDB_TYPE_BOOL     0
#define INDURTDB_TYPE_INT32    1
#define INDURTDB_TYPE_DOUBLE   2
#define INDURTDB_TYPE_STRING   3

#define INDURTDB_QUALITY_GOOD        0
#define INDURTDB_QUALITY_BAD         1
#define INDURTDB_QUALITY_TIMEOUT     2
#define INDURTDB_QUALITY_SUBSTITUTED 3

#define INDURTDB_ACCESS_READ_ONLY   1
#define INDURTDB_ACCESS_READ_WRITE  3

/* ==== 点位数据 (128 字节, 与 v2.x PointData 布局逐字节一致) ==== */
typedef struct {
    union {
        bool    b;
        int32_t i;
        double  d;
        char    str[32];
    } value;
    uint64_t timestamp_ns;
    uint8_t  type;      /* INDURTDB_TYPE_*    */
    uint8_t  quality;   /* INDURTDB_QUALITY_* */
    uint16_t unit;
    uint8_t  access;    /* INDURTDB_ACCESS_*  */
    char     name[64];
    uint8_t  padding[19];
} __attribute__((packed, aligned(128))) indurtdb_point_t;

/* ==== 订阅回调 ==== */
typedef void (*indurtdb_callback_t)(uint32_t id,
    const indurtdb_point_t* data, void* user_data);

/* ==== 生命周期 ==== */
int  indurtdb_initialize(const char* instance_id,
                         uint32_t max_points, uint32_t max_subscribers);
void indurtdb_shutdown(void);
bool indurtdb_is_initialized(void);

/* ==== 单点写 ==== */
int indurtdb_write_bool(uint32_t id, bool value);
int indurtdb_write_int32(uint32_t id, int32_t value);
int indurtdb_write_double(uint32_t id, double value);
int indurtdb_write_string(uint32_t id, const char* value);

/* ==== 单点读 ==== */
int indurtdb_read_bool(uint32_t id, bool* value);
int indurtdb_read_int32(uint32_t id, int32_t* value);
int indurtdb_read_double(uint32_t id, double* value);
int indurtdb_read_string(uint32_t id, char* buffer, size_t buffer_size);
int indurtdb_read_point(uint32_t id, indurtdb_point_t* point_data);
/** 零拷贝读取点位数据 (seqlock 保护, 线程本地缓冲).
 * 返回的指针在下一次 indurtdb_peek() 调用时被覆盖 (同线程).
 * 如需长期持有数据, 请用 indurtdb_read_point() 拷贝到自管理的缓冲区. */
const indurtdb_point_t* indurtdb_peek(uint32_t id);

/* ==== 批量 (返回实际处理点数, 负值=参数错误) ==== */
int indurtdb_read_range(uint32_t start_id, uint16_t count,
                        indurtdb_point_t* out_buf, uint16_t out_cap);
int indurtdb_write_range_bool(uint32_t start_id, const bool* values, uint16_t count);
int indurtdb_write_range_int32(uint32_t start_id, const int32_t* values, uint16_t count);
int indurtdb_write_range_double(uint32_t start_id, const double* values, uint16_t count);

/* ==== 订阅 ==== */
int indurtdb_subscribe(uint32_t id, indurtdb_callback_t cb, void* user_data);
int indurtdb_unsubscribe(uint32_t id);

/* ==== 配置/心跳 ==== */
int  indurtdb_load_config(const char* config_path);
void indurtdb_update_heartbeat(void);

/* ==== 校验/统计/错误 ==== */
int indurtdb_validate_id(uint32_t id);
int  indurtdb_check_timeouts(uint64_t timeout_ns);
uint64_t indurtdb_get_write_count(void);
uint64_t indurtdb_get_timeout_count(void);
const char* indurtdb_get_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* INDURTDB_INDURTDB_H_ */
