cmake_minimum_required(VERSION 3.15)
project(indurtdb_starter C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(STARTER_USE_FETCHCONTENT "Use FetchContent instead of find_package" OFF)

if(STARTER_USE_FETCHCONTENT)
    include(FetchContent)
    if(DEFINED INDURTDB_SOURCE_DIR)
        # 离线/本地: 直接指向 InduRTDB 源码目录
        FetchContent_Declare(indurtdb SOURCE_DIR "${INDURTDB_SOURCE_DIR}")
    else()
        # 在线: 从远端仓库拉取
        FetchContent_Declare(indurtdb
            GIT_REPOSITORY https://github.com/turnarond/InduRTDB.git
            GIT_TAG main)
    endif()
    FetchContent_MakeAvailable(indurtdb)
else()
    # 已安装: find_package (配置时给 -DCMAKE_PREFIX_PATH=<安装前缀>)
    find_package(InduRTDB REQUIRED)
endif()

add_executable(starter main.c)
target_link_libraries(starter PRIVATE InduRTDB::indurtdb)
