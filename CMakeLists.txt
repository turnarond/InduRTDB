# InduRTDB - Industrial Real-Time Database
# CMake Configuration File

cmake_minimum_required(VERSION 3.15)
project(InduRTDB VERSION 1.0.0 LANGUAGES CXX)

# 设置C++标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 编译类型
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type (Debug/Release)" FORCE)
endif()

# 编译选项
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -DNDEBUG")
else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O0 -g -DDEBUG")
endif()

# 通用编译标志
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Werror")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-exceptions -fno-rtti")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-asynchronous-unwind-tables")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ffunction-sections -fdata-sections")

# 平台检测和配置
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(INDURTDB_PLATFORM "Linux")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D_GNU_SOURCE -DINDURTDB_OS_LINUX")
    message(STATUS "Building for Linux platform")
elseif(CMAKE_SYSTEM_NAME STREQUAL "SylixOS")
    set(INDURTDB_PLATFORM "SylixOS")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DINDURTDB_OS_SYLIXOS")
    message(STATUS "Building for SylixOS platform")
else()
    message(FATAL_ERROR "Unsupported platform: ${CMAKE_SYSTEM_NAME}")
endif()

# 包含路径
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# 收集所有源文件
file(GLOB_RECURSE SOURCES
    "src/*.cpp"
)

# 排除不需要的平台实现文件
if(INDURTDB_PLATFORM STREQUAL "Linux")
    # 排除sylixos实现文件
    list(FILTER SOURCES EXCLUDE REGEX ".*/sylixos/.*\\.cpp$")
    message(STATUS "Excluding SylixOS implementation files")
elseif(INDURTDB_PLATFORM STREQUAL "SylixOS")
    # 排除linux实现文件
    list(FILTER SOURCES EXCLUDE REGEX ".*/linux/.*\\.cpp$")
    message(STATUS "Excluding Linux implementation files")
endif()

message(STATUS "Found ${SOURCES} source files")

# 创建静态库
add_library(indurtdb STATIC ${SOURCES})

# 设置库属性
set_target_properties(indurtdb PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
    OUTPUT_NAME "indurtdb"
)

# 链接所需库
if(INDURTDB_PLATFORM STREQUAL "Linux")
    target_link_libraries(indurtdb pthread rt)
    message(STATUS "Linking with pthread and rt libraries")
endif()

# 安装配置
install(TARGETS indurtdb
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

install(DIRECTORY include/ DESTINATION include
    FILES_MATCHING PATTERN "*.hpp" PATTERN "*.h"
)

# 测试配置
option(BUILD_TESTS "Build tests" ON)
if(BUILD_TESTS)
    enable_testing()
    
    # 检查GTest
    find_package(GTest QUIET)
    if(GTest_FOUND)
        message(STATUS "GTest found, building tests")
        
        # 收集测试源文件
        file(GLOB_RECURSE TEST_SOURCES
            "tests/*.cpp"
        )
        
        if(TEST_SOURCES)
            # 创建测试可执行文件
            add_executable(indurtdb_tests ${TEST_SOURCES})
            
            # 链接库
            target_link_libraries(indurtdb_tests
                PRIVATE
                    indurtdb
                    GTest::gtest
                    GTest::gtest_main
            )
            
            # 添加测试
            add_test(NAME indurtdb_tests COMMAND indurtdb_tests)
            message(STATUS "Tests enabled: ${TEST_SOURCES}")
        else()
            message(STATUS "No test sources found")
        endif()
    else()
        message(STATUS "GTest not found, skipping tests")
    endif()
else()
    message(STATUS "Tests disabled by user")
endif()

# 示例配置
option(BUILD_EXAMPLES "Build examples" OFF)
if(BUILD_EXAMPLES)
    # 检查examples目录
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples")
        add_subdirectory(examples)
        message(STATUS "Examples enabled")
    else()
        message(STATUS "Examples directory not found")
    endif()
endif()

# 生成配置
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/InduRTDBConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/InduRTDBConfig.cmake"
    @ONLY
)

# 总结信息
message(STATUS "=========================================")
message(STATUS "InduRTDB Configuration Summary:")
message(STATUS "  Version: ${PROJECT_VERSION}")
message(STATUS "  Platform: ${INDURTDB_PLATFORM}")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  Tests: ${BUILD_TESTS}")
message(STATUS "  Examples: ${BUILD_EXAMPLES}")
message(STATUS "=========================================")