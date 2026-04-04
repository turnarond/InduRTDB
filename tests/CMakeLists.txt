# InduRTDB Tests Configuration

# Find GoogleTest
find_package(GTest REQUIRED)

# Enable testing
enable_testing()

# Add test subdirectories
add_subdirectory(unit)
add_subdirectory(integration)
add_subdirectory(performance)