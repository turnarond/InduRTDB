# InduRTDB Examples CMake Configuration

cmake_minimum_required(VERSION 3.15)

# Add examples
if(BUILD_EXAMPLES)
    add_executable(basic_example basic_example.cpp)
    target_link_libraries(basic_example PRIVATE indurtdb)

    # 需要C语言以编译C API示例
    enable_language(C)
    add_executable(c_example c_example.c)
    target_link_libraries(c_example PRIVATE indurtdb)

    add_executable(multi_process_example multi_process_example.cpp)
    target_link_libraries(multi_process_example PRIVATE indurtdb)

    # Set output directory
    set_target_properties(basic_example c_example multi_process_example PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples"
    )

    message(STATUS "Added example: basic_example")
    message(STATUS "Added example: c_example")
    message(STATUS "Added example: multi_process_example")
endif()
