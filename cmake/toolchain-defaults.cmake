# Defaults for every embedded compiler, including editor and per-file scopes.
# Keep this a list of driver arguments, so paths with spaces remain one argument.
set(CPP_REPL_DEFAULT_COMPILE_OPTIONS "" CACHE STRING
  "Default JIT compiler arguments (semicolon-separated; session flags override)")
set(CPP_REPL_CLANG_CONFIG_FILE "AUTO" CACHE STRING
  "JIT Clang configuration file; AUTO follows the build compiler, empty disables")
if(CPP_REPL_CLANG_CONFIG_FILE STREQUAL "AUTO")
  set(_cpp_repl_config "")
  if(NOT APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    execute_process(COMMAND "${CMAKE_CXX_COMPILER}" -v -E -x c++ /dev/null
      OUTPUT_QUIET ERROR_VARIABLE _cpp_repl_verbose RESULT_VARIABLE _cpp_repl_status)
    if(_cpp_repl_status EQUAL 0 AND _cpp_repl_verbose MATCHES "Configuration file: ([^\n]+)")
      string(STRIP "${CMAKE_MATCH_1}" _cpp_repl_config)
    endif()
  endif()
else()
  set(_cpp_repl_config "${CPP_REPL_CLANG_CONFIG_FILE}")
endif()
if(_cpp_repl_config AND NOT EXISTS "${_cpp_repl_config}")
  message(FATAL_ERROR "JIT Clang configuration file not found: ${_cpp_repl_config}")
endif()

set(CPP_REPL_CLANG_RESOURCE_DIR "" CACHE PATH "Builtin headers for the linked Clang version")
if(NOT CPP_REPL_CLANG_RESOURCE_DIR)
  execute_process(COMMAND "${CPP_REPL_CLANG_EXECUTABLE}" -print-resource-dir
    OUTPUT_VARIABLE CPP_REPL_CLANG_RESOURCE_DIR OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _cpp_repl_resource_status)
  if(NOT _cpp_repl_resource_status EQUAL 0)
    set(CPP_REPL_CLANG_RESOURCE_DIR "${LLVM_LIBRARY_DIR}/clang/${LLVM_VERSION_MAJOR}")
  endif()
endif()
if(NOT EXISTS "${CPP_REPL_CLANG_RESOURCE_DIR}/include/stddef.h")
  message(FATAL_ERROR "Clang builtin headers not found at ${CPP_REPL_CLANG_RESOURCE_DIR}; set CPP_REPL_CLANG_RESOURCE_DIR")
endif()

# Quote generated C++ literals rather than passing lists through -D definitions.
function(cpp_repl_quote value output)
  string(REPLACE "\\" "\\\\" value "${value}")
  string(REPLACE "\"" "\\\"" value "${value}")
  string(REPLACE "\n" "\\n" value "${value}")
  string(REPLACE "\r" "\\r" value "${value}")
  set(${output} "\"${value}\"" PARENT_SCOPE)
endfunction()
cpp_repl_quote("${_cpp_repl_config}" CPP_REPL_CONFIG_LITERAL)
set(CPP_REPL_DEFAULT_ARGUMENTS "")
foreach(_cpp_repl_arg IN LISTS CPP_REPL_DEFAULT_COMPILE_OPTIONS)
  cpp_repl_quote("${_cpp_repl_arg}" _cpp_repl_literal)
  string(APPEND CPP_REPL_DEFAULT_ARGUMENTS "  ${_cpp_repl_literal},\n")
endforeach()
configure_file("${CMAKE_CURRENT_LIST_DIR}/toolchain_defaults.hpp.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/toolchain_defaults.hpp" @ONLY)
message(STATUS "cpp-repl JIT config: ${_cpp_repl_config}")
message(STATUS "cpp-repl JIT resource directory: ${CPP_REPL_CLANG_RESOURCE_DIR}")
message(STATUS "cpp-repl JIT defaults: ${CPP_REPL_DEFAULT_COMPILE_OPTIONS}")
