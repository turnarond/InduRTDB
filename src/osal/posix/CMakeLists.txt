# POSIX OSAL configuration

# Add POSIX OSAL as an interface library
add_library(indurtdb_osal_posix INTERFACE)

# Include directories
target_include_directories(indurtdb_osal_posix
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)

# Link POSIX libraries
target_link_libraries(indurtdb_osal_posix
    INTERFACE
        pthread
        rt
)

# Compile definitions
target_compile_definitions(indurtdb_osal_posix
    INTERFACE
        INDURTDB_OS_POSIX
)