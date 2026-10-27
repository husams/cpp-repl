file(MAKE_DIRECTORY "${WORK}")
set(fake "${WORK}/fake lldb")
file(WRITE "${fake}" "#!/bin/sh\nprintf '%s\\n' \"$@\"\nexit 47\n")
file(CHMOD "${fake}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "CPP_REPL_LLDB=${fake}"
  "${BINARY}" --debug -std=c++20 "-Ipath with spaces"
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT status EQUAL 47)
  message(FATAL_ERROR "LLDB exit status was not preserved: ${status}: ${output}${errors}")
endif()
foreach(argument "--debug-info" "-std=c++20" "-Ipath with spaces"
                 "settings set plugin.jit-loader.gdb.enable on" "command script import")
  string(FIND "${output}" "${argument}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "Missing LLDB argument: ${argument}: ${output}")
  endif()
endforeach()
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "CPP_REPL_LLDB=${WORK}/missing lldb"
  "${BINARY}" --debug RESULT_VARIABLE status
  OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(status EQUAL 0 OR NOT errors MATCHES "cannot start LLDB")
  message(FATAL_ERROR "Missing LLDB was not reported: ${status}: ${errors}")
endif()
file(REMOVE "${fake}")
