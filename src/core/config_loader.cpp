/**
 * @file config_loader.cpp
 * @brief 轻量 YAML 配置解析器实现
 * @version 2.0.0
 */

#include <indurtdb/core/config_loader.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>

namespace indurtdb {
namespace core {

// ---- 辅助函数 ----

static char* trim(char* s) {
    while (std::isspace(static_cast<unsigned char>(*s))) s++;
    if (*s == '\0') return s;
    char* end = s + std::strlen(s) - 1;
    while (end > s && std::isspace(static_cast<unsigned char>(*end))) end--;
    end[1] = '\0';
    return s;
}

// 去掉引号
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

// 解析 "key: value" 行
static bool parse_kv(const char* line, char* key_out, size_t key_sz,
                     char* val_out, size_t val_sz) {
    const char* colon = std::strchr(line, ':');
    if (!colon) return false;

    size_t klen = static_cast<size_t>(colon - line);
    if (klen >= key_sz) klen = key_sz - 1;
    std::memcpy(key_out, line, klen);
    key_out[klen] = '\0';
    trim(key_out);

    const char* vstart = colon + 1;
    std::strncpy(val_out, vstart, val_sz - 1);
    val_out[val_sz - 1] = '\0';
    trim(val_out);
    strip_quotes(val_out);
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
    PointConfig cur;

    while (std::fgets(line, sizeof(line), f)) {
        char* t = trim(line);
        if (t[0] == '\0' || t[0] == '#') continue;

        // 检测 points: 块开始
        if (std::strncmp(t, "points:", 7) == 0) {
            in_points = true;
            continue;
        }

        if (!in_points) continue;

        // 检测列表项 "- id: 1001" 等
        if (t[0] == '-' && t[1] == ' ') {
            // 保存上一个（如果有）
            if (cur.id != 0 || cur.name[0] != '\0') {
                if (out.count >= out.capacity) {
                    size_t new_cap = out.capacity * 2;
                    auto* np = static_cast<PointConfig*>(
                        std::realloc(out.points, new_cap * sizeof(PointConfig)));
                    if (!np) { std::fclose(f); return false; }
                    out.points = np;
                    out.capacity = new_cap;
                }
                std::memcpy(&out.points[out.count], &cur, sizeof(PointConfig));
                out.count++;
            }
            // 重置当前条目
            std::memset(&cur, 0, sizeof(cur));
            continue;
        }

        // 解析 key: value 属性
        char key[64], val[128];
        if (!parse_kv(t, key, sizeof(key), val, sizeof(val))) continue;

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

    // 保存最后一个条目
    if (cur.id != 0 || cur.name[0] != '\0') {
        if (out.count >= out.capacity) {
            size_t new_cap = out.capacity * 2;
            auto* np = static_cast<PointConfig*>(
                std::realloc(out.points, new_cap * sizeof(PointConfig)));
            if (!np) { std::fclose(f); return false; }
            out.points = np;
            out.capacity = new_cap;
        }
        std::memcpy(&out.points[out.count], &cur, sizeof(PointConfig));
        out.count++;
    }

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
