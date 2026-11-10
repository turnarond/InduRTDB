add_executable(basic_example basic_example.c)
target_link_libraries(basic_example PRIVATE indurtdb)
set_target_properties(basic_example PROPERTIES C_STANDARD 11)

# peek 生命周期示例：演示单拷贝陷阱与正确用法（U2）
add_executable(peek_lifetime_example peek_lifetime_example.c)
target_link_libraries(peek_lifetime_example PRIVATE indurtdb)
set_target_properties(peek_lifetime_example PROPERTIES C_STANDARD 11)

# v3.12 M3.2 ⑤：集群成员管控可运行示例（irtcli SDK + 自包含启 rtdbd）
add_executable(peers_example peers_example.c)
target_link_libraries(peers_example PRIVATE irtcli)
target_compile_definitions(peers_example PRIVATE
    _GNU_SOURCE
    RTDBD_BIN="$<TARGET_FILE:rtdbd>")
set_target_properties(peers_example PROPERTIES C_STANDARD 11)
add_dependencies(peers_example rtdbd)
# 示例「可直接运行」注册为 ctest 判据，防止示例随协议演进腐烂
if(INDURTDB_BUILD_TESTS)
    add_test(NAME peers_example COMMAND peers_example)
endif()

# v3.12 M3.3 方案 A：UDS/VSOA 双栈可运行示例（仅 VSOA ON 构建）
if(INDURTDB_ENABLE_VSOA AND TARGET rtdbd)
    add_executable(vsoa_dualstack_example vsoa_dualstack_example.c)
    target_include_directories(vsoa_dualstack_example PRIVATE
        ${PROJECT_SOURCE_DIR}/rtdbd/include
        ${VSOA_SDK_ROOT}/include
        ${VSOA_SDK_ROOT}/include/libvsoa)
    target_compile_definitions(vsoa_dualstack_example PRIVATE
        _GNU_SOURCE RTDBD_BIN="$<TARGET_FILE:rtdbd>")
    target_link_libraries(vsoa_dualstack_example PRIVATE
        "-l:libvsoa-server.so"
        "-l:libvsoa-client.so"
        "-l:libvsoa-parser.so"
        "-l:libvsoa-json.so")
    target_link_directories(vsoa_dualstack_example PRIVATE ${VSOA_SDK_ROOT}/${VSOA_LIB_SUBDIR})
    target_link_options(vsoa_dualstack_example PRIVATE "LINKER:--disable-new-dtags")
    set_target_properties(vsoa_dualstack_example PROPERTIES
        BUILD_RPATH "${VSOA_SDK_ROOT}/${VSOA_LIB_SUBDIR}"
        C_STANDARD 11 C_STANDARD_REQUIRED ON)
    add_dependencies(vsoa_dualstack_example rtdbd)
    if(INDURTDB_BUILD_TESTS)
        add_test(NAME vsoa_dualstack_example COMMAND vsoa_dualstack_example)
    endif()
endif()
