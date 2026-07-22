# InduRTDB Examples (C11)

cmake_minimum_required(VERSION 3.15)

add_executable(basic_example basic_example.c)
target_link_libraries(basic_example PRIVATE indurtdb)
set_target_properties(basic_example PROPERTIES C_STANDARD 11)
