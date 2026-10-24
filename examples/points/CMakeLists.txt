cmake_minimum_required(VERSION 3.20)
project(points_example LANGUAGES CXX)

add_library(points SHARED point.cpp)
set_target_properties(points PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
  CXX_VISIBILITY_PRESET default)
# Preserve native call sites for the manual mocking demonstration.
target_compile_options(points PRIVATE -Wall -Wextra -Wpedantic
  -fno-inline -fno-inline-functions)
