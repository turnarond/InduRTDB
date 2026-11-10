# rtdbd —— 写权威服务进程（v3.3 读写分离架构 · 控制面 OT）
#
# 仅顶层项目且 Linux 下构建；作为子项目引入时默认关闭（不污染消费方构建）。
cmake_minimum_required(VERSION 3.15)

add_executable(rtdbd
    src/rtdbd.c
    src/rtdbd_conf.c
    src/policy.c
    src/audit.c
    src/logbuf.c
    src/subtab.c
    src/dispatch/rtdbd_dispatch.c
    src/cluster/rtdbd_peer_table.c
    src/cluster/rtdbd_peers_global.c
    src/cluster/rtdbd_defer_global.c
    src/cluster/rtdbd_peers_apply.c
    src/cluster/rtdbd_peers_file.c
    src/cluster/rtdbd_defer_actions.c
    src/session/rtdbd_session.c
    src/stats.c
)

target_include_directories(rtdbd PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(rtdbd PRIVATE indurtdb)

# --- VSOA 双栈前端（v3.12 M1.3）---
# 仅在顶层显式开启且 SDK 探测通过时接入（探测在顶层 CMakeLists.txt 完成，
# 缺项/版本不符一律 FATAL_ERROR，绝不静默降级）。
# 头文件用**裸名**（vsoa_server.h 等）引入，与 scripts/check_arch_layers.py 的
# vsoa ring 规则一致：只有 server 层（rtdbd/、client/）可依赖 vsoa。
if(INDURTDB_ENABLE_VSOA)
    # VSOA 适配层（唯一引用 vsoa_* 的文件）；transport 无关 dispatch 纯模块已在
    # add_executable 中无条件编入（UDS 侧同样使用，见 op_caps 迁移）。
    # dispatch 层保持零 vsoa 符号（check_dispatch_boundary），vsoa 层单向依赖它。
    target_sources(rtdbd PRIVATE
        src/vsoa/vsoa_adapter.c)
    target_include_directories(rtdbd PRIVATE
        ${VSOA_SDK_ROOT}/include
        ${VSOA_SDK_ROOT}/include/libvsoa)
    target_link_directories(rtdbd PRIVATE ${VSOA_SDK_ROOT}/${VSOA_LIB_SUBDIR})
    # 用**明确文件名**形式（-l:libX.so）：避免 --as-needed 把「当前未直接引用符号」
    # 的 SDK 库丢弃（adapter 只用 server API，client/parser/json 会被误丢）。
    target_link_libraries(rtdbd PRIVATE
        "-l:libvsoa-server.so"
        "-l:libvsoa-client.so"
        "-l:libvsoa-parser.so"
        "-l:libvsoa-json.so")
    # 运行期可定位 SDK 动态库（无 LD_LIBRARY_PATH 也能起服务）。
    # 必须用 DT_RPATH 而非 DT_RUNPATH：libvsoa-server.so 自身依赖 parser/json，
    # DT_RUNPATH 不参与**传递依赖**查找，会导致 ldd 报 "not found"。
    target_link_options(rtdbd PRIVATE "LINKER:--disable-new-dtags")
    set_target_properties(rtdbd PROPERTIES
        BUILD_RPATH "${VSOA_SDK_ROOT}/${VSOA_LIB_SUBDIR}")
    target_compile_definitions(rtdbd PRIVATE INDURTDB_ENABLE_VSOA=1)
endif()

# getopt_long / POSIX 扩展
target_compile_definitions(rtdbd PRIVATE _GNU_SOURCE)

# v3.7 B5：rtdbd 是 v1 全局 API 的消费方（整体按单例风格编写，数百处调用）。
# v1 到 v4.0 前仍受支持且行为不变，机械改写为零行为收益却担全量 churn 风险，
# 故标注为"刻意使用 v1"，统一迁移推迟到 v4.0 真正移除 v1 时。
target_compile_definitions(rtdbd PRIVATE INDURTDB_NO_DEPRECATE_WARN)

set_target_properties(rtdbd PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS ON
)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(rtdbd PRIVATE -Wall -Wextra -Werror)
endif()

# v3.8 D4'：此前 cmake --install 只安装库本体，rtdbd 留在构建目录里，
# 部署方须手动拷贝。补上安装规则。
# 仅顶层项目安装；作为子项目时不污染消费方的安装树。
if(INDURTDB_IS_TOP_LEVEL)
    install(TARGETS rtdbd RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
