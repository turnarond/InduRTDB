# InduRTDB 诊断工具 C harness（T8）：供 irt-diag 的 smoke/leak 命令驱动。
# 仅 Linux（依赖 /dev/shm 与 /proc/self/fd）。
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(irt_diag_harness c_harness/smoke_leak.c)
    target_link_libraries(irt_diag_harness PRIVATE indurtdb pthread rt)
    # v3.7 B5：本 harness 刻意使用 v1 全局 API（诊断工具需覆盖 v1 路径）。
    target_compile_definitions(irt_diag_harness PRIVATE INDURTDB_NO_DEPRECATE_WARN)
    set_target_properties(irt_diag_harness PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED ON)
endif()

# v3.8 D4'：补齐诊断 harness 的安装。
# 注：irt_diag 的 Python 包不经 CMake 安装（它是 pyproject.toml 管理的
# 纯标准库包），从源码树 `pip install tools/irt_diag` 即可。
if(INDURTDB_IS_TOP_LEVEL AND TARGET irt_diag_harness)
    install(TARGETS irt_diag_harness RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/README.md
            DESTINATION ${CMAKE_INSTALL_DOCDIR})
endif()
