# Language test registration and runner.
#
# Configure mode registers Ny corpus drivers and the generated stdlib debug
# path check. Script mode runs the C frontend suite using its own discovered
# fixture tree, without requiring a second language-specific run.cmake.

if (NY_NY_RUN_DEBUG_PATH)
  if (NOT DEFINED NY_NY_FULL OR NOT DEFINED NY_NY_DEBUG_OUTPUT OR
      NOT DEFINED NY_NY_FIXTURE OR NOT DEFINED NY_NY_ROOT)
    message(FATAL_ERROR "incomplete Ny debug-path test configuration")
  endif()
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env NYTRIX_STD_CACHE=0
      "${NY_NY_FULL}" -g -O0 "--emit-ir=${NY_NY_DEBUG_OUTPUT}" -emit-only
      "${NY_NY_FIXTURE}"
    WORKING_DIRECTORY "${NY_NY_ROOT}"
    RESULT_VARIABLE _ny_debug_result
    OUTPUT_VARIABLE _ny_debug_stdout
    ERROR_VARIABLE _ny_debug_stderr
  )
  if (NOT _ny_debug_result EQUAL 0)
    message(FATAL_ERROR
      "ny-full failed to emit debug IR (${_ny_debug_result})\n"
      "${_ny_debug_stdout}${_ny_debug_stderr}")
  endif()
  file(READ "${NY_NY_DEBUG_OUTPUT}" _ny_debug_ir)
  set(_ny_expected "DIFile(filename: \"str.ny\", directory: \"${NY_NY_ROOT}/core\")")
  string(FIND "${_ny_debug_ir}" "${_ny_expected}" _ny_expected_at)
  if (_ny_expected_at EQUAL -1)
    message(FATAL_ERROR "stdlib debug IR lacks expected source path: ${_ny_expected}")
  endif()
  if (_ny_debug_ir MATCHES "DIFile\\(filename: \\\"<(stdlib|repl_std)>\\\"")
    message(FATAL_ERROR "stdlib debug IR contains a synthetic stdlib filename")
  endif()
  return()
endif()

