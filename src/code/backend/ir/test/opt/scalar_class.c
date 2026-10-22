/*
 * Regression checks for verifier enforcement of NYIR scalar ABI boundaries.
 */
#include "code/backend/abi.h"
#include "code/backend/ir/opt/util.h"
#include "code/lang/parse/ast.h"
#include "support.h"

static bool runtime_value_formal_contracts_are_precise(void) {
  ny_native_runtime_value_abi_decl_t decl = {
      .symbol = "test_alias",
      .arity = 3,
      .arg_ownership = {NY_NATIVE_ABI_BORROWED, NY_NATIVE_ABI_BORROWED,
                        NY_NATIVE_ABI_OWNERSHIP_UNSPECIFIED},
      .result_repr = NY_NATIVE_ABI_VALUE_ALIAS_ARG0,
      .result_ownership = NY_NATIVE_ABI_BORROWED_ALIAS};
  if (ny_native_runtime_value_formal_contract(&decl, 0) !=
          NYIR_ARG_CONTRACT_RETURNS_BORROW ||
      ny_native_runtime_value_formal_contract(&decl, 1) !=
          NYIR_ARG_CONTRACT_BORROWS ||
      ny_native_runtime_value_formal_contract(&decl, 2) !=
          NYIR_ARG_CONTRACT_UNKNOWN ||
      ny_native_runtime_value_formal_contract(&decl, 3) !=
          NYIR_ARG_CONTRACT_UNKNOWN) {
    fprintf(stderr,
            "[scalar-class] runtime formal contract mapping is unsound\n");
    return false;
  }
  decl.result_repr = NY_NATIVE_ABI_VALUE_RAW_I64;
  return ny_native_runtime_value_formal_contract(&decl, 0) ==
         NYIR_ARG_CONTRACT_BORROWS;
}

