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
