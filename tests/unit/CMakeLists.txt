find_package(GTest REQUIRED)

# 每个 test_*.cpp 一个可执行文件
file(GLOB UNIT_TEST_SOURCES "test_c_*.cpp")
foreach(src ${UNIT_TEST_SOURCES})
    get_filename_component(name ${src} NAME_WE)
    add_executable(${name} ${src})
    target_link_libraries(${name} PRIVATE
        GTest::gtest GTest::gtest_main indurtdb)
    target_include_directories(${name} PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/src)
    # v3.7 B5：v1 全局 API 已标注 deprecated，但测试**刻意保留 v1 覆盖**——
    # v1 到 v4.0 前都是受支持的公共 API，测试正是它的回归网。故关闭弃用告警，
    # 而非把约 500 处调用机械改写成 v2（零行为收益却担全量 churn 风险）。
    target_compile_definitions(${name} PRIVATE INDURTDB_NO_DEPRECATE_WARN)
    set_target_properties(${name} PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    add_test(NAME ${name} COMMAND ${name})
endforeach()

# v3.9 T3：策略引擎单测直接编译 rtdbd/src/policy.c（纯叶子模块，仅依赖 libc）。
# 该模块不在 indurtdb 库内，故单独挂源文件与包含目录。
if(TARGET test_c_policy)
    target_sources(test_c_policy PRIVATE
        ${PROJECT_SOURCE_DIR}/rtdbd/src/policy.c)
    target_include_directories(test_c_policy PRIVATE
        ${PROJECT_SOURCE_DIR}/rtdbd/src)
endif()

# 版本一致性：期望值由 CMake project(VERSION) 注入，测试端不得硬编码，
# 否则每次发布都会漂移（v3.3.0 发布时实测踩到）。
if(TARGET test_c_version)
    target_compile_definitions(test_c_version PRIVATE
        INDURTDB_EXPECT_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        INDURTDB_EXPECT_VERSION_MINOR=${PROJECT_VERSION_MINOR}
        INDURTDB_EXPECT_VERSION_PATCH=${PROJECT_VERSION_PATCH}
        INDURTDB_EXPECT_VERSION_STRING="${PROJECT_VERSION}")
endif()

# T5 迁移工具集成测试：告知测试脚本所在目录（驱动 Python 工具）
if(TARGET test_c_migration)
    target_compile_definitions(test_c_migration PRIVATE
        INDURTDB_TOOLS_DIR="${PROJECT_SOURCE_DIR}/tools")
endif()

# T8 B1 跨语言镜像一致性：Python 布局/协议复刻 vs C 头文件。
# 需要 rtdbd/include（protocol.h）与 tools 目录（import 真实 Python 模块）。
if(TARGET test_c_mirror)
    target_include_directories(test_c_mirror PRIVATE
        ${PROJECT_SOURCE_DIR}/rtdbd/include)
    target_compile_definitions(test_c_mirror PRIVATE
        INDURTDB_TOOLS_DIR="${PROJECT_SOURCE_DIR}/tools")
endif()

# T8 诊断工具集成测试：告知工具目录、harness 路径与源码根
if(TARGET test_c_diag)
    target_compile_definitions(test_c_diag PRIVATE
        INDURTDB_TOOLS_DIR="${PROJECT_SOURCE_DIR}/tools"
        INDURTDB_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    if(TARGET irt_diag_harness)
        target_compile_definitions(test_c_diag PRIVATE
            INDURTDB_DIAG_HARNESS="$<TARGET_FILE:irt_diag_harness>")
    endif()
endif()

# ============================================================================
# v3.9 T4：API 计数门禁（CI 首步同款检查，纳入 ctest）。
# 头文件公开 API 计数与 docs/05-SDK手册/03-C-API参考手册.md 首段及
# scripts/check_api_count.py 的 EXPECTED_* 必须一致；改 API 时三处同 commit 更新。
# 未找到 Python 解释器时不注册为失败用例（禁用而非误报红），避免无 Python 环境
# 下 ctest 整体变红。
# ============================================================================
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME Gate.ApiCountMatches
             COMMAND ${Python3_EXECUTABLE}
                     ${PROJECT_SOURCE_DIR}/scripts/check_api_count.py)
else()
    add_test(NAME Gate.ApiCountMatches
             COMMAND ${CMAKE_COMMAND} -E echo "Python3 not found: Gate.ApiCountMatches skipped")
    set_tests_properties(Gate.ApiCountMatches PROPERTIES DISABLED TRUE)
endif()