static bool runtime_intrinsic_formal_contracts_are_declared(void) {
  const ny_native_runtime_arg_abi_decl_t *dict_clear =
      ny_native_runtime_arg_abi_decl("rt_dict_clear_raw");
  const ny_native_runtime_arg_abi_decl_t *dict_delete =
      ny_native_runtime_arg_abi_decl("rt_dict_delete_raw");
  const ny_native_runtime_arg_abi_decl_t *dict_get =
      ny_native_runtime_arg_abi_decl("rt_dict_get_raw");
  const ny_native_runtime_arg_abi_decl_t *tbuf_append =
      ny_native_runtime_arg_abi_decl("rt_tbuf_append_raw");
  const ny_native_runtime_arg_abi_decl_t *tbuf_set =
      ny_native_runtime_arg_abi_decl("rt_tbuf_set_i64_raw");
  const ny_native_runtime_arg_abi_decl_t *tbuf_len =
      ny_native_runtime_arg_abi_decl("rt_tbuf_len_raw");
  const ny_native_runtime_arg_abi_decl_t *unknown =
      ny_native_runtime_arg_abi_decl("rt_syscall_raw");
  if (!(dict_clear &&
        ny_native_runtime_arg_formal_contract(dict_clear, 0) ==
            NYIR_ARG_CONTRACT_RETURNS_BORROW &&
        dict_delete &&
        ny_native_runtime_arg_formal_contract(dict_delete, 0) ==
            NYIR_ARG_CONTRACT_RETURNS_BORROW &&
        ny_native_runtime_arg_formal_contract(dict_delete, 1) ==
            NYIR_ARG_CONTRACT_BORROWS &&
        dict_get &&
        ny_native_runtime_arg_formal_contract(dict_get, 0) ==
            NYIR_ARG_CONTRACT_RETURNS_BORROW &&
        ny_native_runtime_arg_formal_contract(dict_get, 1) ==
            NYIR_ARG_CONTRACT_BORROWS &&
        ny_native_runtime_arg_formal_contract(dict_get, 2) ==
            NYIR_ARG_CONTRACT_RETURNS_BORROW &&
        tbuf_append &&
        ny_native_runtime_arg_formal_contract(tbuf_append, 0) ==
            NYIR_ARG_CONTRACT_RETURNS_BORROW &&
        ny_native_runtime_arg_formal_contract(tbuf_append, 1) ==
            NYIR_ARG_CONTRACT_BORROWS &&
        tbuf_set &&
        ny_native_runtime_arg_formal_contract(tbuf_set, 0) ==
            NYIR_ARG_CONTRACT_RETURNS_BORROW &&
        tbuf_len &&
        ny_native_runtime_arg_formal_contract(tbuf_len, 0) ==
            NYIR_ARG_CONTRACT_BORROWS &&
        ny_native_runtime_arg_formal_contract(tbuf_len, 1) ==
            NYIR_ARG_CONTRACT_UNKNOWN &&
        unknown &&
        ny_native_runtime_arg_formal_contract(unknown, 0) ==
            NYIR_ARG_CONTRACT_UNKNOWN)) {
    fprintf(stderr,
            "[ownership] runtime intrinsic spot checks failed: dict_clear=%d "
            "dict_delete=%d dict_get=%d tbuf_append=%d tbuf_set=%d "
            "tbuf_len=%d unknown=%d\n",
            dict_clear != NULL, dict_delete != NULL, dict_get != NULL,
            tbuf_append != NULL, tbuf_set != NULL, tbuf_len != NULL,
            unknown != NULL);
    return false;
  }

  struct expected_contracts {
    const char *symbol;
    uint8_t args[6];
  } expected[] = {
      {"rt_any_add",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_any_div",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_any_eq",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_any_len",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_any_tag",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_any_to_f64",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_any_to_i64",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_result_ok",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_result_err",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_result_unwrap_or_any",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_RETURNS_BORROW,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_result_unwrap_or_raw",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_result_unwrap_raw",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_unwrap",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_bigint_add",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_bigfloat_abs",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_bigfloat_add",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_bigfloat_cmp_raw",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_bigfloat_div",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_bigfloat_from_value_raw",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_bigfloat_is",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_bigfloat_mul",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_bigfloat_neg",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_bigfloat_pow_int_raw",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_bigfloat_precision_raw",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_bigfloat_sqrt",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_bigfloat_sub",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_bigfloat_to_f64_raw",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_bigfloat_to_str",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_bigint_and",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_bigint_bitlen_raw",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_bigint_clz",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_bigint_cmp_raw",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_bigint_ctz",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_bigint_div",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_bigint_iroot",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_bigint_jacobi",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_bigint_kronecker",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_bigint_legendre",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_bigint_mod",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_bigint_modinv",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_bigint_mul",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_bigint_neg_raw",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_bigint_or",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_bigint_pow",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_bigint_sub",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_bigint_submul",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_BORROWS}},
      {"rt_bigint_to_cstr_raw",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_bigint_to_i64_raw",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_bigint_to_int",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_bigint_to_str",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_bigint_xor",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_canvas_free",
       {NYIR_ARG_CONTRACT_CONSUMES, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_close",
       {NYIR_ARG_CONTRACT_CONSUMES, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_close_raw",
       {NYIR_ARG_CONTRACT_CONSUMES, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_closesocket",
       {NYIR_ARG_CONTRACT_CONSUMES, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_dir_close",
       {NYIR_ARG_CONTRACT_CONSUMES, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_dir_open",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_dir_read",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_dlclose",
       {NYIR_ARG_CONTRACT_CONSUMES, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_dlopen",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_dlsym",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_dict_delete_str_raw",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_dict_merge_raw",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_dict_set_i64_raw",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_dict_set_raw",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_dict_set_str_raw",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_cstr_builder_append",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_cstr_builder_finalize",
       {NYIR_ARG_CONTRACT_CONSUMES, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_cstr_builder_new",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_cstr_concat",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_cstr_eq",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_cstr_get_raw",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_cstr_index_read_raw",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_cstr_len",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_cstr_replace",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_BORROWS}},
      {"rt_cstr_to_str",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_copy_mem_raw",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_drop_owned",
       {NYIR_ARG_CONTRACT_CONSUMES, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_drop_owned_slot",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_file_exists_raw",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_free",
       {NYIR_ARG_CONTRACT_CONSUMES, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_free_raw",
       {NYIR_ARG_CONTRACT_CONSUMES, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_init_str",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_kqueue_close",
       {NYIR_ARG_CONTRACT_CONSUMES, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_mutex_free",
       {NYIR_ARG_CONTRACT_CONSUMES, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_mutex_lock64",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_mutex_unlock64",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_memcmp_raw",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_memcpy_raw",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_memset_raw",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_thread_join",
       {NYIR_ARG_CONTRACT_CONSUMES, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_native_dict_set_nir_i64",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_BORROWS}},
      {"rt_native_dict_set_raw_i64",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_native_dict_set_str_compact",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_BORROWS}},
      {"rt_open_raw",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_proof_cert_check",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_proof_cert_digest",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_read_fd_raw",
       {NYIR_ARG_CONTRACT_UNKNOWN, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_read_off_raw",
       {NYIR_ARG_CONTRACT_UNKNOWN, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_rc_count",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_release_owned",
       {NYIR_ARG_CONTRACT_CONSUMES, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_retain_owned",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_str_builder_append",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_str_builder_free",
       {NYIR_ARG_CONTRACT_CONSUMES, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_str_builder_to_str",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_websocket_close",
       {NYIR_ARG_CONTRACT_CONSUMES, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_win32_find_close_change",
       {NYIR_ARG_CONTRACT_CONSUMES, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_zfree_raw",
       {NYIR_ARG_CONTRACT_CONSUMES, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_write_buffered_raw",
       {NYIR_ARG_CONTRACT_UNKNOWN, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_write_fd_raw",
       {NYIR_ARG_CONTRACT_UNKNOWN, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_write_off",
       {NYIR_ARG_CONTRACT_UNKNOWN, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_write_off_raw",
       {NYIR_ARG_CONTRACT_UNKNOWN, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_tbuf_extend",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_tbuf_append_raw",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_tbuf_append_tagged",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_tbuf_reserve",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_tbuf_set_f64_bits",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_tbuf_set_tagged",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_tbuf_swap",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_any_to_cstr",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_bigint_to_cstr_raw",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_bytes_get_raw",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_RETURNS_BORROW}},
      {"rt_bytes_index_read_raw",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_bytes_set_raw",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_canvas_clear",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_canvas_print",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN, NYIR_ARG_CONTRACT_BORROWS}},
      {"rt_canvas_refresh",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_canvas_set",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN, NYIR_ARG_CONTRACT_BORROWS}},
      {"rt_dict_get_i64_raw",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_RETURNS_BORROW}},
      {"rt_dict_get_raw",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_RETURNS_BORROW}},
      {"rt_dict_get_str_raw",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_RETURNS_BORROW}},
      {"rt_dict_has_str_raw",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_init_str_raw",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_len_strict",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_list_set_len_raw",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_print_cstr",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_print_str_raw",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_range_index_read_raw",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_range_values_raw",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_str_byte",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_tbuf_index_any_raw",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_tbuf_get_any",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_RETURNS_BORROW}},
      {"rt_tbuf_index_any",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_tbuf_index_f64",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_tbuf_to_cstr",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_to_str",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_copy_mem",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_load8_idx",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_load8_raw",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_load16_idx",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_load32_idx",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_load32_f64",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_load32_h",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_load64_idx",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_load64_h",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_memcpy",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_memcmp",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_memset",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_ptr_add",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_ptr_add_i64",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_ptr_key",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_ptr_sub",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_ptr_sub_i64",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_load_item",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_load_item_any",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_clear_raw",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_clone_raw",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_complex_add",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_complex_new",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_contains_raw",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_sequence_len_raw",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_set_from_sequence",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_set_add_raw",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_set_remove",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_set_remove_raw",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_value_get_index_raw",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_RETURNS_BORROW}},
      {"rt_value_is_ptr",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_value_tag",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_value_to_f64",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_vec_add_raw",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_vec_div_scalar_raw",
       {NYIR_ARG_CONTRACT_RETURNS_BORROW, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_vec_dot_raw",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_BORROWS,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_vec_mark_raw",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_store8_idx",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_RETURNS_BORROW}},
      {"rt_store8_raw",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_RETURNS_BORROW}},
      {"rt_store16_idx",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_RETURNS_BORROW}},
      {"rt_store32_idx",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_RETURNS_BORROW}},
      {"rt_store32_f64",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_UNKNOWN}},
      {"rt_store64_h",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_RETURNS_BORROW}},
      {"rt_store64_idx",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_RETURNS_BORROW}},
      {"rt_store_item",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_RETURNS_BORROW}},
      {"rt_store_item_fast",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_RETURNS_BORROW}},
      {"rt_store_item_fast_raw",
       {NYIR_ARG_CONTRACT_BORROWS, NYIR_ARG_CONTRACT_UNKNOWN,
        NYIR_ARG_CONTRACT_RETURNS_BORROW}},
  };
  for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
    const ny_native_runtime_arg_abi_decl_t *decl =
        ny_native_runtime_arg_abi_decl(expected[i].symbol);
    if (!decl) {
      fprintf(stderr, "[ownership] missing ABI declaration for %s\n",
              expected[i].symbol);
      return false;
    }
    for (size_t arg = 0; arg < 3; ++arg) {
      uint8_t actual = ny_native_runtime_arg_formal_contract(decl, arg);
      if (actual != expected[i].args[arg]) {
        fprintf(stderr, "[ownership] %s arg %zu contract=%u expected=%u\n",
                expected[i].symbol, arg, actual, expected[i].args[arg]);
        return false;
      }
    }
  }

  const ny_native_runtime_arg_abi_decl_t *canvas_print =
      ny_native_runtime_arg_abi_decl("rt_canvas_print");
  if (!canvas_print || ny_native_runtime_arg_formal_contract(canvas_print, 3) !=
                           NYIR_ARG_CONTRACT_BORROWS) {
    fprintf(stderr, "[ownership] rt_canvas_print arg 3 is not borrowed\n");
    return false;
  }

  for (size_t i = 0; i < ny_native_runtime_arg_abi_decl_count; ++i) {
    const ny_native_runtime_arg_abi_decl_t *decl =
        &ny_native_runtime_arg_abi_decls[i];
    if (!decl->symbol ||
        (i && strcmp(ny_native_runtime_arg_abi_decls[i - 1].symbol,
                     decl->symbol) >= 0) ||
        ny_native_runtime_arg_abi_decl(decl->symbol) != decl) {
      fprintf(stderr,
              "[ownership] runtime ABI table is not sorted/searchable at %s\n",
              decl->symbol ? decl->symbol : "<null>");
      return false;
    }
  }
  /*
   * Two flat dictionary bridges have both a representation-aware descriptor
   * and a general runtime-argument descriptor. They must state the same
   * ownership for each formal; call-lowering precedence must not change facts.
   */
  for (size_t i = 0; i < ny_native_runtime_value_abi_decl_count; ++i) {
    const ny_native_runtime_value_abi_decl_t *value_decl =
        &ny_native_runtime_value_abi_decls[i];
    const ny_native_runtime_arg_abi_decl_t *arg_decl =
        ny_native_runtime_arg_abi_decl(value_decl->symbol);
    if (!arg_decl)
      continue;
    for (size_t arg = 0; arg < value_decl->arity; ++arg) {
      if (ny_native_runtime_arg_formal_contract(arg_decl, arg) !=
          ny_native_runtime_value_formal_contract(value_decl, arg)) {
        fprintf(
            stderr,
            "[ownership] overlapping ABI declarations disagree: %s arg %zu\n",
            value_decl->symbol, arg);
        return false;
      }
    }
  }
  return true;
}

static bool write_le(FILE *out, uint64_t value, size_t bytes) {
  for (size_t i = 0; i < bytes; ++i) {
    unsigned char byte = (unsigned char)(value >> (i * 8));
    if (fwrite(&byte, 1, 1, out) != 1)
      return false;
  }
  return true;
}

static bool write_legacy_v15_inst(FILE *out, nyr_inst_t inst,
                                  nyr_scalar_class_t scalar_class) {
  return write_le(out, inst.op, 2) && write_le(out, inst.cmp, 2) &&
         write_le(out, (uint32_t)inst.dst, 4) &&
         write_le(out, (uint32_t)inst.a, 4) &&
         write_le(out, (uint32_t)inst.b, 4) &&
         write_le(out, (uint32_t)inst.c, 4) &&
         write_le(out, (uint32_t)inst.d, 4) &&
         write_le(out, (uint32_t)inst.e, 4) &&
         write_le(out, (uint32_t)inst.f, 4) &&
         write_le(out, (uint64_t)inst.imm, 8) && write_le(out, inst.flags, 4) &&
         write_le(out, nyr_inst_effects(&inst), 4) && write_le(out, 0, 4) &&
         write_le(out, 0, 4) && write_le(out, 0, 1) && write_le(out, 0, 1) &&
         write_le(out, 0, 8) && write_le(out, 0, 8) && write_le(out, 0, 4) &&
         write_le(out, 0, 4) && write_le(out, 0, 4) && write_le(out, 0, 4) &&
         write_le(out, 0, 4) && write_le(out, 0, 1) && write_le(out, 0, 1) &&
         write_le(out, scalar_class, 1) && write_le(out, 0, 4);
}

static bool load_legacy_return_class(nyr_scalar_class_t scalar_class,
                                     uint32_t legacy_flag) {
  FILE *artifact = tmpfile();
  if (!artifact)
    return false;
  bool wrote = fwrite("NYIR", 1, 4, artifact) == 4 &&
               write_le(artifact, 15, 2) && write_le(artifact, 0, 2) &&
               write_le(artifact, 7, 4) &&
               fwrite("rt_main", 1, 7, artifact) == 7 &&
               write_le(artifact, 1, 4) && write_le(artifact, 2, 4) &&
               write_le(artifact, 0, 4) && write_le(artifact, 0, 4);
  nyr_op_t const_op =
      scalar_class == NYIR_SCALAR_F64 ? NYIR_CONST_F64 : NYIR_CONST_F32;
  nyr_inst_t constant = {.op = const_op,
                          .dst = 0,
                          .a = -1,
                          .b = -1,
                          .c = -1,
                          .d = -1,
                          .e = -1,
                          .f = -1,
                          .imm = 0x3ff0000000000000LL};
  nyr_inst_t ret = {.op = NYIR_RET,
                     .dst = -1,
                     .a = 0,
                     .b = -1,
                     .c = -1,
                     .d = -1,
                     .e = -1,
                     .f = -1,
                     .flags = legacy_flag};
  wrote = wrote && write_legacy_v15_inst(artifact, constant, scalar_class) &&
          write_legacy_v15_inst(artifact, ret, NYIR_SCALAR_UNKNOWN) &&
          fflush(artifact) == 0 && fseek(artifact, 0, SEEK_SET) == 0;
  nyr_func_t loaded = {0};
  char name[32] = {0};
  char err[256] = {0};
  bool ok = wrote &&
            nyr_load_binary(artifact, &loaded, name, sizeof(name), err,
                             sizeof(err)) &&
            strcmp(name, "rt_main") == 0 && loaded.len == 2 &&
            loaded.data[1].scalar_class == scalar_class &&
            loaded.data[1].flags == 0 && nyr_ir_verify(&loaded, err, sizeof(err));
  if (!ok)
    fprintf(stderr, "[scalar-class] legacy v15 return import failed: %s\n",
            err);
  nyr_ir_func_free(&loaded);
  fclose(artifact);
  return ok;
}

static bool verify_call_class(nyr_scalar_class_t producer,
                              nyr_scalar_class_t formal, bool should_verify,
                              const char *expected_error) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  nyr_op_t op = producer == NYIR_SCALAR_F32   ? NYIR_CONST_F32
                 : producer == NYIR_SCALAR_F64 ? NYIR_CONST_F64
                                               : NYIR_CONST_I64;
  int value = nyr_ir_emit(&f, (nyr_inst_t){.op = op,
                                          .dst = -1,
                                          .a = -1,
                                          .b = -1,
                                          .c = -1,
                                          .d = -1,
                                          .e = -1,
                                          .f = -1,
                                          .scalar_class = producer});
  nyr_inst_t call = {.op = NYIR_CALL,
                      .dst = -1,
                      .a = value,
                      .b = -1,
                      .c = -1,
                      .d = -1,
                      .e = -1,
                      .f = -1,
                      .imm = 1,
                      .symbol = "abi_probe"};
  nyr_call_arg_class_set(call.arg_scalar_classes, 0, formal);
  nyr_ir_emit(&f, call);

  char err[256] = {0};
  bool verified = nyr_ir_verify(&f, err, sizeof(err));
  bool ok = verified == should_verify;
  if (ok && expected_error)
    ok = !verified && strstr(err, expected_error) != NULL;
  if (!ok)
    fprintf(stderr, "[scalar-class] producer=%d formal=%d verified=%d err=%s\n",
            producer, formal, verified, err);
  nyr_ir_func_free(&f);
  return ok;
}

static bool verify_copied_call_class(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int value = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_CONST_I64,
                                          .dst = -1,
                                          .a = -1,
                                          .b = -1,
                                          .c = -1,
                                          .d = -1,
                                          .e = -1,
                                          .f = -1,
                                          .scalar_class = NYIR_SCALAR_STRING});
  int copy = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_COPY,
                                         .dst = -1,
                                         .a = value,
                                         .b = -1,
                                         .c = -1,
                                         .d = -1,
                                         .e = -1,
                                         .f = -1});
  nyr_inst_t call = {.op = NYIR_CALL,
                      .dst = -1,
                      .a = copy,
                      .b = -1,
                      .c = -1,
                      .d = -1,
                      .e = -1,
                      .f = -1,
                      .imm = 1,
                      .symbol = "abi_probe"};
  nyr_call_arg_class_set(call.arg_scalar_classes, 0, NYIR_SCALAR_AGGREGATE);
  nyr_ir_emit(&f, call);
  char err[256] = {0};
  bool ok =
      !nyr_ir_verify(&f, err, sizeof(err)) && strstr(err, "string != aggregate");
  if (!ok)
    fprintf(stderr,
            "[scalar-class] copied producer class was not enforced: %s\n", err);
  nyr_ir_func_free(&f);
  return ok;
}

static bool explicit_tagged_class_overrides_stale_float_type(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int value = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_CONST_F64,
                                          .dst = -1,
                                          .a = -1,
                                          .b = -1,
                                          .c = -1,
                                          .d = -1,
                                          .e = -1,
                                          .f = -1,
                                          .scalar_class = NYIR_SCALAR_F64});
  int copy =
      nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_COPY,
                                  .dst = -1,
                                  .a = value,
                                  .b = -1,
                                  .c = -1,
                                  .d = -1,
                                  .e = -1,
                                  .f = -1,
                                  .scalar_class = NYIR_SCALAR_I64_TAGGED});
  nyr_inst_t call = {.op = NYIR_CALL,
                      .dst = -1,
                      .a = copy,
                      .b = -1,
                      .c = -1,
                      .d = -1,
                      .e = -1,
                      .f = -1,
                      .imm = 1,
                      .symbol = "abi_probe"};
  nyr_call_arg_class_set(call.arg_scalar_classes, 0, NYIR_SCALAR_F64);
  nyr_ir_emit(&f, call);
  char err[256] = {0};
  bool rejected = !nyr_ir_verify(&f, err, sizeof(err)) &&
                  strstr(err, "i64-tagged != f64") != NULL;
  if (!rejected)
    fprintf(stderr,
            "[scalar-class] tagged copy inherited its source float class: %s\n",
            err);
  nyr_ir_func_free(&f);
  return rejected;
}

static bool verify_call_ownership(uint8_t producer_ownership,
                                  uint8_t argument_ownership,
                                  bool should_verify,
                                  const char *expected_error) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int value =
      nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_CONST_I64,
                                  .dst = -1,
                                  .a = -1,
                                  .b = -1,
                                  .c = -1,
                                  .d = -1,
                                  .e = -1,
                                  .f = -1,
                                  .semantic_ownership = producer_ownership});
  nyr_inst_t call = {.op = NYIR_CALL,
                      .dst = -1,
                      .a = value,
                      .b = -1,
                      .c = -1,
                      .d = -1,
                      .e = -1,
                      .f = -1,
                      .imm = 1,
                      .symbol = "ownership_probe"};
  nyr_call_arg_ownership_set(&call, 0, argument_ownership);
  nyr_call_arg_contract_set(
      &call, 0, NYIR_ARG_CONTRACT_BORROWS | NYIR_ARG_CONTRACT_RETURNS_BORROW);
  nyr_ir_emit(&f, call);
  char err[256] = {0};
  bool verified = nyr_ir_verify(&f, err, sizeof(err));
  bool ok = verified == should_verify;
  if (ok && expected_error)
    ok = !verified && strstr(err, expected_error) != NULL;
  if (!ok)
    fprintf(stderr, "[ownership] producer=%u argument=%u verified=%d err=%s\n",
            producer_ownership, argument_ownership, verified, err);
  nyr_ir_func_free(&f);
  return ok;
}

static bool roundtrips_call_ownership(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int value =
      nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_CONST_I64,
                                  .dst = -1,
                                  .a = -1,
                                  .b = -1,
                                  .c = -1,
                                  .d = -1,
                                  .e = -1,
                                  .f = -1,
                                  .semantic_ownership = NY_SEM_OWN_SHARED});
  nyr_inst_t call = {.op = NYIR_CALL,
                      .dst = -1,
                      .a = value,
                      .b = -1,
                      .c = -1,
                      .d = -1,
                      .e = -1,
                      .f = -1,
                      .imm = 1,
                      .symbol = "ownership_probe"};
  nyr_call_arg_ownership_set(&call, 0, NY_SEM_OWN_SHARED);
  nyr_call_arg_contract_set(
      &call, 0, NYIR_ARG_CONTRACT_BORROWS | NYIR_ARG_CONTRACT_RETURNS_BORROW);
  nyr_ir_emit(&f, call);
  uint64_t before = nyr_debug_fingerprint(&f);
  nyr_call_arg_contract_set(&f.data[1], 0, NYIR_ARG_CONTRACT_CONSUMES);
  uint64_t contract_after = nyr_debug_fingerprint(&f);
  nyr_call_arg_contract_set(&f.data[1], 0,
                             NYIR_ARG_CONTRACT_BORROWS |
                                 NYIR_ARG_CONTRACT_RETURNS_BORROW);
  nyr_call_arg_ownership_set(&f.data[1], 0, NY_SEM_OWN_BORROWED);
  uint64_t ownership_after = nyr_debug_fingerprint(&f);
  nyr_call_arg_ownership_set(&f.data[1], 0, NY_SEM_OWN_SHARED);
  FILE *artifact = tmpfile();
  bool wrote = artifact && nyr_dump_binary(artifact, &f, "ownership_probe") &&
               fflush(artifact) == 0 && fseek(artifact, 0, SEEK_SET) == 0;
  nyr_func_t loaded = {0};
  char name[64] = {0};
  char err[256] = {0};
  bool roundtrip =
      wrote &&
      nyr_load_binary(artifact, &loaded, name, sizeof(name), err,
                       sizeof(err)) &&
      loaded.len == 2 &&
      nyr_call_arg_ownership_get(&loaded.data[1], 0) == NY_SEM_OWN_SHARED &&
      nyr_call_arg_contract_get(&loaded.data[1], 0) ==
          (NYIR_ARG_CONTRACT_BORROWS | NYIR_ARG_CONTRACT_RETURNS_BORROW) &&
      nyr_ir_verify(&loaded, err, sizeof(err));
  if (!roundtrip || before == contract_after || before == ownership_after)
    fprintf(
        stderr, "[ownership] call metadata roundtrip=%d fingerprint=%d: %s\n",
        roundtrip, before != contract_after && before != ownership_after, err);
  nyr_ir_func_free(&loaded);
  if (artifact)
    fclose(artifact);
  nyr_ir_func_free(&f);
  return roundtrip && before != contract_after && before != ownership_after;
}

static bool rejects_ownership_metadata_on_non_call(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int value = nyr_opt_const(&f, 7);
  f.data[0].arg_ownership[0] = UINT64_C(1);
  nyr_opt_ret(&f, value);
  char err[256] = {0};
  bool rejected =
      !nyr_ir_verify(&f, err, sizeof(err)) &&
      strstr(err, "call argument ownership on non-call instruction");
  if (!rejected)
    fprintf(stderr,
            "[ownership] non-call argument ownership was accepted: %s\n", err);
  nyr_ir_func_free(&f);
  return rejected;
}

static bool rejects_formal_contract_metadata_on_non_call(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int value = nyr_opt_const(&f, 7);
  f.data[0].arg_contracts[0] = UINT64_C(1);
  nyr_opt_ret(&f, value);
  char err[256] = {0};
  bool rejected =
      !nyr_ir_verify(&f, err, sizeof(err)) &&
      strstr(err, "call argument contracts on non-call instruction");
  if (!rejected)
    fprintf(stderr, "[ownership] non-call formal contract was accepted: %s\n",
            err);
  nyr_ir_func_free(&f);
  return rejected;
}

static bool verify_store_class(nyr_scalar_class_t producer,
                               nyr_scalar_class_t stored, bool should_verify,
                               const char *expected_error) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int addr = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_CONST_I64,
                                         .dst = -1,
                                         .a = -1,
                                         .b = -1,
                                         .c = -1,
                                         .d = -1,
                                         .e = -1,
                                         .f = -1});
  nyr_op_t op = producer == NYIR_SCALAR_F32 ? NYIR_CONST_F32 : NYIR_CONST_F64;
  int value = nyr_ir_emit(&f, (nyr_inst_t){.op = op,
                                          .dst = -1,
                                          .a = -1,
                                          .b = -1,
                                          .c = -1,
                                          .d = -1,
                                          .e = -1,
                                          .f = -1,
                                          .scalar_class = producer});
  nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_STORE_I64,
                              .dst = -1,
                              .a = addr,
                              .b = -1,
                              .c = value,
                              .d = -1,
                              .e = -1,
                              .f = -1,
                              .scalar_class = stored});

  char err[256] = {0};
  bool verified = nyr_ir_verify(&f, err, sizeof(err));
  bool ok = verified == should_verify;
  if (ok && expected_error)
    ok = !verified && strstr(err, expected_error) != NULL;
  if (!ok) {
    fprintf(stderr,
            "[scalar-class] store producer=%d class=%d verified=%d err=%s\n",
            producer, stored, verified, err);
  }
  nyr_ir_func_free(&f);
  return ok;
}

