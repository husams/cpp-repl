cmake_minimum_required(VERSION 3.20)
project(unresolved_static_library LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The consumer deliberately imports all four static data members.
add_library(unresolved_consumer SHARED src/consumer.cpp)
target_include_directories(unresolved_consumer PRIVATE include)
if(APPLE)
  target_link_options(unresolved_consumer PRIVATE "LINKER:-undefined,dynamic_lookup")
endif()

add_executable(loader_probe src/loader_probe.cpp)
target_link_libraries(loader_probe PRIVATE ${CMAKE_DL_LIBS})
