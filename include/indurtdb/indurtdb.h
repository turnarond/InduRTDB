/**
 * @file indurtdb.h
 * @brief InduRTDB 纯 C 公共 API（单例风格）
 * @version 3.3.0
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

/* ==== 版本 (须与 VERSION 文件、CMake project(VERSION) 一致) ==== */
#define INDURTDB_VERSION_MAJOR 3
#define INDURTDB_VERSION_MINOR 6
#define INDURTDB_VERSION_PATCH 0
#define INDURTDB_VERSION_STRING "3.6.0"

/* ==== 点位类型/质量/权限常量 (与 v2.x 枚举值一致) ==== */
#define INDURTDB_TYPE_BOOL     0
#define INDURTDB_TYPE_INT32    1
#define INDURTDB_TYPE_DOUBLE   2
#define INDURTDB_TYPE_STRING   3
/* v3.4 T6 类型扩展：复用 value union 已有 32B，布局不变 */
#define INDURTDB_TYPE_INT64    4
#define INDURTDB_TYPE_UINT32   5
#define INDURTDB_TYPE_FLOAT    6

#define INDURTDB_QUALITY_GOOD        0
#define INDURTDB_QUALITY_BAD         1
#define INDURTDB_QUALITY_TIMEOUT     2
#define INDURTDB_QUALITY_SUBSTITUTED 3

/* 注：access 为「描述性元数据」（点位静态属性标记）。库与 rtdbd 写路径
 * 均不据此强制访问控制 —— 真正的写管控由 rtdbd 的 SO_PEERCRED + UID 策略
 * （deny by default）承担。请勿将 access 当作安全边界。 */
#define INDURTDB_ACCESS_READ_ONLY   1
#define INDURTDB_ACCESS_READ_WRITE  3

/* ==== 点位数据 (128 字节, 与 v2.x PointData 布局逐字节一致) ==== */
typedef struct {
    union {
        bool    b;
        int32_t i;
        int64_t i64;
        uint32_t u32;
        float   f;
        double  d;
        char    str[32];
    } value;
    uint64_t timestamp_ns;
    uint8_t  type;      /* INDURTDB_TYPE_*    */
    uint8_t  quality;   /* INDURTDB_QUALITY_* */
    uint16_t unit;
    uint8_t  access;    /* INDURTDB_ACCESS_*  */
    char     name[64];              /* 45–108  */
    uint8_t  padding[3];            /* 109–111: 保持 8B 对齐前导 */
    uint64_t source_timestamp_ns;   /* 112–119: 采集时刻(SourceTimestamp)，0=未提供 */
    uint8_t  reserved[8];           /* 120–127: 保留（须为 0） */
} __attribute__((packed, aligned(128))) indurtdb_point_t;

/* ---- 元数据 (v3.4 新增, 每点 32B, 冷数据, 按 point_id O(1) 索引) ----
 * 仅存储参数, 不参与热路径读写, 也不上送执行。 */
typedef struct {
    double   eur_min;     /* 0–7   : 工程量程下限 */
    double   eur_max;     /* 8–15  : 工程量程上限 */
    float    deadband;    /* 16–19 : 变化阈值（只存参数） */
    uint32_t flags;       /* 20–23 : 是否启用量程 / 死区等（bit0=eur, bit1=deadband） */
    uint8_t  reserved[8]; /* 24–31 : 保留（须为 0） */
} __attribute__((packed, aligned(32))) indurtdb_meta_t;

/* ---- 质量位（quality）分层布局 ----
 *   bit 0–3 : 基础质量码（16 种，现有 0–3 取值不变）
 *   bit 4–5 : 量程位（无 / Low / High / Constant）
 *   bit 6–7 : 预留（须为 0）
 *
 * 「值是否可用」只看基础码；量程位是与可用性正交的附加信息。
 */
#define INDURTDB_QUALITY_UNCERTAIN        4
#define INDURTDB_QUALITY_NOT_INITIALIZED  5
#define INDURTDB_QUALITY_OUT_OF_SERVICE   6
#define INDURTDB_QUALITY_COMM_FAILURE     7
#define INDURTDB_QUALITY_SENSOR_FAILURE   8
#define INDURTDB_QUALITY_LAST_KNOWN       9
#define INDURTDB_QUALITY_CONFIG_ERROR     10

#define INDURTDB_LIMIT_NONE     0u
#define INDURTDB_LIMIT_LOW      1u
#define INDURTDB_LIMIT_HIGH     2u
#define INDURTDB_LIMIT_CONSTANT 3u

