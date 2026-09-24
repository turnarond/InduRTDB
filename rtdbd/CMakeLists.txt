# rtdbd —— 写权威服务进程（v3.3 读写分离架构 · 控制面 OT）
#
# 仅顶层项目且 Linux 下构建；作为子项目引入时默认关闭（不污染消费方构建）。
cmake_minimum_required(VERSION 3.15)

add_executable(rtdbd
    src/rtdbd.c
    src/policy.c
    src/audit.c
)

target_include_directories(rtdbd PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(rtdbd PRIVATE indurtdb)

# getopt_long / POSIX 扩展
target_compile_definitions(rtdbd PRIVATE _GNU_SOURCE)

set_target_properties(rtdbd PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS ON
)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(rtdbd PRIVATE -Wall -Wextra -Werror)
endif()
