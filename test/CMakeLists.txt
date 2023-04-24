cmake_minimum_required(VERSION 3.0)

project(tilemaker_test)

set(CMAKE_CXX_STANDARD 14)

# Project settings
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ".")

# Dependencies
find_package(Boost COMPONENTS filesystem system unit_test_framework REQUIRED)

# Assign the include directories
include_directories(${Boost_INCLUDE_DIRS})
include_directories(${UNIT_TESTS_INCLUDES})

# Build unit tests
add_executable(tilemaker_test test_helpers.cpp)
target_link_libraries(tilemaker_test ${Boost_LIBRARIES} libtilemaker)

enable_testing()
add_test(tilemaker_test tilemaker_test)