static bool verify_local_store_class(nyr_scalar_class_t producer,
                                     nyr_scalar_class_t stored,
                                     bool should_verify,
                                     const char *expected_error) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  nyr_op_t op = producer == NYIR_SCALAR_F32 ? NYIR_CONST_F32 : NYIR_CONST_F64;
  int value = nyr_ir_emit(&f, (nyr_inst_t){.op = op,
                                          .dst = -1,
                                          .a = -1,
                                          .b = -1,
                                          .c = -1,
                                          .d = -1,
                                          .e = -1,
                                          .f = -1,
                                          .scalar_class = producer});
  nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_STORE_LOCAL,
                              .dst = -1,
                              .a = value,
                              .b = -1,
                              .c = -1,
                              .d = -1,
                              .e = -1,
                              .f = -1,
                              .imm = 0,
                              .scalar_class = stored});
  char err[256] = {0};
  bool verified = nyr_ir_verify(&f, err, sizeof(err));
  bool ok = verified == should_verify;
  if (ok && expected_error)
    ok = !verified && strstr(err, expected_error) != NULL;
  if (!ok)
    fprintf(
        stderr,
        "[scalar-class] local store producer=%d class=%d verified=%d err=%s\n",
        producer, stored, verified, err);
  nyr_ir_func_free(&f);
  return ok;
}

