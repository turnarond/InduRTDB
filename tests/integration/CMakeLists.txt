find_package(GTest REQUIRED)

add_executable(test_c_multi_process test_c_multi_process.cpp)
target_link_libraries(test_c_multi_process PRIVATE
    GTest::gtest GTest::gtest_main indurtdb)
target_include_directories(test_c_multi_process PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(test_c_multi_process PRIVATE _GNU_SOURCE)
set_target_properties(test_c_multi_process PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME test_c_multi_process COMMAND test_c_multi_process)