#define INDURTDB_QUALITY_BASE(q)  ((uint8_t)((q) & 0x0Fu))
#define INDURTDB_QUALITY_LIMIT(q) ((uint8_t)(((q) >> 4) & 0x03u))
#define INDURTDB_QUALITY_MAKE(base, limit) \
        ((uint8_t)(((base) & 0x0Fu) | (((limit) & 0x03u) << 4)))

/* ==== v3.4 T6: 质量语义与 OPC UA 映射（纯函数，无状态） ====
 * 将分层质量码映射为 OPC UA StatusCode（供北向上送）。
 * - base 码决定 severity(bit30-31) + 子状态(code, bit0-15)；
 * - limit 位映射到 StatusCode 的保留位(bit28-29)——远离 severity(bit30-31)，且避开
 *   OPC UA 已定义位(bit24 StructureChanged / bit25 SemanticsChanged)，不污染 severity 与 code；
 * - 双向可逆（见 indurtdb.c 映射表），未知 StatusCode 回退为 BAD。 */
uint32_t indurtdb_quality_to_status_code(uint8_t quality);
uint8_t  indurtdb_status_code_to_quality(uint32_t status_code);
/* 「值是否可用」只看基础码（量程位是与可用性正交的附加信息） */
bool     indurtdb_quality_is_usable(uint8_t quality);

/* ==== 错误码 (v3.4 起新增接口返回语义化负值) ====
 * 既有接口仍返回 -1（= INDURTDB_ERR_ARG 语义），新增接口使用下列码，
 * 便于调用方区分"参数错 / 没找到 / 空间满 / 未初始化 / 忙"。
 * 向前兼容：旧代码只判 < 0 或 != 0 的行为完全不变。 */
#define INDURTDB_OK          0
#define INDURTDB_ERR_ARG     (-1)   /* 参数非法（空指针、空名、越界 id） */
#define INDURTDB_ERR_NOT_FOUND (-2) /* 按名未找到点位 */
#define INDURTDB_ERR_FULL    (-3)   /* 索引/表已满，无法注册 */
#define INDURTDB_ERR_NOT_INIT (-4)  /* 未初始化 */
#define INDURTDB_ERR_BUSY    (-5)   /* 并发写冲突，重试耗尽 */

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

/* ==== 单点写（携带采集时刻 SourceTimestamp） ====
 * source_ts_ns 为数据在现场被采集的时刻；传 0 表示"未提供"，
 * 此时点位语义退化为仅使用入库时刻 timestamp_ns（旧行为不变）。
 */
int indurtdb_write_bool_ts(uint32_t id, bool value, uint64_t source_ts_ns);
int indurtdb_write_int32_ts(uint32_t id, int32_t value, uint64_t source_ts_ns);
int indurtdb_write_double_ts(uint32_t id, double value, uint64_t source_ts_ns);
int indurtdb_write_string_ts(uint32_t id, const char* value, uint64_t source_ts_ns);

/* ==== 质量标记 ====
 * 用于显式设置点位质量（如与控制面失联时标记 COMM_FAILURE）。
 * 取值见 INDURTDB_QUALITY_* 与 INDURTDB_QUALITY_MAKE()。
 */
int indurtdb_set_quality(uint32_t id, uint8_t quality);

/* ==== 单点读 ==== */
int indurtdb_read_bool(uint32_t id, bool* value);
int indurtdb_read_int32(uint32_t id, int32_t* value);
int indurtdb_read_double(uint32_t id, double* value);
int indurtdb_read_string(uint32_t id, char* buffer, size_t buffer_size);
int indurtdb_read_point(uint32_t id, indurtdb_point_t* point_data);

/* ==== v3.4 T6: 类型扩展（int64 / uint32 / float，复用 value union 32B，布局不变） ==== */
int indurtdb_write_int64(uint32_t id, int64_t value);
int indurtdb_write_uint32(uint32_t id, uint32_t value);
int indurtdb_write_float(uint32_t id, float value);
int indurtdb_read_int64(uint32_t id, int64_t* value);
int indurtdb_read_uint32(uint32_t id, uint32_t* value);
int indurtdb_read_float(uint32_t id, float* value);
int indurtdb_write_int64_ts(uint32_t id, int64_t value, uint64_t source_ts_ns);
int indurtdb_write_uint32_ts(uint32_t id, uint32_t value, uint64_t source_ts_ns);
int indurtdb_write_float_ts(uint32_t id, float value, uint64_t source_ts_ns);
/** 单拷贝快速读取点位数据 (seqlock 保护, 拷贝到线程本地缓冲后返回其指针).
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

/* v3.4: 按点位名查找 id（共享内存内 name→id 索引，全局一致）。
 * 成功返回 INDURTDB_OK(0) 并写 *out_id；未找到返回 INDURTDB_ERR_NOT_FOUND；
 * 参数非法 INDURTDB_ERR_ARG；并发冲突重试耗尽 INDURTDB_ERR_BUSY。
 * 点位名须先经 indurtdb_load_config() 注册（或由 rtdbd 注册，T9）。 */
