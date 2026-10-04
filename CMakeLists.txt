# InduRTDB - Industrial Real-Time Database (pure C core)
cmake_minimum_required(VERSION 3.15)
project(InduRTDB VERSION 3.4.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)              # gnu11

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type (Debug/Release)" FORCE)
endif()

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Werror")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -ffunction-sections -fdata-sections")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -D_GNU_SOURCE -DINDURTDB_OS_LINUX")
    set(IRT_OSAL_SRC src/osal/posix/irt_osal_posix.c)
elseif(CMAKE_SYSTEM_NAME STREQUAL "SylixOS")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DINDURTDB_OS_SYLIXOS")
    set(IRT_OSAL_SRC src/osal/sylixos/irt_osal_sylixos.c)
else()
    message(FATAL_ERROR "Unsupported platform: ${CMAKE_SYSTEM_NAME}")
endif()

set(IRT_CORE_SRC
    src/core/irt_shm.c
    src/core/irt_point_manager.c
    src/core/irt_subscription.c
    src/core/irt_config.c
    src/core/irt_index.c
    src/api/indurtdb.c
    ${IRT_OSAL_SRC}
)

# --- 顶层检测 (子项目隔离) ---
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(INDURTDB_IS_TOP_LEVEL TRUE)
else()
    set(INDURTDB_IS_TOP_LEVEL FALSE)
endif()
if(INDURTDB_IS_TOP_LEVEL)
    set(_indurtdb_default ON)
else()
    set(_indurtdb_default OFF)
endif()
option(INDURTDB_BUILD_TESTS    "Build InduRTDB tests"    ${_indurtdb_default})
option(INDURTDB_BUILD_EXAMPLES "Build InduRTDB examples" ${_indurtdb_default})

# --- 覆盖率插桩（T8） ---
# 开启后库以 --coverage 构建，配合 `coverage` 目标生成报告（需 gcovr/lcov）。
option(INDURTDB_ENABLE_COVERAGE "Build with --coverage instrumentation" OFF)
if(INDURTDB_ENABLE_COVERAGE)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} --coverage -O0 -g")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} --coverage")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} --coverage")
endif()

include(GNUInstallDirs)

add_library(indurtdb STATIC ${IRT_CORE_SRC})
add_library(InduRTDB::indurtdb ALIAS indurtdb)
target_include_directories(indurtdb PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(indurtdb PUBLIC rt pthread)
endif()

# --- rtdbd 写权威服务进程（v3.3 读写分离） ---
# 仅 Linux；作为子项目引入时默认关闭，不污染消费方构建。
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(INDURTDB_IS_TOP_LEVEL)
        set(_rtdbd_default ON)
    else()
        set(_rtdbd_default OFF)
    endif()
    option(INDURTDB_BUILD_RTDBD "Build rtdbd write-authority daemon" ${_rtdbd_default})
    if(INDURTDB_BUILD_RTDBD)
        add_subdirectory(rtdbd)
    endif()
endif()

# --- indurtdb-client（rtdbd 的 RPC 客户端，含本地写队列） ---
# 同样仅 Linux；作为子项目引入时默认关闭。
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(INDURTDB_IS_TOP_LEVEL)
        set(_client_default ON)
    else()
        set(_client_default OFF)
    endif()
    option(INDURTDB_BUILD_CLIENT "Build indurtdb-client (RPC client with write queue)" ${_client_default})
    if(INDURTDB_BUILD_CLIENT)
        add_subdirectory(client)
    endif()
endif()

# --- 诊断工具 C harness（T8：供 irt-diag 的 smoke/leak 驱动） ---
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(INDURTDB_IS_TOP_LEVEL)
        set(_diag_default ON)
    else()
        set(_diag_default OFF)
    endif()
    option(INDURTDB_BUILD_DIAG "Build irt_diag harness (diagnostics tooling)" ${_diag_default})
    if(INDURTDB_BUILD_DIAG)
        add_subdirectory(tools/irt_diag)
    endif()
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(INDURTDB_BUILD_TESTS)
        enable_testing()
        add_subdirectory(tests)
    endif()
    if(INDURTDB_BUILD_EXAMPLES)
        add_subdirectory(examples)
    endif()
endif()

# --- 覆盖率目标（T8）：仅当开启插桩且构建测试时可用 ---
if(INDURTDB_ENABLE_COVERAGE AND INDURTDB_BUILD_TESTS)
    find_program(GCOVR gcovr)
    add_custom_target(coverage
        COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Run tests under coverage instrumentation")
    if(GCOVR)
        add_custom_command(TARGET coverage POST_BUILD
            COMMAND ${GCOVR} -r ${CMAKE_SOURCE_DIR} -s
                    --xml -o ${CMAKE_BINARY_DIR}/coverage.xml
                    --html-details -o ${CMAKE_BINARY_DIR}/coverage.html
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            COMMENT "Generate coverage report (coverage.xml / coverage.html)")
    endif()
endif()

# --- 安装与包导出 (仅顶层项目; 子项目不安装) ---
if(INDURTDB_IS_TOP_LEVEL)
    include(CMakePackageConfigHelpers)

    install(TARGETS indurtdb EXPORT InduRTDBTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(DIRECTORY include/indurtdb DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    install(EXPORT InduRTDBTargets
        FILE InduRTDBTargets.cmake
        NAMESPACE InduRTDB::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/InduRTDB)

    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/InduRTDBConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/InduRTDBConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/InduRTDB)
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/InduRTDBConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion)
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/InduRTDBConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/InduRTDBConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/InduRTDB)

    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/indurtdb.pc.in
        ${CMAKE_CURRENT_BINARY_DIR}/indurtdb.pc @ONLY)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/indurtdb.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
endif()
