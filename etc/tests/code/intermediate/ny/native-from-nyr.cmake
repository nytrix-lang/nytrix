if(NOT DEFINED NY_FULL OR NOT DEFINED WORK_DIR OR NOT DEFINED EXE_SUFFIX)
  message(FATAL_ERROR "NY_FULL, WORK_DIR, and EXE_SUFFIX are required")
endif()

file(MAKE_DIRECTORY "${WORK_DIR}")
set(source "${WORK_DIR}/native-from-nyr.ny")
set(bundle "${WORK_DIR}/program.nyr")
set(executable "${WORK_DIR}/program${EXE_SUFFIX}")
file(WRITE "${source}"
  "mut value = 1\n"
  "mut values = [10, 20]\n"
  "fn choose(int index) int { values[index] }\n"
  "print(\"native bundle string ok\")\n"
  "value = choose(1)\n"
  "value\n"
)

execute_process(
  COMMAND "${NY_FULL}" "--native-precompile=${bundle}" "${source}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE error
)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "NYR bundle creation failed: ${output}${error}")
endif()

execute_process(
  COMMAND "${NY_FULL}" "--native-from-nyr=${bundle}" -o "${executable}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE error
)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "NYR native compilation failed: ${output}${error}")
endif()

execute_process(
  COMMAND "${executable}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE error
)
if(NOT result EQUAL 20 OR NOT output MATCHES "native bundle string ok")
  message(FATAL_ERROR
    "NYR native execution mismatch: exit=${result}; output=${output}; error=${error}")
endif()
