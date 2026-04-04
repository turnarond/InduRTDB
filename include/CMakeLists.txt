# InduRTDB Include Configuration

add_library(indurtdb_headers INTERFACE)
add_library(InduRTDB::headers ALIAS indurtdb_headers)

target_include_directories(indurtdb_headers
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:include>
)

target_compile_definitions(indurtdb_headers
    INTERFACE
        INDURTDB_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        INDURTDB_VERSION_MINOR=${PROJECT_VERSION_MINOR}
        INDURTDB_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

install(DIRECTORY indurtdb/ DESTINATION include/indurtdb)