static bool verify_return_class(nyr_scalar_class_t producer,
                                nyr_scalar_class_t declared,
                                bool should_verify,
                                const char *expected_error) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  nyr_op_t op = producer == NYIR_SCALAR_F32   ? NYIR_CONST_F32
                 : producer == NYIR_SCALAR_F64 ? NYIR_CONST_F64
                                               : NYIR_CONST_I64;
  int value = nyr_ir_emit(&f, (nyr_inst_t){.op = op,
                                          .dst = -1,
                                          .a = -1,
                                          .b = -1,
                                          .c = -1,
                                          .d = -1,
                                          .e = -1,
                                          .f = -1,
                                          .scalar_class = producer});
  nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_RET,
                              .dst = -1,
                              .a = value,
                              .b = -1,
                              .c = -1,
                              .d = -1,
                              .e = -1,
                              .f = -1,
                              .scalar_class = declared});
  char err[256] = {0};
  bool verified = nyr_ir_verify(&f, err, sizeof(err));
  bool ok = verified == should_verify;
  if (ok && expected_error)
    ok = !verified && strstr(err, expected_error) != NULL;
  if (!ok)
    fprintf(
        stderr,
        "[scalar-class] return producer=%d declared=%d verified=%d err=%s\n",
        producer, declared, verified, err);
  nyr_ir_func_free(&f);
  return ok;
}

