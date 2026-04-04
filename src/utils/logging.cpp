/**
 * @file logging.cpp
 * @brief Logging system implementation
 * @version 1.0.0
 * @date 2026-03-27
 * 
 * @copyright MIT License
 */

#include <cstdio>
#include <cstdarg>
#include <ctime>
#include <cstring>

namespace indurtdb {
namespace utils {

// 日志级别
enum class LogLevel {
    DEBUG,
    INFO,
    WARNING,
    ERROR,
    FATAL
};

// 日志配置
struct LogConfig {
    LogLevel min_level = LogLevel::INFO;
    bool enable_timestamp = true;
    bool enable_level = true;
    bool enable_file_line = false;
    const char* output_file = nullptr;
};

// 全局日志配置
static LogConfig g_log_config;

// 日志级别字符串
static const char* level_strings[] = {
    "DEBUG",
    "INFO",
    "WARNING",
    "ERROR",
    "FATAL"
};

// 内部日志函数
static void internal_log(LogLevel level,
                        const char* file,
                        int line,
                        const char* format,
                        va_list args) {
    
    // 检查日志级别
    if (level < g_log_config.min_level) {
        return;
    }
    
    // 获取时间
    char time_buf[64];
    if (g_log_config.enable_timestamp) {
        time_t now = time(nullptr);
        struct tm tm_info;
        localtime_r(&now, &tm_info);
        strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_info);
    }
    
    // 打开输出文件
    FILE* output = stdout;
    if (g_log_config.output_file) {
        output = fopen(g_log_config.output_file, "a");
        if (!output) {
            output = stdout;
        }
    }
    
    // 输出时间戳
    if (g_log_config.enable_timestamp) {
        fprintf(output, "[%s] ", time_buf);
    }
    
    // 输出日志级别
    if (g_log_config.enable_level) {
        fprintf(output, "[%s] ", level_strings[static_cast<int>(level)]);
    }
    
    // 输出文件和行号
    if (g_log_config.enable_file_line && file) {
        const char* basename = strrchr(file, '/');
        if (basename) {
            file = basename + 1;
        }
        fprintf(output, "[%s:%d] ", file, line);
    }
    
    // 输出日志消息
    vfprintf(output, format, args);
    fprintf(output, "\n");
    
    // 刷新输出
    fflush(output);
    
    // 关闭文件（如果是文件输出）
    if (output != stdout) {
        fclose(output);
    }
}

// 公共日志函数
void log_debug(const char* file, int line, const char* format, ...) {
#ifndef INDURTDB_DISABLE_LOG
    va_list args;
    va_start(args, format);
    internal_log(LogLevel::DEBUG, file, line, format, args);
    va_end(args);
#endif
}

void log_info(const char* file, int line, const char* format, ...) {
#ifndef INDURTDB_DISABLE_LOG
    va_list args;
    va_start(args, format);
    internal_log(LogLevel::INFO, file, line, format, args);
    va_end(args);
#endif
}

void log_warning(const char* file, int line, const char* format, ...) {
#ifndef INDURTDB_DISABLE_LOG
    va_list args;
    va_start(args, format);
    internal_log(LogLevel::WARNING, file, line, format, args);
    va_end(args);
#endif
}

void log_error(const char* file, int line, const char* format, ...) {
#ifndef INDURTDB_DISABLE_LOG
    va_list args;
    va_start(args, format);
    internal_log(LogLevel::ERROR, file, line, format, args);
    va_end(args);
#endif
}

void log_fatal(const char* file, int line, const char* format, ...) {
#ifndef INDURTDB_DISABLE_LOG
    va_list args;
    va_start(args, format);
    internal_log(LogLevel::FATAL, file, line, format, args);
    va_end(args);
#endif
}

// 配置函数
void log_set_config(const LogConfig& config) {
    g_log_config = config;
}

LogConfig log_get_config() {
    return g_log_config;
}

void log_set_min_level(LogLevel level) {
    g_log_config.min_level = level;
}

void log_enable_timestamp(bool enable) {
    g_log_config.enable_timestamp = enable;
}

void log_enable_level(bool enable) {
    g_log_config.enable_level = enable;
}

void log_enable_file_line(bool enable) {
    g_log_config.enable_file_line = enable;
}

void log_set_output_file(const char* filename) {
    g_log_config.output_file = filename;
}

} // namespace utils
} // namespace indurtdb