# API layer configuration

add_library(indurtdb_api OBJECT)

target_sources(indurtdb_api
    PRIVATE
        cpp/indurtdb_impl.cpp
        c/indurtdb_c_impl.cpp
)

target_include_directories(indurtdb_api
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/c
        ${CMAKE_SOURCE_DIR}/include
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(indurtdb_api
    PRIVATE
        indurtdb_core
        InduRTDB::headers  # 直接链接到headers库
)

target_compile_definitions(indurtdb_api
    PRIVATE
        INDURTDB_API_BUILD
)
