if(NOT EXISTS "${NYC}")
  message(FATAL_ERROR "nyc compiler does not exist: ${NYC}")
endif()
if(NOT EXISTS "${CC}")
  message(FATAL_ERROR "C reference compiler does not exist: ${CC}")
endif()

file(MAKE_DIRECTORY "${BINARY_DIR}")

set(cases
  "program.c|c_kernel|program.o"
  "target-link.c|c_target_link|target-link.o"
  "void.c|c_void_store|void-store.o"
  "void.c|c_void_store_return|void-store-return.o"
  "void.c|c_void_call|void-call.o"
  "void-pointer.c|c_void_pointer_identity|void-pointer.o"
  "branch.c|c_choose|branch.o"
  "predicate.c|c_predicate|predicate.o"
  "bitwise.c|c_bitwise|bitwise.o"
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
  "load.c|c_load|load.o"
  "pointer-arithmetic.c|c_pointer_add|pointer-add.o"
  "pointer-arithmetic.c|c_pointer_add_reversed|pointer-add-reversed.o"
  "pointer-arithmetic.c|c_pointer_subtract|pointer-subtract.o"
  "pointer-arithmetic.c|c_pointer_compound_offset|pointer-compound-offset.o"
  "pointer-arithmetic.c|c_pointer_postfix_increment|pointer-postfix-increment.o"
  "pointer-arithmetic.c|c_pointer_prefix_increment|pointer-prefix-increment.o"
  "pointer-arithmetic.c|c_pointer_prefix_decrement|pointer-prefix-decrement.o"
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
  "functions.c|c_ten_params|ten-params.o"
  "functions.c|c_ten_call|ten-call.o"
  "functions.c|c_ten_doubles|ten-doubles.o"
  "functions.c|c_ten_double_call|ten-double-call.o"
  "functions.c|c_mixed_abi|mixed-abi.o"
  "functions.c|c_mixed_abi_call|mixed-abi-call.o"
  "functions.c|c_recursive|recursive-call.o"
  "functions.c|c_empty_statements|empty-statements.o"
  "double.c|c_double|double.o"
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
  "compound.c|c_pointer_compound_assignment|pointer-compound-assignment.o"
  "compound.c|c_index_compound_assignment|index-compound-assignment.o"
  "compound.c|c_double_pointer_compound_assignment|double-pointer-compound-assignment.o"
  "compound.c|c_shadow_local|shadow-local.o"
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
set(many_functions_input "${BINARY_DIR}/many-functions.c")
set(many_functions_object "${BINARY_DIR}/many-functions.o")
file(WRITE "${many_functions_input}" "${many_functions_source}")
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

set(unsupported_cases
  "unsupported.c|c_unsupported|continue statement is not inside a loop|2:11"
  "unsupported.c|c_break_outside|break statement is not inside a loop|7:8"
  "void.c|c_void_return_value|void function return must not include a value"
  "unsigned.c|c_unsigned|signed 64-bit integers or double"
  "mixed-double.c|c_bad_mix|unsupported cast or implicit conversion"
  "const-update.c|c_const_update|cannot assign to a const C object"
  "const-pointer-store.c|c_const_store|cannot write through a pointer to const C data"
  "const-double-pointer-store.c|c_const_f64_store|cannot write through a pointer to const C data"
  "const-index-store.c|c_const_index_store|cannot write through a pointer to const C data"
  "mixed-pointer-store.c|c_mixed_store|pointer assignment requires a signed 64-bit integer value"
  "for-scope.c|c_for_scope|references must name a function parameter or declared local"
  "compound.c|c_block_scope|references must name a function parameter or declared local"
  "locals.c|c_const_multi_missing_initializer|const local declaration requires an initializer"
  "compound.c|c_redeclare_local|duplicate declaration in the same C scope"
  "volatile.c|c_volatile|volatile C objects are unsupported"
  "increment.c|c_const_postfix_increment|increment and decrement require a mutable scalar or pointer local"
  "increment.c|c_const_prefix_decrement|increment and decrement require a mutable scalar or pointer local"
  "compound.c|c_pointer_compound_unsupported|compound assignment operator is unsupported for this type"
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
  string(FIND "${diagnostic}" "${expected_diagnostic}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR
      "unclear diagnostic for ${source}; expected '${expected_diagnostic}'\n${diagnostic}")
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
set(too_many_args_input "${BINARY_DIR}/too-many-call-args.c")
file(WRITE "${too_many_args_input}"
  "${too_many_args_source}${too_many_args_call}")
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
