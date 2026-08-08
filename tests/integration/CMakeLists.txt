# Integration tests configuration
# 多进程集成测试: fork 子进程验证共享内存跨进程读写

add_executable(test_multi_process test_multi_process.cpp)
target_link_libraries(test_multi_process
    PRIVATE
        indurtdb
        GTest::gtest
        GTest::gtest_main
)
target_include_directories(test_multi_process
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)
set_target_properties(test_multi_process PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
add_test(NAME test_multi_process COMMAND test_multi_process)
