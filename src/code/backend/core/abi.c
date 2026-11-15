/*
 * Empty runtime ABI registry for standalone Nyr lowering.
 */
#include "code/backend/core/abi.h"

#include <string.h>

/*
 * Standalone Nyr lowering has no Ny runtime ABI registry.
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
