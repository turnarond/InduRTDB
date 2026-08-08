# Unit tests configuration
# 每个 test_xxx.cpp 独立 target, 链接 indurtdb 静态库 + GTest
# 编码规范要求: 每个 src/*.cpp 必须有对应的 test_xxx.cpp

set(UNIT_TEST_SOURCES
    test_alignment
    test_basic_types
    test_error
    test_logging
    test_memory_layout
    test_seqlock
    test_subscription_manager
    test_version
)

foreach(TEST_NAME IN LISTS UNIT_TEST_SOURCES)
    add_executable(${TEST_NAME} ${TEST_NAME}.cpp)
    target_link_libraries(${TEST_NAME}
        PRIVATE
            indurtdb
            GTest::gtest
            GTest::gtest_main
    )
    target_include_directories(${TEST_NAME}
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )
    set_target_properties(${TEST_NAME} PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
    )
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endforeach()