static bool nyr_emit_keeps_only_canonical_return_class(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int value = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_CONST_F64,
                                          .dst = -1,
                                          .a = -1,
                                          .b = -1,
                                          .c = -1,
                                          .d = -1,
                                          .e = -1,
                                          .f = -1,
                                          .scalar_class = NYIR_SCALAR_F64});
  nyr_opt_ret(&f, value);
  /*
   * Bit 1 was NYIR_INST_F_RET_F64 in pre-class NYIR producers.  It is
   * intentionally numeric here: current NYIR exposes no legacy ABI flag.
   */
  f.data[f.len - 1].flags = 2u;
  char err[256] = {0};
  bool ok = f.data[f.len - 1].scalar_class == NYIR_SCALAR_UNKNOWN &&
            !nyr_ir_verify(&f, err, sizeof(err)) &&
            strstr(err, "invalid instruction flags") != NULL;
  if (!ok)
    fprintf(stderr, "[scalar-class] legacy return flag survived: %s\n", err);
  nyr_ir_func_free(&f);
  return ok;
}

static bool rejects_invalid_ownership_class(void) {
  static const nyr_op_t ops[] = {NYIR_CONST_I64, NYIR_COPY, NYIR_STORE_LOCAL,
                                  NYIR_PHI, NYIR_CALL};
  for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); ++i) {
    nyr_func_t f;
    nyr_opt_begin(&f);
    nyr_ir_emit(&f, (nyr_inst_t){.op = ops[i],
                                .dst = -1,
                                .a = -1,
                                .b = -1,
                                .c = -1,
                                .d = -1,
                                .e = -1,
                                .f = -1,
                                .semantic_ownership =
                                    (uint8_t)(NY_SEM_OWN_SHARED + 1)});
    char err[256] = {0};
    bool ok = !nyr_ir_verify(&f, err, sizeof(err)) &&
              strstr(err, "invalid semantic ownership class") != NULL;
    if (!ok)
      fprintf(stderr,
              "[scalar-class] invalid ownership accepted for opcode %d: %s\n",
              (int)ops[i], err);
    nyr_ir_func_free(&f);
    if (!ok)
      return false;
  }
  return true;
}

