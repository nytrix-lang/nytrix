/*
 * Runtime NYIR call contracts must stay explicit, ordered, and conservative.
 */
#include "code/backend/abi.h"
#include "code/backend/runtime/aliases.h"

#include <stdio.h>
#include <string.h>

static int sorted_runtime_declarations(void) {
  for (size_t i = 0; i < ny_native_runtime_abi_decl_count; ++i) {
    const ny_native_runtime_abi_decl_t *decl = &ny_native_runtime_abi_decls[i];
    const char *previous =
        i ? ny_native_runtime_abi_decls[i - 1].symbol : "(first)";
    if (!decl->symbol || (i && strcmp(previous, decl->symbol) >= 0)) {
      fprintf(stderr,
              "[runtime-call-contracts] runtime ABI table is not sorted "
              "at %zu: '%s' before '%s'\n",
              i, previous ? previous : "(null)",
              decl->symbol ? decl->symbol : "(null)");
      return 0;
    }
    const ny_native_runtime_abi_decl_t *lookup =
        ny_native_runtime_abi_decl(decl->symbol);
    if (lookup != decl) {
      size_t lookup_index = ny_native_runtime_abi_decl_count;
      for (size_t j = 0; j < ny_native_runtime_abi_decl_count; ++j) {
        if (&ny_native_runtime_abi_decls[j] == lookup) {
          lookup_index = j;
          break;
        }
      }
      fprintf(stderr,
              "[runtime-call-contracts] runtime lookup mismatch for %s: "
              "expected index %zu, found index %zu (%s)\n",
              decl->symbol, i, lookup_index,
              lookup ? lookup->symbol : "(null)");
      return 0;
    }
  }
  return 1;
}

