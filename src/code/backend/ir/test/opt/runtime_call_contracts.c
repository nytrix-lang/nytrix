/*
 * Runtime NYIR call contracts must stay explicit, ordered, and conservative.
 */
#include "code/backend/abi.h"
#include "code/backend/internal.h"
#include "code/lang/runtime/aliases.h"

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
    if (ny_native_runtime_abi_decl(decl->symbol) != decl) {
      fprintf(stderr,
              "[runtime-call-contracts] runtime lookup mismatch for "
              "%s\n",
              decl->symbol);
      return 0;
    }
  }
  return 1;
}

static int sorted_runtime_aliases(void) {
  for (size_t i = 0; i < nyr_runtime_alias_count; ++i) {
    const nyr_runtime_alias_t *alias = &nyr_runtime_aliases[i];
    const char *previous =
        i ? nyr_runtime_aliases[i - 1].name : "(first)";
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
        fprintf(
            stderr,
            "[runtime-call-contracts] invalid formal contract for %s arg %zu\n",
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
      fprintf(
          stderr,
          "[runtime-call-contracts] %s has an incomplete arg0 alias contract\n",
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
      !sorted_arg_declarations() || !alias_result_declarations())
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
