# NYR text round trip: `nyr ir` output is itself a module that must run
# with the same output as its source.
#   cmake -DNYR=<nyr> -DSRC=<file.nyr> -DWORK=<dir> -DLEVEL=-O2 -P this
file(MAKE_DIRECTORY "${WORK}")
get_filename_component(name "${SRC}" NAME_WE)
set(printed "${WORK}/${name}${LEVEL}.nyr")
execute_process(COMMAND "${NYR}" run ${LEVEL} "${SRC}"
  OUTPUT_VARIABLE expected RESULT_VARIABLE rc_expected)
execute_process(COMMAND "${NYR}" ir ${LEVEL} "${SRC}"
  OUTPUT_FILE "${printed}" RESULT_VARIABLE rc_ir)
if(NOT rc_ir EQUAL 0)
  message(FATAL_ERROR "nyr ir ${LEVEL} ${SRC} failed (${rc_ir})")
endif()
execute_process(COMMAND "${NYR}" run -O0 "${printed}"
  OUTPUT_VARIABLE actual RESULT_VARIABLE rc_actual)
if(NOT rc_expected EQUAL rc_actual OR NOT expected STREQUAL actual)
  message(FATAL_ERROR "round trip of ${SRC} ${LEVEL} differs:\n"
    "source (${rc_expected}):\n${expected}\nprinted (${rc_actual}):\n${actual}")
endif()
