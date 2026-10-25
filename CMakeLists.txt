cmake_minimum_required(VERSION 3.20)
# C is enabled too: LLVM's CMake modules run C header probes (FindLibEdit).
project(replxx_example LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

# Pinned replxx revision (commit of tag release-0.0.4).
set(REPLXX_REV "2b248467112cbbc16f63fde38230fd7c7ffc55f1")

set(REPLXX_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(REPLXX_BUILD_PACKAGE OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  replxx
  GIT_REPOSITORY https://github.com/AmokHuginnsson/replxx.git
  GIT_TAG ${REPLXX_REV}
  GIT_SHALLOW FALSE)
FetchContent_MakeAvailable(replxx)

# Clang's incremental interpreter (the clang-repl engine). Configure with
# `cmake --preset default` to use Homebrew's LLVM and its clang, which the JIT
# needs to match LLVM's ABI.
find_package(LLVM REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)
message(STATUS "cpp-repl: LLVM ${LLVM_PACKAGE_VERSION} at ${LLVM_DIR}")
if(NOT LLVM_VERSION_MAJOR EQUAL 22 AND NOT LLVM_VERSION_MAJOR EQUAL 21)
  message(WARNING "cpp-repl supports LLVM 21/22; found ${LLVM_PACKAGE_VERSION} "
                  "(clang/Interpreter changes between releases).")
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
  message(WARNING "Building with Apple clang; use `cmake --preset default` (Homebrew clang) "
                  "so the interpreter matches LLVM's ABI.")
endif()
# Run against the LLVM dylibs without DYLD_* variables.
set(CMAKE_BUILD_RPATH "${LLVM_LIBRARY_DIR}")
set(CMAKE_INSTALL_RPATH "${LLVM_LIBRARY_DIR}")

add_library(example_core STATIC
  src/commands.cpp src/shell_substitution.cpp src/cpp_lexer.cpp src/highlight_cpp.cpp
  src/highlight_repl.cpp src/library_manager.cpp src/library_dependencies.cpp src/inspection_output.cpp)
target_include_directories(example_core PUBLIC src)
target_include_directories(example_core SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
target_link_libraries(example_core PUBLIC replxx ${CMAKE_DL_LIBS} PRIVATE LLVM)
target_compile_options(example_core PRIVATE -Wall -Wextra -Wpedantic)

add_library(cpp_interp STATIC src/cpp_interpreter.cpp src/cpp_inspect.cpp src/cpp_symbols.cpp src/function_hooks.cpp src/loaded_symbols.cpp src/runtime_inspection.cpp src/project_compilation_database.cpp src/source_compile_options.cpp src/startup_files.cpp src/session_commands.cpp src/native_symbol_exports.cpp)
find_program(CPP_REPL_CLANG_EXECUTABLE NAMES clang
  HINTS "${LLVM_TOOLS_BINARY_DIR}" "${LLVM_INSTALL_PREFIX}/bin" REQUIRED)
set(REPLXX_NATIVE_SOURCE src/native_call_patch.cpp)
if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
  set(REPLXX_NATIVE_PLATFORM_SUPPORTED TRUE)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
  set(REPLXX_NATIVE_PLATFORM_SUPPORTED TRUE)
endif()
if(REPLXX_NATIVE_PLATFORM_SUPPORTED)
  option(REPLXX_NATIVE_CALL_PATCHES "Enable native library call-site hooks" ON)
  option(REPLXX_RUNTIME_SYMBOLS "Enable loaded native symbol enumeration" ON)
  if(REPLXX_NATIVE_CALL_PATCHES)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      set(REPLXX_NATIVE_SOURCE src/native_call_patch_linux.cpp)
    endif()
    target_compile_definitions(cpp_interp PUBLIC CPP_REPL_NATIVE_CALL_PATCHES=1)
  endif()
  if(REPLXX_RUNTIME_SYMBOLS)
    target_compile_definitions(cpp_interp PUBLIC CPP_REPL_RUNTIME_SYMBOLS=1)
  endif()
  if(REPLXX_NATIVE_CALL_PATCHES OR REPLXX_RUNTIME_SYMBOLS)
    target_sources(cpp_interp PRIVATE src/native_runtime.cpp)
  endif()
endif()
target_sources(cpp_interp PRIVATE ${REPLXX_NATIVE_SOURCE}
  src/definition_shadowing.cpp src/source_reload.cpp)
target_include_directories(cpp_interp PUBLIC src)
target_include_directories(cpp_interp SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
separate_arguments(CPP_REPL_LLVM_DEFINITIONS NATIVE_COMMAND "${LLVM_DEFINITIONS}")
set(CPP_REPL_CLANG_RESOURCE_DIR "${LLVM_LIBRARY_DIR}/clang/${LLVM_VERSION_MAJOR}")
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  execute_process(COMMAND "${CMAKE_CXX_COMPILER}" -print-resource-dir
    OUTPUT_VARIABLE REPLXX_COMPILER_RESOURCE_DIR OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE REPLXX_RESOURCE_STATUS)
  if(REPLXX_RESOURCE_STATUS EQUAL 0 AND EXISTS "${REPLXX_COMPILER_RESOURCE_DIR}/include/stddef.h")
    set(CPP_REPL_CLANG_RESOURCE_DIR "${REPLXX_COMPILER_RESOURCE_DIR}")
  endif()
endif()
target_compile_definitions(cpp_interp PRIVATE ${CPP_REPL_LLVM_DEFINITIONS}
  CPP_REPL_CLANG_EXECUTABLE="${CPP_REPL_CLANG_EXECUTABLE}"
  CPP_REPL_LLVM_PREFIX="${LLVM_INSTALL_PREFIX}"
  CPP_REPL_RESOURCE_DIR="${CPP_REPL_CLANG_RESOURCE_DIR}")
# libclang-cpp and libLLVM contain clangInterpreter and the ORC JIT.
target_link_libraries(cpp_interp PUBLIC example_core clang-cpp LLVM)
if(NOT LLVM_ENABLE_RTTI)
  target_compile_options(cpp_interp PRIVATE -fno-rtti)
endif()
target_compile_options(cpp_interp PRIVATE -Wall -Wextra -Wpedantic)

add_executable(cpp-repl src/main.cpp src/repl.cpp)
target_link_libraries(cpp-repl PRIVATE cpp_interp)
target_compile_options(cpp-repl PRIVATE -Wall -Wextra -Wpedantic)

option(REPLXX_EXAMPLE_BUILD_TESTS "Build the unit tests" ON)
if(REPLXX_EXAMPLE_BUILD_TESTS)
  enable_testing()
  include(GoogleTest)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.17.0
    SYSTEM)
  FetchContent_MakeAvailable(googletest)

  add_executable(cpp_lexer_test tests/cpp_lexer_test.cpp)
  target_link_libraries(cpp_lexer_test PRIVATE example_core GTest::gtest_main)
  target_compile_options(cpp_lexer_test PRIVATE -Wall -Wextra -Wpedantic)
  gtest_discover_tests(cpp_lexer_test)

  add_executable(commands_test tests/commands_test.cpp)
  target_link_libraries(commands_test PRIVATE example_core GTest::gtest_main)
  target_compile_options(commands_test PRIVATE -Wall -Wextra -Wpedantic)
  gtest_discover_tests(commands_test)

  # These libraries deliberately have no rpath: dependencies must be found by
  # the REPL rather than by CMake's usual build-directory rpath.
  foreach(fixture direct_dependency direct_root nested_leaf nested_middle nested_root retry_dependency retry_root)
    add_library(ctk_${fixture} SHARED tests/fixtures/dependencies/${fixture}.cpp)
    set_target_properties(ctk_${fixture} PROPERTIES
      SOVERSION 1 SKIP_BUILD_RPATH TRUE INSTALL_RPATH ""
      LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/dependency-fixtures/${fixture}")
    if(APPLE)
      set_target_properties(ctk_${fixture} PROPERTIES
        MACOSX_RPATH TRUE INSTALL_NAME_DIR "@rpath" BUILD_WITH_INSTALL_NAME_DIR TRUE)
    endif()
  endforeach()
  # Direct and nested fixtures share their root's directory.
  foreach(fixture direct_dependency direct_root)
    set_target_properties(ctk_${fixture} PROPERTIES
      LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/dependency-fixtures/direct")
  endforeach()
  foreach(fixture nested_leaf nested_middle nested_root)
    set_target_properties(ctk_${fixture} PROPERTIES
      LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/dependency-fixtures/nested")
  endforeach()
  target_link_libraries(ctk_direct_root PRIVATE ctk_direct_dependency)
  target_link_libraries(ctk_nested_middle PRIVATE ctk_nested_leaf)
  target_link_libraries(ctk_nested_root PRIVATE ctk_nested_middle)
  target_link_libraries(ctk_retry_root PRIVATE ctk_retry_dependency)
  add_executable(library_dependency_test tests/library_dependency_test.cpp)
  target_link_libraries(library_dependency_test PRIVATE example_core GTest::gtest_main)
  target_compile_definitions(library_dependency_test PRIVATE
    DIRECT_ROOT_LIBRARY="$<TARGET_FILE:ctk_direct_root>"
    NESTED_ROOT_LIBRARY="$<TARGET_FILE:ctk_nested_root>"
    RETRY_ROOT_LIBRARY="$<TARGET_FILE:ctk_retry_root>"
    RETRY_DEPENDENCY_DIRECTORY="$<TARGET_FILE_DIR:ctk_retry_dependency>")
  add_dependencies(library_dependency_test ctk_direct_root ctk_nested_root ctk_retry_root)
  gtest_discover_tests(library_dependency_test)

  add_executable(cpp_interpreter_test tests/cpp_interpreter_test.cpp)
  add_executable(iterative_development_test tests/iterative_development_test.cpp)
  target_link_libraries(iterative_development_test PRIVATE cpp_interp GTest::gtest_main)
  gtest_discover_tests(iterative_development_test)
  add_library(unresolved_callables_test_library SHARED tests/fixtures/unresolved_callables.cpp)
  if(APPLE)
    target_link_options(unresolved_callables_test_library PRIVATE "LINKER:-undefined,dynamic_lookup")
  endif()
  target_compile_definitions(cpp_interpreter_test PRIVATE
    UNRESOLVED_CALLABLES_LIBRARY="$<TARGET_FILE:unresolved_callables_test_library>"
    UNRESOLVED_CALLABLES_HEADER="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/unresolved_callables.hpp")
  add_dependencies(cpp_interpreter_test unresolved_callables_test_library)
  add_library(unresolved_storage_test_library SHARED tests/fixtures/unresolved_storage.cpp)
  if(APPLE)
    target_link_options(unresolved_storage_test_library PRIVATE "LINKER:-undefined,dynamic_lookup")
  endif()
  target_compile_definitions(cpp_interpreter_test PRIVATE
    UNRESOLVED_STORAGE_LIBRARY="$<TARGET_FILE:unresolved_storage_test_library>"
    UNRESOLVED_STORAGE_HEADER="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/unresolved_storage.hpp")
  add_dependencies(cpp_interpreter_test unresolved_storage_test_library)
  add_library(hook_test_library SHARED tests/fixtures/hook_library.cpp)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(hook_test_library PRIVATE tests/fixtures/hook_library_got.cpp)
    set_source_files_properties(tests/fixtures/hook_library_got.cpp
      PROPERTIES COMPILE_OPTIONS -fno-plt)
  endif()
  set_target_properties(hook_test_library PROPERTIES CXX_VISIBILITY_PRESET default)
  # Keep a real native call in native_inc to verify the interception boundary.
  target_compile_options(hook_test_library PRIVATE -fno-inline -fno-inline-functions)
  target_compile_definitions(cpp_interpreter_test PRIVATE
    HOOK_TEST_LIBRARY="$<TARGET_FILE:hook_test_library>"
    HOOK_TEST_HEADER="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/hook_library.hpp")
  add_dependencies(cpp_interpreter_test hook_test_library)
  target_link_libraries(cpp_interpreter_test PRIVATE cpp_interp GTest::gtest_main)
  target_compile_options(cpp_interpreter_test PRIVATE -Wall -Wextra -Wpedantic)
  gtest_discover_tests(cpp_interpreter_test DISCOVERY_TIMEOUT 30)

  add_executable(compilation_database_test tests/compilation_database_test.cpp)
  target_link_libraries(compilation_database_test PRIVATE cpp_interp GTest::gtest_main)
  target_compile_options(compilation_database_test PRIVATE -Wall -Wextra -Wpedantic)
  gtest_discover_tests(compilation_database_test DISCOVERY_TIMEOUT 30)

  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND AND NOT WIN32)
    add_test(NAME pty_smoke
             COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/pty_smoke.py
                     $<TARGET_FILE:cpp-repl>
                     $<TARGET_FILE:unresolved_callables_test_library>
                     ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/unresolved_callables.hpp)
  endif()
endif()

include(GNUInstallDirs)
install(TARGETS cpp-repl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT cpp-repl)
