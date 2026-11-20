file(GLOB_RECURSE NYR_BACKEND_TEST_DRIVERS CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_LIST_DIR}/*.c")
list(APPEND NYR_BACKEND_TEST_DRIVERS ${NYR_BACKEND_VERIFY_DRIVERS})
list(SORT NYR_BACKEND_TEST_DRIVERS)

# Acceptance kernel baselines and the standalone scaling benchmarks are
# consumed by their dedicated fixture/performance paths, not as in-process
# backend regression drivers.
list(FILTER NYR_BACKEND_TEST_DRIVERS EXCLUDE REGEX
  ".*/core/(backend_(monolithic_)?scaling|floating-kernel|integer-kernel)\\.c$")
list(FILTER NYR_BACKEND_TEST_DRIVERS EXCLUDE REGEX ".*/gcfuzz\\.c$")

# CTest runs this standalone even when LLVM is disabled.
list(FILTER NYR_BACKEND_TEST_DRIVERS EXCLUDE REGEX ".*/lower/jit_layout\\.c$")

# Filter out Linux-specific test files that require headers not available on Windows
# These tests use #if defined(__linux__) && defined(__x86_64__) guards but their
# includes are still processed by the compiler on non-Linux platforms.
if (WIN32 OR NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  list(FILTER NYR_BACKEND_TEST_DRIVERS EXCLUDE REGEX
    ".*/(x64_encoder_oracle|native_vec4_f64|vec8_reduce_loop)\\.c$")
endif()
if (WIN32 OR NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  list(FILTER NYR_BACKEND_TEST_DRIVERS EXCLUDE REGEX
    ".*/core/public_acceptance\\.c$")
endif()

set(NYR_BACKEND_TEST_SOURCES)
set(NYR_BACKEND_TEST_DECLARATIONS "")
set(NYR_BACKEND_TEST_ENTRIES "")
set(NYR_BACKEND_TEST_DIR "${CMAKE_BINARY_DIR}/generated/nyr_backend_tests")
file(MAKE_DIRECTORY "${NYR_BACKEND_TEST_DIR}")
foreach(_driver IN LISTS NYR_BACKEND_TEST_DRIVERS)
  if(_driver IN_LIST NYR_BACKEND_VERIFY_DRIVERS)
    file(RELATIVE_PATH _test_name "${NYTRIX_ROOT}/src/code/backend" "${_driver}")
  else()
    file(RELATIVE_PATH _test_name "${CMAKE_CURRENT_LIST_DIR}" "${_driver}")
  endif()
  string(REGEX REPLACE "\\.c$" "" _test_name "${_test_name}")
  get_filename_component(_test_basename "${_driver}" NAME_WE)
  string(MAKE_C_IDENTIFIER "${_test_name}" _test_id)
  set(_wrapper "${NYR_BACKEND_TEST_DIR}/${_test_id}.c")
  set(_wrapper_template "${_wrapper}.in")
  file(WRITE "${_wrapper_template}"
    "#define main nyr_backend_test_${_test_id}\n#include \"${_driver}\"\n")
  configure_file("${_wrapper_template}" "${_wrapper}" COPYONLY)
  list(APPEND NYR_BACKEND_TEST_SOURCES "${_wrapper}")
  string(APPEND NYR_BACKEND_TEST_DECLARATIONS
    "int nyr_backend_test_${_test_id}(void);\n")
  string(APPEND NYR_BACKEND_TEST_ENTRIES
    "    {\"${_test_name}\", \"${_test_basename}\", nyr_backend_test_${_test_id}},\n")
endforeach()

set(NYR_BACKEND_TEST_REGISTRY "${NYR_BACKEND_TEST_DIR}/registry.c")
set(NYR_BACKEND_TEST_REGISTRY_TEMPLATE "${NYR_BACKEND_TEST_REGISTRY}.in")
file(WRITE "${NYR_BACKEND_TEST_REGISTRY_TEMPLATE}"
  "#include <stdio.h>\n#include <string.h>\n"
  "#include \"code/test/backend/registry.h\"\n"
  "${NYR_BACKEND_TEST_DECLARATIONS}"
  "typedef struct { const char *name; const char *basename; int (*run)(void); } nyr_backend_test_t;\n"
  "int nyr_backend_test_run(const char *name) {\n"
  "  static const nyr_backend_test_t tests[] = {\n"
  "${NYR_BACKEND_TEST_ENTRIES}"
  "  };\n"
  "  size_t ran = 0;\n"
  "  for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {\n"
  "    if (name && strcmp(name, tests[i].name) != 0 &&\n"
  "        strcmp(name, tests[i].basename) != 0) continue;\n"
  "    fprintf(stderr, \"[nyr-test] %s\\n\", tests[i].name);\n"
  "    int rc = tests[i].run();\n"
  "    if (rc != 0) { fprintf(stderr, \"[nyr-test] %s failed (%d)\\n\", tests[i].name, rc); return rc; }\n"
  "    ++ran;\n"
  "  }\n"
  "  if (name && ran == 0) {\n"
  "    fprintf(stderr, \"unknown NYR test group: %s\\nvalid groups:\", name);\n"
  "    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i)\n"
  "      fprintf(stderr, \" %s\", tests[i].name);\n"
  "    fputc('\\n', stderr);\n"
  "    return 2;\n"
  "  }\n"
  "  printf(\"NYR backend tests: %zu passed\\n\", ran);\n"
  "  return 0;\n"
  "}\n")
configure_file("${NYR_BACKEND_TEST_REGISTRY_TEMPLATE}"
  "${NYR_BACKEND_TEST_REGISTRY}" COPYONLY)

target_sources(ny-full PRIVATE
  ${NYR_BACKEND_TEST_SOURCES}
  "${NYR_BACKEND_TEST_REGISTRY}"
)
target_include_directories(ny-full PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