static bool checks_copy_ownership_flow(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int value =
      nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_CONST_I64,
                                  .dst = -1,
                                  .a = -1,
                                  .b = -1,
                                  .c = -1,
                                  .d = -1,
                                  .e = -1,
                                  .f = -1,
                                  .semantic_ownership = NY_SEM_OWN_OWNED});
  int copy =
      nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_COPY,
                                  .dst = -1,
                                  .a = value,
                                  .b = -1,
                                  .c = -1,
                                  .d = -1,
                                  .e = -1,
                                  .f = -1,
                                  .semantic_ownership = NY_SEM_OWN_BORROWED});
  nyr_opt_ret(&f, copy);
  char err[256] = {0};
  bool rejected = !nyr_ir_verify(&f, err, sizeof(err)) &&
                  strstr(err, "copy ownership contradicts its producer");
  f.data[1].semantic_ownership = NY_SEM_OWN_OWNED;
  err[0] = '\0';
  bool matching_ok = nyr_ir_verify(&f, err, sizeof(err));
  f.data[1].semantic_ownership = NY_SEM_OWN_UNKNOWN;
  err[0] = '\0';
  bool unknown_ok = nyr_ir_verify(&f, err, sizeof(err));
  if (!rejected || !matching_ok || !unknown_ok)
    fprintf(
        stderr,
        "[ownership] copy flow rejected=%d matching_ok=%d unknown_ok=%d: %s\n",
        rejected, matching_ok, unknown_ok, err);
  nyr_ir_func_free(&f);
  return rejected && unknown_ok;
}

static bool checks_store_ownership_flow(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int value =
      nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_CONST_I64,
                                  .dst = -1,
                                  .a = -1,
                                  .b = -1,
                                  .c = -1,
                                  .d = -1,
                                  .e = -1,
                                  .f = -1,
                                  .semantic_ownership = NY_SEM_OWN_OWNED});
  nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_STORE_LOCAL,
                              .dst = -1,
                              .a = value,
                              .b = -1,
                              .c = -1,
                              .d = -1,
                              .e = -1,
                              .f = -1,
                              .imm = 0,
                              .semantic_ownership = NY_SEM_OWN_BORROWED});
  nyr_opt_ret(&f, value);
  char err[256] = {0};
  bool rejected = !nyr_ir_verify(&f, err, sizeof(err)) &&
                  strstr(err, "store ownership contradicts its value");
  f.data[1].semantic_ownership = NY_SEM_OWN_OWNED;
  err[0] = '\0';
  bool matching_ok = nyr_ir_verify(&f, err, sizeof(err));
  f.data[0].semantic_ownership = NY_SEM_OWN_UNKNOWN;
  f.data[1].semantic_ownership = NY_SEM_OWN_BORROWED;
  err[0] = '\0';
  bool unknown_ok = nyr_ir_verify(&f, err, sizeof(err));
  if (!rejected || !matching_ok || !unknown_ok)
    fprintf(
        stderr,
        "[ownership] store flow rejected=%d matching_ok=%d unknown_ok=%d: %s\n",
        rejected, matching_ok, unknown_ok, err);
  nyr_ir_func_free(&f);
  return rejected && matching_ok && unknown_ok;
}

static bool checks_local_load_ownership_flow(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int owned =
      nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_CONST_I64,
                                  .dst = -1,
                                  .a = -1,
                                  .b = -1,
                                  .c = -1,
                                  .d = -1,
                                  .e = -1,
                                  .f = -1,
                                  .semantic_ownership = NY_SEM_OWN_OWNED});
  int borrowed =
      nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_CONST_I64,
                                  .dst = -1,
                                  .a = -1,
                                  .b = -1,
                                  .c = -1,
                                  .d = -1,
                                  .e = -1,
                                  .f = -1,
                                  .semantic_ownership = NY_SEM_OWN_BORROWED});
  nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_STORE_LOCAL,
                              .dst = -1,
                              .a = owned,
                              .b = -1,
                              .c = -1,
                              .d = -1,
                              .e = -1,
                              .f = -1,
                              .imm = 0,
                              .semantic_ownership = NY_SEM_OWN_OWNED});
  size_t overwrite = f.len;
  nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_STORE_LOCAL,
                              .dst = -1,
                              .a = borrowed,
                              .b = -1,
                              .c = -1,
                              .d = -1,
                              .e = -1,
                              .f = -1,
                              .imm = 0,
                              .semantic_ownership = NY_SEM_OWN_BORROWED});
  int loaded =
      nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                  .dst = -1,
                                  .a = -1,
                                  .b = -1,
                                  .c = -1,
                                  .d = -1,
                                  .e = -1,
                                  .f = -1,
                                  .imm = 0,
                                  .semantic_ownership = NY_SEM_OWN_OWNED});
  nyr_opt_ret(&f, loaded);
  char err[256] = {0};
  bool rejected = !nyr_ir_verify(&f, err, sizeof(err)) &&
                  strstr(err, "load ownership contradicts a reaching store");
  f.data[overwrite].semantic_ownership = NY_SEM_OWN_UNKNOWN;
  f.data[borrowed].semantic_ownership = NY_SEM_OWN_UNKNOWN;
  err[0] = '\0';
  bool unknown_ok = nyr_ir_verify(&f, err, sizeof(err));
  f.data[overwrite].semantic_ownership = NY_SEM_OWN_OWNED;
  f.data[borrowed].semantic_ownership = NY_SEM_OWN_OWNED;
  err[0] = '\0';
  bool matching_ok = nyr_ir_verify(&f, err, sizeof(err));
  if (!rejected || !unknown_ok || !matching_ok)
    fprintf(stderr,
            "[ownership] local-load flow rejected=%d unknown_ok=%d "
            "matching_ok=%d: %s\n",
            rejected, unknown_ok, matching_ok, err);
  nyr_ir_func_free(&f);
  return rejected && unknown_ok && matching_ok;
}

