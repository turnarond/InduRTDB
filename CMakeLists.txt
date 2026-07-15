# InduRTDB - Industrial Real-Time Database (pure C core)
cmake_minimum_required(VERSION 3.15)
project(InduRTDB VERSION 3.0.0 LANGUAGES C CXX)

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

# 过渡期: 文件尚未全部创建时允许列表中缺失项被过滤
set(IRT_EXISTING_SRC "")
foreach(f ${IRT_CORE_SRC})
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${f})
        list(APPEND IRT_EXISTING_SRC ${f})
    endif()
endforeach()

# 过渡期: 源文件尚不存在时用占位编译单元避免 "No SOURCES given to target"
# (Task 2 创建首个 .c 后本分支自动失效, 随 EXISTS 过滤一并在 Task 9 移除)
if(NOT IRT_EXISTING_SRC)
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/irt_placeholder.c
        "/* transitional placeholder; unused once real .c sources exist */\n"
        "typedef int irt_placeholder_t;\n")
    list(APPEND IRT_EXISTING_SRC ${CMAKE_CURRENT_BINARY_DIR}/irt_placeholder.c)
endif()

add_library(indurtdb STATIC ${IRT_EXISTING_SRC})
target_include_directories(indurtdb PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(indurtdb PUBLIC rt pthread)
endif()

enable_testing()
add_subdirectory(tests)
