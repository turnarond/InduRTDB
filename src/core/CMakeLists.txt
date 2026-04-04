# Core layer configuration

add_library(indurtdb_core OBJECT)

target_sources(indurtdb_core
    PRIVATE
        point_manager.cpp
        shared_memory_segment.cpp
        subscription_manager.cpp
        config_loader.cpp
        seqlock.cpp
)

# 直接添加include路径到编译选项中
target_compile_options(indurtdb_core
    PRIVATE
        -I${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(indurtdb_core
    PRIVATE
        indurtdb_osal
        InduRTDB::headers
)

target_compile_definitions(indurtdb_core
    PRIVATE
        INDURTDB_CORE_BUILD
)

install(FILES
    ${CMAKE_SOURCE_DIR}/include/indurtdb/core/point_manager_interface.hpp
    ${CMAKE_SOURCE_DIR}/include/indurtdb/core/subscription_manager_interface.hpp
    ${CMAKE_SOURCE_DIR}/include/indurtdb/core/seqlock.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/indurtdb/core
)
