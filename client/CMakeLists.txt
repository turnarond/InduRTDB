# indurtdb-client —— rtdbd 的 RPC 客户端（含本地写队列）
#
# 与核心库 libindurtdb.a 并列但独立；核心库不感知 RPC。
add_library(irtcli STATIC
    src/client.c
)

# v3.8 D4'：irtcli 纳入 CMake export set 后，构建树的绝对路径不得出现在
# INTERFACE_INCLUDE_DIRECTORIES 中（CMake 拒绝导出这类目标）。用
# BUILD_INTERFACE / INSTALL_INTERFACE 分隔：构建期行为不变，安装期改用
# 安装后的 include 目录（indurtdb.h 与 client.h 都装在 ${CMAKE_INSTALL_INCLUDEDIR}）。
target_include_directories(irtcli PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
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

# v3.8 D4'：同上，补齐客户端库与其公共头文件的安装。
if(INDURTDB_IS_TOP_LEVEL)
    install(TARGETS irtcli ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(DIRECTORY include/irtcli DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()
