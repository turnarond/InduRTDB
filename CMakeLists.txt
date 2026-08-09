# InduRTDB - Industrial Real-Time Database (pure C core)
cmake_minimum_required(VERSION 3.15)
project(InduRTDB VERSION 3.1.0 LANGUAGES C CXX)

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

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(INDURTDB_BUILD_TESTS)
        enable_testing()
        add_subdirectory(tests)
    endif()
    if(INDURTDB_BUILD_EXAMPLES)
        add_subdirectory(examples)
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