static int runtime_operations(void) {
  if (ny_native_runtime_operation("rt_syscall_raw") !=
          NY_NATIVE_RUNTIME_OP_LINUX_SYSCALL ||
      ny_native_runtime_operation("rt_syscall") != NY_NATIVE_RUNTIME_OP_NONE ||
      ny_native_runtime_operation("rt_tbuf_new_typed_raw") !=
          NY_NATIVE_RUNTIME_OP_TYPED_BUFFER_NEW ||
      ny_native_runtime_operation("rt_tbuf_len_raw") !=
          NY_NATIVE_RUNTIME_OP_TYPED_BUFFER_LEN ||
      ny_native_runtime_operation("rt_tbuf_index_read_raw") !=
          NY_NATIVE_RUNTIME_OP_TYPED_BUFFER_INDEX_READ ||
      ny_native_runtime_operation("rt_tbuf_pop_raw") !=
          NY_NATIVE_RUNTIME_OP_TYPED_BUFFER_POP ||
      ny_native_runtime_operation("rt_tbuf_clone_raw") !=
          NY_NATIVE_RUNTIME_OP_TYPED_BUFFER_CLONE ||
      ny_native_runtime_operation("rt_bounds_trap") !=
          NY_NATIVE_RUNTIME_OP_BOUNDS_TRAP ||
      ny_native_runtime_operation("rt_print_cstr") !=
          NY_NATIVE_RUNTIME_OP_PRINT_CSTR ||
      ny_native_runtime_operation("rt_print_i64_raw") !=
          NY_NATIVE_RUNTIME_OP_PRINT_I64 ||
      ny_native_runtime_operation("rt_print_newline") !=
          NY_NATIVE_RUNTIME_OP_PRINT_NEWLINE ||
      ny_native_runtime_operation("rt_cstr_to_str") !=
          NY_NATIVE_RUNTIME_OP_CSTR_TO_STRING ||
      ny_native_runtime_operation("rt_i64_to_cstr_raw") !=
          NY_NATIVE_RUNTIME_OP_I64_TO_CSTR ||
      ny_native_runtime_operation("rt_bool_to_cstr") !=
          NY_NATIVE_RUNTIME_OP_BOOL_TO_CSTR ||
      ny_native_runtime_operation("rt_monotonic_ns_raw") !=
          NY_NATIVE_RUNTIME_OP_MONOTONIC_NS ||
      ny_native_runtime_operation("rt_memset") != NY_NATIVE_RUNTIME_OP_MEMSET ||
      ny_native_runtime_operation("rt_memset_raw") !=
          NY_NATIVE_RUNTIME_OP_MEMSET ||
      ny_native_runtime_operation("rt_memcpy_raw") !=
          NY_NATIVE_RUNTIME_OP_MEMCPY ||
      ny_native_runtime_operation("rt_memcmp_raw") !=
          NY_NATIVE_RUNTIME_OP_MEMCMP ||
      ny_native_runtime_operation("rt_cstr_concat") !=
          NY_NATIVE_RUNTIME_OP_CSTR_CONCAT ||
      ny_native_runtime_operation("rt_cstr_len") !=
          NY_NATIVE_RUNTIME_OP_CSTR_LEN ||
      ny_native_runtime_operation("rt_cstr_eq") !=
          NY_NATIVE_RUNTIME_OP_CSTR_EQUAL ||
      ny_native_runtime_operation("rt_cstr_cmp") !=
          NY_NATIVE_RUNTIME_OP_CSTR_COMPARE ||
      ny_native_runtime_operation("rt_f64_to_cstr_raw") !=
          NY_NATIVE_RUNTIME_OP_F64_TO_CSTR ||
      ny_native_runtime_operation("rt_print_f64_raw") !=
          NY_NATIVE_RUNTIME_OP_PRINT_F64 ||
      ny_native_runtime_operation("rt_any_to_cstr") !=
          NY_NATIVE_RUNTIME_OP_ANY_TO_CSTR ||
      ny_native_runtime_operation("rt_f64_bits") !=
          NY_NATIVE_RUNTIME_OP_F64_BITS ||
      ny_native_runtime_operation("rt_flt_box_val") !=
          NY_NATIVE_RUNTIME_OP_FLT_BOX_VAL ||
      ny_native_runtime_operation("rt_tag") != NY_NATIVE_RUNTIME_OP_TAG ||
      ny_native_runtime_operation("rt_value_tag") !=
          NY_NATIVE_RUNTIME_OP_VALUE_TAG ||
      ny_native_runtime_operation("rt_cstr_replace") !=
          NY_NATIVE_RUNTIME_OP_CSTR_REPLACE ||
      ny_native_runtime_operation("rt_cstr_slice") !=
          NY_NATIVE_RUNTIME_OP_CSTR_SLICE ||
      ny_native_runtime_operation("rt_cstr_reverse") !=
          NY_NATIVE_RUNTIME_OP_CSTR_REVERSE ||
      ny_native_runtime_operation("rt_cstr_builder_new") !=
          NY_NATIVE_RUNTIME_OP_CSTR_BUILDER_NEW ||
      ny_native_runtime_operation("rt_cstr_builder_append") !=
          NY_NATIVE_RUNTIME_OP_CSTR_BUILDER_APPEND ||
      ny_native_runtime_operation("rt_cstr_builder_append_known") !=
          NY_NATIVE_RUNTIME_OP_CSTR_BUILDER_APPEND_KNOWN ||
      ny_native_runtime_operation("rt_cstr_builder_repeat_known") !=
          NY_NATIVE_RUNTIME_OP_CSTR_BUILDER_REPEAT_KNOWN ||
      ny_native_runtime_operation("rt_cstr_builder_finalize") !=
          NY_NATIVE_RUNTIME_OP_CSTR_BUILDER_FINALIZE ||
      ny_native_runtime_operation("rt_tbuf_register_static_handle") !=
          NY_NATIVE_RUNTIME_OP_TBUF_REGISTER_STATIC_HANDLE ||
      ny_native_runtime_operation("rt_write_off_raw") !=
          NY_NATIVE_RUNTIME_OP_WRITE_OFF ||
      ny_native_runtime_operation("rt_syscall") != NY_NATIVE_RUNTIME_OP_NONE ||
      ny_native_runtime_operation(NULL) != NY_NATIVE_RUNTIME_OP_NONE) {
    fprintf(stderr, "[runtime-call-contracts] runtime operation classification "
                    "must be exact and null-safe\n");
    return 0;
  }
  return 1;
}

static int sorted_runtime_aliases(void) {
  for (size_t i = 0; i < nyr_runtime_alias_count; ++i) {
    const nyr_runtime_alias_t *alias = &nyr_runtime_aliases[i];
    const char *previous = i ? nyr_runtime_aliases[i - 1].name : "(first)";
    if (!alias->name || !alias->symbol ||
        (i && strcmp(previous, alias->name) >= 0)) {
      fprintf(stderr,
              "[runtime-call-contracts] runtime alias table is not sorted "
              "at %zu: '%s' before '%s'\n",
              i, previous ? previous : "(null)",
              alias->name ? alias->name : "(null)");
      return 0;
    }
    const char *resolved = nyr_runtime_symbol(alias->name);
    if (!resolved || strcmp(resolved, alias->symbol) != 0) {
      fprintf(stderr,
              "[runtime-call-contracts] runtime alias lookup mismatch for "
              "%s: expected %s, got %s\n",
              alias->name, alias->symbol, resolved ? resolved : "(null)");
      return 0;
    }
  }
  return 1;
}

