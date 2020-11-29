cmake_minimum_required(VERSION 3.0)
project(agreement)

enable_testing()
find_package(GTest REQUIRED)

add_executable (main
  agreement.hpp
  test.cpp)

set_property(TARGET main PROPERTY CXX_STANDARD 17)

target_link_libraries(main GTest::GTest GTest::Main)
include_directories(${GTEST_INCLUDE_DIRS})