if (DEFINED NYTRIX_ROOT)
  set(NY_TEST_LANG_NY_CORPUS_DIR "${CMAKE_CURRENT_LIST_DIR}/ny")
  set(NY_TEST_LANG_NY_GENERATED_DIR
    "${CMAKE_BINARY_DIR}/generated/ny_lang_tests")
  file(GLOB_RECURSE NY_TEST_LANG_NY_CORPUS_FILES CONFIGURE_DEPENDS
    LIST_DIRECTORIES false
    "${NY_TEST_LANG_NY_CORPUS_DIR}/*.c"
    "${NY_TEST_LANG_NY_CORPUS_DIR}/*.h")
  list(SORT NY_TEST_LANG_NY_CORPUS_FILES)
  set(_ny_test_corpus_names)
  foreach(_corpus_file IN LISTS NY_TEST_LANG_NY_CORPUS_FILES)
    get_filename_component(_corpus_name "${_corpus_file}" NAME)
    list(FIND _ny_test_corpus_names "${_corpus_name}" _existing_name)
    if(NOT _existing_name EQUAL -1)
      message(FATAL_ERROR
        "Ny test corpus files must have unique basenames: ${_corpus_name}")
    endif()
    list(APPEND _ny_test_corpus_names "${_corpus_name}")
    set("NY_TEST_LANG_NY_CORPUS_${_corpus_name}" "${_corpus_file}")
  endforeach()

  # Corpus C sources are purpose-specific: embedding support is linked into
  # ny-full; allocator and module identity checks remain standalone drivers.
  set(NY_TEST_LANG_NY_SUBST_ALLOCATION
    "${NY_TEST_LANG_NY_CORPUS_subst_allocation.c}")
  set(NY_TEST_LANG_NY_MODULE_IDENTITY
    "${NY_TEST_LANG_NY_CORPUS_module_identity.c}")

  if (TARGET ny-full)
    target_sources(ny-full PRIVATE
      "${NYTRIX_ROOT}/src/code/lang/ny/pipeline/embedding.c"
    )
  endif()

  if (BUILD_TESTING AND UNIX AND NOT APPLE)
    # Isolate realloc failure injection from compiler bootstrap allocations.
    add_executable(ny-subst-allocation-test
      "${NY_TEST_LANG_NY_SUBST_ALLOCATION}"
      "${NYTRIX_ROOT}/src/code/lang/ny/semantics/typeinfer.c")
    target_include_directories(ny-subst-allocation-test PRIVATE
      "${NYTRIX_ROOT}/src" "${NYTRIX_ROOT}/src/include"
      "${CMAKE_BINARY_DIR}" "${NYTRIX_LLVM_INCLUDE}")
    target_compile_options(ny-subst-allocation-test PRIVATE
      -ffunction-sections -fdata-sections ${NYTRIX_HOST_CFLAGS})
    target_link_options(ny-subst-allocation-test PRIVATE
      -Wl,--gc-sections -Wl,--wrap=realloc ${NYTRIX_HOST_LDFLAGS})
    add_test(NAME ny-subst-allocation COMMAND ny-subst-allocation-test)
  endif()

  if (NYTRIX_USE_LLVM AND TARGET ny-full)
    add_executable(ny-module-identity-test
      "${NY_TEST_LANG_NY_MODULE_IDENTITY}"
      $<TARGET_OBJECTS:nytrix_compiler>
      $<TARGET_OBJECTS:nytrix_runtime>
    )
    add_dependencies(ny-module-identity-test nytrix_version_header std)
    target_include_directories(ny-module-identity-test PRIVATE
      "${NYTRIX_ROOT}/src"
      "${NYTRIX_ROOT}/src/include"
      "${NYTRIX_ROOT}/src/code/backend/runtime"
      "${CMAKE_BINARY_DIR}"
    )
    get_target_property(_ny_identity_compile_defs ny-full COMPILE_DEFINITIONS)
    if (_ny_identity_compile_defs)
      target_compile_definitions(ny-module-identity-test PRIVATE
        ${_ny_identity_compile_defs})
    endif()
    get_target_property(_ny_identity_compile_opts ny-full COMPILE_OPTIONS)
    if (_ny_identity_compile_opts)
      target_compile_options(ny-module-identity-test PRIVATE
        ${_ny_identity_compile_opts})
    endif()
    get_target_property(_ny_identity_link_libs ny-full LINK_LIBRARIES)
    if (_ny_identity_link_libs)
      target_link_libraries(ny-module-identity-test PRIVATE
        ${_ny_identity_link_libs})
    endif()
    get_target_property(_ny_identity_link_opts ny-full LINK_OPTIONS)
    if (_ny_identity_link_opts)
      target_link_options(ny-module-identity-test PRIVATE
        ${_ny_identity_link_opts})
    endif()
    set_target_properties(ny-module-identity-test PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    if(UNIX AND NOT APPLE AND EXISTS "${NYTRIX_ROOT}/build/vendor/lib/host")
      set_property(TARGET ny-module-identity-test PROPERTY BUILD_RPATH "$ORIGIN/../vendor/lib/host")
      target_link_options(ny-module-identity-test PRIVATE "-Wl,--disable-new-dtags")
    endif()
    nytrix_apply_compile_perf_flags(ny-module-identity-test)
    nytrix_apply_link_perf_flags(ny-module-identity-test)
    nytrix_apply_size_flags(ny-module-identity-test)
    if (UNIX AND NOT APPLE)
      target_compile_definitions(ny-module-identity-test PRIVATE _GNU_SOURCE)
    endif()
    if (WIN32)
      target_compile_definitions(ny-module-identity-test PRIVATE
        _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS PATH_MAX=4096)
      nytrix_stage_windows_runtime_deps(ny-module-identity-test)
    endif()

    if (BUILD_TESTING)
      add_test(NAME ny-module-identity COMMAND ny-module-identity-test)
      set_tests_properties(ny-module-identity PROPERTIES
        WORKING_DIRECTORY "${NYTRIX_ROOT}" TIMEOUT 30)
      file(MAKE_DIRECTORY "${NY_TEST_LANG_NY_GENERATED_DIR}")
      set(NY_TEST_LANG_NY_DEBUG_FIXTURE
        "${NY_TEST_LANG_NY_GENERATED_DIR}/stdlib-debug-path.ny")
      file(WRITE "${NY_TEST_LANG_NY_DEBUG_FIXTURE}"
        "use core.str\n\nprint(\"registered stdlib debug path\".len)\n")
      set(NY_TEST_LANG_NY_DEBUG_IR
        "${NY_TEST_LANG_NY_GENERATED_DIR}/stdlib-debug.ll")
      add_test(NAME ny-stdlib-debug-path
        COMMAND "${CMAKE_COMMAND}"
          "-DNY_NY_RUN_DEBUG_PATH=ON"
          "-DNY_NY_FULL=$<TARGET_FILE:ny-full>"
          "-DNY_NY_DEBUG_OUTPUT=${NY_TEST_LANG_NY_DEBUG_IR}"
          "-DNY_NY_FIXTURE=${NY_TEST_LANG_NY_DEBUG_FIXTURE}"
          "-DNY_NY_ROOT=${NYTRIX_ROOT}"
          -P "${CMAKE_CURRENT_LIST_FILE}")
      set_tests_properties(ny-stdlib-debug-path PROPERTIES TIMEOUT 120)
    endif()
  endif()
endif()

# In project mode this file registers Ny tests and returns; in script mode
# NYC is supplied and the C frontend corpus runner continues below.
if (NOT DEFINED NYC)
  return()
endif()

if(NOT EXISTS "${NYC}")
  message(FATAL_ERROR "nyc compiler does not exist: ${NYC}")
endif()
if(NOT EXISTS "${CC}")
  message(FATAL_ERROR "C reference compiler does not exist: ${CC}")
endif()
include(CMakeParseArguments)

set(NY_C_CORPUS_DIR "${SOURCE_DIR}/c")
file(GLOB_RECURSE NY_C_CORPUS_FILES LIST_DIRECTORIES false
  "${NY_C_CORPUS_DIR}/*")
list(SORT NY_C_CORPUS_FILES)
set(_ny_c_corpus_names)
foreach(_corpus_file IN LISTS NY_C_CORPUS_FILES)
  get_filename_component(_corpus_name "${_corpus_file}" NAME)
  list(FIND _ny_c_corpus_names "${_corpus_name}" _existing_name)
  if(NOT _existing_name EQUAL -1)
    message(FATAL_ERROR
      "C corpus files must have unique basenames when referenced by the runner: ${_corpus_name}")
  endif()
  list(APPEND _ny_c_corpus_names "${_corpus_name}")
  set("NY_C_CORPUS_${_corpus_name}" "${_corpus_file}")
endforeach()

file(MAKE_DIRECTORY "${BINARY_DIR}")

# C regression sources live directly under lang/c/; this script owns build
# outputs and generated artifacts.

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

# Require fresh artifacts, bounded execution, and exact program output on each
# x86-64 route. A machine disassembly is positive evidence for main because
# functions that bypass machine lowering silently use the direct encoder.
function(nyc_check_x64_parity name expected)
  set(source "${BINARY_DIR}/${name}.c")
  foreach(route IN ITEMS assembly direct machine)
    set(compiler "${NYC}")
    set(artifact "${BINARY_DIR}/${name}-${route}.o")
    set(mode -c)
    if(route STREQUAL "assembly")
      set(artifact "${BINARY_DIR}/${name}-${route}.s")
      set(mode -S)
    elseif(route STREQUAL "direct")
      set(compiler "${CMAKE_COMMAND}" -E env NYC_NO_MACH_OBJECT=1 "${NYC}")
    else()
      set(compiler "${CMAKE_COMMAND}" -E env NYC_NO_MACH_OBJECT=0
        NYC_TRACE_MACHINE_FALLBACK=1 "${NYC}" --mixed-disasm=main)
    endif()
    nyc_execute_output("${name} ${route} compilation failed" "${artifact}"
      COMMAND ${compiler} -O2 ${mode} "${source}" -o "${artifact}" TIMEOUT 15)
    if(route STREQUAL "machine" AND
       (stderr MATCHES "NYC object fallback" OR NOT stderr MATCHES "== main  src="))
      message(FATAL_ERROR
        "${name} did not use the expected machine object route\n${stderr}")
    endif()

    set(program "${BINARY_DIR}/${name}-${route}")
    nyc_execute_output("${name} ${route} link failed" "${program}"
      COMMAND "${CC}" "${artifact}" -o "${program}" TIMEOUT 15)
    nyc_execute_ok("${name} ${route} execution failed (\${status})"
      COMMAND "${program}" TIMEOUT 5)
    if(NOT "${stdout}" STREQUAL "${expected}" OR NOT "${stderr}" STREQUAL "")
      message(FATAL_ERROR
        "${name} ${route} output mismatch\nExpected stdout:\n${expected}"
        "Actual stdout:\n${stdout}Actual stderr:\n${stderr}")
    endif()
  endforeach()
endfunction()

# Common executable route: compile one corpus case to assembly, link it with
# optional host-side oracle sources, then check its exit status. Keep tests
# that inspect assembly/object metadata explicit at the call site.
function(nyc_compile_asm_run name source)
  cmake_parse_arguments(CASE "" "EXPECTED_EXIT;TIMEOUT"
    "NYC_FLAGS;LINK_FLAGS;HOST_SOURCES" ${ARGN})
  if(NOT CASE_EXPECTED_EXIT)
    set(CASE_EXPECTED_EXIT 0)
  endif()
  set(_timeout_option)
  if(CASE_TIMEOUT)
    list(APPEND _timeout_option TIMEOUT "${CASE_TIMEOUT}")
  endif()
  set(_assembly "${BINARY_DIR}/${name}.s")
  set(_program "${BINARY_DIR}/${name}")

  nyc_execute_output("nyc ${name} compilation failed" "${_assembly}"
    COMMAND "${NYC}" ${CASE_NYC_FLAGS} -S "${source}" -o "${_assembly}")
  nyc_execute_output("${name} link failed" "${_program}"
    COMMAND "${CC}" ${CASE_LINK_FLAGS} "${_assembly}" ${CASE_HOST_SOURCES}
      -o "${_program}")

  execute_process(
    COMMAND "${_program}" ${_timeout_option}
    RESULT_VARIABLE _result OUTPUT_VARIABLE _stdout ERROR_VARIABLE _stderr)
  if(NOT "${_result}" STREQUAL "${CASE_EXPECTED_EXIT}")
    message(FATAL_ERROR
      "${name} returned ${_result}, expected ${CASE_EXPECTED_EXIT}\n"
      "${_stdout}${_stderr}")
  endif()
endfunction()

# Object route for fixtures that only need a successful compile, link, and run.
function(nyc_compile_obj_run name source)
  cmake_parse_arguments(CASE "" "EXPECTED_EXIT;TIMEOUT;OUTPUT_MATCH"
    "NYC_FLAGS;LINK_FLAGS;HOST_SOURCES" ${ARGN})
  if(NOT CASE_EXPECTED_EXIT)
    set(CASE_EXPECTED_EXIT 0)
  endif()
  set(_timeout_option)
  if(CASE_TIMEOUT)
    list(APPEND _timeout_option TIMEOUT "${CASE_TIMEOUT}")
  endif()
  set(_object "${BINARY_DIR}/${name}.o")
  set(_program "${BINARY_DIR}/${name}")

  nyc_execute_output("nyc ${name} object compilation failed" "${_object}"
    COMMAND "${NYC}" ${CASE_NYC_FLAGS} -c "${source}" -o "${_object}")
  nyc_execute_output("${name} link failed" "${_program}"
    COMMAND "${CC}" ${CASE_LINK_FLAGS} ${CASE_HOST_SOURCES} "${_object}"
      -o "${_program}")
  execute_process(COMMAND "${_program}" ${_timeout_option}
    RESULT_VARIABLE _result OUTPUT_VARIABLE _stdout ERROR_VARIABLE _stderr)
  if(NOT "${_result}" STREQUAL "${CASE_EXPECTED_EXIT}")
    message(FATAL_ERROR
      "${name} returned ${_result}, expected ${CASE_EXPECTED_EXIT}\n${_stdout}${_stderr}")
  endif()
  if(CASE_OUTPUT_MATCH AND NOT _stdout MATCHES "${CASE_OUTPUT_MATCH}")
    message(FATAL_ERROR "${name} output mismatch\n${_stdout}${_stderr}")
  endif()
endfunction()

# Function entry is also an incoming edge when the first block is a loop
# header. Bound execution so a folded-away exit fails instead of hanging CI.
nyc_compile_asm_run(loop-entry-range-asm "${NY_C_CORPUS_loop-entry-range.c}"
  TIMEOUT 5 NYC_FLAGS -O1)
nyc_compile_obj_run(loop-entry-range-obj "${NY_C_CORPUS_loop-entry-range.c}"
  TIMEOUT 5 NYC_FLAGS -O2)

# Frontend semantics, declarations, and aggregate layout.
foreach(probe IN ITEMS small-copy-loop-residency declared-copy-definition
                       unevaluated-metadata abstract-callback-parameters
                       static-inline-boundaries driver-pthread
                       void-pointer-increment cleanup-type-attribute
                       atomic-memory-cmpxchg-small dead-inline-asm
                       unreachable-asm asm-add-carry32 asm-add-carry64
                       asm-repeat-stos-widths asm-mull-edx asm-delay-loop
                       asm-mwaitx asm-mulq-positional asm-usercopy-movnti
                       global-incomplete-char-array-rows
                       global-incomplete-integer-array-rows
                       typedef-row-pointer-difference
                       typeof-cast-array-member asm-memory-address
                       asm-memory-output asm-bit-string-rmw asm-fninit-status
                       asm-cmpxchg16b asm-single-movs asm-repeat-cmpsb
                       asm-divl-outputs asm-repeat-copy-tail asm-memory-const-local
                       asm-ip-fast-csum
                       asm-crc32
                       futex-atomic-exception-table-asm
                       aggregate-pointer-self-initializer
                       volatile-pointer-increment
                       aggregate-sizeof-global-scalar-array
                       aggregate-block-tag-scope
                       gnu-omitted-middle-array-extent
                       object-alias-followed-by-external
                       character-array-alias
                       address-of-choose-expr-member
                       typeof-pointer-qualifier-dereference
                       typeof-prefixed-section
                       typeof-aggregate-local
                       automatic-array-designator-extent
                       sizeof-empty-global-array
                       function-pointer-byte-arithmetic
                       generic-function-pointer-call
                       generic-aggregate-pointer-call
                       computed-goto-label-table
                       computed-goto-trampoline
                       aggregate-pointer-index-member-address
                       aggregate-pointer-index-assignment
                       empty-aggregate-assignment
                       c23-auto-min
                       statement-expression-trailing-control
                       indirect-void-conditional
                       typeof-builtin-conditional
                       macro-argument-conditional-close
                       aggregate-array-arrow-member
                       function-pointer-array-null
                       linux-local-static-function-pointer-array
                       function-pointer-array-row-designators
                       integer-cast-array-extent
                       bitfield-log2-constant
                       multidimensional-scalar-array-parameter
                       address-of-multidimensional-array-element
                       typeof-builtin-clz
                       typeof-const-aggregate-pointer
                       static-function-name
                       typeof-cast-binary-precedence
                       void-global-array-member-call
                       aggregate-array-rank4
                       static-pointer-array-range-designator
                       local-aggregate-array-range
                       aggregate-scalar-pointer-to-array
                       local-pointer-to-array-stride
                       aggregate-double-pointer-address
                       compound-literal-zero-fields
                       inline-aggregate-return-capture
                       bool-pointer-call
                       bool-scalar-return
                       atomic-type-specifier
                       static-local-label-address-range
                       inline-asm-unlocked-add-condition
                       runtime-constant-shift-inline-asm
                       runtime-constant-pointer-clamp-inline-asm
                       atomic-bts-condition
                       aggregate-pointer-array-indirection
                       aggregate-pointer-array-two-dimensional-store
                       aggregate-anonymous-member-size
                       global-aggregate-type-alignment
                       aggregate-array-compound-elements
                       function-call-pointer-array-address
                       function-pointer-array-forward-declaration
                       tentative-pointer-array
                       nested-const-function-pointer-parameter
                       transparent-union-pointer-argument
                       parenthesized-global-address-store
                       typedef-nested-pointer-array
                       typedef-pointer-array-decay
                       roundup-pow-two-array-extent
                       static-array-range-string-rows
                       auto-type-aggregate-copy
                       builtin-object-size-array-member
                       generic-container-of-const-mutable
                       typeof-function-pointer-dereference
                       preproc-multiline-comment-continuation
                       address-of-multidimensional-global-aggregate
                       static-self-address-initializer
                       aggregate-pointer-field-decrement
                       typeof-gnu-conditional
                       pointer-array-element-assignment
                       braced-narrow-string-array
                       zero-length-aggregate-global
                       file-scope-diagnostic-pragma
                       global-aggregate-array-compound-literals
                       static-compound-literal-pointer-array
                       static-array-compound-subobject-pointer
                       static-address-of-null-dereference
                       static-null-base-subobject-pointer
                       comma-expression-after-indexed-assignment
                       aggregate-array-compound-literal-call
                       aggregate-array-local-compound-literal
                       array-compound-literal-null-pointer
                       array-compound-literal-fixed-bound
                       pointer-to-incomplete-aggregate-array-field
                       aggregate-member-bitwise-condition
                       inline-asm-endbr-constant
                       static-local-data-dead-function
                       int128-arithmetic
                       int128-member-assignment
                       global-incomplete-aggregate-matrix
                       system-netinet-tcp
                       int128-pointer-parameter
                       int128-signedness-call
                       generic-typedef-long-long
                       aggregate-char-matrix-string-rows
                       switch-aggregate-return
                       conditional-in-function-macro-arguments
                       local-aggregate-pointer-array pointer-attributes
                       aligned-local-aggregate enum-wide-alias
                       global-array-boundaries aggregate-large-array
                       zero-bound-aggregate-global
                       sizeof-member-array offsetof-dynamic-type
                       typeof-incomplete-aggregate-pointer-sizeof
                       pointer-to-bool-assignment
                       aggregate-trailing-attribute
                       prefix-section-placement
                       post-inline-short-circuit
                       aggregate-object-alias
                       file-scope-local-label
                       sectioned-helper-inline
                       dominated-comparison
                       discarded-lvalue-read
                       private-stack-fields
                       function-pointer-aggregate-return
                       indirect-aggregate-return
                       packed-bitfield-aggregate-argument
                       typedef-function-signature
                       driver-no-builtin mixed-void-conditional
                       unused-inline-section noreturn-call
                       noreturn-return-switch)
  set(probe_flags)
  set(probe_env)
  if(probe STREQUAL "aggregate-anonymous-member-size")
    list(APPEND probe_flags -fms-extensions)
  endif()
  if(probe STREQUAL "driver-pthread")
    list(APPEND probe_flags -pthread)
  endif()
  if(probe STREQUAL "mixed-void-conditional")
    list(APPEND probe_env "NYC_KERNEL_COMPAT=1")
  endif()
  nyc_execute_ok(
    [=[nyc ${probe} compilation failed\n${stdout}${stderr}]=]
    COMMAND "${CMAKE_COMMAND}" -E env ${probe_env} "${NYC}" -O3 -S
                ${probe_flags} "${NY_C_CORPUS_${probe}.c}" -o
                "${BINARY_DIR}/${probe}.s"
  )
  nyc_execute_ok(
    [=[${probe} link failed\n${stdout}${stderr}]=]
    COMMAND "${CC}" ${probe_flags} "${BINARY_DIR}/${probe}.s"
                -o "${BINARY_DIR}/${probe}"
  )
  nyc_execute_ok(
    [=[${probe} returned an incorrect result\n${stdout}${stderr}]=]
    COMMAND "${BINARY_DIR}/${probe}"
  )
endforeach()
foreach(probe IN ITEMS prefix-section-placement post-inline-short-circuit
                       aggregate-object-alias character-array-alias
                       file-scope-local-label sectioned-helper-inline
                       dominated-comparison discarded-lvalue-read
                       private-stack-fields linux-function-pointer-xchg
                       xchg-widths asm-cmpxchg16b asm-single-movs
                       asm-bit-string-rmw asm-fninit-status asm-repeat-cmpsb
                       asm-divl-outputs asm-repeat-copy-tail asm-memory-const-local)
  set(probe_flags -O2)
  if(probe STREQUAL "post-inline-short-circuit")
    set(probe_flags -O1 -fconserve-stack)
  endif()
  if(probe STREQUAL "sectioned-helper-inline" OR
     probe STREQUAL "dominated-comparison" OR
     probe STREQUAL "private-stack-fields")
    set(probe_flags -O2 -fconserve-stack)
  endif()
  nyc_execute_ok(
    [=[nyc ${probe} object compilation failed\n${stdout}${stderr}]=]
    COMMAND "${NYC}" ${probe_flags} -c "${NY_C_CORPUS_${probe}.c}"
                -o "${BINARY_DIR}/${probe}.o"
  )
  nyc_execute_ok(
    [=[${probe} object link failed\n${stdout}${stderr}]=]
    COMMAND "${CC}" "${BINARY_DIR}/${probe}.o"
                -o "${BINARY_DIR}/${probe}-object"
  )
  nyc_execute_ok(
    [=[${probe} object returned incorrect results\n${stdout}${stderr}]=]
    COMMAND "${BINARY_DIR}/${probe}-object"
  )
  if(probe STREQUAL "prefix-section-placement" AND READELF)
    foreach(artifact IN ITEMS "${BINARY_DIR}/${probe}"
                              "${BINARY_DIR}/${probe}.o")
      execute_process(
        COMMAND "${READELF}" -n "${artifact}"
        RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
      if(NOT status EQUAL 0 OR NOT stdout MATCHES "NYC[ \t]+0x00000004" OR
         NOT stdout MATCHES "78 56 34 12")
        message(FATAL_ERROR
          "ELF note type or payload was lost in ${artifact}\n${stdout}${stderr}")
      endif()
    endforeach()
  endif()
endforeach()
foreach(route IN ITEMS -c -S)
  set(stem "${BINARY_DIR}/bitfield-select${route}")
  set(suffix o)
  if(route STREQUAL "-S")
    set(suffix s)
  endif()
  nyc_execute_ok(
    [=[bitfield select did not compile (${route})]=]
    COMMAND "${NYC}" -O1 "${route}" "${NY_C_CORPUS_bitfield-select.c}"
            -o "${stem}.${suffix}"
  )
  if(stderr MATCHES "verification failed|continuing unoptimized")
    message(FATAL_ERROR "optimized bitfield select failed verification\n${stderr}")
  endif()
  nyc_execute_ok(
    [=[bitfield select did not link (${route})]=]
    COMMAND "${CC}" "${stem}.${suffix}" -o "${stem}"
  )
  nyc_execute_ok(
    [=[bitfield select returned the wrong result (${route})]=]
    COMMAND "${stem}"
  )
endforeach()

# Stack protection: execute actual corruption through both native routes.
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
  foreach(guard IN ITEMS global global-symbol fs fs-symbol gs-offset gs-symbol disabled)
    set(guard_flags -fno-stack-protector -fstack-protector-strong)
    set(guard_mode 1)
    set(ssp_mode 2)
    set(guard_offset 40)
    if(guard MATCHES "^global")
      set(guard_mode 0)
      set(ssp_mode 3)
      list(APPEND guard_flags -fstack-protector-all
           -mstack-protector-guard=global)
      if(guard STREQUAL "global-symbol")
        set(guard_mode 3)
        list(APPEND guard_flags -mstack-protector-guard-symbol=custom_guard)
      endif()
    elseif(guard STREQUAL "disabled")
      set(guard_mode 0)
      set(ssp_mode 0)
      list(APPEND guard_flags -fno-stack-protector)
    elseif(guard STREQUAL "fs-symbol")
      list(APPEND guard_flags
           -mstack-protector-guard-symbol=__nyc_guard_offset)
    elseif(guard MATCHES "^gs-")
      set(guard_mode 2)
      set(guard_offset 56)
      list(APPEND guard_flags -mstack-protector-guard-reg=gs
           -mstack-protector-guard-offset=56)
      if(guard STREQUAL "gs-symbol")
        set(guard_offset 40)
        list(APPEND guard_flags -mstack-protector-guard-offset=16
             -mstack-protector-guard-symbol=__nyc_guard_offset)
      endif()
    endif()
    foreach(route IN ITEMS S c)
      set(stem "${BINARY_DIR}/stack-protector-${guard}-${route}")
      set(frame_flags)
      if(route STREQUAL "c")
        set(frame_flags -fomit-frame-pointer)
      endif()
      nyc_execute_ok(
        [=[stack protector compile failed\n${stdout}${stderr}]=]
        COMMAND "${NYC}" -O2 ${guard_flags} ${frame_flags} "-${route}"
                        "-DNYC_EXPECT_SSP=${ssp_mode}"
                        "${NY_C_CORPUS_stack-protector.c}" -o "${stem}.o"
      )
      if(route STREQUAL "S")
        file(RENAME "${stem}.o" "${stem}.s")
        set(input "${stem}.s")
      else()
        set(input "${stem}.o")
      endif()
      nyc_execute_ok(
        [=[stack protector link failed\n${stdout}${stderr}]=]
        COMMAND "${CC}" -fno-stack-protector -no-pie "-DGUARD_MODE=${guard_mode}"
                        "-DGUARD_OFFSET=${guard_offset}"
                        "${NY_C_CORPUS_stack-protector-host.c}" "${input}"
                        -o "${stem}"
      )
      foreach(action IN ITEMS normal corrupt)
        set(arguments)
        set(expected 0)
        if(action STREQUAL "corrupt")
          set(arguments corrupt)
          set(expected 93)
          if(guard STREQUAL "disabled")
            set(expected 2)
          endif()
        endif()
        execute_process(COMMAND "${stem}" ${arguments}
          RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
        if(NOT status EQUAL expected)
          message(FATAL_ERROR
            "stack protector ${guard}/${route}/${action}: ${status}, expected ${expected}\n${stdout}${stderr}")
        endif()
      endforeach()
      if(guard MATCHES "^global")
        execute_process(COMMAND "${stem}" scalar RESULT_VARIABLE status)
        if(NOT status EQUAL 93)
          message(FATAL_ERROR "stack-protector-all missed scalar return: ${status}")
        endif()
      endif()
      if(guard STREQUAL "global")
        nyc_execute_ok(
          [=[returning stack-protector hook test did not link]=]
          COMMAND "${CC}" -fno-stack-protector -no-pie -DGUARD_MODE=0
                          -DGUARD_HOOK_RETURNS
                          "${NY_C_CORPUS_stack-protector-host.c}" "${input}"
                          -o "${stem}-returning-hook"
        )
        execute_process(COMMAND "${stem}-returning-hook" scalar
          RESULT_VARIABLE status)
        if(NOT status STREQUAL "94")
          message(FATAL_ERROR "returning stack-protector hook did not trap: ${status}")
        endif()
      endif()
    endforeach()
  endforeach()
endif()

# Profiling entry hooks: -pg calls mcount, -mfentry calls __fentry__, and
# no_instrument_function opts a function out.
file(WRITE "${BINARY_DIR}/profile-entry.c"
  "__attribute__((noinline)) int profiled(int a) { return a + 1; }\n")
file(WRITE "${BINARY_DIR}/profile-entry-skip.c"
  "__attribute__((no_instrument_function)) int skipped(int a) { return a + 2; }\n")
nyc_execute_ok(
  [=[-pg entry instrumentation failed\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -pg -S "${BINARY_DIR}/profile-entry.c"
          -o "${BINARY_DIR}/profile-entry-pg.s"
)
file(READ "${BINARY_DIR}/profile-entry-pg.s" profile_entry_pg)
if(NOT profile_entry_pg MATCHES "call[ \t]+mcount")
  message(FATAL_ERROR "-pg did not emit the mcount entry hook")
endif()
nyc_execute_ok(
  [=[-pg -mfentry entry instrumentation failed\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -pg -mfentry -S "${BINARY_DIR}/profile-entry.c"
          -o "${BINARY_DIR}/profile-entry-fentry.s"
)
file(READ "${BINARY_DIR}/profile-entry-fentry.s" profile_entry_fentry)
if(NOT profile_entry_fentry MATCHES "call[ \t]+__fentry__" OR
   profile_entry_fentry MATCHES "call[ \t]+mcount")
  message(FATAL_ERROR
    "-mfentry did not replace the mcount entry hook with __fentry__")
endif()
nyc_execute_ok(
  [=[no_instrument_function opt-out failed to compile\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -pg -S "${BINARY_DIR}/profile-entry-skip.c"
          -o "${BINARY_DIR}/profile-entry-skip.s"
)
file(READ "${BINARY_DIR}/profile-entry-skip.s" profile_entry_skip)
if(profile_entry_skip MATCHES "call[ \t]+mcount")
  message(FATAL_ERROR "no_instrument_function still emitted a profiling hook")
endif()
nyc_execute_ok(
  [=[-mfentry without -pg should not instrument\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -mfentry -S "${BINARY_DIR}/profile-entry.c"
          -o "${BINARY_DIR}/profile-entry-plain.s"
)
file(READ "${BINARY_DIR}/profile-entry-plain.s" profile_entry_plain)
if(profile_entry_plain MATCHES "__fentry__")
  message(FATAL_ERROR "-mfentry instrumented a function without -pg")
endif()

# Frame-pointer omission in object mode: ordinary call, loop, array, and leaf
# shapes must not open an rbp frame. The entry byte is checked at runtime
# instead of relying on objdump being installed.
file(WRITE "${BINARY_DIR}/omit-fp-defs.c" [=[
int callee(int);
int calls(int x) { return callee(x); }
int loop(int n) { int s = 0; for (int i = 0; i < n; ++i) s += i; return s; }
int array(int x) { int a[4]; a[0] = x; return a[0]; }
int leaf(void) { return 4; }
int vol(void) { volatile int v = 3; v = v + 1; return v; }
]=])
file(WRITE "${BINARY_DIR}/omit-fp-main.c" [=[
#include <stdio.h>
int callee(int x) { return x + 1; }
int calls(int); int loop(int); int array(int); int leaf(void); int vol(void);
static int opens_frame(const void *fn) {
  return ((const unsigned char *)fn)[0] == 0x55;
}
int main(void) {
  printf("%d %d %d %d %d %d\n", opens_frame((const void *)calls),
         opens_frame((const void *)loop), opens_frame((const void *)array),
         opens_frame((const void *)leaf), opens_frame((const void *)vol),
         calls(1) + loop(3) + array(2) + leaf() + vol());
  return 0;
}
]=])
nyc_execute_ok(
  [=[omit-frame-pointer definitions failed\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -fomit-frame-pointer -c "${BINARY_DIR}/omit-fp-defs.c"
          -o "${BINARY_DIR}/omit-fp-defs.o"
)
nyc_execute_ok(
  [=[omit-frame-pointer caller failed\n${stdout}${stderr}]=]
  COMMAND "${CC}" -c "${BINARY_DIR}/omit-fp-main.c"
          -o "${BINARY_DIR}/omit-fp-main.o"
)
nyc_execute_ok(
  [=[omit-frame-pointer link failed\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/omit-fp-defs.o" "${BINARY_DIR}/omit-fp-main.o"
          -o "${BINARY_DIR}/omit-fp"
)
execute_process(
  COMMAND "${BINARY_DIR}/omit-fp"
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT status EQUAL 0 OR NOT stdout STREQUAL "0 0 0 0 0 15\n")
  message(FATAL_ERROR
    "-fomit-frame-pointer left an rbp frame in object mode: ${stdout}${stderr}")
endif()

# Assembly-route PIC must use GOT-relative data addressing like the object
# route, and stay direct without -fPIC.
file(WRITE "${BINARY_DIR}/pic-asm.c" [=[
int g = 7;
int read(void) { return g; }
]=])
nyc_execute_ok(
  [=[-fPIC -S compilation failed\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -fPIC -S "${BINARY_DIR}/pic-asm.c"
          -o "${BINARY_DIR}/pic-asm.s"
)
file(READ "${BINARY_DIR}/pic-asm.s" pic_asm)
if(NOT pic_asm MATCHES "GOTPCREL")
  message(FATAL_ERROR
    "assembly-route -fPIC did not use GOTPCREL data addressing")
endif()
nyc_execute_ok(
  [=[-S compilation failed\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -S "${BINARY_DIR}/pic-asm.c"
          -o "${BINARY_DIR}/pic-asm-plain.s"
)
file(READ "${BINARY_DIR}/pic-asm-plain.s" pic_asm_plain)
if(pic_asm_plain MATCHES "GOTPCREL")
  message(FATAL_ERROR "non-PIC assembly used GOTPCREL data addressing")
endif()

if(READELF)
  set(hidden_pic "${BINARY_DIR}/pic-hidden-visibility.o")
  nyc_execute_output("hidden PIC compilation failed" "${hidden_pic}"
    COMMAND "${NYC}" -Os -fPIC -c "${NY_C_CORPUS_pic-hidden-visibility.c}"
      -o "${hidden_pic}")
  nyc_execute_ok("could not inspect hidden PIC object"
    COMMAND "${READELF}" -rWs "${hidden_pic}")
  if(NOT stdout MATCHES "R_X86_64_PC32[^\n]* startup_data" OR
     stdout MATCHES "GOTPCREL[^\n]* startup_data" OR
     NOT stdout MATCHES "GOTPCREL[^\n]* interposable_data" OR
     NOT stdout MATCHES "HIDDEN[^\n]* startup_data" OR
     NOT stdout MATCHES "HIDDEN[^\n]* startup_address" OR
     NOT stdout MATCHES "HIDDEN[^\n]* startup_value" OR
     stdout MATCHES "UND[^\n]* dead_data" OR
     stdout MATCHES "UND[^\n]* local_startup_address" OR
     NOT stdout MATCHES "DEFAULT[^\n]* interposable_address")
    message(FATAL_ERROR "visibility pragma produced incorrect PIC relocations or binding\n${stdout}")
  endif()
endif()

# Unsigned arithmetic, 16/32-bit widths, and named sections must agree across
# the assembly and direct-object routes and with the pinned reference output.
file(WRITE "${BINARY_DIR}/width-parity.c" [=[
#include <stdio.h>
__attribute__((section("mydata"))) int sec_g = 5;
__attribute__((section(".mytext"))) int secf(void) { return sec_g; }
int pointer_before(const void *a, const void *b) { return a < b; }
int unsigned_before(unsigned a, unsigned b) { return a < b; }
int bool_local(unsigned flags, double from_float) {
  _Bool selected = flags & 16u;
  int pools[] = {7, 9};
  int pool = pools[selected];
  selected = flags & 256u;
  int assigned = selected;
  selected += 2;
  _Bool converted = from_float;
  return pool * 1000 + assigned * 100 + selected * 10 + converted;
}
int main(void) {
  unsigned a = 0xffffffffu, q, r;
  unsigned short s = 65535; short ss = -32768; unsigned char uc = 250;
  unsigned long long u64 = 0xffffffffffffffffULL;
  unsigned short s2 = (unsigned short)(s + 1), prod = (unsigned short)(s * 3u);
  q = a / 3u; r = a % 3u;
  printf("%u %u %u %d %u %d\n", q, r, a >> 31, (int)ss, (unsigned)s2, (int)uc);
  printf("%llu %llu %u %u %d\n", u64 / 10u, u64 % 10u,
         (unsigned)(s / 7u), (unsigned)prod, (int)(s2 < 2));
  printf("%d %u %d\n", secf(), (unsigned)(uc + 10u) & 0xffu, (int)(ss >> 3));
  const void *upper = (const void *)0xffff800000000002ULL;
  const void *lower = (const void *)254UL;
  printf("%d %d\n", pointer_before(upper, lower), pointer_before(lower, upper));
  printf("%d %d\n", unsigned_before(0x80000000u, 1),
         unsigned_before(1, 0xffffffffu));
  printf("%d %d\n", bool_local(272u, 0.25), bool_local(0u, -0.0));
  return 0;
}
]=])
set(width_expected "1431655765 0 1 -32768 0 250\n1844674407370955161 5 9362 65533 1\n5 4 -4096\n0 1\n0 1\n9111 7010\n")
nyc_check_x64_parity(width-parity "${width_expected}")
file(READ "${BINARY_DIR}/width-parity-assembly.s" width_asm)
string(FIND "${width_asm}" ".section \"mydata\"" width_asm_data)
string(FIND "${width_asm}" ".section \".mytext\"" width_asm_text)
if(width_asm_data EQUAL -1 OR width_asm_text EQUAL -1)
  message(FATAL_ERROR "assembly route dropped a named section")
endif()
if(READELF)
  foreach(route IN ITEMS direct machine)
    nyc_execute_ok("could not read width-parity ${route} sections"
      COMMAND "${READELF}" -SW "${BINARY_DIR}/width-parity-${route}.o")
    if(NOT stdout MATCHES "[ \t]mydata[ \t]" OR
       NOT stdout MATCHES "[ \t][.]mytext[ \t]")
      message(FATAL_ERROR "${route} route dropped a named section: ${stdout}${stderr}")
    endif()
  endforeach()
endif()

# Floating point, a by-value struct with a 16-bit member and an address-taken
# member, arrays, and break/continue/switch control flow must agree across both
# routes and with the pinned reference output.
file(WRITE "${BINARY_DIR}/mixed-shapes.c" [=[
#include <stdio.h>
struct P { int x; short y; };
struct P make(int a, int b) { struct P p; p.x = a; p.y = (short)b; return p; }
int sumP(struct P p) { int *q = &p.x; *q += 3; return p.x + (int)p.y; }
int loops(int n) {
  int s = 0;
  for (int i = 0; i < n; ++i) { if (i == 3) continue; s += i * 3; }
  int j = 0;
  while (j < n) { ++j; if (j > 7) break; s += 1; }
  switch (n % 3) { case 0: s += 2; break; case 1: s -= 1; break; default: s += 5; }
  return s;
}
int array_sum(int a) {
  int arr[5];
  for (int i = 0; i < 5; ++i) arr[i] = i * a;
  int s = 0;
  for (int i = 0; i < 5; ++i) s += arr[i];
  return s;
}
int main(void) {
  int A = 17, B = 23;
  double D = A * 1.25 + B * 0.5;
  double E = D / 2.0;
  float F = (float)(D * 0.5f);
  int I = (int)(E * 4.0);
  struct P p = make(A + 1, B - 2);
  printf("%d %d %.4f %.4f %.4f %d %d %d\n", sumP(p), loops(A % 9), D, E,
         (double)F, I, array_sum(B), A < B);
  return 0;
}
]=])
set(mixed_expected "42 87 32.7500 16.3750 16.3750 65 230 1\n")
nyc_check_x64_parity(mixed-shapes "${mixed_expected}")

# The assembly-route machine text emitter must encode the 16/32-bit, unsigned,
# and SHR shapes C lowering produces, so these functions must not fall back to
# the NYIR emitter and their output must stay correct.
file(WRITE "${BINARY_DIR}/asm-machine-reject.c" [=[
int leaf(int x) { return x + 1; }
int caller(int x) { return leaf(x) * 2; }
unsigned udiv(unsigned a, unsigned b) { return a / b; }
unsigned ushr(unsigned a) { return a >> 3; }
int narrow(short a) { return a + 1; }
int main(void) {
  return caller(3) == 8 && udiv(9, 2) == 4 && ushr(16) == 2 &&
                 narrow(-2) == -1
             ? 0
             : 1;
}
]=])
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env NYC_TRACE_MACHINE_FALLBACK=1
          "${NYC}" -O2 -S "${BINARY_DIR}/asm-machine-reject.c"
          -o "${BINARY_DIR}/asm-machine-reject.s"
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT status EQUAL 0 OR stderr MATCHES "machine fallback")
  message(FATAL_ERROR
    "assembly machine form fell back for C width shapes: ${stdout}${stderr}")
endif()
nyc_execute_ok(
  [=[assembly machine rejection output did not link\n${stdout}${stderr}]=]
  COMMAND "${CC}" -o "${BINARY_DIR}/asm-machine-reject"
          "${BINARY_DIR}/asm-machine-reject.s"
)
nyc_execute_ok(
  [=[assembly machine rejection fallback output was wrong\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/asm-machine-reject"
)

# Assembly-route stack guards and extern retpoline thunks must be emitted and
# remain executable.
file(WRITE "${BINARY_DIR}/asm-hardening.c" [=[
#include <stdio.h>
int (*volatile fp)(int) = 0;
int inc(int x) { return x + 1; }
int call_through(int x) { return fp ? fp(x) : x; }
int guarded(const char *s) { char b[8]; b[0] = s[0]; b[1] = 0; return b[0]; }
int main(void) {
  fp = inc;
  printf("%d %d\n", call_through(1), guarded("A"));
  return 0;
}
]=])
file(WRITE "${BINARY_DIR}/asm-hardening-stub.c" [=[
__asm__(".globl __x86_indirect_thunk_rax\n"
        "__x86_indirect_thunk_rax:\n"
        "jmp *%rax\n"
        ".globl __x86_indirect_thunk_r11\n"
        "__x86_indirect_thunk_r11:\n"
        "jmp *%r11\n");
]=])
nyc_execute_ok(
  [=[assembly hardening compilation failed\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -S -fstack-protector-all
          -mindirect-branch=thunk-extern "${BINARY_DIR}/asm-hardening.c"
          -o "${BINARY_DIR}/asm-hardening.s"
)
file(READ "${BINARY_DIR}/asm-hardening.s" hardening_asm)
string(FIND "${hardening_asm}" "__stack_chk_fail" hardening_guard)
string(FIND "${hardening_asm}" "__x86_indirect_thunk_" hardening_thunk)
if(hardening_guard EQUAL -1 OR hardening_thunk EQUAL -1)
  message(FATAL_ERROR "assembly route dropped its stack guard or retpoline thunk")
endif()
nyc_execute_ok(
  [=[assembly hardening link failed\n${stdout}${stderr}]=]
  COMMAND "${CC}" -o "${BINARY_DIR}/asm-hardening"
          "${BINARY_DIR}/asm-hardening.s"
          "${BINARY_DIR}/asm-hardening-stub.c"
)
execute_process(
  COMMAND "${BINARY_DIR}/asm-hardening"
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT status EQUAL 0 OR NOT stdout STREQUAL "2 65\n")
  message(FATAL_ERROR
    "assembly-route guard/retpoline behavior mismatch: ${stdout}${stderr}")
endif()

# Complex component access through a pointer, including address-of a complex
# local. Offsets are 0 and 8 for double complex and 0 and 4 for float complex.
file(WRITE "${BINARY_DIR}/complex-components.c" [=[
#include <stdio.h>
struct S { double _Complex z; };
void store(double _Complex *p, double re, double im) {
  __real__ *p = re;
  __imag__ *p = im;
}
double load_real(double _Complex *p) { return __real__ *p; }
double load_imag(double _Complex *p) { return __imag__ *p; }
void fstore(float _Complex *p, float re, float im) {
  __real__ *p = re;
  __imag__ *p = im;
}
float fload_real(float _Complex *p) { return __real__ *p; }
float fload_imag(float _Complex *p) { return __imag__ *p; }
int main(void) {
  double _Complex z;
  float _Complex w;
  store(&z, 1.25, -2.5);
  fstore(&w, 0.5f, -1.5f);
  printf("%g %g\n", load_real(&z), load_imag(&z));
  printf("%g %g\n", fload_real(&w), fload_imag(&w));
  struct S s;
  struct S *sp = &s;
  __real__ sp->z = 1.25;
  __imag__ sp->z = -2.5;
  __real__ s.z += 0.25;
  printf("%g %g\n", __real__ s.z, __imag__ s.z);
  struct S t;
  t.z = s.z;
  printf("%g %g\n", __real__ t.z, __imag__ t.z);
  return 0;
}
]=])
nyc_execute_ok(
  [=[complex component object compilation failed\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -c "${BINARY_DIR}/complex-components.c"
          -o "${BINARY_DIR}/complex-components.o"
)
nyc_execute_ok(
  [=[complex component link failed\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/complex-components.o"
          -o "${BINARY_DIR}/complex-components"
)
execute_process(
  COMMAND "${BINARY_DIR}/complex-components"
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT status EQUAL 0 OR
   NOT stdout STREQUAL "1.25 -2.5\n0.5 -1.5\n1.5 -2.5\n1.5 -2.5\n")
  message(FATAL_ERROR
    "complex component access mismatch: ${stdout}${stderr}")
endif()

# Complex locals are aggregate objects: two eight-byte slots for double
# complex, one for float complex. Copies must survive the optimizer at -O2.
file(WRITE "${BINARY_DIR}/complex-locals.c" [=[
#include <complex.h>
#include <stdio.h>
struct S { double _Complex z; };
static double _Complex cadd(double _Complex a, double _Complex b) {
  return a + b;
}
static float _Complex fcadd(float _Complex a, float _Complex b) {
  return a + b;
}
static void complex_copy(double _Complex *d, double _Complex *s) { *d = *s; }
int main(void) {
  double _Complex x, y;
  __real__ x = 1.5;
  __imag__ x = 2.5;
  y = x;
  printf("%zu %zu %g %g %g %g\n", sizeof x, sizeof y, __real__ y, __imag__ y,
         __real__ x, __imag__ x);
  __real__ y += 0.25;
  printf("%g %g\n", __real__ y, __imag__ y);
  float _Complex a, b;
  __real__ a = 0.5f;
  __imag__ a = -1.5f;
  b = a;
  printf("%g %g\n", __real__ b, __imag__ b);
  struct S s, t;
  __real__ s.z = 1.25;
  __imag__ s.z = -2.5;
  t.z = s.z;
  printf("%g %g\n", __real__ t.z, __imag__ t.z);
  double _Complex u, v;
  __real__ u = 3.5;
  __imag__ u = -4.5;
  complex_copy(&v, &u);
  printf("%g %g\n", __real__ v, __imag__ v);
  double _Complex p, q, r;
  __real__ p = 1.5;
  __imag__ p = 2.5;
  __real__ q = 0.5;
  __imag__ q = -4.0;
  r = p + q;
  printf("%g %g\n", __real__ r, __imag__ r);
  r = p - q;
  printf("%g %g\n", __real__ r, __imag__ r);
  float _Complex fp, fq, fr;
  __real__ fp = 1.5f;
  __imag__ fp = 2.5f;
  __real__ fq = 0.5f;
  __imag__ fq = -4.0f;
  fr = fp + fq;
  printf("%g %g\n", __real__ fr, __imag__ fr);
  r = p * q;
  printf("%g %g\n", __real__ r, __imag__ r);
  r = p / q;
  printf("%g %g\n", __real__ r, __imag__ r);
  r = cadd(p, q);
  printf("%g %g\n", __real__ r, __imag__ r);
  fr = fcadd(fp, fq);
  printf("%g %g\n", __real__ fr, __imag__ fr);
  fr = fp * fq;
  printf("%g %g\n", __real__ fr, __imag__ fr);
  fr = fp / fq;
  printf("%g %g\n", __real__ fr, __imag__ fr);
  double _Complex lit;
  lit = 2.0i;
  printf("%g %g\n", __real__ lit, __imag__ lit);
  lit = 2.0;
  printf("%g %g\n", __real__ lit, __imag__ lit);
  lit = 3;
  printf("%g %g\n", __real__ lit, __imag__ lit);
  float _Complex flit;
  flit = 1.5fi;
  printf("%g %g\n", __real__ flit, __imag__ flit);
  double _Complex dfromf;
  dfromf = 1.0iF;
  printf("%g %g\n", __real__ dfromf, __imag__ dfromf);
  float _Complex ffromd;
  ffromd = 2.0i;
  printf("%g %g\n", __real__ ffromd, __imag__ ffromd);
  double _Complex initd = 2.0i;
  printf("%g %g\n", __real__ initd, __imag__ initd);
  float _Complex initf = 1.5fi;
  printf("%g %g\n", __real__ initf, __imag__ initf);
  double _Complex initv = 4.0;
  printf("%g %g\n", __real__ initv, __imag__ initv);
  double _Complex initcross = 3.0iF;
  printf("%g %g\n", __real__ initcross, __imag__ initcross);
  double _Complex mixc = 2.0 * I;
  printf("%g %g\n", __real__ mixc, __imag__ mixc);
  double _Complex sqc = I * I;
  printf("%g %g\n", __real__ sqc, __imag__ sqc);
  double _Complex negc = -(2.0 + 3.0i);
  printf("%g %g\n", __real__ negc, __imag__ negc);
  printf("%d %d\n", mixc == mixc, mixc != sqc);
  return 0;
}
]=])
foreach(complex_opt IN ITEMS -O0 -O2)
  string(REPLACE "-" "" complex_tag "${complex_opt}")
  nyc_execute_ok(
    [=[complex local object compilation failed\n${stdout}${stderr}]=]
    COMMAND "${NYC}" ${complex_opt} -c "${BINARY_DIR}/complex-locals.c"
            -o "${BINARY_DIR}/complex-locals-${complex_tag}.o"
  )
  nyc_execute_ok(
    [=[complex local link failed\n${stdout}${stderr}]=]
    COMMAND "${CC}" "${BINARY_DIR}/complex-locals-${complex_tag}.o"
            -o "${BINARY_DIR}/complex-locals-${complex_tag}"
  )
  execute_process(
    COMMAND "${BINARY_DIR}/complex-locals-${complex_tag}"
    RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
  if(NOT status EQUAL 0 OR
     NOT stdout STREQUAL
         "16 16 1.5 2.5 1.5 2.5\n1.75 2.5\n0.5 -1.5\n1.25 -2.5\n3.5 -4.5\n2 -1.5\n1 6.5\n2 -1.5\n10.75 -4.75\n-0.569231 0.446154\n2 -1.5\n2 -1.5\n10.75 -4.75\n-0.569231 0.446154\n0 2\n2 0\n3 0\n0 1.5\n0 1\n0 2\n0 2\n0 1.5\n4 0\n0 3\n0 2\n-1 0\n-2 -3\n1 1\n")
    message(FATAL_ERROR
      "complex local value mismatch at ${complex_opt}: ${stdout}${stderr}")
  endif()
endforeach()

# Unsupported complex forms must stay hard diagnostics, never a scalar or an
# unclassified aggregate substitute.
file(WRITE "${BINARY_DIR}/complex-unsupported.c" [=[
void bad(void) { double _Complex a, b; a = a % b; }
]=])
execute_process(
  COMMAND "${NYC}" -O2 -c "${BINARY_DIR}/complex-unsupported.c"
          -o "${BINARY_DIR}/complex-unsupported.o"
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(status EQUAL 0 OR
   NOT stderr MATCHES "unsupported complex binary operator")
  message(FATAL_ERROR
    "unsupported complex form was not rejected:\n${stdout}${stderr}")
endif()
file(WRITE "${BINARY_DIR}/complex-long-double.c" [=[
long double _Complex z;
long double real_part(void) { return __real__ z; }
]=])
execute_process(
  COMMAND "${NYC}" -O2 -c "${BINARY_DIR}/complex-long-double.c"
          -o "${BINARY_DIR}/complex-long-double.o"
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(status EQUAL 0 OR
   NOT stderr MATCHES "requires a double or float complex object")
  message(FATAL_ERROR
    "long double complex component was not rejected:\n${stdout}${stderr}")
endif()

# Cross-compiler ABI: NYC-compiled and host-compiled counterparts must
# interoperate in both directions for INTEGER/INTEGER, mixed classes, SSE/SSE, and
# MEMORY-class (sret) aggregate returns. Compiling both sides with NYC hides
# a wrong but self-consistent convention, so the host compiler is the oracle.
file(WRITE "${BINARY_DIR}/abi-cross-defs.c" [=[
struct II { const char *p; unsigned long n; };
struct MI { long i; double d; };
struct IM { double d; long i; };
struct MM { double a, b; };
struct BIG { long a, b, c; double d; };
struct II f_ii(struct II v) { v.p += 1; v.n *= 3; return v; }
struct MI f_mi(struct MI v) { v.i += 1; v.d *= 2; return v; }
struct IM f_im(struct IM v) { v.d *= 2; v.i -= 1; return v; }
struct MM f_mm(struct MM v) { v.a += v.b; v.b -= 1; return v; }
struct BIG f_big(struct BIG v) { v.a += 1; v.d += 2; return v; }
]=])
file(WRITE "${BINARY_DIR}/abi-cross-main.c" [=[
#include <stdio.h>
struct II { const char *p; unsigned long n; };
struct MI { long i; double d; };
struct IM { double d; long i; };
struct MM { double a, b; };
struct BIG { long a, b, c; double d; };
struct II f_ii(struct II v);
struct MI f_mi(struct MI v);
struct IM f_im(struct IM v);
struct MM f_mm(struct MM v);
struct BIG f_big(struct BIG v);
int main(void) {
  struct II z = f_ii((struct II){"abc", 257});
  struct MI a = f_mi((struct MI){3, 1.5});
  struct IM m = f_im((struct IM){2.25, 9});
  struct MM b = f_mm((struct MM){2.5, 4.0});
  struct BIG c = f_big((struct BIG){1, 2, 3, 4.5});
  printf("%s %lu | %ld %g | %g %ld | %g %g | %ld %ld %ld %g\n",
         z.p, z.n, a.i, a.d, m.d, m.i, b.a, b.b, c.a, c.b, c.c, c.d);
  return 0;
}
]=])
set(abi_cross_expected "bc 771 | 4 3 | 4.5 8 | 6.5 3 | 2 2 3 6.5\n")
nyc_execute_ok(
  [=[cross ABI: NYC callees compilation failed\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -c "${BINARY_DIR}/abi-cross-defs.c"
          -o "${BINARY_DIR}/abi-cross-defs-nyc.o"
)
nyc_execute_ok(
  [=[cross ABI: host caller compilation failed\n${stdout}${stderr}]=]
  COMMAND "${CC}" -O2 -c "${BINARY_DIR}/abi-cross-main.c"
          -o "${BINARY_DIR}/abi-cross-main-cc.o"
)
nyc_execute_ok(
  [=[cross ABI: NYC callee link failed\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/abi-cross-defs-nyc.o"
          "${BINARY_DIR}/abi-cross-main-cc.o"
          -o "${BINARY_DIR}/abi-cross-nyc-callee"
)
execute_process(
  COMMAND "${BINARY_DIR}/abi-cross-nyc-callee"
  RESULT_VARIABLE abi_callee_status OUTPUT_VARIABLE abi_callee_stdout
  ERROR_VARIABLE abi_callee_stderr)
if(NOT abi_callee_status EQUAL 0 OR
   NOT abi_callee_stdout STREQUAL abi_cross_expected)
  message(FATAL_ERROR
    "cross ABI: NYC callees\nexpected: ${abi_cross_expected}actual:\n${abi_callee_stdout}${abi_callee_stderr}")
endif()
nyc_execute_ok(
  [=[cross ABI: host callees compilation failed\n${stdout}${stderr}]=]
  COMMAND "${CC}" -O2 -c "${BINARY_DIR}/abi-cross-defs.c"
          -o "${BINARY_DIR}/abi-cross-defs-cc.o"
)
nyc_execute_ok(
  [=[cross ABI: NYC caller compilation failed\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -c "${BINARY_DIR}/abi-cross-main.c"
          -o "${BINARY_DIR}/abi-cross-main-nyc.o"
)
nyc_execute_ok(
  [=[cross ABI: NYC caller link failed\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/abi-cross-defs-cc.o"
          "${BINARY_DIR}/abi-cross-main-nyc.o"
          -o "${BINARY_DIR}/abi-cross-nyc-caller"
)
execute_process(
  COMMAND "${BINARY_DIR}/abi-cross-nyc-caller"
  RESULT_VARIABLE abi_caller_status OUTPUT_VARIABLE abi_caller_stdout
  ERROR_VARIABLE abi_caller_stderr)
if(NOT abi_caller_status EQUAL 0 OR
   NOT abi_caller_stdout STREQUAL abi_cross_expected)
  message(FATAL_ERROR
    "cross ABI: NYC caller\nexpected: ${abi_cross_expected}actual:\n${abi_caller_stdout}${abi_caller_stderr}")
endif()
foreach(side IN ITEMS defs main)
  if(side STREQUAL "defs")
    set(host_side main)
  else()
    set(host_side defs)
  endif()
  nyc_execute_ok(
    [=[cross ABI: NYC assembly emission failed\n${stdout}${stderr}]=]
    COMMAND "${NYC}" -O2 -fconserve-stack -S "${BINARY_DIR}/abi-cross-${side}.c"
            -o "${BINARY_DIR}/abi-cross-${side}.s")
  nyc_execute_ok(
    [=[cross ABI: NYC assembly link failed\n${stdout}${stderr}]=]
    COMMAND "${CC}" "${BINARY_DIR}/abi-cross-${side}.s"
            "${BINARY_DIR}/abi-cross-${host_side}-cc.o"
            -o "${BINARY_DIR}/abi-cross-${side}-asm")
  execute_process(
    COMMAND "${BINARY_DIR}/abi-cross-${side}-asm"
    RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
  if(NOT status EQUAL 0 OR NOT stdout STREQUAL abi_cross_expected)
    message(FATAL_ERROR
      "cross ABI: NYC assembly (${side})\nexpected: ${abi_cross_expected}actual:\n${stdout}${stderr}")
  endif()
endforeach()

# SysV aggregate/variadic boundary audit: mixed integer/SSE classes, hidden
# sret, stack aggregates, register-bank rollback, and fixed aggregate named
# arguments before va_start.
file(WRITE "${BINARY_DIR}/sysv-boundary.c" [=[
#include <stdarg.h>
#include <stdio.h>
struct MI { long i; double d; };
struct MM { double a, b; };
struct II { long a, b; };
struct BIG { long a, b, c; double d; };
struct MI f_mi(struct MI v) { v.i += 1; v.d *= 2; return v; }
struct MM f_mm(struct MM v) { v.a += v.b; v.b -= 1; return v; }
struct II f_ii(struct II v) { v.a += v.b; v.b *= 3; return v; }
struct BIG f_big(struct BIG v) { v.a += 1; v.d += 2; return v; }
double spill(int a, int b, int c, int d, int e, struct MI m, struct MM n,
             struct II o, long p) {
  return a + b + c + d + e + m.i + m.d + n.a + n.b + o.a + o.b + p;
}
double stacked(int a, int b, int c, int d, int e, int f, int g,
               struct II h, struct MM i, struct MI j) {
  return a + b + c + d + e + f + g + h.a + h.b + i.a + i.b + j.i + j.d;
}
double va_after(struct MI fixed, int n, ...) {
  va_list ap; va_start(ap, n);
  double s = fixed.i + fixed.d;
  for (int k = 0; k < n; ++k) s += va_arg(ap, double);
  va_end(ap); return s;
}
double va_scalars(int a, double b, int c, double d, int n, ...) {
  va_list ap; va_start(ap, n);
  double s = a + b + c + d;
  for (int k = 0; k < n; ++k) { s += va_arg(ap, int); s += va_arg(ap, double); }
  va_end(ap); return s;
}
int main(void) {
  struct MI mi = f_mi((struct MI){3, 1.5});
  struct MM mm = f_mm((struct MM){2.5, 4.0});
  struct II ii = f_ii((struct II){7, 5});
  struct BIG bg = f_big((struct BIG){1, 2, 3, 4.5});
  printf("%ld %g\n", mi.i, mi.d);
  printf("%g %g\n", mm.a, mm.b);
  printf("%ld %ld\n", ii.a, ii.b);
  printf("%ld %ld %ld %g\n", bg.a, bg.b, bg.c, bg.d);
  printf("%g\n", spill(1,2,3,4,5,(struct MI){6,0.5},(struct MM){1.5,2.5},(struct II){8,9},10));
  printf("%g\n", stacked(1,2,3,4,5,6,7,(struct II){8,9},(struct MM){1.5,2.5},(struct MI){6,0.5}));
  printf("%g\n", va_after((struct MI){4,0.25}, 2, 1.5, 2.5));
  printf("%g\n", va_scalars(1,2.5,3,4.5,2, 5,6.5,7,8.5));
  return 0;
}
]=])
nyc_execute_ok(
  [=[SysV boundary object compilation failed\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -c "${BINARY_DIR}/sysv-boundary.c"
          -o "${BINARY_DIR}/sysv-boundary.o"
)
nyc_execute_ok(
  [=[SysV boundary link failed\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/sysv-boundary.o"
          -o "${BINARY_DIR}/sysv-boundary"
)
execute_process(
  COMMAND "${BINARY_DIR}/sysv-boundary"
  RESULT_VARIABLE sysv_status OUTPUT_VARIABLE sysv_stdout
  ERROR_VARIABLE sysv_stderr)
set(sysv_expected "4 3\n6.5 3\n12 15\n2 2 3 6.5\n52.5\n55.5\n8.25\n38\n")
if(NOT sysv_status EQUAL 0 OR NOT sysv_stdout STREQUAL sysv_expected)
  message(FATAL_ERROR
    "SysV aggregate/variadic boundary mismatch (${sysv_status})\n"
    "expected:\n${sysv_expected}\nactual:\n${sysv_stdout}${sysv_stderr}")
endif()

# Complex type layout: sizes and alignments must match the host ABI.
file(WRITE "${BINARY_DIR}/complex-layout.c" [=[
#include <stdio.h>
struct S { double _Complex z; };
struct F { float _Complex z; };
struct M { char c; double _Complex z; };
struct L { long double _Complex z; };
int main(void) {
  printf("%zu %zu %zu %zu %zu %zu %zu %zu %zu %zu\n",
         sizeof(double _Complex), sizeof(float _Complex),
         _Alignof(double _Complex), _Alignof(float _Complex),
         sizeof(struct S), sizeof(struct F), sizeof(struct M),
         sizeof(long double _Complex), _Alignof(long double _Complex),
         sizeof(struct L));
  double _Complex g;
  __real__ g = 1.5;
  __imag__ g = -2.5;
  printf("%g %g\n", __real__ g, __imag__ g);
  return 0;
}
]=])
nyc_execute_ok(
  [=[complex layout object compilation failed\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -c "${BINARY_DIR}/complex-layout.c"
          -o "${BINARY_DIR}/complex-layout.o"
)
nyc_execute_ok(
  [=[complex layout link failed\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/complex-layout.o"
          -o "${BINARY_DIR}/complex-layout"
)
execute_process(
  COMMAND "${BINARY_DIR}/complex-layout"
  RESULT_VARIABLE complex_status OUTPUT_VARIABLE complex_stdout
  ERROR_VARIABLE complex_stderr)
if(NOT complex_status EQUAL 0 OR
   NOT complex_stdout STREQUAL "16 8 8 4 16 8 24 32 16 32\n1.5 -2.5\n")
  message(FATAL_ERROR
    "complex sizeof/_Alignof mismatch (${complex_status})\n"
    "expected: 16 8 8 4 16 8 24 32 16 32\n1.5 -2.5\nactual:\n${complex_stdout}${complex_stderr}")
endif()

file(READ "${BINARY_DIR}/zero-bound-aggregate-global.s"
     zero_bound_aggregate_assembly)
if(NOT zero_bound_aggregate_assembly MATCHES
   "\\.globl[ \t]+zero_bound_ranges" OR
   NOT zero_bound_aggregate_assembly MATCHES
   "\\.size[ \t]+zero_bound_ranges,0")
  message(FATAL_ERROR
    "zero-bound aggregate definition was not emitted as a zero-size object")
endif()
file(READ "${BINARY_DIR}/aggregate-trailing-attribute.s"
     aggregate_trailing_attribute_assembly)
if(NOT aggregate_trailing_attribute_assembly MATCHES
   "\\.section[ \t]+\"?\\.data\\.\\.read_mostly")
  message(FATAL_ERROR
    "file-scope aggregate section attribute was not preserved in assembly")
endif()
nyc_execute_ok(
  [=[nyc could not preprocess the Clang stdatomic system header\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -E
            "${NY_C_CORPUS_system-stdatomic-header.c}"
            -o "${BINARY_DIR}/system-stdatomic-header.i"
)
execute_process(
  COMMAND "${NYC}" -fno-builtin -S
          "${NY_C_CORPUS_driver-no-builtin.c}"
          -o "${BINARY_DIR}/driver-no-builtin.s"
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(status EQUAL 0 OR NOT stderr MATCHES "must be declared")
  message(FATAL_ERROR
    "nyc -fno-builtin did not disable implicit builtin lookup\n${stdout}${stderr}")
endif()
foreach(inline_opt IN ITEMS -O0 -O3)
  nyc_compile_asm_run("gnu-extern-inline-asm${inline_opt}"
    "${NY_C_CORPUS_gnu-extern-inline.c}" NYC_FLAGS "${inline_opt}"
    HOST_SOURCES "${NY_C_CORPUS_gnu-extern-inline-definition.c}")
  nyc_compile_obj_run("gnu-extern-inline-obj${inline_opt}"
    "${NY_C_CORPUS_gnu-extern-inline.c}" NYC_FLAGS "${inline_opt}"
    HOST_SOURCES "${NY_C_CORPUS_gnu-extern-inline-definition.c}")
endforeach()
nyc_compile_obj_run(elf-local-function-order
  "${NY_C_CORPUS_elf-local-function-order.c}" NYC_FLAGS -O2 -fPIC)
nyc_compile_obj_run(asm-rip-relative-object
  "${NY_C_CORPUS_asm-rip-relative.c}" NYC_FLAGS -O2 -fPIC
  HOST_SOURCES "${NY_C_CORPUS_asm-rip-relative-host.c}")
nyc_compile_asm_run(asm-rip-relative-text
  "${NY_C_CORPUS_asm-rip-relative.c}" NYC_FLAGS -O2 -fPIC
  HOST_SOURCES "${NY_C_CORPUS_asm-rip-relative-host.c}")
execute_process(
  COMMAND "${NYC}" -O2 -fPIC -c "${NY_C_CORPUS_asm-rip-relative-invalid.c}"
          -o "${BINARY_DIR}/asm-rip-relative-invalid.o"
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(status EQUAL 0 OR NOT stderr MATCHES "requires a link-time symbol address")
  message(FATAL_ERROR
    "RIP-relative asm must reject a run-time immediate\n${stdout}${stderr}")
endif()
if(READELF)
  nyc_execute_ok(
    [=[could not assemble the RIP-relative fixture\n${stdout}${stderr}]=]
    COMMAND "${CC}" -c "${BINARY_DIR}/asm-rip-relative-text.s"
            -o "${BINARY_DIR}/asm-rip-relative-text.o")
  foreach(route IN ITEMS object text)
    execute_process(
      COMMAND "${READELF}" -rW "${BINARY_DIR}/asm-rip-relative-${route}.o"
      RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
    if(NOT status EQUAL 0 OR
       NOT stdout MATCHES "R_X86_64_PC32[^\n]*image_anchor" OR
       stdout MATCHES "GOTPCREL[^\n]*image_anchor")
      message(FATAL_ERROR
        "${route} RIP-relative asm must not use the GOT\n${stdout}${stderr}")
    endif()
  endforeach()
endif()
nyc_compile_asm_run(cleanup-jumps-asm
  "${NY_C_CORPUS_cleanup-jumps.c}" NYC_FLAGS -O2)
nyc_compile_obj_run(cleanup-jumps-object
  "${NY_C_CORPUS_cleanup-jumps.c}" NYC_FLAGS -O2 -mcmodel=kernel)
nyc_compile_obj_run(elf-mergeable-file-asm
  "${NY_C_CORPUS_elf-mergeable-file-asm.c}" NYC_FLAGS -mcmodel=kernel)
nyc_compile_obj_run(size-disabled-paths
  "${NY_C_CORPUS_size-disabled-paths.c}" NYC_FLAGS -Os)
if(READELF)
  nyc_compile_obj_run(elf-callback-table-section
    "${NY_C_CORPUS_elf-callback-table-section.c}" NYC_FLAGS -O0)
  execute_process(
    COMMAND "${READELF}" -rW "${BINARY_DIR}/elf-callback-table-section.o"
    RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
  if(NOT status EQUAL 0 OR NOT stdout MATCHES "\\.rela\\.init\\.data")
    message(FATAL_ERROR
      "function-pointer table lost its section or initializer\n${stdout}${stderr}")
  endif()
  nyc_execute_ok(
    [=[could not produce a kernel export record\n${stdout}${stderr}]=]
    COMMAND "${NYC}" -c "${NY_C_CORPUS_elf-export-record.c}"
            -o "${BINARY_DIR}/elf-export-record.o"
  )
  execute_process(
    COMMAND "${READELF}" -sWr "${BINARY_DIR}/elf-export-record.o"
    RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
  if(NOT status EQUAL 0 OR NOT stdout MATCHES "\\.rela\\.export_symbol" OR
      NOT stdout MATCHES "R_X86_64_64[^\n]+ example_export \\+" OR
      NOT stdout MATCHES "[ \t]__export_symbol_example_export" OR
      stdout MATCHES "__nyc_data___export_symbol_example_export")
    message(FATAL_ERROR
      "kernel export relocation did not identify one assembler record\n${stdout}${stderr}")
  endif()
endif()
set(static_assert_source "${NY_C_CORPUS_static-assert.c}")
set(static_assert_false_source "${NY_C_CORPUS_static-assert-false.c}")

set(ffs_source "${NY_C_CORPUS_builtin-ffs.c}")

nyc_compile_obj_run(builtin-ffs "${ffs_source}")
set(aggregate_double_pointer_source "${NY_C_CORPUS_aggregate-double-pointer-cast.c}")
nyc_compile_asm_run(aggregate-double-pointer-cast
  "${aggregate_double_pointer_source}")
set(aggregate_triple_pointer_source "${NY_C_CORPUS_aggregate-triple-pointer.c}")
nyc_compile_asm_run(aggregate-triple-pointer
  "${aggregate_triple_pointer_source}")
set(runtime_const_pointer_source "${NY_C_CORPUS_runtime-const-pointer-inline-asm.c}")
nyc_compile_asm_run(runtime-const-pointer-inline-asm
  "${runtime_const_pointer_source}")
set(user_load_goto_source "${NY_C_CORPUS_linux-user-load-goto.c}")
nyc_compile_asm_run(linux-user-load-goto "${user_load_goto_source}")
set(xstore_inline_asm_source
    "${NY_C_CORPUS_linux-xstore-inline-asm.c}")
nyc_execute_ok(
  [=[nyc xstore inline-asm compilation failed\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -S "${xstore_inline_asm_source}"
            -o "${BINARY_DIR}/linux-xstore-inline-asm.s"
)
nyc_execute_ok(
  [=[xstore inline-asm assembly failed\n${stdout}${stderr}]=]
  COMMAND "${CC}" -c "${BINARY_DIR}/linux-xstore-inline-asm.s"
            -o "${BINARY_DIR}/linux-xstore-inline-asm.o"
)
set(function_pointer_xchg_source
    "${NY_C_CORPUS_linux-function-pointer-xchg.c}")
nyc_compile_asm_run(linux-function-pointer-xchg "${function_pointer_xchg_source}")
nyc_compile_asm_run(linux-pm-trace-metadata
  "${NY_C_CORPUS_linux-pm-trace-metadata.c}")
nyc_compile_asm_run(linux-int128-shift-compound
  "${NY_C_CORPUS_linux-int128-shift-compound.c}")
set(movntdqa_asm_source
    "${NY_C_CORPUS_linux-movntdqa-asm.c}")
nyc_execute_ok(
  [=[nyc movntdqa inline-asm compilation failed\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -S "${movntdqa_asm_source}"
            -o "${BINARY_DIR}/linux-movntdqa-asm.s"
)
nyc_execute_ok(
  [=[movntdqa inline-asm assembly failed\n${stdout}${stderr}]=]
  COMMAND "${CC}" -c "${BINARY_DIR}/linux-movntdqa-asm.s"
            -o "${BINARY_DIR}/linux-movntdqa-asm.o"
)
set(rep_movsl_tail_source
    "${NY_C_CORPUS_linux-rep-movsl-tail.c}")
nyc_compile_asm_run(linux-rep-movsl-tail "${rep_movsl_tail_source}")
set(user_load_register_extable_source
    "${NY_C_CORPUS_linux-user-load-register-extable.c}")
nyc_compile_asm_run(linux-user-load-register-extable
  "${user_load_register_extable_source}")
set(atomic_memory_orq_source "${NY_C_CORPUS_atomic-memory-orq-er.c}")
nyc_compile_asm_run(atomic-memory-orq-er "${atomic_memory_orq_source}"
  NYC_FLAGS -O3)
set(variadic_source "${NY_C_CORPUS_variadic-va-args.c}")

# The integer conversion rules are shared by the x86-64 output routes and
# i386. Compare both x86-64 routes with the host oracle at O2 and O3 too.
foreach(opt IN ITEMS 2 3 s z)
  foreach(extension IN ITEMS s o)
    if(extension STREQUAL "s")
      set(compile_flag -S)
    else()
      set(compile_flag -c)
    endif()
    set(arithmetic_output "${BINARY_DIR}/spill-arithmetic-${opt}.${extension}")
    nyc_execute_ok(
      [=[integer arithmetic compilation failed\n${stdout}${stderr}]=]
      COMMAND "${NYC}" "-O${opt}" "${compile_flag}"
                    "${NY_C_CORPUS_i386-spill-arithmetic.c}" -o "${arithmetic_output}"
    )
    set(arithmetic_binary "${BINARY_DIR}/spill-arithmetic-${opt}-${extension}")
    nyc_execute_ok(
      [=[integer arithmetic oracle link failed\n${stdout}${stderr}]=]
      COMMAND "${CC}" -O2 -DNYC_PROBE_HOSTED
                    "${NY_C_CORPUS_i386-spill-arithmetic-host.c}"
                    "${arithmetic_output}" -o "${arithmetic_binary}"
    )
    nyc_execute_ok(
      [=[integer arithmetic O${opt}/${extension} mismatch: ${status}]=]
      COMMAND "${arithmetic_binary}"
    )
  endforeach()
endforeach()

nyc_compile_asm_run(variadic-va-args "${variadic_source}")
set(aggregate_variadic_source
    "${NY_C_CORPUS_aggregate-variadic-call.c}")
nyc_compile_asm_run(aggregate-variadic-call "${aggregate_variadic_source}"
  HOST_SOURCES "${NY_C_CORPUS_aggregate-variadic-host.c}")
set(aggregate_va_arg_source
    "${NY_C_CORPUS_aggregate-variadic-va-arg.c}")
set(aggregate_va_arg_host
    "${NY_C_CORPUS_aggregate-variadic-va-arg-host.c}")
nyc_compile_asm_run(aggregate-variadic-va-arg-asm "${aggregate_va_arg_source}"
  HOST_SOURCES "${aggregate_va_arg_host}")
nyc_compile_obj_run(aggregate-variadic-va-arg-obj "${aggregate_va_arg_source}"
  HOST_SOURCES "${aggregate_va_arg_host}")
set(va_start_aggregate_source
    "${NY_C_CORPUS_va-start-fixed-aggregate.c}")
nyc_compile_asm_run(va-start-fixed-aggregate "${va_start_aggregate_source}"
  HOST_SOURCES "${NY_C_CORPUS_va-start-fixed-aggregate-host.c}")
set(static_wide_array_source
    "${NY_C_CORPUS_static-wide-array-extent.c}")
nyc_compile_asm_run(static-wide-array-extent "${static_wide_array_source}"
  HOST_SOURCES "${NY_C_CORPUS_static-wide-array-extent-host.c}")
set(indirect_jump_source
    "${NY_C_CORPUS_indirect-asm-jump.c}")
nyc_compile_asm_run(indirect-asm-jump "${indirect_jump_source}"
  HOST_SOURCES "${NY_C_CORPUS_indirect-asm-jump-host.c}")
set(percpu_source "${NY_C_CORPUS_percpu-raw-pointer.c}")
nyc_compile_asm_run(percpu-raw-pointer "${percpu_source}" EXPECTED_EXIT 1)
set(function_typedef_source "${NY_C_CORPUS_function-typedef-definition.c}")
nyc_compile_asm_run(function-typedef-definition "${function_typedef_source}")
nyc_execute_ok(
  [=[nyc valid _Static_assert failed\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -c -std=gnu11 "${static_assert_source}"
            -o "${BINARY_DIR}/static-assert.o"
)
set(floatn_source "${NY_C_CORPUS_floatn.c}")
set(global_char_array_source "${NY_C_CORPUS_global-char-array-store.c}")

nyc_compile_obj_run(global-char-array-store "${global_char_array_source}")
set(global_multidimensional_scalar_array_source
    "${NY_C_CORPUS_global-multidimensional-scalar-array.c}")
nyc_compile_obj_run(global-multidimensional-scalar-array
  "${global_multidimensional_scalar_array_source}")
nyc_compile_obj_run(floatn "${floatn_source}"
  HOST_SOURCES "${NY_C_CORPUS_floatn-host.c}")
nyc_compile_asm_run(builtin-ctzll "${NY_C_CORPUS_builtin-ctzll.c}")
set(pointer_array_designator_source
    "${NY_C_CORPUS_pointer-array-designators.c}")

nyc_execute_ok(
  [=[nyc failed to compile designated string-pointer array\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -S "${pointer_array_designator_source}"
            -o "${BINARY_DIR}/pointer-array-designators.s"
)
nyc_execute_ok(
  [=[host compiler could not link designated string-pointer array test\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/pointer-array-designators.s" -o
            "${BINARY_DIR}/pointer-array-designators"
)
nyc_execute_ok(
  [=[designated string-pointer array values mismatch\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/pointer-array-designators"
)
set(aggregate_array_designator_source
    "${NY_C_CORPUS_aggregate-array-designators.c}")

nyc_execute_ok(
  [=[nyc failed to infer designated aggregate-array bound\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -S "${aggregate_array_designator_source}"
            -o "${BINARY_DIR}/aggregate-array-designators.s"
)
nyc_execute_ok(
  [=[host compiler could not link aggregate-array designator test\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/aggregate-array-designators.s" -o
            "${BINARY_DIR}/aggregate-array-designators"
)
nyc_execute_ok(
  [=[aggregate-array designator values mismatch\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/aggregate-array-designators"
)
set(empty_inferred_aggregate_array_source
    "${NY_C_CORPUS_empty-inferred-aggregate-array.c}")
nyc_execute_ok(
  [=[nyc failed to compile an empty inferred aggregate array\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -S "${empty_inferred_aggregate_array_source}"
            -o "${BINARY_DIR}/empty-inferred-aggregate-array.s"
)
nyc_execute_ok(
  [=[host compiler could not link empty inferred aggregate array test\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/empty-inferred-aggregate-array.s" -o
            "${BINARY_DIR}/empty-inferred-aggregate-array"
)
nyc_execute_ok(
  [=[empty inferred aggregate array has nonzero size\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/empty-inferred-aggregate-array"
)
set(trailing_overlap_source "${NY_C_CORPUS_trailing-overlap-assert.c}")

nyc_execute_ok(
  [=[nyc rejected the Linux trailing-overlap _Static_assert\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -c -std=gnu11 "${trailing_overlap_source}"
            -o "${BINARY_DIR}/trailing-overlap-assert.o"
)

execute_process(
  COMMAND "${NYC}" -c -std=gnu11 "${static_assert_false_source}"
          -o "${BINARY_DIR}/static-assert-false.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(status EQUAL 0 OR NOT stderr MATCHES "_Static_assert condition is false")
  message(FATAL_ERROR
    "nyc accepted a false _Static_assert\n${stdout}${stderr}")
endif()
set(offsetof_source "${NY_C_CORPUS_builtin-stddef.c}")

nyc_execute_ok(
  [=[nyc failed to evaluate a constant offsetof expression\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -c -std=gnu11 "${offsetof_source}"
            -o "${BINARY_DIR}/builtin-stddef.o"
)

set(function_attribute_source "${NY_C_CORPUS_function-attribute.c}")

set(local_attribute_source "${NY_C_CORPUS_local-attribute.c}")

nyc_execute_ok(
  [=[nyc failed to parse a supported block-scope unused attribute\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -c "${local_attribute_source}"
            -o "${BINARY_DIR}/local-attribute.o"
)
nyc_execute_ok(
  [=[host compiler could not link local-attribute regression\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/local-attribute.o" -o
            "${BINARY_DIR}/local-attribute"
)
nyc_execute_ok(
  [=[local attribute regression failed at runtime\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/local-attribute"
)

nyc_execute_ok(
  [=[nyc failed to parse a function return attribute\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -c -std=gnu11 "${function_attribute_source}"
            -o "${BINARY_DIR}/function-attribute.o"
)
set(plan9_member_source "${NY_C_CORPUS_plan9-anonymous-member.c}")

nyc_execute_ok(
  [=[nyc failed Plan 9 anonymous aggregate layout assertions\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -fplan9-extensions -c -std=gnu11 "${plan9_member_source}"
            -o "${BINARY_DIR}/plan9-anonymous-member.o"
)
set(generic_probe_source "${NY_C_CORPUS_generic-ternary-probe.c}")

nyc_execute_ok(
  [=[nyc failed to compile a ternary-controlled _Generic selection\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -c -std=gnu11 "${generic_probe_source}"
            -o "${BINARY_DIR}/generic-ternary-probe.o"
)
nyc_execute_ok(
  [=[reference linker rejected the ternary-controlled _Generic probe\n${stdout}${stderr}]=]
  COMMAND "${CC}" -no-pie "${BINARY_DIR}/generic-ternary-probe.o"
            -o "${BINARY_DIR}/generic-ternary-probe"
)
nyc_execute_ok(
  [=[ternary-controlled _Generic selection returned the wrong pointer]=]
  COMMAND "${BINARY_DIR}/generic-ternary-probe"
)
set(global_after_static_source "${NY_C_CORPUS_global-after-static.c}")

set(global_after_static_consumer "${NY_C_CORPUS_global-after-static-consumer.c}")

foreach(source IN ITEMS "${global_after_static_source}"
                        "${global_after_static_consumer}")
  get_filename_component(stem "${source}" NAME_WE)
  nyc_execute_ok(
    [=[nyc failed to compile a cross-translation-unit global linkage probe\n${stdout}${stderr}]=]
    COMMAND "${NYC}" -c "${source}"
                -o "${BINARY_DIR}/${stem}.o"
  )
endforeach()
nyc_execute_ok(
  [=[file-scope global after a function-local static did not link externally\n${stdout}${stderr}]=]
  COMMAND "${CC}" -no-pie "${BINARY_DIR}/global-after-static.o"
            "${BINARY_DIR}/global-after-static-consumer.o"
            -o "${BINARY_DIR}/global-after-static"
)
nyc_execute_ok(
  [=[external file-scope global following a local static returned the wrong value]=]
  COMMAND "${BINARY_DIR}/global-after-static"
)
set(function_alias_call_source "${NY_C_CORPUS_function-asm-alias-call.c}")

nyc_execute_ok(
  [=[nyc failed to compile a call through a function asm alias\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -c "${function_alias_call_source}"
            -o "${BINARY_DIR}/function-asm-alias-call.o"
)
if(READELF)
  execute_process(
    COMMAND "${READELF}" -rW "${BINARY_DIR}/function-asm-alias-call.o"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0 OR
     NOT stdout MATCHES "nyc_actual_symbol" OR
     stdout MATCHES "nyc_alias_target")
    message(FATAL_ERROR
      "function asm alias relocation did not target its declared symbol\n${stdout}${stderr}")
  endif()
endif()
set(large_aggregate_source "${NY_C_CORPUS_large-aggregate-fields.c}")

nyc_execute_ok(
  [=[nyc rejected an aggregate with more than 512 fields\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -c "${large_aggregate_source}"
            -o "${BINARY_DIR}/large-aggregate-fields.o"
)
set(function_array_source "${NY_C_CORPUS_function-pointer-array.c}")

nyc_execute_ok(
  [=[nyc failed to parse external function-pointer arrays\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -c -std=gnu11 -falign-functions=16
            "${function_array_source}"
            -o "${BINARY_DIR}/function-pointer-array.o"
)
set(omit_fp_source "${NY_C_CORPUS_omit-frame-pointer.c}")

nyc_execute_ok(
  [=[nyc failed to compile -fomit-frame-pointer regression\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -c -fomit-frame-pointer "${omit_fp_source}"
            -o "${BINARY_DIR}/omit-frame-pointer.o"
)
set(omit_fp_harness "${NY_C_CORPUS_omit-frame-pointer-host.c}")

nyc_execute_ok(
  [=[could not link -fomit-frame-pointer regression\n${stdout}${stderr}]=]
  COMMAND "${CC}" -no-pie "${omit_fp_harness}"
            "${BINARY_DIR}/omit-frame-pointer.o" -o
            "${BINARY_DIR}/omit-frame-pointer-host"
)
nyc_execute_ok(
  [=[failed -fomit-frame-pointer stack/call regression\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/omit-frame-pointer-host"
)
execute_process(
  COMMAND "${NYC}" -fno-omit-frame-pointer -fomit-frame-pointer
          "${function_array_source}"
          "${BINARY_DIR}/single-function-omit-fp.o" c_nyc_initcall_ranges_equal
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(status EQUAL 0)
  message(FATAL_ERROR
    "nyc did not reject unsupported single-function -fomit-frame-pointer mode\n${stdout}${stderr}")
endif()

# The last frame-pointer option controls both code generation and route
# eligibility: cancelling omission must restore single-function compilation.
nyc_execute_ok(
  [=[frame-pointer option cancellation failed\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -fomit-frame-pointer -fno-omit-frame-pointer
            "${omit_fp_source}" "${BINARY_DIR}/frame-pointer-order.o" omit_fp_sum8
)
nyc_execute_ok(
  [=[frame-pointer option order link failed\n${stdout}${stderr}]=]
  COMMAND "${CC}" -std=c11 -Wall -Wextra -Werror -no-pie
            "${NY_C_CORPUS_frame-pointer-order-host.c}"
            "${BINARY_DIR}/frame-pointer-order.o"
            -o "${BINARY_DIR}/frame-pointer-order"
)
nyc_execute_ok(
  [=[frame-pointer option order ABI failed\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/frame-pointer-order"
)
set(retpoline_source "${NY_C_CORPUS_retpoline-indirect-call.c}")

nyc_execute_ok(
  [=[nyc failed to compile retpoline indirect-call regression\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -c -fomit-frame-pointer
            -mindirect-branch=thunk-extern -mindirect-branch-register
            "${retpoline_source}" -o "${BINARY_DIR}/retpoline-indirect-call.o"
)
nyc_execute_ok(
  [=[nyc failed the empty Kbuild retpoline option probe\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -Werror -mindirect-branch=thunk-extern
            -mindirect-branch-register -c -x c /dev/null
            -o "${BINARY_DIR}/empty-kbuild-probe.o"
)
if(READELF)
  execute_process(
    COMMAND "${READELF}" -rW "${BINARY_DIR}/retpoline-indirect-call.o"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0 OR
     NOT stdout MATCHES "__x86_indirect_thunk_r11")
    message(FATAL_ERROR
      "retpoline call did not reference the external r11 thunk\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${READELF}" -h "${BINARY_DIR}/empty-kbuild-probe.o"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0 OR NOT stdout MATCHES "REL \\(Relocatable file\\)")
    message(FATAL_ERROR
      "empty Kbuild probe did not produce a relocatable ELF object\n${stdout}${stderr}")
  endif()
endif()
set(retpoline_thunk "${NY_C_CORPUS_retpoline-indirect-thunk.S}")
if(LD AND READELF)
  nyc_execute_ok(
    [=[host could not assemble the untyped retpoline label\n${stdout}${stderr}]=]
    COMMAND "${CC}" -c "${retpoline_thunk}" -o "${BINARY_DIR}/retpoline-thunk.o"
  )
  nyc_execute_ok(
    [=[relocatable link of retpoline target failed\n${stdout}${stderr}]=]
    COMMAND "${LD}" -r "${BINARY_DIR}/retpoline-indirect-call.o"
            "${BINARY_DIR}/retpoline-thunk.o"
            -o "${BINARY_DIR}/retpoline-relocatable.o"
  )
  execute_process(
    COMMAND "${READELF}" -sW "${BINARY_DIR}/retpoline-relocatable.o"
    RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
  if(NOT status EQUAL 0 OR NOT stdout MATCHES
      "NOTYPE +GLOBAL +DEFAULT +[0-9]+ +__x86_indirect_thunk_r11")
    message(FATAL_ERROR
      "NYC call relocations changed the linked thunk label type\n${stdout}${stderr}")
  endif()
endif()

set(retpoline_harness "${NY_C_CORPUS_retpoline-indirect-call-host.c}")

nyc_execute_ok(
  [=[could not link retpoline indirect-call regression\n${stdout}${stderr}]=]
  COMMAND "${CC}" -no-pie "${retpoline_harness}" "${retpoline_thunk}"
            "${BINARY_DIR}/retpoline-indirect-call.o" -o
            "${BINARY_DIR}/retpoline-indirect-call-host"
)
nyc_execute_ok(
  [=[retpoline indirect-call regression failed\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/retpoline-indirect-call-host"
)
nyc_execute_ok(
  [=[nyc failed to compile inline-retpoline regression\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -c -fomit-frame-pointer
            -mindirect-branch=thunk-inline -mindirect-branch-register
            "${retpoline_source}" -o "${BINARY_DIR}/retpoline-inline-call.o"
)
nyc_execute_ok(
  [=[could not link inline-retpoline regression\n${stdout}${stderr}]=]
  COMMAND "${CC}" -no-pie "${retpoline_harness}"
            "${BINARY_DIR}/retpoline-inline-call.o" -o
            "${BINARY_DIR}/retpoline-inline-call-host"
)
nyc_execute_ok(
  [=[inline-retpoline indirect-call regression failed\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/retpoline-inline-call-host"
)
foreach(thunk IN ITEMS extern inline)
  set(thunk_source)
  if(thunk STREQUAL "extern")
    set(thunk_source "${retpoline_thunk}")
  endif()
  nyc_execute_output("machine retpoline compilation failed (${thunk})"
    "${BINARY_DIR}/retpoline-machine-${thunk}.o"
    COMMAND "${CMAKE_COMMAND}" -E env NYC_NO_MACH_OBJECT=0
            NYC_TRACE_MACHINE_FALLBACK=1 "${NYC}"
            --mixed-disasm=retpoline_variadic_call -O2 -c
            "-mindirect-branch=thunk-${thunk}" "${retpoline_source}"
            -o "${BINARY_DIR}/retpoline-machine-${thunk}.o"
  )
  if(stderr MATCHES "NYC object fallback" OR
     NOT stderr MATCHES "== retpoline_variadic_call  src=")
    message(FATAL_ERROR
      "retpoline did not use the expected machine object route (${thunk})\n${stderr}")
  endif()
  nyc_execute_output("machine retpoline did not link (${thunk})"
    "${BINARY_DIR}/retpoline-machine-${thunk}"
    COMMAND "${CC}" -no-pie "${retpoline_harness}" ${thunk_source}
            "${BINARY_DIR}/retpoline-machine-${thunk}.o"
            -o "${BINARY_DIR}/retpoline-machine-${thunk}"
  )
  nyc_execute_ok(
    [=[machine retpoline changed call behavior (${thunk})]=]
    COMMAND "${BINARY_DIR}/retpoline-machine-${thunk}" TIMEOUT 5
  )
endforeach()
nyc_execute_ok(
  [=[nyc failed to emit assembly with external retpoline calls\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -S -mindirect-branch=thunk-extern
            -mindirect-branch-register "${retpoline_source}" -o
            "${BINARY_DIR}/retpoline-indirect-call.s"
)
nyc_execute_ok(
  [=[could not link assembly-mode external-retpoline regression\n${stdout}${stderr}]=]
  COMMAND "${CC}" -no-pie "${retpoline_harness}" "${retpoline_thunk}"
            "${BINARY_DIR}/retpoline-indirect-call.s" -o
            "${BINARY_DIR}/retpoline-indirect-call-assembly-host"
)
nyc_execute_ok(
  [=[assembly-mode external-retpoline call failed\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/retpoline-indirect-call-assembly-host"
)
nyc_execute_ok(
  [=[nyc failed to emit assembly with inline retpolines\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -S -mindirect-branch=thunk-inline
            -mindirect-branch-register "${retpoline_source}" -o
            "${BINARY_DIR}/retpoline-inline-call.s"
)
nyc_execute_ok(
  [=[could not link assembly-mode inline-retpoline regression\n${stdout}${stderr}]=]
  COMMAND "${CC}" -no-pie "${retpoline_harness}"
            "${BINARY_DIR}/retpoline-inline-call.s" -o
            "${BINARY_DIR}/retpoline-inline-call-assembly-host"
)
nyc_execute_ok(
  [=[assembly-mode inline-retpoline call failed\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/retpoline-inline-call-assembly-host"
)
execute_process(
  COMMAND "${NYC}" -mindirect-branch=thunk-extern
          "${retpoline_source}" "${BINARY_DIR}/single-retpoline.o"
          retpoline_call
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(status EQUAL 0 OR
   NOT stderr MATCHES "retpoline options require whole-unit object mode")
  message(FATAL_ERROR
    "nyc did not reject single-function retpoline mode\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${NYC}" -fPIC "${retpoline_source}"
          "${BINARY_DIR}/single-pic.o" retpoline_call
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(status EQUAL 0 OR
   NOT stderr MATCHES "-fPIC/-fPIE require whole-unit object mode")
  message(FATAL_ERROR
    "nyc did not reject single-function PIC mode\n${stdout}${stderr}")
endif()
foreach(x86_flag IN ITEMS -m64 -march=x86-64 -mno-sse)
  execute_process(
    COMMAND "${NYC}" -E --target=aarch64-unknown-linux-gnu "${x86_flag}"
            "${function_array_source}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(status EQUAL 0)
    message(FATAL_ERROR "nyc accepted ${x86_flag} for a non-x86-64 target")
  endif()
endforeach()
set(pic_source "${NY_C_CORPUS_pic-global-address.c}")

set(pic_harness "${NY_C_CORPUS_pic-harness.c}")

foreach(pic_flag IN ITEMS -fPIC -fPIE)
  set(pic_object "${BINARY_DIR}/pic-${pic_flag}.o")
  nyc_execute_ok(
    [=[nyc rejected ${pic_flag}\n${stdout}${stderr}]=]
    COMMAND "${NYC}" -c "${pic_flag}" "${pic_source}" -o "${pic_object}"
  )
  if(READELF)
    execute_process(
      COMMAND "${READELF}" -r "${pic_object}"
      RESULT_VARIABLE status
      OUTPUT_VARIABLE stdout
      ERROR_VARIABLE stderr
    )
    if(NOT status EQUAL 0 OR
       NOT stdout MATCHES "R_X86_64_GOTPCREL" OR
       NOT stdout MATCHES "R_X86_64_PLT32" OR
       NOT stdout MATCHES "R_X86_64_64")
      message(FATAL_ERROR
        "${pic_flag} object lacks GOT/PLT or file-scope data relocations\n${stdout}${stderr}")
    endif()
  endif()
  nyc_execute_ok(
    [=[host linker rejected NYC ${pic_flag} object as shared object\n${stdout}${stderr}]=]
    COMMAND "${CC}" -shared -o "${BINARY_DIR}/libpic-${pic_flag}.so"
                "${pic_object}"
  )
  nyc_execute_ok(
    [=[host linker rejected NYC ${pic_flag} executable\n${stdout}${stderr}]=]
    COMMAND "${CC}" "${pic_harness}" "${pic_object}" -o
                "${BINARY_DIR}/pic-harness-${pic_flag}"
  )
  nyc_execute_ok(
    [=[${pic_flag} PIC global/call runtime regression failed]=]
    COMMAND "${BINARY_DIR}/pic-harness-${pic_flag}"
  )
endforeach()
set(overflow_source "${NY_C_CORPUS_unsigned-overflow.c}")

set(overflow_object "${BINARY_DIR}/unsigned-overflow.o")
nyc_execute_ok(
  [=[nyc rejected unsigned overflow builtins\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -c "${overflow_source}" -o "${overflow_object}"
)
nyc_execute_ok(
  [=[host linker rejected NYC overflow object\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${overflow_object}" -o "${BINARY_DIR}/unsigned-overflow"
)
nyc_execute_ok(
  [=[unsigned overflow runtime regression failed: ${status}]=]
  COMMAND "${BINARY_DIR}/unsigned-overflow"
)
nyc_execute_ok(
  [=[nyc rejected -Os\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -c -Os "${pic_source}" -o "${BINARY_DIR}/pic-os.o"
)
nyc_execute_ok(
  [=[host linker rejected NYC -Os object\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${pic_harness}" "${BINARY_DIR}/pic-os.o" -o
            "${BINARY_DIR}/pic-os-harness"
)
nyc_execute_ok(
  [=[NYC -Os runtime regression failed]=]
  COMMAND "${BINARY_DIR}/pic-os-harness"
)
if(READELF)
  execute_process(
    COMMAND "${READELF}" -SW "${BINARY_DIR}/function-pointer-array.o"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  # The x86-64 object writer aligns .text to 32 bytes so that loop headers
  # padded to 32 keep their position; 16-byte function alignment still holds.
  if(NOT status EQUAL 0 OR NOT stdout MATCHES "\\.text.*[ \t](16|32)" OR
     NOT stdout MATCHES "\\.nyc_fnptr")
    message(FATAL_ERROR
      "NYC did not honor function alignment or concatenated section names\n${stdout}${stderr}")
  endif()
endif()
set(function_array_harness "${NY_C_CORPUS_function-pointer-array-host.c}")

nyc_execute_ok(
  [=[reference compiler could not link external function-pointer arrays\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${function_array_harness}"
            "${BINARY_DIR}/function-pointer-array.o" -o
            "${BINARY_DIR}/function-pointer-array-host"
)
nyc_execute_ok(
  [=[NYC external function-pointer array references resolved incorrectly]=]
  COMMAND "${BINARY_DIR}/function-pointer-array-host"
)
set(read-once-pointer-array-source
    "${NY_C_CORPUS_read-once-pointer-array.c}")

nyc_execute_ok(
  [=[nyc failed to compile READ_ONCE on an external aggregate-pointer array\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -c -std=gnu11 "${read-once-pointer-array-source}"
            -o "${BINARY_DIR}/read-once-pointer-array.o"
)
set(read-once-pointer-array-host
    "${NY_C_CORPUS_read-once-pointer-array-host.c}")

nyc_execute_ok(
  [=[host linker rejected READ_ONCE pointer-array regression\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${read-once-pointer-array-host}"
            "${BINARY_DIR}/read-once-pointer-array.o" -o
            "${BINARY_DIR}/read-once-pointer-array"
)
nyc_execute_ok(
  [=[READ_ONCE pointer-array regression returned an incorrect value]=]
  COMMAND "${BINARY_DIR}/read-once-pointer-array"
)
set(typeof-volatile-member-source
    "${NY_C_CORPUS_typeof-volatile-member.c}")

nyc_execute_ok(
  [=[nyc failed to preserve a scalar member type through typeof\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -c -std=gnu11 "${typeof-volatile-member-source}"
            -o "${BINARY_DIR}/typeof-volatile-member.o"
)
set(typeof-volatile-member-host
    "${NY_C_CORPUS_typeof-volatile-member-host.c}")

nyc_execute_ok(
  [=[host linker rejected typeof scalar-member regression\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${typeof-volatile-member-host}"
            "${BINARY_DIR}/typeof-volatile-member.o" -o
            "${BINARY_DIR}/typeof-volatile-member"
)
nyc_execute_ok(
  [=[typeof scalar-member store produced an incorrect runtime value]=]
  COMMAND "${BINARY_DIR}/typeof-volatile-member"
)
set(typeof-function-designator-source
    "${NY_C_CORPUS_typeof-function-designator.c}")

nyc_execute_ok(
  [=[nyc failed to call a typeof-derived function designator\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -c -std=gnu11 "${typeof-function-designator-source}"
            -o "${BINARY_DIR}/typeof-function-designator.o"
)
set(typeof-function-designator-host
    "${NY_C_CORPUS_typeof-function-designator-host.c}")

nyc_execute_ok(
  [=[host linker rejected typeof function-designator regression\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${typeof-function-designator-host}"
            "${BINARY_DIR}/typeof-function-designator.o" -o
            "${BINARY_DIR}/typeof-function-designator"
)
nyc_execute_ok(
  [=[typeof function-designator call returned an incorrect result]=]
  COMMAND "${BINARY_DIR}/typeof-function-designator"
)
set(fixed-register-syscall-source
    "${NY_C_CORPUS_fixed-register-syscall.c}")

nyc_execute_ok(
  [=[nyc failed to lower ABI-matched fixed-register syscall arguments\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -S "${fixed-register-syscall-source}"
            -o "${BINARY_DIR}/fixed-register-syscall.s"
)
nyc_execute_ok(
  [=[host linker rejected fixed-register syscall regression\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/fixed-register-syscall.s" -o
            "${BINARY_DIR}/fixed-register-syscall"
)
nyc_execute_ok(
  [=[fixed-register syscall did not return a positive process ID]=]
  COMMAND "${BINARY_DIR}/fixed-register-syscall"
)
set(fixed-register-syscall-mismatch-source
    "${NY_C_CORPUS_fixed-register-syscall-mismatch.c}")

execute_process(
  COMMAND "${NYC}" -S "${fixed-register-syscall-mismatch-source}"
          -o "${BINARY_DIR}/fixed-register-syscall-mismatch.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(status EQUAL 0 OR
   NOT "${stdout}${stderr}" MATCHES
       "fixed-register syscall argument does not match its ABI register")
  message(FATAL_ERROR
    "nyc did not reject a syscall argument bound to the wrong ABI register\n${stdout}${stderr}")
endif()
set(external_integer_array_source "${NY_C_CORPUS_external-integer-arrays.c}")

nyc_execute_ok(
  [=[nyc failed to parse multiple external integer arrays\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -c -std=gnu11 "${external_integer_array_source}"
            -o "${BINARY_DIR}/external-integer-arrays.o"
)
set(external_integer_array_harness
    "${NY_C_CORPUS_external-integer-arrays-host.c}")

nyc_execute_ok(
  [=[reference compiler could not link external integer arrays\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${external_integer_array_harness}"
            "${BINARY_DIR}/external-integer-arrays.o" -o
            "${BINARY_DIR}/external-integer-arrays-host"
)
nyc_execute_ok(
  [=[NYC external integer array references resolved incorrectly]=]
  COMMAND "${BINARY_DIR}/external-integer-arrays-host"
)
set(static_pointer_arrays_source
    "${NY_C_CORPUS_static-pointer-arrays.c}")

nyc_execute_ok(
  [=[nyc failed to compile zero-initialized static pointer arrays\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -c -std=gnu11 "${static_pointer_arrays_source}"
            -o "${BINARY_DIR}/static-pointer-arrays.o"
)

nyc_execute_ok(
  [=[host linker rejected zero-initialized static pointer arrays\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${NY_C_CORPUS_static-pointer-arrays-main.c}"
            "${BINARY_DIR}/static-pointer-arrays.o"
            -o "${BINARY_DIR}/static-pointer-arrays"
)
nyc_execute_ok(
  [=[static pointer arrays were not zero initialized]=]
  COMMAND "${BINARY_DIR}/static-pointer-arrays"
)
set(global_function_pointer_source
    "${NY_C_CORPUS_external-global-function-pointer.c}")

nyc_execute_ok(
  [=[nyc failed to compile an external global function-pointer call\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -c -std=gnu11 "${global_function_pointer_source}"
            -o "${BINARY_DIR}/external-global-function-pointer.o"
)
set(global_function_pointer_harness
    "${NY_C_CORPUS_external-global-function-pointer-main.c}")

nyc_execute_ok(
  [=[linking an external global function-pointer call failed\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${global_function_pointer_harness}"
            "${BINARY_DIR}/external-global-function-pointer.o"
            -o "${BINARY_DIR}/external-global-function-pointer"
)
nyc_execute_ok(
  [=[external global function-pointer call returned an incorrect value\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/external-global-function-pointer"
)
set(multidimensional_array_source
    "${NY_C_CORPUS_external-multidimensional-array.c}")

nyc_execute_ok(
  [=[nyc multidimensional external array compilation failed\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -fms-extensions -c "${multidimensional_array_source}"
            -o "${BINARY_DIR}/external-multidimensional-array.o"
)
set(multidimensional_array_harness
    "${NY_C_CORPUS_external-multidimensional-array-host.c}")

nyc_execute_ok(
  [=[host compiler could not link the NYC multidimensional-array test\n${stdout}${stderr}]=]
  COMMAND "${CC}" -fms-extensions "${multidimensional_array_harness}"
            "${BINARY_DIR}/external-multidimensional-array.o"
            -o "${BINARY_DIR}/external-multidimensional-array-host"
)
nyc_execute_ok(
  [=[NYC multidimensional external-array stride or value mismatch\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/external-multidimensional-array-host"
)
foreach(flag
    -mcmodel=large
    -mstack-protector-guard=gs
    )
  execute_process(
    COMMAND "${NYC}" -E "${flag}" "${static_assert_source}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(status EQUAL 0)
    message(FATAL_ERROR
      "nyc did not reject unsupported semantic option ${flag}\n${stdout}${stderr}")
  endif()
endforeach()
execute_process(
  COMMAND "${NYC}" -fcf-protection=full -E "${static_assert_source}"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(status EQUAL 0 OR
   NOT stderr MATCHES "unsupported control-flow protection mode 'full'")
  message(FATAL_ERROR
    "nyc did not reject unsupported full control-flow protection\n${stdout}${stderr}")
endif()
set(cf_protection_source "${NY_C_CORPUS_cf-protection-branch.c}")
file(WRITE "${BINARY_DIR}/ibt-target.c" [=[
int ibt_target(void) { volatile int v = 5; return v; }
]=])
file(WRITE "${BINARY_DIR}/ibt-main.c" [=[
#include <stdio.h>
int ibt_target(void);
int main(void) {
  unsigned char *p = (unsigned char *)(void *)ibt_target;
  printf("%d %d %d %d %d\n", p[0], p[1], p[2], p[3], ibt_target());
  return 0;
}
]=])
nyc_execute_ok(
  [=[IBT object compilation failed\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O0 -fcf-protection=branch -c "${BINARY_DIR}/ibt-target.c"
          -o "${BINARY_DIR}/ibt-target.o"
)
nyc_execute_ok(
  [=[IBT caller compilation failed\n${stdout}${stderr}]=]
  COMMAND "${CC}" -c "${BINARY_DIR}/ibt-main.c"
          -o "${BINARY_DIR}/ibt-main.o"
)
nyc_execute_ok(
  [=[IBT object link failed\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/ibt-target.o" "${BINARY_DIR}/ibt-main.o"
          -o "${BINARY_DIR}/ibt-object"
)
execute_process(
  COMMAND "${BINARY_DIR}/ibt-object"
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT status EQUAL 0 OR NOT stdout STREQUAL "243 15 30 250 5\n")
  message(FATAL_ERROR
    "IBT object entry is missing its endbr64 landing pad: ${stdout}${stderr}")
endif()
set(rethunk_source "${NY_C_CORPUS_cf-protection-branch.c}")
file(WRITE "${BINARY_DIR}/return-thunk-target.c" [=[
int rethunk_target(int selector) {
  if (selector == 0) return 3;
  volatile int v = 7;
  return v;
}
]=])
file(WRITE "${BINARY_DIR}/return-thunk-stub.s" [=[
	.text
	.globl __x86_return_thunk
	.type __x86_return_thunk,@function
__x86_return_thunk:
	addl $1, %eax
	ret
	.size __x86_return_thunk, .-__x86_return_thunk
]=])
file(WRITE "${BINARY_DIR}/return-thunk-main.c" [=[
#include <stdio.h>
int rethunk_target(int);
int main(void) {
  printf("%d %d\n", rethunk_target(0), rethunk_target(1));
  return 0;
}
]=])
nyc_execute_ok(
  [=[return-thunk object compilation failed\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O0 -mfunction-return=thunk-extern -c
          "${BINARY_DIR}/return-thunk-target.c"
          -o "${BINARY_DIR}/return-thunk-target.o"
)
nyc_execute_ok(
  [=[return-thunk stub assembly failed\n${stdout}${stderr}]=]
  COMMAND "${CC}" -c "${BINARY_DIR}/return-thunk-stub.s"
          -o "${BINARY_DIR}/return-thunk-stub.o"
)
nyc_execute_ok(
  [=[return-thunk main compilation failed\n${stdout}${stderr}]=]
  COMMAND "${CC}" -c "${BINARY_DIR}/return-thunk-main.c"
          -o "${BINARY_DIR}/return-thunk-main.o"
)
nyc_execute_ok(
  [=[return-thunk link failed\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/return-thunk-target.o"
          "${BINARY_DIR}/return-thunk-stub.o"
          "${BINARY_DIR}/return-thunk-main.o" -o "${BINARY_DIR}/return-thunk"
)
execute_process(
  COMMAND "${BINARY_DIR}/return-thunk"
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT status EQUAL 0 OR NOT stdout STREQUAL "4 8\n")
  message(FATAL_ERROR
    "return-thunk object runtime failed: ${stdout}${stderr}")
endif()
nyc_execute_ok(
  [=[nyc could not emit return-thunk C assembly\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O0 -mfunction-return=thunk-extern -S "${rethunk_source}"
            -o "${BINARY_DIR}/return-thunk.s"
)
file(READ "${BINARY_DIR}/return-thunk.s" rethunk_assembly)
string(FIND "${rethunk_assembly}" "jmp\t__x86_return_thunk" rethunk_offset)
if(rethunk_offset EQUAL -1)
  message(FATAL_ERROR
    "NYC accepted -mfunction-return=thunk-extern but emitted no __x86_return_thunk")
endif()
nyc_execute_ok(
  [=[nyc could not emit branch-protected C functions\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O0 -fcf-protection=branch -S "${cf_protection_source}"
            -o "${BINARY_DIR}/cf-protection-branch.s"
)
file(READ "${BINARY_DIR}/cf-protection-branch.s" cf_protection_assembly)
foreach(symbol IN ITEMS nyc_cf_target main)
  string(FIND "${cf_protection_assembly}"
         "${symbol}:\n\tendbr64\n" landing_pad_offset)
  if(landing_pad_offset EQUAL -1)
    message(FATAL_ERROR
      "NYC omitted endbr64 at branch-protected entry ${symbol}")
  endif()
endforeach()
nyc_execute_ok(
  [=[host assembler rejected branch-protected C output\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/cf-protection-branch.s"
            -o "${BINARY_DIR}/cf-protection-branch"
)
nyc_execute_ok(
  [=[branch-protected indirect call changed runtime behavior\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/cf-protection-branch"
)
nyc_execute_ok(
  [=[NYC could not protect computed-label targets\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -fcf-protection=branch -DEXPECT_IBT=1 -S
            "${NY_C_CORPUS_computed-goto-label-table.c}"
            -o "${BINARY_DIR}/ibt-label-targets.s"
)
nyc_execute_ok(
  [=[computed-label IBT output did not assemble\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/ibt-label-targets.s"
            -o "${BINARY_DIR}/ibt-label-targets"
)
nyc_execute_ok(
  [=[computed goto reached an invalid IBT landing pad (${status})\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/ibt-label-targets"
)
set(pic-lowercase-source "${NY_C_CORPUS_pic-lowercase.c}")

nyc_execute_ok(
  [=[nyc rejected the Linux vDSO -fpic option\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -fpic -fno-omit-frame-pointer
            -foptimize-sibling-calls -fasynchronous-unwind-tables -O2 -S
            "${pic-lowercase-source}"
            -o "${BINARY_DIR}/pic-lowercase.s"
)
nyc_execute_ok(
  [=[external assembler rejected NYC -fpic output\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/pic-lowercase.s"
            -o "${BINARY_DIR}/pic-lowercase"
)
nyc_execute_ok(
  [=[NYC -fpic global access failed\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/pic-lowercase"
)
execute_process(
  COMMAND "${NYC}" -dM -E -x c -DNYC_DRIVER_PROBE=9 -U__NYC__ /dev/null
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0 OR
   NOT stdout MATCHES "#define NYC_DRIVER_PROBE 9" OR
   NOT stdout MATCHES "#define __BYTE_ORDER__ 1234" OR
   stdout MATCHES "#define __NYC__")
  message(FATAL_ERROR
    "nyc -dM did not report compiler/target macros\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${NYC}" -print-file-name=plugin
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0 OR NOT stdout STREQUAL "plugin\n")
  message(FATAL_ERROR
    "nyc plugin lookup must fail closed as unavailable\n${stdout}${stderr}")
endif()
find_program(NYC_AS_EXECUTABLE as)
if(NYC_AS_EXECUTABLE)
  execute_process(
    COMMAND "${NYC_AS_EXECUTABLE}" --version
    RESULT_VARIABLE as_status
    OUTPUT_VARIABLE as_stdout
    ERROR_VARIABLE as_stderr
  )
  execute_process(
    COMMAND "${NYC}" -Wa,--version -c -x assembler-with-cpp /dev/null -o /dev/null
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT as_status EQUAL 0 OR NOT status EQUAL 0 OR
     NOT stdout STREQUAL as_stdout OR NOT stderr STREQUAL as_stderr)
    message(FATAL_ERROR
      "nyc assembler version query did not report the host assembler result\n"
      "as: ${as_status}\n${as_stdout}${as_stderr}"
      "nyc: ${status}\n${stdout}${stderr}")
  endif()
  file(WRITE "${BINARY_DIR}/as-probe-input" [=[
#define LANDING endbr64
LANDING
]=])
  nyc_execute_ok(
    [=[assembler-with-cpp stdin probe failed]=]
    COMMAND "${NYC}" -Wa,--fatal-warnings -c -x assembler-with-cpp
            -o "${BINARY_DIR}/as-probe.o" -
    INPUT_FILE "${BINARY_DIR}/as-probe-input"
  )
  if(OBJDUMP)
    nyc_execute_ok(
      [=[assembler-with-cpp probe did not produce an object]=]
      COMMAND "${OBJDUMP}" -d "${BINARY_DIR}/as-probe.o"
    )
    if(NOT stdout MATCHES "endbr64")
      message(FATAL_ERROR "preprocessed probe did not assemble endbr64\n${stdout}")
    endif()
  endif()
  file(WRITE "${BINARY_DIR}/as-warning-input" ".warning \"expected probe diagnostic\"\n")
  execute_process(
    COMMAND "${NYC}" -Wa,--fatal-warnings -c -x assembler
            -o "${BINARY_DIR}/as-warning.o" -
    INPUT_FILE "${BINARY_DIR}/as-warning-input"
    RESULT_VARIABLE status ERROR_VARIABLE stderr
  )
  if(NOT status STREQUAL "1" OR NOT stderr MATCHES "expected probe diagnostic")
    message(FATAL_ERROR "assembler warning was not fatal: ${status}\n${stderr}")
  endif()
endif()
# Driver flags, preprocessing, and target option behavior.
set(i386_data_model_source "${NY_C_CORPUS_i386-data-model.c}")
execute_process(
  COMMAND "${NYC}" -c -m32 "${i386_data_model_source}" -o
          "${BINARY_DIR}/i386-data-model.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
# i386 whole-unit C now reaches GNU as, so the old "requires x86-64 ELF"
# driver gate no longer applies. This fixture deliberately uses alternative
# inline asm that the i386 text emitter does not encode, so the build must
# still fail at that boundary rather than silently dropping the statement.
if(status EQUAL 0 OR
   NOT "${stdout}${stderr}" MATCHES
       "(i386 assembly emission failed|unsupported inline assembly)" OR
   "${stdout}${stderr}" MATCHES "_Static_assert condition is false")
  message(FATAL_ERROR
    "nyc i386 data-model assertions failed before the i386 asm emitter boundary\n${stdout}${stderr}")
endif()
if(NYC_AS_EXECUTABLE)
  set(i386_vdso_ops_source "${NY_C_CORPUS_i386-vdso-ops.c}")
  nyc_execute_ok(
    [=[nyc could not emit i386 VDSO syscall and arithmetic assembly\n${stdout}${stderr}]=]
    COMMAND "${NYC}" -m32 -S "${i386_vdso_ops_source}" -o
                "${BINARY_DIR}/i386-vdso-ops.s"
  )
  file(READ "${BINARY_DIR}/i386-vdso-ops.s" i386_vdso_ops_assembly)
  if(NOT i386_vdso_ops_assembly MATCHES "int[ \t]+\\$0x80" OR
     NOT i386_vdso_ops_assembly MATCHES "(div64_[0-9]+_loop|divl[ \t]+)")
    message(FATAL_ERROR
      "i386 VDSO lowering omitted syscall or unsigned division code")
  endif()
  nyc_execute_ok(
    [=[GNU as --32 rejected NYC's i386 VDSO output\n${stdout}${stderr}]=]
    COMMAND "${NYC_AS_EXECUTABLE}" --32 -o
                "${BINARY_DIR}/i386-vdso-ops.o"
                "${BINARY_DIR}/i386-vdso-ops.s"
  )
  set(i386_lsl_source "${NY_C_CORPUS_i386-lsl-alternative.c}")
  nyc_execute_ok(
    [=[nyc could not lower the i386 LSL alternative\n${stdout}${stderr}]=]
    COMMAND "${NYC}" -m32 -S "${i386_lsl_source}" -o
                "${BINARY_DIR}/i386-lsl-alternative.s"
  )
  file(READ "${BINARY_DIR}/i386-lsl-alternative.s" i386_lsl_assembly)
  if(NOT i386_lsl_assembly MATCHES "lsll[ \t]+%cx,[ \t]*%eax")
    message(FATAL_ERROR
      "NYC's i386 LSL alternative did not emit the LSL instruction")
  endif()
  nyc_execute_ok(
    [=[GNU as --32 rejected NYC's i386 LSL output\n${stdout}${stderr}]=]
    COMMAND "${NYC_AS_EXECUTABLE}" --32 -o
                "${BINARY_DIR}/i386-lsl-alternative.o"
                "${BINARY_DIR}/i386-lsl-alternative.s"
  )
  if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    # GCC and Clang disagree on variadic sret popping under -mregparm.
    # NYC follows GCC; use its ABI as the oracle for this GNU option.
    find_program(NYC_GNU_CC NAMES gcc)
    set(i386_probes i386-aggregate-abi i386-spill-arithmetic i386-register-runs
                    asm-bit-string-rmw asm-fninit-status
                    asm-repeat-cmpsb asm-divl-outputs asm-repeat-copy-tail
                    asm-segment-memory asm-es-fill i386-pointer-member-store)
    if(NYC_GNU_CC)
      list(APPEND i386_probes i386-varargs)
    endif()
    foreach(regparm RANGE 0 3)
      foreach(probe IN LISTS i386_probes)
        set(i386_probe_cc "${CC}")
        if(probe STREQUAL "i386-varargs")
          set(i386_probe_cc "${NYC_GNU_CC}")
        endif()
        set(i386_host_source "${NY_C_CORPUS_${probe}-host.c}")
        if(probe STREQUAL "asm-segment-memory")
          set(i386_host_source "${NY_C_CORPUS_asm-segment-memory-host32.c}")
        elseif(probe STREQUAL "asm-bit-string-rmw" OR probe STREQUAL "asm-fninit-status"
           OR probe STREQUAL "asm-repeat-cmpsb" OR probe STREQUAL "asm-divl-outputs"
           OR probe STREQUAL "asm-repeat-copy-tail")
          set(i386_host_source "${NY_C_CORPUS_i386-main-entry.c}")
        endif()
        set(i386_opt_levels 3)
        if(probe STREQUAL "i386-spill-arithmetic" OR probe STREQUAL "i386-register-runs")
          set(i386_opt_levels 0 1 2 3 s z)
        endif()
        foreach(i386_opt IN LISTS i386_opt_levels)
        set(i386_abi_binary "${BINARY_DIR}/${probe}-${regparm}-O${i386_opt}")
        nyc_execute_ok(
          [=[${probe} regparm=${regparm} compilation failed\n${stdout}${stderr}]=]
          COMMAND "${NYC}" -m32 "-mregparm=${regparm}" "-O${i386_opt}" -S
                            "${NY_C_CORPUS_${probe}.c}"
                            -o "${i386_abi_binary}.s"
        )
        nyc_execute_ok(
          [=[${probe} regparm=${regparm} link failed\n${stdout}${stderr}]=]
          COMMAND "${i386_probe_cc}" -m32 "-mregparm=${regparm}" -O2 -ffreestanding
                            -fno-stack-protector -fno-pie -nostdlib -no-pie -Wl,-e,_start
                            "${i386_host_source}"
                            "${i386_abi_binary}.s" -o "${i386_abi_binary}"
        )
        nyc_execute_ok(
          [=[${probe} O${i386_opt} regparm=${regparm} returned incorrect results: ${status}\n${stdout}${stderr}]=]
          COMMAND "${i386_abi_binary}"
        )
        endforeach()
      endforeach()
    endforeach()
    if(NYC_GNU_CC)
      set(i386_short_stack "${BINARY_DIR}/i386-register-runs-short-stack")
      nyc_execute_ok(
        [=[i386 four-byte-stack compilation failed\n${stdout}${stderr}]=]
        COMMAND "${NYC}" -m32 -mregparm=3 -mpreferred-stack-boundary=2 -Os -S
                "${NY_C_CORPUS_i386-register-runs.c}"
                -o "${i386_short_stack}.s"
      )
      nyc_execute_ok(
        [=[i386 four-byte-stack link failed\n${stdout}${stderr}]=]
        COMMAND "${NYC_GNU_CC}" -m32 -mregparm=3 -mpreferred-stack-boundary=2
                -Os -ffreestanding -fno-stack-protector -fno-pie -nostdlib
                -no-pie -Wl,-e,_start "${NY_C_CORPUS_i386-register-runs-host.c}"
                "${i386_short_stack}.s" -o "${i386_short_stack}"
      )
      nyc_execute_ok(
        [=[i386 four-byte-stack register permutation failed: ${status}\n${stdout}${stderr}]=]
        COMMAND "${i386_short_stack}"
      )
    endif()
    set(far_call_source "${NY_C_CORPUS_asm-far-call.c}")
    set(far_call_assembly "${BINARY_DIR}/asm-far-call.s")
    nyc_execute_ok(
      [=[NYC could not lower the i386 memory far-call template\n${stdout}${stderr}]=]
      COMMAND "${NYC}" -m16 -mregparm=0 -O2 -S
                    "${far_call_source}" -o "${far_call_assembly}"
    )
    file(READ "${far_call_assembly}" far_call_text)
    if(NOT far_call_text MATCHES "lcallw[ \t]+\\*\\(%eax\\)")
      message(FATAL_ERROR
        "NYC emitted an unexpected i386 far-call sequence\n${far_call_text}")
    endif()
    nyc_execute_ok(
      [=[GNU as --32 rejected NYC's far-call output\n${stdout}${stderr}]=]
      COMMAND "${NYC_AS_EXECUTABLE}" --32 -o
                    "${BINARY_DIR}/asm-far-call.o" "${far_call_assembly}"
    )
  endif()
endif()
execute_process(
  COMMAND "${NYC}" -m32 -dM -E -
  INPUT_FILE "${NY_C_CORPUS_no-sse-integer.c}"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0 OR NOT stdout MATCHES "#define __i386__ 1" OR
   NOT stdout MATCHES "#define __ILP32__ 1" OR
   stdout MATCHES "#define __x86_64__ 1" OR
   stdout MATCHES "#define __LP64__ 1" OR
   stdout MATCHES "#define __SIZEOF_INT128__ 16" OR
   NOT stdout MATCHES "#define __SIZEOF_LONG__ 4" OR
   NOT stdout MATCHES "#define __SIZEOF_POINTER__ 4" OR
   NOT stdout MATCHES "#define __SIZE_TYPE__ unsigned int" OR
   NOT stdout MATCHES "#define __PTRDIFF_TYPE__ int" OR
   NOT stdout MATCHES "#define __INT64_TYPE__ long long int" OR
   NOT stdout MATCHES "#define __UINT64_TYPE__ long long unsigned int")
  message(FATAL_ERROR
    "nyc i386 target predefined types are inconsistent\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${NYC}" -m64 -dM -E -
  INPUT_FILE "${NY_C_CORPUS_no-sse-integer.c}"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0 OR NOT stdout MATCHES "#define __x86_64__ 1" OR
   NOT stdout MATCHES "#define __LP64__ 1" OR
   NOT stdout MATCHES "#define __SIZEOF_LONG__ 8" OR
   NOT stdout MATCHES "#define __SIZEOF_POINTER__ 8")
  message(FATAL_ERROR
    "nyc x86-64 target predefined types changed unexpectedly\n${stdout}${stderr}")
endif()
# A unit without symbols must still assemble to an object with a .symtab,
# which objtool and other ELF consumers require.
file(WRITE "${BINARY_DIR}/empty-unit.c" "")
nyc_execute_ok(
  [=[nyc failed to emit assembly for an empty translation unit]=]
  COMMAND "${NYC}" -S "${BINARY_DIR}/empty-unit.c"
            -o "${BINARY_DIR}/empty-unit.s"
)
nyc_execute_ok(
  [=[could not assemble the empty translation unit]=]
  COMMAND "${CC}" -c "${BINARY_DIR}/empty-unit.s"
            -o "${BINARY_DIR}/empty-unit.o"
)
if(READELF)
  execute_process(COMMAND "${READELF}" -S "${BINARY_DIR}/empty-unit.o"
    OUTPUT_VARIABLE empty_unit_sections)
  if(NOT empty_unit_sections MATCHES "\\.symtab")
    message(FATAL_ERROR "empty translation unit object has no .symtab")
  endif()
endif()
set(no_sse_source "${NY_C_CORPUS_no-sse-integer.c}")
set(no_sse_harness "${NY_C_CORPUS_no-sse-main.c}")

foreach(route IN ITEMS -c -S)
  set(stem "${BINARY_DIR}/no-sse-integer${route}")
  set(output "${stem}.o")
  if(route STREQUAL "-S")
    set(output "${stem}.s")
  endif()
  nyc_execute_ok(
    [=[nyc scalar integer compilation with no-SSE flags failed (${route})]=]
    COMMAND "${NYC}" "${route}" -std=gnu11 -O2 -mno-sse -mno-sse2 -mno-mmx
              -mno-3dnow -mno-avx -mno-sse4a -mno-80387
              -mno-fp-ret-in-387 -march=x86-64 -mtune=generic -mno-red-zone
              -mpreferred-stack-boundary=3
              -mcmodel=kernel -mcmodel=small -fno-stack-check -fno-strict-overflow
              -falign-functions=16 -mskip-rax-setup
              "${no_sse_source}" -o "${output}"
  )
  if(route STREQUAL "-S")
    nyc_execute_ok(
      [=[could not assemble no-SSE NYC test output]=]
      COMMAND "${CC}" -c "${output}" -o "${stem}.o"
    )
  endif()
  if(OBJDUMP)
    nyc_execute_ok(
      [=[could not disassemble no-SSE NYC object (${route})]=]
      COMMAND "${OBJDUMP}" -d "${stem}.o"
    )
    if(stdout MATCHES "%(xmm|ymm|zmm|mm[0-7])")
      message(FATAL_ERROR
        "NYC emitted SIMD registers under no-SSE flags (${route})\n${stdout}")
    endif()
  endif()
  nyc_execute_ok(
    [=[could not link no-SSE NYC behavior test (${route})]=]
    COMMAND "${CC}" "${no_sse_harness}" "${stem}.o" -o "${stem}"
  )
  nyc_execute_ok(
    [=[no-SSE NYC integer function returned a wrong result (${route})]=]
    COMMAND "${stem}"
  )
endforeach()
set(no_sse_float_source "${NY_C_CORPUS_no-sse-float.c}")

execute_process(
  COMMAND "${NYC}" -c -std=gnu11 -mno-sse "${no_sse_float_source}"
          -o "${BINARY_DIR}/no-sse-float.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(status EQUAL 0 OR NOT stderr MATCHES "floating-point or SIMD code")
  message(FATAL_ERROR
    "nyc did not reject floating-point code under -mno-sse\n${stdout}${stderr}")
endif()
set(wchar_source "${NY_C_CORPUS_short-wchar.c}")

nyc_execute_ok(
  [=[nyc -fshort-wchar semantics failed\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -c -std=gnu11 -fshort-wchar "${wchar_source}"
            -o "${BINARY_DIR}/short-wchar-nyc.o"
)
nyc_execute_ok(
  [=[reference compiler rejected short wchar_t assertions\n${stdout}${stderr}]=]
  COMMAND "${CC}" -std=gnu11 -fshort-wchar -c "${wchar_source}"
            -o "${BINARY_DIR}/short-wchar-reference.o"
)
set(char_source "${NY_C_CORPUS_char-signedness.c}")

set(char_harness "${NY_C_CORPUS_char-signedness-main.c}")

foreach(flag IN ITEMS "" "-funsigned-char")
  set(suffix "signed")
  if(flag)
    set(suffix "unsigned")
  endif()
  nyc_execute_ok(
    [=[nyc char signedness semantics failed (${suffix})\n${stdout}${stderr}]=]
    COMMAND "${NYC}" -c -std=gnu11 ${flag} "${char_source}"
                -o "${BINARY_DIR}/char-${suffix}-nyc.o"
  )
  nyc_execute_ok(
    [=[reference char signedness assertions failed (${suffix})\n${stdout}${stderr}]=]
    COMMAND "${CC}" -std=gnu11 ${flag} -c "${char_source}"
                -o "${BINARY_DIR}/char-${suffix}-reference.o"
  )
  nyc_execute_ok(
    [=[reference char signedness harness failed (${suffix})\n${stdout}${stderr}]=]
    COMMAND "${CC}" -std=gnu11 ${flag} -c "${char_harness}"
                -o "${BINARY_DIR}/char-${suffix}-main.o"
  )
  nyc_execute_ok(
    [=[could not link NYC char signedness output (${suffix})\n${stdout}${stderr}]=]
    COMMAND "${CC}" "${BINARY_DIR}/char-${suffix}-main.o"
                "${BINARY_DIR}/char-${suffix}-nyc.o"
                -o "${BINARY_DIR}/char-${suffix}-check"
  )
  nyc_execute_ok(
    [=[NYC char signedness runtime check failed (${suffix})\n${stdout}${stderr}]=]
    COMMAND "${BINARY_DIR}/char-${suffix}-check"
  )
endforeach()
set(alias_source "${NY_C_CORPUS_no-strict-aliasing.c}")

set(alias_harness "${NY_C_CORPUS_no-strict-aliasing-main.c}")

nyc_execute_ok(
  [=[nyc -fno-strict-aliasing compile failed\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -c -std=gnu11 -fno-strict-aliasing "${alias_source}"
            -o "${BINARY_DIR}/no-strict-aliasing-nyc.o"
)
nyc_execute_ok(
  [=[alias harness compile failed\n${stdout}${stderr}]=]
  COMMAND "${CC}" -std=gnu11 -c "${alias_harness}"
            -o "${BINARY_DIR}/no-strict-aliasing-main.o"
)
nyc_execute_ok(
  [=[alias harness link failed\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/no-strict-aliasing-main.o"
            "${BINARY_DIR}/no-strict-aliasing-nyc.o"
            -o "${BINARY_DIR}/no-strict-aliasing-check"
)
nyc_execute_ok(
  [=[NYC -fno-strict-aliasing runtime check failed\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/no-strict-aliasing-check"
)
set(driver_include "${BINARY_DIR}/driver-forced-include.h")
set(driver_source "${NY_C_CORPUS_driver-forced-include.c}")
set(driver_object "${BINARY_DIR}/driver-forced-include.o")
set(driver_depfile "${BINARY_DIR}/driver-forced-include.d")
get_filename_component(driver_binary_real "${BINARY_DIR}" REALPATH)
get_filename_component(driver_source_real "${driver_source}" REALPATH)
get_filename_component(driver_fixture_real "${NY_C_CORPUS_DIR}" REALPATH)
configure_file("${NY_C_CORPUS_driver-forced-include.h}" "${driver_include}" COPYONLY)

execute_process(
  COMMAND "${NYC}" -E -P "-I${BINARY_DIR}" -include "${driver_include}"
          "-fmacro-prefix-map=${BINARY_DIR}=mapped"
          "-fmacro-prefix-map=${driver_binary_real}=mapped"
          "-fmacro-prefix-map=${driver_fixture_real}=mapped"
          "-fmacro-prefix-map=${driver_source_real}=mapped-source"
          "-fmacro-prefix-map=${driver_source}=mapped-source"
          "${driver_source}"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0 OR
   NOT stdout MATCHES "mapped-source" OR
   NOT stdout MATCHES "mapped/driver-forced-include\\.h")
  message(FATAL_ERROR
    "nyc -fmacro-prefix-map did not rewrite __FILE__\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${NYC}" -E -P "-I${BINARY_DIR}" -include "${driver_include}"
          "-fmacro-prefix-map=${driver_source}=mapped-source"
          "-fmacro-prefix-map=${BINARY_DIR}=mapped"
          "-fmacro-prefix-map=${driver_binary_real}=mapped"
          "-fmacro-prefix-map=${driver_fixture_real}=mapped"
          "${driver_source}"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0 OR
   NOT stdout MATCHES "mapped/driver-forced-include\\.c" OR
   stdout MATCHES "mapped-source" OR
   NOT stdout MATCHES "mapped/driver-forced-include\\.h")
  message(FATAL_ERROR
    "nyc -fmacro-prefix-map did not honor mapping order\n${stdout}${stderr}")
endif()
nyc_remove_artifact("${driver_depfile}")
nyc_execute_output(
  [=[nyc forced-include compile failed\n${stdout}${stderr}]=]
  "${driver_object}"
  COMMAND "${NYC}" -c -nostdinc -std=gnu11 -m64 -fno-PIE
            -fms-extensions -Werror "-I${BINARY_DIR}"
            -include "${driver_include}" "-Wp,-MMD,${driver_depfile}"
            "${driver_source}" -o "${driver_object}"
)
nyc_require_artifact("nyc forced-include dependencies" "${driver_depfile}")
file(READ "${driver_depfile}" driver_dependencies)
if(NOT driver_dependencies MATCHES
   "driver-forced-include\\.h" OR NOT driver_dependencies MATCHES
   "driver-forced-include\\.c")
  message(FATAL_ERROR
    "nyc dependency output omitted the forced header or source: ${driver_dependencies}")
endif()

nyc_execute_output(
  [=[linking forced-include NYC object failed\n${stdout}${stderr}]=]
  "${BINARY_DIR}/driver-forced-include-host"
  COMMAND "${CC}" -o "${BINARY_DIR}/driver-forced-include-host"
            "${NY_C_CORPUS_driver-forced-include-host.c}" "${driver_object}"
)
nyc_execute_ok(
  [=[forced include did not affect the emitted object as expected\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/driver-forced-include-host"
)
execute_process(
  COMMAND "${NYC}" -E -I "${BINARY_DIR}" -include "${driver_include}" -
  INPUT_FILE "${driver_source}"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0 OR NOT stdout MATCHES "return 37")
  message(FATAL_ERROR
    "nyc forced-include preprocessing from stdin failed\n${stdout}${stderr}")
endif()
set(preprocess_depfile "${BINARY_DIR}/driver-preprocess.d")
nyc_remove_artifact("${preprocess_depfile}")
nyc_execute_output("nyc preprocessing failed" "${BINARY_DIR}/driver-preprocess.i"
  COMMAND "${NYC}" -E -I "${BINARY_DIR}" -include "${driver_include}"
          "-Wp,-MMD,${preprocess_depfile}" -o
          "${BINARY_DIR}/driver-preprocess.i" "${driver_source}"
)
nyc_require_artifact("nyc preprocessing dependencies" "${preprocess_depfile}")
file(READ "${preprocess_depfile}" preprocess_dependencies)
if(NOT preprocess_dependencies MATCHES
   "driver-forced-include\\.h" OR NOT preprocess_dependencies MATCHES
   "driver-forced-include\\.c")
  message(FATAL_ERROR
    "nyc preprocessing dependencies omitted input/header: ${preprocess_dependencies}")
endif()
set(stdin_preprocess_depfile "${BINARY_DIR}/stdin-preprocess.d")
nyc_execute_output("nyc stdin preprocessing dependencies" "${stdin_preprocess_depfile}"
  COMMAND "${NYC}" -E -I "${BINARY_DIR}"
          "-Wp,-MMD,${stdin_preprocess_depfile}" -
  INPUT_FILE "${driver_source}"
)
file(READ "${stdin_preprocess_depfile}" stdin_preprocess_dependencies)
if(NOT stdin_preprocess_dependencies MATCHES "^-[^:]*:" OR
   NOT stdin_preprocess_dependencies MATCHES
   "driver-forced-include\\.h")
  message(FATAL_ERROR
    "nyc stdin dependencies omitted the target or forced header: ${stdin_preprocess_dependencies}")
endif()
configure_file("${NY_C_CORPUS_variadic.h}"
               "${BINARY_DIR}/variadic.h" COPYONLY)

foreach(probe IN ITEMS variadic-comma-elision wide-variadic)
  nyc_execute_ok(
    [=[nyc ${probe} compilation failed\n${stdout}${stderr}]=]
    COMMAND "${NYC}" -O3 -S "-I${BINARY_DIR}"
                "${NY_C_CORPUS_${probe}.c}"
                -o "${BINARY_DIR}/${probe}.s"
  )
  nyc_execute_ok(
    [=[${probe} link failed\n${stdout}${stderr}]=]
    COMMAND "${CC}" "${BINARY_DIR}/${probe}.s" -o "${BINARY_DIR}/${probe}"
  )
  nyc_execute_ok(
    [=[${probe} returned incorrect results\n${stdout}${stderr}]=]
    COMMAND "${BINARY_DIR}/${probe}"
  )
endforeach()

set(expanded_header_source "${NY_C_CORPUS_expanded-header-path.c}")
configure_file("${NY_C_CORPUS_variadic.h}"
               "${BINARY_DIR}/trace/events/variadic.h" COPYONLY)

execute_process(
  COMMAND "${NYC}" -E -P "-I${BINARY_DIR}" "${expanded_header_source}"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0 OR NOT stdout MATCHES "expanded_header_path=1")
  message(FATAL_ERROR
    "nyc macro-expanded header path failed\n${stdout}${stderr}")
endif()

set(anonymous_aggregate_source "${NY_C_CORPUS_anonymous-aggregate.c}")
set(anonymous_aggregate_object "${BINARY_DIR}/anonymous-aggregate.o")

nyc_execute_ok(
  [=[nyc anonymous aggregate compile failed\n${stdout}${stderr}]=]
  COMMAND "${NYC}" --all "${anonymous_aggregate_source}"
            "${anonymous_aggregate_object}"
)

nyc_execute_ok(
  [=[linking anonymous aggregate object failed\n${stdout}${stderr}]=]
  COMMAND "${CC}" -o "${BINARY_DIR}/anonymous-aggregate-host"
            "${NY_C_CORPUS_anonymous-aggregate-host.c}"
            "${anonymous_aggregate_object}"
)
nyc_execute_ok(
  [=[anonymous aggregate field offsets or values mismatched\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/anonymous-aggregate-host"
)

set(static_local_section_source
  "${NY_C_CORPUS_static-local-section.c}")

nyc_execute_ok(
  [=[nyc failed to compile attributed static local array\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -S "${static_local_section_source}"
            -o "${BINARY_DIR}/static-local-section.s"
)

nyc_execute_ok(
  [=[host compiler rejected attributed static local assembly\n${stdout}${stderr}]=]
  COMMAND "${CC}" -o "${BINARY_DIR}/static-local-section"
            "${NY_C_CORPUS_static-local-section-host.c}"
            "${BINARY_DIR}/static-local-section.s"
)
nyc_execute_ok(
  [=[attributed static local array did not preserve static storage\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/static-local-section"
)

set(array_member_designator_source
    "${NY_C_CORPUS_array-member-designator.c}")
nyc_execute_ok(
  [=[nyc failed to compile chained array/member designators\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O3 -S "${array_member_designator_source}"
            -o "${BINARY_DIR}/array-member-designator.s"
)
nyc_execute_ok(
  [=[host linker rejected chained array/member designators\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/array-member-designator.s"
            -o "${BINARY_DIR}/array-member-designator"
)
nyc_execute_ok(
  [=[chained array/member initializer data was incorrect\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/array-member-designator"
)

# Per-function C lowering and object compilation.
set(cases
  "structs.c|c_point_after_three|point-after-three.o"
  "structs.c|c_point_after_four|point-after-four.o"
  "structs.c|c_point_zero_array|point-zero-array.o"
  "structs.c|c_point_member_assign|point-member-assign.o"
  "structs.c|c_bitfields_update|bitfields-update.o"
  "structs.c|c_bitfield_tail_update|bitfield-tail-update.o"
  "structs.c|c_unnamed_bitfield_offset_offset|unnamed-bitfield-offset.o"
  "structs.c|c_unnamed_bitfield_constant_offset|unnamed-bitfield-constant.o"
  "structs.c|c_point_element_address|point-element-address.o"
  "structs.c|c_point_pointer_element|point-pointer-element.o"
  "structs.c|c_point_pointer_set|point-pointer-set.o"
  "structs.c|c_point_pointer_slot|point-pointer-slot.o"
  "structs.c|c_byte_pointer_element|byte-pointer-element.o"
  "sysv-byte-array-union-argument.c|intel_slicemask_from_xehp_dssmask|sysv-byte-array-union-callee.o"
  "sysv-byte-array-union-argument.c|c_sseu_mask_argument|sysv-byte-array-union-argument.o"
  "program.c|c_kernel|program.o"
  "target-link.c|c_target_link|target-link.o"
  "void.c|c_void_store|void-store.o"
  "void.c|c_void_store_return|void-store-return.o"
  "void.c|c_void_call|void-call.o"
  "void-pointer.c|c_void_pointer_identity|void-pointer.o"
  "branch.c|c_choose|branch.o"
  "predicate.c|c_predicate|predicate.o"
  "bitwise.c|c_bitwise|bitwise.o"
  "bitwise.c|c_bitwise_select|bitwise-select.o"
  "bitwise.c|c_shift_left|shift-left.o"
  "bitwise.c|c_shift_right|shift-right.o"
  "bitwise.c|c_shift_precedence|shift-precedence.o"
  "bitwise.c|c_shift_additive_precedence|shift-additive-precedence.o"
  "bitwise.c|c_shift_compound|shift-compound.o"
  "logical.c|c_logic_mutate|logical-mutate.o"
  "logical.c|c_logic_and_skip|logical-and-skip.o"
  "logical.c|c_logic_or_skip|logical-or-skip.o"
  "logical.c|c_logic_and_run|logical-and-run.o"
  "logical.c|c_logic_or_run|logical-or-run.o"
  "logical.c|c_logic_and_precedence|logical-and-precedence.o"
  "logical.c|c_logic_or_precedence|logical-or-precedence.o"
  "conditional.c|c_cond_mutate|conditional-mutate.o"
  "conditional.c|c_cond_true|conditional-true.o"
  "conditional.c|c_cond_false|conditional-false.o"
  "conditional.c|c_cond_precedence|conditional-precedence.o"
  "conditional.c|c_cond_nested|conditional-nested.o"
  "increment.c|c_prefix_increment|prefix-increment.o"
  "increment.c|c_prefix_decrement|prefix-decrement.o"
  "increment.c|c_postfix_update|postfix-update.o"
  "increment.c|c_postfix_decrement|postfix-decrement.o"
  "increment.c|c_for_increment|for-increment.o"
  "increment.c|c_while_increment|while-increment.o"
  "increment.c|c_double_postfix|double-postfix.o"
  "do-loop.c|c_do_sum|do-sum.o"
  "do-loop.c|c_do_break|do-break.o"
  "do-loop.c|c_do_nested_conditional|do-nested-conditional.o"
  "do-loop.c|c_do_forever_return|do-forever-return.o"
  "load.c|c_load|load.o"
  "pointer-arithmetic.c|c_pointer_add|pointer-add.o"
  "pointer-arithmetic.c|c_pointer_add_reversed|pointer-add-reversed.o"
  "pointer-arithmetic.c|c_pointer_subtract|pointer-subtract.o"
  "pointer-arithmetic.c|c_pointer_compound_offset|pointer-compound-offset.o"
  "pointer-arithmetic.c|c_pointer_postfix_increment|pointer-postfix-increment.o"
  "pointer-arithmetic.c|c_pointer_prefix_increment|pointer-prefix-increment.o"
  "pointer-arithmetic.c|c_pointer_prefix_decrement|pointer-prefix-decrement.o"
  "pointer-arithmetic.c|c_aggregate_pointer_postfix_increment|aggregate-pointer-postfix-increment.o"
  "pointer-arithmetic.c|c_aggregate_pointer_index|aggregate-pointer-index.o"
  "pointer-arithmetic.c|c_aggregate_pointer_index_stride_56|aggregate-pointer-index-stride-56.o"
  "pointer-arithmetic.c|c_vdbe_register_index|vdbe-register-index.o"
  "pointer-arithmetic.c|c_vdbe_output|vdbe-output.o"
  "pointer-arithmetic.c|c_vdbe_register_output_index|vdbe-register-output-index.o"
  "pointer-arithmetic.c|c_vdbe_register_cast_index|vdbe-register-cast-index.o"
  "pointer-arithmetic.c|c_aggregate_pointer_field_increment|aggregate-pointer-field-increment.o"
  "pointer-arithmetic.c|c_bitfield_layout_pointer_offset|bitfield-layout-pointer-offset.o"
  "pointer-arithmetic.c|c_const_data_pointer_increment|const-data-pointer-increment.o"
  "pointer-arithmetic.c|c_const_pointer_multi_declarator_increment|const-pointer-multi-declarator-increment.o"
  "pointer-arithmetic.c|c_double_pointer_add|double-pointer-add.o"
  "pointer-arithmetic.c|c_double_pointer_compound_offset|double-pointer-compound-offset.o"
  "pointer-arithmetic.c|c_double_pointer_postfix_increment|double-pointer-postfix-increment.o"
  "double-pointer.c|c_f64_load|f64-load.o"
  "double-pointer.c|c_f64_store|f64-store.o"
  "double-pointer.c|c_f64_index|f64-index.o"
  "double-pointer.c|c_f64_array|f64-array.o"
  "double-pointer.c|c_f64_array_forward|f64-array-forward.o"
  "double-pointer.c|c_f64_array_store|f64-array-store.o"
  "double-pointer.c|c_f64_index_store|f64-index-store.o"
  "store.c|c_store|store.o"
  "locals.c|c_locals|locals.o"
  "locals.c|c_many_locals|many-locals.o"
  "locals.c|c_double_local|double-local.o"
  "locals.c|c_local_update|local-update.o"
  "locals.c|c_multi_declarator|multi-declarator.o"
  "locals.c|c_declare_then_assign|declare-then-assign.o"
  "locals.c|c_parameter_update|parameter-update.o"
  "locals.c|c_double_local_update|double-local-update.o"
  "functions.c|c_helper|function-helper.o"
  "functions.c|c_call|function-call.o"
  "functions.c|c_double_helper|double-function-helper.o"
  "functions.c|c_double_call|double-function-call.o"
  "functions.c|c_seven_params|seven-params.o"
  "functions.c|c_seven_call|seven-call.o"
  "structs.c|c_point_sum|point-sum.o"
  "structs.c|c_point_add|point-add.o"
  "structs.c|c_big_sum|big-sum.o"
  "structs.c|c_big_make|big-make.o"
  "structs.c|c_pair_second|pair-second.o"
  "structs.c|c_pair_make|pair-make.o"
  "structs.c|c_mixed_blend|mixed-blend.o"
  "structs.c|c_many_with_struct|many-with-struct.o"
  "structs.c|c_struct_array_roundtrip|struct-array-roundtrip.o"
  "structs.c|c_mixed_declarator_aggregate_copy|mixed-declarator-aggregate-copy.o"

  "structs.c|c_pointer_integer_pointer_layout_offsets|pointer-integer-pointer-layout.o"
  "structs.c|c_pointer_integer_pointer_layout_store|pointer-integer-pointer-layout-store.o"
  "structs.c|c_json_parse_layout_store|json-parse-layout-store.o"
  "structs.c|c_json_cache_layout_store|json-cache-layout-store.o"
  "structs.c|c_json_cache_layout_entry|json-cache-layout-entry.o"
  "structs.c|c_json_cache_layout_insert|json-cache-layout-insert.o"
  "functions.c|c_ten_params|ten-params.o"
  "functions.c|c_ten_call|ten-call.o"
  "functions.c|c_ten_doubles|ten-doubles.o"
  "functions.c|c_ten_double_call|ten-double-call.o"
  "functions.c|c_mixed_abi|mixed-abi.o"
  "functions.c|c_mixed_abi_call|mixed-abi-call.o"
  "functions.c|c_recursive|recursive-call.o"
  "functions.c|c_empty_statements|empty-statements.o"
  "double.c|c_double|double.o"
  "float.c|c_float_roundtrip|float-roundtrip.o"
  "float.c|c_float_negate|float-negate.o"
  "float.c|c_float_select|float-select.o"
  "float.c|c_float_pointer|float-pointer.o"
  "double.c|c_double_choose|double-choose.o"
  "while-loop.c|c_while_sum|while-loop.o"
  "while-loop.c|c_while_early_return|while-early-return.o"
  "while-loop.c|c_while_continue|while-continue.o"
  "while-loop.c|c_while_break|while-break.o"
  "while-loop.c|c_nested_while_control|nested-while-control.o"
  "for-loop.c|c_for_sum|for-loop.o"
  "for-loop.c|c_for_multi_declarators|for-multi-declarators.o"
  "for-loop.c|c_for_assign_sum|for-assign-loop.o"
  "for-loop.c|c_for_body_step|for-body-step.o"
  "for-loop.c|c_for_early_return|for-early-return.o"
  "for-loop.c|c_for_shadow|for-shadow.o"
  "for-loop.c|c_nested_for_sum|nested-for-sum.o"
  "for-loop.c|c_for_continue|for-continue.o"
  "for-loop.c|c_for_break|for-break.o"
  "for-loop.c|c_nested_for_control|nested-for-control.o"
  "indexed.c|c_index_load|indexed-load.o"
  "indexed.c|c_i64_array|i64-array.o"
  "indexed.c|c_i64_array_forward|i64-array-forward.o"
  "indexed.c|c_i64_array_store|i64-array-store.o"
  "indexed.c|c_index_store|indexed-store.o"
  "condition.c|c_if_increment|if-increment.o"
  "condition.c|c_double_truth|double-truth.o"
  "condition.c|c_if_early_return|if-early-return.o"
  "compound.c|c_compound|compound.o"
  "compound.c|c_compound_assignments|compound-assignments.o"
  "compound.c|c_double_compound_assignment|double-compound-assignment.o"
  "compound.c|c_double_compound_for_update|double-compound-for-update.o"
  "compound.c|c_stdint_header_smoke|stdint-header-smoke.o"
  "compound.c|c_pointer_compound_assignment|pointer-compound-assignment.o"
  "compound.c|c_index_compound_assignment|index-compound-assignment.o"
  "compound.c|c_double_pointer_compound_assignment|double-pointer-compound-assignment.o"
  "compound.c|c_shadow_local|shadow-local.o"
  "typedef.c|c_typedef_sum|typedef-sum.o"
  "typedef.c|c_typedef_call|typedef-call.o"
  "typedef.c|c_typedef_add|typedef-add.o"
  "typedef.c|c_typedef_const|typedef-const.o"
  "typedef.c|c_typedef_real|typedef-real.o"
  "typedef.c|c_mode_word_size|typedef-mode-word-size.o"
  "typedef.c|c_mode_word_negative|typedef-mode-word-negative.o"
  "typedef.c|c_gnu_inline_specifier|gnu-inline-specifier.o"
)
set(objects)
foreach(case IN LISTS cases)
  string(REPLACE "|" ";" fields "${case}")
  list(GET fields 0 source)
  list(GET fields 1 symbol)
  list(GET fields 2 object)
  set(output "${BINARY_DIR}/${object}")
  nyc_execute_ok(
    [=[nyc failed for ${source}:${symbol}\n${stdout}${stderr}]=]
    COMMAND "${NYC}" "${NY_C_CORPUS_${source}}" "${output}" "${symbol}"
  )
  list(APPEND objects "${output}")
endforeach()

set(many_functions_source "")
foreach(index RANGE 0 128)
  string(APPEND many_functions_source "long c_forward_${index}();\n")
endforeach()
string(APPEND many_functions_source "long c_many_functions() { return 42; }\n")
set(many_functions_input "${NY_C_CORPUS_many-functions.c}")
set(many_functions_object "${BINARY_DIR}/many-functions.o")

nyc_execute_ok(
  [=[nyc failed for a translation unit with more than 128 declarations\n${stdout}${stderr}]=]
  COMMAND "${NYC}" "${many_functions_input}" "${many_functions_object}"
            c_many_functions
)
list(APPEND objects "${many_functions_object}")

set(unit_object "${BINARY_DIR}/translation-unit.o")
set(host_runtime_flag -rtlib=compiler-rt)
if(CC_ID STREQUAL "GNU")
  set(host_runtime_flag)
endif()
nyc_execute_ok(
  [=[nyc whole-unit compilation failed\n${stdout}${stderr}]=]
  COMMAND "${NYC}" --all "${NY_C_CORPUS_translation-unit.c}" "${unit_object}"
)
nyc_execute_ok(
  [=[linking NYR whole-unit object failed\n${stdout}${stderr}]=]
  COMMAND "${CC}" -std=c11 -O2 -Wall -Wextra -Werror
            -o "${BINARY_DIR}/translation-unit-host"
            "${NY_C_CORPUS_translation-unit-host.c}" "${unit_object}"
            ${host_runtime_flag}
)
nyc_execute_ok(
  [=[NYR whole-unit object exported functions with incorrect behavior\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/translation-unit-host"
)
if(NOT stdout STREQUAL "C stdio: 42 1.25\n")
  message(FATAL_ERROR "NYR C stdio variadic output mismatch: ${stdout}")
endif()
set(empty_variadic_source "${NY_C_CORPUS_empty-variadic.c}")

nyc_execute_ok(
  [=[NYC failed to compile an unused variadic function\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -S "${empty_variadic_source}"
            -o "${BINARY_DIR}/empty-variadic.s"
)
nyc_execute_ok(
  [=[host assembler/linker rejected empty variadic function output\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/empty-variadic.s"
            -o "${BINARY_DIR}/empty-variadic"
)
nyc_execute_ok(
  [=[empty variadic function corrupted its caller stack\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/empty-variadic"
)
set(unit_omit_fp_object "${BINARY_DIR}/translation-unit-omit-fp.o")
nyc_execute_ok(
  [=[nyc whole-unit compilation with -fomit-frame-pointer failed\n${stdout}${stderr}]=]
  COMMAND "${NYC}" --all -fomit-frame-pointer
            "${NY_C_CORPUS_translation-unit.c}" "${unit_omit_fp_object}"
)
nyc_execute_ok(
  [=[linking omitted-frame NYR whole-unit object failed\n${stdout}${stderr}]=]
  COMMAND "${CC}" -std=c11 -O2 -Wall -Wextra -Werror
            -o "${BINARY_DIR}/translation-unit-omit-fp-host"
            "${NY_C_CORPUS_translation-unit-host.c}" "${unit_omit_fp_object}"
            ${host_runtime_flag}
)
execute_process(
  COMMAND "${BINARY_DIR}/translation-unit-omit-fp-host"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0 OR NOT stdout STREQUAL "C stdio: 42 1.25\n")
  message(FATAL_ERROR
    "omitted-frame NYR whole-unit fallback behavior mismatch\n${stdout}${stderr}")
endif()

set(static_alignment_object "${BINARY_DIR}/static-alignment.o")
nyc_execute_ok(
  [=[nyc static alignment compilation failed\n${stdout}${stderr}]=]
  COMMAND "${NYC}" --all "${NY_C_CORPUS_static-alignment.c}"
            "${static_alignment_object}"
)
nyc_execute_ok(
  [=[linking static alignment regression failed\n${stdout}${stderr}]=]
  COMMAND "${CC}" -std=c11 -O2 -Wall -Wextra -Werror
            -o "${BINARY_DIR}/static-alignment-host"
            "${NY_C_CORPUS_static-alignment-host.c}" "${static_alignment_object}"
)
nyc_execute_ok(
  [=[NYR static aggregate data is misaligned (exit ${status})\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/static-alignment-host"
)

set(linkage_a_source "${NY_C_CORPUS_internal-linkage-a.c}")
set(linkage_b_source "${NY_C_CORPUS_internal-linkage-b.c}")
set(linkage_host_source "${NY_C_CORPUS_internal-linkage-host.c}")

foreach(unit IN ITEMS a b)
  nyc_execute_ok(
    [=[nyc failed to compile the ${unit} internal-linkage fixture\n${stdout}${stderr}]=]
    COMMAND "${NYC}" -c "${NY_C_CORPUS_internal-linkage-${unit}.c}"
                -o "${BINARY_DIR}/internal-linkage-${unit}.o"
  )
endforeach()
nyc_execute_ok(
  [=[host compiler could not link NYC objects with colliding internal symbols\n${stdout}${stderr}]=]
  COMMAND "${CC}" -no-pie -o "${BINARY_DIR}/internal-linkage-host"
            "${linkage_host_source}" "${BINARY_DIR}/internal-linkage-a.o"
            "${BINARY_DIR}/internal-linkage-b.o"
)
nyc_execute_ok(
  [=[NYC internal-linkage symbols crossed translation-unit boundaries\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/internal-linkage-host"
)

# Whole-translation-unit, linkage, and inline-assembly regressions.
set(global_asm_object "${BINARY_DIR}/global-asm.o")
nyc_execute_ok(
  [=[NYC file-scope asm compilation failed\n${stdout}${stderr}]=]
  COMMAND "${NYC}" --all "${NY_C_CORPUS_global-asm.c}" "${global_asm_object}"
)
nyc_execute_ok(
  [=[host compiler could not link NYC file-scope asm output\n${stdout}${stderr}]=]
  COMMAND "${CC}" -std=c11 -Wall -Wextra -Werror
            -no-pie
            -o "${BINARY_DIR}/global-asm-host"
            "${NY_C_CORPUS_global-asm-host.c}" "${global_asm_object}"
)
nyc_execute_ok(
  [=[NYC file-scope asm PC-relative data relocation was incorrect\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/global-asm-host"
)

set(global_asm_assembly "${BINARY_DIR}/global-asm.s")
set(global_asm_assembly_object "${BINARY_DIR}/global-asm-assembly.o")
nyc_execute_ok(
  [=[NYC file-scope asm assembly output failed\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -S "${NY_C_CORPUS_global-asm.c}" -o "${global_asm_assembly}"
)
nyc_execute_ok(
  [=[host assembler could not assemble NYC file-scope asm output\n${stdout}${stderr}]=]
  COMMAND "${CC}" -c -x assembler -o "${global_asm_assembly_object}"
            "${global_asm_assembly}"
)
nyc_execute_ok(
  [=[host compiler could not link NYC assembly output\n${stdout}${stderr}]=]
  COMMAND "${CC}" -std=c11 -Wall -Wextra -Werror -no-pie
            -o "${BINARY_DIR}/global-asm-assembly-host"
            "${NY_C_CORPUS_global-asm-host.c}" "${global_asm_assembly_object}"
)
nyc_execute_ok(
  [=[NYC assembly output had an incorrect signed absolute relocation\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/global-asm-assembly-host"
)

# Repeated callback declarations denote one weak object, and accesses from
# the defining unit must follow the strong override rather than a local alias.
foreach(route IN ITEMS S c)
  if(route STREQUAL "S")
    set(parameter_artifact "${BINARY_DIR}/private-parameter-constants.s")
  else()
    set(parameter_artifact "${BINARY_DIR}/private-parameter-constants.o")
  endif()
  nyc_execute_ok(
    [=[private parameter specialization failed\n${stdout}${stderr}]=]
    COMMAND "${NYC}" -O2 "-${route}"
                "${NY_C_CORPUS_private-parameter-constants.c}"
                -o "${parameter_artifact}"
  )
  nyc_execute_ok(
    [=[private parameter specialization retained a disabled feature\n${stdout}${stderr}]=]
    COMMAND "${CC}" -std=c11 -Wall -Wextra -Werror
                "${NY_C_CORPUS_private-parameter-constants-host.c}"
                "${parameter_artifact}"
                -o "${BINARY_DIR}/private-parameter-constants-${route}"
  )
  nyc_execute_ok(
    [=[private parameter specialization changed observable behavior (${status})\n${stdout}${stderr}]=]
    COMMAND "${BINARY_DIR}/private-parameter-constants-${route}"
  )
endforeach()

foreach(route IN ITEMS S c)
  if(route STREQUAL "S")
    set(callback_artifact "${BINARY_DIR}/weak-callback.s")
  else()
    set(callback_artifact "${BINARY_DIR}/weak-callback.o")
  endif()
  nyc_execute_ok(
    [=[weak callback compile failed\n${stdout}${stderr}]=]
    COMMAND "${NYC}" -O2 "-${route}"
                "${NY_C_CORPUS_weak-callback.c}" -o "${callback_artifact}"
  )
  foreach(override IN ITEMS 0 1)
    set(callback_executable "${BINARY_DIR}/weak-callback-${route}-${override}")
    nyc_execute_ok(
      [=[weak callback link failed\n${stdout}${stderr}]=]
      COMMAND "${CC}" -std=c11 -Wall -Wextra -Werror
                    "-DNYC_CALLBACK_OVERRIDE=${override}"
                    "${NY_C_CORPUS_weak-callback-host.c}"
                    "${callback_artifact}" -o "${callback_executable}"
    )
    nyc_execute_ok(
      [=[weak callback override/read/write failed\n${stdout}${stderr}]=]
      COMMAND "${callback_executable}"
    )
  endforeach()
endforeach()

# Attributes bind declarations, not references, prototype parameters, typedef
# spellings, initializers, or another declarator in the same declaration.
foreach(route IN ITEMS S c)
  if(route STREQUAL "S")
    set(binding_artifact "${BINARY_DIR}/weak-binding-scope.s")
  else()
    set(binding_artifact "${BINARY_DIR}/weak-binding-scope.o")
  endif()
  nyc_execute_ok(
    [=[weak binding scope compile failed\n${stdout}${stderr}]=]
    COMMAND "${NYC}" -O2 "-${route}"
                "${NY_C_CORPUS_weak-binding-scope.c}"
                -o "${binding_artifact}"
  )
  set(binding_executable "${BINARY_DIR}/weak-binding-scope-${route}")
  nyc_execute_ok(
    [=[weak binding scope link failed\n${stdout}${stderr}]=]
    COMMAND "${CC}" -std=c11 -Wall -Wextra -Werror
                "${NY_C_CORPUS_weak-binding-scope-host.c}"
                "${binding_artifact}" -o "${binding_executable}"
  )
  nyc_execute_ok(
    [=[incorrect declaration binding scope (${status})\n${stdout}${stderr}]=]
    COMMAND "${binding_executable}"
  )
endforeach()

# UD2 in opaque assembly may be resumed by a signal or kernel trap handler.
# Only C noreturn/unreachable semantics may terminate the continuation.
foreach(route IN ITEMS S c)
  if(route STREQUAL "S")
    set(trap_artifact "${BINARY_DIR}/resumable-asm.s")
    set(trap_source "${NY_C_CORPUS_resumable-asm.c}")
    set(trap_host_options)
  else()
    set(trap_artifact "${BINARY_DIR}/resumable-asm.o")
    set(trap_source "${NY_C_CORPUS_resumable-asm-simple.c}")
    set(trap_host_options -DNYC_SIMPLE_TRAPS=1)
  endif()
  nyc_execute_ok(
    [=[resumable asm compile failed\n${stdout}${stderr}]=]
    COMMAND "${NYC}" -O2 "-${route}"
                "${trap_source}" -o "${trap_artifact}"
  )
  set(trap_executable "${BINARY_DIR}/resumable-asm-${route}")
  nyc_execute_ok(
    [=[resumable asm link failed\n${stdout}${stderr}]=]
    COMMAND "${CC}" -std=c11 -Wall -Wextra -Werror
                ${trap_host_options}
                "${NY_C_CORPUS_resumable-asm-host.c}"
                "${trap_artifact}" -o "${trap_executable}"
  )
  nyc_execute_ok(
    [=[asm trap lost its continuation (${status})\n${stdout}${stderr}]=]
    COMMAND "${trap_executable}"
        TIMEOUT 10
  )
endforeach()

# Section-bearing raw assembly remains unsupported by the direct encoder:
# reject it rather than silently dropping its annotation data.
execute_process(
  COMMAND "${NYC}" -O2 -c "${NY_C_CORPUS_resumable-asm.c}"
          -o "${BINARY_DIR}/resumable-asm-annotated.o"
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(status EQUAL 0)
  message(FATAL_ERROR "direct object silently accepted unsupported section-bearing asm")
endif()

set(weak_function_source "${NY_C_CORPUS_weak-function.c}")
set(strong_function_source "${NY_C_CORPUS_strong-function.c}")
foreach(suffix IN ITEMS s o)
  set(route -c)
  if(suffix STREQUAL "s")
    set(route -S)
  endif()
  nyc_execute_ok(
    [=[NYC weak function compilation failed (${route})]=]
    COMMAND "${NYC}" "${route}" -O3 "${weak_function_source}"
              -o "${BINARY_DIR}/weak-function.${suffix}"
  )
  nyc_execute_ok(
    [=[NYC strong function compilation failed (${route})]=]
    COMMAND "${NYC}" "${route}" -O3 "${strong_function_source}"
              -o "${BINARY_DIR}/strong-function.${suffix}"
  )
  nyc_execute_ok(
    [=[host compiler could not link strong-over-weak NYC symbols (${route})]=]
    COMMAND "${CC}" -std=c11 -Wall -Wextra -Werror
              -o "${BINARY_DIR}/weak-function-override-${suffix}"
              "${NY_C_CORPUS_weak-function-main.c}"
              "${BINARY_DIR}/weak-function.${suffix}"
              "${BINARY_DIR}/strong-function.${suffix}"
  )
  nyc_execute_ok(
    [=[strong definitions did not override NYC weak symbols (${route})]=]
    COMMAND "${BINARY_DIR}/weak-function-override-${suffix}"
  )
  nyc_execute_ok(
    [=[host compiler could not link NYC weak alias target (${route})]=]
    COMMAND "${CC}" -std=c11 -Wall -Wextra -Werror
              -o "${BINARY_DIR}/weak-function-alias-${suffix}"
              "${NY_C_CORPUS_weak-function-alias-main.c}"
              "${BINARY_DIR}/weak-function.${suffix}"
  )
  nyc_execute_ok(
    [=[NYC weak alias did not bind to its target (${route})]=]
    COMMAND "${BINARY_DIR}/weak-function-alias-${suffix}"
  )
endforeach()

foreach(source compound-array-a compound-array-b)
  nyc_execute_ok(
    [=[NYC compound-literal translation-unit compilation failed\n${stdout}${stderr}]=]
    COMMAND "${NYC}" -S "${NY_C_CORPUS_${source}.c}"
                -o "${BINARY_DIR}/${source}.s"
  )
endforeach()
nyc_execute_ok(
  [=[host compiler could not link NYC compound literals from separate units\n${stdout}${stderr}]=]
  COMMAND "${CC}" -std=c11 -Wall -Wextra -Werror
            -o "${BINARY_DIR}/compound-array-link"
            "${NY_C_CORPUS_compound-array-main.c}"
            "${BINARY_DIR}/compound-array-a.s"
            "${BINARY_DIR}/compound-array-b.s"
)
nyc_execute_ok(
  [=[NYC compound-literal data collided or had incorrect contents across units\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/compound-array-link"
)

set(inline_asm_object "${BINARY_DIR}/inline-asm.o")
nyc_execute_ok(
  [=[NYC operand-free inline asm compilation failed\n${stdout}${stderr}]=]
  COMMAND "${NYC}" --all "${NY_C_CORPUS_inline-asm.c}" "${inline_asm_object}"
)
nyc_execute_ok(
  [=[host compiler could not link NYC inline asm output\n${stdout}${stderr}]=]
  COMMAND "${CC}" -std=c11 -Wall -Wextra -Werror
            -o "${BINARY_DIR}/inline-asm-host"
            "${NY_C_CORPUS_inline-asm-host.c}" "${inline_asm_object}"
)
nyc_execute_ok(
  [=[NYC inline asm host smoke failed\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/inline-asm-host"
)
if(OBJDUMP)
  execute_process(
    COMMAND "${OBJDUMP}" -d "${inline_asm_object}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0 OR NOT stdout MATCHES "f3[ \t]+90")
    message(FATAL_ERROR
      "NYC pause inline asm was not encoded as PAUSE\n${stdout}${stderr}")
  endif()
endif()

set(alternative_wrmsr_source
  "${NY_C_CORPUS_linux-alternative-wrmsr.c}")
nyc_execute_ok(
  [=[NYC failed to specialize the Linux alternative WRMSR helper\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -S "${alternative_wrmsr_source}"
            -o "${BINARY_DIR}/linux-alternative-wrmsr.s"
)
file(READ "${BINARY_DIR}/linux-alternative-wrmsr.s" alternative_wrmsr_asm)
if(NOT alternative_wrmsr_asm MATCHES "\\.altinstructions" OR
   NOT alternative_wrmsr_asm MATCHES "\\.altinstr_replacement" OR
   NOT alternative_wrmsr_asm MATCHES "\\.4byte[ \t]+246" OR
   NOT alternative_wrmsr_asm MATCHES "wrmsr")
  message(FATAL_ERROR
    "NYC output omitted alternative WRMSR metadata or replacement code")
endif()
if(NYC_AS_EXECUTABLE)
  nyc_execute_ok(
    [=[GNU as rejected Linux alternative WRMSR output\n${stdout}${stderr}]=]
    COMMAND "${NYC_AS_EXECUTABLE}" --64
                "${BINARY_DIR}/linux-alternative-wrmsr.s"
                -o "${BINARY_DIR}/linux-alternative-wrmsr.o"
  )
endif()

set(gs_percpu_load_source "${NY_C_CORPUS_linux-gs-percpu-load.c}")
nyc_execute_ok(
  [=[NYC failed to lower Linux GS-relative per-CPU loading\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -S "${gs_percpu_load_source}"
            -o "${BINARY_DIR}/linux-gs-percpu-load.s"
)
file(READ "${BINARY_DIR}/linux-gs-percpu-load.s" gs_percpu_load_asm)
if(NOT gs_percpu_load_asm MATCHES "movl[ \t]+%gs:\\(%rax\\), %eax")
  message(FATAL_ERROR
    "NYC output omitted the GS-relative 32-bit load")
endif()
if(NYC_AS_EXECUTABLE)
  nyc_execute_ok(
    [=[GNU as rejected Linux GS-relative load output\n${stdout}${stderr}]=]
    COMMAND "${NYC_AS_EXECUTABLE}" --64
                "${BINARY_DIR}/linux-gs-percpu-load.s"
                -o "${BINARY_DIR}/linux-gs-percpu-load.o"
  )
endif()

set(data_segment_reload_source
  "${NY_C_CORPUS_linux-data-segment-reload.c}")
nyc_execute_ok(
  [=[NYC failed to lower Linux data-segment reload asm\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -S "${data_segment_reload_source}"
            -o "${BINARY_DIR}/linux-data-segment-reload.s"
)
if(NYC_AS_EXECUTABLE)
  nyc_execute_ok(
    [=[GNU as rejected Linux data-segment reload output\n${stdout}${stderr}]=]
    COMMAND "${NYC_AS_EXECUTABLE}" --64
                "${BINARY_DIR}/linux-data-segment-reload.s"
                -o "${BINARY_DIR}/linux-data-segment-reload.o"
  )
  if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    nyc_execute_ok(
      [=[host compiler could not link data-segment reload test\n${stdout}${stderr}]=]
      COMMAND "${CC}" "${NY_C_CORPUS_linux-data-segment-reload-host.c}"
                    "${BINARY_DIR}/linux-data-segment-reload.o"
                    -o "${BINARY_DIR}/linux-data-segment-reload"
    )
    nyc_execute_ok(
      [=[data-segment reload produced incorrect segment state\n${stdout}${stderr}]=]
      COMMAND "${BINARY_DIR}/linux-data-segment-reload"
    )
  endif()
endif()

set(static_local_section_source
  "${NY_C_CORPUS_static-local-section-attribute.c}")
nyc_execute_ok(
  [=[NYC failed to preserve a static local section attribute\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -S "${static_local_section_source}"
            -o "${BINARY_DIR}/static-local-section-attribute.s"
)
file(READ "${BINARY_DIR}/static-local-section-attribute.s"
     static_local_section_asm)
if(NOT static_local_section_asm MATCHES
   "\\.section[ \t]+\\\"\\.discard.addressable\\\"" OR
   NOT static_local_section_asm MATCHES
   "\\.quad[ \t]+nyc_static_local_section_target")
  message(FATAL_ERROR
    "NYC omitted the static local's discard section or target relocation")
endif()
if(NYC_AS_EXECUTABLE)
  nyc_execute_ok(
    [=[GNU as rejected static local section output\n${stdout}${stderr}]=]
    COMMAND "${NYC_AS_EXECUTABLE}" --64
                "${BINARY_DIR}/static-local-section-attribute.s"
                -o "${BINARY_DIR}/static-local-section-attribute.o"
  )
endif()

set(section_no_leak_source
  "${NY_C_CORPUS_section-does-not-leak.c}")
nyc_execute_ok(
  [=[NYC failed to compile section non-leak fixture\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -c "${section_no_leak_source}"
            -o "${BINARY_DIR}/section-does-not-leak.o"
)
if(OBJDUMP)
  execute_process(
    COMMAND "${OBJDUMP}" -t "${BINARY_DIR}/section-does-not-leak.o"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0 OR
     NOT stdout MATCHES
       "\\.discard\\.addressable[ \t]+[0-9]+[ \t]+addressable_anchor" OR
     NOT stdout MATCHES
       "\\.rodata[ \t]+[0-9]+[ \t]+section_following_array")
    message(FATAL_ERROR
      "section attribute leaked to a later global array\n${stdout}${stderr}")
  endif()
endif()

set(unused_inline_table_source
  "${NY_C_CORPUS_unused-inline-static-pointer-table.c}")
nyc_execute_ok(
  [=[NYC failed to compile static pointer-table liveness fixture\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -S "${unused_inline_table_source}"
            -o "${BINARY_DIR}/unused-inline-static-pointer-table.s"
)
file(READ "${BINARY_DIR}/unused-inline-static-pointer-table.s"
     unused_inline_table_asm)
string(REGEX MATCHALL "\\.local[ \t]+__nyc_static_ptrs_[0-9]+"
       unused_inline_table_symbols "${unused_inline_table_asm}")
list(LENGTH unused_inline_table_symbols unused_inline_table_count)
if(NOT unused_inline_table_count EQUAL 1)
  message(FATAL_ERROR
    "NYC should emit exactly one live static pointer table and its alias")
endif()

set(nested_static_table_source
  "${NY_C_CORPUS_nested-static-pointer-table.c}")
nyc_execute_ok(
  [=[NYC failed to compile nested static pointer-table fixture\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -S "${nested_static_table_source}"
            -o "${BINARY_DIR}/nested-static-pointer-table.s"
)
file(READ "${BINARY_DIR}/nested-static-pointer-table.s"
     nested_static_table_asm)
string(REGEX MATCHALL "\\.local[ \t]+__nyc_static_ptrs_[0-9]+"
       nested_static_table_symbols "${nested_static_table_asm}")
list(LENGTH nested_static_table_symbols nested_static_table_count)
if(NOT nested_static_table_count EQUAL 1 OR
   NOT nested_static_table_asm MATCHES
     "\\.quad[ \t]+__nyc_static_ptrs_[0-9]+")
  message(FATAL_ERROR
    "NYC dropped a static pointer table referenced by another table\n${nested_static_table_asm}")
endif()

set(inline_asm_immediate_source "${NY_C_CORPUS_inline-asm-immediate.c}")
get_filename_component(inline_asm_immediate_source_path
  "${inline_asm_immediate_source}" REALPATH)
file(TO_CMAKE_PATH "${inline_asm_immediate_source_path}"
  inline_asm_immediate_source_path)

nyc_execute_ok(
  [=[nyc failed to substitute an immediate-only inline-asm operand\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -S "${inline_asm_immediate_source}"
            -o "${BINARY_DIR}/inline-asm-immediate.s"
)
nyc_execute_ok(
  [=[host compiler could not link inline-asm immediate test\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/inline-asm-immediate.s" -o
            "${BINARY_DIR}/inline-asm-immediate"
)
nyc_execute_ok(
  [=[inline-asm immediate expansion returned incorrect data\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/inline-asm-immediate"
)

set(inline_asm_memory_source "${NY_C_CORPUS_inline-asm-memory.c}")

nyc_execute_ok(
  [=[nyc failed to lower a named read-write memory asm operand\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -S "${inline_asm_memory_source}"
            -o "${BINARY_DIR}/inline-asm-memory.s"
)
nyc_execute_ok(
  [=[host compiler could not link inline-asm memory test\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/inline-asm-memory.s" -o
            "${BINARY_DIR}/inline-asm-memory"
)
nyc_execute_ok(
  [=[inline-asm memory operation returned incorrect data\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/inline-asm-memory"
)

set(typedef_callback_source "${NY_C_CORPUS_typedef-callback-field.c}")

nyc_execute_ok(
  [=[nyc failed to call a typedef function-pointer aggregate field\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -S "${typedef_callback_source}"
            -o "${BINARY_DIR}/typedef-callback-field.s"
)
nyc_execute_ok(
  [=[host compiler could not link typedef callback field test\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/typedef-callback-field.s" -o
            "${BINARY_DIR}/typedef-callback-field"
)
nyc_execute_ok(
  [=[typedef callback field invoked with incorrect result\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/typedef-callback-field"
)

set(function_parameter_adjustment_source
    "${NY_C_CORPUS_function-parameter-adjustment.c}")

nyc_execute_ok(
  [=[nyc failed to adjust a function-typed parameter to a pointer\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -S "${function_parameter_adjustment_source}"
            -o "${BINARY_DIR}/function-parameter-adjustment.s"
)
nyc_execute_ok(
  [=[host compiler could not link function parameter adjustment test\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/function-parameter-adjustment.s" -o
            "${BINARY_DIR}/function-parameter-adjustment"
)
nyc_execute_ok(
  [=[function-typed parameter was not invoked correctly\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/function-parameter-adjustment"
)

set(static_array_member_pointer_source
    "${NY_C_CORPUS_static-array-member-pointer.c}")

nyc_execute_ok(
  [=[nyc failed to initialize a pointer from a static array member\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -S "${static_array_member_pointer_source}"
            -o "${BINARY_DIR}/static-array-member-pointer.s"
)
nyc_execute_ok(
  [=[host compiler could not link static array-member pointer test\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/static-array-member-pointer.s" -o
            "${BINARY_DIR}/static-array-member-pointer"
)
nyc_execute_ok(
  [=[static array-member pointer initializer targeted the wrong address\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/static-array-member-pointer"
)

set(multidimensional_pointer_source
    "${NY_C_CORPUS_multidimensional-pointer-index.c}")

nyc_execute_ok(
  [=[nyc failed to index through a multidimensional array pointer\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -S "${multidimensional_pointer_source}"
            -o "${BINARY_DIR}/multidimensional-pointer-index.s"
)
set(multidimensional_pointer_main
    "${NY_C_CORPUS_multidimensional-pointer-index-main.c}")

nyc_execute_ok(
  [=[host compiler could not link multidimensional array pointer test\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/multidimensional-pointer-index.s"
            "${multidimensional_pointer_main}" -o
            "${BINARY_DIR}/multidimensional-pointer-index"
)
nyc_execute_ok(
  [=[multidimensional array pointer index returned an incorrect value\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/multidimensional-pointer-index"
)

set(global_pointer_array_address_source
    "${NY_C_CORPUS_global-pointer-array-address.c}")

nyc_execute_ok(
  [=[nyc failed to take the address of a global pointer-array row\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -S "${global_pointer_array_address_source}"
            -o "${BINARY_DIR}/global-pointer-array-address.s"
)
set(global_pointer_array_address_main
    "${NY_C_CORPUS_global-pointer-array-address-main.c}")

nyc_execute_ok(
  [=[host compiler could not link global pointer-array address regression\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/global-pointer-array-address.s"
            "${global_pointer_array_address_main}" -o
            "${BINARY_DIR}/global-pointer-array-address"
)
nyc_execute_ok(
  [=[global pointer-array row address did not match its source location\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/global-pointer-array-address"
)

set(file_scope_asm_source "${NY_C_CORPUS_file-scope-asm-ret.c}")

nyc_execute_ok(
  [=[nyc failed to emit supported file-scope x86 instructions\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -S "${file_scope_asm_source}"
            -o "${BINARY_DIR}/file-scope-asm-ret.s"
)
set(file_scope_asm_main "${NY_C_CORPUS_file-scope-asm-ret-main.c}")

nyc_execute_ok(
  [=[host compiler could not link file-scope assembly test\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/file-scope-asm-ret.s"
            "${file_scope_asm_main}" -o "${BINARY_DIR}/file-scope-asm-ret"
)
nyc_execute_ok(
  [=[file-scope assembly return shim failed\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/file-scope-asm-ret"
)

set(pointer_to_byte_init_source "${NY_C_CORPUS_pointer-to-byte-init.c}")

execute_process(
  COMMAND "${NYC}" -S "${pointer_to_byte_init_source}"
          -o "${BINARY_DIR}/pointer-to-byte-init.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(status EQUAL 0)
  message(FATAL_ERROR
    "nyc accepted a pointer-to-unsigned-char initializer")
endif()

set(preprocessor_object "${BINARY_DIR}/preprocessor.o")
nyc_execute_ok(
  [=[nyc conditional preprocessing failed\n${stdout}${stderr}]=]
  COMMAND "${NYC}" --all "${NY_C_CORPUS_preprocessor.c}" "${preprocessor_object}"
)
set(preprocessor_host "${BINARY_DIR}/preprocessor-host")
nyc_execute_ok(
  [=[linking conditional preprocessing object failed\n${stdout}${stderr}]=]
  COMMAND "${CC}" -std=c11 -O2 -Wall -Wextra -Werror
            -o "${preprocessor_host}" "${NY_C_CORPUS_preprocessor-host.c}"
            "${preprocessor_object}"
)
nyc_execute_ok(
  [=[conditional preprocessing selected the wrong branches\n${stdout}${stderr}]=]
  COMMAND "${preprocessor_host}"
)

nyc_execute_ok(
  [=[nyc failed to compile the bool-argument-normalization fixture (${status})\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 --all "${NY_C_CORPUS_bool-argument-normalization.c}"
            "${BINARY_DIR}/bool-argument-normalization.o"
)
nyc_execute_ok(
  [=[linking the bool-argument-normalization fixture failed\n${stdout}${stderr}]=]
  COMMAND "${CC}" -o "${BINARY_DIR}/bool-argument-normalization-host"
            "${NY_C_CORPUS_bool-argument-normalization-main.c}"
            "${BINARY_DIR}/bool-argument-normalization.o"
)
nyc_execute_ok(
  [=[a non-zero integer converted to _Bool lost its value\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/bool-argument-normalization-host"
)
foreach(bool_store_opt -O0 -O2)
  nyc_execute_ok(
    [=[nyc failed to compile the bool-store-paths fixture (${status})\n${stdout}${stderr}]=]
    COMMAND "${NYC}" ${bool_store_opt} --all "${NY_C_CORPUS_bool-store-paths.c}"
              "${BINARY_DIR}/bool-store-paths.o"
  )
  nyc_execute_ok(
    [=[linking the bool-store-paths fixture failed\n${stdout}${stderr}]=]
    COMMAND "${CC}" -o "${BINARY_DIR}/bool-store-paths-host"
              "${NY_C_CORPUS_bool-store-paths-main.c}"
              "${BINARY_DIR}/bool-store-paths.o"
  )
  nyc_execute_ok(
    [=[a non-zero integer stored to a _Bool kept only its low byte (${bool_store_opt})\n${stdout}${stderr}]=]
    COMMAND "${BINARY_DIR}/bool-store-paths-host"
  )
endforeach()
nyc_execute_ok(
  [=[nyc failed to compile the noreturn-no-dead-code fixture (${status})\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -S "${NY_C_CORPUS_noreturn-no-dead-code.c}"
            -o "${BINARY_DIR}/noreturn-no-dead-code.s"
)
file(READ "${BINARY_DIR}/noreturn-no-dead-code.s" noreturn_dead_asm)
if(noreturn_dead_asm MATCHES "call[^\n]*\tdie\n[^\n]*(movq|ud2)")
  message(FATAL_ERROR
    "an instruction follows a noreturn call (objtool reports it as unreachable)\n${noreturn_dead_asm}")
endif()
if(noreturn_dead_asm MATCHES "ud2 # noreturn")
  message(FATAL_ERROR "the noreturn marker leaked into the assembly\n${noreturn_dead_asm}")
endif()
if(noreturn_dead_asm MATCHES "\n[ \t]*ud2\n[ \t]*(jmp|movq|call)")
  message(FATAL_ERROR "code follows __builtin_unreachable\n${noreturn_dead_asm}")
endif()
if(noreturn_dead_asm MATCHES "\ntripled:")
  message(FATAL_ERROR "an unreferenced static always_inline function was emitted\n${noreturn_dead_asm}")
endif()
foreach(inline_array_opt -O0 -O2)
  nyc_execute_ok(
    [=[nyc failed to compile the always-inline-array fixture (${status})\n${stdout}${stderr}]=]
    COMMAND "${NYC}" ${inline_array_opt} --all "${NY_C_CORPUS_always-inline-array.c}"
              "${BINARY_DIR}/always-inline-array.o"
  )
  nyc_execute_ok(
    [=[linking the always-inline-array fixture failed\n${stdout}${stderr}]=]
    COMMAND "${CC}" -o "${BINARY_DIR}/always-inline-array-host"
              "${NY_C_CORPUS_always-inline-array-main.c}"
              "${BINARY_DIR}/always-inline-array.o"
  )
  nyc_execute_ok(
    [=[an always_inline callee with automatic arrays computed the wrong result (${inline_array_opt})\n${stdout}${stderr}]=]
    COMMAND "${BINARY_DIR}/always-inline-array-host"
  )
endforeach()
nyc_execute_ok(
  [=[nyc failed to assemble the always-inline-array fixture (${status})\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -S "${NY_C_CORPUS_always-inline-array.c}"
            -o "${BINARY_DIR}/always-inline-array.s"
)
file(READ "${BINARY_DIR}/always-inline-array.s" always_inline_array_asm)
if(always_inline_array_asm MATCHES "call[^\n]*\tsum_to" OR always_inline_array_asm MATCHES "\nsum_to:")
  message(FATAL_ERROR "an always_inline callee with automatic arrays was not inlined\n${always_inline_array_asm}")
endif()
nyc_execute_ok(
  [=[nyc failed to compile the indirect-call-folding fixture (${status})\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 --all "${NY_C_CORPUS_indirect-call-folding.c}"
            "${BINARY_DIR}/indirect-call-folding.o"
)
nyc_execute_ok(
  [=[linking the indirect-call-folding fixture failed\n${stdout}${stderr}]=]
  COMMAND "${CC}" -o "${BINARY_DIR}/indirect-call-folding-host"
            "${NY_C_CORPUS_indirect-call-folding-main.c}"
            "${BINARY_DIR}/indirect-call-folding.o"
)
nyc_execute_ok(
  [=[a folded indirect call computed the wrong result\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/indirect-call-folding-host"
)
nyc_execute_ok(
  [=[nyc failed to assemble the indirect-call-folding fixture (${status})\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -S "${NY_C_CORPUS_indirect-call-folding.c}"
            -o "${BINARY_DIR}/indirect-call-folding.s"
)
file(READ "${BINARY_DIR}/indirect-call-folding.s" folded_call_asm)
if(folded_call_asm MATCHES "call[ \t]+\\*" OR folded_call_asm MATCHES "indirect_thunk")
  message(FATAL_ERROR "a call through a function symbol stayed indirect\n${folded_call_asm}")
endif()
nyc_execute_ok(
  [=[nyc failed to compile the zero-length-static fixture (${status})\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -S "${NY_C_CORPUS_zero-length-static.c}"
            -o "${BINARY_DIR}/zero-length-static.s"
)
file(READ "${BINARY_DIR}/zero-length-static.s" zero_length_asm)
if(NOT zero_length_asm MATCHES "\\.bss\n[^\n]*\n[^\n]*\n[^\n]*\n[^\n]*zero_length_marker:" OR
   zero_length_asm MATCHES "\\.data\n[^\n]*\n[^\n]*\n[^\n]*\n[^\n]*zero_length_marker:")
  message(FATAL_ERROR
    "a zero-length local object belongs in .bss, not at the end of .data\n${zero_length_asm}")
endif()
foreach(implied_opt -O1 -O2)
  nyc_execute_ok(
    [=[nyc failed to compile the jump-thread-implied-branch fixture (${status})\n${stdout}${stderr}]=]
    COMMAND "${NYC}" ${implied_opt} --all "${NY_C_CORPUS_jump-thread-implied-branch.c}"
              "${BINARY_DIR}/jump-thread-implied-branch.o"
  )
  nyc_execute_ok(
    [=[linking the jump-thread-implied-branch fixture failed\n${stdout}${stderr}]=]
    COMMAND "${CC}" -o "${BINARY_DIR}/jump-thread-implied-branch-host"
              "${NY_C_CORPUS_jump-thread-implied-branch-main.c}"
              "${BINARY_DIR}/jump-thread-implied-branch.o"
  )
  nyc_execute_ok(
    [=[a branch decided by a dominating test returned the wrong value (${implied_opt})\n${stdout}${stderr}]=]
    COMMAND "${BINARY_DIR}/jump-thread-implied-branch-host"
  )
endforeach()
nyc_execute_ok(
  [=[nyc failed to assemble the jump-thread-implied-branch fixture (${status})\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -S "${NY_C_CORPUS_jump-thread-implied-branch.c}"
            -o "${BINARY_DIR}/jump-thread-implied-branch.s"
)
file(READ "${BINARY_DIR}/jump-thread-implied-branch.s" implied_branch_asm)
if(NOT implied_branch_asm MATCHES "\n\tnop\n\tjmp\t[^\n]*\n")
  message(FATAL_ERROR
    "the entered path was not threaded past the repeated test\n${implied_branch_asm}")
endif()
nyc_execute_ok(
  [=[nyc failed to compile the global byte array alignment fixture (${status})\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -S "${NY_C_CORPUS_global-byte-array-alignment.c}"
            -o "${BINARY_DIR}/global-byte-array-alignment.s"
)
file(READ "${BINARY_DIR}/global-byte-array-alignment.s" byte_array_alignment_asm)
if(NOT byte_array_alignment_asm MATCHES "balign 4096[^:]*nyc_zero_page:" OR
   NOT byte_array_alignment_asm MATCHES "balign 16[^:]*nyc_small:")
  message(FATAL_ERROR
    "file-scope byte arrays lost their aligned attribute\n${byte_array_alignment_asm}")
endif()
nyc_execute_ok(
  [=[nyc failed to compile the typeof-literal-width fixture (${status})\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 --all "${NY_C_CORPUS_typeof-literal-width.c}"
            "${BINARY_DIR}/typeof-literal-width.o"
)
nyc_execute_ok(
  [=[linking the typeof-literal-width fixture failed\n${stdout}${stderr}]=]
  COMMAND "${CC}" -o "${BINARY_DIR}/typeof-literal-width-host"
            "${NY_C_CORPUS_typeof-literal-width-main.c}"
            "${BINARY_DIR}/typeof-literal-width.o"
)
nyc_execute_ok(
  [=[typeof of a long literal lost its upper 32 bits\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/typeof-literal-width-host"
)
nyc_execute_ok(
  [=[nyc failed to compile the inline-asm descriptor store fixture (${status})\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -S "${NY_C_CORPUS_inline-asm-descriptor-store.c}"
            -o "${BINARY_DIR}/inline-asm-descriptor-store.s"
)
file(READ "${BINARY_DIR}/inline-asm-descriptor-store.s" descriptor_store_asm)
if(NOT descriptor_store_asm MATCHES "movb.*movb.*lgdt")
  message(FATAL_ERROR
    "stores feeding an lgdt memory operand were eliminated\n${descriptor_store_asm}")
endif()
nyc_execute_ok(
  [=[nyc failed to compile the inline-loop-entry fixture (${status})\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 --all "${NY_C_CORPUS_inline-loop-entry.c}"
            "${BINARY_DIR}/inline-loop-entry.o"
)
nyc_execute_ok(
  [=[linking the inline-loop-entry fixture failed\n${stdout}${stderr}]=]
  COMMAND "${CC}" -o "${BINARY_DIR}/inline-loop-entry-host"
            "${NY_C_CORPUS_inline-loop-entry-main.c}"
            "${BINARY_DIR}/inline-loop-entry.o"
)
nyc_execute_ok(
  [=[an inlined callee that starts with a loop reset its formals\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/inline-loop-entry-host"
)
nyc_execute_ok(
  [=[nyc failed to compile mixed arithmetic conditionals (${status})\n${stdout}${stderr}]=]
  COMMAND "${NYC}" --all "${NY_C_CORPUS_mixed-arithmetic-conditional.c}"
            "${BINARY_DIR}/mixed-arithmetic-conditional.o"
)
nyc_execute_ok(
  [=[linking mixed arithmetic conditionals failed\n${stdout}${stderr}]=]
  COMMAND "${CC}" -o "${BINARY_DIR}/mixed-arithmetic-conditional-host"
            "${NY_C_CORPUS_mixed-arithmetic-conditional-main.c}"
            "${BINARY_DIR}/mixed-arithmetic-conditional.o"
)
nyc_execute_ok(
  [=[mixed arithmetic conditionals computed the wrong value\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/mixed-arithmetic-conditional-host"
)

nyc_execute_ok(
  [=[nyc failed to compile a conditional preprocessor expression (${status})\n${stdout}${stderr}]=]
  COMMAND "${NYC}" --all "${NY_C_CORPUS_conditional-expression.c}"
            "${BINARY_DIR}/conditional-expression.o"
)

nyc_execute_ok(
  [=[linking conditional-expression object failed\n${stdout}${stderr}]=]
  COMMAND "${CC}" -o "${BINARY_DIR}/conditional-expression-host"
            "${NY_C_CORPUS_conditional-expression-main.c}"
            "${BINARY_DIR}/conditional-expression.o"
)
nyc_execute_ok(
  [=[conditional expression selected the wrong preprocessor branch\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/conditional-expression-host"
)

nyc_execute_ok(
  [=[building C reference host failed\n${stdout}${stderr}]=]
  COMMAND "${CC}" -std=c11 -O2 -Wall -Wextra -Werror
            -o "${BINARY_DIR}/host"
            "${NY_C_CORPUS_host.c}" "${NY_C_CORPUS_reference.c}" ${objects}
)

nyc_execute_ok(
  [=[C reference comparison failed\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/host"
)
message(STATUS "${stdout}")

# Object-format acceptance uses external LLVM lld drivers only as optional
# linker dependencies. The public `nyc` API remains linker-independent.
# Cross-target ABI checks and unsupported-input diagnostics.
set(target_source "${NY_C_CORPUS_target-link.c}")
set(aarch64_object "${BINARY_DIR}/target-link-aarch64.o")
nyc_execute_output("nyc AArch64 ELF emission failed" "${aarch64_object}"
  COMMAND "${NYC}" --target=aarch64-unknown-linux-gnu "${target_source}"
          "${aarch64_object}" c_target_link
)
file(READ "${aarch64_object}" aarch64_header HEX LIMIT 20)
string(SUBSTRING "${aarch64_header}" 36 4 aarch64_machine)
if(NOT aarch64_machine STREQUAL "b700")
  message(FATAL_ERROR
    "nyc AArch64 output has wrong ELF machine id: ${aarch64_machine}")
endif()

# Exercise the target-aware AAPCS64 aggregate classifier and lowering. Compile
# each exported symbol separately, then link the objects against a native C
# caller. When a cross runner is installed, also check the actual register and
# stack conventions at runtime.
set(aarch64_aggregate_source "${NY_C_CORPUS_aarch64-aggregates.c}")
set(aarch64_aggregate_objects "")
foreach(symbol IN ITEMS c_a64_pair_scale c_a64_triple_shift
                        c_a64_quad_scale c_a64_call_quad_scale
                        c_a64_call_five_identity
                        c_a64_point_add c_a64_big_shift
                        c_a64_call_pair_scale c_a64_call_triple_shift
                        c_a64_call_point_add c_a64_call_big_shift
                        c_a64_union_identity c_a64_call_union_identity
                        c_a64_call_mixed_union_identity)
  set(object "${BINARY_DIR}/${symbol}-aarch64.o")
  nyc_execute_output("nyc AArch64 aggregate compilation failed for ${symbol}" "${object}"
    COMMAND "${NYC}" --target=aarch64-unknown-linux-gnu
            "${aarch64_aggregate_source}" "${object}" "${symbol}"
  )
  list(APPEND aarch64_aggregate_objects "${object}")
endforeach()

find_program(AARCH64_CC NAMES aarch64-linux-gnu-gcc)
find_program(AARCH64_QEMU NAMES qemu-aarch64)
if(AARCH64_CC)
  set(aarch64_aggregate_exe "${BINARY_DIR}/aarch64-aggregate-host")
  nyc_execute_output("AArch64 C aggregate link failed" "${aarch64_aggregate_exe}"
    COMMAND "${AARCH64_CC}" -std=c11 -O2 -Wall -Wextra -Werror
            -o "${aarch64_aggregate_exe}"
            "${NY_C_CORPUS_aarch64-aggregate-host.c}"
            ${aarch64_aggregate_objects}
  )
  if(AARCH64_QEMU)
    execute_process(COMMAND "${AARCH64_CC}" -print-sysroot
                    OUTPUT_VARIABLE aarch64_sysroot
                    OUTPUT_STRIP_TRAILING_WHITESPACE)
    nyc_execute_ok(
      [=[AArch64 C aggregate runtime parity failed\n${stdout}${stderr}]=]
      COMMAND "${AARCH64_QEMU}" -L "${aarch64_sysroot}"
                    "${aarch64_aggregate_exe}"
    )
    message(STATUS "${stdout}")
  else()
    message(STATUS "AArch64 C aggregate runtime skipped: qemu-aarch64 was not found")
  endif()
else()
  message(STATUS "AArch64 C aggregate link/runtime skipped: aarch64-linux-gnu-gcc was not found")
endif()

# Targets without a matching classifier must continue to reject aggregate
# signatures instead of reusing a superficially similar ABI.
foreach(triple IN ITEMS aarch64-pc-windows-msvc)
  string(REPLACE "-" "_" target_key "${triple}")
  set(unsupported_aggregate_object
      "${BINARY_DIR}/aggregate-${target_key}.o")
  execute_process(
    COMMAND "${NYC}" "--target=${triple}"
            "${NY_C_CORPUS_structs.c}" "${unsupported_aggregate_object}"
            c_point_sum
    RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr
  )
  if(status EQUAL 0 OR NOT stderr MATCHES "aggregate.*not supported")
    message(FATAL_ERROR
      "nyc must fail closed for ${triple} aggregate ABI\n${stdout}${stderr}")
  endif()
endforeach()

# Win64 uses positional GP/XMM registers and indirect by-value arguments for
# aggregates larger than eight bytes. Exercise direct returns, sret returns,
# register arguments, and arguments after the four positional slots.
set(win64_aggregate_source "${NY_C_CORPUS_win64-aggregates.c}")
set(win64_aggregate_objects "")
foreach(symbol IN ITEMS c_win_word_add c_win_point_add c_win_big_make
                        c_win_big_sum c_win_big_sum_after4
                        c_win_many_with_point)
  set(object "${BINARY_DIR}/${symbol}-win64.obj")
  nyc_execute_output("nyc Win64 aggregate compilation failed for ${symbol}" "${object}"
    COMMAND "${NYC}" --target=x86_64-pc-windows-msvc
            "${win64_aggregate_source}" "${object}" "${symbol}"
  )
  list(APPEND win64_aggregate_objects "${object}")
endforeach()

find_program(WIN64_CC NAMES x86_64-w64-mingw32-gcc)
find_program(WIN64_RUNNER NAMES wine wine64)
if(WIN64_CC)
  set(win64_aggregate_exe "${BINARY_DIR}/win64-aggregate-host.exe")
  nyc_execute_output("Win64 C aggregate link failed" "${win64_aggregate_exe}"
    COMMAND "${WIN64_CC}" -std=c11 -O2 -Wall -Wextra -Werror
            -o "${win64_aggregate_exe}"
            "${NY_C_CORPUS_win64-aggregate-host.c}"
            ${win64_aggregate_objects}
  )
  if(WIN64_RUNNER)
    nyc_execute_ok(
      [=[Win64 C aggregate runtime parity failed\n${stdout}${stderr}]=]
      COMMAND "${WIN64_RUNNER}" "${win64_aggregate_exe}"
    )
  else()
    message(STATUS "Win64 C aggregate runtime skipped: Wine was not found")
  endif()
else()
  message(STATUS "Win64 C aggregate link/runtime skipped: MinGW-w64 was not found")
endif()

if(LLD_LINK AND EXISTS "${LLD_LINK}")
  set(coff_object "${BINARY_DIR}/target-link.obj")
  set(coff_exe "${BINARY_DIR}/target-link-coff.exe")
  nyc_execute_output("nyc COFF emission failed" "${coff_object}"
    COMMAND "${NYC}" --target=x86_64-pc-windows-msvc "${target_source}"
                "${coff_object}" c_target_link
  )
  nyc_execute_output("lld-link could not link nyc COFF output" "${coff_exe}"
    COMMAND "${LLD_LINK}" /entry:c_target_link /subsystem:console
            /nodefaultlib /machine:x64 "/out:${coff_exe}" "${coff_object}"
  )
else()
  message(STATUS "COFF link acceptance skipped: lld-link was not found")
endif()

if(LD64_LLD AND EXISTS "${LD64_LLD}")
  set(macho_object "${BINARY_DIR}/target-link-macho.o")
  set(macho_exe "${BINARY_DIR}/target-link-macho")
  nyc_execute_output("nyc Mach-O emission failed" "${macho_object}"
    COMMAND "${NYC}" --target=x86_64-apple-darwin "${target_source}"
                "${macho_object}" c_target_link
  )
  nyc_execute_output("ld64.lld could not link nyc Mach-O output" "${macho_exe}"
    COMMAND "${LD64_LLD}" -arch x86_64
            -platform_version macos 10.15 14.0 -e _c_target_link
            -o "${macho_exe}" "${macho_object}"
  )
else()
  message(STATUS "Mach-O link acceptance skipped: ld64.lld was not found")
endif()

# Negative diagnostics and argument-limit checks.
set(unsupported_cases
  "unsupported.c|c_unsupported|continue statement is not inside a loop"
  "unsupported.c|c_break_outside|break statement is not inside a loop"
  "void.c|c_void_return_value|void function return expression must have void type"

  "const-update.c|c_const_update|cannot assign to a const C object"
  "const-update.c|c_const_expression_update|assignment requires a modifiable C lvalue"
  "const-pointer-store.c|c_const_store|cannot write through a pointer to const C data"
  "const-double-pointer-store.c|c_const_f64_store|cannot write through a pointer to const C data"
  "const-index-store.c|c_const_index_store|cannot write through a pointer to const C data"
  "mixed-pointer-store.c|c_mixed_store|pointer assignment type mismatch"
  "for-scope.c|c_for_scope|must name a function parameter or declared local"
  "compound.c|c_block_scope|must name a function parameter or declared local"
  "compound.c|c_redeclare_local|duplicate declaration in the same C scope"

  "increment.c|c_const_postfix_increment|increment and decrement require a mutable scalar or pointer local"
  "increment.c|c_const_prefix_decrement|increment and decrement require a mutable scalar or pointer local"
  "increment.c|c_const_pointer_increment|increment and decrement require a mutable scalar or pointer local"
  "compound.c|c_pointer_compound_unsupported|compound assignment operator is unsupported for this type"
  "typedef.c|c_typedef_const_update|cannot assign to a const C object"
  "typedef-function-signature-conflict.c|nyc_signature_conflict|conflicting function typedef declarations"
  "global-asm-unsupported.c|main|unsupported file-scope assembler directive or expression"
  "inline-asm-unsupported.c|nyc_inline_asm_unsupported|unsupported inline asm template '__nyc_unsupported_inline_asm:leaq"
  "inline-asm-unsupported.c|nyc_inline_asm_expression_unsupported|GNU inline assembly is not supported by NYC"
  "inline-asm-unsupported.c|nyc_inline_asm_goto_unsupported|unsupported inline asm template '__nyc_unsupported_inline_asm:jmp"
  "invalid-shift.c|c_invalid_shift|unsupported #if expression"
)
foreach(case IN LISTS unsupported_cases)
  string(REPLACE "|" ";" fields "${case}")
  list(GET fields 0 source)
  list(GET fields 1 symbol)
  list(GET fields 2 expected_diagnostic)
  set(object "${BINARY_DIR}/unsupported.o")
  nyc_remove_artifact("${object}")
  execute_process(
    COMMAND "${NYC}" "${NY_C_CORPUS_${source}}" "${object}" "${symbol}"
    RESULT_VARIABLE status
    ERROR_VARIABLE diagnostic
    TIMEOUT 15
  )
  if(NOT "${status}" STREQUAL "1")
    message(FATAL_ERROR
      "nyc returned ${status} for ${source}:${symbol}, expected rejection (1)\n${diagnostic}")
  endif()
  string(FIND "${diagnostic}" "${expected_diagnostic}" found)
  if(found EQUAL -1 OR EXISTS "${object}" OR IS_SYMLINK "${object}")
    message(FATAL_ERROR
      "expected '${expected_diagnostic}' and no object for ${source}:${symbol}\n${diagnostic}")
  endif()
endforeach()

set(too_many_args_source "long c_arg_limit_target(")
set(too_many_args_call "long c_arg_limit_entry() { return c_arg_limit_target(")
foreach(index RANGE 0 64)
  if(index GREATER 0)
    string(APPEND too_many_args_source ", ")
    string(APPEND too_many_args_call ", ")
  endif()
  string(APPEND too_many_args_source "long arg${index}")
  string(APPEND too_many_args_call "0")
endforeach()
string(APPEND too_many_args_source ");\n")
string(APPEND too_many_args_call "); }\n")
set(too_many_args_input "${NY_C_CORPUS_too-many-call-args.c}")

execute_process(
  COMMAND "${NYC}" "${too_many_args_input}"
          "${BINARY_DIR}/too-many-call-args.o" c_arg_limit_entry
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(status EQUAL 0)
  message(FATAL_ERROR "nyc accepted a call beyond the NYIR argument limit")
endif()
string(CONCAT diagnostic "${stdout}" "${stderr}")
string(FIND "${diagnostic}" "direct call exceeds the NYIR limit of 64 arguments"
       found)
if(found EQUAL -1)
  message(FATAL_ERROR "unclear diagnostic for a call beyond the NYIR argument limit\n${diagnostic}")
endif()

# Section, global-assembly, and Linux kernel inline-assembly forms.
set(section_source "${NY_C_CORPUS_named-sections.c}")

nyc_execute_ok(
  [=[nyc named-section data compilation failed\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -c "${section_source}"
            -o "${BINARY_DIR}/named-sections.o"
)
set(section_harness "${NY_C_CORPUS_named-sections-main.c}")

nyc_execute_ok(
  [=[reference compiler could not link NYRI named-section data\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${section_harness}" "${BINARY_DIR}/named-sections.o" -o
            "${BINARY_DIR}/named-sections-host"
)
nyc_execute_ok(
  [=[NYC named-section data placements resolved incorrectly (7 + 0 + 11 = 18 expected)]=]
  COMMAND "${BINARY_DIR}/named-sections-host"
)

set(assembly_source "${NY_C_CORPUS_assembly-output.c}")

nyc_execute_ok(
  [=[nyc assembly output failed\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -S -fverbose-asm "${assembly_source}" -o
            "${BINARY_DIR}/assembly-output.s"
)
file(READ "${BINARY_DIR}/assembly-output.s" assembly_text)
if(NOT assembly_text MATCHES "# NYC verbose assembly:")
  message(FATAL_ERROR "nyc -fverbose-asm omitted assembly annotations")
endif()
nyc_execute_ok(
  [=[host compiler rejected NYC-generated assembly\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/assembly-output.s" -o
            "${BINARY_DIR}/assembly-output"
)
nyc_execute_ok(
  [=[NYC-generated assembly failed at runtime\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/assembly-output"
)

set(assembly_data_source "${NY_C_CORPUS_assembly-data.c}")

nyc_execute_ok(
  [=[nyc assembly output failed for file-scope data and relocations\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -S "${assembly_data_source}" -o
            "${BINARY_DIR}/assembly-data.s"
)
nyc_execute_ok(
  [=[host compiler could not link NYC assembly data output\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/assembly-data.s" -o
            "${BINARY_DIR}/assembly-data"
)
nyc_execute_ok(
  [=[NYC assembly data relocation failed at runtime\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/assembly-data"
)

set(unused_asm_goto_source "${NY_C_CORPUS_unused-asm-goto.c}")

nyc_execute_ok(
  [=[nyc failed to discard unused static-inline asm-goto function\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -c "${unused_asm_goto_source}"
            -o "${BINARY_DIR}/unused-asm-goto.o"
)
nyc_execute_ok(
  [=[host compiler could not link unit after dead asm-goto removal\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/unused-asm-goto.o" -o
            "${BINARY_DIR}/unused-asm-goto"
)
nyc_execute_ok(
  [=[dead asm-goto removal changed the live program result\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/unused-asm-goto"
)
set(used_asm_goto_source "${NY_C_CORPUS_used-asm-goto.c}")

execute_process(
  COMMAND "${NYC}" -c "${used_asm_goto_source}"
          -o "${BINARY_DIR}/used-asm-goto.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(status EQUAL 0)
  message(FATAL_ERROR
    "nyc silently accepted a reachable unsupported asm-goto function\n${stdout}${stderr}")
endif()
set(unused_operand_asm_source "${NY_C_CORPUS_unused-operand-asm.c}")

nyc_execute_ok(
  [=[nyc failed to discard an unused static-inline operand-asm function\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -c "${unused_operand_asm_source}"
            -o "${BINARY_DIR}/unused-operand-asm.o"
)
nyc_execute_ok(
  [=[host compiler could not link after unused operand-asm removal\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/unused-operand-asm.o" -o
            "${BINARY_DIR}/unused-operand-asm"
)
nyc_execute_ok(
  [=[dead asm-goto removal changed the live program result\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/unused-operand-asm"
)
set(unit_inline_source "${NY_C_CORPUS_unit-inline.c}")

foreach(unit_inline_level IN ITEMS -O1 -O2)
  nyc_execute_ok(
    [=[nyc could not optimize an inline function in a C unit\n${stdout}${stderr}]=]
    COMMAND "${NYC}" "${unit_inline_level}" -S "${unit_inline_source}"
                -o "${BINARY_DIR}/unit-inline.s"
  )
  file(READ "${BINARY_DIR}/unit-inline.s" unit_inline_assembly)
  if(unit_inline_assembly MATCHES "call[ \t]+nyc_unit_inline_add")
    message(FATAL_ERROR
      "NYC left an always-available inline helper call in the C unit\n${unit_inline_assembly}")
  endif()
  nyc_execute_ok(
    [=[host assembler/linker rejected optimized C-unit assembly\n${stdout}${stderr}]=]
    COMMAND "${CC}" "${BINARY_DIR}/unit-inline.s"
                -o "${BINARY_DIR}/unit-inline"
  )
  nyc_execute_ok(
    [=[inlined C-unit helper changed runtime behavior\n${stdout}${stderr}]=]
    COMMAND "${BINARY_DIR}/unit-inline"
  )
endforeach()
set(unit_function_type_pointer_source
    "${NY_C_CORPUS_unit-function-type-pointer.c}")
nyc_execute_ok(
  [=[nyc could not emit storage for an extern function-type pointer definition\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -S "${unit_function_type_pointer_source}"
            -o "${BINARY_DIR}/unit-function-type-pointer.s"
)
nyc_execute_ok(
  [=[function-type pointer definition did not link with its C-unit consumer\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/unit-function-type-pointer.s"
            -o "${BINARY_DIR}/unit-function-type-pointer"
)
nyc_execute_ok(
  [=[tentative function-type pointer definition was not zero-initialized\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/unit-function-type-pointer"
)
set(unit_asm_goto_source "${NY_C_CORPUS_unit-asm-goto.c}")

nyc_execute_ok(
  [=[nyc could not lower a call-site inlined jump-label asm goto\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -S "${unit_asm_goto_source}"
            -o "${BINARY_DIR}/unit-asm-goto.s"
)
nyc_execute_ok(
  [=[external assembler rejected NYC jump-label asm goto output\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/unit-asm-goto.s"
            -o "${BINARY_DIR}/unit-asm-goto"
)
nyc_execute_ok(
  [=[NYC jump-label asm goto did not branch to the C label\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/unit-asm-goto"
)
nyc_execute_ok(
  [=[nyc could not emit an out-of-line static-branch asm-goto helper\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O0 -S "${unit_asm_goto_source}"
            -o "${BINARY_DIR}/unit-asm-goto-o0.s"
)
nyc_execute_ok(
  [=[out-of-line static-branch asm-goto helper did not link\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/unit-asm-goto-o0.s"
            -o "${BINARY_DIR}/unit-asm-goto-o0"
)
nyc_execute_ok(
  [=[out-of-line static-branch asm goto did not branch to the C label\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/unit-asm-goto-o0"
)
set(unit_builtin_strcmp_source
    "${NY_C_CORPUS_unit-builtin-strcmp.c}")
foreach(unit_builtin_strcmp_level IN ITEMS -O0 -O2)
  set(unit_builtin_strcmp_base
      "${BINARY_DIR}/unit-builtin-strcmp-${unit_builtin_strcmp_level}")
  nyc_execute_ok(
    [=[nyc could not fold literal __builtin_strcmp guards\n${stdout}${stderr}]=]
    COMMAND "${NYC}" "${unit_builtin_strcmp_level}" -S
                "${unit_builtin_strcmp_source}"
                -o "${unit_builtin_strcmp_base}.s"
  )
  nyc_execute_ok(
    [=[literal __builtin_strcmp left a kernel build-error reference\n${stdout}${stderr}]=]
    COMMAND "${CC}" "${unit_builtin_strcmp_base}.s"
                -o "${unit_builtin_strcmp_base}"
  )
  nyc_execute_ok(
    [=[literal __builtin_strcmp produced incorrect operator results\n${stdout}${stderr}]=]
    COMMAND "${unit_builtin_strcmp_base}"
  )
endforeach()
set(unit_unreachable_loop_label_source
    "${NY_C_CORPUS_unit-unreachable-loop-label.c}")
set(unit_unreachable_loop_label_base
    "${BINARY_DIR}/unit-unreachable-loop-label")
nyc_execute_ok(
  [=[nyc lost loop control targets before an unreachable labeled loop\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O0 -S "${unit_unreachable_loop_label_source}"
            -o "${unit_unreachable_loop_label_base}.s"
)
nyc_execute_ok(
  [=[unreachable labeled loop fixture did not link\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${unit_unreachable_loop_label_base}.s"
            -o "${unit_unreachable_loop_label_base}"
)
nyc_execute_ok(
  [=[unreachable labeled loop changed the early return result\n${stdout}${stderr}]=]
  COMMAND "${unit_unreachable_loop_label_base}"
)
set(unit_dead_used_static_source
    "${NY_C_CORPUS_unit-dead-used-static.c}")
set(unit_dead_used_static_asm "${BINARY_DIR}/unit-dead-used-static.s")
nyc_execute_ok(
  [=[nyc could not lower used static data in a dead if arm\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O0 -S "${unit_dead_used_static_source}"
            -o "${unit_dead_used_static_asm}"
)
file(READ "${unit_dead_used_static_asm}" unit_dead_used_static_asm_text)
if(NOT unit_dead_used_static_asm_text MATCHES
   "\\.section[ \t]+\\\"\\.discard.addressable\\\"" OR
   NOT unit_dead_used_static_asm_text MATCHES
   "\\.quad[ \t]+nyc_dead_used_static_target" OR
   NOT unit_dead_used_static_asm_text MATCHES
   "\\.quad[ \t]+nyc_inlined_used_static_target" OR
   unit_dead_used_static_asm_text MATCHES
   "leaq[ \t]+__nyc_data___nyc_static_")
  message(FATAL_ERROR
    "used static relocation was dropped or emitted as a runtime text reference\n${unit_dead_used_static_asm_text}")
endif()
set(unit_bitfield_constant_mask_source
    "${NY_C_CORPUS_unit-bitfield-constant-mask.c}")
set(unit_bitfield_constant_mask_base
    "${BINARY_DIR}/unit-bitfield-constant-mask")
nyc_execute_ok(
  [=[nyc could not lower the constant bitfield mask at O0\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O0 -S "${unit_bitfield_constant_mask_source}"
            -o "${unit_bitfield_constant_mask_base}.s"
)
file(READ "${unit_bitfield_constant_mask_base}.s"
     unit_bitfield_constant_mask_asm_text)
if(unit_bitfield_constant_mask_asm_text MATCHES "call[ \t]+__bad_mask")
  message(FATAL_ERROR
    "constant bitfield mask left a runtime __bad_mask call\n${unit_bitfield_constant_mask_asm_text}")
endif()
nyc_execute_ok(
  [=[constant bitfield mask left an unresolved compile-time error symbol\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${unit_bitfield_constant_mask_base}.s"
            -o "${unit_bitfield_constant_mask_base}"
)
nyc_execute_ok(
  [=[constant bitfield multiplier produced the wrong value\n${stdout}${stderr}]=]
  COMMAND "${unit_bitfield_constant_mask_base}"
)
nyc_execute_ok(
  [=[constant bitfield multiplier failed on the alternate control-flow path\n${stdout}${stderr}]=]
  COMMAND "${unit_bitfield_constant_mask_base}" branch
)
# Linux fixed-register exception-table assembly cases.
set(lkgs_extable_source "${NY_C_CORPUS_linux-lkgs-extable.c}")
nyc_execute_ok(
  [=[NYC failed to lower Linux LKGS exception-table asm\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -S "${lkgs_extable_source}"
            -o "${BINARY_DIR}/linux-lkgs-extable.s"
)
nyc_execute_ok(
  [=[host assembler rejected Linux LKGS exception-table asm\n${stdout}${stderr}]=]
  COMMAND "${CC}" -c "${BINARY_DIR}/linux-lkgs-extable.s"
            -o "${BINARY_DIR}/linux-lkgs-extable.o"
)
set(loadsegment_extable_source
  "${NY_C_CORPUS_linux-loadsegment-extable.c}")
nyc_execute_ok(
  [=[NYC failed to lower Linux typed segment-load asm\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -S "${loadsegment_extable_source}"
            -o "${BINARY_DIR}/linux-loadsegment-extable.s"
)
nyc_execute_ok(
  [=[host assembler rejected Linux typed segment-load asm\n${stdout}${stderr}]=]
  COMMAND "${CC}" -c "${BINARY_DIR}/linux-loadsegment-extable.s"
            -o "${BINARY_DIR}/linux-loadsegment-extable.o"
)
set(save_segment_source "${NY_C_CORPUS_linux-save-segment.c}")
nyc_execute_ok(
  [=[NYC failed to lower Linux segment-register read asm\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -S "${save_segment_source}"
            -o "${BINARY_DIR}/linux-save-segment.s"
)
nyc_execute_ok(
  [=[host assembler rejected Linux segment-register read asm\n${stdout}${stderr}]=]
  COMMAND "${CC}" -c "${BINARY_DIR}/linux-save-segment.s"
            -o "${BINARY_DIR}/linux-save-segment.o"
)

set(static-cpu-has-asm-goto-source
  "${NY_C_CORPUS_static-cpu-has-asm-goto.c}")

nyc_execute_ok(
  [=[NYC failed to lower static CPU feature asm goto\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -S "${static-cpu-has-asm-goto-source}"
            -o "${BINARY_DIR}/static-cpu-has-asm-goto.s"
)
nyc_execute_ok(
  [=[host assembler rejected static CPU feature asm-goto lowering\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/static-cpu-has-asm-goto.s"
            -o "${BINARY_DIR}/static-cpu-has-asm-goto"
)
nyc_execute_ok(
  [=[static CPU feature asm-goto runtime behavior failed\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/static-cpu-has-asm-goto"
)
set(clear_pages_alternative_source
  "${NY_C_CORPUS_linux-clear-pages-alternative.c}")

nyc_execute_ok(
  [=[NYC failed to lower the Linux clear-pages alternative\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -S "${clear_pages_alternative_source}"
            -o "${BINARY_DIR}/linux-clear-pages-alternative.s"
)
nyc_execute_ok(
  [=[host assembler rejected Linux clear-pages alternative lowering\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/linux-clear-pages-alternative.s"
            -o "${BINARY_DIR}/linux-clear-pages-alternative"
)
nyc_execute_ok(
  [=[Linux clear-pages alternative produced incorrect data\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/linux-clear-pages-alternative"
)
set(clear_user_alternative_source
  "${NY_C_CORPUS_linux-clear-user-alternative.c}")
nyc_execute_ok(
  [=[NYC failed to lower the Linux clear-user alternative\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -S "${clear_user_alternative_source}"
            -o "${BINARY_DIR}/linux-clear-user-alternative.s"
)
file(READ "${BINARY_DIR}/linux-clear-user-alternative.s" clear_user_alt_asm)
if(NOT clear_user_alt_asm MATCHES "movq[^\n]*%rax, %rcx\n[^\n]*xorl[^\n]*\n[^\n]*call[^\n]*rep_stos_alternative")
  message(FATAL_ERROR
    "clear-user must call rep_stos_alternative directly with the length in %rcx\n${clear_user_alt_asm}")
endif()
nyc_execute_ok(
  [=[host assembler rejected Linux clear-user alternative lowering\n${stdout}${stderr}]=]
  COMMAND "${CC}" -c "${BINARY_DIR}/linux-clear-user-alternative.s"
            -o "${BINARY_DIR}/linux-clear-user-alternative.o"
)

set(la57-alternative-source "${NY_C_CORPUS_la57-alternative.c}")

nyc_execute_ok(
  [=[NYC failed to lower the x86 LA57 alternative\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -S "${la57-alternative-source}"
            -o "${BINARY_DIR}/la57-alternative.s"
)
nyc_execute_ok(
  [=[host assembler rejected the x86 LA57 alternative lowering\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/la57-alternative.s"
            -o "${BINARY_DIR}/la57-alternative"
)
nyc_execute_ok(
  [=[x86 LA57 alternative runtime behavior failed\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/la57-alternative"
)
set(ibpb-alternative-source "${NY_C_CORPUS_ibpb-alternative.c}")

nyc_execute_ok(
  [=[NYC failed to lower the x86 IBPB alternative\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -S "${ibpb-alternative-source}"
            -o "${BINARY_DIR}/ibpb-alternative.s"
)
nyc_execute_ok(
  [=[host assembler rejected the x86 IBPB alternative lowering\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/ibpb-alternative.s"
            -o "${BINARY_DIR}/ibpb-alternative"
)
nyc_execute_ok(
  [=[x86 IBPB alternative runtime behavior failed\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/ibpb-alternative"
)
set(div0-alternative-source "${NY_C_CORPUS_div0-alternative.c}")

nyc_execute_ok(
  [=[NYC failed to lower the x86 DIV0 alternative\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -S "${div0-alternative-source}"
            -o "${BINARY_DIR}/div0-alternative.s"
)
nyc_execute_ok(
  [=[host assembler rejected the x86 DIV0 alternative lowering\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/div0-alternative.s"
            -o "${BINARY_DIR}/div0-alternative"
)
nyc_execute_ok(
  [=[x86 DIV0 alternative runtime behavior failed\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/div0-alternative"
)
set(atomic_add_negative_source
  "${NY_C_CORPUS_linux-atomic-add-negative.c}")
nyc_execute_ok(
  [=[NYC failed to lower locked add with a sign-flag output\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -S "${atomic_add_negative_source}"
            -o "${BINARY_DIR}/linux-atomic-add-negative.s"
)
nyc_execute_ok(
  [=[host linker rejected locked add with a sign-flag output\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/linux-atomic-add-negative.s"
            -o "${BINARY_DIR}/linux-atomic-add-negative"
)
nyc_execute_ok(
  [=[locked add sign-flag output returned an incorrect result\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/linux-atomic-add-negative"
)
set(spinlock_xadd_source "${NY_C_CORPUS_linux-spinlock-xadd.c}")
nyc_execute_ok(
  [=[NYC failed to lower xaddq with named read-write operands\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -S "${spinlock_xadd_source}"
            -o "${BINARY_DIR}/linux-spinlock-xadd.s"
)
file(READ "${BINARY_DIR}/linux-spinlock-xadd.s" spinlock_xadd_assembly)
if(spinlock_xadd_assembly MATCHES "lock xaddq")
  message(FATAL_ERROR
    "NYC changed the unlocked Linux xaddq into a locked instruction")
endif()
nyc_execute_ok(
  [=[host linker rejected xaddq named-operand lowering\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/linux-spinlock-xadd.s"
            -o "${BINARY_DIR}/linux-spinlock-xadd"
)
nyc_execute_ok(
  [=[xaddq named-operand runtime result was incorrect\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/linux-spinlock-xadd"
)
set(local-array-range-source
    "${NY_C_CORPUS_linux-local-array-range-init.c}")
nyc_execute_ok(
  [=[nyc could not lower a local GNU array range initializer\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -S "${local-array-range-source}"
            -o "${BINARY_DIR}/linux-local-array-range-init.s"
)
nyc_execute_ok(
  [=[external assembler rejected local array range output\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/linux-local-array-range-init.s"
            -o "${BINARY_DIR}/linux-local-array-range-init"
)
nyc_execute_ok(
  [=[local array range initializer produced incorrect values\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/linux-local-array-range-init"
)
set(user_store_goto_source
  "${NY_C_CORPUS_linux-user-store-goto.c}")
nyc_execute_ok(
  [=[NYC failed to lower fault-table asm goto stores\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -S "${user_store_goto_source}"
            -o "${BINARY_DIR}/linux-user-store-goto.s"
)
nyc_execute_ok(
  [=[host linker rejected fault-table asm goto lowering\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/linux-user-store-goto.s"
            -o "${BINARY_DIR}/linux-user-store-goto"
)
nyc_execute_ok(
  [=[fault-table asm goto store returned an incorrect result\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/linux-user-store-goto"
)
set(atomic_inc_asm_source "${NY_C_CORPUS_atomic-inc-asm.c}")

nyc_execute_ok(
  [=[nyc could not lower lock incq with tied memory constraints\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -S "${atomic_inc_asm_source}"
            -o "${BINARY_DIR}/atomic-inc-asm.s"
)
nyc_execute_ok(
  [=[external assembler rejected NYC lock incq output\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/atomic-inc-asm.s"
            -o "${BINARY_DIR}/atomic-inc-asm"
)
nyc_execute_ok(
  [=[NYC lock incq failed to update its memory operand\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/atomic-inc-asm"
)
set(atomic_and_asm_source "${NY_C_CORPUS_atomic-and-asm.c}")

nyc_execute_ok(
  [=[nyc could not lower lock andl with read-write memory constraints\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -S "${atomic_and_asm_source}"
            -o "${BINARY_DIR}/atomic-and-asm.s"
)
file(READ "${BINARY_DIR}/atomic-and-asm.s" atomic_and_assembly)
if(NOT atomic_and_assembly MATCHES "lock andl" OR
   NOT atomic_and_assembly MATCHES "lock orl" OR
   NOT atomic_and_assembly MATCHES "lock xorl" OR
   NOT atomic_and_assembly MATCHES "lock addl" OR
   NOT atomic_and_assembly MATCHES "lock subl")
  message(FATAL_ERROR "NYC omitted an atomic lock read-modify-write instruction")
endif()
nyc_execute_ok(
  [=[external assembler rejected NYC lock andl output\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/atomic-and-asm.s"
            -o "${BINARY_DIR}/atomic-and-asm"
)
nyc_execute_ok(
  [=[NYC lock andl failed to update its memory operand\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/atomic-and-asm"
)
set(atomic-xorb-condition-source "${NY_C_CORPUS_atomic-xorb-condition.c}")

nyc_execute_ok(
  [=[nyc could not lower locked byte xor with sign output\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -S "${atomic-xorb-condition-source}"
            -o "${BINARY_DIR}/atomic-xorb-condition.s"
)
nyc_execute_ok(
  [=[external assembler rejected NYC locked byte xor output\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/atomic-xorb-condition.s"
            -o "${BINARY_DIR}/atomic-xorb-condition"
)
nyc_execute_ok(
  [=[locked byte xor sign result or memory update was incorrect\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/atomic-xorb-condition"
)
set(cmpxchg64_asm_source "${NY_C_CORPUS_cmpxchg64-asm.c}")

nyc_execute_ok(
  [=[nyc could not lower 64-bit segment-relative cmpxchg asm\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -S "${cmpxchg64_asm_source}"
            -o "${BINARY_DIR}/cmpxchg64-asm.s"
)
nyc_execute_ok(
  [=[external assembler rejected NYC 64-bit cmpxchg output\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/cmpxchg64-asm.s"
            -o "${BINARY_DIR}/cmpxchg64-asm"
)
nyc_execute_ok(
  [=[NYC 64-bit cmpxchg failed to update its memory operand\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/cmpxchg64-asm"
)
set(gs-store-asm-source "${NY_C_CORPUS_gs-store-asm.c}")

nyc_execute_ok(
  [=[nyc could not lower write-only GS-relative memory asm\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -S "${gs-store-asm-source}"
            -o "${BINARY_DIR}/gs-store-asm.s"
)
nyc_execute_ok(
  [=[external assembler rejected write-only GS-relative memory asm\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/gs-store-asm.s"
            -o "${BINARY_DIR}/gs-store-asm"
)
nyc_execute_ok(
  [=[NYC GS-relative write-only memory asm failed\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/gs-store-asm"
)
set(index-mask-asm-source "${NY_C_CORPUS_index-mask-asm.c}")

nyc_execute_ok(
  [=[nyc could not lower unsigned index-mask asm\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -S "${index-mask-asm-source}"
            -o "${BINARY_DIR}/index-mask-asm.s"
)
nyc_execute_ok(
  [=[external assembler rejected unsigned index-mask asm\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/index-mask-asm.s"
            -o "${BINARY_DIR}/index-mask-asm"
)
nyc_execute_ok(
  [=[NYC unsigned index-mask asm produced incorrect bounds masks\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/index-mask-asm"
)
set(visibility-object-source "${NY_C_CORPUS_visibility-object.c}")

nyc_execute_ok(
  [=[nyc could not lower a hidden-visibility aggregate object\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -S "${visibility-object-source}"
            -o "${BINARY_DIR}/visibility-object.s"
)
nyc_execute_ok(
  [=[external assembler rejected a hidden-visibility aggregate object\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/visibility-object.s"
            -o "${BINARY_DIR}/visibility-object"
)
nyc_execute_ok(
  [=[NYC hidden-visibility aggregate object produced incorrect data\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/visibility-object"
)
set(pointer-initializer-field-source
    "${NY_C_CORPUS_pointer-initializer-field.c}")

nyc_execute_ok(
  [=[nyc could not lower a nested array-address pointer initializer\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -S "${pointer-initializer-field-source}"
            -o "${BINARY_DIR}/pointer-initializer-field.s"
)
nyc_execute_ok(
  [=[external assembler rejected a nested array-address pointer initializer\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/pointer-initializer-field.s"
            -o "${BINARY_DIR}/pointer-initializer-field"
)
nyc_execute_ok(
  [=[NYC nested array-address pointer initializer produced an incorrect relocation\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/pointer-initializer-field"
)
set(empty-read-write-asm-source "${NY_C_CORPUS_empty-read-write-asm.c}")

nyc_execute_ok(
  [=[nyc could not lower empty read-write asm\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -S "${empty-read-write-asm-source}"
            -o "${BINARY_DIR}/empty-read-write-asm.s"
)
nyc_execute_ok(
  [=[external assembler rejected empty read-write asm output\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/empty-read-write-asm.s"
            -o "${BINARY_DIR}/empty-read-write-asm"
)
nyc_execute_ok(
  [=[NYC empty read-write asm changed its operand\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/empty-read-write-asm"
)
set(futex-robust-asm-source "${NY_C_CORPUS_futex-robust-asm.c}")

nyc_execute_ok(
  [=[nyc could not lower robust-futex cmpxchg asm\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -S "${futex-robust-asm-source}"
            -o "${BINARY_DIR}/futex-robust-asm.s"
)
nyc_execute_ok(
  [=[external assembler rejected NYC robust-futex output\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/futex-robust-asm.s"
            -o "${BINARY_DIR}/futex-robust-asm"
)
nyc_execute_ok(
  [=[NYC robust-futex unlock semantics failed\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/futex-robust-asm"
)
set(rdtsc-asm-source "${NY_C_CORPUS_rdtsc-asm.c}")

nyc_execute_ok(
  [=[nyc could not lower x86 RDTSC output constraints\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -S "${rdtsc-asm-source}"
            -o "${BINARY_DIR}/rdtsc-asm.s"
)
nyc_execute_ok(
  [=[external assembler rejected NYC RDTSC output\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/rdtsc-asm.s"
            -o "${BINARY_DIR}/rdtsc-asm"
)
nyc_execute_ok(
  [=[NYC RDTSC output did not produce a nonzero timestamp\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/rdtsc-asm"
)
set(x64-narrow-load-source "${NY_C_CORPUS_x64-narrow-load.c}")

nyc_execute_ok(
  [=[nyc could not lower narrow x86-64 memory results\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -S "${x64-narrow-load-source}"
            -o "${BINARY_DIR}/x64-narrow-load.s"
)
nyc_execute_ok(
  [=[external assembler rejected narrow x86-64 memory results\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/x64-narrow-load.s"
            -o "${BINARY_DIR}/x64-narrow-load"
)
nyc_execute_ok(
  [=[NYC narrow memory operations returned incorrect values\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/x64-narrow-load"
)
set(typeof-string-source "${NY_C_CORPUS_typeof-string.c}")

nyc_execute_ok(
  [=[nyc could not resolve typeof(string literal) array type\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -S "${typeof-string-source}"
            -o "${BINARY_DIR}/typeof-string.s"
)
nyc_execute_ok(
  [=[external assembler rejected typeof string output\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/typeof-string.s"
            -o "${BINARY_DIR}/typeof-string"
)
nyc_execute_ok(
  [=[typeof string literal did not retain its array extent\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/typeof-string"
)
set(typeof-address-deref-pointer-local-source
    "${NY_C_CORPUS_typeof-address-deref-pointer-local.c}")

nyc_execute_ok(
  [=[nyc could not retain the pointer type in typeof(*&member)\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -S "${typeof-address-deref-pointer-local-source}"
            -o "${BINARY_DIR}/typeof-address-deref-pointer-local.s"
)
nyc_execute_ok(
  [=[external assembler rejected typeof(*&member) output\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/typeof-address-deref-pointer-local.s"
            -o "${BINARY_DIR}/typeof-address-deref-pointer-local"
)
nyc_execute_ok(
  [=[typeof(*&void_pointer_member) did not preserve its pointer type\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/typeof-address-deref-pointer-local"
)
set(generic-static-init-source "${NY_C_CORPUS_generic-static-init.c}")

nyc_execute_ok(
  [=[nyc could not resolve file-scope _Generic initializers or complete a tentative character array\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -S "${generic-static-init-source}"
            -o "${BINARY_DIR}/generic-static-init.s"
)
nyc_execute_ok(
  [=[external assembler rejected file-scope generic initializer output\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/generic-static-init.s"
            -o "${BINARY_DIR}/generic-static-init"
)
nyc_execute_ok(
  [=[file-scope _Generic initializer selected incorrect data\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/generic-static-init"
)
set(static-designated-array-source
    "${NY_C_CORPUS_static-designated-array.c}")

nyc_execute_ok(
  [=[nyc could not lower sparse file-scope integer array initializers\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -S "${static-designated-array-source}"
            -o "${BINARY_DIR}/static-designated-array.s"
)
nyc_execute_ok(
  [=[external assembler rejected designated integer array output\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/static-designated-array.s"
            -o "${BINARY_DIR}/static-designated-array"
)
nyc_execute_ok(
  [=[sparse designated integer array values were not preserved\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/static-designated-array"
)
set(static-designated-pointer-array-source
    "${NY_C_CORPUS_static-designated-pointer-array.c}")
nyc_execute_ok(
  [=[nyc could not lower a sparse file-scope pointer array\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -S "${static-designated-pointer-array-source}"
            -o "${BINARY_DIR}/static-designated-pointer-array.s"
)
nyc_execute_ok(
  [=[external assembler rejected designated pointer array output\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/static-designated-pointer-array.s"
            -o "${BINARY_DIR}/static-designated-pointer-array"
)
nyc_execute_ok(
  [=[sparse designated pointer array relocations were incorrect\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/static-designated-pointer-array"
)
set(global-generic-array-decay-source
    "${NY_C_CORPUS_global-generic-array-decay.c}")
nyc_execute_ok(
  [=[nyc could not decay an array in a file-scope _Generic control\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -S "${global-generic-array-decay-source}"
            -o "${BINARY_DIR}/global-generic-array-decay.s"
)
nyc_execute_ok(
  [=[external assembler rejected file-scope _Generic array output\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/global-generic-array-decay.s"
            -o "${BINARY_DIR}/global-generic-array-decay"
)
nyc_execute_ok(
  [=[file-scope _Generic did not select the decayed array pointer\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/global-generic-array-decay"
)
set(nested-scalar-pointer-store-source
    "${NY_C_CORPUS_nested-scalar-pointer-store.c}")
nyc_execute_ok(
  [=[nyc could not lower a scalar store through a nested pointer\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -S "${nested-scalar-pointer-store-source}"
            -o "${BINARY_DIR}/nested-scalar-pointer-store.s"
)
nyc_execute_ok(
  [=[external assembler rejected nested scalar pointer output\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/nested-scalar-pointer-store.s"
            -o "${BINARY_DIR}/nested-scalar-pointer-store"
)
nyc_execute_ok(
  [=[nested scalar pointer store wrote an incorrect value\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/nested-scalar-pointer-store"
)
set(address-indexed-pointer-array-member-source
    "${NY_C_CORPUS_address-indexed-pointer-array-member.c}")
nyc_execute_ok(
  [=[nyc could not lower address-of an indexed pointer-array member\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -S "${address-indexed-pointer-array-member-source}"
            -o "${BINARY_DIR}/address-indexed-pointer-array-member.s"
)
nyc_execute_ok(
  [=[external assembler rejected indexed pointer-array member output\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/address-indexed-pointer-array-member.s"
            -o "${BINARY_DIR}/address-indexed-pointer-array-member"
)
nyc_execute_ok(
  [=[indexed pointer-array member address wrote an incorrect value\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/address-indexed-pointer-array-member"
)
set(static-aggregate-redeclaration-source
    "${NY_C_CORPUS_static-aggregate-redeclaration.c}")
nyc_execute_ok(
  [=[nyc could not accept a matching tentative aggregate redeclaration\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -S "${static-aggregate-redeclaration-source}"
            -o "${BINARY_DIR}/static-aggregate-redeclaration.s"
)
nyc_execute_ok(
  [=[external assembler rejected aggregate redeclaration output\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/static-aggregate-redeclaration.s"
            -o "${BINARY_DIR}/static-aggregate-redeclaration"
)
nyc_execute_ok(
  [=[tentative aggregate redeclaration changed the defined object\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/static-aggregate-redeclaration"
)
set(dynamic-alloca-source "${NY_C_CORPUS_dynamic-alloca.c}")

nyc_execute_ok(
  [=[nyc could not emit dynamic alloca object code\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -c "${dynamic-alloca-source}"
            -o "${BINARY_DIR}/dynamic-alloca.o"
)
nyc_execute_ok(
  [=[external linker rejected dynamic alloca object code\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/dynamic-alloca.o"
            -o "${BINARY_DIR}/dynamic-alloca"
)
nyc_execute_ok(
  [=[dynamic alloca object-code smoke failed\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/dynamic-alloca"
)
set(linux-frontend-forms-source "${NY_C_CORPUS_linux-frontend-forms.c}")

nyc_execute_ok(
  [=[nyc rejected Linux pointer, typedef-array, or compound-literal expressions\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 -S "${linux-frontend-forms-source}"
            -o "${BINARY_DIR}/linux-frontend-forms.s"
)
nyc_execute_ok(
  [=[external assembler rejected Linux frontend regression output\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/linux-frontend-forms.s"
            -o "${BINARY_DIR}/linux-frontend-forms"
)
nyc_execute_ok(
  [=[Linux pointer, typedef-array, or compound-literal runtime regression failed\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/linux-frontend-forms"
)
set(used_operand_asm_source "${NY_C_CORPUS_used-operand-asm.c}")

foreach(mode IN ITEMS S c)
  foreach(kind IN ITEMS SEGMENT EXCHANGE)
    execute_process(
      COMMAND "${CMAKE_COMMAND}" -E env NYC_KERNEL_COMPAT=1
              "${NYC}" "-${mode}" "-DNYC_ASM_${kind}"
              "${NY_C_CORPUS_unsupported-kernel-asm.c}"
              -o "${BINARY_DIR}/unsupported-kernel-asm-${kind}.${mode}"
      RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
    if(status EQUAL 0)
      message(FATAL_ERROR
        "nyc silently compiled unsupported ${kind} operands in -${mode} mode\n${stdout}${stderr}")
    endif()
  endforeach()
endforeach()

execute_process(
  COMMAND "${NYC}" -c "${used_operand_asm_source}"
          -o "${BINARY_DIR}/used-operand-asm.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(status EQUAL 0)
  message(FATAL_ERROR
    "nyc silently accepted a reachable unsupported operand-asm function\n${stdout}${stderr}")
endif()

find_program(NM_TOOL nm DOC "binutils nm for symbol assertions")
if(NM_TOOL)
  foreach(route IN ITEMS S c)
    set(retention_object "${BINARY_DIR}/static-data-retention-${route}.o")
    if(route STREQUAL "S")
      set(retention_artifact "${BINARY_DIR}/static-data-retention.s")
    else()
      set(retention_artifact "${retention_object}")
    endif()
    nyc_execute_ok(
      [=[static data retention compile failed\n${stdout}${stderr}]=]
      COMMAND "${NYC}" -O2 "-${route}"
                    "${NY_C_CORPUS_static-data-retention.c}"
                    -o "${retention_artifact}"
    )
    if(route STREQUAL "S")
      nyc_execute_ok(
        [=[static data retention assembly failed\n${stdout}${stderr}]=]
        COMMAND "${CC}" -c "${retention_artifact}" -o "${retention_object}"
      )
    endif()
    nyc_execute_ok(
      [=[dead callback table retained an unavailable feature\n${stdout}${stderr}]=]
      COMMAND "${CC}" -std=c11 -Wall -Wextra -Werror -no-pie
                    "${NY_C_CORPUS_static-data-retention-host.c}"
                    "${retention_object}" -o "${BINARY_DIR}/static-data-retention-${route}"
    )
    nyc_execute_ok(
      [=[live static data lost its callbacks\n${stdout}${stderr}]=]
      COMMAND "${BINARY_DIR}/static-data-retention-${route}"
    )
    execute_process(
      COMMAND "${NM_TOOL}" "${retention_object}"
      RESULT_VARIABLE status
      OUTPUT_VARIABLE symbols
      ERROR_VARIABLE stderr)
    if (NOT status EQUAL 0)
      message(FATAL_ERROR
        "could not inspect retained definitions\n${stderr}")
    endif()
    foreach(symbol IN ITEMS kept_prefix_operations kept_suffix_operations)
      if(NOT symbols MATCHES "[ \t]${symbol}(\r?\n|$)")
        message(FATAL_ERROR "GNU used definition ${symbol} was discarded")
      endif()
    endforeach()
  endforeach()
  set(register_source "${NY_C_CORPUS_global-register.c}")

  nyc_execute_ok(
    [=[nyc rejected a global register declaration\n${stdout}${stderr}]=]
    COMMAND "${NYC}" -c "${register_source}"
                -o "${BINARY_DIR}/global-register.o"
  )
  execute_process(
    COMMAND "${NM_TOOL}" "${BINARY_DIR}/global-register.o"
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(stdout MATCHES "cur" OR stderr MATCHES "cur")
    message(FATAL_ERROR
      "nyc emitted a symbol for a global register variable\n${stdout}${stderr}")
  endif()
  if(OBJDUMP)
    execute_process(
      COMMAND "${OBJDUMP}" -d "${BINARY_DIR}/global-register.o"
      RESULT_VARIABLE status
      OUTPUT_VARIABLE stdout
      ERROR_VARIABLE stderr
    )
    if(NOT status EQUAL 0 OR NOT stdout MATCHES "48[ \t]+89[ \t]+e0")
      message(FATAL_ERROR
        "global RSP access was not emitted as a register read\n${stdout}${stderr}")
    endif()
  endif()
  set(register_host_source "${NY_C_CORPUS_global-register-host.c}")

  nyc_execute_ok(
    [=[host compiler could not link global-register typeof regression\n${stdout}${stderr}]=]
    COMMAND "${CC}" -std=gnu11 -Wall -Wextra -Werror
                -o "${BINARY_DIR}/global-register-host"
                "${register_host_source}" "${BINARY_DIR}/global-register.o"
  )
  nyc_execute_ok(
    [=[global-register typeof regression failed at runtime\n${stdout}${stderr}]=]
    COMMAND "${BINARY_DIR}/global-register-host"
  )
  set(register_call_constraint_source
    "${NY_C_CORPUS_global-register-call-constraint.c}")

  nyc_execute_ok(
    [=[nyc rejected the x86 RSP read/write asm constraint\n${stdout}${stderr}]=]
    COMMAND "${NYC}" -S "${register_call_constraint_source}"
                -o "${BINARY_DIR}/global-register-call-constraint.s"
  )
  nyc_execute_ok(
    [=[assembler rejected NYC's RSP call-constraint output\n${stdout}${stderr}]=]
    COMMAND "${CC}" -c "${BINARY_DIR}/global-register-call-constraint.s"
                -o "${BINARY_DIR}/global-register-call-constraint.o"
  )
  set(register_call_constraint_host
    "${NY_C_CORPUS_global-register-call-constraint-host.c}")

  nyc_execute_ok(
    [=[host compiler could not link the RSP call-constraint regression\n${stdout}${stderr}]=]
    COMMAND "${CC}" -std=gnu11 -Wall -Wextra -Werror
                -o "${BINARY_DIR}/global-register-call-constraint-host"
                "${register_call_constraint_host}"
                "${BINARY_DIR}/global-register-call-constraint.o"
  )
  nyc_execute_ok(
    [=[RSP read/write call-constraint regression failed at runtime\n${stdout}${stderr}]=]
    COMMAND "${BINARY_DIR}/global-register-call-constraint-host"
  )
  set(register_rsp_read_source "${NY_C_CORPUS_global-register-rsp-read.c}")

  nyc_execute_ok(
    [=[nyc rejected a global RSP read\n${stdout}${stderr}]=]
    COMMAND "${NYC}" -O2 -S "${register_rsp_read_source}"
                -o "${BINARY_DIR}/global-register-rsp-read.s"
  )
  nyc_execute_ok(
    [=[assembler rejected the global RSP read output\n${stdout}${stderr}]=]
    COMMAND "${CC}" "${BINARY_DIR}/global-register-rsp-read.s"
                -o "${BINARY_DIR}/global-register-rsp-read"
  )
  nyc_execute_ok(
    [=[global RSP read returned a null or misaligned stack pointer\n${stdout}${stderr}]=]
    COMMAND "${BINARY_DIR}/global-register-rsp-read"
  )
  set(asm_memory_output_source
    "${NY_C_CORPUS_inline-asm-memory-output.c}")
  set(asm_memory_output_host
    "${NY_C_CORPUS_inline-asm-memory-output-host.c}")

  foreach(route IN ITEMS S c)
    if(route STREQUAL "S")
      set(asm_output_artifact "${BINARY_DIR}/inline-asm-memory-output.s")
    else()
      set(asm_output_artifact "${BINARY_DIR}/inline-asm-memory-output.o")
    endif()
    set(asm_output_exe "${BINARY_DIR}/inline-asm-memory-output-${route}")
    nyc_execute_ok(
      [=[nyc rejected an inlined x86 memory-capable asm output\n${stdout}${stderr}]=]
      COMMAND "${NYC}" -O2 "-${route}" "${asm_memory_output_source}"
                  -o "${asm_output_artifact}"
    )
    nyc_execute_ok(
      [=[host compiler could not link the memory asm output regression\n${stdout}${stderr}]=]
      COMMAND "${CC}" -std=gnu11 -Wall -Wextra -Werror
                  "${asm_memory_output_host}" "${asm_output_artifact}"
                  -o "${asm_output_exe}"
    )
    nyc_execute_ok(
      [=[inlined memory asm output regression failed at runtime\n${stdout}${stderr}]=]
      COMMAND "${asm_output_exe}"
    )
  endforeach()
  set(asm_tzcnt_source "${NY_C_CORPUS_inline-asm-tzcnt.c}")

  nyc_execute_ok(
    [=[nyc rejected the x86 tzcnt register-output asm\n${stdout}${stderr}]=]
    COMMAND "${NYC}" -S "${asm_tzcnt_source}"
                -o "${BINARY_DIR}/inline-asm-tzcnt.s"
  )
  file(READ "${BINARY_DIR}/inline-asm-tzcnt.s" asm_tzcnt_output)
  if(NOT asm_tzcnt_output MATCHES "tzcntq[ \t]+%rax, %rax")
    message(FATAL_ERROR
      "NYC did not lower tzcnt asm to the x86-64 instruction\n${asm_tzcnt_output}")
  endif()
  set(asm_memory_input_source
    "${NY_C_CORPUS_inline-asm-memory-input-immediate.c}")

  nyc_execute_ok(
    [=[nyc rejected an x86 immediate-plus-memory asm input\n${stdout}${stderr}]=]
    COMMAND "${NYC}" -S "${asm_memory_input_source}"
                -o "${BINARY_DIR}/inline-asm-memory-input-immediate.s"
  )
  nyc_execute_ok(
    [=[assembler rejected NYC's memory-input asm output\n${stdout}${stderr}]=]
    COMMAND "${CC}" -c "${BINARY_DIR}/inline-asm-memory-input-immediate.s"
                -o "${BINARY_DIR}/inline-asm-memory-input-immediate.o"
  )
  set(asm_percpu_read_source
    "${NY_C_CORPUS_inline-asm-percpu-read.c}")

  nyc_execute_ok(
    [=[nyc rejected the x86-64 per-CPU read asm\n${stdout}${stderr}]=]
    COMMAND "${NYC}" -S "${asm_percpu_read_source}"
                -o "${BINARY_DIR}/inline-asm-percpu-read.s"
  )
  file(READ "${BINARY_DIR}/inline-asm-percpu-read.s" asm_percpu_read_output)
  if(NOT asm_percpu_read_output MATCHES
     "movq[ \t]+%gs:\\(%rax\\), %rax")
    message(FATAL_ERROR
      "NYC did not lower the x86-64 per-CPU read asm\n${asm_percpu_read_output}")
  endif()
  nyc_execute_ok(
    [=[assembler rejected NYC's per-CPU read output\n${stdout}${stderr}]=]
    COMMAND "${CC}" -c "${BINARY_DIR}/inline-asm-percpu-read.s"
                -o "${BINARY_DIR}/inline-asm-percpu-read.o"
  )
  set(asm_percpu_inc_source
    "${NY_C_CORPUS_inline-asm-percpu-inc.c}")

  nyc_execute_ok(
    [=[nyc rejected the x86-64 per-CPU increment asm\n${stdout}${stderr}]=]
    COMMAND "${NYC}" -c "${asm_percpu_inc_source}"
                -o "${BINARY_DIR}/inline-asm-percpu-inc.o"
  )
  set(asm_gs_percpu_add_source
    "${NY_C_CORPUS_inline-asm-gs-percpu-add.c}")
  nyc_execute_ok(
    [=[nyc rejected the GS-address-space per-CPU add asm\n${stdout}${stderr}]=]
    COMMAND "${NYC}" -S "${asm_gs_percpu_add_source}"
                -o "${BINARY_DIR}/inline-asm-gs-percpu-add.s"
  )
  file(READ "${BINARY_DIR}/inline-asm-gs-percpu-add.s"
       asm_gs_percpu_add_output)
  if(NOT asm_gs_percpu_add_output MATCHES
     "addb[ \t]+%r11b, %gs:\\(%rax\\)" OR
     NOT asm_gs_percpu_add_output MATCHES
     "addw[ \t]+%r11w, %gs:\\(%rax\\)" OR
     NOT asm_gs_percpu_add_output MATCHES
     "addl[ \t]+%r11d, %gs:\\(%rax\\)" OR
     NOT asm_gs_percpu_add_output MATCHES
     "addq[ \t]+%r11, %gs:\\(%rax\\)")
    message(FATAL_ERROR
      "NYC omitted the GS-relative per-CPU add\n${asm_gs_percpu_add_output}")
  endif()
  nyc_execute_ok(
    [=[assembler rejected NYC's GS-relative per-CPU add\n${stdout}${stderr}]=]
    COMMAND "${CC}" -c "${BINARY_DIR}/inline-asm-gs-percpu-add.s"
                -o "${BINARY_DIR}/inline-asm-gs-percpu-add.o"
  )
  set(asm_memory_input_host "${NY_C_CORPUS_inline-asm-memory-input-host.c}")

  nyc_execute_ok(
    [=[host compiler could not link the memory-input asm regression\n${stdout}${stderr}]=]
    COMMAND "${CC}" -std=gnu11 -Wall -Wextra -Werror
                -o "${BINARY_DIR}/inline-asm-memory-input-host"
                "${asm_memory_input_host}"
                "${BINARY_DIR}/inline-asm-memory-input-immediate.o"
  )
  nyc_execute_ok(
    [=[memory-input asm regression failed at runtime\n${stdout}${stderr}]=]
    COMMAND "${BINARY_DIR}/inline-asm-memory-input-host"
  )
  set(asm_fixed_register_source
    "${NY_C_CORPUS_inline-asm-fixed-register.c}")

  nyc_execute_ok(
    [=[nyc rejected fixed-register inline-asm inputs\n${stdout}${stderr}]=]
    COMMAND "${NYC}" -S "${asm_fixed_register_source}"
                -o "${BINARY_DIR}/inline-asm-fixed-register.s"
  )
  nyc_execute_ok(
    [=[assembler rejected fixed-register inline-asm output\n${stdout}${stderr}]=]
    COMMAND "${CC}" -c "${BINARY_DIR}/inline-asm-fixed-register.s"
                -o "${BINARY_DIR}/inline-asm-fixed-register.o"
  )
  set(asm_fixed_register_host
    "${NY_C_CORPUS_inline-asm-fixed-register-host.c}")

  nyc_execute_ok(
    [=[host compiler could not link the fixed-register asm regression\n${stdout}${stderr}]=]
    COMMAND "${CC}" -std=gnu11 -Wall -Wextra -Werror
                -o "${BINARY_DIR}/inline-asm-fixed-register-host"
                "${asm_fixed_register_host}"
                "${BINARY_DIR}/inline-asm-fixed-register.o"
  )
  nyc_execute_ok(
    [=[fixed-register asm inputs were not preserved at runtime\n${stdout}${stderr}]=]
    COMMAND "${BINARY_DIR}/inline-asm-fixed-register-host"
  )
  set(asm_memory_input_source
    "${NY_C_CORPUS_inline-asm-memory-input.c}")

  nyc_execute_ok(
    [=[nyc rejected a standalone inline-asm memory input\n${stdout}${stderr}]=]
    COMMAND "${NYC}" -S "${asm_memory_input_source}"
                -o "${BINARY_DIR}/inline-asm-memory-input.s"
  )
  nyc_execute_ok(
    [=[assembler rejected inline-asm memory input output\n${stdout}${stderr}]=]
    COMMAND "${CC}" -c "${BINARY_DIR}/inline-asm-memory-input.s"
                -o "${BINARY_DIR}/inline-asm-memory-input.o"
  )
  nyc_execute_ok(
    [=[host linker rejected inline-asm memory input output\n${stdout}${stderr}]=]
    COMMAND "${CC}" -o "${BINARY_DIR}/inline-asm-memory-input"
                "${BINARY_DIR}/inline-asm-memory-input.o"
  )
  nyc_execute_ok(
    [=[inline-asm memory input produced an incorrect runtime value\n${stdout}${stderr}]=]
    COMMAND "${BINARY_DIR}/inline-asm-memory-input"
  )
  set(asm_memory_register_input_source
    "${NY_C_CORPUS_inline-asm-memory-register-input.c}")

  nyc_execute_ok(
    [=[nyc rejected inline-asm memory and register inputs\n${stdout}${stderr}]=]
    COMMAND "${NYC}" -S "${asm_memory_register_input_source}"
                -o "${BINARY_DIR}/inline-asm-memory-register-input.s"
  )
  nyc_execute_ok(
    [=[assembler rejected inline-asm memory/register output\n${stdout}${stderr}]=]
    COMMAND "${CC}" -c
                "${BINARY_DIR}/inline-asm-memory-register-input.s"
                -o "${BINARY_DIR}/inline-asm-memory-register-input.o"
  )
  nyc_execute_ok(
    [=[host linker rejected inline-asm memory/register output\n${stdout}${stderr}]=]
    COMMAND "${CC}" -o "${BINARY_DIR}/inline-asm-memory-register-input"
                "${BINARY_DIR}/inline-asm-memory-register-input.o"
  )
  nyc_execute_ok(
    [=[inline-asm memory/register inputs produced an incorrect value\n${stdout}${stderr}]=]
    COMMAND "${BINARY_DIR}/inline-asm-memory-register-input"
  )
  set(asm_carry_output_source
    "${NY_C_CORPUS_inline-asm-carry-output.c}")

  nyc_execute_ok(
    [=[nyc rejected carry-flag inline-asm output\n${stdout}${stderr}]=]
    COMMAND "${NYC}" -S "${asm_carry_output_source}"
                -o "${BINARY_DIR}/inline-asm-carry-output.s"
  )
  nyc_execute_ok(
    [=[host linker rejected carry-flag inline-asm output\n${stdout}${stderr}]=]
    COMMAND "${CC}" "${BINARY_DIR}/inline-asm-carry-output.s" -o
                "${BINARY_DIR}/inline-asm-carry-output"
  )
  nyc_execute_ok(
    [=[carry-flag inline-asm output returned an incorrect result\n${stdout}${stderr}]=]
    COMMAND "${BINARY_DIR}/inline-asm-carry-output"
  )
  set(asm_register_output_source
    "${NY_C_CORPUS_inline-asm-register-output.c}")

  nyc_execute_ok(
    [=[nyc rejected an unnamed register output constraint\n${stdout}${stderr}]=]
    COMMAND "${NYC}" -S "${asm_register_output_source}"
                -o "${BINARY_DIR}/inline-asm-register-output.s"
  )
  nyc_execute_ok(
    [=[host linker rejected register-output assembly\n${stdout}${stderr}]=]
    COMMAND "${CC}" "${BINARY_DIR}/inline-asm-register-output.s" -o
                "${BINARY_DIR}/inline-asm-register-output"
  )
  nyc_execute_ok(
    [=[unnamed register output returned an incorrect result\n${stdout}${stderr}]=]
    COMMAND "${BINARY_DIR}/inline-asm-register-output"
  )
  set(asm_memory_output_source
    "${NY_C_CORPUS_inline-asm-memory-output-unnamed.c}")

  nyc_execute_ok(
    [=[nyc rejected an unnamed read-write memory operand\n${stdout}${stderr}]=]
    COMMAND "${NYC}" -S "${asm_memory_output_source}"
                -o "${BINARY_DIR}/inline-asm-memory-output-unnamed.s"
  )
  nyc_execute_ok(
    [=[host linker rejected read-write memory output assembly\n${stdout}${stderr}]=]
    COMMAND "${CC}" "${BINARY_DIR}/inline-asm-memory-output-unnamed.s" -o
                "${BINARY_DIR}/inline-asm-memory-output-unnamed"
  )
  nyc_execute_ok(
    [=[read-write memory output did not update its operand\n${stdout}${stderr}]=]
    COMMAND "${BINARY_DIR}/inline-asm-memory-output-unnamed"
  )
  set(asm_named_carry_output_source
    "${NY_C_CORPUS_inline-asm-named-carry-output.c}")

  nyc_execute_ok(
    [=[nyc rejected named memory and carry-flag asm outputs\n${stdout}${stderr}]=]
    COMMAND "${NYC}" -S "${asm_named_carry_output_source}"
                -o "${BINARY_DIR}/inline-asm-named-carry-output.s"
  )
  nyc_execute_ok(
    [=[host linker rejected named carry-output assembly\n${stdout}${stderr}]=]
    COMMAND "${CC}" "${BINARY_DIR}/inline-asm-named-carry-output.s" -o
                "${BINARY_DIR}/inline-asm-named-carry-output"
  )
  nyc_execute_ok(
    [=[named carry-output assembly returned an incorrect result\n${stdout}${stderr}]=]
    COMMAND "${BINARY_DIR}/inline-asm-named-carry-output"
  )
  set(asm_nonzero_memory_output_source
    "${NY_C_CORPUS_inline-asm-nonzero-memory-output.c}")

  nyc_execute_ok(
    [=[nyc rejected nonzero-flag memory asm input\n${stdout}${stderr}]=]
    COMMAND "${NYC}" -S "${asm_nonzero_memory_output_source}"
                -o "${BINARY_DIR}/inline-asm-nonzero-memory-output.s"
  )
  nyc_execute_ok(
    [=[host linker rejected nonzero-flag assembly\n${stdout}${stderr}]=]
    COMMAND "${CC}" "${BINARY_DIR}/inline-asm-nonzero-memory-output.s" -o
                "${BINARY_DIR}/inline-asm-nonzero-memory-output"
  )
  nyc_execute_ok(
    [=[nonzero-flag memory asm returned an incorrect result\n${stdout}${stderr}]=]
    COMMAND "${BINARY_DIR}/inline-asm-nonzero-memory-output"
  )
  set(parenthesized_function_pointer_source
    "${NY_C_CORPUS_parenthesized-function-pointer.c}")

  nyc_execute_ok(
    [=[nyc rejected a parenthesized file-scope function pointer\n${stdout}${stderr}]=]
    COMMAND "${NYC}" -S "${parenthesized_function_pointer_source}"
                -o "${BINARY_DIR}/parenthesized-function-pointer.s"
  )
  nyc_execute_ok(
    [=[assembler rejected parenthesized function-pointer relocation\n${stdout}${stderr}]=]
    COMMAND "${CC}" -c "${BINARY_DIR}/parenthesized-function-pointer.s"
                -o "${BINARY_DIR}/parenthesized-function-pointer.o"
  )
  if(READELF)
    execute_process(
      COMMAND "${READELF}" -rW
              "${BINARY_DIR}/parenthesized-function-pointer.o"
      RESULT_VARIABLE status
      OUTPUT_VARIABLE stdout
      ERROR_VARIABLE stderr
    )
    if(NOT status EQUAL 0 OR
       NOT stdout MATCHES "\\.rela\\.nyc_exitcall")
      message(FATAL_ERROR
        "function-pointer object lacks its function relocation\n${stdout}${stderr}")
    endif()
  endif()
  set(typeof_function_call_source
    "${NY_C_CORPUS_typeof-function-call.c}")

  nyc_execute_ok(
    [=[nyc rejected a typeof function-pointer call\n${stdout}${stderr}]=]
    COMMAND "${NYC}" -S "${typeof_function_call_source}"
                -o "${BINARY_DIR}/typeof-function-call.s"
  )
  nyc_execute_ok(
    [=[host linker rejected typeof function-pointer call\n${stdout}${stderr}]=]
    COMMAND "${CC}" "${BINARY_DIR}/typeof-function-call.s" -o
                "${BINARY_DIR}/typeof-function-call"
  )
  nyc_execute_ok(
    [=[typeof function-pointer call returned an incorrect result\n${stdout}${stderr}]=]
    COMMAND "${BINARY_DIR}/typeof-function-call"
  )
  set(static_self_reference_source
    "${NY_C_CORPUS_static-self-reference.c}")

  nyc_execute_ok(
    [=[nyc rejected a self-referential static aggregate\n${stdout}${stderr}]=]
    COMMAND "${NYC}" -S "${static_self_reference_source}"
                -o "${BINARY_DIR}/static-self-reference.s"
  )
  nyc_execute_ok(
    [=[host linker rejected static self-reference assembly\n${stdout}${stderr}]=]
    COMMAND "${CC}" "${BINARY_DIR}/static-self-reference.s" -o
                "${BINARY_DIR}/static-self-reference"
  )
  nyc_execute_ok(
    [=[static aggregate self-relocations point to the wrong object\n${stdout}${stderr}]=]
    COMMAND "${BINARY_DIR}/static-self-reference"
  )
  set(asm_aggregate_memory_source
    "${NY_C_CORPUS_inline-asm-aggregate-memory.c}")

  nyc_execute_ok(
    [=[nyc rejected an aggregate inline-asm memory input\n${stdout}${stderr}]=]
    COMMAND "${NYC}" -S "${asm_aggregate_memory_source}"
                -o "${BINARY_DIR}/inline-asm-aggregate-memory.s"
  )
  nyc_execute_ok(
    [=[assembler rejected aggregate inline-asm memory output\n${stdout}${stderr}]=]
    COMMAND "${CC}" -c "${BINARY_DIR}/inline-asm-aggregate-memory.s"
                -o "${BINARY_DIR}/inline-asm-aggregate-memory.o"
  )
  set(asm_named_immediate_source
    "${NY_C_CORPUS_inline-asm-named-immediate.c}")

  nyc_execute_ok(
    [=[nyc rejected named inline-asm immediate operands\n${stdout}${stderr}]=]
    COMMAND "${NYC}" -S "${asm_named_immediate_source}"
                -o "${BINARY_DIR}/inline-asm-named-immediate.s"
  )
  nyc_execute_ok(
    [=[assembler/linker rejected named inline-asm output\n${stdout}${stderr}]=]
    COMMAND "${CC}" "${BINARY_DIR}/inline-asm-named-immediate.s"
                -o "${BINARY_DIR}/inline-asm-named-immediate"
  )
  nyc_execute_ok(
    [=[named inline-asm immediate produced an incorrect value\n${stdout}${stderr}]=]
    COMMAND "${BINARY_DIR}/inline-asm-named-immediate"
  )
  set(aggregate_matrix_source
    "${NY_C_CORPUS_aggregate-matrix.c}")

  nyc_execute_ok(
    [=[nyc rejected multidimensional aggregate indexing\n${stdout}${stderr}]=]
    COMMAND "${NYC}" -S "${aggregate_matrix_source}"
                -o "${BINARY_DIR}/aggregate-matrix.s"
  )
  nyc_execute_ok(
    [=[assembler rejected multidimensional aggregate output\n${stdout}${stderr}]=]
    COMMAND "${CC}" -c "${BINARY_DIR}/aggregate-matrix.s"
                -o "${BINARY_DIR}/aggregate-matrix.o"
  )
  set(aggregate_matrix_host
    "${NY_C_CORPUS_aggregate-matrix-host.c}")

  nyc_execute_ok(
    [=[host could not link multidimensional aggregate regression\n${stdout}${stderr}]=]
    COMMAND "${CC}" -std=gnu11 -Wall -Wextra -Werror
                -o "${BINARY_DIR}/aggregate-matrix-host"
                "${aggregate_matrix_host}" "${BINARY_DIR}/aggregate-matrix.o"
  )
  nyc_execute_ok(
    [=[multidimensional aggregate indexing returned the wrong element\n${stdout}${stderr}]=]
    COMMAND "${BINARY_DIR}/aggregate-matrix-host"
  )
  set(local_pointer_to_array_source
    "${NY_C_CORPUS_local-pointer-to-array.c}")

  nyc_execute_ok(
    [=[nyc rejected a local pointer-to-array declaration\n${stdout}${stderr}]=]
    COMMAND "${NYC}" -S "${local_pointer_to_array_source}"
                -o "${BINARY_DIR}/local-pointer-to-array.s"
  )
  nyc_execute_ok(
    [=[host linker rejected local pointer-to-array output\n${stdout}${stderr}]=]
    COMMAND "${CC}" "${BINARY_DIR}/local-pointer-to-array.s" -o
                "${BINARY_DIR}/local-pointer-to-array"
  )
  nyc_execute_ok(
    [=[local pointer-to-array indexing returned the wrong element\n${stdout}${stderr}]=]
    COMMAND "${BINARY_DIR}/local-pointer-to-array"
  )
  set(pointer_to_array_source
    "${NY_C_CORPUS_pointer-to-array-field.c}")

  nyc_execute_ok(
    [=[nyc rejected pointer-to-array aggregate fields\n${stdout}${stderr}]=]
    COMMAND "${NYC}" -S "${pointer_to_array_source}"
                -o "${BINARY_DIR}/pointer-to-array-field.s"
  )
  nyc_execute_ok(
    [=[assembler rejected pointer-to-array field output\n${stdout}${stderr}]=]
    COMMAND "${CC}" -c "${BINARY_DIR}/pointer-to-array-field.s"
                -o "${BINARY_DIR}/pointer-to-array-field.o"
  )
  nyc_execute_ok(
    [=[host linker rejected pointer-to-array field output\n${stdout}${stderr}]=]
    COMMAND "${CC}" -o "${BINARY_DIR}/pointer-to-array-field"
                "${BINARY_DIR}/pointer-to-array-field.o"
  )
  nyc_execute_ok(
    [=[pointer-to-array field size was incorrect\n${stdout}${stderr}]=]
    COMMAND "${BINARY_DIR}/pointer-to-array-field"
  )
  set(noinstr_attribute_source
    "${NY_C_CORPUS_noinstr-attributes.c}")

  nyc_execute_ok(
    [=[nyc rejected Linux noinstr attributes\n${stdout}${stderr}]=]
    COMMAND "${NYC}" -S "${noinstr_attribute_source}"
                -o "${BINARY_DIR}/noinstr-attributes.s"
  )
  nyc_execute_ok(
    [=[assembler rejected Linux noinstr attribute output\n${stdout}${stderr}]=]
    COMMAND "${CC}" -c "${BINARY_DIR}/noinstr-attributes.s"
                -o "${BINARY_DIR}/noinstr-attributes.o"
  )
  nyc_execute_ok(
    [=[host linker rejected Linux noinstr attribute output\n${stdout}${stderr}]=]
    COMMAND "${CC}" -o "${BINARY_DIR}/noinstr-attributes"
                "${BINARY_DIR}/noinstr-attributes.o"
  )
  nyc_execute_ok(
    [=[Linux noinstr attribute function returned an incorrect value\n${stdout}${stderr}]=]
    COMMAND "${BINARY_DIR}/noinstr-attributes"
  )
  set(aggregate_compound_source
    "${NY_C_CORPUS_aggregate-compound-literal.c}")

  nyc_execute_ok(
    [=[nyc rejected a runtime aggregate compound literal\n${stdout}${stderr}]=]
    COMMAND "${NYC}" -S "${aggregate_compound_source}"
                -o "${BINARY_DIR}/aggregate-compound-literal.s"
  )
  nyc_execute_ok(
    [=[assembler rejected NYC's aggregate compound-literal output\n${stdout}${stderr}]=]
    COMMAND "${CC}" -c "${BINARY_DIR}/aggregate-compound-literal.s"
                -o "${BINARY_DIR}/aggregate-compound-literal.o"
  )
  set(aggregate_compound_host
    "${NY_C_CORPUS_aggregate-compound-literal-host.c}")

  nyc_execute_ok(
    [=[host compiler could not link the aggregate compound-literal regression\n${stdout}${stderr}]=]
    COMMAND "${CC}" -std=gnu11 -Wall -Wextra -Werror
                -o "${BINARY_DIR}/aggregate-compound-literal-host"
                "${aggregate_compound_host}"
                "${BINARY_DIR}/aggregate-compound-literal.o"
  )
  nyc_execute_ok(
    [=[aggregate compound-literal regression failed at runtime\n${stdout}${stderr}]=]
    COMMAND "${BINARY_DIR}/aggregate-compound-literal-host"
  )
  set(register_pointer_source "${NY_C_CORPUS_global-register-pointer.c}")

  nyc_execute_ok(
    [=[nyc rejected a pointer-typed global RSP read\n${stdout}${stderr}]=]
    COMMAND "${NYC}" -c "${register_pointer_source}"
                -o "${BINARY_DIR}/global-register-pointer.o"
  )
  if(OBJDUMP)
    execute_process(
      COMMAND "${OBJDUMP}" -d "${BINARY_DIR}/global-register-pointer.o"
      RESULT_VARIABLE status
      OUTPUT_VARIABLE stdout
      ERROR_VARIABLE stderr
    )
    if(NOT status EQUAL 0 OR NOT stdout MATCHES "48[ \t]+89[ \t]+e0")
      message(FATAL_ERROR
        "pointer-typed global RSP read did not emit `mov %rsp, %rax`\n${stdout}${stderr}")
    endif()
  endif()
  set(register_pointer_host_source
      "${NY_C_CORPUS_global-register-pointer-host.c}")

  nyc_execute_ok(
    [=[host compiler could not link pointer-typed global RSP regression\n${stdout}${stderr}]=]
    COMMAND "${CC}" -std=gnu11 -Wall -Wextra -Werror
                -o "${BINARY_DIR}/global-register-pointer-host"
                "${register_pointer_host_source}"
                "${BINARY_DIR}/global-register-pointer.o"
  )
  nyc_execute_ok(
    [=[pointer-typed global RSP regression failed at runtime\n${stdout}${stderr}]=]
    COMMAND "${BINARY_DIR}/global-register-pointer-host"
  )
endif()
# Kernel data sections, register variables, and low-level runtime helpers.
set(register_use_source "${NY_C_CORPUS_global-register-use.c}")

execute_process(
  COMMAND "${NYC}" -c "${register_use_source}"
          -o "${BINARY_DIR}/global-register-use.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(status EQUAL 0)
  message(FATAL_ERROR "nyc accepted a data use of a global register variable\n${stdout}${stderr}")
endif()
set(register_write_source "${NY_C_CORPUS_global-register-write.c}")

execute_process(
  COMMAND "${NYC}" -c "${register_write_source}"
          -o "${BINARY_DIR}/global-register-write.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(status EQUAL 0)
  message(FATAL_ERROR "nyc accepted a write to the global RSP register\n${stdout}${stderr}")
endif()
set(register_dup_source "${NY_C_CORPUS_global-register-dup.c}")

execute_process(
  COMMAND "${NYC}" -c "${register_dup_source}"
          -o "${BINARY_DIR}/global-register-dup.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(status EQUAL 0 OR NOT stderr MATCHES "inconsistent declaration of global register variable")
  message(FATAL_ERROR "nyc accepted conflicting global register bindings\n${stdout}${stderr}")
endif()
set(register_non_rsp_source "${NY_C_CORPUS_global-register-non-rsp.c}")

execute_process(
  COMMAND "${NYC}" -c "${register_non_rsp_source}"
          -o "${BINARY_DIR}/global-register-non-rsp.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(status EQUAL 0 OR
   NOT stderr MATCHES "global register variable.*rbx.*unsupported by NYC")
  message(FATAL_ERROR
    "nyc did not clearly reject an unsupported non-RSP binding\n${stdout}${stderr}")
endif()
set(register_init_source "${NY_C_CORPUS_global-register-init.c}")

execute_process(
  COMMAND "${NYC}" -c "${register_init_source}"
          -o "${BINARY_DIR}/global-register-init.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(status EQUAL 0 OR NOT stderr MATCHES "global register variables cannot have initializers")
  message(FATAL_ERROR "nyc accepted a global register initializer\n${stdout}${stderr}")
endif()
set(initdata_source "${NY_C_CORPUS_initdata-array.c}")

nyc_execute_ok(
  [=[nyc rejected an extern char array with a mid-position section attribute\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -c "${initdata_source}"
            -o "${BINARY_DIR}/initdata-array.o"
)
set(initdata_harness "${NY_C_CORPUS_initdata-array-main.c}")

nyc_execute_ok(
  [=[reference compiler could not link an NYRI extern char-array reference]=]
  COMMAND "${CC}" "${initdata_harness}" "${BINARY_DIR}/initdata-array.o" -o
            "${BINARY_DIR}/initdata-array-host"
)
nyc_execute_ok(
  [=[NYC extern char-array reference resolved incorrectly]=]
  COMMAND "${BINARY_DIR}/initdata-array-host"
)
set(function_section_source "${NY_C_CORPUS_function-section.c}")

nyc_execute_ok(
  [=[nyc named function section compilation failed\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -c "${function_section_source}"
            -o "${BINARY_DIR}/function-section.o"
)
if(OBJDUMP)
  execute_process(
    COMMAND "${OBJDUMP}" -t "${BINARY_DIR}/function-section.o"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0 OR
     NOT stdout MATCHES "\\.probe\\.text" OR
     NOT stdout MATCHES "\\.probe\\.init\\.text.*nyc_parameter_section_function")
    message(FATAL_ERROR
      "NYC functions did not land in their requested ELF sections\n${stdout}${stderr}")
  endif()
endif()
set(function_section_harness "${NY_C_CORPUS_function-section-main.c}")

nyc_execute_ok(
  [=[host compiler could not link the named function section object\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${function_section_harness}"
            "${BINARY_DIR}/function-section.o" -o
            "${BINARY_DIR}/function-section-host"
)
nyc_execute_ok(
  [=[NYC named function section execution returned a wrong value]=]
  COMMAND "${BINARY_DIR}/function-section-host"
)
set(section_conflict_source "${NY_C_CORPUS_section-conflict.c}")

execute_process(
  COMMAND "${NYC}" -c "${section_conflict_source}"
          -o "${BINARY_DIR}/section-conflict.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(status EQUAL 0 OR NOT stderr MATCHES "conflicting section attributes")
  message(FATAL_ERROR "nyc accepted conflicting section attributes\n${stdout}${stderr}")
endif()
set(section_conflict_source "${NY_C_CORPUS_section-conflict-defined.c}")

execute_process(
  COMMAND "${NYC}" -c "${section_conflict_source}"
          -o "${BINARY_DIR}/section-conflict-defined.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(status EQUAL 0 OR NOT stderr MATCHES "duplicate file-scope C data symbol")
  message(FATAL_ERROR "nyc accepted a duplicated section definition\n${stdout}${stderr}")
endif()
if(NOT EXISTS "${LD}")
  message(FATAL_ERROR "kernel code-model regression needs the configured linker: ${LD}")
endif()
set(kernel_model_source "${NY_C_CORPUS_kernel-model-range.c}")
nyc_execute_ok(
  [=[nyc kernel code-model object compilation failed\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -c -mcmodel=kernel "${kernel_model_source}"
            -o "${BINARY_DIR}/kernel-model-range.o"
)
nyc_execute_ok(
  [=[kernel-model PC-relative reference within range failed to link\n${stdout}${stderr}]=]
  COMMAND "${LD}" -m elf_x86_64 -Ttext=0xffffffff81000000
            -Tdata=0xffffffffa0000000
            -o "${BINARY_DIR}/kernel-model-in-range.elf"
            "${BINARY_DIR}/kernel-model-range.o"
)
execute_process(
  COMMAND "${LD}" -m elf_x86_64 -Ttext=0xffffffff81000000
          -Tdata=0x10000000
          -o "${BINARY_DIR}/kernel-model-out-of-range.elf"
          "${BINARY_DIR}/kernel-model-range.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(status EQUAL 0 OR
   NOT stderr MATCHES "relocation.*(truncated to fit|out of range)")
  message(FATAL_ERROR
    "linker did not reject an out-of-range kernel-model reference\n${stdout}${stderr}")
endif()
set(frame_address_source "${NY_C_CORPUS_frame-address.c}")

nyc_execute_ok(
  [=[frame-address builtin compilation failed\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -c "${frame_address_source}"
            -o "${BINARY_DIR}/frame-address.o"
)
set(frame_address_harness "${NY_C_CORPUS_frame-address-main.c}")

nyc_execute_ok(
  [=[host compiler could not link frame-address regression\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${frame_address_harness}"
            "${BINARY_DIR}/frame-address.o" -o
            "${BINARY_DIR}/frame-address-host"
)
nyc_execute_ok(
  [=[NYC frame-address builtin returned an invalid frame address\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/frame-address-host"
)
nyc_execute_ok(
  [=[optimized NYC assembly failed for frame-address builtin\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O1 -S "${frame_address_source}"
            -o "${BINARY_DIR}/frame-address-optimized.s"
)
nyc_execute_ok(
  [=[host compiler could not link optimized frame-address output\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${frame_address_harness}"
            "${BINARY_DIR}/frame-address-optimized.s" -o
            "${BINARY_DIR}/frame-address-optimized-host"
)
nyc_execute_ok(
  [=[optimized NYC frame-address output returned an invalid frame address\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/frame-address-optimized-host"
)

set(linux_type_source "${NY_C_CORPUS_linux-type-regressions.c}")

nyc_execute_ok(
  [=[Linux type frontend regression compilation failed\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -c "${linux_type_source}"
            -o "${BINARY_DIR}/linux-type-regressions.o"
)
set(linux_type_harness "${NY_C_CORPUS_linux-type-regressions-main.c}")

nyc_execute_ok(
  [=[host compiler could not link Linux type regressions\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${linux_type_harness}"
            "${BINARY_DIR}/linux-type-regressions.o" -o
            "${BINARY_DIR}/linux-type-regressions-host"
)
nyc_execute_ok(
  [=[Linux frontend regression executable returned wrong results\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/linux-type-regressions-host"
)
set(get_user_asm_source "${NY_C_CORPUS_get-user-inline-asm.c}")
nyc_execute_ok(
  [=[get_user inline assembly lowering failed\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -S "${get_user_asm_source}"
            -o "${BINARY_DIR}/get-user-inline-asm.s"
)
nyc_execute_ok(
  [=[get_user inline assembly was not accepted by assembler\n${stdout}${stderr}]=]
  COMMAND "${CC}" -c "${BINARY_DIR}/get-user-inline-asm.s"
            -o "${BINARY_DIR}/get-user-inline-asm.o"
)
set(function_alias_source "${NY_C_CORPUS_function-alias.c}")

nyc_execute_ok(
  [=[nyc failed to emit a GNU function alias\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -S "${function_alias_source}"
            -o "${BINARY_DIR}/function-alias.s"
)
nyc_execute_ok(
  [=[host assembler/linker rejected GNU function alias output\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/function-alias.s"
            -o "${BINARY_DIR}/function-alias"
)
nyc_execute_ok(
  [=[GNU function alias returned an incorrect result\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/function-alias"
)
set(asm_readwrite_memory_source
    "${NY_C_CORPUS_asm-readwrite-memory.c}")

nyc_execute_ok(
  [=[nyc failed to compile read-write register/memory asm\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -S "${asm_readwrite_memory_source}"
            -o "${BINARY_DIR}/asm-readwrite-memory.s"
)
set(asm_readwrite_memory_harness
    "${NY_C_CORPUS_asm-readwrite-memory-main.c}")

nyc_execute_ok(
  [=[host assembler/linker rejected read-write register/memory asm\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${asm_readwrite_memory_harness}"
            "${BINARY_DIR}/asm-readwrite-memory.s"
            -o "${BINARY_DIR}/asm-readwrite-memory"
)
nyc_execute_ok(
  [=[read-write register/memory asm runtime regression failed\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/asm-readwrite-memory"
)
set(asm_cmpxchg_source "${NY_C_CORPUS_asm-cmpxchg.c}")

nyc_execute_ok(
  [=[nyc failed to compile compare-exchange asm\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -S "${asm_cmpxchg_source}"
            -o "${BINARY_DIR}/asm-cmpxchg.s"
)
set(asm_cmpxchg_harness "${NY_C_CORPUS_asm-cmpxchg-main.c}")

nyc_execute_ok(
  [=[host assembler/linker rejected compare-exchange asm\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${asm_cmpxchg_harness}" "${BINARY_DIR}/asm-cmpxchg.s"
            -o "${BINARY_DIR}/asm-cmpxchg"
)
nyc_execute_ok(
  [=[compare-exchange asm runtime regression failed\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/asm-cmpxchg"
)
set(sizeof_unparenthesized_source "${NY_C_CORPUS_sizeof-unparenthesized.c}")

nyc_execute_ok(
  [=[nyc failed to compile unparenthesized sizeof expression\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -S "${sizeof_unparenthesized_source}"
            -o "${BINARY_DIR}/sizeof-unparenthesized.s"
)
nyc_execute_ok(
  [=[host assembler/linker rejected unparenthesized sizeof expression\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/sizeof-unparenthesized.s"
            -o "${BINARY_DIR}/sizeof-unparenthesized"
)
nyc_execute_ok(
  [=[unparenthesized sizeof expression runtime regression failed\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/sizeof-unparenthesized"
)
set(asm_sub_cce_source "${NY_C_CORPUS_asm-sub-cce.c}")

nyc_execute_ok(
  [=[nyc failed to compile er/memory condition asm\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -S "${asm_sub_cce_source}"
            -o "${BINARY_DIR}/asm-sub-cce.s"
)
set(asm_sub_cce_harness "${NY_C_CORPUS_asm-sub-cce-main.c}")

nyc_execute_ok(
  [=[host assembler/linker rejected er/memory condition asm\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${asm_sub_cce_harness}" "${BINARY_DIR}/asm-sub-cce.s"
            -o "${BINARY_DIR}/asm-sub-cce"
)
nyc_execute_ok(
  [=[er/memory condition asm runtime regression failed\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/asm-sub-cce"
)
set(asm_external_memory_source "${NY_C_CORPUS_asm-external-memory.c}")

nyc_execute_ok(
  [=[nyc failed to emit an external global memory asm input\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -S "${asm_external_memory_source}"
            -o "${BINARY_DIR}/asm-external-memory.s"
)
set(asm_external_memory_harness
    "${NY_C_CORPUS_asm-external-memory-main.c}")

nyc_execute_ok(
  [=[host assembler/linker rejected external memory asm input\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${asm_external_memory_harness}"
            "${BINARY_DIR}/asm-external-memory.s"
            -o "${BINARY_DIR}/asm-external-memory"
)
nyc_execute_ok(
  [=[external global memory asm input returned an incorrect value\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/asm-external-memory"
)
set(shadowed-function-pointer-source
    "${NY_C_CORPUS_shadowed-function-pointer.c}")

nyc_execute_ok(
  [=[nyc failed to preserve shadowed function and aggregate pointer types\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -S "${shadowed-function-pointer-source}"
            -o "${BINARY_DIR}/shadowed-function-pointer.s"
)
set(shadowed-function-pointer-harness
    "${NY_C_CORPUS_shadowed-function-pointer-main.c}")

nyc_execute_ok(
  [=[host compiler could not link shadowed function pointer test\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${shadowed-function-pointer-harness}"
            "${BINARY_DIR}/shadowed-function-pointer.s"
            -o "${BINARY_DIR}/shadowed-function-pointer"
)
nyc_execute_ok(
  [=[shadowed function pointer or typeof pointer returned an incorrect value\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/shadowed-function-pointer"
)
set(xchg-widths-source "${NY_C_CORPUS_xchg-widths.c}")

nyc_execute_ok(
  [=[nyc could not lower 8/16/32/64-bit xchg asm\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -S "${xchg-widths-source}"
            -o "${BINARY_DIR}/xchg-widths.s"
)
nyc_execute_ok(
  [=[external assembler rejected NYC xchg width output\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/xchg-widths.s"
            -o "${BINARY_DIR}/xchg-widths"
)
nyc_execute_ok(
  [=[NYC xchg width lowering returned or stored incorrect values\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/xchg-widths"
)
set(get-user-asm-source "${NY_C_CORPUS_get-user-asm.c}")

nyc_execute_ok(
  [=[nyc failed to lower get_user helper inline assembly\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -S "${get-user-asm-source}"
            -o "${BINARY_DIR}/get-user-asm.s"
)
set(get-user-asm-stub "${NY_C_CORPUS_get-user-asm-stub.s}")

set(get-user-asm-harness "${NY_C_CORPUS_get-user-asm-main.c}")

nyc_execute_ok(
  [=[host compiler could not link get_user helper inline asm\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/get-user-asm.s"
            "${get-user-asm-stub}" "${get-user-asm-harness}"
            -o "${BINARY_DIR}/get-user-asm"
)
nyc_execute_ok(
  [=[get_user helper outputs were not preserved through the call\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/get-user-asm"
)
set(irq_stack_asm "${BINARY_DIR}/irq-stack-boundaries.s")
nyc_execute_output("IRQ and plain stack-call lowering failed" "${irq_stack_asm}"
  COMMAND "${NYC}" -S "${NY_C_CORPUS_irq-stack-boundaries.c}"
    -o "${irq_stack_asm}")
file(READ "${irq_stack_asm}" irq_stack_text)
string(REGEX MATCHALL "call[ \t]+(irq_enter_rcu|handler|irq_exit_rcu)\n"
  irq_stack_calls "${irq_stack_text}")
string(REGEX REPLACE "call[ \t]+|\n" "" irq_stack_calls "${irq_stack_calls}")
if(NOT irq_stack_calls STREQUAL "irq_enter_rcu;handler;irq_exit_rcu;handler")
  message(FATAL_ERROR "incorrect IRQ stack call order: ${irq_stack_calls}")
endif()

set(put-user-asm-source "${NY_C_CORPUS_put-user-asm.c}")

nyc_execute_ok(
  [=[nyc failed to lower put_user helper inline assembly\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -S "${put-user-asm-source}"
            -o "${BINARY_DIR}/put-user-asm.s"
)
set(put-user-asm-stub "${NY_C_CORPUS_put-user-asm-stub.s}")

set(put-user-asm-harness "${NY_C_CORPUS_put-user-asm-main.c}")

nyc_execute_ok(
  [=[host compiler could not link put_user helper inline asm\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/put-user-asm.s"
            "${put-user-asm-stub}" "${put-user-asm-harness}"
            -o "${BINARY_DIR}/put-user-asm"
)
nyc_execute_ok(
  [=[put_user helper arguments or return value were not preserved\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/put-user-asm"
)
set(bsr-asm-source "${NY_C_CORPUS_bsr-asm.c}")

nyc_execute_ok(
  [=[nyc failed to lower read-write bsr inline assembly\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -S "${bsr-asm-source}" -o "${BINARY_DIR}/bsr-asm.s"
)
set(bsr-asm-harness "${NY_C_CORPUS_bsr-asm-main.c}")

nyc_execute_ok(
  [=[host compiler could not link bsr inline asm regression\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/bsr-asm.s" "${bsr-asm-harness}"
            -o "${BINARY_DIR}/bsr-asm"
)
nyc_execute_ok(
  [=[bsr inline asm returned incorrect zero or bit-index results\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/bsr-asm"
)
set(hweight-asm-source "${NY_C_CORPUS_hweight-asm.c}")

nyc_execute_ok(
  [=[nyc failed to lower read-write hweight inline assembly\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -S "${hweight-asm-source}" -o "${BINARY_DIR}/hweight-asm.s"
)
set(hweight-asm-harness "${NY_C_CORPUS_hweight-asm-main.c}")

nyc_execute_ok(
  [=[host compiler could not link hweight inline asm regression\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/hweight-asm.s" "${hweight-asm-harness}"
            -o "${BINARY_DIR}/hweight-asm"
)
nyc_execute_ok(
  [=[hweight inline asm returned incorrect counts\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/hweight-asm"
)
set(cpuid-asm-source "${NY_C_CORPUS_cpuid-asm.c}")
set(cpuid-asm-harness "${NY_C_CORPUS_cpuid-asm-main.c}")
foreach(route IN ITEMS S c)
if(route STREQUAL "S")
  set(cpuid_artifact "${BINARY_DIR}/cpuid-asm.s")
else()
  set(cpuid_artifact "${BINARY_DIR}/cpuid-asm.o")
endif()
nyc_execute_ok(
  [=[nyc failed to lower CPUID inline assembly\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -O2 "-${route}" "${cpuid-asm-source}" -o "${cpuid_artifact}"
)
nyc_execute_ok(
  [=[host compiler could not link CPUID inline asm regression\n${stdout}${stderr}]=]
  COMMAND "${CC}" -O2 "${cpuid_artifact}" "${cpuid-asm-harness}"
            -o "${BINARY_DIR}/cpuid-asm-${route}"
)
nyc_execute_ok(
  [=[NYC CPUID outputs differed from host CPUID\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/cpuid-asm-${route}"
)
endforeach()
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
  foreach(probe IN ITEMS asm-fixed-r9 asm-segment-memory
                         segment-aggregate-pointer-load)
    foreach(route IN ITEMS S c)
      if(route STREQUAL "S")
        set(asm_artifact "${BINARY_DIR}/${probe}.s")
      else()
        set(asm_artifact "${BINARY_DIR}/${probe}.o")
      endif()
      nyc_execute_ok(
        [=[${probe} compilation failed (${status})\n${stdout}${stderr}]=]
        COMMAND "${NYC}" -O2 "-${route}"
                        "${NY_C_CORPUS_${probe}.c}" -o "${asm_artifact}"
      )
      nyc_execute_ok(
        [=[${probe} host harness link failed (${status})\n${stdout}${stderr}]=]
        COMMAND "${CC}" -O2
                        "${NY_C_CORPUS_${probe}-host.c}" "${asm_artifact}"
                        -o "${BINARY_DIR}/${probe}-${route}"
      )
      nyc_execute_ok(
        [=[${probe} runtime result failed (${status})\n${stdout}${stderr}]=]
        COMMAND "${BINARY_DIR}/${probe}-${route}"
      )
    endforeach()
  endforeach()
endif()
set(rdmsr-asm-source "${NY_C_CORPUS_rdmsr-asm.c}")

nyc_execute_ok(
  [=[nyc failed to lower rdmsr inline assembly\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -S "${rdmsr-asm-source}" -o "${BINARY_DIR}/rdmsr-asm.s"
)
nyc_execute_ok(
  [=[assembler rejected lowered rdmsr and exception table\n${stdout}${stderr}]=]
  COMMAND "${CC}" -m64 -c "${BINARY_DIR}/rdmsr-asm.s"
            -o "${BINARY_DIR}/rdmsr-asm.o"
)
execute_process(
  COMMAND "${OBJDUMP}" -d "${BINARY_DIR}/rdmsr-asm.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0 OR NOT stdout MATCHES "[ \t]rdmsr" OR
   NOT stdout MATCHES "[ \t]wrmsr" OR NOT stdout MATCHES "[ \t]rdpmc")
  message(FATAL_ERROR
    "lowered MSR/PMC object omitted a required privileged instruction\n${stdout}${stderr}")
endif()
set(local-add-asm-source "${NY_C_CORPUS_local-add-asm.c}")

nyc_execute_ok(
  [=[nyc failed to lower read-write memory add asm\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -S "${local-add-asm-source}"
            -o "${BINARY_DIR}/local-add-asm.s"
)
set(local-add-asm-harness "${NY_C_CORPUS_local-add-asm-main.c}")

nyc_execute_ok(
  [=[host compiler could not link read-write memory add asm\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/local-add-asm.s"
            "${local-add-asm-harness}" -o "${BINARY_DIR}/local-add-asm"
)
nyc_execute_ok(
  [=[read-write memory add asm computed an incorrect counter\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/local-add-asm"
)
set(mulq_addcarry_source "${NY_C_CORPUS_asm-mulq-addcarry.c}")
nyc_execute_ok(
  [=[nyc failed to lower mulq/add-carry assembly\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -S "${mulq_addcarry_source}"
            -o "${BINARY_DIR}/asm-mulq-addcarry.s"
)
nyc_execute_ok(
  [=[host compiler could not link mulq/add-carry fixture\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/asm-mulq-addcarry.s"
            "${NY_C_CORPUS_asm-mulq-addcarry-main.c}"
            -o "${BINARY_DIR}/asm-mulq-addcarry"
)
nyc_execute_ok(
  [=[mulq/add-carry fixture returned an incorrect result\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/asm-mulq-addcarry"
)
set(typedef_scope_source "${NY_C_CORPUS_typedef-scope-index.c}")

nyc_execute_ok(
  [=[nyc failed to parse typedefs across a block scope\n${stdout}${stderr}]=]
  COMMAND "${NYC}" -S "${typedef_scope_source}"
            -o "${BINARY_DIR}/typedef-scope-index.s"
)
nyc_execute_ok(
  [=[host compiler could not link typedef scope regression\n${stdout}${stderr}]=]
  COMMAND "${CC}" "${BINARY_DIR}/typedef-scope-index.s"
            -o "${BINARY_DIR}/typedef-scope-index"
)
nyc_execute_ok(
  [=[typedef lookup returned the wrong alias after scope exit\n${stdout}${stderr}]=]
  COMMAND "${BINARY_DIR}/typedef-scope-index"
)

nyc_compile_obj_run(signed-pow2-31 "${NY_C_CORPUS_signed-pow2-31.c}"
  NYC_FLAGS -O3 HOST_SOURCES "${NY_C_CORPUS_signed-pow2-31-host.c}")

nyc_compile_obj_run(machine-regressions "${NY_C_CORPUS_machine-regressions.c}"
  NYC_FLAGS -O3)
nyc_compile_asm_run(machine-regressions-asm
  "${NY_C_CORPUS_machine-regressions.c}" NYC_FLAGS -O3)

nyc_compile_obj_run(machine-float-reduce
  "${NY_C_CORPUS_machine-float-reduce.c}" NYC_FLAGS -O3 TIMEOUT 30
  OUTPUT_MATCH "checksum=15157.0 -148.0 100.0 -520.0 10000000 100.0")

foreach(machine_volatile_opt IN ITEMS -O0 -O3)
  nyc_compile_obj_run(machine-volatile-wait
    "${NY_C_CORPUS_machine-volatile-wait.c}"
    NYC_FLAGS "${machine_volatile_opt}" TIMEOUT 10 OUTPUT_MATCH "checksum=1")
endforeach()

foreach(volatile_mode IN ITEMS -m16 -m64)
  nyc_execute_output("volatile global array compilation failed"
    "${BINARY_DIR}/volatile-global-array${volatile_mode}.s"
    COMMAND "${CMAKE_COMMAND}" -E env NYTRIX_DUMP_FINAL_NYIR=1
      "${NYC}" "${volatile_mode}" -D_SETUP -Os -S
      "${NY_C_CORPUS_volatile-global-array.c}"
      -o "${BINARY_DIR}/volatile-global-array${volatile_mode}.s")
  string(REGEX MATCHALL "ld\\.i64\\.i32 " volatile_loads "${stderr}")
  list(LENGTH volatile_loads volatile_load_count)
  string(REGEX MATCHALL "[ls][dt]\\.i64\\.u16 " volatile_halfword_accesses
    "${stderr}")
  list(LENGTH volatile_halfword_accesses volatile_halfword_count)
  if(NOT volatile_load_count EQUAL 4 OR NOT volatile_halfword_count EQUAL 2)
    message(FATAL_ERROR
      "volatile global arrays lost a read or used the wrong access width in ${volatile_mode}\n${stderr}")
  endif()
endforeach()
