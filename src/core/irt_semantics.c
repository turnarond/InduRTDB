/**
 * @file irt_semantics.c
 * @brief v3.7 主题A：点位语义纯函数（值归一化 / EURange 量程位 / 死区判定）
 *
 * 纯函数、无状态、不触共享内存，集中收口"值→语义"逻辑，
 * 供核心可选质量感知写入口与 rtdbd 写权威复用，消除散落的多处 per-type 处理。
 * @version 3.7.0
 */

#include <indurtdb/indurtdb.h>

/* 点位值 → double 归一化（bool/int32/uint32/int64/float/double）。
 * rtdbd 传入 &w->value_bits（uint64_t，低 32 位承载 int32/uint32 等），
 * 按 type 解析对应子类型，与 indurtdb_point_t.value union 一致。 */
double indurtdb_value_to_double(uint8_t type, const void* value_bits) {
    if (!value_bits) return 0.0;
    switch (type) {
    case INDURTDB_TYPE_BOOL:   return *(const bool*)value_bits ? 1.0 : 0.0;
    case INDURTDB_TYPE_INT32:  return (double)(*(const int32_t*)value_bits);
    case INDURTDB_TYPE_UINT32: return (double)(*(const uint32_t*)value_bits);
    case INDURTDB_TYPE_INT64:  return (double)(*(const int64_t*)value_bits);
    case INDURTDB_TYPE_FLOAT:  return (double)(*(const float*)value_bits);
    case INDURTDB_TYPE_DOUBLE: return *(const double*)value_bits;
    default:                   return 0.0;
    }
}

/* EURange 量程位：eur 未启用返回 NONE；低于下界→LOW，高于上界→HIGH，区间内（含边界）→NONE。 */
uint8_t indurtdb_eurange_limit(bool eur_enabled, double eur_min, double eur_max, double value) {
    if (!eur_enabled) return INDURTDB_LIMIT_NONE;
    if (value < eur_min) return INDURTDB_LIMIT_LOW;
    if (value > eur_max) return INDURTDB_LIMIT_HIGH;
    return INDURTDB_LIMIT_NONE;
}

/* 死区是否越过：abs_mode=绝对阈值；否则(百分比)阈值=deadband%*(eur_max-eur_min)。
 * 严格大于阈值才触发（等号不触发）；百分比模式 eur_max==eur_min 时不触发（除零保护）。 */
bool indurtdb_deadband_exceeded(bool abs_mode, double deadband,
                                double eur_min, double eur_max,
                                double last, double cur) {
    double delta = (cur > last) ? (cur - last) : (last - cur);
    if (abs_mode) {
        return delta > deadband;
    }
    /* 百分比模式：阈值 = deadband% * (eur_max - eur_min) */
    double span = eur_max - eur_min;
    if (span <= 0.0) return false; /* eur_max==eur_min / 非法：不触发（除零保护） */
    double abs_thresh = deadband * 0.01 * span;
    return delta > abs_thresh;
}