static bool checks_local_load_ownership_at_merge(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int cond = nyr_opt_const(&f, 1);
  int owned =
      nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_CONST_I64,
                                  .dst = -1,
                                  .a = -1,
                                  .b = -1,
                                  .c = -1,
                                  .d = -1,
                                  .e = -1,
                                  .f = -1,
                                  .semantic_ownership = NY_SEM_OWN_OWNED});
  int borrowed =
      nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_CONST_I64,
                                  .dst = -1,
                                  .a = -1,
                                  .b = -1,
                                  .c = -1,
                                  .d = -1,
                                  .e = -1,
                                  .f = -1,
                                  .semantic_ownership = NY_SEM_OWN_BORROWED});
  nyr_opt_br_if(&f, cond, 10);
  nyr_opt_br(&f, 20);
  nyr_opt_label(&f, 10);
  nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_STORE_LOCAL,
                              .dst = -1,
                              .a = owned,
                              .b = -1,
                              .c = -1,
                              .d = -1,
                              .e = -1,
                              .f = -1,
                              .imm = 0,
                              .semantic_ownership = NY_SEM_OWN_OWNED});
  nyr_opt_br(&f, 30);
  nyr_opt_label(&f, 20);
  size_t borrowed_store = f.len;
  nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_STORE_LOCAL,
                              .dst = -1,
                              .a = borrowed,
                              .b = -1,
                              .c = -1,
                              .d = -1,
                              .e = -1,
                              .f = -1,
                              .imm = 0,
                              .semantic_ownership = NY_SEM_OWN_BORROWED});
  nyr_opt_br(&f, 30);
  nyr_opt_label(&f, 30);
  int loaded =
      nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                  .dst = -1,
                                  .a = -1,
                                  .b = -1,
                                  .c = -1,
                                  .d = -1,
                                  .e = -1,
                                  .f = -1,
                                  .imm = 0,
                                  .semantic_ownership = NY_SEM_OWN_OWNED});
  nyr_opt_ret(&f, loaded);

  char err[256] = {0};
  bool rejected = !nyr_ir_verify(&f, err, sizeof(err)) &&
                  strstr(err, "load ownership contradicts a reaching store");
  f.data[borrowed].semantic_ownership = NY_SEM_OWN_OWNED;
  f.data[borrowed_store].semantic_ownership = NY_SEM_OWN_OWNED;
  err[0] = '\0';
  bool matching_ok = nyr_ir_verify(&f, err, sizeof(err));
  f.data[borrowed].semantic_ownership = NY_SEM_OWN_UNKNOWN;
  f.data[borrowed_store].semantic_ownership = NY_SEM_OWN_UNKNOWN;
  err[0] = '\0';
  bool unknown_ok = nyr_ir_verify(&f, err, sizeof(err));
  if (!rejected || !matching_ok || !unknown_ok)
    fprintf(stderr,
            "[ownership] merge rejected=%d matching_ok=%d unknown_ok=%d: %s\n",
            rejected, matching_ok, unknown_ok, err);
  nyr_ir_func_free(&f);
  return rejected && matching_ok && unknown_ok;
}

static bool unknown_call_invalidates_local_ownership(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int value =
      nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_CONST_I64,
                                  .dst = -1,
                                  .a = -1,
                                  .b = -1,
                                  .c = -1,
                                  .d = -1,
                                  .e = -1,
                                  .f = -1,
                                  .semantic_ownership = NY_SEM_OWN_OWNED});
  nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_STORE_LOCAL,
                              .dst = -1,
                              .a = value,
                              .b = -1,
                              .c = -1,
                              .d = -1,
                              .e = -1,
                              .f = -1,
                              .imm = 0,
                              .semantic_ownership = NY_SEM_OWN_OWNED});
  int address = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                                            .dst = -1,
                                            .a = -1,
                                            .b = -1,
                                            .c = -1,
                                            .d = -1,
                                            .e = -1,
                                            .f = -1,
                                            .imm = 0});
  nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_CALL,
                              .dst = -1,
                              .a = address,
                              .b = -1,
                              .c = -1,
                              .d = -1,
                              .e = -1,
                              .f = -1,
                              .imm = 1,
                              .symbol = "unknown_mutator"});
  int loaded =
      nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                  .dst = -1,
                                  .a = -1,
                                  .b = -1,
                                  .c = -1,
                                  .d = -1,
                                  .e = -1,
                                  .f = -1,
                                  .imm = 0,
                                  .semantic_ownership = NY_SEM_OWN_BORROWED});
  nyr_opt_ret(&f, loaded);
  char err[256] = {0};
  bool ok = nyr_ir_verify(&f, err, sizeof(err));
  if (!ok)
    fprintf(
        stderr,
        "[ownership] unknown address-taking call was not conservative: %s\n",
        err);
  nyr_ir_func_free(&f);
  return ok;
}

static bool checks_phi_ownership_flow(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int cond = nyr_opt_const(&f, 1);
  nyr_opt_br_if(&f, cond, 1);
  nyr_opt_br(&f, 2);
  nyr_opt_label(&f, 1);
  int owned =
      nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_CONST_I64,
                                  .dst = -1,
                                  .a = -1,
                                  .b = -1,
                                  .c = -1,
                                  .d = -1,
                                  .e = -1,
                                  .f = -1,
                                  .semantic_ownership = NY_SEM_OWN_OWNED});
  nyr_opt_br(&f, 3);
  nyr_opt_label(&f, 2);
  int borrowed =
      nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_CONST_I64,
                                  .dst = -1,
                                  .a = -1,
                                  .b = -1,
                                  .c = -1,
                                  .d = -1,
                                  .e = -1,
                                  .f = -1,
                                  .semantic_ownership = NY_SEM_OWN_BORROWED});
  nyr_opt_br(&f, 3);
  nyr_opt_label(&f, 3);
  nyr_phi_incoming_t *incoming = malloc(sizeof(*incoming) * 2);
  if (!incoming) {
    nyr_ir_func_free(&f);
    return false;
  }
  incoming[0] = (nyr_phi_incoming_t){.predecessor_label = 1, .value = owned};
  incoming[1] =
      (nyr_phi_incoming_t){.predecessor_label = 2, .value = borrowed};
  nyr_inst_t phi = {.op = NYIR_PHI,
                     .dst = -1,
                     .phi_incoming = incoming,
                     .phi_incoming_len = 2,
                     .semantic_ownership = NY_SEM_OWN_OWNED};
  int result = nyr_ir_emit(&f, phi);
  nyr_opt_ret(&f, result);
  char err[256] = {0};
  bool rejected = !nyr_ir_verify(&f, err, sizeof(err)) &&
                  strstr(err, "PHI ownership contradicts an incoming value");
  f.data[7].semantic_ownership = NY_SEM_OWN_OWNED;
  err[0] = '\0';
  bool matching_ok = nyr_ir_verify(&f, err, sizeof(err));
  f.data[7].semantic_ownership = NY_SEM_OWN_BORROWED;
  f.data[10].semantic_ownership = NY_SEM_OWN_UNKNOWN;
  err[0] = '\0';
  bool unknown_ok = nyr_ir_verify(&f, err, sizeof(err));
  if (!rejected || !matching_ok || !unknown_ok)
    fprintf(
        stderr,
        "[ownership] PHI flow rejected=%d matching_ok=%d unknown_ok=%d: %s\n",
        rejected, matching_ok, unknown_ok, err);
  nyr_ir_func_free(&f);
  return rejected && matching_ok && unknown_ok;
}