int indurtdb_find_by_name(const char* name, uint32_t* out_id);

/* v3.4 T3: 读写点位元数据 (每点 32B: eur_min/max/deadband/flags)。
 * 冷数据, 按 id O(1) 索引, 不进入读写热路径, 也不上送执行。
 * set 成功返回 INDURTDB_OK(0); 越界 id / 空指针返回 INDURTDB_ERR_ARG;
 * 未初始化 INDURTDB_ERR_NOT_INIT; 并发写冲突重试耗尽 INDURTDB_ERR_BUSY。
 * 读/写前须确保 id 对应的点已存在 (注册), 否则数据无意义。 */
int indurtdb_get_meta(uint32_t id, indurtdb_meta_t* meta);
int indurtdb_set_meta(uint32_t id, const indurtdb_meta_t* meta);
int indurtdb_create_point(uint32_t id, const char* name, int type, int access);
int indurtdb_delete_point(uint32_t id);
int indurtdb_rename_point(uint32_t id, const char* name);
int  indurtdb_check_timeouts(uint64_t timeout_ns);
uint64_t indurtdb_get_write_count(void);
uint64_t indurtdb_get_timeout_count(void);
uint64_t indurtdb_get_scan_skipped(void);  /* v3.4 T7: 被超时扫描跳过的点数（可观测） */
const char* indurtdb_get_last_error(void);

/* ==== API v2: 句柄化 (v3.4 新增) ====
 * 去单例化: 同一进程可同时持有多个实例 (indurtdb_t* 不透明句柄),
 * 各自读写互不干扰。v1 的全局函数保留为默认句柄的薄封装 (见下), 不破既有调用方。
 *
 * 命名: v2 一律加 `_h_` 前缀 (C 无重载); 与内部 `irt_` 前缀不冲突。 */
typedef struct indurtdb indurtdb_t;   /* 不透明句柄, 定义见库实现 */

typedef struct {
    uint32_t max_points;        /* 必填 > 0 */
    uint32_t max_subscribers;   /* 0 表示不订阅 */
} indurtdb_cfg_t;

/* 打开/创建一个实例。成功返回 0 并把实例写入 *out;
 * 参数非法 INDURTDB_ERR_ARG; 实例槽位耗尽 INDURTDB_ERR_FULL。
 * 同 instance_id 跨进程仍共享同一段 (与 v1 行为一致)。 */
int indurtdb_h_open(indurtdb_t** out, const char* instance_id,
                    const indurtdb_cfg_t* cfg);
/* 关闭实例 (进程本地 detach; owner 进程负责 shm_unlink)。
 * 对同一句柄多次调用安全 (幂等)。 */
void indurtdb_h_close(indurtdb_t* h);
bool indurtdb_h_is_initialized(const indurtdb_t* h);

int indurtdb_h_write_bool(indurtdb_t* h, uint32_t id, bool value);
int indurtdb_h_write_int32(indurtdb_t* h, uint32_t id, int32_t value);
int indurtdb_h_write_double(indurtdb_t* h, uint32_t id, double value);
int indurtdb_h_write_string(indurtdb_t* h, uint32_t id, const char* value);
int indurtdb_h_write_bool_ts(indurtdb_t* h, uint32_t id, bool value, uint64_t source_ts_ns);
int indurtdb_h_write_int32_ts(indurtdb_t* h, uint32_t id, int32_t value, uint64_t source_ts_ns);
int indurtdb_h_write_double_ts(indurtdb_t* h, uint32_t id, double value, uint64_t source_ts_ns);
int indurtdb_h_write_string_ts(indurtdb_t* h, uint32_t id, const char* value, uint64_t source_ts_ns);

int indurtdb_h_read_bool(indurtdb_t* h, uint32_t id, bool* value);
int indurtdb_h_read_int32(indurtdb_t* h, uint32_t id, int32_t* value);
int indurtdb_h_read_double(indurtdb_t* h, uint32_t id, double* value);
int indurtdb_h_read_string(indurtdb_t* h, uint32_t id, char* buffer, size_t buffer_size);
int indurtdb_h_read_point(indurtdb_t* h, uint32_t id, indurtdb_point_t* point_data);
const indurtdb_point_t* indurtdb_h_peek(indurtdb_t* h, uint32_t id);
int indurtdb_h_read_range(indurtdb_t* h, uint32_t start_id, uint16_t count,
                          indurtdb_point_t* out_buf, uint16_t out_cap);
