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

add_library(indurtdb STATIC ${IRT_CORE_SRC})
target_include_directories(indurtdb PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(indurtdb PUBLIC rt pthread)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	enable_testing()
	add_subdirectory(tests)
	add_subdirectory(examples)
endif()