static int sorted_arg_declarations(void) {
  for (size_t i = 0; i < ny_native_runtime_arg_abi_decl_count; ++i) {
    const ny_native_runtime_arg_abi_decl_t *decl =
        &ny_native_runtime_arg_abi_decls[i];
    if (!decl->symbol ||
        (i && strcmp(ny_native_runtime_arg_abi_decls[i - 1].symbol,
                     decl->symbol) >= 0)) {
      const char *previous =
          i ? ny_native_runtime_arg_abi_decls[i - 1].symbol : "(first)";
      fprintf(stderr,
              "[runtime-call-contracts] arg ABI table is not sorted at %zu: "
              "'%s' before '%s'\n",
              i, previous ? previous : "(null)",
              decl->symbol ? decl->symbol : "(null)");
      return 0;
    }
    /*
     * Binary-search lookup must return this exact declaration.
     */
    if (ny_native_runtime_arg_abi_decl(decl->symbol) != decl) {
      fprintf(stderr, "[runtime-call-contracts] lookup mismatch for %s\n",
              decl->symbol);
      return 0;
    }
    /*
     * Unknown means unknown: do not synthesize a borrowed/consuming fact.
     */
    for (size_t arg = 0; arg < 16; ++arg) {
      uint8_t formal = ny_native_runtime_arg_formal_contract(decl, arg);
      if (formal > NYIR_ARG_CONTRACT_RETURNS_BORROW) {
        fprintf(stderr,
                "[runtime-call-contracts] invalid formal contract for %s arg "
                "%zu\n",
                decl->symbol, arg);
        return 0;
      }
    }
  }
  return 1;
}

static int alias_result_declarations(void) {
  size_t aliases = 0;
  for (size_t i = 0; i < ny_native_runtime_value_abi_decl_count; ++i) {
    const ny_native_runtime_value_abi_decl_t *decl =
        &ny_native_runtime_value_abi_decls[i];
    if (!decl->symbol || !decl->arity || decl->arity > 3 ||
        (i && strcmp(ny_native_runtime_value_abi_decls[i - 1].symbol,
                     decl->symbol) >= 0)) {
      fprintf(
          stderr,
          "[runtime-call-contracts] malformed value ABI declaration at %zu\n",
          i);
      return 0;
    }
    if (decl->result_repr != NY_NATIVE_ABI_VALUE_ALIAS_ARG0)
      continue;
    ++aliases;
    if (decl->result_ownership != NY_NATIVE_ABI_BORROWED_ALIAS ||
        decl->arg_ownership[0] != NY_NATIVE_ABI_BORROWED ||
        ny_native_runtime_value_formal_contract(decl, 0) !=
            NYIR_ARG_CONTRACT_RETURNS_BORROW) {
      fprintf(stderr,
              "[runtime-call-contracts] %s has an incomplete arg0 alias "
              "contract\n",
              decl->symbol);
      return 0;
    }
    for (size_t arg = 0; arg < decl->arity; ++arg) {
      if (!decl->arg_repr[NY_NATIVE_ABI_DICT_RECEIVER_NATIVE][arg] ||
          !decl->arg_repr[NY_NATIVE_ABI_DICT_RECEIVER_MANAGED][arg] ||
          decl->arg_ownership[arg] != NY_NATIVE_ABI_BORROWED) {
        fprintf(stderr,
                "[runtime-call-contracts] %s arg %zu lacks receiver/ownership "
                "coverage\n",
                decl->symbol, arg);
        return 0;
      }
    }
  }
  if (aliases != 2) {
    fprintf(stderr,
            "[runtime-call-contracts] expected 2 audited alias results, found "
            "%zu\n",
            aliases);
    return 0;
  }
  return 1;
}

int main(void) {
  if (!sorted_runtime_aliases() || !sorted_runtime_declarations() ||
      !sorted_arg_declarations() || !alias_result_declarations() ||
      !runtime_operations())
    return 1;
  size_t unspecified = 0;
  for (size_t i = 0; i < ny_native_runtime_arg_abi_decl_count; ++i)
    unspecified += ny_native_runtime_arg_abi_decls[i].formal_contracts == 0;
  printf("[runtime-call-contracts] %zu argument declarations indexed; "
         "%zu retain unknown ownership; alias results borrow arg0\n",
         ny_native_runtime_arg_abi_decl_count, unspecified);
  for (size_t i = 0; i < ny_native_runtime_arg_abi_decl_count; ++i)
    if (ny_native_runtime_arg_abi_decls[i].formal_contracts == 0)
      printf("  ownership unknown: %s\n",
             ny_native_runtime_arg_abi_decls[i].symbol);
  return 0;
}
