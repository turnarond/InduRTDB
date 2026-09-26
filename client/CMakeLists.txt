# indurtdb-client —— rtdbd 的 RPC 客户端（含本地写队列）
#
# 与核心库 libindurtdb.a 并列但独立；核心库不感知 RPC。
add_library(irtcli STATIC
    src/client.c
)

target_include_directories(irtcli PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_include_directories(irtcli PRIVATE
    ${PROJECT_SOURCE_DIR}/rtdbd/include
)

set_target_properties(irtcli PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS ON
)

target_compile_definitions(irtcli PRIVATE _GNU_SOURCE)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(irtcli PRIVATE -Wall -Wextra -Werror)
endif()

find_package(Threads QUIET)
if(Threads_FOUND)
    target_link_libraries(irtcli PUBLIC Threads::Threads)
endif()
