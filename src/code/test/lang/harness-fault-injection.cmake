# Fault injection for the C frontend harness. Each case runs one harness
# helper against a deliberately broken command in a child cmake process, which
# must exit nonzero: wrong output, bad assembly, link failure, crash, timeout,
# a missing artifact, an empty artifact, and a stale artifact left by an
# earlier run.
if(NOT DEFINED NYC OR NOT DEFINED CC OR NOT DEFINED SOURCE_DIR OR
   NOT DEFINED BINARY_DIR)
  message(FATAL_ERROR "NYC, CC, SOURCE_DIR and BINARY_DIR are required")
endif()
file(MAKE_DIRECTORY "${BINARY_DIR}")
set(good "${SOURCE_DIR}/c/zero-length-static.c")

function(expect_detected name body)
  set(script "${BINARY_DIR}/fault-${name}.cmake")
  file(WRITE "${script}" "include(\"${SOURCE_DIR}/harness.cmake\")\nset(BINARY_DIR \"${BINARY_DIR}\")\n${body}\n")
  execute_process(COMMAND "${CMAKE_COMMAND}" -P "${script}"
    RESULT_VARIABLE status OUTPUT_QUIET ERROR_QUIET)
  if(status EQUAL 0)
    message(FATAL_ERROR "harness missed injected fault: ${name}")
  endif()
endfunction()

file(WRITE "${BINARY_DIR}/fault-bad.c" "int broken( {\n")
file(WRITE "${BINARY_DIR}/fault-link.c" "int missing_symbol(void);\nint main(void) { return missing_symbol(); }\n")
file(WRITE "${BINARY_DIR}/fault-empty.o" "")
file(WRITE "${BINARY_DIR}/fault-stale.o" "stale")

expect_detected(wrong-exit
  "nyc_execute_ok([=[wrong result]=] COMMAND sh -c \"exit 3\")")
expect_detected(crash
  "nyc_execute_ok([=[crash]=] COMMAND sh -c \"kill -ABRT \\$\\$\")")
expect_detected(timeout
  "nyc_execute_ok([=[timeout]=] COMMAND sleep 30 TIMEOUT 1)")
expect_detected(bad-source
  "nyc_execute_ok([=[bad source]=] COMMAND \"${NYC}\" -O2 -S \"${BINARY_DIR}/fault-bad.c\" -o \"${BINARY_DIR}/fault-bad.s\")")
expect_detected(link-failure
  "nyc_execute_ok([=[link]=] COMMAND \"${CC}\" -o \"${BINARY_DIR}/fault-link\" \"${BINARY_DIR}/fault-link.c\")")
expect_detected(missing-artifact
  "nyc_require_artifact([=[missing]=] \"${BINARY_DIR}/fault-does-not-exist.o\")")
expect_detected(empty-artifact
  "nyc_require_artifact([=[empty]=] \"${BINARY_DIR}/fault-empty.o\")")
expect_detected(stale-artifact
  "nyc_execute_output([=[stale]=] \"${BINARY_DIR}/fault-stale.o\" COMMAND true)")

# And the harness must still accept a good compile.
execute_process(COMMAND "${NYC}" -O2 -S "${good}" -o "${BINARY_DIR}/fault-good.s"
  RESULT_VARIABLE good_status OUTPUT_QUIET ERROR_QUIET)
if(NOT good_status EQUAL 0)
  message(FATAL_ERROR "the good control compile failed")
endif()
