# Utils layer configuration

add_library(indurtdb_utils OBJECT)

target_sources(indurtdb_utils
    PRIVATE
        logging.cpp
        error.cpp
        alignment.cpp
)

target_include_directories(indurtdb_utils
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/include
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(indurtdb_utils
    PRIVATE
        InduRTDB::headers  # 直接链接到headers库
)

target_compile_definitions(indurtdb_utils
    PRIVATE
        INDURTDB_UTILS_BUILD
)
