find_package(GTest REQUIRED)

# 每个 test_*.cpp 一个可执行文件
file(GLOB UNIT_TEST_SOURCES "test_c_*.cpp")
foreach(src ${UNIT_TEST_SOURCES})
    get_filename_component(name ${src} NAME_WE)
    add_executable(${name} ${src})
    target_link_libraries(${name} PRIVATE
        GTest::gtest GTest::gtest_main indurtdb)
    target_include_directories(${name} PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/src)
    set_target_properties(${name} PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    add_test(NAME ${name} COMMAND ${name})
endforeach()

# 版本一致性：期望值由 CMake project(VERSION) 注入，测试端不得硬编码，
# 否则每次发布都会漂移（v3.3.0 发布时实测踩到）。
if(TARGET test_c_version)
    target_compile_definitions(test_c_version PRIVATE
        INDURTDB_EXPECT_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        INDURTDB_EXPECT_VERSION_MINOR=${PROJECT_VERSION_MINOR}
        INDURTDB_EXPECT_VERSION_PATCH=${PROJECT_VERSION_PATCH}
        INDURTDB_EXPECT_VERSION_STRING="${PROJECT_VERSION}")
endif()
