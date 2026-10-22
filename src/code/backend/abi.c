#include "code/backend/abi.h"

/*
 * Standalone Nyr code generation has no Ny runtime helper registry. Frontends
 * must mark their own external calls explicitly in NYIR metadata.
 */
const ny_native_runtime_abi_decl_t ny_native_runtime_abi_decls[1] = {{0}};
const size_t ny_native_runtime_abi_decl_count = 0;

const ny_native_runtime_arg_abi_decl_t ny_native_runtime_arg_abi_decls[1] = {
    {0}};
const size_t ny_native_runtime_arg_abi_decl_count = 0;

const ny_native_runtime_overload_abi_decl_t
    ny_native_runtime_overload_abi_decls[1] = {{0}};
const size_t ny_native_runtime_overload_abi_decl_count = 0;

const ny_native_runtime_value_abi_decl_t ny_native_runtime_value_abi_decls[1] =
    {{0}};
const size_t ny_native_runtime_value_abi_decl_count = 0;

const ny_native_runtime_callback_abi_decl_t
    ny_native_runtime_callback_abi_decls[1] = {{0}};
const size_t ny_native_runtime_callback_abi_decl_count = 0;

const ny_native_runtime_abi_decl_t *
ny_native_runtime_abi_decl(const char *symbol) {
  (void)symbol;
  return NULL;
}

const ny_native_runtime_arg_abi_decl_t *
ny_native_runtime_arg_abi_decl(const char *symbol) {
  (void)symbol;
  return NULL;
}

const ny_native_runtime_overload_abi_decl_t *
ny_native_runtime_overload_abi_decl(const char *symbol) {
  (void)symbol;
  return NULL;
}

const ny_native_runtime_value_abi_decl_t *
ny_native_runtime_value_abi_decl(const char *symbol) {
  (void)symbol;
  return NULL;
}

const ny_native_runtime_callback_abi_decl_t *
ny_native_runtime_callback_abi_decl(const char *symbol) {
  (void)symbol;
  return NULL;
}

const ny_native_runtime_callback_abi_decl_t *
ny_native_runtime_callback_abi_decl_for_arity(size_t source_arity) {
  (void)source_arity;
  return NULL;
}
