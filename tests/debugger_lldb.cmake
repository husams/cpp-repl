# Opt-in: this test needs permission to launch a process under a debugger.
file(MAKE_DIRECTORY "${WORK}")
execute_process(COMMAND "${LLDB}" --no-lldbinit --batch
  -o "command script import '${SCRIPT}'"
  -o "command script import '${CMAKE_CURRENT_LIST_DIR}/debugger_stop_hook.py'"
  -o "script debugger_stop_hook.verify()"
  TIMEOUT 30 RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT status EQUAL 0 OR output MATCHES "Traceback|AssertionError" OR
   NOT output MATCHES "concurrent stop checks passed")
  message(FATAL_ERROR "Concurrent stop checks failed:\n${output}${errors}")
endif()
set(input "${WORK}/input.repl")
file(WRITE "${input}" "%add \"${SOURCE}\"\ndebug_twice(21)\n%quit\n")
execute_process(COMMAND "${LLDB}" --no-lldbinit --batch
  -o "settings set plugin.jit-loader.gdb.enable on"
  -o "settings set target.disable-aslr false"
  -o "command script import '${SCRIPT}'"
  -o "breakpoint set -f functions.cpp -l 7"
  -o "process launch -i '${input}'"
  -o next -o "frame variable doubled" -o step
  -o "frame variable value" -o next -o "frame variable result"
  -o bt -o finish -o "frame variable doubled" -o continue
  -- "${BINARY}" --debug-info -O3 -g0
  WORKING_DIRECTORY "${WORK}" TIMEOUT 60
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors)
file(WRITE "${WORK}/transcript.txt" "${output}${errors}")
foreach(expected "stop reason = breakpoint" "doubled = 42" "value = 42"
                 "result = 43" "stop reason = step in"
                 "debug_increment" "input-2.cpp:1" "(int) 43"
                 "exited with status = 0")
  string(FIND "${output}" "${expected}" found)
  if(NOT status EQUAL 0 OR found EQUAL -1 OR output MATCHES "Traceback|AssertionError")
    message(FATAL_ERROR "LLDB check failed (${expected}), status ${status}:\n${output}${errors}")
  endif()
endforeach()

# Explicit dispatcher breakpoints and instruction steps must remain inspectable.
file(WRITE "${input}" "%add \"${SOURCE}\"\ndebug_twice(21)\n%quit\n")
execute_process(COMMAND "${LLDB}" --no-lldbinit --batch
  -o "settings set plugin.jit-loader.gdb.enable on"
  -o "settings set target.disable-aslr false"
  -o "command script import '${SCRIPT}'"
  -o "breakpoint set -f __cpp_repl_dispatch -l 1"
  -o "process launch -i '${input}'"
  -o "script assert lldb.thread.GetStopReason() == lldb.eStopReasonBreakpoint"
  -o stepi
  -o "script assert lldb.frame.GetLineEntry().GetFileSpec().GetFilename() == '__cpp_repl_dispatch'"
  -o "breakpoint delete 1" -o continue -- "${BINARY}" --debug-info
  WORKING_DIRECTORY "${WORK}" TIMEOUT 60
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors)
file(WRITE "${WORK}/explicit-stops-transcript.txt" "${output}${errors}")
if(NOT status EQUAL 0 OR output MATCHES "Traceback|AssertionError" OR
   NOT output MATCHES "stop reason = breakpoint" OR
   NOT output MATCHES "stop reason = instruction step" OR
   NOT output MATCHES "exited with status = 0")
  message(FATAL_ERROR "Explicit stops were not preserved:\n${output}${errors}")
endif()

# A watchpoint encountered by step-in must stop rather than auto-resume.
execute_process(COMMAND "${LLDB}" --no-lldbinit --batch
  -o "settings set plugin.jit-loader.gdb.enable on"
  -o "settings set target.disable-aslr false"
  -o "command script import '${SCRIPT}'"
  -o "breakpoint set -f functions.cpp -l 7"
  -o "process launch -i '${input}'" -o next
  -o "watchpoint set variable -w read doubled" -o step
  -o "script assert lldb.thread.GetStopReason() == lldb.eStopReasonWatchpoint"
  -o "watchpoint delete 1" -o "thread plan discard 1" -o continue
  -- "${BINARY}" --debug-info
  WORKING_DIRECTORY "${WORK}" TIMEOUT 60
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors)
file(WRITE "${WORK}/watchpoint-transcript.txt" "${output}${errors}")
if(NOT status EQUAL 0 OR output MATCHES "Traceback|AssertionError" OR
   NOT output MATCHES "stop reason = watchpoint" OR
   NOT output MATCHES "exited with status = 0")
  message(FATAL_ERROR "Watchpoint stop was not preserved:\n${output}${errors}")
