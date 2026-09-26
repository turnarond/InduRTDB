find_package(GTest REQUIRED)

add_executable(test_c_multi_process test_c_multi_process.cpp)
target_link_libraries(test_c_multi_process PRIVATE
    GTest::gtest GTest::gtest_main indurtdb)
target_include_directories(test_c_multi_process PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(test_c_multi_process PRIVATE _GNU_SOURCE)
set_target_properties(test_c_multi_process PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME test_c_multi_process COMMAND test_c_multi_process)

# ---- rtdbd 骨架集成测试（v3.3 T1） ----
if(TARGET rtdbd)
    add_executable(test_rtdbd_core test_rtdbd_core.cpp)
    target_link_libraries(test_rtdbd_core PRIVATE
        GTest::gtest GTest::gtest_main indurtdb)
    target_include_directories(test_rtdbd_core PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/rtdbd/include)
    target_compile_definitions(test_rtdbd_core PRIVATE
        _GNU_SOURCE
        RTDBD_BIN="$<TARGET_FILE:rtdbd>")
    set_target_properties(test_rtdbd_core PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    add_dependencies(test_rtdbd_core rtdbd)
    add_test(NAME test_rtdbd_core COMMAND test_rtdbd_core)
endif()

# ---- rtdbd 快速自恢复（v3.3 T2） ----
if(TARGET rtdbd)
    add_executable(test_rtdbd_recovery test_rtdbd_recovery.cpp)
    target_link_libraries(test_rtdbd_recovery PRIVATE
        GTest::gtest GTest::gtest_main indurtdb)
    target_include_directories(test_rtdbd_recovery PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/rtdbd/include)
    target_compile_definitions(test_rtdbd_recovery PRIVATE
        _GNU_SOURCE
        RTDBD_BIN="$<TARGET_FILE:rtdbd>")
    set_target_properties(test_rtdbd_recovery PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    add_dependencies(test_rtdbd_recovery rtdbd)
    add_test(NAME test_rtdbd_recovery COMMAND test_rtdbd_recovery)
endif()

# ---- rtdbd 变更通知（v3.3 T4） ----
if(TARGET rtdbd)
    add_executable(test_rtdbd_notify test_rtdbd_notify.cpp)
    target_link_libraries(test_rtdbd_notify PRIVATE
        GTest::gtest GTest::gtest_main indurtdb)
    target_include_directories(test_rtdbd_notify PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/rtdbd/include)
    target_compile_definitions(test_rtdbd_notify PRIVATE
        _GNU_SOURCE
        RTDBD_BIN="$<TARGET_FILE:rtdbd>")
    set_target_properties(test_rtdbd_notify PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    add_dependencies(test_rtdbd_notify rtdbd)
    add_test(NAME test_rtdbd_notify COMMAND test_rtdbd_notify)
endif()

# ---- indurtdb-client 与写队列集成测试（v3.3 T3） ----
if(TARGET rtdbd AND TARGET irtcli)
    add_executable(test_irtcli_queue test_irtcli_queue.cpp)
    target_link_libraries(test_irtcli_queue PRIVATE
        GTest::gtest GTest::gtest_main indurtdb irtcli)
    target_include_directories(test_irtcli_queue PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/rtdbd/include
        ${PROJECT_SOURCE_DIR}/client/include)
    target_compile_definitions(test_irtcli_queue PRIVATE
        _GNU_SOURCE
        RTDBD_BIN="$<TARGET_FILE:rtdbd>")
    set_target_properties(test_irtcli_queue PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    add_dependencies(test_irtcli_queue rtdbd irtcli)
    add_test(NAME test_irtcli_queue COMMAND test_irtcli_queue)
endif()