int indurtdb_h_write_range_bool(indurtdb_t* h, uint32_t start_id, const bool* values, uint16_t count);
int indurtdb_h_write_range_int32(indurtdb_t* h, uint32_t start_id, const int32_t* values, uint16_t count);
int indurtdb_h_write_range_double(indurtdb_t* h, uint32_t start_id, const double* values, uint16_t count);

/* v3.4 T6: 类型扩展（int64 / uint32 / float，复用 value union 32B，布局不变） */
int indurtdb_h_write_int64(indurtdb_t* h, uint32_t id, int64_t value);
int indurtdb_h_write_uint32(indurtdb_t* h, uint32_t id, uint32_t value);
int indurtdb_h_write_float(indurtdb_t* h, uint32_t id, float value);
int indurtdb_h_read_int64(indurtdb_t* h, uint32_t id, int64_t* value);
int indurtdb_h_read_uint32(indurtdb_t* h, uint32_t id, uint32_t* value);
int indurtdb_h_read_float(indurtdb_t* h, uint32_t id, float* value);
int indurtdb_h_write_int64_ts(indurtdb_t* h, uint32_t id, int64_t value, uint64_t source_ts_ns);
int indurtdb_h_write_uint32_ts(indurtdb_t* h, uint32_t id, uint32_t value, uint64_t source_ts_ns);
int indurtdb_h_write_float_ts(indurtdb_t* h, uint32_t id, float value, uint64_t source_ts_ns);

int indurtdb_h_subscribe(indurtdb_t* h, uint32_t id, indurtdb_callback_t cb, void* user_data);
int indurtdb_h_unsubscribe(indurtdb_t* h, uint32_t id);
int indurtdb_h_load_config(indurtdb_t* h, const char* config_path);
int indurtdb_h_set_quality(indurtdb_t* h, uint32_t id, uint8_t quality);
void indurtdb_h_update_heartbeat(indurtdb_t* h);

int indurtdb_h_find_by_name(indurtdb_t* h, const char* name, uint32_t* out_id);
int indurtdb_h_get_meta(indurtdb_t* h, uint32_t id, indurtdb_meta_t* meta);
int indurtdb_h_set_meta(indurtdb_t* h, uint32_t id, const indurtdb_meta_t* meta);

/* v3.6 管控写通道：点位 CRUD（运行时注册表增删改名；须配合 rtdbd 的 uid 鉴权使用）。
 * create: 在空闲槽(id 对应点位 name[0]=='\0')注册；同名已存在(指其他 id)返回 ERR_ARG；
 *         同 id 已注册返回 ERR_FULL（非幂等更新）。索引插入失败(BUSY/FULL)会回滚注册。
 * delete: 清空 name[0] 并注销 name→id 索引；点位不存在返回 ERR_NOT_FOUND。
 * rename: 改 name 并同步索引（原子）；点位不存在返回 ERR_NOT_FOUND；同名(同 id)为幂等 no-op。
 * 返回 INDURTDB_OK / INDURTDB_ERR_ARG / INDURTDB_ERR_NOT_FOUND / INDURTDB_ERR_FULL / INDURTDB_ERR_BUSY。 */
int indurtdb_h_create_point(indurtdb_t* h, uint32_t id, const char* name, int type, int access);
int indurtdb_h_delete_point(indurtdb_t* h, uint32_t id);
int indurtdb_h_rename_point(indurtdb_t* h, uint32_t id, const char* name);

int indurtdb_h_check_timeouts(indurtdb_t* h, uint64_t timeout_ns);
uint64_t indurtdb_h_get_write_count(indurtdb_t* h);
uint64_t indurtdb_h_get_timeout_count(indurtdb_t* h);
uint64_t indurtdb_h_get_scan_skipped(indurtdb_t* h);  /* v3.4 T7 */
int indurtdb_h_validate_id(indurtdb_t* h, uint32_t id);

/* v1 全局函数 (= 默认句柄的薄封装) 仍全部保留, 行为不变 ——
 * 仅在 v3.5 起标注 INDURTDB_DEPRECATED 并移除 (届时同步迁移测试/调用方)。 */

#ifdef __cplusplus
}
#endif

#endif /* INDURTDB_INDURTDB_H_ */
