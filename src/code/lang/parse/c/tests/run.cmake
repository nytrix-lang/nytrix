if(NOT EXISTS "${NYC}")
  message(FATAL_ERROR "nyc compiler does not exist: ${NYC}")
endif()
if(NOT EXISTS "${CC}")
  message(FATAL_ERROR "C reference compiler does not exist: ${CC}")
endif()

file(MAKE_DIRECTORY "${BINARY_DIR}")

# Source fixtures live in fixtures/; this script owns build outputs and header copies.

# Frontend semantics, declarations, and aggregate layout.
foreach(probe IN ITEMS unevaluated-metadata abstract-callback-parameters
                       static-inline-boundaries driver-pthread
                       void-pointer-increment cleanup-type-attribute
                       atomic-memory-cmpxchg-small dead-inline-asm
                       unreachable-asm asm-add-carry32 asm-add-carry64
                       asm-repeat-stos-widths asm-mull-edx asm-delay-loop
                       asm-mwaitx asm-mulq-positional asm-usercopy-movnti
                       global-incomplete-char-array-rows
                       global-incomplete-integer-array-rows
                       typeof-cast-array-member asm-memory-address
                       asm-memory-output
                       asm-cmpxchg16b
                       asm-ip-fast-csum
                       futex-atomic-exception-table-asm
                       aggregate-pointer-self-initializer
                       volatile-pointer-increment
                       aggregate-sizeof-global-scalar-array
                       sizeof-empty-global-array
                       function-pointer-byte-arithmetic
                       generic-function-pointer-call
                       generic-aggregate-pointer-call
                       computed-goto-label-table
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
                       typeof-const-aggregate-pointer
                       static-function-name
                       typeof-cast-binary-precedence
                       void-global-array-member-call
                       aggregate-array-rank4
                       static-pointer-array-range-designator
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
                       aggregate-char-matrix-string-rows
                       conditional-in-function-macro-arguments
                       local-aggregate-pointer-array pointer-attributes
                       aligned-local-aggregate enum-wide-alias
                       global-array-boundaries aggregate-large-array
                       sizeof-member-array offsetof-dynamic-type
                       typeof-incomplete-aggregate-pointer-sizeof
                       pointer-to-bool-assignment
                       aggregate-trailing-attribute
                       function-pointer-aggregate-return
                       typedef-function-signature
                       driver-no-builtin)
  set(probe_flags)
  if(probe STREQUAL "aggregate-anonymous-member-size")
    list(APPEND probe_flags -fms-extensions)
  endif()
  if(probe STREQUAL "driver-pthread")
    list(APPEND probe_flags -pthread)
  endif()
  execute_process(
    COMMAND "${NYC}" -O3 -S ${probe_flags} "${SOURCE_DIR}/fixtures/${probe}.c"
            -o "${BINARY_DIR}/${probe}.s"
    RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
  if(NOT status EQUAL 0)
    message(FATAL_ERROR "nyc ${probe} compilation failed\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${CC}" ${probe_flags} "${BINARY_DIR}/${probe}.s"
            -o "${BINARY_DIR}/${probe}"
    RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
  if(NOT status EQUAL 0)
    message(FATAL_ERROR "${probe} link failed\n${stdout}${stderr}")
  endif()
  execute_process(COMMAND "${BINARY_DIR}/${probe}"
    RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
  if(NOT status EQUAL 0)
    message(FATAL_ERROR "${probe} returned an incorrect result\n${stdout}${stderr}")
  endif()
endforeach()
execute_process(
  COMMAND "${NYC}" -fno-builtin -S
          "${SOURCE_DIR}/fixtures/driver-no-builtin.c"
          -o "${BINARY_DIR}/driver-no-builtin.s"
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(status EQUAL 0 OR NOT stderr MATCHES "must be declared")
  message(FATAL_ERROR
    "nyc -fno-builtin did not disable implicit builtin lookup\n${stdout}${stderr}")
endif()
set(gnu_extern_inline_source
    "${SOURCE_DIR}/fixtures/gnu-extern-inline.c")
set(gnu_extern_inline_definition
    "${SOURCE_DIR}/fixtures/gnu-extern-inline-definition.c")
execute_process(
  COMMAND "${NYC}" -O3 -S "${gnu_extern_inline_source}"
          -o "${BINARY_DIR}/gnu-extern-inline.s"
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "nyc GNU extern-inline compilation failed\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/gnu-extern-inline.s"
          "${gnu_extern_inline_definition}" -o "${BINARY_DIR}/gnu-extern-inline"
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "GNU extern-inline link failed\n${stdout}${stderr}")
endif()
execute_process(COMMAND "${BINARY_DIR}/gnu-extern-inline"
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "GNU extern-inline returned an incorrect result\n${stdout}${stderr}")
endif()
set(static_assert_source "${SOURCE_DIR}/fixtures/static-assert.c")
set(static_assert_false_source "${SOURCE_DIR}/fixtures/static-assert-false.c")

set(ffs_source "${SOURCE_DIR}/fixtures/builtin-ffs.c")

execute_process(COMMAND "${NYC}" -c "${ffs_source}" -o "${BINARY_DIR}/builtin-ffs.o"
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "nyc __builtin_ffs compilation failed\n${stdout}${stderr}")
endif()
execute_process(COMMAND "${CC}" "${BINARY_DIR}/builtin-ffs.o" -o "${BINARY_DIR}/builtin-ffs"
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "__builtin_ffs link failed\n${stdout}${stderr}")
endif()
execute_process(COMMAND "${BINARY_DIR}/builtin-ffs" RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "__builtin_ffs returned incorrect results: ${status}")
endif()
set(aggregate_double_pointer_source "${SOURCE_DIR}/fixtures/aggregate-double-pointer-cast.c")

execute_process(COMMAND "${NYC}" -S "${aggregate_double_pointer_source}"
                -o "${BINARY_DIR}/aggregate-double-pointer-cast.s"
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "nyc aggregate double-pointer cast failed\n${stdout}${stderr}")
endif()
execute_process(COMMAND "${CC}" "${BINARY_DIR}/aggregate-double-pointer-cast.s"
                -o "${BINARY_DIR}/aggregate-double-pointer-cast"
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "aggregate double-pointer cast link failed\n${stdout}${stderr}")
endif()
execute_process(COMMAND "${BINARY_DIR}/aggregate-double-pointer-cast"
                RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "aggregate double-pointer cast returned incorrect result: ${status}")
endif()
set(aggregate_triple_pointer_source "${SOURCE_DIR}/fixtures/aggregate-triple-pointer.c")

execute_process(COMMAND "${NYC}" -S "${aggregate_triple_pointer_source}"
                -o "${BINARY_DIR}/aggregate-triple-pointer.s"
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "nyc aggregate triple-pointer compilation failed\n${stdout}${stderr}")
endif()
execute_process(COMMAND "${CC}" "${BINARY_DIR}/aggregate-triple-pointer.s"
                -o "${BINARY_DIR}/aggregate-triple-pointer"
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "aggregate triple-pointer link failed\n${stdout}${stderr}")
endif()
execute_process(COMMAND "${BINARY_DIR}/aggregate-triple-pointer"
                RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "aggregate triple-pointer returned incorrect result: ${status}")
endif()
set(runtime_const_pointer_source "${SOURCE_DIR}/fixtures/runtime-const-pointer-inline-asm.c")

execute_process(COMMAND "${NYC}" -S "${runtime_const_pointer_source}"
                -o "${BINARY_DIR}/runtime-const-pointer-inline-asm.s"
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "nyc runtime-constant pointer compilation failed\n${stdout}${stderr}")
endif()
execute_process(COMMAND "${CC}" "${BINARY_DIR}/runtime-const-pointer-inline-asm.s"
                -o "${BINARY_DIR}/runtime-const-pointer-inline-asm"
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "runtime-constant pointer link failed\n${stdout}${stderr}")
endif()
execute_process(COMMAND "${BINARY_DIR}/runtime-const-pointer-inline-asm"
                RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "runtime-constant pointer returned incorrect result: ${status}")
endif()
set(user_load_goto_source "${SOURCE_DIR}/fixtures/linux-user-load-goto.c")

execute_process(COMMAND "${NYC}" -S "${user_load_goto_source}"
                -o "${BINARY_DIR}/linux-user-load-goto.s"
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "nyc user-load asm-goto compilation failed\n${stdout}${stderr}")
endif()
execute_process(COMMAND "${CC}" "${BINARY_DIR}/linux-user-load-goto.s"
                -o "${BINARY_DIR}/linux-user-load-goto"
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "user-load asm-goto link failed\n${stdout}${stderr}")
endif()
execute_process(COMMAND "${BINARY_DIR}/linux-user-load-goto"
                RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "user-load asm-goto returned incorrect results: ${status}")
endif()
set(user_load_register_extable_source
    "${SOURCE_DIR}/fixtures/linux-user-load-register-extable.c")

execute_process(COMMAND "${NYC}" -S "${user_load_register_extable_source}"
                -o "${BINARY_DIR}/linux-user-load-register-extable.s"
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "nyc register-output user-load compilation failed\n${stdout}${stderr}")
endif()
execute_process(COMMAND "${CC}" "${BINARY_DIR}/linux-user-load-register-extable.s"
                -o "${BINARY_DIR}/linux-user-load-register-extable"
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "register-output user-load link failed\n${stdout}${stderr}")
endif()
execute_process(COMMAND "${BINARY_DIR}/linux-user-load-register-extable"
                RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "register-output user-load returned incorrect results: ${status}")
endif()
set(atomic_memory_orq_source "${SOURCE_DIR}/fixtures/atomic-memory-orq-er.c")

execute_process(COMMAND "${NYC}" -O3 -S "${atomic_memory_orq_source}"
                -o "${BINARY_DIR}/atomic-memory-orq-er.s"
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "nyc qword atomic OR compilation failed\n${stdout}${stderr}")
endif()
execute_process(COMMAND "${CC}" "${BINARY_DIR}/atomic-memory-orq-er.s"
                -o "${BINARY_DIR}/atomic-memory-orq-er"
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "qword atomic OR link failed\n${stdout}${stderr}")
endif()
execute_process(COMMAND "${BINARY_DIR}/atomic-memory-orq-er"
                RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "qword atomic OR returned incorrect results: ${status}")
endif()
set(variadic_source "${SOURCE_DIR}/fixtures/variadic-va-args.c")

execute_process(COMMAND "${NYC}" -S "${variadic_source}"
                -o "${BINARY_DIR}/variadic-va-args.s"
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "nyc variadic va_arg compilation failed\n${stdout}${stderr}")
endif()
execute_process(COMMAND "${CC}" "${BINARY_DIR}/variadic-va-args.s"
                -o "${BINARY_DIR}/variadic-va-args"
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "variadic va_arg link failed\n${stdout}${stderr}")
endif()
execute_process(COMMAND "${BINARY_DIR}/variadic-va-args" RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "variadic va_arg returned incorrect result: ${status}")
endif()
set(aggregate_variadic_source
    "${SOURCE_DIR}/fixtures/aggregate-variadic-call.c")
execute_process(
  COMMAND "${NYC}" -S "${aggregate_variadic_source}"
          -o "${BINARY_DIR}/aggregate-variadic-call.s"
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "nyc aggregate variadic call compilation failed\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/aggregate-variadic-call.s"
          "${SOURCE_DIR}/fixtures/aggregate-variadic-host.c"
          -o "${BINARY_DIR}/aggregate-variadic-call"
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "aggregate variadic call link failed\n${stdout}${stderr}")
endif()
execute_process(COMMAND "${BINARY_DIR}/aggregate-variadic-call"
                RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "aggregate variadic call returned incorrect result: ${status}")
endif()
set(aggregate_va_arg_source
    "${SOURCE_DIR}/fixtures/aggregate-variadic-va-arg.c")
execute_process(
  COMMAND "${NYC}" -S "${aggregate_va_arg_source}"
          -o "${BINARY_DIR}/aggregate-variadic-va-arg.s"
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "nyc aggregate va_arg compilation failed\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/aggregate-variadic-va-arg.s"
          -o "${BINARY_DIR}/aggregate-variadic-va-arg"
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "aggregate va_arg link failed\n${stdout}${stderr}")
endif()
execute_process(COMMAND "${BINARY_DIR}/aggregate-variadic-va-arg"
                RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "aggregate va_arg returned incorrect result: ${status}")
endif()
set(percpu_source "${SOURCE_DIR}/fixtures/percpu-raw-pointer.c")
execute_process(
  COMMAND "${NYC}" -S "${percpu_source}" -o "${BINARY_DIR}/percpu-raw-pointer.s"
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "nyc per-CPU pointer compilation failed\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/percpu-raw-pointer.s"
          -o "${BINARY_DIR}/percpu-raw-pointer"
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "per-CPU pointer link failed\n${stdout}${stderr}")
endif()
execute_process(COMMAND "${BINARY_DIR}/percpu-raw-pointer" RESULT_VARIABLE status)
if(NOT status EQUAL 1)
  message(FATAL_ERROR "per-CPU pointer update returned ${status}, expected one")
endif()
set(function_typedef_source "${SOURCE_DIR}/fixtures/function-typedef-definition.c")

execute_process(COMMAND "${NYC}" -S "${function_typedef_source}"
                -o "${BINARY_DIR}/function-typedef-definition.s"
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "nyc function typedef definition compilation failed\n${stdout}${stderr}")
endif()
execute_process(COMMAND "${CC}" "${BINARY_DIR}/function-typedef-definition.s"
                -o "${BINARY_DIR}/function-typedef-definition"
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "function typedef definition link failed\n${stdout}${stderr}")
endif()
execute_process(COMMAND "${BINARY_DIR}/function-typedef-definition"
                RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "function typedef definition returned incorrect result: ${status}")
endif()
execute_process(
  COMMAND "${NYC}" -c -std=gnu11 "${static_assert_source}"
          -o "${BINARY_DIR}/static-assert.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "nyc valid _Static_assert failed\n${stdout}${stderr}")
endif()
set(floatn_source "${SOURCE_DIR}/fixtures/floatn.c")
set(global_char_array_source "${SOURCE_DIR}/fixtures/global-char-array-store.c")

execute_process(COMMAND "${NYC}" -c "${global_char_array_source}" -o "${BINARY_DIR}/global-char-array-store.o"
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "nyc global character array store failed\n${stdout}${stderr}")
endif()
execute_process(COMMAND "${CC}" "${BINARY_DIR}/global-char-array-store.o" -o "${BINARY_DIR}/global-char-array-store"
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "global character array store link failed\n${stdout}${stderr}")
endif()
execute_process(COMMAND "${BINARY_DIR}/global-char-array-store" RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "global character array store produced incorrect output: ${status}")
endif()
set(global_multidimensional_scalar_array_source
    "${SOURCE_DIR}/fixtures/global-multidimensional-scalar-array.c")
execute_process(
  COMMAND "${NYC}" -c "${global_multidimensional_scalar_array_source}"
          -o "${BINARY_DIR}/global-multidimensional-scalar-array.o"
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc multidimensional file-scope scalar array initializer failed\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/global-multidimensional-scalar-array.o"
          -o "${BINARY_DIR}/global-multidimensional-scalar-array"
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "multidimensional scalar array initializer link failed\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/global-multidimensional-scalar-array"
  RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "multidimensional scalar array initializer returned incorrect result: ${status}")
endif()
set(floatn_host "${SOURCE_DIR}/fixtures/floatn-host.c")

execute_process(COMMAND "${NYC}" -c "${floatn_source}" -o "${BINARY_DIR}/floatn.o"
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "nyc _FloatN compilation failed\n${stdout}${stderr}")
endif()
execute_process(COMMAND "${CC}" "${floatn_host}" "${BINARY_DIR}/floatn.o" -o "${BINARY_DIR}/floatn"
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "_FloatN ABI link failed\n${stdout}${stderr}")
endif()
execute_process(COMMAND "${BINARY_DIR}/floatn" RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "_FloatN ABI runtime check failed: ${status}")
endif()
set(ctzll_source "${SOURCE_DIR}/fixtures/builtin-ctzll.c")

execute_process(
  COMMAND "${NYC}" -S "${ctzll_source}" -o "${BINARY_DIR}/builtin-ctzll.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc failed to compile __builtin_ctzll calls\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/builtin-ctzll.s" -o
          "${BINARY_DIR}/builtin-ctzll"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host compiler could not link __builtin_ctzll test\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/builtin-ctzll"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "__builtin_ctzll returned an incorrect bit index\n${stdout}${stderr}")
endif()
set(pointer_array_designator_source
    "${SOURCE_DIR}/fixtures/pointer-array-designators.c")

execute_process(
  COMMAND "${NYC}" -S "${pointer_array_designator_source}"
          -o "${BINARY_DIR}/pointer-array-designators.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc failed to compile designated string-pointer array\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/pointer-array-designators.s" -o
          "${BINARY_DIR}/pointer-array-designators"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host compiler could not link designated string-pointer array test\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/pointer-array-designators"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "designated string-pointer array values mismatch\n${stdout}${stderr}")
endif()
set(aggregate_array_designator_source
    "${SOURCE_DIR}/fixtures/aggregate-array-designators.c")

execute_process(
  COMMAND "${NYC}" -S "${aggregate_array_designator_source}"
          -o "${BINARY_DIR}/aggregate-array-designators.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc failed to infer designated aggregate-array bound\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/aggregate-array-designators.s" -o
          "${BINARY_DIR}/aggregate-array-designators"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host compiler could not link aggregate-array designator test\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/aggregate-array-designators"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "aggregate-array designator values mismatch\n${stdout}${stderr}")
endif()
set(empty_inferred_aggregate_array_source
    "${SOURCE_DIR}/fixtures/empty-inferred-aggregate-array.c")
execute_process(
  COMMAND "${NYC}" -S "${empty_inferred_aggregate_array_source}"
          -o "${BINARY_DIR}/empty-inferred-aggregate-array.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc failed to compile an empty inferred aggregate array\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/empty-inferred-aggregate-array.s" -o
          "${BINARY_DIR}/empty-inferred-aggregate-array"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host compiler could not link empty inferred aggregate array test\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/empty-inferred-aggregate-array"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "empty inferred aggregate array has nonzero size\n${stdout}${stderr}")
endif()
set(trailing_overlap_source "${SOURCE_DIR}/fixtures/trailing-overlap-assert.c")

execute_process(
  COMMAND "${NYC}" -c -std=gnu11 "${trailing_overlap_source}"
          -o "${BINARY_DIR}/trailing-overlap-assert.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc rejected the Linux trailing-overlap _Static_assert\n${stdout}${stderr}")
endif()

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
set(offsetof_source "${SOURCE_DIR}/fixtures/builtin-stddef.c")

execute_process(
  COMMAND "${NYC}" -c -std=gnu11 "${offsetof_source}"
          -o "${BINARY_DIR}/builtin-stddef.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc failed to evaluate a constant offsetof expression\n${stdout}${stderr}")
endif()

set(function_attribute_source "${SOURCE_DIR}/fixtures/function-attribute.c")

set(local_attribute_source "${SOURCE_DIR}/fixtures/local-attribute.c")

execute_process(
  COMMAND "${NYC}" -c "${local_attribute_source}"
          -o "${BINARY_DIR}/local-attribute.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc failed to parse a supported block-scope unused attribute\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/local-attribute.o" -o
          "${BINARY_DIR}/local-attribute"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host compiler could not link local-attribute regression\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/local-attribute"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "local attribute regression failed at runtime\n${stdout}${stderr}")
endif()

execute_process(
  COMMAND "${NYC}" -c -std=gnu11 "${function_attribute_source}"
          -o "${BINARY_DIR}/function-attribute.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc failed to parse a function return attribute\n${stdout}${stderr}")
endif()
set(plan9_member_source "${SOURCE_DIR}/fixtures/plan9-anonymous-member.c")

execute_process(
  COMMAND "${NYC}" -fplan9-extensions -c -std=gnu11 "${plan9_member_source}"
          -o "${BINARY_DIR}/plan9-anonymous-member.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc failed Plan 9 anonymous aggregate layout assertions\n${stdout}${stderr}")
endif()
set(generic_probe_source "${SOURCE_DIR}/fixtures/generic-ternary-probe.c")

execute_process(
  COMMAND "${NYC}" -c -std=gnu11 "${generic_probe_source}"
          -o "${BINARY_DIR}/generic-ternary-probe.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc failed to compile a ternary-controlled _Generic selection\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" -no-pie "${BINARY_DIR}/generic-ternary-probe.o"
          -o "${BINARY_DIR}/generic-ternary-probe"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "reference linker rejected the ternary-controlled _Generic probe\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/generic-ternary-probe"
  RESULT_VARIABLE status
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "ternary-controlled _Generic selection returned the wrong pointer")
endif()
set(global_after_static_source "${SOURCE_DIR}/fixtures/global-after-static.c")

set(global_after_static_consumer "${SOURCE_DIR}/fixtures/global-after-static-consumer.c")

foreach(source IN ITEMS "${global_after_static_source}"
                        "${global_after_static_consumer}")
  get_filename_component(stem "${source}" NAME_WE)
  execute_process(
    COMMAND "${NYC}" -c "${source}"
            -o "${BINARY_DIR}/${stem}.o"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "nyc failed to compile a cross-translation-unit global linkage probe\n${stdout}${stderr}")
  endif()
endforeach()
execute_process(
  COMMAND "${CC}" -no-pie "${BINARY_DIR}/global-after-static.o"
          "${BINARY_DIR}/global-after-static-consumer.o"
          -o "${BINARY_DIR}/global-after-static"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "file-scope global after a function-local static did not link externally\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/global-after-static"
  RESULT_VARIABLE status
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "external file-scope global following a local static returned the wrong value")
endif()
set(function_alias_call_source "${SOURCE_DIR}/fixtures/function-asm-alias-call.c")

execute_process(
  COMMAND "${NYC}" -c "${function_alias_call_source}"
          -o "${BINARY_DIR}/function-asm-alias-call.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc failed to compile a call through a function asm alias\n${stdout}${stderr}")
endif()
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
set(large_aggregate_source "${SOURCE_DIR}/fixtures/large-aggregate-fields.c")

foreach(field_index RANGE 0 512)

endforeach()

execute_process(
  COMMAND "${NYC}" -c "${large_aggregate_source}"
          -o "${BINARY_DIR}/large-aggregate-fields.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc rejected an aggregate with more than 512 fields\n${stdout}${stderr}")
endif()
set(function_array_source "${SOURCE_DIR}/fixtures/function-pointer-array.c")

execute_process(
  COMMAND "${NYC}" -c -std=gnu11 -falign-functions=16
          "${function_array_source}"
          -o "${BINARY_DIR}/function-pointer-array.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc failed to parse external function-pointer arrays\n${stdout}${stderr}")
endif()
set(omit_fp_source "${SOURCE_DIR}/fixtures/omit-frame-pointer.c")

execute_process(
  COMMAND "${NYC}" -c -fomit-frame-pointer "${omit_fp_source}"
          -o "${BINARY_DIR}/omit-frame-pointer.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc failed to compile -fomit-frame-pointer regression\n${stdout}${stderr}")
endif()
set(omit_fp_harness "${SOURCE_DIR}/fixtures/omit-frame-pointer-host.c")

execute_process(
  COMMAND "${CC}" -no-pie "${omit_fp_harness}"
          "${BINARY_DIR}/omit-frame-pointer.o" -o
          "${BINARY_DIR}/omit-frame-pointer-host"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "could not link -fomit-frame-pointer regression\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/omit-frame-pointer-host"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "failed -fomit-frame-pointer stack/call regression\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${NYC}" -fomit-frame-pointer "${function_array_source}"
          "${BINARY_DIR}/single-function-omit-fp.o" c_nyc_initcall_ranges_equal
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(status EQUAL 0 OR
   NOT stderr MATCHES "requires whole-unit object mode")
  message(FATAL_ERROR
    "nyc did not reject unsupported single-function -fomit-frame-pointer mode\n${stdout}${stderr}")
endif()
set(retpoline_source "${SOURCE_DIR}/fixtures/retpoline-indirect-call.c")

execute_process(
  COMMAND "${NYC}" -c -fomit-frame-pointer
          -mindirect-branch=thunk-extern -mindirect-branch-register
          "${retpoline_source}" -o "${BINARY_DIR}/retpoline-indirect-call.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc failed to compile retpoline indirect-call regression\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${NYC}" -Werror -mindirect-branch=thunk-extern
          -mindirect-branch-register -c -x c /dev/null
          -o "${BINARY_DIR}/empty-kbuild-probe.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc failed the empty Kbuild retpoline option probe\n${stdout}${stderr}")
endif()
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
set(retpoline_thunk "${SOURCE_DIR}/fixtures/retpoline-indirect-thunk.S")

set(retpoline_harness "${SOURCE_DIR}/fixtures/retpoline-indirect-call-host.c")

execute_process(
  COMMAND "${CC}" -no-pie "${retpoline_harness}" "${retpoline_thunk}"
          "${BINARY_DIR}/retpoline-indirect-call.o" -o
          "${BINARY_DIR}/retpoline-indirect-call-host"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "could not link retpoline indirect-call regression\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/retpoline-indirect-call-host"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "retpoline indirect-call regression failed\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${NYC}" -c -fomit-frame-pointer
          -mindirect-branch=thunk-inline -mindirect-branch-register
          "${retpoline_source}" -o "${BINARY_DIR}/retpoline-inline-call.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc failed to compile inline-retpoline regression\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" -no-pie "${retpoline_harness}"
          "${BINARY_DIR}/retpoline-inline-call.o" -o
          "${BINARY_DIR}/retpoline-inline-call-host"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "could not link inline-retpoline regression\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/retpoline-inline-call-host"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "inline-retpoline indirect-call regression failed\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${NYC}" -S -mindirect-branch=thunk-extern
          -mindirect-branch-register "${retpoline_source}" -o
          "${BINARY_DIR}/retpoline-indirect-call.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc failed to emit assembly with external retpoline calls\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" -no-pie "${retpoline_harness}" "${retpoline_thunk}"
          "${BINARY_DIR}/retpoline-indirect-call.s" -o
          "${BINARY_DIR}/retpoline-indirect-call-assembly-host"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "could not link assembly-mode external-retpoline regression\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/retpoline-indirect-call-assembly-host"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "assembly-mode external-retpoline call failed\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${NYC}" -S -mindirect-branch=thunk-inline
          -mindirect-branch-register "${retpoline_source}" -o
          "${BINARY_DIR}/retpoline-inline-call.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc failed to emit assembly with inline retpolines\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" -no-pie "${retpoline_harness}"
          "${BINARY_DIR}/retpoline-inline-call.s" -o
          "${BINARY_DIR}/retpoline-inline-call-assembly-host"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "could not link assembly-mode inline-retpoline regression\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/retpoline-inline-call-assembly-host"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "assembly-mode inline-retpoline call failed\n${stdout}${stderr}")
endif()
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
set(pic_source "${SOURCE_DIR}/fixtures/pic-global-address.c")

set(pic_harness "${SOURCE_DIR}/fixtures/pic-harness.c")

foreach(pic_flag IN ITEMS -fPIC -fPIE)
  set(pic_object "${BINARY_DIR}/pic-${pic_flag}.o")
  execute_process(
    COMMAND "${NYC}" -c "${pic_flag}" "${pic_source}" -o "${pic_object}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR "nyc rejected ${pic_flag}\n${stdout}${stderr}")
  endif()
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
  execute_process(
    COMMAND "${CC}" -shared -o "${BINARY_DIR}/libpic-${pic_flag}.so"
            "${pic_object}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "host linker rejected NYC ${pic_flag} object as shared object\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${CC}" "${pic_harness}" "${pic_object}" -o
            "${BINARY_DIR}/pic-harness-${pic_flag}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "host linker rejected NYC ${pic_flag} executable\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${BINARY_DIR}/pic-harness-${pic_flag}"
    RESULT_VARIABLE status
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR "${pic_flag} PIC global/call runtime regression failed")
  endif()
endforeach()
set(overflow_source "${SOURCE_DIR}/fixtures/unsigned-overflow.c")

set(overflow_object "${BINARY_DIR}/unsigned-overflow.o")
execute_process(
  COMMAND "${NYC}" -c "${overflow_source}" -o "${overflow_object}"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "nyc rejected unsigned overflow builtins\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${overflow_object}" -o "${BINARY_DIR}/unsigned-overflow"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "host linker rejected NYC overflow object\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/unsigned-overflow"
  RESULT_VARIABLE status
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "unsigned overflow runtime regression failed: ${status}")
endif()
execute_process(
  COMMAND "${NYC}" -c -Os "${pic_source}" -o "${BINARY_DIR}/pic-os.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "nyc rejected -Os\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${pic_harness}" "${BINARY_DIR}/pic-os.o" -o
          "${BINARY_DIR}/pic-os-harness"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "host linker rejected NYC -Os object\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/pic-os-harness"
  RESULT_VARIABLE status
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "NYC -Os runtime regression failed")
endif()
if(READELF)
  execute_process(
    COMMAND "${READELF}" -SW "${BINARY_DIR}/function-pointer-array.o"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0 OR NOT stdout MATCHES "\\.text.*[ \t]16")
    message(FATAL_ERROR
      "NYC did not honor 16-byte function alignment\n${stdout}${stderr}")
  endif()
endif()
set(function_array_harness "${SOURCE_DIR}/fixtures/function-pointer-array-host.c")

execute_process(
  COMMAND "${CC}" "${function_array_harness}"
          "${BINARY_DIR}/function-pointer-array.o" -o
          "${BINARY_DIR}/function-pointer-array-host"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "reference compiler could not link external function-pointer arrays\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/function-pointer-array-host"
  RESULT_VARIABLE status
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "NYC external function-pointer array references resolved incorrectly")
endif()
set(read-once-pointer-array-source
    "${SOURCE_DIR}/fixtures/read-once-pointer-array.c")

execute_process(
  COMMAND "${NYC}" -c -std=gnu11 "${read-once-pointer-array-source}"
          -o "${BINARY_DIR}/read-once-pointer-array.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc failed to compile READ_ONCE on an external aggregate-pointer array\n${stdout}${stderr}")
endif()
set(read-once-pointer-array-host
    "${SOURCE_DIR}/fixtures/read-once-pointer-array-host.c")

execute_process(
  COMMAND "${CC}" "${read-once-pointer-array-host}"
          "${BINARY_DIR}/read-once-pointer-array.o" -o
          "${BINARY_DIR}/read-once-pointer-array"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host linker rejected READ_ONCE pointer-array regression\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/read-once-pointer-array"
  RESULT_VARIABLE status
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "READ_ONCE pointer-array regression returned an incorrect value")
endif()
set(typeof-volatile-member-source
    "${SOURCE_DIR}/fixtures/typeof-volatile-member.c")

execute_process(
  COMMAND "${NYC}" -c -std=gnu11 "${typeof-volatile-member-source}"
          -o "${BINARY_DIR}/typeof-volatile-member.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc failed to preserve a scalar member type through typeof\n${stdout}${stderr}")
endif()
set(typeof-volatile-member-host
    "${SOURCE_DIR}/fixtures/typeof-volatile-member-host.c")

execute_process(
  COMMAND "${CC}" "${typeof-volatile-member-host}"
          "${BINARY_DIR}/typeof-volatile-member.o" -o
          "${BINARY_DIR}/typeof-volatile-member"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host linker rejected typeof scalar-member regression\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/typeof-volatile-member"
  RESULT_VARIABLE status
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "typeof scalar-member store produced an incorrect runtime value")
endif()
set(typeof-function-designator-source
    "${SOURCE_DIR}/fixtures/typeof-function-designator.c")

execute_process(
  COMMAND "${NYC}" -c -std=gnu11 "${typeof-function-designator-source}"
          -o "${BINARY_DIR}/typeof-function-designator.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc failed to call a typeof-derived function designator\n${stdout}${stderr}")
endif()
set(typeof-function-designator-host
    "${SOURCE_DIR}/fixtures/typeof-function-designator-host.c")

execute_process(
  COMMAND "${CC}" "${typeof-function-designator-host}"
          "${BINARY_DIR}/typeof-function-designator.o" -o
          "${BINARY_DIR}/typeof-function-designator"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host linker rejected typeof function-designator regression\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/typeof-function-designator"
  RESULT_VARIABLE status
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "typeof function-designator call returned an incorrect result")
endif()
set(fixed-register-syscall-source
    "${SOURCE_DIR}/fixtures/fixed-register-syscall.c")

execute_process(
  COMMAND "${NYC}" -S "${fixed-register-syscall-source}"
          -o "${BINARY_DIR}/fixed-register-syscall.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc failed to lower ABI-matched fixed-register syscall arguments\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/fixed-register-syscall.s" -o
          "${BINARY_DIR}/fixed-register-syscall"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host linker rejected fixed-register syscall regression\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/fixed-register-syscall"
  RESULT_VARIABLE status
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "fixed-register syscall did not return a positive process ID")
endif()
set(fixed-register-syscall-mismatch-source
    "${SOURCE_DIR}/fixtures/fixed-register-syscall-mismatch.c")

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
set(external_integer_array_source "${SOURCE_DIR}/fixtures/external-integer-arrays.c")

execute_process(
  COMMAND "${NYC}" -c -std=gnu11 "${external_integer_array_source}"
          -o "${BINARY_DIR}/external-integer-arrays.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc failed to parse multiple external integer arrays\n${stdout}${stderr}")
endif()
set(external_integer_array_harness
    "${SOURCE_DIR}/fixtures/external-integer-arrays-host.c")

execute_process(
  COMMAND "${CC}" "${external_integer_array_harness}"
          "${BINARY_DIR}/external-integer-arrays.o" -o
          "${BINARY_DIR}/external-integer-arrays-host"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "reference compiler could not link external integer arrays\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/external-integer-arrays-host"
  RESULT_VARIABLE status
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "NYC external integer array references resolved incorrectly")
endif()
set(static_pointer_arrays_source
    "${SOURCE_DIR}/fixtures/static-pointer-arrays.c")

execute_process(
  COMMAND "${NYC}" -c -std=gnu11 "${static_pointer_arrays_source}"
          -o "${BINARY_DIR}/static-pointer-arrays.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc failed to compile zero-initialized static pointer arrays\n${stdout}${stderr}")
endif()

execute_process(
  COMMAND "${CC}" "${SOURCE_DIR}/fixtures/static-pointer-arrays-main.c"
          "${BINARY_DIR}/static-pointer-arrays.o"
          -o "${BINARY_DIR}/static-pointer-arrays"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host linker rejected zero-initialized static pointer arrays\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/static-pointer-arrays"
  RESULT_VARIABLE status
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "static pointer arrays were not zero initialized")
endif()
set(global_function_pointer_source
    "${SOURCE_DIR}/fixtures/external-global-function-pointer.c")

execute_process(
  COMMAND "${NYC}" -c -std=gnu11 "${global_function_pointer_source}"
          -o "${BINARY_DIR}/external-global-function-pointer.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc failed to compile an external global function-pointer call\n${stdout}${stderr}")
endif()
set(global_function_pointer_harness
    "${SOURCE_DIR}/fixtures/external-global-function-pointer-main.c")

execute_process(
  COMMAND "${CC}" "${global_function_pointer_harness}"
          "${BINARY_DIR}/external-global-function-pointer.o"
          -o "${BINARY_DIR}/external-global-function-pointer"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "linking an external global function-pointer call failed\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/external-global-function-pointer"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "external global function-pointer call returned an incorrect value\n${stdout}${stderr}")
endif()
set(multidimensional_array_source
    "${SOURCE_DIR}/fixtures/external-multidimensional-array.c")

execute_process(
  COMMAND "${NYC}" -fms-extensions -c "${multidimensional_array_source}"
          -o "${BINARY_DIR}/external-multidimensional-array.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc multidimensional external array compilation failed\n${stdout}${stderr}")
endif()
set(multidimensional_array_harness
    "${SOURCE_DIR}/fixtures/external-multidimensional-array-host.c")

execute_process(
  COMMAND "${CC}" -fms-extensions "${multidimensional_array_harness}"
          "${BINARY_DIR}/external-multidimensional-array.o"
          -o "${BINARY_DIR}/external-multidimensional-array-host"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host compiler could not link the NYC multidimensional-array test\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/external-multidimensional-array-host"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "NYC multidimensional external-array stride or value mismatch\n${stdout}${stderr}")
endif()
foreach(flag
    -mcmodel=large
    -fcf-protection=branch
    -mfunction-return=thunk-extern
    -mstack-protector-guard=gs
    )
  execute_process(
    COMMAND "${NYC}" -E "${flag}" "${static_assert_source}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(status EQUAL 0 OR NOT stderr MATCHES "unknown option")
    message(FATAL_ERROR
      "nyc did not reject unsupported semantic option ${flag}\n${stdout}${stderr}")
  endif()
endforeach()
set(pic-lowercase-source "${SOURCE_DIR}/fixtures/pic-lowercase.c")

execute_process(
  COMMAND "${NYC}" -fpic -fno-omit-frame-pointer
          -foptimize-sibling-calls -fasynchronous-unwind-tables -O2 -S
          "${pic-lowercase-source}"
          -o "${BINARY_DIR}/pic-lowercase.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc rejected the Linux vDSO -fpic option\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/pic-lowercase.s"
          -o "${BINARY_DIR}/pic-lowercase"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "external assembler rejected NYC -fpic output\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/pic-lowercase"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "NYC -fpic global access failed\n${stdout}${stderr}")
endif()
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
endif()
# Driver flags, preprocessing, and target option behavior.
set(i386_data_model_source "${SOURCE_DIR}/fixtures/i386-data-model.c")
execute_process(
  COMMAND "${NYC}" -c -m32 "${i386_data_model_source}" -o
          "${BINARY_DIR}/i386-data-model.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(status EQUAL 0 OR
   NOT "${stdout}${stderr}" MATCHES
       "whole-unit output currently requires x86-64 ELF, or i386 GNU assembly output" OR
   "${stdout}${stderr}" MATCHES "_Static_assert condition is false")
  message(FATAL_ERROR
    "nyc i386 data-model assertions failed before the known emitter boundary\n${stdout}${stderr}")
endif()
if(NYC_AS_EXECUTABLE)
  set(i386_vdso_ops_source "${SOURCE_DIR}/fixtures/i386-vdso-ops.c")
  execute_process(
    COMMAND "${NYC}" -m32 -S "${i386_vdso_ops_source}" -o
            "${BINARY_DIR}/i386-vdso-ops.s"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "nyc could not emit i386 VDSO syscall and arithmetic assembly\n${stdout}${stderr}")
  endif()
  file(READ "${BINARY_DIR}/i386-vdso-ops.s" i386_vdso_ops_assembly)
  if(NOT i386_vdso_ops_assembly MATCHES "int[ \t]+\\$0x80" OR
     NOT i386_vdso_ops_assembly MATCHES "udiv_loop")
    message(FATAL_ERROR
      "i386 VDSO lowering omitted syscall or unsigned division code")
  endif()
  execute_process(
    COMMAND "${NYC_AS_EXECUTABLE}" --32 -o
            "${BINARY_DIR}/i386-vdso-ops.o"
            "${BINARY_DIR}/i386-vdso-ops.s"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "GNU as --32 rejected NYC's i386 VDSO output\n${stdout}${stderr}")
  endif()
  set(i386_lsl_source "${SOURCE_DIR}/fixtures/i386-lsl-alternative.c")
  execute_process(
    COMMAND "${NYC}" -m32 -S "${i386_lsl_source}" -o
            "${BINARY_DIR}/i386-lsl-alternative.s"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "nyc could not lower the i386 LSL alternative\n${stdout}${stderr}")
  endif()
  file(READ "${BINARY_DIR}/i386-lsl-alternative.s" i386_lsl_assembly)
  if(NOT i386_lsl_assembly MATCHES "lsll[ \t]+%cx,[ \t]*%eax")
    message(FATAL_ERROR
      "NYC's i386 LSL alternative did not emit the LSL instruction")
  endif()
  execute_process(
    COMMAND "${NYC_AS_EXECUTABLE}" --32 -o
            "${BINARY_DIR}/i386-lsl-alternative.o"
            "${BINARY_DIR}/i386-lsl-alternative.s"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "GNU as --32 rejected NYC's i386 LSL output\n${stdout}${stderr}")
  endif()
  if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    foreach(regparm RANGE 0 3)
      set(i386_abi_binary "${BINARY_DIR}/i386-aggregate-abi-${regparm}")
      execute_process(
        COMMAND "${NYC}" -m32 "-mregparm=${regparm}" -O3 -S
                "${SOURCE_DIR}/fixtures/i386-aggregate-abi.c"
                -o "${i386_abi_binary}.s"
        RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
      if(NOT status EQUAL 0)
        message(FATAL_ERROR
          "i386 regparm=${regparm} compilation failed\n${stdout}${stderr}")
      endif()
      execute_process(
        COMMAND "${CC}" -m32 "-mregparm=${regparm}" -O2 -ffreestanding
                -fno-stack-protector -fno-pie -nostdlib -no-pie -Wl,-e,_start
                "${SOURCE_DIR}/fixtures/i386-aggregate-abi-host.c"
                "${i386_abi_binary}.s" -o "${i386_abi_binary}"
        RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
      if(NOT status EQUAL 0)
        message(FATAL_ERROR
          "i386 regparm=${regparm} link failed\n${stdout}${stderr}")
      endif()
      execute_process(COMMAND "${i386_abi_binary}"
        RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
      if(NOT status EQUAL 0)
        message(FATAL_ERROR
          "i386 regparm=${regparm} returned incorrect results: ${status}\n${stdout}${stderr}")
      endif()
    endforeach()
  endif()
endif()
execute_process(
  COMMAND "${NYC}" -m32 -dM -E -
  INPUT_FILE "${SOURCE_DIR}/fixtures/no-sse-integer.c"
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
  INPUT_FILE "${SOURCE_DIR}/fixtures/no-sse-integer.c"
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
set(no_sse_source "${SOURCE_DIR}/fixtures/no-sse-integer.c")

execute_process(
  COMMAND "${NYC}" -c -std=gnu11 -O2 -mno-sse -mno-sse2 -mno-mmx
          -mno-3dnow -mno-avx -mno-sse4a -mno-80387
          -mno-fp-ret-in-387 -march=x86-64 -mtune=generic -mno-red-zone
          -mpreferred-stack-boundary=3
          -mcmodel=kernel -mcmodel=small -fno-stack-check -fno-strict-overflow
          -falign-functions=16 -mskip-rax-setup
          "${no_sse_source}" -o "${BINARY_DIR}/no-sse-integer.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc scalar integer compilation with no-SSE flags failed\n${stdout}${stderr}")
endif()
if(OBJDUMP)
  execute_process(
    COMMAND "${OBJDUMP}" -d "${BINARY_DIR}/no-sse-integer.o"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0 OR
     stdout MATCHES "%(xmm|ymm|zmm|mm[0-7])")
    message(FATAL_ERROR
      "NYC emitted SIMD registers under no-SSE flags\n${stdout}${stderr}")
  endif()
endif()
set(no_sse_harness "${SOURCE_DIR}/fixtures/no-sse-main.c")

execute_process(
  COMMAND "${CC}" "${no_sse_harness}" "${BINARY_DIR}/no-sse-integer.o"
          -o "${BINARY_DIR}/no-sse-test"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "could not link no-SSE NYC behavior test\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/no-sse-test"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "no-SSE NYC integer function returned a wrong result\n${stdout}${stderr}")
endif()
set(no_sse_float_source "${SOURCE_DIR}/fixtures/no-sse-float.c")

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
set(wchar_source "${SOURCE_DIR}/fixtures/short-wchar.c")

execute_process(
  COMMAND "${NYC}" -c -std=gnu11 -fshort-wchar "${wchar_source}"
          -o "${BINARY_DIR}/short-wchar-nyc.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "nyc -fshort-wchar semantics failed\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" -std=gnu11 -fshort-wchar -c "${wchar_source}"
          -o "${BINARY_DIR}/short-wchar-reference.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "reference compiler rejected short wchar_t assertions\n${stdout}${stderr}")
endif()
set(char_source "${SOURCE_DIR}/fixtures/char-signedness.c")

set(char_harness "${SOURCE_DIR}/fixtures/char-signedness-main.c")

foreach(flag IN ITEMS "" "-funsigned-char")
  set(suffix "signed")
  if(flag)
    set(suffix "unsigned")
  endif()
  execute_process(
    COMMAND "${NYC}" -c -std=gnu11 ${flag} "${char_source}"
            -o "${BINARY_DIR}/char-${suffix}-nyc.o"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "nyc char signedness semantics failed (${suffix})\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${CC}" -std=gnu11 ${flag} -c "${char_source}"
            -o "${BINARY_DIR}/char-${suffix}-reference.o"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "reference char signedness assertions failed (${suffix})\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${CC}" -std=gnu11 ${flag} -c "${char_harness}"
            -o "${BINARY_DIR}/char-${suffix}-main.o"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "reference char signedness harness failed (${suffix})\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${CC}" "${BINARY_DIR}/char-${suffix}-main.o"
            "${BINARY_DIR}/char-${suffix}-nyc.o"
            -o "${BINARY_DIR}/char-${suffix}-check"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "could not link NYC char signedness output (${suffix})\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${BINARY_DIR}/char-${suffix}-check"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "NYC char signedness runtime check failed (${suffix})\n${stdout}${stderr}")
  endif()
endforeach()
set(alias_source "${SOURCE_DIR}/fixtures/no-strict-aliasing.c")

set(alias_harness "${SOURCE_DIR}/fixtures/no-strict-aliasing-main.c")

execute_process(
  COMMAND "${NYC}" -O2 -c -std=gnu11 -fno-strict-aliasing "${alias_source}"
          -o "${BINARY_DIR}/no-strict-aliasing-nyc.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc -fno-strict-aliasing compile failed\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" -std=gnu11 -c "${alias_harness}"
          -o "${BINARY_DIR}/no-strict-aliasing-main.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "alias harness compile failed\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/no-strict-aliasing-main.o"
          "${BINARY_DIR}/no-strict-aliasing-nyc.o"
          -o "${BINARY_DIR}/no-strict-aliasing-check"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "alias harness link failed\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/no-strict-aliasing-check"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "NYC -fno-strict-aliasing runtime check failed\n${stdout}${stderr}")
endif()
set(driver_include "${BINARY_DIR}/driver-forced-include.h")
set(driver_source "${SOURCE_DIR}/fixtures/driver-forced-include.c")
set(driver_object "${BINARY_DIR}/driver-forced-include.o")
set(driver_depfile "${BINARY_DIR}/driver-forced-include.d")
get_filename_component(driver_binary_real "${BINARY_DIR}" REALPATH)
get_filename_component(driver_source_real "${driver_source}" REALPATH)
get_filename_component(driver_fixture_real "${SOURCE_DIR}/fixtures" REALPATH)
configure_file("${SOURCE_DIR}/fixtures/driver-forced-include.h" "${driver_include}" COPYONLY)

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
execute_process(
  COMMAND "${NYC}" -c -nostdinc -std=gnu11 -m64 -fno-PIE
          -fms-extensions -Werror "-I${BINARY_DIR}"
          -include "${driver_include}" "-Wp,-MMD,${driver_depfile}"
          "${driver_source}" -o "${driver_object}"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "nyc forced-include compile failed\n${stdout}${stderr}")
endif()
file(READ "${driver_depfile}" driver_dependencies)
if(NOT driver_dependencies MATCHES
   "driver-forced-include\\.h" OR NOT driver_dependencies MATCHES
   "driver-forced-include\\.c")
  message(FATAL_ERROR
    "nyc dependency output omitted the forced header or source: ${driver_dependencies}")
endif()

execute_process(
  COMMAND "${CC}" -o "${BINARY_DIR}/driver-forced-include-host"
          "${SOURCE_DIR}/fixtures/driver-forced-include-host.c" "${driver_object}"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "linking forced-include NYC object failed\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/driver-forced-include-host"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "forced include did not affect the emitted object as expected\n${stdout}${stderr}")
endif()
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
execute_process(
  COMMAND "${NYC}" -E -I "${BINARY_DIR}" -include "${driver_include}"
          "-Wp,-MMD,${preprocess_depfile}" -o
          "${BINARY_DIR}/driver-preprocess.i" "${driver_source}"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0 OR NOT EXISTS "${preprocess_depfile}")
  message(FATAL_ERROR
    "nyc preprocessing did not produce requested dependencies\n${stdout}${stderr}")
endif()
file(READ "${preprocess_depfile}" preprocess_dependencies)
if(NOT preprocess_dependencies MATCHES
   "driver-forced-include\\.h" OR NOT preprocess_dependencies MATCHES
   "driver-forced-include\\.c")
  message(FATAL_ERROR
    "nyc preprocessing dependencies omitted input/header: ${preprocess_dependencies}")
endif()
set(stdin_preprocess_depfile "${BINARY_DIR}/stdin-preprocess.d")
execute_process(
  COMMAND "${NYC}" -E -I "${BINARY_DIR}"
          "-Wp,-MMD,${stdin_preprocess_depfile}" -
  INPUT_FILE "${driver_source}"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0 OR NOT EXISTS "${stdin_preprocess_depfile}")
  message(FATAL_ERROR
    "nyc stdin preprocessing dependency generation failed\n${stdout}${stderr}")
endif()
file(READ "${stdin_preprocess_depfile}" stdin_preprocess_dependencies)
if(NOT stdin_preprocess_dependencies MATCHES "^-[^:]*:" OR
   NOT stdin_preprocess_dependencies MATCHES
   "driver-forced-include\\.h")
  message(FATAL_ERROR
    "nyc stdin dependencies omitted the target or forced header: ${stdin_preprocess_dependencies}")
endif()
file(MAKE_DIRECTORY "${BINARY_DIR}/trace/events")
configure_file("${SOURCE_DIR}/fixtures/trace/events/variadic.h"
               "${BINARY_DIR}/trace/events/variadic.h" COPYONLY)

foreach(probe IN ITEMS variadic-comma-elision wide-variadic)
  execute_process(
    COMMAND "${NYC}" -O3 -S "-I${BINARY_DIR}"
            "${SOURCE_DIR}/fixtures/${probe}.c"
            -o "${BINARY_DIR}/${probe}.s"
    RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
  if(NOT status EQUAL 0)
    message(FATAL_ERROR "nyc ${probe} compilation failed\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${CC}" "${BINARY_DIR}/${probe}.s" -o "${BINARY_DIR}/${probe}"
    RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
  if(NOT status EQUAL 0)
    message(FATAL_ERROR "${probe} link failed\n${stdout}${stderr}")
  endif()
  execute_process(COMMAND "${BINARY_DIR}/${probe}"
    RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
  if(NOT status EQUAL 0)
    message(FATAL_ERROR "${probe} returned incorrect results\n${stdout}${stderr}")
  endif()
endforeach()

set(expanded_header_source "${SOURCE_DIR}/fixtures/expanded-header-path.c")

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

set(anonymous_aggregate_source "${SOURCE_DIR}/fixtures/anonymous-aggregate.c")
set(anonymous_aggregate_object "${BINARY_DIR}/anonymous-aggregate.o")

execute_process(
  COMMAND "${NYC}" --all "${anonymous_aggregate_source}"
          "${anonymous_aggregate_object}"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "nyc anonymous aggregate compile failed\n${stdout}${stderr}")
endif()

execute_process(
  COMMAND "${CC}" -o "${BINARY_DIR}/anonymous-aggregate-host"
          "${SOURCE_DIR}/fixtures/anonymous-aggregate-host.c"
          "${anonymous_aggregate_object}"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "linking anonymous aggregate object failed\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/anonymous-aggregate-host"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "anonymous aggregate field offsets or values mismatched\n${stdout}${stderr}")
endif()

set(static_local_section_source
  "${SOURCE_DIR}/fixtures/static-local-section.c")

execute_process(
  COMMAND "${NYC}" -S "${static_local_section_source}"
          -o "${BINARY_DIR}/static-local-section.s"
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc failed to compile attributed static local array\n${stdout}${stderr}")
endif()

execute_process(
  COMMAND "${CC}" -o "${BINARY_DIR}/static-local-section"
          "${SOURCE_DIR}/fixtures/static-local-section-host.c"
          "${BINARY_DIR}/static-local-section.s"
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host compiler rejected attributed static local assembly\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/static-local-section"
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "attributed static local array did not preserve static storage\n${stdout}${stderr}")
endif()

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
  execute_process(
    COMMAND "${NYC}" "${SOURCE_DIR}/${source}" "${output}" "${symbol}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR "nyc failed for ${source}:${symbol}\n${stdout}${stderr}")
  endif()
  list(APPEND objects "${output}")
endforeach()

set(many_functions_source "")
foreach(index RANGE 0 128)
  string(APPEND many_functions_source "long c_forward_${index}();\n")
endforeach()
string(APPEND many_functions_source "long c_many_functions() { return 42; }\n")
set(many_functions_input "${SOURCE_DIR}/fixtures/many-functions.c")
set(many_functions_object "${BINARY_DIR}/many-functions.o")

execute_process(
  COMMAND "${NYC}" "${many_functions_input}" "${many_functions_object}"
          c_many_functions
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc failed for a translation unit with more than 128 declarations\n${stdout}${stderr}")
endif()
list(APPEND objects "${many_functions_object}")

set(unit_object "${BINARY_DIR}/translation-unit.o")
execute_process(
  COMMAND "${NYC}" --all "${SOURCE_DIR}/translation-unit.c" "${unit_object}"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "nyc whole-unit compilation failed\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" -std=c11 -O2 -Wall -Wextra -Werror
          -o "${BINARY_DIR}/translation-unit-host"
          "${SOURCE_DIR}/translation-unit-host.c" "${unit_object}"
          -rtlib=compiler-rt
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "linking NYR whole-unit object failed\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/translation-unit-host"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "NYR whole-unit object exported functions with incorrect behavior\n${stdout}${stderr}")
endif()
if(NOT stdout STREQUAL "C stdio: 42 1.25\n")
  message(FATAL_ERROR "NYR C stdio variadic output mismatch: ${stdout}")
endif()
set(empty_variadic_source "${SOURCE_DIR}/fixtures/empty-variadic.c")

execute_process(
  COMMAND "${NYC}" -O2 -S "${empty_variadic_source}"
          -o "${BINARY_DIR}/empty-variadic.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "NYC failed to compile an unused variadic function\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/empty-variadic.s"
          -o "${BINARY_DIR}/empty-variadic"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host assembler/linker rejected empty variadic function output\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/empty-variadic"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "empty variadic function corrupted its caller stack\n${stdout}${stderr}")
endif()
set(unit_omit_fp_object "${BINARY_DIR}/translation-unit-omit-fp.o")
execute_process(
  COMMAND "${NYC}" --all -fomit-frame-pointer
          "${SOURCE_DIR}/translation-unit.c" "${unit_omit_fp_object}"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc whole-unit compilation with -fomit-frame-pointer failed\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" -std=c11 -O2 -Wall -Wextra -Werror
          -o "${BINARY_DIR}/translation-unit-omit-fp-host"
          "${SOURCE_DIR}/translation-unit-host.c" "${unit_omit_fp_object}"
          -rtlib=compiler-rt
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "linking omitted-frame NYR whole-unit object failed\n${stdout}${stderr}")
endif()
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
execute_process(
  COMMAND "${NYC}" --all "${SOURCE_DIR}/static-alignment.c"
          "${static_alignment_object}"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "nyc static alignment compilation failed\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" -std=c11 -O2 -Wall -Wextra -Werror
          -o "${BINARY_DIR}/static-alignment-host"
          "${SOURCE_DIR}/static-alignment-host.c" "${static_alignment_object}"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "linking static alignment regression failed\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/static-alignment-host"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "NYR static aggregate data is misaligned (exit ${status})\n${stdout}${stderr}")
endif()

set(linkage_a_source "${SOURCE_DIR}/fixtures/internal-linkage-a.c")
set(linkage_b_source "${SOURCE_DIR}/fixtures/internal-linkage-b.c")
set(linkage_host_source "${SOURCE_DIR}/fixtures/internal-linkage-host.c")

foreach(unit IN ITEMS a b)
  execute_process(
    COMMAND "${NYC}" -c "${SOURCE_DIR}/fixtures/internal-linkage-${unit}.c"
            -o "${BINARY_DIR}/internal-linkage-${unit}.o"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "nyc failed to compile the ${unit} internal-linkage fixture\n${stdout}${stderr}")
  endif()
endforeach()
execute_process(
  COMMAND "${CC}" -no-pie -o "${BINARY_DIR}/internal-linkage-host"
          "${linkage_host_source}" "${BINARY_DIR}/internal-linkage-a.o"
          "${BINARY_DIR}/internal-linkage-b.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host compiler could not link NYC objects with colliding internal symbols\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/internal-linkage-host"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "NYC internal-linkage symbols crossed translation-unit boundaries\n${stdout}${stderr}")
endif()

# Whole-translation-unit, linkage, and inline-assembly regressions.
set(global_asm_object "${BINARY_DIR}/global-asm.o")
execute_process(
  COMMAND "${NYC}" --all "${SOURCE_DIR}/global-asm.c" "${global_asm_object}"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "NYC file-scope asm compilation failed\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" -std=c11 -Wall -Wextra -Werror
          -o "${BINARY_DIR}/global-asm-host"
          "${SOURCE_DIR}/global-asm-host.c" "${global_asm_object}"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host compiler could not link NYC file-scope asm output\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/global-asm-host"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "NYC file-scope asm PC-relative data relocation was incorrect\n${stdout}${stderr}")
endif()

set(inline_asm_object "${BINARY_DIR}/inline-asm.o")
execute_process(
  COMMAND "${NYC}" --all "${SOURCE_DIR}/inline-asm.c" "${inline_asm_object}"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "NYC operand-free inline asm compilation failed\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" -std=c11 -Wall -Wextra -Werror
          -o "${BINARY_DIR}/inline-asm-host"
          "${SOURCE_DIR}/inline-asm-host.c" "${inline_asm_object}"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host compiler could not link NYC inline asm output\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/inline-asm-host"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "NYC inline asm host smoke failed\n${stdout}${stderr}")
endif()
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
  "${SOURCE_DIR}/fixtures/linux-alternative-wrmsr.c")
execute_process(
  COMMAND "${NYC}" -O2 -S "${alternative_wrmsr_source}"
          -o "${BINARY_DIR}/linux-alternative-wrmsr.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "NYC failed to specialize the Linux alternative WRMSR helper\n${stdout}${stderr}")
endif()
file(READ "${BINARY_DIR}/linux-alternative-wrmsr.s" alternative_wrmsr_asm)
if(NOT alternative_wrmsr_asm MATCHES "\\.altinstructions" OR
   NOT alternative_wrmsr_asm MATCHES "\\.altinstr_replacement" OR
   NOT alternative_wrmsr_asm MATCHES "\\.4byte[ \t]+246" OR
   NOT alternative_wrmsr_asm MATCHES "wrmsr")
  message(FATAL_ERROR
    "NYC output omitted alternative WRMSR metadata or replacement code")
endif()
if(NYC_AS_EXECUTABLE)
  execute_process(
    COMMAND "${NYC_AS_EXECUTABLE}" --64
            "${BINARY_DIR}/linux-alternative-wrmsr.s"
            -o "${BINARY_DIR}/linux-alternative-wrmsr.o"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "GNU as rejected Linux alternative WRMSR output\n${stdout}${stderr}")
  endif()
endif()

set(gs_percpu_load_source "${SOURCE_DIR}/fixtures/linux-gs-percpu-load.c")
execute_process(
  COMMAND "${NYC}" -O2 -S "${gs_percpu_load_source}"
          -o "${BINARY_DIR}/linux-gs-percpu-load.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "NYC failed to lower Linux GS-relative per-CPU loading\n${stdout}${stderr}")
endif()
file(READ "${BINARY_DIR}/linux-gs-percpu-load.s" gs_percpu_load_asm)
if(NOT gs_percpu_load_asm MATCHES "movl[ \t]+%gs:\\(%rax\\), %eax")
  message(FATAL_ERROR
    "NYC output omitted the GS-relative 32-bit load")
endif()
if(NYC_AS_EXECUTABLE)
  execute_process(
    COMMAND "${NYC_AS_EXECUTABLE}" --64
            "${BINARY_DIR}/linux-gs-percpu-load.s"
            -o "${BINARY_DIR}/linux-gs-percpu-load.o"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "GNU as rejected Linux GS-relative load output\n${stdout}${stderr}")
  endif()
endif()

set(data_segment_reload_source
  "${SOURCE_DIR}/fixtures/linux-data-segment-reload.c")
execute_process(
  COMMAND "${NYC}" -O2 -S "${data_segment_reload_source}"
          -o "${BINARY_DIR}/linux-data-segment-reload.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "NYC failed to lower Linux data-segment reload asm\n${stdout}${stderr}")
endif()
if(NYC_AS_EXECUTABLE)
  execute_process(
    COMMAND "${NYC_AS_EXECUTABLE}" --64
            "${BINARY_DIR}/linux-data-segment-reload.s"
            -o "${BINARY_DIR}/linux-data-segment-reload.o"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "GNU as rejected Linux data-segment reload output\n${stdout}${stderr}")
  endif()
  if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    execute_process(
      COMMAND "${CC}" "${SOURCE_DIR}/fixtures/linux-data-segment-reload-host.c"
              "${BINARY_DIR}/linux-data-segment-reload.o"
              -o "${BINARY_DIR}/linux-data-segment-reload"
      RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
    if(NOT status EQUAL 0)
      message(FATAL_ERROR
        "host compiler could not link data-segment reload test\n${stdout}${stderr}")
    endif()
    execute_process(COMMAND "${BINARY_DIR}/linux-data-segment-reload"
      RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
    if(NOT status EQUAL 0)
      message(FATAL_ERROR
        "data-segment reload produced incorrect segment state\n${stdout}${stderr}")
    endif()
  endif()
endif()

set(static_local_section_source
  "${SOURCE_DIR}/fixtures/static-local-section-attribute.c")
execute_process(
  COMMAND "${NYC}" -O2 -S "${static_local_section_source}"
          -o "${BINARY_DIR}/static-local-section-attribute.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "NYC failed to preserve a static local section attribute\n${stdout}${stderr}")
endif()
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
  execute_process(
    COMMAND "${NYC_AS_EXECUTABLE}" --64
            "${BINARY_DIR}/static-local-section-attribute.s"
            -o "${BINARY_DIR}/static-local-section-attribute.o"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "GNU as rejected static local section output\n${stdout}${stderr}")
  endif()
endif()

set(unused_inline_table_source
  "${SOURCE_DIR}/fixtures/unused-inline-static-pointer-table.c")
execute_process(
  COMMAND "${NYC}" -O2 -S "${unused_inline_table_source}"
          -o "${BINARY_DIR}/unused-inline-static-pointer-table.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "NYC failed to compile static pointer-table liveness fixture\n${stdout}${stderr}")
endif()
file(READ "${BINARY_DIR}/unused-inline-static-pointer-table.s"
     unused_inline_table_asm)
string(REGEX MATCHALL "\\.local[ \t]+__nyc_static_ptrs_[0-9]+"
       unused_inline_table_symbols "${unused_inline_table_asm}")
list(LENGTH unused_inline_table_symbols unused_inline_table_count)
if(NOT unused_inline_table_count EQUAL 1)
  message(FATAL_ERROR
    "NYC should emit exactly one live static pointer table and its alias")
endif()

set(inline_asm_immediate_source "${SOURCE_DIR}/fixtures/inline-asm-immediate.c")
get_filename_component(inline_asm_immediate_source_path
  "${inline_asm_immediate_source}" REALPATH)
file(TO_CMAKE_PATH "${inline_asm_immediate_source_path}"
  inline_asm_immediate_source_path)

execute_process(
  COMMAND "${NYC}" -S "${inline_asm_immediate_source}"
          -o "${BINARY_DIR}/inline-asm-immediate.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc failed to substitute an immediate-only inline-asm operand\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/inline-asm-immediate.s" -o
          "${BINARY_DIR}/inline-asm-immediate"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host compiler could not link inline-asm immediate test\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/inline-asm-immediate"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "inline-asm immediate expansion returned incorrect data\n${stdout}${stderr}")
endif()

set(inline_asm_memory_source "${SOURCE_DIR}/fixtures/inline-asm-memory.c")

execute_process(
  COMMAND "${NYC}" -S "${inline_asm_memory_source}"
          -o "${BINARY_DIR}/inline-asm-memory.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc failed to lower a named read-write memory asm operand\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/inline-asm-memory.s" -o
          "${BINARY_DIR}/inline-asm-memory"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host compiler could not link inline-asm memory test\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/inline-asm-memory"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "inline-asm memory operation returned incorrect data\n${stdout}${stderr}")
endif()

set(typedef_callback_source "${SOURCE_DIR}/fixtures/typedef-callback-field.c")

execute_process(
  COMMAND "${NYC}" -S "${typedef_callback_source}"
          -o "${BINARY_DIR}/typedef-callback-field.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc failed to call a typedef function-pointer aggregate field\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/typedef-callback-field.s" -o
          "${BINARY_DIR}/typedef-callback-field"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host compiler could not link typedef callback field test\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/typedef-callback-field"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "typedef callback field invoked with incorrect result\n${stdout}${stderr}")
endif()

set(function_parameter_adjustment_source
    "${SOURCE_DIR}/fixtures/function-parameter-adjustment.c")

execute_process(
  COMMAND "${NYC}" -S "${function_parameter_adjustment_source}"
          -o "${BINARY_DIR}/function-parameter-adjustment.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc failed to adjust a function-typed parameter to a pointer\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/function-parameter-adjustment.s" -o
          "${BINARY_DIR}/function-parameter-adjustment"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host compiler could not link function parameter adjustment test\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/function-parameter-adjustment"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "function-typed parameter was not invoked correctly\n${stdout}${stderr}")
endif()

set(static_array_member_pointer_source
    "${SOURCE_DIR}/fixtures/static-array-member-pointer.c")

execute_process(
  COMMAND "${NYC}" -S "${static_array_member_pointer_source}"
          -o "${BINARY_DIR}/static-array-member-pointer.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc failed to initialize a pointer from a static array member\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/static-array-member-pointer.s" -o
          "${BINARY_DIR}/static-array-member-pointer"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host compiler could not link static array-member pointer test\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/static-array-member-pointer"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "static array-member pointer initializer targeted the wrong address\n${stdout}${stderr}")
endif()

set(multidimensional_pointer_source
    "${SOURCE_DIR}/fixtures/multidimensional-pointer-index.c")

execute_process(
  COMMAND "${NYC}" -S "${multidimensional_pointer_source}"
          -o "${BINARY_DIR}/multidimensional-pointer-index.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc failed to index through a multidimensional array pointer\n${stdout}${stderr}")
endif()
set(multidimensional_pointer_main
    "${SOURCE_DIR}/fixtures/multidimensional-pointer-index-main.c")

execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/multidimensional-pointer-index.s"
          "${multidimensional_pointer_main}" -o
          "${BINARY_DIR}/multidimensional-pointer-index"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host compiler could not link multidimensional array pointer test\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/multidimensional-pointer-index"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "multidimensional array pointer index returned an incorrect value\n${stdout}${stderr}")
endif()

set(global_pointer_array_address_source
    "${SOURCE_DIR}/fixtures/global-pointer-array-address.c")

execute_process(
  COMMAND "${NYC}" -S "${global_pointer_array_address_source}"
          -o "${BINARY_DIR}/global-pointer-array-address.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc failed to take the address of a global pointer-array row\n${stdout}${stderr}")
endif()
set(global_pointer_array_address_main
    "${SOURCE_DIR}/fixtures/global-pointer-array-address-main.c")

execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/global-pointer-array-address.s"
          "${global_pointer_array_address_main}" -o
          "${BINARY_DIR}/global-pointer-array-address"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host compiler could not link global pointer-array address regression\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/global-pointer-array-address"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "global pointer-array row address did not match its source location\n${stdout}${stderr}")
endif()

set(file_scope_asm_source "${SOURCE_DIR}/fixtures/file-scope-asm-ret.c")

execute_process(
  COMMAND "${NYC}" -S "${file_scope_asm_source}"
          -o "${BINARY_DIR}/file-scope-asm-ret.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc failed to emit supported file-scope x86 instructions\n${stdout}${stderr}")
endif()
set(file_scope_asm_main "${SOURCE_DIR}/fixtures/file-scope-asm-ret-main.c")

execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/file-scope-asm-ret.s"
          "${file_scope_asm_main}" -o "${BINARY_DIR}/file-scope-asm-ret"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host compiler could not link file-scope assembly test\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/file-scope-asm-ret"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "file-scope assembly return shim failed\n${stdout}${stderr}")
endif()

set(pointer_to_byte_init_source "${SOURCE_DIR}/fixtures/pointer-to-byte-init.c")

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
execute_process(
  COMMAND "${NYC}" --all "${SOURCE_DIR}/preprocessor.c" "${preprocessor_object}"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "nyc conditional preprocessing failed\n${stdout}${stderr}")
endif()
set(preprocessor_host "${BINARY_DIR}/preprocessor-host")
execute_process(
  COMMAND "${CC}" -std=c11 -O2 -Wall -Wextra -Werror
          -o "${preprocessor_host}" "${SOURCE_DIR}/preprocessor-host.c"
          "${preprocessor_object}"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "linking conditional preprocessing object failed\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${preprocessor_host}"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "conditional preprocessing selected the wrong branches\n${stdout}${stderr}")
endif()

execute_process(
  COMMAND "${NYC}" --all "${SOURCE_DIR}/conditional-expression.c"
          "${BINARY_DIR}/conditional-expression.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc failed to compile a conditional preprocessor expression\n${stdout}${stderr}")
endif()

execute_process(
  COMMAND "${CC}" -o "${BINARY_DIR}/conditional-expression-host"
          "${SOURCE_DIR}/fixtures/conditional-expression-main.c"
          "${BINARY_DIR}/conditional-expression.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "linking conditional-expression object failed\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/conditional-expression-host"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "conditional expression selected the wrong preprocessor branch\n${stdout}${stderr}")
endif()

execute_process(
  COMMAND "${CC}" -std=c11 -O2 -Wall -Wextra -Werror
          -o "${BINARY_DIR}/host"
          "${SOURCE_DIR}/host.c" "${SOURCE_DIR}/reference.c" ${objects}
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "building C reference host failed\n${stdout}${stderr}")
endif()

execute_process(
  COMMAND "${BINARY_DIR}/host"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "C reference comparison failed\n${stdout}${stderr}")
endif()
message(STATUS "${stdout}")

# Object-format acceptance uses external LLVM lld drivers only as optional
# linker dependencies. The public `nyc` API remains linker-independent.
# Cross-target ABI checks and unsupported-input diagnostics.
set(target_source "${SOURCE_DIR}/target-link.c")
set(aarch64_object "${BINARY_DIR}/target-link-aarch64.o")
execute_process(
  COMMAND "${NYC}" --target=aarch64-unknown-linux-gnu "${target_source}"
          "${aarch64_object}" c_target_link
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0 OR NOT EXISTS "${aarch64_object}")
  message(FATAL_ERROR "nyc AArch64 ELF emission failed\n${stdout}${stderr}")
endif()
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
set(aarch64_aggregate_source "${SOURCE_DIR}/aarch64-aggregates.c")
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
  execute_process(
    COMMAND "${NYC}" --target=aarch64-unknown-linux-gnu
            "${aarch64_aggregate_source}" "${object}" "${symbol}"
    RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0 OR NOT EXISTS "${object}")
    message(FATAL_ERROR "nyc AArch64 aggregate compilation failed for ${symbol}\n${stdout}${stderr}")
  endif()
  list(APPEND aarch64_aggregate_objects "${object}")
endforeach()

find_program(AARCH64_CC NAMES aarch64-linux-gnu-gcc)
find_program(AARCH64_QEMU NAMES qemu-aarch64)
if(AARCH64_CC)
  set(aarch64_aggregate_exe "${BINARY_DIR}/aarch64-aggregate-host")
  execute_process(
    COMMAND "${AARCH64_CC}" -std=c11 -O2 -Wall -Wextra -Werror
            -o "${aarch64_aggregate_exe}"
            "${SOURCE_DIR}/aarch64-aggregate-host.c"
            ${aarch64_aggregate_objects}
    RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0 OR NOT EXISTS "${aarch64_aggregate_exe}")
    message(FATAL_ERROR "AArch64 C aggregate link failed\n${stdout}${stderr}")
  endif()
  if(AARCH64_QEMU)
    execute_process(COMMAND "${AARCH64_CC}" -print-sysroot
                    OUTPUT_VARIABLE aarch64_sysroot
                    OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(
      COMMAND "${AARCH64_QEMU}" -L "${aarch64_sysroot}"
              "${aarch64_aggregate_exe}"
      RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr
    )
    if(NOT status EQUAL 0)
      message(FATAL_ERROR "AArch64 C aggregate runtime parity failed\n${stdout}${stderr}")
    endif()
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
            "${SOURCE_DIR}/structs.c" "${unsupported_aggregate_object}"
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
set(win64_aggregate_source "${SOURCE_DIR}/win64-aggregates.c")
set(win64_aggregate_objects "")
foreach(symbol IN ITEMS c_win_word_add c_win_point_add c_win_big_make
                        c_win_big_sum c_win_big_sum_after4
                        c_win_many_with_point)
  set(object "${BINARY_DIR}/${symbol}-win64.obj")
  execute_process(
    COMMAND "${NYC}" --target=x86_64-pc-windows-msvc
            "${win64_aggregate_source}" "${object}" "${symbol}"
    RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0 OR NOT EXISTS "${object}")
    message(FATAL_ERROR "nyc Win64 aggregate compilation failed for ${symbol}\n${stdout}${stderr}")
  endif()
  list(APPEND win64_aggregate_objects "${object}")
endforeach()

find_program(WIN64_CC NAMES x86_64-w64-mingw32-gcc)
find_program(WIN64_RUNNER NAMES wine wine64)
if(WIN64_CC)
  set(win64_aggregate_exe "${BINARY_DIR}/win64-aggregate-host.exe")
  execute_process(
    COMMAND "${WIN64_CC}" -std=c11 -O2 -Wall -Wextra -Werror
            -o "${win64_aggregate_exe}"
            "${SOURCE_DIR}/win64-aggregate-host.c"
            ${win64_aggregate_objects}
    RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0 OR NOT EXISTS "${win64_aggregate_exe}")
    message(FATAL_ERROR "Win64 C aggregate link failed\n${stdout}${stderr}")
  endif()
  if(WIN64_RUNNER)
    execute_process(
      COMMAND "${WIN64_RUNNER}" "${win64_aggregate_exe}"
      RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr
    )
    if(NOT status EQUAL 0)
      message(FATAL_ERROR "Win64 C aggregate runtime parity failed\n${stdout}${stderr}")
    endif()
  else()
    message(STATUS "Win64 C aggregate runtime skipped: Wine was not found")
  endif()
else()
  message(STATUS "Win64 C aggregate link/runtime skipped: MinGW-w64 was not found")
endif()

if(LLD_LINK AND EXISTS "${LLD_LINK}")
  set(coff_object "${BINARY_DIR}/target-link.obj")
  set(coff_exe "${BINARY_DIR}/target-link-coff.exe")
  execute_process(
    COMMAND "${NYC}" --target=x86_64-pc-windows-msvc "${target_source}"
            "${coff_object}" c_target_link
    RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR "nyc COFF emission failed\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${LLD_LINK}" /entry:c_target_link /subsystem:console
            /nodefaultlib /machine:x64 "/out:${coff_exe}" "${coff_object}"
    RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0 OR NOT EXISTS "${coff_exe}")
    message(FATAL_ERROR "lld-link could not link nyc COFF output\n${stdout}${stderr}")
  endif()
else()
  message(STATUS "COFF link acceptance skipped: lld-link was not found")
endif()

if(LD64_LLD AND EXISTS "${LD64_LLD}")
  set(macho_object "${BINARY_DIR}/target-link-macho.o")
  set(macho_exe "${BINARY_DIR}/target-link-macho")
  execute_process(
    COMMAND "${NYC}" --target=x86_64-apple-darwin "${target_source}"
            "${macho_object}" c_target_link
    RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR "nyc Mach-O emission failed\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${LD64_LLD}" -arch x86_64
            -platform_version macos 10.15 14.0 -e _c_target_link
            -o "${macho_exe}" "${macho_object}"
    RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0 OR NOT EXISTS "${macho_exe}")
    message(FATAL_ERROR "ld64.lld could not link nyc Mach-O output\n${stdout}${stderr}")
  endif()
else()
  message(STATUS "Mach-O link acceptance skipped: ld64.lld was not found")
endif()

# Negative diagnostics and argument-limit checks.
set(unsupported_cases
  "unsupported.c|c_unsupported|continue statement is not inside a loop|2:11"
  "unsupported.c|c_break_outside|break statement is not inside a loop|7:8"
  "void.c|c_void_return_value|void function return must not include a value"

  "const-update.c|c_const_update|cannot assign to a const C object"
  "const-pointer-store.c|c_const_store|cannot write through a pointer to const C data"
  "const-double-pointer-store.c|c_const_f64_store|cannot write through a pointer to const C data"
  "const-index-store.c|c_const_index_store|cannot write through a pointer to const C data"
  "mixed-pointer-store.c|c_mixed_store||"
  "for-scope.c|c_for_scope|references must name a function parameter or declared local"
  "compound.c|c_block_scope|references must name a function parameter or declared local"
  "compound.c|c_redeclare_local|duplicate declaration in the same C scope"

  "increment.c|c_const_postfix_increment|increment and decrement require a mutable scalar or pointer local"
  "increment.c|c_const_prefix_decrement|increment and decrement require a mutable scalar or pointer local"
  "compound.c|c_pointer_compound_unsupported|compound assignment operator is unsupported for this type"
  "typedef.c|c_typedef_const_update|cannot assign to a const C object"
  "typedef-function-signature-conflict.c|nyc_signature_conflict|conflicting function typedef declarations"
  "global-asm-unsupported.c|main|unsupported file-scope assembler directive or expression"
  "inline-asm-unsupported.c|nyc_inline_asm_unsupported|unsupported GNU RIP-relative pointer assembly operands"
  "inline-asm-unsupported.c|nyc_inline_asm_expression_unsupported|GNU inline assembly is not supported by NYC"
  "inline-asm-unsupported.c|nyc_inline_asm_goto_unsupported|GNU asm goto is not supported by NYC"
  "invalid-shift.c|c_invalid_shift|unsupported #if expression"
)
foreach(case IN LISTS unsupported_cases)
  string(REPLACE "|" ";" fields "${case}")
  list(GET fields 0 source)
  list(GET fields 1 symbol)
  list(GET fields 2 expected_diagnostic)
  list(LENGTH fields field_count)
  if(field_count GREATER 3)
    list(GET fields 3 expected_location)
  else()
    set(expected_location "")
  endif()
  execute_process(
    COMMAND "${NYC}" "${SOURCE_DIR}/${source}"
            "${BINARY_DIR}/unsupported.o" "${symbol}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(status EQUAL 0)
    message(FATAL_ERROR "nyc accepted unsupported input ${source}")
  endif()
  string(CONCAT diagnostic "${stdout}" "${stderr}")
  if(diagnostic STREQUAL "")
    message(FATAL_ERROR "nyc rejected ${source} without a diagnostic")
  endif()
  if(expected_location)
    string(FIND "${diagnostic}" "${expected_location}" location_found)
    if(location_found EQUAL -1)
      message(FATAL_ERROR
        "diagnostic for ${source}:${symbol} omitted source location "
        "'${expected_location}'\n${diagnostic}")
    endif()
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
set(too_many_args_input "${SOURCE_DIR}/fixtures/too-many-call-args.c")

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
set(section_source "${SOURCE_DIR}/fixtures/named-sections.c")

execute_process(
  COMMAND "${NYC}" -c "${section_source}"
          -o "${BINARY_DIR}/named-sections.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "nyc named-section data compilation failed\n${stdout}${stderr}")
endif()
set(section_harness "${SOURCE_DIR}/fixtures/named-sections-main.c")

execute_process(
  COMMAND "${CC}" "${section_harness}" "${BINARY_DIR}/named-sections.o" -o
          "${BINARY_DIR}/named-sections-host"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "reference compiler could not link NYRI named-section data\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/named-sections-host"
  RESULT_VARIABLE status
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "NYC named-section data placements resolved incorrectly (7 + 0 + 11 = 18 expected)")
endif()

set(assembly_source "${SOURCE_DIR}/fixtures/assembly-output.c")

execute_process(
  COMMAND "${NYC}" -S -fverbose-asm "${assembly_source}" -o
          "${BINARY_DIR}/assembly-output.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "nyc assembly output failed\n${stdout}${stderr}")
endif()
file(READ "${BINARY_DIR}/assembly-output.s" assembly_text)
if(NOT assembly_text MATCHES "# NYC verbose assembly:")
  message(FATAL_ERROR "nyc -fverbose-asm omitted assembly annotations")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/assembly-output.s" -o
          "${BINARY_DIR}/assembly-output"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host compiler rejected NYC-generated assembly\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/assembly-output"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "NYC-generated assembly failed at runtime\n${stdout}${stderr}")
endif()

set(assembly_data_source "${SOURCE_DIR}/fixtures/assembly-data.c")

execute_process(
  COMMAND "${NYC}" -S "${assembly_data_source}" -o
          "${BINARY_DIR}/assembly-data.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc assembly output failed for file-scope data and relocations\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/assembly-data.s" -o
          "${BINARY_DIR}/assembly-data"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host compiler could not link NYC assembly data output\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/assembly-data"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "NYC assembly data relocation failed at runtime\n${stdout}${stderr}")
endif()

set(unused_asm_goto_source "${SOURCE_DIR}/fixtures/unused-asm-goto.c")

execute_process(
  COMMAND "${NYC}" -c "${unused_asm_goto_source}"
          -o "${BINARY_DIR}/unused-asm-goto.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc failed to discard unused static-inline asm-goto function\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/unused-asm-goto.o" -o
          "${BINARY_DIR}/unused-asm-goto"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host compiler could not link unit after dead asm-goto removal\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/unused-asm-goto"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "dead asm-goto removal changed the live program result\n${stdout}${stderr}")
endif()
set(used_asm_goto_source "${SOURCE_DIR}/fixtures/used-asm-goto.c")

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
set(unused_operand_asm_source "${SOURCE_DIR}/fixtures/unused-operand-asm.c")

execute_process(
  COMMAND "${NYC}" -c "${unused_operand_asm_source}"
          -o "${BINARY_DIR}/unused-operand-asm.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc failed to discard an unused static-inline operand-asm function\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/unused-operand-asm.o" -o
          "${BINARY_DIR}/unused-operand-asm"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host compiler could not link after unused operand-asm removal\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/unused-operand-asm"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "dead asm-goto removal changed the live program result\n${stdout}${stderr}")
endif()
set(unit_inline_source "${SOURCE_DIR}/fixtures/unit-inline.c")

foreach(unit_inline_level IN ITEMS -O1 -O2)
  execute_process(
    COMMAND "${NYC}" "${unit_inline_level}" -S "${unit_inline_source}"
            -o "${BINARY_DIR}/unit-inline.s"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "nyc could not optimize an inline function in a C unit\n${stdout}${stderr}")
  endif()
  file(READ "${BINARY_DIR}/unit-inline.s" unit_inline_assembly)
  if(unit_inline_assembly MATCHES "call[ \t]+nyc_unit_inline_add")
    message(FATAL_ERROR
      "NYC left an always-available inline helper call in the C unit\n${unit_inline_assembly}")
  endif()
  execute_process(
    COMMAND "${CC}" "${BINARY_DIR}/unit-inline.s"
            -o "${BINARY_DIR}/unit-inline"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "host assembler/linker rejected optimized C-unit assembly\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${BINARY_DIR}/unit-inline"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "inlined C-unit helper changed runtime behavior\n${stdout}${stderr}")
  endif()
endforeach()
set(unit_asm_goto_source "${SOURCE_DIR}/fixtures/unit-asm-goto.c")

execute_process(
  COMMAND "${NYC}" -O2 -S "${unit_asm_goto_source}"
          -o "${BINARY_DIR}/unit-asm-goto.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc could not lower a call-site inlined jump-label asm goto\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/unit-asm-goto.s"
          -o "${BINARY_DIR}/unit-asm-goto"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "external assembler rejected NYC jump-label asm goto output\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/unit-asm-goto"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "NYC jump-label asm goto did not branch to the C label\n${stdout}${stderr}")
endif()
# Linux fixed-register exception-table assembly cases.
set(lkgs_extable_source "${SOURCE_DIR}/fixtures/linux-lkgs-extable.c")
execute_process(
  COMMAND "${NYC}" -S "${lkgs_extable_source}"
          -o "${BINARY_DIR}/linux-lkgs-extable.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "NYC failed to lower Linux LKGS exception-table asm\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" -c "${BINARY_DIR}/linux-lkgs-extable.s"
          -o "${BINARY_DIR}/linux-lkgs-extable.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host assembler rejected Linux LKGS exception-table asm\n${stdout}${stderr}")
endif()
set(loadsegment_extable_source
  "${SOURCE_DIR}/fixtures/linux-loadsegment-extable.c")
execute_process(
  COMMAND "${NYC}" -S "${loadsegment_extable_source}"
          -o "${BINARY_DIR}/linux-loadsegment-extable.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "NYC failed to lower Linux typed segment-load asm\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" -c "${BINARY_DIR}/linux-loadsegment-extable.s"
          -o "${BINARY_DIR}/linux-loadsegment-extable.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host assembler rejected Linux typed segment-load asm\n${stdout}${stderr}")
endif()
set(save_segment_source "${SOURCE_DIR}/fixtures/linux-save-segment.c")
execute_process(
  COMMAND "${NYC}" -S "${save_segment_source}"
          -o "${BINARY_DIR}/linux-save-segment.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "NYC failed to lower Linux segment-register read asm\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" -c "${BINARY_DIR}/linux-save-segment.s"
          -o "${BINARY_DIR}/linux-save-segment.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host assembler rejected Linux segment-register read asm\n${stdout}${stderr}")
endif()

set(static-cpu-has-asm-goto-source
  "${SOURCE_DIR}/fixtures/static-cpu-has-asm-goto.c")

execute_process(
  COMMAND "${NYC}" -O2 -S "${static-cpu-has-asm-goto-source}"
          -o "${BINARY_DIR}/static-cpu-has-asm-goto.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "NYC failed to lower static CPU feature asm goto\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/static-cpu-has-asm-goto.s"
          -o "${BINARY_DIR}/static-cpu-has-asm-goto"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host assembler rejected static CPU feature asm-goto lowering\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/static-cpu-has-asm-goto"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "static CPU feature asm-goto runtime behavior failed\n${stdout}${stderr}")
endif()
set(clear_pages_alternative_source
  "${SOURCE_DIR}/fixtures/linux-clear-pages-alternative.c")

execute_process(
  COMMAND "${NYC}" -O2 -S "${clear_pages_alternative_source}"
          -o "${BINARY_DIR}/linux-clear-pages-alternative.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "NYC failed to lower the Linux clear-pages alternative\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/linux-clear-pages-alternative.s"
          -o "${BINARY_DIR}/linux-clear-pages-alternative"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host assembler rejected Linux clear-pages alternative lowering\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/linux-clear-pages-alternative"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "Linux clear-pages alternative produced incorrect data\n${stdout}${stderr}")
endif()
set(clear_user_alternative_source
  "${SOURCE_DIR}/fixtures/linux-clear-user-alternative.c")
execute_process(
  COMMAND "${NYC}" -O2 -S "${clear_user_alternative_source}"
          -o "${BINARY_DIR}/linux-clear-user-alternative.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "NYC failed to lower the Linux clear-user alternative\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" -c "${BINARY_DIR}/linux-clear-user-alternative.s"
          -o "${BINARY_DIR}/linux-clear-user-alternative.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host assembler rejected Linux clear-user alternative lowering\n${stdout}${stderr}")
endif()

set(la57-alternative-source "${SOURCE_DIR}/fixtures/la57-alternative.c")

execute_process(
  COMMAND "${NYC}" -O2 -S "${la57-alternative-source}"
          -o "${BINARY_DIR}/la57-alternative.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "NYC failed to lower the x86 LA57 alternative\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/la57-alternative.s"
          -o "${BINARY_DIR}/la57-alternative"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host assembler rejected the x86 LA57 alternative lowering\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/la57-alternative"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "x86 LA57 alternative runtime behavior failed\n${stdout}${stderr}")
endif()
set(ibpb-alternative-source "${SOURCE_DIR}/fixtures/ibpb-alternative.c")

execute_process(
  COMMAND "${NYC}" -O2 -S "${ibpb-alternative-source}"
          -o "${BINARY_DIR}/ibpb-alternative.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "NYC failed to lower the x86 IBPB alternative\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/ibpb-alternative.s"
          -o "${BINARY_DIR}/ibpb-alternative"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host assembler rejected the x86 IBPB alternative lowering\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/ibpb-alternative"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "x86 IBPB alternative runtime behavior failed\n${stdout}${stderr}")
endif()
set(div0-alternative-source "${SOURCE_DIR}/fixtures/div0-alternative.c")

execute_process(
  COMMAND "${NYC}" -O2 -S "${div0-alternative-source}"
          -o "${BINARY_DIR}/div0-alternative.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "NYC failed to lower the x86 DIV0 alternative\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/div0-alternative.s"
          -o "${BINARY_DIR}/div0-alternative"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host assembler rejected the x86 DIV0 alternative lowering\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/div0-alternative"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "x86 DIV0 alternative runtime behavior failed\n${stdout}${stderr}")
endif()
set(atomic_add_negative_source
  "${SOURCE_DIR}/fixtures/linux-atomic-add-negative.c")
execute_process(
  COMMAND "${NYC}" -O2 -S "${atomic_add_negative_source}"
          -o "${BINARY_DIR}/linux-atomic-add-negative.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "NYC failed to lower locked add with a sign-flag output\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/linux-atomic-add-negative.s"
          -o "${BINARY_DIR}/linux-atomic-add-negative"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host linker rejected locked add with a sign-flag output\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/linux-atomic-add-negative"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "locked add sign-flag output returned an incorrect result\n${stdout}${stderr}")
endif()
set(spinlock_xadd_source "${SOURCE_DIR}/fixtures/linux-spinlock-xadd.c")
execute_process(
  COMMAND "${NYC}" -O2 -S "${spinlock_xadd_source}"
          -o "${BINARY_DIR}/linux-spinlock-xadd.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "NYC failed to lower xaddq with named read-write operands\n${stdout}${stderr}")
endif()
file(READ "${BINARY_DIR}/linux-spinlock-xadd.s" spinlock_xadd_assembly)
if(spinlock_xadd_assembly MATCHES "lock xaddq")
  message(FATAL_ERROR
    "NYC changed the unlocked Linux xaddq into a locked instruction")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/linux-spinlock-xadd.s"
          -o "${BINARY_DIR}/linux-spinlock-xadd"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host linker rejected xaddq named-operand lowering\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/linux-spinlock-xadd"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "xaddq named-operand runtime result was incorrect\n${stdout}${stderr}")
endif()
set(local-array-range-source
    "${SOURCE_DIR}/fixtures/linux-local-array-range-init.c")
execute_process(
  COMMAND "${NYC}" -O2 -S "${local-array-range-source}"
          -o "${BINARY_DIR}/linux-local-array-range-init.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc could not lower a local GNU array range initializer\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/linux-local-array-range-init.s"
          -o "${BINARY_DIR}/linux-local-array-range-init"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "external assembler rejected local array range output\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/linux-local-array-range-init"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "local array range initializer produced incorrect values\n${stdout}${stderr}")
endif()
set(user_store_goto_source
  "${SOURCE_DIR}/fixtures/linux-user-store-goto.c")
execute_process(
  COMMAND "${NYC}" -O2 -S "${user_store_goto_source}"
          -o "${BINARY_DIR}/linux-user-store-goto.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "NYC failed to lower fault-table asm goto stores\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/linux-user-store-goto.s"
          -o "${BINARY_DIR}/linux-user-store-goto"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host linker rejected fault-table asm goto lowering\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/linux-user-store-goto"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "fault-table asm goto store returned an incorrect result\n${stdout}${stderr}")
endif()
set(atomic_inc_asm_source "${SOURCE_DIR}/fixtures/atomic-inc-asm.c")

execute_process(
  COMMAND "${NYC}" -O2 -S "${atomic_inc_asm_source}"
          -o "${BINARY_DIR}/atomic-inc-asm.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc could not lower lock incq with tied memory constraints\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/atomic-inc-asm.s"
          -o "${BINARY_DIR}/atomic-inc-asm"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "external assembler rejected NYC lock incq output\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/atomic-inc-asm"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "NYC lock incq failed to update its memory operand\n${stdout}${stderr}")
endif()
set(atomic_and_asm_source "${SOURCE_DIR}/fixtures/atomic-and-asm.c")

execute_process(
  COMMAND "${NYC}" -O2 -S "${atomic_and_asm_source}"
          -o "${BINARY_DIR}/atomic-and-asm.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc could not lower lock andl with read-write memory constraints\n${stdout}${stderr}")
endif()
file(READ "${BINARY_DIR}/atomic-and-asm.s" atomic_and_assembly)
if(NOT atomic_and_assembly MATCHES "lock andl" OR
   NOT atomic_and_assembly MATCHES "lock orl" OR
   NOT atomic_and_assembly MATCHES "lock xorl" OR
   NOT atomic_and_assembly MATCHES "lock addl" OR
   NOT atomic_and_assembly MATCHES "lock subl")
  message(FATAL_ERROR "NYC omitted an atomic lock read-modify-write instruction")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/atomic-and-asm.s"
          -o "${BINARY_DIR}/atomic-and-asm"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "external assembler rejected NYC lock andl output\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/atomic-and-asm"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "NYC lock andl failed to update its memory operand\n${stdout}${stderr}")
endif()
set(atomic-xorb-condition-source "${SOURCE_DIR}/fixtures/atomic-xorb-condition.c")

execute_process(
  COMMAND "${NYC}" -O2 -S "${atomic-xorb-condition-source}"
          -o "${BINARY_DIR}/atomic-xorb-condition.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc could not lower locked byte xor with sign output\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/atomic-xorb-condition.s"
          -o "${BINARY_DIR}/atomic-xorb-condition"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "external assembler rejected NYC locked byte xor output\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/atomic-xorb-condition"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "locked byte xor sign result or memory update was incorrect\n${stdout}${stderr}")
endif()
set(cmpxchg64_asm_source "${SOURCE_DIR}/fixtures/cmpxchg64-asm.c")

execute_process(
  COMMAND "${NYC}" -O2 -S "${cmpxchg64_asm_source}"
          -o "${BINARY_DIR}/cmpxchg64-asm.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc could not lower 64-bit segment-relative cmpxchg asm\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/cmpxchg64-asm.s"
          -o "${BINARY_DIR}/cmpxchg64-asm"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "external assembler rejected NYC 64-bit cmpxchg output\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/cmpxchg64-asm"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "NYC 64-bit cmpxchg failed to update its memory operand\n${stdout}${stderr}")
endif()
set(gs-store-asm-source "${SOURCE_DIR}/fixtures/gs-store-asm.c")

execute_process(
  COMMAND "${NYC}" -O2 -S "${gs-store-asm-source}"
          -o "${BINARY_DIR}/gs-store-asm.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc could not lower write-only GS-relative memory asm\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/gs-store-asm.s"
          -o "${BINARY_DIR}/gs-store-asm"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "external assembler rejected write-only GS-relative memory asm\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/gs-store-asm"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "NYC GS-relative write-only memory asm failed\n${stdout}${stderr}")
endif()
set(index-mask-asm-source "${SOURCE_DIR}/fixtures/index-mask-asm.c")

execute_process(
  COMMAND "${NYC}" -O2 -S "${index-mask-asm-source}"
          -o "${BINARY_DIR}/index-mask-asm.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc could not lower unsigned index-mask asm\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/index-mask-asm.s"
          -o "${BINARY_DIR}/index-mask-asm"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "external assembler rejected unsigned index-mask asm\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/index-mask-asm"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "NYC unsigned index-mask asm produced incorrect bounds masks\n${stdout}${stderr}")
endif()
set(visibility-object-source "${SOURCE_DIR}/fixtures/visibility-object.c")

execute_process(
  COMMAND "${NYC}" -O2 -S "${visibility-object-source}"
          -o "${BINARY_DIR}/visibility-object.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc could not lower a hidden-visibility aggregate object\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/visibility-object.s"
          -o "${BINARY_DIR}/visibility-object"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "external assembler rejected a hidden-visibility aggregate object\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/visibility-object"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "NYC hidden-visibility aggregate object produced incorrect data\n${stdout}${stderr}")
endif()
set(pointer-initializer-field-source
    "${SOURCE_DIR}/fixtures/pointer-initializer-field.c")

execute_process(
  COMMAND "${NYC}" -O2 -S "${pointer-initializer-field-source}"
          -o "${BINARY_DIR}/pointer-initializer-field.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc could not lower a nested array-address pointer initializer\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/pointer-initializer-field.s"
          -o "${BINARY_DIR}/pointer-initializer-field"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "external assembler rejected a nested array-address pointer initializer\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/pointer-initializer-field"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "NYC nested array-address pointer initializer produced an incorrect relocation\n${stdout}${stderr}")
endif()
set(empty-read-write-asm-source "${SOURCE_DIR}/fixtures/empty-read-write-asm.c")

execute_process(
  COMMAND "${NYC}" -O2 -S "${empty-read-write-asm-source}"
          -o "${BINARY_DIR}/empty-read-write-asm.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc could not lower empty read-write asm\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/empty-read-write-asm.s"
          -o "${BINARY_DIR}/empty-read-write-asm"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "external assembler rejected empty read-write asm output\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/empty-read-write-asm"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "NYC empty read-write asm changed its operand\n${stdout}${stderr}")
endif()
set(futex-robust-asm-source "${SOURCE_DIR}/fixtures/futex-robust-asm.c")

execute_process(
  COMMAND "${NYC}" -O2 -S "${futex-robust-asm-source}"
          -o "${BINARY_DIR}/futex-robust-asm.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc could not lower robust-futex cmpxchg asm\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/futex-robust-asm.s"
          -o "${BINARY_DIR}/futex-robust-asm"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "external assembler rejected NYC robust-futex output\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/futex-robust-asm"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "NYC robust-futex unlock semantics failed\n${stdout}${stderr}")
endif()
set(rdtsc-asm-source "${SOURCE_DIR}/fixtures/rdtsc-asm.c")

execute_process(
  COMMAND "${NYC}" -O2 -S "${rdtsc-asm-source}"
          -o "${BINARY_DIR}/rdtsc-asm.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc could not lower x86 RDTSC output constraints\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/rdtsc-asm.s"
          -o "${BINARY_DIR}/rdtsc-asm"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "external assembler rejected NYC RDTSC output\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/rdtsc-asm"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "NYC RDTSC output did not produce a nonzero timestamp\n${stdout}${stderr}")
endif()
set(x64-narrow-load-source "${SOURCE_DIR}/fixtures/x64-narrow-load.c")

execute_process(
  COMMAND "${NYC}" -O2 -S "${x64-narrow-load-source}"
          -o "${BINARY_DIR}/x64-narrow-load.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc could not lower narrow x86-64 memory results\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/x64-narrow-load.s"
          -o "${BINARY_DIR}/x64-narrow-load"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "external assembler rejected narrow x86-64 memory results\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/x64-narrow-load"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "NYC narrow memory operations returned incorrect values\n${stdout}${stderr}")
endif()
set(typeof-string-source "${SOURCE_DIR}/fixtures/typeof-string.c")

execute_process(
  COMMAND "${NYC}" -O2 -S "${typeof-string-source}"
          -o "${BINARY_DIR}/typeof-string.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc could not resolve typeof(string literal) array type\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/typeof-string.s"
          -o "${BINARY_DIR}/typeof-string"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "external assembler rejected typeof string output\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/typeof-string"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "typeof string literal did not retain its array extent\n${stdout}${stderr}")
endif()
set(typeof-address-deref-pointer-local-source
    "${SOURCE_DIR}/fixtures/typeof-address-deref-pointer-local.c")

execute_process(
  COMMAND "${NYC}" -O2 -S "${typeof-address-deref-pointer-local-source}"
          -o "${BINARY_DIR}/typeof-address-deref-pointer-local.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc could not retain the pointer type in typeof(*&member)\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/typeof-address-deref-pointer-local.s"
          -o "${BINARY_DIR}/typeof-address-deref-pointer-local"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "external assembler rejected typeof(*&member) output\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/typeof-address-deref-pointer-local"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "typeof(*&void_pointer_member) did not preserve its pointer type\n${stdout}${stderr}")
endif()
set(generic-static-init-source "${SOURCE_DIR}/fixtures/generic-static-init.c")

execute_process(
  COMMAND "${NYC}" -O2 -S "${generic-static-init-source}"
          -o "${BINARY_DIR}/generic-static-init.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc could not resolve file-scope _Generic initializers or complete a tentative character array\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/generic-static-init.s"
          -o "${BINARY_DIR}/generic-static-init"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "external assembler rejected file-scope generic initializer output\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/generic-static-init"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "file-scope _Generic initializer selected incorrect data\n${stdout}${stderr}")
endif()
set(static-designated-array-source
    "${SOURCE_DIR}/fixtures/static-designated-array.c")

execute_process(
  COMMAND "${NYC}" -O2 -S "${static-designated-array-source}"
          -o "${BINARY_DIR}/static-designated-array.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc could not lower sparse file-scope integer array initializers\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/static-designated-array.s"
          -o "${BINARY_DIR}/static-designated-array"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "external assembler rejected designated integer array output\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/static-designated-array"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "sparse designated integer array values were not preserved\n${stdout}${stderr}")
endif()
set(static-designated-pointer-array-source
    "${SOURCE_DIR}/fixtures/static-designated-pointer-array.c")
execute_process(
  COMMAND "${NYC}" -O2 -S "${static-designated-pointer-array-source}"
          -o "${BINARY_DIR}/static-designated-pointer-array.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc could not lower a sparse file-scope pointer array\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/static-designated-pointer-array.s"
          -o "${BINARY_DIR}/static-designated-pointer-array"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "external assembler rejected designated pointer array output\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/static-designated-pointer-array"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "sparse designated pointer array relocations were incorrect\n${stdout}${stderr}")
endif()
set(global-generic-array-decay-source
    "${SOURCE_DIR}/fixtures/global-generic-array-decay.c")
execute_process(
  COMMAND "${NYC}" -O2 -S "${global-generic-array-decay-source}"
          -o "${BINARY_DIR}/global-generic-array-decay.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc could not decay an array in a file-scope _Generic control\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/global-generic-array-decay.s"
          -o "${BINARY_DIR}/global-generic-array-decay"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "external assembler rejected file-scope _Generic array output\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/global-generic-array-decay"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "file-scope _Generic did not select the decayed array pointer\n${stdout}${stderr}")
endif()
set(nested-scalar-pointer-store-source
    "${SOURCE_DIR}/fixtures/nested-scalar-pointer-store.c")
execute_process(
  COMMAND "${NYC}" -O2 -S "${nested-scalar-pointer-store-source}"
          -o "${BINARY_DIR}/nested-scalar-pointer-store.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc could not lower a scalar store through a nested pointer\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/nested-scalar-pointer-store.s"
          -o "${BINARY_DIR}/nested-scalar-pointer-store"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "external assembler rejected nested scalar pointer output\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/nested-scalar-pointer-store"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nested scalar pointer store wrote an incorrect value\n${stdout}${stderr}")
endif()
set(address-indexed-pointer-array-member-source
    "${SOURCE_DIR}/fixtures/address-indexed-pointer-array-member.c")
execute_process(
  COMMAND "${NYC}" -O2 -S "${address-indexed-pointer-array-member-source}"
          -o "${BINARY_DIR}/address-indexed-pointer-array-member.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc could not lower address-of an indexed pointer-array member\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/address-indexed-pointer-array-member.s"
          -o "${BINARY_DIR}/address-indexed-pointer-array-member"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "external assembler rejected indexed pointer-array member output\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/address-indexed-pointer-array-member"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "indexed pointer-array member address wrote an incorrect value\n${stdout}${stderr}")
endif()
set(static-aggregate-redeclaration-source
    "${SOURCE_DIR}/fixtures/static-aggregate-redeclaration.c")
execute_process(
  COMMAND "${NYC}" -O2 -S "${static-aggregate-redeclaration-source}"
          -o "${BINARY_DIR}/static-aggregate-redeclaration.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc could not accept a matching tentative aggregate redeclaration\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/static-aggregate-redeclaration.s"
          -o "${BINARY_DIR}/static-aggregate-redeclaration"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "external assembler rejected aggregate redeclaration output\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/static-aggregate-redeclaration"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "tentative aggregate redeclaration changed the defined object\n${stdout}${stderr}")
endif()
set(dynamic-alloca-source "${SOURCE_DIR}/fixtures/dynamic-alloca.c")

execute_process(
  COMMAND "${NYC}" -O2 -c "${dynamic-alloca-source}"
          -o "${BINARY_DIR}/dynamic-alloca.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc could not emit dynamic alloca object code\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/dynamic-alloca.o"
          -o "${BINARY_DIR}/dynamic-alloca"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "external linker rejected dynamic alloca object code\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/dynamic-alloca"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "dynamic alloca object-code smoke failed\n${stdout}${stderr}")
endif()
set(linux-frontend-forms-source "${SOURCE_DIR}/fixtures/linux-frontend-forms.c")

execute_process(
  COMMAND "${NYC}" -O2 -S "${linux-frontend-forms-source}"
          -o "${BINARY_DIR}/linux-frontend-forms.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc rejected Linux pointer, typedef-array, or compound-literal expressions\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/linux-frontend-forms.s"
          -o "${BINARY_DIR}/linux-frontend-forms"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "external assembler rejected Linux frontend regression output\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/linux-frontend-forms"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "Linux pointer, typedef-array, or compound-literal runtime regression failed\n${stdout}${stderr}")
endif()
set(used_operand_asm_source "${SOURCE_DIR}/fixtures/used-operand-asm.c")

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
  set(register_source "${SOURCE_DIR}/fixtures/global-register.c")

  execute_process(
    COMMAND "${NYC}" -c "${register_source}"
            -o "${BINARY_DIR}/global-register.o"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR "nyc rejected a global register declaration\n${stdout}${stderr}")
  endif()
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
  set(register_host_source "${SOURCE_DIR}/fixtures/global-register-host.c")

  execute_process(
    COMMAND "${CC}" -std=gnu11 -Wall -Wextra -Werror
            -o "${BINARY_DIR}/global-register-host"
            "${register_host_source}" "${BINARY_DIR}/global-register.o"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "host compiler could not link global-register typeof regression\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${BINARY_DIR}/global-register-host"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "global-register typeof regression failed at runtime\n${stdout}${stderr}")
  endif()
  set(register_call_constraint_source
    "${SOURCE_DIR}/fixtures/global-register-call-constraint.c")

  execute_process(
    COMMAND "${NYC}" -S "${register_call_constraint_source}"
            -o "${BINARY_DIR}/global-register-call-constraint.s"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "nyc rejected the x86 RSP read/write asm constraint\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${CC}" -c "${BINARY_DIR}/global-register-call-constraint.s"
            -o "${BINARY_DIR}/global-register-call-constraint.o"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "assembler rejected NYC's RSP call-constraint output\n${stdout}${stderr}")
  endif()
  set(register_call_constraint_host
    "${SOURCE_DIR}/fixtures/global-register-call-constraint-host.c")

  execute_process(
    COMMAND "${CC}" -std=gnu11 -Wall -Wextra -Werror
            -o "${BINARY_DIR}/global-register-call-constraint-host"
            "${register_call_constraint_host}"
            "${BINARY_DIR}/global-register-call-constraint.o"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "host compiler could not link the RSP call-constraint regression\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${BINARY_DIR}/global-register-call-constraint-host"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "RSP read/write call-constraint regression failed at runtime\n${stdout}${stderr}")
  endif()
  set(register_rsp_read_source "${SOURCE_DIR}/fixtures/global-register-rsp-read.c")

  execute_process(
    COMMAND "${NYC}" -O2 -S "${register_rsp_read_source}"
            -o "${BINARY_DIR}/global-register-rsp-read.s"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "nyc rejected a global RSP read\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${CC}" "${BINARY_DIR}/global-register-rsp-read.s"
            -o "${BINARY_DIR}/global-register-rsp-read"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "assembler rejected the global RSP read output\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${BINARY_DIR}/global-register-rsp-read"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "global RSP read returned a null or misaligned stack pointer\n${stdout}${stderr}")
  endif()
  set(asm_memory_output_source
    "${SOURCE_DIR}/fixtures/inline-asm-memory-output.c")

  execute_process(
    COMMAND "${NYC}" -S "${asm_memory_output_source}"
            -o "${BINARY_DIR}/inline-asm-memory-output.s"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "nyc rejected an x86 memory-capable asm output\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${CC}" -c "${BINARY_DIR}/inline-asm-memory-output.s"
            -o "${BINARY_DIR}/inline-asm-memory-output.o"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "assembler rejected NYC's memory asm output\n${stdout}${stderr}")
  endif()
  set(asm_memory_output_host
    "${SOURCE_DIR}/fixtures/inline-asm-memory-output-host.c")

  execute_process(
    COMMAND "${CC}" -std=gnu11 -Wall -Wextra -Werror
            -o "${BINARY_DIR}/inline-asm-memory-output-host"
            "${asm_memory_output_host}"
            "${BINARY_DIR}/inline-asm-memory-output.o"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "host compiler could not link the memory asm output regression\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${BINARY_DIR}/inline-asm-memory-output-host"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "memory asm output regression failed at runtime\n${stdout}${stderr}")
  endif()
  set(asm_tzcnt_source "${SOURCE_DIR}/fixtures/inline-asm-tzcnt.c")

  execute_process(
    COMMAND "${NYC}" -S "${asm_tzcnt_source}"
            -o "${BINARY_DIR}/inline-asm-tzcnt.s"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "nyc rejected the x86 tzcnt register-output asm\n${stdout}${stderr}")
  endif()
  file(READ "${BINARY_DIR}/inline-asm-tzcnt.s" asm_tzcnt_output)
  if(NOT asm_tzcnt_output MATCHES "tzcntq[ \t]+%rax, %rax")
    message(FATAL_ERROR
      "NYC did not lower tzcnt asm to the x86-64 instruction\n${asm_tzcnt_output}")
  endif()
  set(asm_memory_input_source
    "${SOURCE_DIR}/fixtures/inline-asm-memory-input-immediate.c")

  execute_process(
    COMMAND "${NYC}" -S "${asm_memory_input_source}"
            -o "${BINARY_DIR}/inline-asm-memory-input-immediate.s"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "nyc rejected an x86 immediate-plus-memory asm input\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${CC}" -c "${BINARY_DIR}/inline-asm-memory-input-immediate.s"
            -o "${BINARY_DIR}/inline-asm-memory-input-immediate.o"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "assembler rejected NYC's memory-input asm output\n${stdout}${stderr}")
  endif()
  set(asm_percpu_read_source
    "${SOURCE_DIR}/fixtures/inline-asm-percpu-read.c")

  execute_process(
    COMMAND "${NYC}" -S "${asm_percpu_read_source}"
            -o "${BINARY_DIR}/inline-asm-percpu-read.s"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "nyc rejected the x86-64 per-CPU read asm\n${stdout}${stderr}")
  endif()
  file(READ "${BINARY_DIR}/inline-asm-percpu-read.s" asm_percpu_read_output)
  if(NOT asm_percpu_read_output MATCHES
     "movq[ \t]+%gs:\\(%rax\\), %rax")
    message(FATAL_ERROR
      "NYC did not lower the x86-64 per-CPU read asm\n${asm_percpu_read_output}")
  endif()
  execute_process(
    COMMAND "${CC}" -c "${BINARY_DIR}/inline-asm-percpu-read.s"
            -o "${BINARY_DIR}/inline-asm-percpu-read.o"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "assembler rejected NYC's per-CPU read output\n${stdout}${stderr}")
  endif()
  set(asm_percpu_inc_source
    "${SOURCE_DIR}/fixtures/inline-asm-percpu-inc.c")

  execute_process(
    COMMAND "${NYC}" -c "${asm_percpu_inc_source}"
            -o "${BINARY_DIR}/inline-asm-percpu-inc.o"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "nyc rejected the x86-64 per-CPU increment asm\n${stdout}${stderr}")
  endif()
  set(asm_memory_input_host "${SOURCE_DIR}/fixtures/inline-asm-memory-input-host.c")

  execute_process(
    COMMAND "${CC}" -std=gnu11 -Wall -Wextra -Werror
            -o "${BINARY_DIR}/inline-asm-memory-input-host"
            "${asm_memory_input_host}"
            "${BINARY_DIR}/inline-asm-memory-input-immediate.o"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "host compiler could not link the memory-input asm regression\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${BINARY_DIR}/inline-asm-memory-input-host"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "memory-input asm regression failed at runtime\n${stdout}${stderr}")
  endif()
  set(asm_fixed_register_source
    "${SOURCE_DIR}/fixtures/inline-asm-fixed-register.c")

  execute_process(
    COMMAND "${NYC}" -S "${asm_fixed_register_source}"
            -o "${BINARY_DIR}/inline-asm-fixed-register.s"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "nyc rejected fixed-register inline-asm inputs\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${CC}" -c "${BINARY_DIR}/inline-asm-fixed-register.s"
            -o "${BINARY_DIR}/inline-asm-fixed-register.o"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "assembler rejected fixed-register inline-asm output\n${stdout}${stderr}")
  endif()
  set(asm_fixed_register_host
    "${SOURCE_DIR}/fixtures/inline-asm-fixed-register-host.c")

  execute_process(
    COMMAND "${CC}" -std=gnu11 -Wall -Wextra -Werror
            -o "${BINARY_DIR}/inline-asm-fixed-register-host"
            "${asm_fixed_register_host}"
            "${BINARY_DIR}/inline-asm-fixed-register.o"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "host compiler could not link the fixed-register asm regression\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${BINARY_DIR}/inline-asm-fixed-register-host"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "fixed-register asm inputs were not preserved at runtime\n${stdout}${stderr}")
  endif()
  set(asm_memory_input_source
    "${SOURCE_DIR}/fixtures/inline-asm-memory-input.c")

  execute_process(
    COMMAND "${NYC}" -S "${asm_memory_input_source}"
            -o "${BINARY_DIR}/inline-asm-memory-input.s"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "nyc rejected a standalone inline-asm memory input\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${CC}" -c "${BINARY_DIR}/inline-asm-memory-input.s"
            -o "${BINARY_DIR}/inline-asm-memory-input.o"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "assembler rejected inline-asm memory input output\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${CC}" -o "${BINARY_DIR}/inline-asm-memory-input"
            "${BINARY_DIR}/inline-asm-memory-input.o"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "host linker rejected inline-asm memory input output\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${BINARY_DIR}/inline-asm-memory-input"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "inline-asm memory input produced an incorrect runtime value\n${stdout}${stderr}")
  endif()
  set(asm_memory_register_input_source
    "${SOURCE_DIR}/fixtures/inline-asm-memory-register-input.c")

  execute_process(
    COMMAND "${NYC}" -S "${asm_memory_register_input_source}"
            -o "${BINARY_DIR}/inline-asm-memory-register-input.s"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "nyc rejected inline-asm memory and register inputs\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${CC}" -c
            "${BINARY_DIR}/inline-asm-memory-register-input.s"
            -o "${BINARY_DIR}/inline-asm-memory-register-input.o"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "assembler rejected inline-asm memory/register output\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${CC}" -o "${BINARY_DIR}/inline-asm-memory-register-input"
            "${BINARY_DIR}/inline-asm-memory-register-input.o"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "host linker rejected inline-asm memory/register output\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${BINARY_DIR}/inline-asm-memory-register-input"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "inline-asm memory/register inputs produced an incorrect value\n${stdout}${stderr}")
  endif()
  set(asm_carry_output_source
    "${SOURCE_DIR}/fixtures/inline-asm-carry-output.c")

  execute_process(
    COMMAND "${NYC}" -S "${asm_carry_output_source}"
            -o "${BINARY_DIR}/inline-asm-carry-output.s"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "nyc rejected carry-flag inline-asm output\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${CC}" "${BINARY_DIR}/inline-asm-carry-output.s" -o
            "${BINARY_DIR}/inline-asm-carry-output"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "host linker rejected carry-flag inline-asm output\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${BINARY_DIR}/inline-asm-carry-output"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "carry-flag inline-asm output returned an incorrect result\n${stdout}${stderr}")
  endif()
  set(asm_register_output_source
    "${SOURCE_DIR}/fixtures/inline-asm-register-output.c")

  execute_process(
    COMMAND "${NYC}" -S "${asm_register_output_source}"
            -o "${BINARY_DIR}/inline-asm-register-output.s"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "nyc rejected an unnamed register output constraint\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${CC}" "${BINARY_DIR}/inline-asm-register-output.s" -o
            "${BINARY_DIR}/inline-asm-register-output"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "host linker rejected register-output assembly\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${BINARY_DIR}/inline-asm-register-output"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "unnamed register output returned an incorrect result\n${stdout}${stderr}")
  endif()
  set(asm_memory_output_source
    "${SOURCE_DIR}/fixtures/inline-asm-memory-output-unnamed.c")

  execute_process(
    COMMAND "${NYC}" -S "${asm_memory_output_source}"
            -o "${BINARY_DIR}/inline-asm-memory-output-unnamed.s"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "nyc rejected an unnamed read-write memory operand\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${CC}" "${BINARY_DIR}/inline-asm-memory-output-unnamed.s" -o
            "${BINARY_DIR}/inline-asm-memory-output-unnamed"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "host linker rejected read-write memory output assembly\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${BINARY_DIR}/inline-asm-memory-output-unnamed"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "read-write memory output did not update its operand\n${stdout}${stderr}")
  endif()
  set(asm_named_carry_output_source
    "${SOURCE_DIR}/fixtures/inline-asm-named-carry-output.c")

  execute_process(
    COMMAND "${NYC}" -S "${asm_named_carry_output_source}"
            -o "${BINARY_DIR}/inline-asm-named-carry-output.s"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "nyc rejected named memory and carry-flag asm outputs\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${CC}" "${BINARY_DIR}/inline-asm-named-carry-output.s" -o
            "${BINARY_DIR}/inline-asm-named-carry-output"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "host linker rejected named carry-output assembly\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${BINARY_DIR}/inline-asm-named-carry-output"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "named carry-output assembly returned an incorrect result\n${stdout}${stderr}")
  endif()
  set(asm_nonzero_memory_output_source
    "${SOURCE_DIR}/fixtures/inline-asm-nonzero-memory-output.c")

  execute_process(
    COMMAND "${NYC}" -S "${asm_nonzero_memory_output_source}"
            -o "${BINARY_DIR}/inline-asm-nonzero-memory-output.s"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "nyc rejected nonzero-flag memory asm input\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${CC}" "${BINARY_DIR}/inline-asm-nonzero-memory-output.s" -o
            "${BINARY_DIR}/inline-asm-nonzero-memory-output"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "host linker rejected nonzero-flag assembly\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${BINARY_DIR}/inline-asm-nonzero-memory-output"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "nonzero-flag memory asm returned an incorrect result\n${stdout}${stderr}")
  endif()
  set(parenthesized_function_pointer_source
    "${SOURCE_DIR}/fixtures/parenthesized-function-pointer.c")

  execute_process(
    COMMAND "${NYC}" -S "${parenthesized_function_pointer_source}"
            -o "${BINARY_DIR}/parenthesized-function-pointer.s"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "nyc rejected a parenthesized file-scope function pointer\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${CC}" -c "${BINARY_DIR}/parenthesized-function-pointer.s"
            -o "${BINARY_DIR}/parenthesized-function-pointer.o"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "assembler rejected parenthesized function-pointer relocation\n${stdout}${stderr}")
  endif()
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
    "${SOURCE_DIR}/fixtures/typeof-function-call.c")

  execute_process(
    COMMAND "${NYC}" -S "${typeof_function_call_source}"
            -o "${BINARY_DIR}/typeof-function-call.s"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "nyc rejected a typeof function-pointer call\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${CC}" "${BINARY_DIR}/typeof-function-call.s" -o
            "${BINARY_DIR}/typeof-function-call"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "host linker rejected typeof function-pointer call\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${BINARY_DIR}/typeof-function-call"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "typeof function-pointer call returned an incorrect result\n${stdout}${stderr}")
  endif()
  set(static_self_reference_source
    "${SOURCE_DIR}/fixtures/static-self-reference.c")

  execute_process(
    COMMAND "${NYC}" -S "${static_self_reference_source}"
            -o "${BINARY_DIR}/static-self-reference.s"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "nyc rejected a self-referential static aggregate\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${CC}" "${BINARY_DIR}/static-self-reference.s" -o
            "${BINARY_DIR}/static-self-reference"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "host linker rejected static self-reference assembly\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${BINARY_DIR}/static-self-reference"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "static aggregate self-relocations point to the wrong object\n${stdout}${stderr}")
  endif()
  set(asm_aggregate_memory_source
    "${SOURCE_DIR}/fixtures/inline-asm-aggregate-memory.c")

  execute_process(
    COMMAND "${NYC}" -S "${asm_aggregate_memory_source}"
            -o "${BINARY_DIR}/inline-asm-aggregate-memory.s"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "nyc rejected an aggregate inline-asm memory input\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${CC}" -c "${BINARY_DIR}/inline-asm-aggregate-memory.s"
            -o "${BINARY_DIR}/inline-asm-aggregate-memory.o"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "assembler rejected aggregate inline-asm memory output\n${stdout}${stderr}")
  endif()
  set(asm_named_immediate_source
    "${SOURCE_DIR}/fixtures/inline-asm-named-immediate.c")

  execute_process(
    COMMAND "${NYC}" -S "${asm_named_immediate_source}"
            -o "${BINARY_DIR}/inline-asm-named-immediate.s"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "nyc rejected named inline-asm immediate operands\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${CC}" "${BINARY_DIR}/inline-asm-named-immediate.s"
            -o "${BINARY_DIR}/inline-asm-named-immediate"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "assembler/linker rejected named inline-asm output\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${BINARY_DIR}/inline-asm-named-immediate"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "named inline-asm immediate produced an incorrect value\n${stdout}${stderr}")
  endif()
  set(aggregate_matrix_source
    "${SOURCE_DIR}/fixtures/aggregate-matrix.c")

  execute_process(
    COMMAND "${NYC}" -S "${aggregate_matrix_source}"
            -o "${BINARY_DIR}/aggregate-matrix.s"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "nyc rejected multidimensional aggregate indexing\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${CC}" -c "${BINARY_DIR}/aggregate-matrix.s"
            -o "${BINARY_DIR}/aggregate-matrix.o"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "assembler rejected multidimensional aggregate output\n${stdout}${stderr}")
  endif()
  set(aggregate_matrix_host
    "${SOURCE_DIR}/fixtures/aggregate-matrix-host.c")

  execute_process(
    COMMAND "${CC}" -std=gnu11 -Wall -Wextra -Werror
            -o "${BINARY_DIR}/aggregate-matrix-host"
            "${aggregate_matrix_host}" "${BINARY_DIR}/aggregate-matrix.o"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "host could not link multidimensional aggregate regression\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${BINARY_DIR}/aggregate-matrix-host"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "multidimensional aggregate indexing returned the wrong element\n${stdout}${stderr}")
  endif()
  set(local_pointer_to_array_source
    "${SOURCE_DIR}/fixtures/local-pointer-to-array.c")

  execute_process(
    COMMAND "${NYC}" -S "${local_pointer_to_array_source}"
            -o "${BINARY_DIR}/local-pointer-to-array.s"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "nyc rejected a local pointer-to-array declaration\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${CC}" "${BINARY_DIR}/local-pointer-to-array.s" -o
            "${BINARY_DIR}/local-pointer-to-array"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "host linker rejected local pointer-to-array output\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${BINARY_DIR}/local-pointer-to-array"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "local pointer-to-array indexing returned the wrong element\n${stdout}${stderr}")
  endif()
  set(pointer_to_array_source
    "${SOURCE_DIR}/fixtures/pointer-to-array-field.c")

  execute_process(
    COMMAND "${NYC}" -S "${pointer_to_array_source}"
            -o "${BINARY_DIR}/pointer-to-array-field.s"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "nyc rejected pointer-to-array aggregate fields\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${CC}" -c "${BINARY_DIR}/pointer-to-array-field.s"
            -o "${BINARY_DIR}/pointer-to-array-field.o"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "assembler rejected pointer-to-array field output\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${CC}" -o "${BINARY_DIR}/pointer-to-array-field"
            "${BINARY_DIR}/pointer-to-array-field.o"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "host linker rejected pointer-to-array field output\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${BINARY_DIR}/pointer-to-array-field"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "pointer-to-array field size was incorrect\n${stdout}${stderr}")
  endif()
  set(noinstr_attribute_source
    "${SOURCE_DIR}/fixtures/noinstr-attributes.c")

  execute_process(
    COMMAND "${NYC}" -S "${noinstr_attribute_source}"
            -o "${BINARY_DIR}/noinstr-attributes.s"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "nyc rejected Linux noinstr attributes\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${CC}" -c "${BINARY_DIR}/noinstr-attributes.s"
            -o "${BINARY_DIR}/noinstr-attributes.o"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "assembler rejected Linux noinstr attribute output\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${CC}" -o "${BINARY_DIR}/noinstr-attributes"
            "${BINARY_DIR}/noinstr-attributes.o"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "host linker rejected Linux noinstr attribute output\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${BINARY_DIR}/noinstr-attributes"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "Linux noinstr attribute function returned an incorrect value\n${stdout}${stderr}")
  endif()
  set(aggregate_compound_source
    "${SOURCE_DIR}/fixtures/aggregate-compound-literal.c")

  execute_process(
    COMMAND "${NYC}" -S "${aggregate_compound_source}"
            -o "${BINARY_DIR}/aggregate-compound-literal.s"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "nyc rejected a runtime aggregate compound literal\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${CC}" -c "${BINARY_DIR}/aggregate-compound-literal.s"
            -o "${BINARY_DIR}/aggregate-compound-literal.o"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "assembler rejected NYC's aggregate compound-literal output\n${stdout}${stderr}")
  endif()
  set(aggregate_compound_host
    "${SOURCE_DIR}/fixtures/aggregate-compound-literal-host.c")

  execute_process(
    COMMAND "${CC}" -std=gnu11 -Wall -Wextra -Werror
            -o "${BINARY_DIR}/aggregate-compound-literal-host"
            "${aggregate_compound_host}"
            "${BINARY_DIR}/aggregate-compound-literal.o"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "host compiler could not link the aggregate compound-literal regression\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${BINARY_DIR}/aggregate-compound-literal-host"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "aggregate compound-literal regression failed at runtime\n${stdout}${stderr}")
  endif()
  set(register_pointer_source "${SOURCE_DIR}/fixtures/global-register-pointer.c")

  execute_process(
    COMMAND "${NYC}" -c "${register_pointer_source}"
            -o "${BINARY_DIR}/global-register-pointer.o"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "nyc rejected a pointer-typed global RSP read\n${stdout}${stderr}")
  endif()
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
      "${SOURCE_DIR}/fixtures/global-register-pointer-host.c")

  execute_process(
    COMMAND "${CC}" -std=gnu11 -Wall -Wextra -Werror
            -o "${BINARY_DIR}/global-register-pointer-host"
            "${register_pointer_host_source}"
            "${BINARY_DIR}/global-register-pointer.o"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "host compiler could not link pointer-typed global RSP regression\n${stdout}${stderr}")
  endif()
  execute_process(
    COMMAND "${BINARY_DIR}/global-register-pointer-host"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
      "pointer-typed global RSP regression failed at runtime\n${stdout}${stderr}")
  endif()
endif()
# Kernel data sections, register variables, and low-level runtime helpers.
set(register_use_source "${SOURCE_DIR}/fixtures/global-register-use.c")

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
set(register_write_source "${SOURCE_DIR}/fixtures/global-register-write.c")

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
set(register_dup_source "${SOURCE_DIR}/fixtures/global-register-dup.c")

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
set(register_non_rsp_source "${SOURCE_DIR}/fixtures/global-register-non-rsp.c")

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
set(register_init_source "${SOURCE_DIR}/fixtures/global-register-init.c")

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
set(initdata_source "${SOURCE_DIR}/fixtures/initdata-array.c")

execute_process(
  COMMAND "${NYC}" -c "${initdata_source}"
          -o "${BINARY_DIR}/initdata-array.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc rejected an extern char array with a mid-position section attribute\n${stdout}${stderr}")
endif()
set(initdata_harness "${SOURCE_DIR}/fixtures/initdata-array-main.c")

execute_process(
  COMMAND "${CC}" "${initdata_harness}" "${BINARY_DIR}/initdata-array.o" -o
          "${BINARY_DIR}/initdata-array-host"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "reference compiler could not link an NYRI extern char-array reference"
    "\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/initdata-array-host"
  RESULT_VARIABLE status
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "NYC extern char-array reference resolved incorrectly")
endif()
set(function_section_source "${SOURCE_DIR}/fixtures/function-section.c")

execute_process(
  COMMAND "${NYC}" -c "${function_section_source}"
          -o "${BINARY_DIR}/function-section.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc named function section compilation failed\n${stdout}${stderr}")
endif()
if(OBJDUMP)
  execute_process(
    COMMAND "${OBJDUMP}" -h "${BINARY_DIR}/function-section.o"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0 OR NOT stdout MATCHES "\\.probe\\.text")
    message(FATAL_ERROR
      "NYC function did not land in its requested ELF section\n${stdout}${stderr}")
  endif()
endif()
set(function_section_harness "${SOURCE_DIR}/fixtures/function-section-main.c")

execute_process(
  COMMAND "${CC}" "${function_section_harness}"
          "${BINARY_DIR}/function-section.o" -o
          "${BINARY_DIR}/function-section-host"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host compiler could not link the named function section object\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/function-section-host"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "NYC named function section execution returned a wrong value")
endif()
set(section_conflict_source "${SOURCE_DIR}/fixtures/section-conflict.c")

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
set(section_conflict_source "${SOURCE_DIR}/fixtures/section-conflict-defined.c")

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
set(kernel_model_source "${SOURCE_DIR}/kernel-model-range.c")
execute_process(
  COMMAND "${NYC}" -c -mcmodel=kernel "${kernel_model_source}"
          -o "${BINARY_DIR}/kernel-model-range.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc kernel code-model object compilation failed\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${LD}" -m elf_x86_64 -Ttext=0xffffffff81000000
          -Tdata=0xffffffffa0000000
          -o "${BINARY_DIR}/kernel-model-in-range.elf"
          "${BINARY_DIR}/kernel-model-range.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "kernel-model PC-relative reference within range failed to link\n${stdout}${stderr}")
endif()
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
set(frame_address_source "${SOURCE_DIR}/fixtures/frame-address.c")

execute_process(
  COMMAND "${NYC}" -c "${frame_address_source}"
          -o "${BINARY_DIR}/frame-address.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "frame-address builtin compilation failed\n${stdout}${stderr}")
endif()
set(frame_address_harness "${SOURCE_DIR}/fixtures/frame-address-main.c")

execute_process(
  COMMAND "${CC}" "${frame_address_harness}"
          "${BINARY_DIR}/frame-address.o" -o
          "${BINARY_DIR}/frame-address-host"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host compiler could not link frame-address regression\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/frame-address-host"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "NYC frame-address builtin returned an invalid frame address\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${NYC}" -O1 -S "${frame_address_source}"
          -o "${BINARY_DIR}/frame-address-optimized.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "optimized NYC assembly failed for frame-address builtin\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${frame_address_harness}"
          "${BINARY_DIR}/frame-address-optimized.s" -o
          "${BINARY_DIR}/frame-address-optimized-host"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host compiler could not link optimized frame-address output\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/frame-address-optimized-host"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "optimized NYC frame-address output returned an invalid frame address\n${stdout}${stderr}")
endif()

set(linux_type_source "${SOURCE_DIR}/fixtures/linux-type-regressions.c")

execute_process(
  COMMAND "${NYC}" -c "${linux_type_source}"
          -o "${BINARY_DIR}/linux-type-regressions.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "Linux type frontend regression compilation failed\n${stdout}${stderr}")
endif()
set(linux_type_harness "${SOURCE_DIR}/fixtures/linux-type-regressions-main.c")

execute_process(
  COMMAND "${CC}" "${linux_type_harness}"
          "${BINARY_DIR}/linux-type-regressions.o" -o
          "${BINARY_DIR}/linux-type-regressions-host"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host compiler could not link Linux type regressions\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/linux-type-regressions-host"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "Linux frontend regression executable returned wrong results\n${stdout}${stderr}")
endif()
set(get_user_asm_source "${SOURCE_DIR}/fixtures/get-user-inline-asm.c")
execute_process(
  COMMAND "${NYC}" -S "${get_user_asm_source}"
          -o "${BINARY_DIR}/get-user-inline-asm.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "get_user inline assembly lowering failed\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" -c "${BINARY_DIR}/get-user-inline-asm.s"
          -o "${BINARY_DIR}/get-user-inline-asm.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "get_user inline assembly was not accepted by assembler\n${stdout}${stderr}")
endif()
set(function_alias_source "${SOURCE_DIR}/fixtures/function-alias.c")

execute_process(
  COMMAND "${NYC}" -S "${function_alias_source}"
          -o "${BINARY_DIR}/function-alias.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc failed to emit a GNU function alias\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/function-alias.s"
          -o "${BINARY_DIR}/function-alias"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host assembler/linker rejected GNU function alias output\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/function-alias"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "GNU function alias returned an incorrect result\n${stdout}${stderr}")
endif()
set(asm_readwrite_memory_source
    "${SOURCE_DIR}/fixtures/asm-readwrite-memory.c")

execute_process(
  COMMAND "${NYC}" -S "${asm_readwrite_memory_source}"
          -o "${BINARY_DIR}/asm-readwrite-memory.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc failed to compile read-write register/memory asm\n${stdout}${stderr}")
endif()
set(asm_readwrite_memory_harness
    "${SOURCE_DIR}/fixtures/asm-readwrite-memory-main.c")

execute_process(
  COMMAND "${CC}" "${asm_readwrite_memory_harness}"
          "${BINARY_DIR}/asm-readwrite-memory.s"
          -o "${BINARY_DIR}/asm-readwrite-memory"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host assembler/linker rejected read-write register/memory asm\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/asm-readwrite-memory"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "read-write register/memory asm runtime regression failed\n${stdout}${stderr}")
endif()
set(asm_cmpxchg_source "${SOURCE_DIR}/fixtures/asm-cmpxchg.c")

execute_process(
  COMMAND "${NYC}" -S "${asm_cmpxchg_source}"
          -o "${BINARY_DIR}/asm-cmpxchg.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc failed to compile compare-exchange asm\n${stdout}${stderr}")
endif()
set(asm_cmpxchg_harness "${SOURCE_DIR}/fixtures/asm-cmpxchg-main.c")

execute_process(
  COMMAND "${CC}" "${asm_cmpxchg_harness}" "${BINARY_DIR}/asm-cmpxchg.s"
          -o "${BINARY_DIR}/asm-cmpxchg"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host assembler/linker rejected compare-exchange asm\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/asm-cmpxchg"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "compare-exchange asm runtime regression failed\n${stdout}${stderr}")
endif()
set(sizeof_unparenthesized_source "${SOURCE_DIR}/fixtures/sizeof-unparenthesized.c")

execute_process(
  COMMAND "${NYC}" -S "${sizeof_unparenthesized_source}"
          -o "${BINARY_DIR}/sizeof-unparenthesized.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc failed to compile unparenthesized sizeof expression\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/sizeof-unparenthesized.s"
          -o "${BINARY_DIR}/sizeof-unparenthesized"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host assembler/linker rejected unparenthesized sizeof expression\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/sizeof-unparenthesized"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "unparenthesized sizeof expression runtime regression failed\n${stdout}${stderr}")
endif()
set(asm_sub_cce_source "${SOURCE_DIR}/fixtures/asm-sub-cce.c")

execute_process(
  COMMAND "${NYC}" -S "${asm_sub_cce_source}"
          -o "${BINARY_DIR}/asm-sub-cce.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc failed to compile er/memory condition asm\n${stdout}${stderr}")
endif()
set(asm_sub_cce_harness "${SOURCE_DIR}/fixtures/asm-sub-cce-main.c")

execute_process(
  COMMAND "${CC}" "${asm_sub_cce_harness}" "${BINARY_DIR}/asm-sub-cce.s"
          -o "${BINARY_DIR}/asm-sub-cce"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host assembler/linker rejected er/memory condition asm\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/asm-sub-cce"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "er/memory condition asm runtime regression failed\n${stdout}${stderr}")
endif()
set(asm_external_memory_source "${SOURCE_DIR}/fixtures/asm-external-memory.c")

execute_process(
  COMMAND "${NYC}" -S "${asm_external_memory_source}"
          -o "${BINARY_DIR}/asm-external-memory.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc failed to emit an external global memory asm input\n${stdout}${stderr}")
endif()
set(asm_external_memory_harness
    "${SOURCE_DIR}/fixtures/asm-external-memory-main.c")

execute_process(
  COMMAND "${CC}" "${asm_external_memory_harness}"
          "${BINARY_DIR}/asm-external-memory.s"
          -o "${BINARY_DIR}/asm-external-memory"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host assembler/linker rejected external memory asm input\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/asm-external-memory"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "external global memory asm input returned an incorrect value\n${stdout}${stderr}")
endif()
set(shadowed-function-pointer-source
    "${SOURCE_DIR}/fixtures/shadowed-function-pointer.c")

execute_process(
  COMMAND "${NYC}" -S "${shadowed-function-pointer-source}"
          -o "${BINARY_DIR}/shadowed-function-pointer.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc failed to preserve shadowed function and aggregate pointer types\n${stdout}${stderr}")
endif()
set(shadowed-function-pointer-harness
    "${SOURCE_DIR}/fixtures/shadowed-function-pointer-main.c")

execute_process(
  COMMAND "${CC}" "${shadowed-function-pointer-harness}"
          "${BINARY_DIR}/shadowed-function-pointer.s"
          -o "${BINARY_DIR}/shadowed-function-pointer"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host compiler could not link shadowed function pointer test\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/shadowed-function-pointer"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "shadowed function pointer or typeof pointer returned an incorrect value\n${stdout}${stderr}")
endif()
set(xchg-widths-source "${SOURCE_DIR}/fixtures/xchg-widths.c")

execute_process(
  COMMAND "${NYC}" -S "${xchg-widths-source}"
          -o "${BINARY_DIR}/xchg-widths.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc could not lower 8/16/32/64-bit xchg asm\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/xchg-widths.s"
          -o "${BINARY_DIR}/xchg-widths"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "external assembler rejected NYC xchg width output\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/xchg-widths"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "NYC xchg width lowering returned or stored incorrect values\n${stdout}${stderr}")
endif()
set(get-user-asm-source "${SOURCE_DIR}/fixtures/get-user-asm.c")

execute_process(
  COMMAND "${NYC}" -S "${get-user-asm-source}"
          -o "${BINARY_DIR}/get-user-asm.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc failed to lower get_user helper inline assembly\n${stdout}${stderr}")
endif()
set(get-user-asm-stub "${SOURCE_DIR}/fixtures/get-user-asm-stub.s")

set(get-user-asm-harness "${SOURCE_DIR}/fixtures/get-user-asm-main.c")

execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/get-user-asm.s"
          "${get-user-asm-stub}" "${get-user-asm-harness}"
          -o "${BINARY_DIR}/get-user-asm"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host compiler could not link get_user helper inline asm\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/get-user-asm"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "get_user helper outputs were not preserved through the call\n${stdout}${stderr}")
endif()
set(put-user-asm-source "${SOURCE_DIR}/fixtures/put-user-asm.c")

execute_process(
  COMMAND "${NYC}" -S "${put-user-asm-source}"
          -o "${BINARY_DIR}/put-user-asm.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc failed to lower put_user helper inline assembly\n${stdout}${stderr}")
endif()
set(put-user-asm-stub "${SOURCE_DIR}/fixtures/put-user-asm-stub.s")

set(put-user-asm-harness "${SOURCE_DIR}/fixtures/put-user-asm-main.c")

execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/put-user-asm.s"
          "${put-user-asm-stub}" "${put-user-asm-harness}"
          -o "${BINARY_DIR}/put-user-asm"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host compiler could not link put_user helper inline asm\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/put-user-asm"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "put_user helper arguments or return value were not preserved\n${stdout}${stderr}")
endif()
set(bsr-asm-source "${SOURCE_DIR}/fixtures/bsr-asm.c")

execute_process(
  COMMAND "${NYC}" -S "${bsr-asm-source}" -o "${BINARY_DIR}/bsr-asm.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc failed to lower read-write bsr inline assembly\n${stdout}${stderr}")
endif()
set(bsr-asm-harness "${SOURCE_DIR}/fixtures/bsr-asm-main.c")

execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/bsr-asm.s" "${bsr-asm-harness}"
          -o "${BINARY_DIR}/bsr-asm"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host compiler could not link bsr inline asm regression\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/bsr-asm"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "bsr inline asm returned incorrect zero or bit-index results\n${stdout}${stderr}")
endif()
set(hweight-asm-source "${SOURCE_DIR}/fixtures/hweight-asm.c")

execute_process(
  COMMAND "${NYC}" -S "${hweight-asm-source}" -o "${BINARY_DIR}/hweight-asm.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc failed to lower read-write hweight inline assembly\n${stdout}${stderr}")
endif()
set(hweight-asm-harness "${SOURCE_DIR}/fixtures/hweight-asm-main.c")

execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/hweight-asm.s" "${hweight-asm-harness}"
          -o "${BINARY_DIR}/hweight-asm"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host compiler could not link hweight inline asm regression\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/hweight-asm"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "hweight inline asm returned incorrect counts\n${stdout}${stderr}")
endif()
set(cpuid-asm-source "${SOURCE_DIR}/fixtures/cpuid-asm.c")

execute_process(
  COMMAND "${NYC}" -S "${cpuid-asm-source}" -o "${BINARY_DIR}/cpuid-asm.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc failed to lower CPUID inline assembly\n${stdout}${stderr}")
endif()
set(cpuid-asm-harness "${SOURCE_DIR}/fixtures/cpuid-asm-main.c")

execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/cpuid-asm.s" "${cpuid-asm-harness}"
          -o "${BINARY_DIR}/cpuid-asm"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host compiler could not link CPUID inline asm regression\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/cpuid-asm"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "NYC CPUID outputs differed from host CPUID\n${stdout}${stderr}")
endif()
set(rdmsr-asm-source "${SOURCE_DIR}/fixtures/rdmsr-asm.c")

execute_process(
  COMMAND "${NYC}" -S "${rdmsr-asm-source}" -o "${BINARY_DIR}/rdmsr-asm.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc failed to lower rdmsr inline assembly\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" -m64 -c "${BINARY_DIR}/rdmsr-asm.s"
          -o "${BINARY_DIR}/rdmsr-asm.o"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "assembler rejected lowered rdmsr and exception table\n${stdout}${stderr}")
endif()
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
set(local-add-asm-source "${SOURCE_DIR}/fixtures/local-add-asm.c")

execute_process(
  COMMAND "${NYC}" -S "${local-add-asm-source}"
          -o "${BINARY_DIR}/local-add-asm.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc failed to lower read-write memory add asm\n${stdout}${stderr}")
endif()
set(local-add-asm-harness "${SOURCE_DIR}/fixtures/local-add-asm-main.c")

execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/local-add-asm.s"
          "${local-add-asm-harness}" -o "${BINARY_DIR}/local-add-asm"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host compiler could not link read-write memory add asm\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/local-add-asm"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "read-write memory add asm computed an incorrect counter\n${stdout}${stderr}")
endif()
set(mulq_addcarry_source "${SOURCE_DIR}/fixtures/asm-mulq-addcarry.c")
execute_process(
  COMMAND "${NYC}" -S "${mulq_addcarry_source}"
          -o "${BINARY_DIR}/asm-mulq-addcarry.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc failed to lower mulq/add-carry assembly\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/asm-mulq-addcarry.s"
          "${SOURCE_DIR}/fixtures/asm-mulq-addcarry-main.c"
          -o "${BINARY_DIR}/asm-mulq-addcarry"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host compiler could not link mulq/add-carry fixture\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/asm-mulq-addcarry"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "mulq/add-carry fixture returned an incorrect result\n${stdout}${stderr}")
endif()
set(typedef_scope_source "${SOURCE_DIR}/fixtures/typedef-scope-index.c")

execute_process(
  COMMAND "${NYC}" -S "${typedef_scope_source}"
          -o "${BINARY_DIR}/typedef-scope-index.s"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "nyc failed to parse typedefs across a block scope\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${CC}" "${BINARY_DIR}/typedef-scope-index.s"
          -o "${BINARY_DIR}/typedef-scope-index"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "host compiler could not link typedef scope regression\n${stdout}${stderr}")
endif()
execute_process(
  COMMAND "${BINARY_DIR}/typedef-scope-index"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "typedef lookup returned the wrong alias after scope exit\n${stdout}${stderr}")
endif()
