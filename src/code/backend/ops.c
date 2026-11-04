#include "code/backend/abi.h"

#include <string.h>

/*
 * Runtime-op identity is backend metadata; it is shared by the Ny
 * frontend and standalone Nyr lowering. ABI declarations remain frontend-owned.
 */
ny_native_runtime_op_t ny_native_runtime_operation(const char *symbol) {
  if (!symbol)
    return NY_NATIVE_RUNTIME_OP_NONE;
  if (strcmp(symbol, "rt_syscall_raw") == 0)
    return NY_NATIVE_RUNTIME_OP_LINUX_SYSCALL;
  if (strcmp(symbol, "rt_tbuf_new_typed_raw") == 0)
    return NY_NATIVE_RUNTIME_OP_TYPED_BUFFER_NEW;
  if (strcmp(symbol, "rt_tbuf_len_raw") == 0)
    return NY_NATIVE_RUNTIME_OP_TYPED_BUFFER_LEN;
  if (strcmp(symbol, "rt_tbuf_index_read_raw") == 0)
    return NY_NATIVE_RUNTIME_OP_TYPED_BUFFER_INDEX_READ;
  if (strcmp(symbol, "rt_tbuf_pop_raw") == 0)
    return NY_NATIVE_RUNTIME_OP_TYPED_BUFFER_POP;
  if (strcmp(symbol, "rt_tbuf_clone_raw") == 0)
    return NY_NATIVE_RUNTIME_OP_TYPED_BUFFER_CLONE;
  if (strcmp(symbol, "rt_tbuf_register_static_handle") == 0)
    return NY_NATIVE_RUNTIME_OP_TBUF_REGISTER_STATIC_HANDLE;
  if (strcmp(symbol, "rt_bounds_trap") == 0)
    return NY_NATIVE_RUNTIME_OP_BOUNDS_TRAP;
  if (strcmp(symbol, "rt_print_cstr") == 0)
    return NY_NATIVE_RUNTIME_OP_PRINT_CSTR;
  if (strcmp(symbol, "rt_print_i64_raw") == 0)
    return NY_NATIVE_RUNTIME_OP_PRINT_I64;
  if (strcmp(symbol, "rt_print_newline") == 0)
    return NY_NATIVE_RUNTIME_OP_PRINT_NEWLINE;
  if (strcmp(symbol, "rt_cstr_to_str") == 0)
    return NY_NATIVE_RUNTIME_OP_CSTR_TO_STRING;
  if (strcmp(symbol, "rt_i64_to_cstr_raw") == 0)
    return NY_NATIVE_RUNTIME_OP_I64_TO_CSTR;
  if (strcmp(symbol, "rt_bool_to_cstr") == 0)
    return NY_NATIVE_RUNTIME_OP_BOOL_TO_CSTR;
  if (strcmp(symbol, "rt_monotonic_ns_raw") == 0)
    return NY_NATIVE_RUNTIME_OP_MONOTONIC_NS;
  if (strcmp(symbol, "rt_memset") == 0 || strcmp(symbol, "rt_memset_raw") == 0)
    return NY_NATIVE_RUNTIME_OP_MEMSET;
  if (strcmp(symbol, "rt_memcpy_raw") == 0)
    return NY_NATIVE_RUNTIME_OP_MEMCPY;
  if (strcmp(symbol, "rt_memcmp_raw") == 0)
    return NY_NATIVE_RUNTIME_OP_MEMCMP;
  if (strcmp(symbol, "rt_cstr_concat") == 0)
    return NY_NATIVE_RUNTIME_OP_CSTR_CONCAT;
  if (strcmp(symbol, "rt_cstr_len") == 0)
    return NY_NATIVE_RUNTIME_OP_CSTR_LEN;
  if (strcmp(symbol, "rt_write_off_raw") == 0)
    return NY_NATIVE_RUNTIME_OP_WRITE_OFF;
  if (strcmp(symbol, "rt_malloc_i64") == 0 ||
      strcmp(symbol, "rt_malloc_raw_i64") == 0 ||
      strcmp(symbol, "rt_malloc_raw") == 0 ||
      strcmp(symbol, "rt_calloc_raw_i64") == 0 ||
      strcmp(symbol, "rt_calloc_raw") == 0 ||
      strcmp(symbol, "rt_zalloc_raw") == 0 ||
      strcmp(symbol, "rt_malloc") == 0 ||
      strcmp(symbol, "rt_malloc_uninit") == 0)
    return NY_NATIVE_RUNTIME_OP_BARE_HEAP_ALLOC;
  if (strcmp(symbol, "rt_free") == 0 || strcmp(symbol, "rt_free_raw") == 0 ||
      strcmp(symbol, "rt_zfree_raw") == 0)
    return NY_NATIVE_RUNTIME_OP_BARE_HEAP_FREE;
  if (strcmp(symbol, "rt_untag") == 0)
    return NY_NATIVE_RUNTIME_OP_BARE_UNTAG;
  if (strcmp(symbol, "rt_cstr_eq") == 0)
    return NY_NATIVE_RUNTIME_OP_CSTR_EQUAL;
  if (strcmp(symbol, "rt_cstr_cmp") == 0)
    return NY_NATIVE_RUNTIME_OP_CSTR_COMPARE;
  if (strcmp(symbol, "rt_str_chr") == 0)
    return NY_NATIVE_RUNTIME_OP_STR_CHR;
  if (strcmp(symbol, "rt_str_repeat") == 0 ||
      strcmp(symbol, "rt_str_repeat_raw") == 0)
    return NY_NATIVE_RUNTIME_OP_STR_REPEAT;
  if (strcmp(symbol, "rt_print_cstr_stderr_raw") == 0)
    return NY_NATIVE_RUNTIME_OP_PRINT_CSTR_STDERR;
  if (strcmp(symbol, "rt_print_i64_stderr_raw") == 0)
    return NY_NATIVE_RUNTIME_OP_PRINT_I64_STDERR;
  if (strcmp(symbol, "rt_print_newline_stderr_raw") == 0)
    return NY_NATIVE_RUNTIME_OP_PRINT_NEWLINE_STDERR;
  if (strcmp(symbol, "rt_f64_to_cstr_raw") == 0)
    return NY_NATIVE_RUNTIME_OP_F64_TO_CSTR;
  if (strcmp(symbol, "rt_print_f64_raw") == 0)
    return NY_NATIVE_RUNTIME_OP_PRINT_F64;
  if (strcmp(symbol, "rt_any_to_cstr") == 0)
    return NY_NATIVE_RUNTIME_OP_ANY_TO_CSTR;
  if (strcmp(symbol, "rt_f64_bits") == 0)
    return NY_NATIVE_RUNTIME_OP_F64_BITS;
  if (strcmp(symbol, "rt_flt_box_val") == 0)
    return NY_NATIVE_RUNTIME_OP_FLT_BOX_VAL;
  if (strcmp(symbol, "rt_tag") == 0)
    return NY_NATIVE_RUNTIME_OP_TAG;
  if (strcmp(symbol, "rt_value_tag") == 0)
    return NY_NATIVE_RUNTIME_OP_VALUE_TAG;
  if (strcmp(symbol, "rt_cstr_replace") == 0)
    return NY_NATIVE_RUNTIME_OP_CSTR_REPLACE;
  if (strcmp(symbol, "rt_cstr_slice") == 0)
    return NY_NATIVE_RUNTIME_OP_CSTR_SLICE;
  if (strcmp(symbol, "rt_cstr_reverse") == 0)
    return NY_NATIVE_RUNTIME_OP_CSTR_REVERSE;
  if (strcmp(symbol, "rt_cstr_builder_new") == 0)
    return NY_NATIVE_RUNTIME_OP_CSTR_BUILDER_NEW;
  if (strcmp(symbol, "rt_cstr_builder_append") == 0)
    return NY_NATIVE_RUNTIME_OP_CSTR_BUILDER_APPEND;
  if (strcmp(symbol, "rt_cstr_builder_append_known") == 0)
    return NY_NATIVE_RUNTIME_OP_CSTR_BUILDER_APPEND_KNOWN;
  if (strcmp(symbol, "rt_cstr_builder_repeat_known") == 0)
    return NY_NATIVE_RUNTIME_OP_CSTR_BUILDER_REPEAT_KNOWN;
  if (strcmp(symbol, "rt_cstr_builder_finalize") == 0)
    return NY_NATIVE_RUNTIME_OP_CSTR_BUILDER_FINALIZE;
  return NY_NATIVE_RUNTIME_OP_NONE;
}
