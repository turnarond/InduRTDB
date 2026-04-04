# OS Abstraction Layer configuration

# Platform detection
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(OSAL_PLATFORM "linux")
    add_definitions(-DINDURTDB_OS_LINUX)
elseif(CMAKE_SYSTEM_NAME STREQUAL "SylixOS")
    set(OSAL_PLATFORM "sylixos")
    add_definitions(-DINDURTDB_OS_SYLIXOS)
else()
    message(FATAL_ERROR "Unsupported platform: ${CMAKE_SYSTEM_NAME}")
endif()

# POSIX interface library
add_library(indurtdb_osal_posix INTERFACE)
target_include_directories(indurtdb_osal_posix
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/posix
        ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(indurtdb_osal_posix
    INTERFACE
        pthread
        rt
)
target_compile_definitions(indurtdb_osal_posix
    INTERFACE
        INDURTDB_OS_POSIX
)

# OSAL object library
add_library(indurtdb_osal OBJECT)

# Add source files
target_sources(indurtdb_osal
    PRIVATE
        posix/shared_memory_posix.cpp
        posix/time_posix.cpp
        posix/threading_posix.cpp
        posix/notification_posix.cpp
        ${OSAL_PLATFORM}/shared_memory_${OSAL_PLATFORM}.cpp
        ${OSAL_PLATFORM}/time_${OSAL_PLATFORM}.cpp
        ${OSAL_PLATFORM}/threading_${OSAL_PLATFORM}.cpp
        ${OSAL_PLATFORM}/notification_${OSAL_PLATFORM}.cpp
        factory.cpp
)

target_include_directories(indurtdb_osal
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/posix
        ${CMAKE_CURRENT_SOURCE_DIR}/${OSAL_PLATFORM}
        ${CMAKE_SOURCE_DIR}/include
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
)

# Link libraries
target_link_libraries(indurtdb_osal
    PRIVATE
        indurtdb_osal_posix
        InduRTDB::headers  # 直接链接到headers库
)

# Compile definitions
target_compile_definitions(indurtdb_osal
    PRIVATE
        INDURTDB_OSAL_BUILD
)

# Installation
install(TARGETS indurtdb_osal_posix
    EXPORT InduRTDBTargets
)
