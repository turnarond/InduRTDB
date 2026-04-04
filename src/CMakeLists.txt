# InduRTDB Source Configuration

# 收集所有源文件
file(GLOB_RECURSE SOURCES
    "*.cpp"
)

# 创建库
add_library(indurtdb STATIC ${SOURCES})

# 包含目录
target_include_directories(indurtdb
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# 链接库
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(indurtdb pthread rt)
endif()

# 设置编译选项
target_compile_options(indurtdb
    PRIVATE
        -Wall
        -Wextra
        -Werror
        -fno-exceptions
        -fno-rtti
        -fno-asynchronous-unwind-tables
        -ffunction-sections
        -fdata-sections
)

# 设置C++标准
set_target_properties(indurtdb PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)