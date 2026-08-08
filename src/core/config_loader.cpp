/**
 * @file config_loader.cpp
 * @brief 轻量 YAML 配置解析器实现
 * @version 2.1.0
 */

#include <indurtdb/core/config_loader.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>

namespace indurtdb {
namespace core {

// ---- 辅助函数 ----

// 去除首尾空白, 结果原地写回缓冲区头部
// (注意: 不能只返回跳过头部后的指针 —— 调用方缓冲区必须持有最终字符串)
static void trim_inplace(char* s) {
    char* p = s;
    while (std::isspace(static_cast<unsigned char>(*p))) p++;
    if (p != s) {
        std::memmove(s, p, std::strlen(p) + 1);
    }
    size_t len = std::strlen(s);
    while (len > 0 && std::isspace(static_cast<unsigned char>(s[len - 1]))) {
        s[--len] = '\0';
    }
}

// 去掉成对引号
static void strip_quotes(char* s) {
    size_t len = std::strlen(s);
    if (len >= 2) {
        if ((s[0] == '"' && s[len-1] == '"') ||
            (s[0] == '\'' && s[len-1] == '\'')) {
            std::memmove(s, s + 1, len - 2);
            s[len - 2] = '\0';
        }
    }
}

// 解析 "key: value" 行 (key/value 去首尾空白, value 去引号)
static bool parse_kv(const char* line, char* key_out, size_t key_sz,
                     char* val_out, size_t val_sz) {
    const char* colon = std::strchr(line, ':');
    if (!colon) return false;

    size_t klen = static_cast<size_t>(colon - line);
    if (klen >= key_sz) klen = key_sz - 1;
    std::memcpy(key_out, line, klen);
    key_out[klen] = '\0';
    trim_inplace(key_out);

    std::strncpy(val_out, colon + 1, val_sz - 1);
    val_out[val_sz - 1] = '\0';
    trim_inplace(val_out);
    strip_quotes(val_out);
    return true;
}

// 将单个 key/value 应用到点位配置
static void apply_kv(PointConfig& cur, const char* key, const char* val) {
    if (std::strcmp(key, "id") == 0) {
        cur.id = static_cast<PointId>(std::strtoul(val, nullptr, 10));
    } else if (std::strcmp(key, "name") == 0) {
        std::strncpy(cur.name, val, sizeof(cur.name) - 1);
        cur.name[sizeof(cur.name) - 1] = '\0';
    } else if (std::strcmp(key, "type") == 0) {
        cur.type = parse_type_string(val);
    } else if (std::strcmp(key, "unit") == 0) {
        cur.unit = static_cast<uint16_t>(std::strtoul(val, nullptr, 10));
    } else if (std::strcmp(key, "access") == 0) {
        cur.access = static_cast<uint8_t>(std::strtoul(val, nullptr, 10));
    }
}

// 保存当前条目到结果集 (空条目跳过), 内存不足返回 false
static bool save_current(ConfigResult& out, const PointConfig& cur) {
    if (cur.id == 0 && cur.name[0] == '\0') return true;  // 空条目

    if (out.count >= out.capacity) {
        size_t new_cap = out.capacity * 2;
        auto* np = static_cast<PointConfig*>(
            std::realloc(out.points, new_cap * sizeof(PointConfig)));
        if (!np) return false;
        out.points = np;
        out.capacity = new_cap;
    }
    std::memcpy(&out.points[out.count], &cur, sizeof(PointConfig));
    out.count++;
    return true;
}

uint8_t parse_type_string(const char* s) {
    if (!s) return 0xff;
    if (std::strcmp(s, "bool") == 0)   return 0;
    if (std::strcmp(s, "int") == 0)    return 1;
    if (std::strcmp(s, "int32") == 0)  return 1;
    if (std::strcmp(s, "double") == 0) return 2;
    if (std::strcmp(s, "float") == 0)  return 2;
    if (std::strcmp(s, "str") == 0)    return 3;
    if (std::strcmp(s, "string") == 0) return 3;
    return 0xff;
}

// ---- 主解析函数 ----

bool parse_point_config(const char* path, ConfigResult& out) {
    if (!path) return false;

    FILE* f = std::fopen(path, "r");
    if (!f) {
        std::fprintf(stderr, "[InduRTDB] Cannot open config: %s\n", path);
        return false;
    }

    // 初始分配
    out.capacity = 64;
    out.count = 0;
    out.points = static_cast<PointConfig*>(
        std::calloc(out.capacity, sizeof(PointConfig)));
    if (!out.points) { std::fclose(f); return false; }

    char line[512];
    bool in_points = false;
    PointConfig cur{};  // 必须初始化: 用于判断当前条目是否为空

    while (std::fgets(line, sizeof(line), f)) {
        trim_inplace(line);
        if (line[0] == '\0' || line[0] == '#') continue;

        // 检测 points: 块开始
        if (std::strncmp(line, "points:", 7) == 0) {
            in_points = true;
            continue;
        }
        if (!in_points) continue;

        // 列表项 "- id: 1001 ...": 先保存上一条目,
        // 再把 "- " 之后的剩余部分作为本条目的第一个 kv 解析
        const char* kv_text = line;
        if (line[0] == '-' && line[1] == ' ') {
            if (!save_current(out, cur)) { std::fclose(f); return false; }
            std::memset(&cur, 0, sizeof(cur));
            kv_text = line + 2;
        }

        char key[64], val[128];
        if (!parse_kv(kv_text, key, sizeof(key), val, sizeof(val))) continue;
        apply_kv(cur, key, val);
    }

    // 保存最后一个条目
    if (!save_current(out, cur)) { std::fclose(f); return false; }

    std::fclose(f);
    return out.count > 0;
}

void free_config_result(ConfigResult& result) {
    std::free(result.points);
    result.points = nullptr;
    result.count = 0;
    result.capacity = 0;
}

} // namespace core
} // namespace indurtdb
