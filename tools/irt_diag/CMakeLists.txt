# InduRTDB 诊断工具 C harness（T8）：供 irt-diag 的 smoke/leak 命令驱动。
# 仅 Linux（依赖 /dev/shm 与 /proc/self/fd）。
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(irt_diag_harness c_harness/smoke_leak.c)
    target_link_libraries(irt_diag_harness PRIVATE indurtdb pthread rt)
    set_target_properties(irt_diag_harness PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED ON)
endif()
