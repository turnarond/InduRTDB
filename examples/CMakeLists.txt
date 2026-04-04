# InduRTDB Examples CMake Configuration

cmake_minimum_required(VERSION 3.15)

# Add examples
if(BUILD_EXAMPLES)
    add_executable(basic_example basic_example.cpp)
    target_link_libraries(basic_example PRIVATE indurtdb)
    
    # Set output directory
    set_target_properties(basic_example PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples"
    )
    
    message(STATUS "Added example: basic_example")
endif()
