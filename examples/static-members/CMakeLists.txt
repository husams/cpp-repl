cmake_minimum_required(VERSION 3.20)
project(static_members_example LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(static_members_native SHARED src/native.cpp)
target_include_directories(static_members_native PRIVATE include)

# Compile this source without linking it: the REPL supplies its missing storage.
add_library(session_reference OBJECT src/session.cpp)
target_include_directories(session_reference PRIVATE include)
target_compile_definitions(session_reference PRIVATE
  EXAMPLE_COMPILE_DB=73 EXAMPLE_STARTUP_FLAG=19)
