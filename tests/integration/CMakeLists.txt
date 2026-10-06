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

# ---- 端到端混合角色（v3.3 T6） ----
if(TARGET rtdbd AND TARGET irtcli)
    add_executable(test_e2e_mixed test_e2e_mixed.cpp)
    target_link_libraries(test_e2e_mixed PRIVATE
        GTest::gtest GTest::gtest_main indurtdb irtcli)
    target_include_directories(test_e2e_mixed PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/rtdbd/include
        ${PROJECT_SOURCE_DIR}/client/include)
    target_compile_definitions(test_e2e_mixed PRIVATE
        _GNU_SOURCE
        RTDBD_BIN="$<TARGET_FILE:rtdbd>")
    set_target_properties(test_e2e_mixed PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    add_dependencies(test_e2e_mixed rtdbd irtcli)
    add_test(NAME test_e2e_mixed COMMAND test_e2e_mixed)
endif()

# ---- rtdbd / irtcli 协议 v2（v3.4 T9） ----
if(TARGET rtdbd AND TARGET irtcli)
    add_executable(test_rtdbd_proto_v2 test_rtdbd_proto_v2.cpp)
    target_link_libraries(test_rtdbd_proto_v2 PRIVATE
        GTest::gtest GTest::gtest_main indurtdb irtcli)
    target_include_directories(test_rtdbd_proto_v2 PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/rtdbd/include
        ${PROJECT_SOURCE_DIR}/client/include)
    target_compile_definitions(test_rtdbd_proto_v2 PRIVATE
        _GNU_SOURCE
        RTDBD_BIN="$<TARGET_FILE:rtdbd>")
    set_target_properties(test_rtdbd_proto_v2 PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    add_dependencies(test_rtdbd_proto_v2 rtdbd irtcli)
    add_test(NAME test_rtdbd_proto_v2 COMMAND test_rtdbd_proto_v2)
endif()

# ---- rtdbd 健康 / 错误计数 + 自检（v3.7 主题B B1） ----
if(TARGET rtdbd)
    add_executable(test_rtdbd_health test_rtdbd_health.cpp)
    target_link_libraries(test_rtdbd_health PRIVATE
        GTest::gtest GTest::gtest_main indurtdb)
    target_include_directories(test_rtdbd_health PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/rtdbd/include)
    target_compile_definitions(test_rtdbd_health PRIVATE
        _GNU_SOURCE
        RTDBD_BIN="$<TARGET_FILE:rtdbd>")
    set_target_properties(test_rtdbd_health PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    add_dependencies(test_rtdbd_health rtdbd)
    add_test(NAME test_rtdbd_health COMMAND test_rtdbd_health)
endif()

# ---- rtdbd 配置校验 + fail-fast（v3.7 主题B B3） ----
if(TARGET rtdbd)
    add_executable(test_rtdbd_config test_rtdbd_config.cpp)
    target_link_libraries(test_rtdbd_config PRIVATE
        GTest::gtest GTest::gtest_main indurtdb)
    target_include_directories(test_rtdbd_config PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/rtdbd/include)
    target_compile_definitions(test_rtdbd_config PRIVATE
        _GNU_SOURCE
        RTDBD_BIN="$<TARGET_FILE:rtdbd>")
    set_target_properties(test_rtdbd_config PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    add_dependencies(test_rtdbd_config rtdbd)
    add_test(NAME test_rtdbd_config COMMAND test_rtdbd_config)
endif()

# ---- rtdbd 背压 + 优雅退出（v3.7 主题B B2） ----
if(TARGET rtdbd)
    add_executable(test_rtdbd_backpressure test_rtdbd_backpressure.cpp)
    target_link_libraries(test_rtdbd_backpressure PRIVATE
        GTest::gtest GTest::gtest_main indurtdb)
    target_include_directories(test_rtdbd_backpressure PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/rtdbd/include)
    target_compile_definitions(test_rtdbd_backpressure PRIVATE
        _GNU_SOURCE
        RTDBD_BIN="$<TARGET_FILE:rtdbd>")
    set_target_properties(test_rtdbd_backpressure PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    add_dependencies(test_rtdbd_backpressure rtdbd)
    add_test(NAME test_rtdbd_backpressure COMMAND test_rtdbd_backpressure)
endif()
