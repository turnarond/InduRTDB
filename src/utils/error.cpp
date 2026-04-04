/**
 * @file error.cpp
 * @brief Error handling implementation
 * @version 1.0.0
 * @date 2026-03-27
 * 
 * @copyright MIT License
 */

#include <cstring>
#include <cerrno>

namespace indurtdb {
namespace utils {

// 错误码定义
enum class ErrorCode {
    SUCCESS = 0,
    INVALID_ARGUMENT,
    OUT_OF_RANGE,
    IO_ERROR,
    MEMORY_ERROR,
    TIMEOUT,
    NOT_INITIALIZED,
    ALREADY_INITIALIZED,
    NOT_FOUND,
    ALREADY_EXISTS,
    PERMISSION_DENIED,
    NOT_SUPPORTED,
    INTERNAL_ERROR,
    UNKNOWN_ERROR
};

// 错误信息
struct ErrorInfo {
    ErrorCode code;
    const char* message;
    int system_error; // 系统错误码（如errno）
};

// 线程局部错误信息
static thread_local ErrorInfo g_last_error = {
    ErrorCode::SUCCESS,
    "Success",
    0
};

// 错误码到字符串的映射
static const char* error_strings[] = {
    "Success",
    "Invalid argument",
    "Out of range",
    "I/O error",
    "Memory error",
    "Timeout",
    "Not initialized",
    "Already initialized",
    "Not found",
    "Already exists",
    "Permission denied",
    "Not supported",
    "Internal error",
    "Unknown error"
};

// 设置错误信息
void set_error(ErrorCode code, const char* message, int system_error) {
    g_last_error.code = code;
    g_last_error.message = message ? message : error_strings[static_cast<int>(code)];
    g_last_error.system_error = system_error;
}

// 设置系统错误
void set_system_error(ErrorCode code, int system_error) {
    g_last_error.code = code;
    g_last_error.message = strerror(system_error);
    g_last_error.system_error = system_error;
}

// 清除错误
void clear_error() {
    g_last_error.code = ErrorCode::SUCCESS;
    g_last_error.message = "Success";
    g_last_error.system_error = 0;
}

// 获取最后错误码
ErrorCode get_last_error_code() {
    return g_last_error.code;
}

// 获取最后错误信息
const char* get_last_error_message() {
    return g_last_error.message;
}

// 获取系统错误码
int get_last_system_error() {
    return g_last_error.system_error;
}

// 获取完整错误信息
void get_last_error(ErrorCode* code, const char** message, int* system_error) {
    if (code) {
        *code = g_last_error.code;
    }
    if (message) {
        *message = g_last_error.message;
    }
    if (system_error) {
        *system_error = g_last_error.system_error;
    }
}

// 检查是否成功
bool is_success() {
    return g_last_error.code == ErrorCode::SUCCESS;
}

// 检查是否失败
bool is_failure() {
    return g_last_error.code != ErrorCode::SUCCESS;
}

// 错误码转字符串
const char* error_code_to_string(ErrorCode code) {
    int index = static_cast<int>(code);
    if (index >= 0 && index < static_cast<int>(sizeof(error_strings) / sizeof(error_strings[0]))) {
        return error_strings[index];
    }
    return "Unknown error code";
}

// 系统错误码转字符串
const char* system_error_to_string(int error_code) {
    return strerror(error_code);
}

} // namespace utils
} // namespace indurtdb