endif()

# Follow the active mock rather than assuming the dispatcher calls its original.
file(WRITE "${input}" "%add \"${SOURCE}\"\n%mock debug_increment(int value) cpp`return value + 5;`\ndebug_twice(21)\n%quit\n")
execute_process(COMMAND "${LLDB}" --no-lldbinit --batch
  -o "settings set plugin.jit-loader.gdb.enable on"
  -o "settings set target.disable-aslr false"
  -o "command script import '${SCRIPT}'"
  -o "breakpoint set -f functions.cpp -l 7"
  -o "process launch -i '${input}'" -o next -o step
  -o "script assert '__cpp_repl_hook_' in lldb.frame.GetFunctionName()"
  -o continue -- "${BINARY}" --debug-info
  WORKING_DIRECTORY "${WORK}" TIMEOUT 60
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors)
file(WRITE "${WORK}/hook-transcript.txt" "${output}${errors}")
if(NOT status EQUAL 0 OR output MATCHES "Traceback|AssertionError" OR
   NOT output MATCHES "\\(int\\) 47" OR NOT output MATCHES "exited with status = 0")
  message(FATAL_ERROR "Stepping did not follow the active mock:\n${output}${errors}")
endif()

# Interrupting inside a dispatcher must remain stopped at the signal.
file(WRITE "${input}" "%add \"${SOURCE}\"\ndebug_twice(21)\n%quit\n")
execute_process(COMMAND "${LLDB}" --no-lldbinit --batch
  -o "settings set plugin.jit-loader.gdb.enable on"
  -o "settings set target.disable-aslr false"
  -o "command script import '${SCRIPT}'"
  -o "breakpoint set -f __cpp_repl_dispatch -l 1"
  -o "process launch -i '${input}'"
  -o "script import os, signal; os.kill(lldb.process.GetProcessID(), signal.SIGINT)"
  -o continue
  -o "script assert lldb.thread.GetStopReason() == lldb.eStopReasonSignal"
  -o "process kill" -- "${BINARY}" --debug-info
  WORKING_DIRECTORY "${WORK}" TIMEOUT 60
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors)
file(WRITE "${WORK}/interrupt-transcript.txt" "${output}${errors}")
if(NOT status EQUAL 0 OR output MATCHES "Traceback|AssertionError" OR
   NOT output MATCHES "stop reason = signal SIGINT")
  message(FATAL_ERROR "Interrupt was not preserved:\n${output}${errors}")
endif()

# Prompt definitions must have a source file too, and errors must stay recoverable.
file(WRITE "${input}" "int debug_prompt(int value) { return value + 2; }\ndebug_prompt(40)\nthis is invalid\ndebug_prompt(1)\n%quit\n")
execute_process(COMMAND "${LLDB}" --no-lldbinit --batch
  -o "settings set plugin.jit-loader.gdb.enable on"
  -o "settings set target.disable-aslr false"
  -o "command script import '${SCRIPT}'"
  -o "breakpoint set -f input-1.cpp -l 1"
  -o "process launch -i '${input}'" -o "frame variable value"
  -o "expression -- value + 1" -o "source list" -o "breakpoint delete 1" -o continue
  -- "${BINARY}" --debug-info
  WORKING_DIRECTORY "${WORK}" TIMEOUT 60
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors)
file(WRITE "${WORK}/prompt-transcript.txt" "${output}${errors}")
foreach(expected "stop reason = breakpoint" "input-1.cpp:1" "value = 40"
                 "= 41" "(int) 42" "(int) 3" "exited with status = 0")
  string(FIND "${output}" "${expected}" found)
  if(NOT status EQUAL 0 OR found EQUAL -1)
    message(FATAL_ERROR "Prompt debugging failed (${expected}):\n${output}${errors}")
  endif()
endforeach()
