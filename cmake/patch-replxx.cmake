# Keep the upstream revision pinned; this small extension separates a completion's
# insertion text from its displayed signature. Reconfiguration is idempotent.
find_package(Git REQUIRED)
execute_process(COMMAND "${GIT_EXECUTABLE}" apply --check "${patch}"
  WORKING_DIRECTORY "${source}" RESULT_VARIABLE can_apply
  OUTPUT_QUIET ERROR_QUIET)
if(can_apply EQUAL 0)
  execute_process(COMMAND "${GIT_EXECUTABLE}" apply "${patch}"
    WORKING_DIRECTORY "${source}" COMMAND_ERROR_IS_FATAL ANY)
else()
  execute_process(COMMAND "${GIT_EXECUTABLE}" apply --reverse --check "${patch}"
    WORKING_DIRECTORY "${source}" RESULT_VARIABLE already_applied
    OUTPUT_QUIET ERROR_QUIET)
  if(NOT already_applied EQUAL 0)
    message(FATAL_ERROR "Cannot apply cpp-repl's completion display extension")
  endif()
endif()