int main(void) {
  if (!checks_copy_ownership_flow() || !checks_store_ownership_flow() ||
      !checks_local_load_ownership_flow() || !checks_phi_ownership_flow() ||
      !checks_local_load_ownership_at_merge() ||
      !unknown_call_invalidates_local_ownership() ||
      !verify_call_ownership(NY_SEM_OWN_OWNED, NY_SEM_OWN_OWNED, true, NULL) ||
      !verify_call_ownership(NY_SEM_OWN_OWNED, NY_SEM_OWN_BORROWED, false,
                             "call argument ownership contradicts its value") ||
      !verify_call_ownership(NY_SEM_OWN_UNKNOWN, NY_SEM_OWN_SHARED, true,
                             NULL) ||
      !roundtrips_call_ownership() ||
      !rejects_ownership_metadata_on_non_call() ||
      !rejects_formal_contract_metadata_on_non_call() ||
      !verify_call_class(NYIR_SCALAR_F64, NYIR_SCALAR_F64, true, NULL) ||
      !verify_call_class(NYIR_SCALAR_F32, NYIR_SCALAR_F32, true, NULL) ||
      !verify_call_class(NYIR_SCALAR_F64, NYIR_SCALAR_F32, false,
                         "producer is f64 via const.f64") ||
      !verify_call_class(NYIR_SCALAR_F32, NYIR_SCALAR_F64, false,
                         "producer is f32 via const.f32") ||
      !verify_call_class(NYIR_SCALAR_F64, NYIR_SCALAR_UNKNOWN, false,
                         "producer is f64 via const.f64") ||
      !verify_call_class(
          NYIR_SCALAR_UNKNOWN, NYIR_SCALAR_F64, false,
          "has float ABI class f64 but producer is not a classified float") ||
      !verify_call_class(
          NYIR_SCALAR_UNKNOWN, NYIR_SCALAR_F32, false,
          "has float ABI class f32 but producer is not a classified float") ||
      !verify_call_class(NYIR_SCALAR_I64_TAGGED, NYIR_SCALAR_I64_RAW, false,
                         "i64-tagged != i64-raw") ||
      !verify_call_class(NYIR_SCALAR_I64_RAW, NYIR_SCALAR_I64_TAGGED, false,
                         "i64-raw != i64-tagged") ||
      !verify_call_class(NYIR_SCALAR_TYPED_BUFFER, NYIR_SCALAR_I64_TAGGED, true,
                         NULL) ||
      !verify_call_class(NYIR_SCALAR_POINTER, NYIR_SCALAR_I64_TAGGED, true,
                         NULL) ||
      !verify_call_class(NYIR_SCALAR_AGGREGATE, NYIR_SCALAR_POINTER, true,
                         NULL) ||
      !verify_call_class(NYIR_SCALAR_I64_RAW, NYIR_SCALAR_POINTER, true,
                         NULL) ||
      !verify_call_class(NYIR_SCALAR_AGGREGATE, NYIR_SCALAR_STRING, false,
                         "aggregate != string") ||
      !verify_call_class(NYIR_SCALAR_STRING, NYIR_SCALAR_AGGREGATE, false,
                         "string != aggregate") ||
      !verify_copied_call_class() ||
      !explicit_tagged_class_overrides_stale_float_type() ||
      !verify_store_class(NYIR_SCALAR_F64, NYIR_SCALAR_F64, true, NULL) ||
      !verify_store_class(NYIR_SCALAR_F32, NYIR_SCALAR_F32, true, NULL) ||
      !verify_store_class(NYIR_SCALAR_F64, NYIR_SCALAR_F32, false,
                          "store value is f64 but memory class is f32") ||
      !verify_store_class(NYIR_SCALAR_F32, NYIR_SCALAR_F64, false,
                          "store value is f32 but memory class is f64") ||
      !verify_local_store_class(NYIR_SCALAR_F64, NYIR_SCALAR_F64, true, NULL) ||
      !verify_local_store_class(NYIR_SCALAR_F32, NYIR_SCALAR_F32, true, NULL) ||
      !verify_local_store_class(
          NYIR_SCALAR_F64, NYIR_SCALAR_F32, false,
          "local store value is f64 but local ABI class is f32") ||
      !verify_local_store_class(
          NYIR_SCALAR_F32, NYIR_SCALAR_F64, false,
          "local store value is f32 but local ABI class is f64") ||
      !verify_store_class(
          NYIR_SCALAR_F64, NYIR_SCALAR_UNKNOWN, false,
          "store value is f64 but memory class is non-float/unknown") ||
      !verify_return_class(NYIR_SCALAR_F64, NYIR_SCALAR_F64, true, NULL) ||
      !verify_return_class(NYIR_SCALAR_F32, NYIR_SCALAR_F32, true, NULL) ||
      !verify_return_class(
          NYIR_SCALAR_F64, NYIR_SCALAR_F32, false,
          "return producer is f64 but return ABI class is f32") ||
      !verify_return_class(
          NYIR_SCALAR_F32, NYIR_SCALAR_UNKNOWN, false,
          "return producer is f32 but return ABI class is non-float/unknown") ||
      !verify_return_class(
          NYIR_SCALAR_UNKNOWN, NYIR_SCALAR_F64, false,
          "return producer is non-float/unknown but return ABI class is f64") ||
      !nyr_emit_keeps_only_canonical_return_class() ||
      !rejects_invalid_ownership_class() ||
      !runtime_value_formal_contracts_are_precise() ||
      !runtime_intrinsic_formal_contracts_are_declared() ||
      !load_legacy_return_class(NYIR_SCALAR_F64, 2u) ||
      !load_legacy_return_class(NYIR_SCALAR_F32, 4u))
    return 1;
  return 0;
}
