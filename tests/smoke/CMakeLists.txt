# InduRTDB Smoke Tests Configuration
#
# 冒烟测试为独立可执行文件，不依赖 GTest，通过返回码 (0=通过, 非0=失败)
# 报告结果。覆盖 C++ API 生命周期、C ABI、多进程共享内存三条核心链路。

# C++ API 全生命周期冒烟测试
add_executable(smoke_lifecycle smoke_lifecycle.cpp)
target_link_libraries(smoke_lifecycle PRIVATE indurtdb)
target_include_directories(smoke_lifecycle PRIVATE ${PROJECT_SOURCE_DIR}/include)
set_target_properties(smoke_lifecycle PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
add_test(NAME smoke_lifecycle COMMAND smoke_lifecycle)

# C ABI 冒烟测试（需要启用 C 语言）
enable_language(C)
add_executable(smoke_c_api smoke_c_api.c)
target_link_libraries(smoke_c_api PRIVATE indurtdb)
target_include_directories(smoke_c_api PRIVATE ${PROJECT_SOURCE_DIR}/include)
set_target_properties(smoke_c_api PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
)
add_test(NAME smoke_c_api COMMAND smoke_c_api)

# 多进程共享内存冒烟测试
add_executable(smoke_multi_process smoke_multi_process.cpp)
target_link_libraries(smoke_multi_process PRIVATE indurtdb)
target_include_directories(smoke_multi_process PRIVATE ${PROJECT_SOURCE_DIR}/include)
set_target_properties(smoke_multi_process PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
add_test(NAME smoke_multi_process COMMAND smoke_multi_process)

# 输出目录统一到 build/tests/smoke
set_target_properties(smoke_lifecycle smoke_c_api smoke_multi_process PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/smoke"
)

message(STATUS "Smoke tests enabled: smoke_lifecycle, smoke_c_api, smoke_multi_process")
