# Shared C-frontend test helpers: a failing command, a missing or empty
# artifact, or a stale artifact must fail the test that uses them.

# Run a command expected to succeed, preserving its status and output for any
# following assertions in the calling scope.
function(nyc_execute_ok error_context)
  execute_process(${ARGN}
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr)
  set(status "${status}" PARENT_SCOPE)
  set(stdout "${stdout}" PARENT_SCOPE)
  set(stderr "${stderr}" PARENT_SCOPE)
  if (NOT "${status}" STREQUAL "0")
    string(REPLACE [[\n${stdout}${stderr}]] "" _error_context
      "${error_context}")
    string(CONFIGURE "${_error_context}" _error_context)
    message(FATAL_ERROR "${_error_context}\n${stdout}${stderr}")
  endif()
endfunction()

function(nyc_remove_artifact path)
  file(REMOVE "${path}")
  if(EXISTS "${path}" OR IS_SYMLINK "${path}")
    message(FATAL_ERROR "could not remove stale test artifact: ${path}")
  endif()
endfunction()

function(nyc_require_artifact error_context path)
  if(NOT EXISTS "${path}" OR IS_DIRECTORY "${path}")
    message(FATAL_ERROR "${error_context}: no output produced at ${path}")
  endif()
  file(SIZE "${path}" size)
  if(size EQUAL 0)
    message(FATAL_ERROR "${error_context}: empty output at ${path}")
  endif()
endfunction()

# Require an artifact produced by the command under test.
function(nyc_execute_output error_context path)
  nyc_remove_artifact("${path}")
  nyc_execute_ok("${error_context}" ${ARGN})
  nyc_require_artifact("${error_context}" "${path}")
  set(stdout "${stdout}" PARENT_SCOPE)
  set(stderr "${stderr}" PARENT_SCOPE)
endfunction()
