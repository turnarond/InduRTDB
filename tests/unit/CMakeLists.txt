# Unit tests configuration

# Find GoogleTest
find_package(GTest REQUIRED)

# Add tests
add_executable(test_seqlock
    test_seqlock.cpp
)

# Link libraries
target_link_libraries(test_seqlock
    PRIVATE
        GTest::gtest
        GTest::gtest_main
        indurtdb_core
        indurtdb_osal
)

# Include directories
target_include_directories(test_seqlock
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/include
)

# Add test to CTest
add_test(NAME test_seqlock COMMAND test_seqlock)

# Set test properties
set_target_properties(test_seqlock PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

# Install test executable
install(TARGETS test_seqlock
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/tests
)
