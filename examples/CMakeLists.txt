add_executable(basic_example basic_example.c)
target_link_libraries(basic_example PRIVATE indurtdb)
set_target_properties(basic_example PROPERTIES C_STANDARD 11)

# peek 生命周期示例：演示单拷贝陷阱与正确用法（U2）
add_executable(peek_lifetime_example peek_lifetime_example.c)
target_link_libraries(peek_lifetime_example PRIVATE indurtdb)
set_target_properties(peek_lifetime_example PROPERTIES C_STANDARD 11)
