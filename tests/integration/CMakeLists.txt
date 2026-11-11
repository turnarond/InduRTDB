find_package(GTest REQUIRED)

# v3.7 B5：v1 全局 API 已标注 deprecated，但测试**刻意保留 v1 覆盖**
# （v1 到 v4.0 前都是受支持公共 API，测试是其回归网），故关闭弃用告警。
# 对该目录下所有测试目标统一生效。
add_compile_definitions(INDURTDB_NO_DEPRECATE_WARN)

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
    add_executable(test_rtdbd_core test_rtdbd_core.cpp
        ${PROJECT_SOURCE_DIR}/rtdbd/src/subtab.c)
    target_link_libraries(test_rtdbd_core PRIVATE
        GTest::gtest GTest::gtest_main indurtdb)
    target_include_directories(test_rtdbd_core PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/rtdbd/include
        ${PROJECT_SOURCE_DIR}/rtdbd/src)
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

# ---- rtdbd 协议 v3（v3.9 T5：分区/命名空间） ----
if(TARGET rtdbd AND TARGET irtcli)
    add_executable(test_rtdbd_proto_v3 test_rtdbd_proto_v3.cpp)
    target_link_libraries(test_rtdbd_proto_v3 PRIVATE
        GTest::gtest GTest::gtest_main indurtdb irtcli)
    target_include_directories(test_rtdbd_proto_v3 PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/rtdbd/include
        ${PROJECT_SOURCE_DIR}/client/include)
    target_compile_definitions(test_rtdbd_proto_v3 PRIVATE
        _GNU_SOURCE
        RTDBD_BIN="$<TARGET_FILE:rtdbd>")
    set_target_properties(test_rtdbd_proto_v3 PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    add_dependencies(test_rtdbd_proto_v3 rtdbd irtcli)
    add_test(NAME test_rtdbd_proto_v3 COMMAND test_rtdbd_proto_v3)
endif()

# ---- irtcli CLI 分区支持（v3.9 T6） ----
if(TARGET rtdbd AND TARGET irtcli AND TARGET irtcli_cli)
    add_executable(test_irtcli_cli test_irtcli_cli.cpp)
    target_link_libraries(test_irtcli_cli PRIVATE
        GTest::gtest GTest::gtest_main indurtdb irtcli)
    target_include_directories(test_irtcli_cli PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/rtdbd/include
        ${PROJECT_SOURCE_DIR}/client/include)
    target_compile_definitions(test_irtcli_cli PRIVATE
        _GNU_SOURCE
        RTDBD_BIN="$<TARGET_FILE:rtdbd>"
        IRTCLI_BIN="$<TARGET_FILE:irtcli_cli>")
    set_target_properties(test_irtcli_cli PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    add_dependencies(test_irtcli_cli rtdbd irtcli irtcli_cli)
    add_test(NAME test_irtcli_cli COMMAND test_irtcli_cli)
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

# ---- 运行时变更 delta 持久化（v3.7 主题B B4） ----
if(TARGET rtdbd)
    add_executable(test_rtdbd_delta test_rtdbd_delta.cpp)
    target_link_libraries(test_rtdbd_delta PRIVATE
        GTest::gtest GTest::gtest_main indurtdb)
    target_include_directories(test_rtdbd_delta PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/rtdbd/include)
    target_compile_definitions(test_rtdbd_delta PRIVATE
        _GNU_SOURCE
        RTDBD_BIN="$<TARGET_FILE:rtdbd>")
    set_target_properties(test_rtdbd_delta PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    add_dependencies(test_rtdbd_delta rtdbd)
    add_test(NAME test_rtdbd_delta COMMAND test_rtdbd_delta)
endif()

# ---- v3.12 M3.2 ②：UDS 23/24 端到端 ----
if(TARGET rtdbd)
    add_executable(test_rtdbd_peers test_rtdbd_peers.cpp)
    target_link_libraries(test_rtdbd_peers PRIVATE GTest::gtest GTest::gtest_main)
    target_include_directories(test_rtdbd_peers PRIVATE
        ${PROJECT_SOURCE_DIR}/rtdbd/include)
    target_compile_definitions(test_rtdbd_peers PRIVATE
        _GNU_SOURCE
        RTDBD_BIN="$<TARGET_FILE:rtdbd>")
    set_target_properties(test_rtdbd_peers PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    add_dependencies(test_rtdbd_peers rtdbd)
    add_test(NAME test_rtdbd_peers COMMAND test_rtdbd_peers)
endif()

# ---- v3.12 M3.3：成员表进程级单例 + --sync-self-addr 接线 ----
add_executable(test_peers_global test_peers_global.cpp
    ${PROJECT_SOURCE_DIR}/rtdbd/src/cluster/rtdbd_peer_table.c
    ${PROJECT_SOURCE_DIR}/rtdbd/src/cluster/rtdbd_peers_global.c)
target_include_directories(test_peers_global PRIVATE
    ${PROJECT_SOURCE_DIR}/rtdbd/include
    ${PROJECT_SOURCE_DIR}/rtdbd/src
    ${PROJECT_SOURCE_DIR}/rtdbd/src/cluster)
target_link_libraries(test_peers_global PRIVATE GTest::gtest GTest::gtest_main)
set_target_properties(test_peers_global PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME test_peers_global COMMAND test_peers_global)

if(TARGET rtdbd)
    add_executable(test_sync_self_addr test_sync_self_addr.cpp)
    target_include_directories(test_sync_self_addr PRIVATE
        ${PROJECT_SOURCE_DIR}/rtdbd/include)
    target_compile_definitions(test_sync_self_addr PRIVATE
        _GNU_SOURCE RTDBD_BIN="$<TARGET_FILE:rtdbd>")
    target_link_libraries(test_sync_self_addr PRIVATE GTest::gtest GTest::gtest_main)
    set_target_properties(test_sync_self_addr PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    add_dependencies(test_sync_self_addr rtdbd)
    add_test(NAME test_sync_self_addr COMMAND test_sync_self_addr)
endif()

# ---- v3.12 M3.2 ④：irtcli SDK / CLI 成员管控通道（真实 rtdbd 往返）----
if(TARGET rtdbd AND TARGET irtcli AND TARGET irtcli_cli)
    add_executable(test_irtcli_peers test_irtcli_peers.cpp)
    target_link_libraries(test_irtcli_peers PRIVATE
        GTest::gtest GTest::gtest_main irtcli)
    target_include_directories(test_irtcli_peers PRIVATE
        ${PROJECT_SOURCE_DIR}/client/include
        ${PROJECT_SOURCE_DIR}/rtdbd/include)
    target_compile_definitions(test_irtcli_peers PRIVATE
        _GNU_SOURCE
        RTDBD_BIN="$<TARGET_FILE:rtdbd>"
        IRTCLI_CLI_BIN="$<TARGET_FILE:irtcli_cli>")
    set_target_properties(test_irtcli_peers PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    add_dependencies(test_irtcli_peers rtdbd irtcli_cli)
    add_test(NAME test_irtcli_peers COMMAND test_irtcli_peers)
endif()

# ---- v3.12 M3.x / P7：鉴权判据（纯逻辑，默认构建即可运行）----
# 用例名固定 IR2_AuthDeny（§11.1 required_irs 之一，不得改名）。
add_executable(IR2_AuthDeny IR2_AuthDeny.cpp
    ${PROJECT_SOURCE_DIR}/rtdbd/src/session/rtdbd_authz.c
    ${PROJECT_SOURCE_DIR}/rtdbd/src/cluster/rtdbd_peer_table.c)
target_include_directories(IR2_AuthDeny PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/rtdbd/include
    ${PROJECT_SOURCE_DIR}/rtdbd/src
    ${PROJECT_SOURCE_DIR}/rtdbd/src/cluster)
target_link_libraries(IR2_AuthDeny PRIVATE GTest::gtest GTest::gtest_main)
set_target_properties(IR2_AuthDeny PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME IR2_AuthDeny COMMAND IR2_AuthDeny)

# ---- v3.12 M3.2 ③：延迟动作队列循环尾执行器（纯逻辑，默认构建即可运行）----
# 用例名固定 IR3_DeferredQueue（§11.1 required_irs 之一，不得改名）。
add_executable(IR3_DeferredQueue IR3_DeferredQueue.cpp
    ${PROJECT_SOURCE_DIR}/rtdbd/src/session/rtdbd_session.c
    ${PROJECT_SOURCE_DIR}/rtdbd/src/cluster/rtdbd_defer_actions.c
    ${PROJECT_SOURCE_DIR}/rtdbd/src/subtab.c)
target_include_directories(IR3_DeferredQueue PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/rtdbd/include
    ${PROJECT_SOURCE_DIR}/rtdbd/src
    ${PROJECT_SOURCE_DIR}/rtdbd/src/session
    ${PROJECT_SOURCE_DIR}/rtdbd/src/cluster)
target_link_libraries(IR3_DeferredQueue PRIVATE GTest::gtest GTest::gtest_main indurtdb)
set_target_properties(IR3_DeferredQueue PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME IR3_DeferredQueue COMMAND IR3_DeferredQueue)

# ---- v3.12 M3/IR4：成员运行时表语义（纯逻辑，默认构建即可运行）----
# 用例名固定为 IR4_PeersRuntime（§11.1 required_irs 之一，不得改名）。
add_executable(IR4_PeersRuntime IR4_PeersRuntime.cpp
    ${PROJECT_SOURCE_DIR}/rtdbd/src/cluster/rtdbd_peer_table.c)
target_include_directories(IR4_PeersRuntime PRIVATE
    ${PROJECT_SOURCE_DIR}/rtdbd/include
    ${PROJECT_SOURCE_DIR}/rtdbd/src/cluster)
target_link_libraries(IR4_PeersRuntime PRIVATE GTest::gtest GTest::gtest_main)
set_target_properties(IR4_PeersRuntime PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME IR4_PeersRuntime COMMAND IR4_PeersRuntime)

# ---- v3.12 M1.3c：VSOA 端到端联调用例（server 子进程 + client 父进程）----
# 仅在 INDURTDB_ENABLE_VSOA=ON 时注册 —— 干净树默认构建不含此用例，也不引入
# 任何 SDK 依赖（doc 13 §95：干净树默认 OFF 仍可构建）。
if(INDURTDB_ENABLE_VSOA)
    add_executable(test_vsoa_e2e test_vsoa_e2e.cpp
        ${PROJECT_SOURCE_DIR}/rtdbd/src/vsoa/vsoa_adapter.c
        ${PROJECT_SOURCE_DIR}/rtdbd/src/cluster/rtdbd_peer_table.c
        ${PROJECT_SOURCE_DIR}/rtdbd/src/cluster/rtdbd_peers_global.c
        ${PROJECT_SOURCE_DIR}/rtdbd/src/cluster/rtdbd_peers_apply.c
        ${PROJECT_SOURCE_DIR}/rtdbd/src/cluster/rtdbd_defer_global.c
        ${PROJECT_SOURCE_DIR}/rtdbd/src/dispatch/rtdbd_dispatch.c
        ${PROJECT_SOURCE_DIR}/rtdbd/src/session/rtdbd_session.c
        ${PROJECT_SOURCE_DIR}/rtdbd/src/session/rtdbd_authz.c
        ${PROJECT_SOURCE_DIR}/rtdbd/src/subtab.c
        ${PROJECT_SOURCE_DIR}/rtdbd/src/policy.c
        ${PROJECT_SOURCE_DIR}/rtdbd/src/rtdbd_conf.c
        ${PROJECT_SOURCE_DIR}/rtdbd/src/stats.c)
    target_include_directories(test_vsoa_e2e PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/rtdbd/include
        ${PROJECT_SOURCE_DIR}/rtdbd/src
        ${VSOA_SDK_ROOT}/include
        ${VSOA_SDK_ROOT}/include/libvsoa)
    target_link_libraries(test_vsoa_e2e PRIVATE
        GTest::gtest GTest::gtest_main indurtdb
        "-l:libvsoa-server.so"
        "-l:libvsoa-client.so"
        "-l:libvsoa-parser.so"
        "-l:libvsoa-json.so")
    target_link_directories(test_vsoa_e2e PRIVATE ${VSOA_SDK_ROOT}/${VSOA_LIB_SUBDIR})
    # DT_RPATH（--disable-new-dtags）：libvsoa-server.so 依赖 parser/json，
    # DT_RUNPATH 不参与传递依赖查找，会导致运行期 not found。
    target_link_options(test_vsoa_e2e PRIVATE "LINKER:--disable-new-dtags")
    set_target_properties(test_vsoa_e2e PROPERTIES
        BUILD_RPATH "${VSOA_SDK_ROOT}/${VSOA_LIB_SUBDIR}"
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    add_test(NAME test_vsoa_e2e COMMAND test_vsoa_e2e)

    # ---- v3.12 M3.3 方案 A：单线程统一 poll 主循环（真实 rtdbd 双栈可达）----
    add_executable(test_vsoa_main_loop test_vsoa_main_loop.cpp)
    target_include_directories(test_vsoa_main_loop PRIVATE
        ${PROJECT_SOURCE_DIR}/rtdbd/include
        ${VSOA_SDK_ROOT}/include
        ${VSOA_SDK_ROOT}/include/libvsoa)
    target_compile_definitions(test_vsoa_main_loop PRIVATE
        _GNU_SOURCE RTDBD_BIN="$<TARGET_FILE:rtdbd>")
    target_link_libraries(test_vsoa_main_loop PRIVATE
        GTest::gtest GTest::gtest_main
        "-l:libvsoa-server.so"
        "-l:libvsoa-client.so"
        "-l:libvsoa-parser.so"
        "-l:libvsoa-json.so")
    target_link_directories(test_vsoa_main_loop PRIVATE ${VSOA_SDK_ROOT}/${VSOA_LIB_SUBDIR})
    target_link_options(test_vsoa_main_loop PRIVATE "LINKER:--disable-new-dtags")
    set_target_properties(test_vsoa_main_loop PROPERTIES
        BUILD_RPATH "${VSOA_SDK_ROOT}/${VSOA_LIB_SUBDIR}"
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    add_dependencies(test_vsoa_main_loop rtdbd)
    add_test(NAME test_vsoa_main_loop COMMAND test_vsoa_main_loop)

    # ---- v3.12 M3.3 切片 C：VSOA /sync/peers + role=sync_admin ----
    add_executable(test_vsoa_peers_admin test_vsoa_peers_admin.cpp)
    target_include_directories(test_vsoa_peers_admin PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/rtdbd/include
        ${PROJECT_SOURCE_DIR}/rtdbd/src
        ${VSOA_SDK_ROOT}/include
        ${VSOA_SDK_ROOT}/include/libvsoa)
    target_compile_definitions(test_vsoa_peers_admin PRIVATE
        _GNU_SOURCE RTDBD_BIN="$<TARGET_FILE:rtdbd>")
    target_link_libraries(test_vsoa_peers_admin PRIVATE
        GTest::gtest GTest::gtest_main
        "-l:libvsoa-server.so"
        "-l:libvsoa-client.so"
        "-l:libvsoa-parser.so"
        "-l:libvsoa-json.so")
    target_link_directories(test_vsoa_peers_admin PRIVATE ${VSOA_SDK_ROOT}/${VSOA_LIB_SUBDIR})
    target_link_options(test_vsoa_peers_admin PRIVATE "LINKER:--disable-new-dtags")
    set_target_properties(test_vsoa_peers_admin PROPERTIES
        BUILD_RPATH "${VSOA_SDK_ROOT}/${VSOA_LIB_SUBDIR}"
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    add_dependencies(test_vsoa_peers_admin rtdbd)
    add_test(NAME test_vsoa_peers_admin COMMAND test_vsoa_peers_admin)

    # ---- v3.12 M3.x / P7：鉴权端到端（真实 rtdbd + token 表，ON-only）----
    # 与默认构建的 IR2_AuthDeny 纯逻辑判据互补：证明约束真的接进生产路径。
    add_executable(test_vsoa_auth test_vsoa_auth.cpp)
    target_include_directories(test_vsoa_auth PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/rtdbd/include
        ${PROJECT_SOURCE_DIR}/rtdbd/src
        ${VSOA_SDK_ROOT}/include
        ${VSOA_SDK_ROOT}/include/libvsoa)
    target_compile_definitions(test_vsoa_auth PRIVATE
        _GNU_SOURCE RTDBD_BIN="$<TARGET_FILE:rtdbd>")
    target_link_libraries(test_vsoa_auth PRIVATE
        GTest::gtest GTest::gtest_main
        "-l:libvsoa-server.so"
        "-l:libvsoa-client.so"
        "-l:libvsoa-parser.so"
        "-l:libvsoa-json.so")
    target_link_directories(test_vsoa_auth PRIVATE ${VSOA_SDK_ROOT}/${VSOA_LIB_SUBDIR})
    target_link_options(test_vsoa_auth PRIVATE "LINKER:--disable-new-dtags")
    set_target_properties(test_vsoa_auth PROPERTIES
        BUILD_RPATH "${VSOA_SDK_ROOT}/${VSOA_LIB_SUBDIR}"
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    add_dependencies(test_vsoa_auth rtdbd)
    add_test(NAME test_vsoa_auth COMMAND test_vsoa_auth)

    # ---- v3.12 M2：6 节点编排 + 故障注入 harness（IR1–IR7 的基础设施）----
    add_executable(test_vsoa_cluster test_vsoa_cluster.cpp
        vsoa_cluster_harness.cpp
        ${PROJECT_SOURCE_DIR}/rtdbd/src/vsoa/vsoa_adapter.c
        ${PROJECT_SOURCE_DIR}/rtdbd/src/cluster/rtdbd_peer_table.c
        ${PROJECT_SOURCE_DIR}/rtdbd/src/cluster/rtdbd_peers_global.c
        ${PROJECT_SOURCE_DIR}/rtdbd/src/cluster/rtdbd_peers_apply.c
        ${PROJECT_SOURCE_DIR}/rtdbd/src/cluster/rtdbd_defer_global.c
        ${PROJECT_SOURCE_DIR}/rtdbd/src/dispatch/rtdbd_dispatch.c
        ${PROJECT_SOURCE_DIR}/rtdbd/src/session/rtdbd_session.c
        ${PROJECT_SOURCE_DIR}/rtdbd/src/session/rtdbd_authz.c
        ${PROJECT_SOURCE_DIR}/rtdbd/src/subtab.c
        ${PROJECT_SOURCE_DIR}/rtdbd/src/policy.c
        ${PROJECT_SOURCE_DIR}/rtdbd/src/rtdbd_conf.c
        ${PROJECT_SOURCE_DIR}/rtdbd/src/stats.c)
    target_include_directories(test_vsoa_cluster PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/rtdbd/include
        ${PROJECT_SOURCE_DIR}/rtdbd/src
        ${VSOA_SDK_ROOT}/include
        ${VSOA_SDK_ROOT}/include/libvsoa)
    target_link_libraries(test_vsoa_cluster PRIVATE
        GTest::gtest GTest::gtest_main indurtdb
        "-l:libvsoa-server.so"
        "-l:libvsoa-client.so"
        "-l:libvsoa-parser.so"
        "-l:libvsoa-json.so")
    target_link_directories(test_vsoa_cluster PRIVATE ${VSOA_SDK_ROOT}/${VSOA_LIB_SUBDIR})
    target_link_options(test_vsoa_cluster PRIVATE "LINKER:--disable-new-dtags")
    set_target_properties(test_vsoa_cluster PROPERTIES
        BUILD_RPATH "${VSOA_SDK_ROOT}/${VSOA_LIB_SUBDIR}"
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    add_test(NAME test_vsoa_cluster COMMAND test_vsoa_cluster)
endif()
