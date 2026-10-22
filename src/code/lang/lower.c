/*
 * Nytrix frontend lowering: translates typed compiler AST into NYIR, selecting
 * operand widths, applying ABI constraints, and emitting NYIR ops.
 *
 * Large lowering regions are kept in focused include fragments. This preserves
 * declaration order and a single translation unit while separating special/asm,
 * arithmetic, call, expression, and statement lowering for review and testing.
 */
#include "code/lang/lower.h"
#include "base/common.h"
#include "base/debug.h"
#include "base/intern.h"
#include "base/loader.h"
#include "base/time.h"
#include "base/trace.h"
#include "base/util.h"
#include "code/backend/abi.h"
#include "code/backend/internal.h"
#include "code/backend/ir/internal.h"
#include "code/lang/c/c.h"
#include "code/lang/parse/frontend/visitor.h"
#include "code/lang/parse/proof.h"
#include "code/lang/pipeline/legacy_backend.h"
#include "code/lang/pipeline/native.h"
#include "code/lang/priv.h"
#include "code/lang/runtime/aliases.h"
#include "code/lang/runtime/shared.h"
#include "code/lang/types/pipeline.h"
#include "code/lang/types/policy.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/*
 * AST-to-NYIR lowering, extern discovery, optimized construction, dumps, and
 * metadata summaries. Execution and target emission live in other modules.
 *
 * Unlike the LLVM codegen layer (src/code/lang/llvm/binary.c), the
 * native pipeline carries no tagged-int/BigInt fast-vs-slow dispatch.  NYIR
 * integer values are raw i64s and every supported binary op maps 1:1 to a raw
 * NYIR instruction (NYIR_ADD_I64 … NYIR_SAR_I64) with no runtime tag guard.
 * Unsupported dynamic shapes fail with a native diagnostic rather than
 * silently degrading to a runtime guard.
 *
 * Known slow-path fallbacks that still hand off to a slower form:
 *   - machine-form encode (ny_native_x86_64_emit_mach_scalar) → NYIR-object
 *     encoder (see ny_native_stat_nir_fallback),
 *   - register-color allocators (fp_fast_path / vec_fast_path) → stack spill
 *     (see ny_native_stat_regalloc_spilled),
 *   - tiered plan → AST interpretation
 *
 * Bounds-check elision in --safe-mode for Fin-typed indices is implemented:
 * ny_native_parse_fin_bound parses Fin<N> from parameter type annotations,
 * nyr_index_fin_bound_elision matches a Fin-indexed access against
 * the buffer's comptime byte length (tbuf_new provenance via
 * ny_native_extract_tbuf_new_len), and drops the NYIR_BOUNDS_CHECK when the
 * bound covers the access.  load64/store64/f64buf_load/store call it.
 *     (NY_NATIVE_CAP_AST_FALLBACK / prefer_ast_fallback).
 */

static bool nyr_binop(const char *op, nyr_op_t *out) {
  if (!op || !out)
    return false;
  if (strcmp(op, "+") == 0)
    *out = NYIR_ADD_I64;
  else if (strcmp(op, "-") == 0)
    *out = NYIR_SUB_I64;
  else if (strcmp(op, "*") == 0)
    *out = NYIR_MUL_I64;
  else if (strcmp(op, "/") == 0)
    *out = NYIR_DIV_I64;
  else if (strcmp(op, "%") == 0)
    *out = NYIR_MOD_I64;
  else if (strcmp(op, "&") == 0)
    *out = NYIR_AND_I64;
  else if (strcmp(op, "|") == 0)
    *out = NYIR_OR_I64;
  else if (strcmp(op, "^") == 0)
    *out = NYIR_XOR_I64;
  else if (strcmp(op, "<<") == 0)
    *out = NYIR_SHL_I64;
  else if (strcmp(op, ">>") == 0)
    *out = NYIR_SAR_I64;
  else
    return false;
  return true;
}

/*
 * Native NYIR has no general power instruction. Keep the supported constant
 * subset in the language IR: exponentiation by a non-negative integer literal
 * is deterministic, has no runtime helper dependency, and can be represented
 * by one raw integer constant. Overflow remains an explicit lowering failure
 * instead of inheriting host-C signed-overflow behaviour.
 */
static bool nyr_const_i64_expr(const expr_t *e, int64_t *out, unsigned depth) {
  if (!e || !out || depth > 64)
    return false;
  if (e->kind == NY_E_LITERAL && e->as.literal.kind == NY_LIT_INT &&
      e->tok.kind != NY_T_NIL) {
    *out = e->as.literal.as.i;
    return true;
  }
  if (e->kind == NY_E_UNARY && e->as.unary.right && e->as.unary.op &&
      (strcmp(e->as.unary.op, "+") == 0 || strcmp(e->as.unary.op, "-") == 0)) {
    int64_t value = 0;
    if (!nyr_const_i64_expr(e->as.unary.right, &value, depth + 1))
      return false;
    if (strcmp(e->as.unary.op, "-") == 0 && value == INT64_MIN)
      return false;
    *out = strcmp(e->as.unary.op, "-") == 0 ? -value : value;
    return true;
  }
  if (e->kind == NY_E_BINARY && e->as.binary.op &&
      strcmp(e->as.binary.op, "**") == 0) {
    int64_t base = 0, exponent = 0;
    if (!nyr_const_i64_expr(e->as.binary.left, &base, depth + 1) ||
        !nyr_const_i64_expr(e->as.binary.right, &exponent, depth + 1) ||
        exponent < 0)
      return false;
    uint64_t power = (uint64_t)exponent;
    int64_t result = 1;
    while (power) {
      if ((power & 1u) && __builtin_mul_overflow(result, base, &result))
        return false;
      power >>= 1u;
      if (power && __builtin_mul_overflow(base, base, &base))
        return false;
    }
    *out = result;
    return true;
  }
  return false;
}

static bool nyr_fold_const_pow(const expr_t *left, const expr_t *right,
                               int64_t *out) {
  int64_t base = 0, exponent_value = 0;
  if (!left || !right || !out || !nyr_const_i64_expr(left, &base, 0) ||
      !nyr_const_i64_expr(right, &exponent_value, 0) || exponent_value < 0)
    return false;
  int64_t result = 1;
  uint64_t exponent = (uint64_t)exponent_value;
  while (exponent) {
    if ((exponent & 1u) && __builtin_mul_overflow(result, base, &result))
      return false;
    exponent >>= 1u;
    if (exponent && __builtin_mul_overflow(base, base, &base))
      return false;
  }
  *out = result;
  return true;
}

static bool nyr_cmp(const char *op, nyr_cmp_t *out) {
  if (!op || !out)
    return false;
  if (strcmp(op, "==") == 0)
    *out = NYIR_CMP_EQ;
  else if (strcmp(op, "!=") == 0)
    *out = NYIR_CMP_NE;
  else if (strcmp(op, "<") == 0)
    *out = NYIR_CMP_LT;
  else if (strcmp(op, "<=") == 0)
    *out = NYIR_CMP_LE;
  else if (strcmp(op, ">") == 0)
    *out = NYIR_CMP_GT;
  else if (strcmp(op, ">=") == 0)
    *out = NYIR_CMP_GE;
  else
    return false;
  return true;
}

static const char *ny_native_call_leaf(const expr_t *e) {
  if (!e || e->kind != NY_E_CALL || !e->as.call.callee)
    return NULL;
  if (e->as.call.callee->kind == NY_E_IDENT)
    return ny_name_leaf(e->as.call.callee->as.ident.name);
  if (e->as.call.callee->kind == NY_E_MEMBER)
    return ny_name_leaf(e->as.call.callee->as.member.name);
  return NULL;
}
typedef enum {
  NY_NATIVE_LEAF_NONE = 0,
  NY_NATIVE_LEAF_INTRINSIC,
  NY_NATIVE_LEAF_ASSERT,
  NY_NATIVE_LEAF_PRINT,
  NY_NATIVE_LEAF_EPRINT,
  NY_NATIVE_LEAF_FLOAT,
  NY_NATIVE_LEAF_ARGC,
  NY_NATIVE_LEAF_TICKS,
  NY_NATIVE_LEAF_FLT_SQRT,
  NY_NATIVE_LEAF_ADDR,
  NY_NATIVE_LEAF_F64BUF_NEW,
  NY_NATIVE_LEAF_F64BUF_LOAD,
  NY_NATIVE_LEAF_F64BUF_STORE,
  NY_NATIVE_LEAF_I64BUF_NEW,
  NY_NATIVE_LEAF_I64BUF_LOAD,
  NY_NATIVE_LEAF_I64BUF_STORE,
  NY_NATIVE_LEAF_LOAD8,
  NY_NATIVE_LEAF_LOAD16,
  NY_NATIVE_LEAF_STORE8,
  NY_NATIVE_LEAF_STORE16,
  NY_NATIVE_LEAF_LOAD32,
  NY_NATIVE_LEAF_STORE32,
  NY_NATIVE_LEAF_LOAD64,
  NY_NATIVE_LEAF_LOAD64_IDX,
  NY_NATIVE_LEAF_STORE64,
  NY_NATIVE_LEAF_STORE64_H,
  NY_NATIVE_LEAF_STORE64_IDX,
  NY_NATIVE_LEAF_IS_STR,
  NY_NATIVE_LEAF_PROVE,
  NY_NATIVE_LEAF_STATIC_ASSERT,
  NY_NATIVE_LEAF_ASSERT_COMPILE,
  NY_NATIVE_LEAF_PROOF_MATCHES,
  NY_NATIVE_LEAF_RANGE_PROVEN,
  NY_NATIVE_LEAF_INDEX_PROVEN,
  NY_NATIVE_LEAF_LAYOUT_SIZE,
  NY_NATIVE_LEAF_LAYOUT_ALIGN,
  NY_NATIVE_LEAF_LAYOUT_OFFSET,
  NY_NATIVE_LEAF_LAYOUT_LOAD,
  NY_NATIVE_LEAF_LAYOUT_STORE,
  NY_NATIVE_LEAF_LAYOUT_FIELD_STORE,
} ny_native_leaf_kind_t;

static ny_native_leaf_kind_t ny_native_leaf_kind(const char *leaf) {
  static const struct {
    const char *name;
    ny_native_leaf_kind_t kind;
  } names[] = {
      {"intrinsic", NY_NATIVE_LEAF_INTRINSIC},
      {"assert", NY_NATIVE_LEAF_ASSERT},
      {"print", NY_NATIVE_LEAF_PRINT},
      {"eprint", NY_NATIVE_LEAF_EPRINT},
      /*
       * `println` has the same raw scalar ABI; the native emitter already
       * appends a newline for the print family.
       */
      {"println", NY_NATIVE_LEAF_PRINT},
      {"float", NY_NATIVE_LEAF_FLOAT},
      {"__argc", NY_NATIVE_LEAF_ARGC},
      {"argc", NY_NATIVE_LEAF_ARGC},
      {"ticks", NY_NATIVE_LEAF_TICKS},
      {"__flt_sqrt", NY_NATIVE_LEAF_FLT_SQRT},
      {"addr_of", NY_NATIVE_LEAF_ADDR},
      {"borrow", NY_NATIVE_LEAF_ADDR},
      {"f64buf_new", NY_NATIVE_LEAF_F64BUF_NEW},
      {"f64buf_load", NY_NATIVE_LEAF_F64BUF_LOAD},
      {"load64_f64", NY_NATIVE_LEAF_F64BUF_LOAD},
      {"f64buf_store", NY_NATIVE_LEAF_F64BUF_STORE},
      {"store64_f64", NY_NATIVE_LEAF_F64BUF_STORE},
      {"i64buf_new", NY_NATIVE_LEAF_I64BUF_NEW},
      {"i64buf_load", NY_NATIVE_LEAF_I64BUF_LOAD},
      {"i64buf_store", NY_NATIVE_LEAF_I64BUF_STORE},
      {"load8", NY_NATIVE_LEAF_LOAD8},
      {"__load8_idx", NY_NATIVE_LEAF_LOAD8},
      {"load16", NY_NATIVE_LEAF_LOAD16},
      {"__load16_idx", NY_NATIVE_LEAF_LOAD16},
      {"store8", NY_NATIVE_LEAF_STORE8},
      {"__store8_idx", NY_NATIVE_LEAF_STORE8},
      {"store16", NY_NATIVE_LEAF_STORE16},
      {"__store16_idx", NY_NATIVE_LEAF_STORE16},
      {"load32", NY_NATIVE_LEAF_LOAD32},
      {"__load32_idx", NY_NATIVE_LEAF_LOAD32},
      {"store32", NY_NATIVE_LEAF_STORE32},
      {"__store32_idx", NY_NATIVE_LEAF_STORE32},
      {"load64_i", NY_NATIVE_LEAF_LOAD64},
      {"load64_h", NY_NATIVE_LEAF_LOAD64},
      {"load64", NY_NATIVE_LEAF_LOAD64},
      {"__load64_h", NY_NATIVE_LEAF_LOAD64},
      {"__load64_idx", NY_NATIVE_LEAF_LOAD64_IDX},
      {"__load64_tagged_idx", NY_NATIVE_LEAF_LOAD64_IDX},
      {"store64_i", NY_NATIVE_LEAF_STORE64},
      {"store64_h", NY_NATIVE_LEAF_STORE64_H},
      {"store64", NY_NATIVE_LEAF_STORE64},
      {"__store64_h", NY_NATIVE_LEAF_STORE64_H},
      {"__store64_idx", NY_NATIVE_LEAF_STORE64_IDX},
      {"__is_str_obj", NY_NATIVE_LEAF_IS_STR},
      /*
       * Proof and layout intrinsics: one declarative row per spelling keeps
       * the lowering policy keyed on the leaf-kind table instead of scattered
       * string tests inside the call and statement lowerers.
       */
      {"prove", NY_NATIVE_LEAF_PROVE},
      {"static_assert", NY_NATIVE_LEAF_STATIC_ASSERT},
      {"assert_compile", NY_NATIVE_LEAF_ASSERT_COMPILE},
      {"proof_matches", NY_NATIVE_LEAF_PROOF_MATCHES},
      {"range_proven", NY_NATIVE_LEAF_RANGE_PROVEN},
      {"assert_compile_range", NY_NATIVE_LEAF_RANGE_PROVEN},
      {"index_proven", NY_NATIVE_LEAF_INDEX_PROVEN},
      {"assert_compile_index", NY_NATIVE_LEAF_INDEX_PROVEN},
      {"__layout_size", NY_NATIVE_LEAF_LAYOUT_SIZE},
      {"__layout_align", NY_NATIVE_LEAF_LAYOUT_ALIGN},
      {"__layout_offset", NY_NATIVE_LEAF_LAYOUT_OFFSET},
      {"load_layout", NY_NATIVE_LEAF_LAYOUT_LOAD},
      {"store_layout", NY_NATIVE_LEAF_LAYOUT_STORE},
      {"store_layout_field", NY_NATIVE_LEAF_LAYOUT_FIELD_STORE},
  };
  if (!leaf)
    return NY_NATIVE_LEAF_NONE;
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
    if (strcmp(leaf, names[i].name) == 0)
      return names[i].kind;
  return NY_NATIVE_LEAF_NONE;
}
/*
 * Runtime builtins are exported by their C implementation names in
 * src/code/lang/runtime/defs.h.  Canonical declarations use that generated
 * table; aliases with distinct native ABI bridges are listed explicitly below.
 */
#define NY_RT_SYMBOL_CACHE_BITS 13
#define NY_RT_SYMBOL_CACHE_SIZE (1u << NY_RT_SYMBOL_CACHE_BITS)
#define NY_RT_SYMBOL_CACHE_MASK (NY_RT_SYMBOL_CACHE_SIZE - 1u)
#define NY_RT_SYMBOL_KEY_MAX 160
static const char *ny_native_runtime_symbol_impl(const char *name);

/*
 * Kept sorted because runtime-symbol resolution uses binary search.
 */
const nyr_runtime_alias_t nyr_runtime_aliases[] = {
    {"__bigfloat_abs", "rt_bigfloat_abs"},
    {"__bigfloat_add", "rt_bigfloat_add"},
    {"__bigfloat_cmp", "rt_bigfloat_cmp_raw"},
    {"__bigfloat_div", "rt_bigfloat_div"},
    {"__bigfloat_from_value", "rt_bigfloat_from_value_raw"},
    {"__bigfloat_is", "rt_bigfloat_is"},
    {"__bigfloat_mul", "rt_bigfloat_mul"},
    {"__bigfloat_neg", "rt_bigfloat_neg"},
    {"__bigfloat_one", "rt_bigfloat_one"},
    {"__bigfloat_pow_int", "rt_bigfloat_pow_int_raw"},
    {"__bigfloat_precision", "rt_bigfloat_precision_raw"},
    {"__bigfloat_sqrt", "rt_bigfloat_sqrt"},
    {"__bigfloat_sub", "rt_bigfloat_sub"},
    {"__bigfloat_to_f64", "rt_bigfloat_to_f64_raw"},
    {"__bigfloat_to_str", "rt_bigfloat_to_str"},
    {"__bigfloat_zero", "rt_bigfloat_zero"},
    {"__bigint_add", "rt_bigint_add"},
    {"__bigint_and", "rt_bigint_and"},
    {"__bigint_bitlen", "rt_bigint_bitlen_raw"},
    {"__bigint_cmp", "rt_bigint_cmp_raw"},
    {"__bigint_div", "rt_bigint_div"},
    {"__bigint_mod", "rt_bigint_mod"},
    {"__bigint_mul", "rt_bigint_mul"},
    {"__bigint_or", "rt_bigint_or"},
    {"__bigint_pow", "rt_bigint_pow"},
    {"__bigint_sub", "rt_bigint_sub"},
    {"__bigint_to_int", "rt_bigint_to_i64_raw"},
    {"__bigint_to_str", "rt_bigint_to_str"},
    {"__bigint_xor", "rt_bigint_xor"},
    {"__bytes_new", "rt_bytes_new"},
    {"__bytes_set_raw", "rt_bytes_set_raw"},
    {"__close", "rt_close_raw"},
    {"__complex_imag", "rt_complex_im_f64"},
    {"__complex_real", "rt_complex_re_f64"},
    {"__contains", "rt_contains_raw"},
    {"__copy_mem", "rt_copy_mem_raw"},
    {"__copy_mem_raw", "rt_copy_mem_raw"},
    {"__cstr_to_str", "rt_cstr_to_str"},
    {"__dict_clear", "rt_dict_clear_raw"},
    {"__dict_clone", "rt_dict_clone_raw"},
    {"__dict_del", "rt_dict_delete_raw"},
    {"__dict_exists", "rt_dict_has_raw"},
    {"__dict_get", "rt_dict_get_raw"},
    {"__dict_get_i64", "rt_dict_get_i64_raw"},
    {"__dict_get_i64_raw", "rt_dict_get_i64_raw"},
    {"__dict_get_raw", "rt_dict_get_raw"},
    {"__dict_has", "rt_dict_has_raw"},
    {"__dict_items", "rt_dict_items_raw"},
    {"__dict_keys", "rt_dict_keys_raw"},
    {"__dict_len", "rt_dict_len_raw"},
    {"__dict_len_raw", "rt_dict_len_raw"},
    {"__dict_merge", "rt_dict_merge_raw"},
    {"__dict_remove", "rt_dict_delete_raw"},
    {"__dict_set", "rt_native_dict_set_nir_i64"},
    {"__dict_set_i64", "rt_dict_set_i64_raw"},
    {"__dict_set_i64_raw", "rt_native_dict_set_nir_i64"},
    {"__dict_set_raw", "rt_native_dict_set_nir_i64"},
    {"__dict_set_str", "rt_native_dict_set_str_compact"},
    {"__dict_set_str_raw", "rt_native_dict_set_str_compact"},
    {"__dict_values", "rt_dict_values_raw"},
    {"__f64_to_json_str", "rt_f64_to_json_str"},
    {"__free", "rt_free"},
    {"__free_raw", "rt_free_raw"},
    {"__gc_stat", "rt_gc_stat_raw"},
    {"__gc_tag_bytes", "rt_gc_tag_bytes_raw"},
    {"__globals", "rt_globals_get"},
    {"__has_tag", "rt_native_has_tag"},
    {"__init_str", "rt_init_str"},
    {"__init_str_raw", "rt_init_str_raw"},
    {"__is_int", "rt_native_is_int"},
    {"__is_ptr", "rt_value_is_ptr"},
    {"__len", "rt_len"},
    {"__list_set_len", "rt_list_set_len_raw"},
    {"__load_item", "rt_load_item"},
    {"__load_item_any", "rt_load_item_any"},
    {"__load_item_fast", "rt_load_item"},
    {"__long", "rt_long"},
    {"__malloc", "rt_malloc"},
    {"__malloc_raw", "rt_malloc_raw_i64"},
    {"__memcmp", "rt_memcmp_raw"},
    {"__memcpy", "rt_memcpy_raw"},
    {"__memset", "rt_memset_raw"},
    {"__mutex_free", "rt_mutex_free"},
    {"__mutex_lock64", "rt_mutex_lock64"},
    {"__mutex_new", "rt_mutex_new"},
    {"__mutex_unlock64", "rt_mutex_unlock64"},
    {"__open", "rt_open_raw"},
    {"__print_flush", "rt_print_flush_raw"},
    {"__ptr_add", "rt_ptr_add_i64"},
    {"__ptr_sub", "rt_ptr_sub_i64"},
    {"__read_fd_raw", "rt_read_fd_raw"},
    {"__read_off", "rt_read_off_raw"},
    {"__read_off_raw", "rt_read_off_raw"},
    {"__realloc", "rt_realloc_raw"},
    {"__request_native_oracle", "rt_request_native_oracle_raw"},
    {"__request_tv_checks", "rt_request_tv_checks_raw"},
    {"__result_err", "rt_result_err"},
    {"__result_ok", "rt_result_ok"},
    {"__runtime_alloc_count", "rt_runtime_alloc_count_raw"},
    {"__runtime_realloc_count", "rt_runtime_realloc_count_raw"},
    {"__runtime_tag", "rt_runtime_tag_raw_value"},
    {"__runtime_tag_raw", "rt_runtime_tag_raw_value"},
    {"__set_globals", "rt_globals_set"},
    {"__shl", "rt_shl_raw"},
    {"__simmd_bswap32", "rt_simmd_bswap32_i64"},
    {"__simmd_bswap64", "rt_simmd_bswap64_i64"},
    {"__simmd_clz32", "rt_simmd_clz32_i64"},
    {"__simmd_clz64", "rt_simmd_clz64_i64"},
    {"__simmd_ctz32", "rt_simmd_ctz32_i64"},
    {"__simmd_ctz64", "rt_simmd_ctz64_i64"},
    {"__simmd_popcnt32", "rt_simmd_popcnt32_i64"},
    {"__simmd_popcnt64", "rt_simmd_popcnt64_i64"},
    {"__simmd_rotl32", "rt_simmd_rotl32_i64"},
    {"__simmd_rotl64", "rt_simmd_rotl64_i64"},
    {"__simmd_rotr32", "rt_simmd_rotr32_i64"},
    {"__simmd_rotr64", "rt_simmd_rotr64_i64"},
    {"__sound_web_play", "rt_sound_web_play"},
    {"__spawn_pipe", "rt_spawn_pipe"},
    {"__store_item", "rt_store_item"},
    {"__store_item_fast", "rt_store_item_fast_raw"},
    {"__str_builder_append", "rt_str_builder_append"},
    {"__str_builder_append_byte", "rt_str_builder_append_byte"},
    {"__str_builder_free", "rt_str_builder_free"},
    {"__str_builder_new", "rt_str_builder_new"},
    {"__str_builder_to_str", "rt_str_builder_to_str"},
    {"__str_byte", "rt_str_byte"},
    {"__str_concat", "rt_str_concat"},
    {"__str_eq", "rt_str_eq"},
    {"__str_len", "rt_cstr_len"},
    {"__syscall", "rt_syscall_raw"},
    {"__syscall_raw", "rt_syscall_raw"},
    {"__tag", "rt_tag"},
    {"__tagof", "rt_value_tag"},
    {"__thread_join", "rt_thread_join"},
    {"__thread_launch_call", "rt_thread_launch_call_argv"},
    {"__thread_spawn_call", "rt_thread_spawn_call_argv"},
    {"__trace_allocations", "rt_trace_allocations_raw"},
    {"__trace_arg_f64", "rt_trace_arg_f64"},
    {"__trace_arg_f64_bits", "rt_trace_arg_f64_bits"},
    {"__trace_arg_i64", "rt_trace_arg_i64"},
    {"__trace_arg_tagged", "rt_trace_arg_tagged"},
    {"__trace_count", "rt_trace_count_raw"},
    {"__trace_cpu_ns", "rt_trace_cpu_ns_raw"},
    {"__trace_dump", "rt_trace_dump_raw"},
    {"__trace_enter", "rt_trace_enter_raw"},
    {"__trace_exclusive_cpu_ns", "rt_trace_exclusive_cpu_ns_raw"},
    {"__trace_exclusive_ns", "rt_trace_exclusive_ns_raw"},
    {"__trace_exit", "rt_trace_exit_raw"},
    {"__trace_func", "rt_trace_func_raw"},
    {"__trace_inclusive_ns", "rt_trace_inclusive_ns_raw"},
    {"__trace_loc", "rt_trace_loc_raw"},
    {"__trace_reset", "rt_trace_reset_raw"},
    {"__trace_ret_i64", "rt_trace_ret_i64_raw"},
    {"__trace_ret_u64", "rt_trace_ret_u64_raw"},
    {"__trace_ret_void", "rt_trace_ret_void_raw"},
    {"__trace_sampled_rss_delta", "rt_trace_sampled_rss_delta_raw"},
    {"__trace_sampled_rss_peak", "rt_trace_sampled_rss_peak_raw"},
    {"__tty_isatty", "rt_tty_isatty_raw"},
    {"__tty_pending", "rt_tty_pending_raw"},
    {"__tty_raw", "rt_tty_raw_raw"},
    {"__type_name_tagged", "rt_type_name_tagged"},
    {"__untag", "rt_untag"},
    {"__unwrap", "rt_unwrap"},
    {"__value_get_index_raw", "rt_value_get_index_raw"},
    {"__vec_mark", "rt_vec_mark_raw"},
    {"__wait4", "rt_wait4"},
    {"__wait_process", "rt_wait_process"},
    {"__write_buffered", "rt_write_buffered_raw"},
    {"__write_buffered_raw", "rt_write_buffered_raw"},
    {"__write_fd_raw", "rt_write_fd_raw"},
    {"__write_off", "rt_write_off"},
    {"__write_off_raw", "rt_write_off_raw"},
    {"arch", "rt_arch_name"},
    {"args", "rt_args_raw"},
    {"bytes", "rt_bytes_new_raw"},
    {"bytes_get", "rt_bytes_get_raw"},
    {"bytes_new_raw", "rt_bytes_new"},
    {"bytes_set", "rt_bytes_set_raw"},
    {"canvas", "rt_canvas"},
    {"canvas_clear", "rt_canvas_clear"},
    {"canvas_free", "rt_canvas_free"},
    {"canvas_print", "rt_canvas_print"},
    {"canvas_refresh", "rt_canvas_refresh"},
    {"canvas_set", "rt_canvas_set"},
    {"color", "rt_color_raw"},
    {"contains", "rt_contains_raw"},
    {"core.__memcmp", "rt_memcmp_raw"},
    {"core.__memcpy", "rt_memcpy_raw"},
    {"core.__memset", "rt_memset_raw"},
    {"core.any.long", "rt_long"},
    {"core.bytes", "rt_bytes_new_raw"},
    {"core.bytes.long", "rt_long"},
    {"core.bytes_get", "rt_bytes_get_raw"},
    {"core.bytes_set", "rt_bytes_set_raw"},
    {"core.compiler.syntax.type.is_str", "rt_is_str_obj"},
    {"core.contains", "rt_contains_raw"},
    {"core.dict_mod.dict_len", "rt_dict_len_raw"},
    {"core.dict_mod.list", "rt_list_new_raw"},
    {"core.is_ptr", "rt_value_is_ptr"},
    {"core.len", "rt_len"},
    {"core.list", "rt_list_new_raw"},
    {"core.list.long", "rt_long"},
    {"core.math.float.is_float", "rt_is_float_obj"},
    {"core.os.args.args", "rt_args_raw"},
    {"core.os.exit", "rt_exit"},
    {"core.os.ffi.__call4_ptr_ptr_ptr_ptr_i32", "rt_call4_ptr_ptr_ptr_ptr_i32"},
    {"core.os.ffi.__call4_ptr_u64_ptr_ptr_i32", "rt_call4_ptr_u64_ptr_ptr_i32"},
    {"core.os.file_exists", "rt_file_exists_raw"},
    {"core.primitives.__list_set_len", "rt_list_set_len_raw"},
    {"core.primitives.globals", "rt_globals_get"},
    {"core.primitives.set_globals", "rt_globals_set"},
    {"core.reflect.__value_get_index_raw", "rt_value_get_index_raw"},
    {"core.reflect.globals", "rt_globals_get"},
    {"core.set_mod.len", "rt_set_len_raw"},
    {"core.str.long", "rt_long"},
    {"core.term.canvas", "rt_canvas"},
    {"core.term.canvas_clear", "rt_canvas_clear"},
    {"core.term.canvas_free", "rt_canvas_free"},
    {"core.term.canvas_print", "rt_canvas_print"},
    {"core.term.canvas_refresh", "rt_canvas_refresh"},
    {"core.term.canvas_set", "rt_canvas_set"},
    {"core.term.color", "rt_color_raw"},
    {"core.term.get_terminal_size", "rt_terminal_size_raw"},
    {"core.term.write_str", "rt_print_cstr"},
    {"cos", "rt_cos_f64"},
    {"dict_clear", "rt_dict_clear_raw"},
    {"dict_clone", "rt_dict_clone_raw"},
    {"dict_del", "rt_dict_delete_raw"},
    {"dict_exists", "rt_dict_has_raw"},
    {"dict_get", "rt_dict_get_raw"},
    {"dict_get_i64", "rt_dict_get_i64_raw"},
    {"dict_has", "rt_dict_has_raw"},
    {"dict_items", "rt_dict_items_raw"},
    {"dict_keys", "rt_dict_keys_raw"},
    {"dict_len", "rt_dict_len_raw"},
    {"dict_merge", "rt_dict_merge_raw"},
    {"dict_remove", "rt_dict_delete_raw"},
    {"dict_set", "rt_native_dict_set_nir_i64"},
    {"dict_set_i64", "rt_dict_set_i64_raw"},
    {"dict_set_str", "rt_native_dict_set_str_compact"},
    {"dict_values", "rt_dict_values_raw"},
    {"exit", "rt_exit"},
    {"file_exists", "rt_file_exists_raw"},
    {"free", "rt_free"},
    {"free_raw", "rt_free_raw"},
    {"get_terminal_size", "rt_terminal_size_raw"},
    {"globals", "rt_globals_get"},
    {"is_float", "rt_is_float_obj"},
    {"is_ptr", "rt_value_is_ptr"},
    {"len", "rt_len"},
    {"list", "rt_list_new_raw"},
    {"long", "rt_long"},
    {"malloc", "rt_malloc"},
    {"memcmp", "rt_memcmp_raw"},
    {"memcpy", "rt_memcpy_raw"},
    {"memset", "rt_memset_raw"},
    {"mutex_free", "rt_mutex_free"},
    {"mutex_lock", "rt_mutex_lock64"},
    {"mutex_new", "rt_mutex_new"},
    {"mutex_unlock", "rt_mutex_unlock64"},
    {"os", "rt_os_name"},
    {"realloc", "rt_realloc_raw"},
    {"rt_any_div", "rt_any_div"},
    {"rt_any_mod", "rt_any_mod"},
    {"rt_any_mul", "rt_any_mul"},
    {"rt_box_i64", "rt_box_i64"},
    {"rt_bytes_index_read_raw", "rt_bytes_index_read_raw"},
    {"rt_bytes_new", "rt_bytes_new"},
    {"rt_cstr_index_read_raw", "rt_cstr_index_read_raw"},
    {"rt_dict_get_str_raw", "rt_dict_get_str_raw"},
    {"rt_free", "rt_free"},
    {"rt_index_key_error", "rt_index_key_error"},
    {"rt_len", "rt_len"},
    {"rt_malloc", "rt_malloc"},
    {"rt_malloc_raw", "rt_malloc_raw_i64"},
    {"rt_range_index_read_raw", "rt_range_index_read_raw"},
    {"rt_result_unwrap_or_any", "rt_result_unwrap_or_any"},
    {"rt_result_unwrap_or_raw", "rt_result_unwrap_or_raw"},
    {"rt_str_concat", "rt_str_concat"},
    {"rt_str_eq", "rt_str_eq"},
    {"rt_str_len", "rt_cstr_len"},
    {"rt_tag_or_raw_int", "rt_tag_or_raw_int"},
    {"rt_tbuf_get_any", "rt_tbuf_get_any"},
    {"rt_tbuf_index_any", "rt_tbuf_index_any"},
    {"rt_tbuf_index_any_raw", "rt_tbuf_index_any_raw"},
    {"rt_tbuf_index_read_raw", "rt_tbuf_index_read_raw"},
    {"rt_tbuf_swap", "rt_tbuf_swap"},
    {"rt_tbuf_to_cstr", "rt_tbuf_to_cstr"},
    {"rt_trace_arg_f64", "rt_trace_arg_f64"},
    {"rt_trace_arg_f64_bits", "rt_trace_arg_f64_bits"},
    {"rt_trace_arg_i64", "rt_trace_arg_i64"},
    {"rt_trace_arg_tagged", "rt_trace_arg_tagged"},
    {"rt_trace_ret_i64", "rt_trace_ret_i64_raw"},
    {"rt_trace_ret_u64", "rt_trace_ret_u64_raw"},
    {"rt_trace_ret_void", "rt_trace_ret_void_raw"},
    {"rt_value_is_ptr", "rt_value_is_ptr"},
    {"runtime_tag_raw", "rt_runtime_tag_raw_value"},
    {"set_globals", "rt_globals_set"},
    {"sin", "rt_sin_f64"},
    {"sqrt", "rt_sqrt_f64"},
    {"thread_join", "rt_thread_join"},
    {"thread_launch_call", "rt_thread_launch_call"},
    {"thread_spawn_call", "rt_thread_spawn_call"},
    {"write_str", "rt_print_cstr"},
};
const size_t nyr_runtime_alias_count =
    sizeof(nyr_runtime_aliases) / sizeof(nyr_runtime_aliases[0]);

const char *nyr_runtime_symbol(const char *name) {
  if (!name || !*name)
    return NULL;
  /*
   * Symbol resolution is a pure function of the name, but every miss pays a
   * binary search over the alias table plus a linear strcmp scan of the
   * generated runtime-declaration table.  Representation queries call this
   * millions of times per cold compile (recursive is_f64/is_cstr probes
   * resolve the callee leaf on every level), which made those string scans
   * the dominant compile cost of import-heavy programs.  Memoize on a copy
   * of the key: callers routinely pass short-lived stack buffers, so the
   * pointer itself cannot key the cache.
   */
  static __thread struct {
    char name[NY_RT_SYMBOL_KEY_MAX];
    const char *symbol;
    bool probed;
  } memo[NY_RT_SYMBOL_CACHE_SIZE];
  uint32_t h = 2166136261u;
  size_t len = 0;
  for (const char *p = name; *p; ++p) {
    h = (h ^ (uint8_t)*p) * 16777619u;
    ++len;
  }
  uint32_t slot = h & NY_RT_SYMBOL_CACHE_MASK;
  if (memo[slot].probed && strcmp(memo[slot].name, name) == 0)
    return memo[slot].symbol;
  const char *symbol = ny_native_runtime_symbol_impl(name);
  if (len < NY_RT_SYMBOL_KEY_MAX) {
    memcpy(memo[slot].name, name, len + 1);
    memo[slot].symbol = symbol;
    memo[slot].probed = true;
  }
  return symbol;
}

static const char *ny_native_runtime_symbol_impl(const char *name) {
  if (!name || !*name)
    return NULL;
  size_t lo = 0, hi = nyr_runtime_alias_count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    int order = strcmp(name, nyr_runtime_aliases[mid].name);
    if (order == 0)
      return nyr_runtime_aliases[mid].symbol;
    if (order < 0)
      hi = mid;
    else
      lo = mid + 1;
  }

#define RT_DEF(n, implementation, args, sig, doc)                              \
  if (strcmp(name, n) == 0)                                                    \
    return #implementation;
#define RT_GV(n, implementation, type, doc)
#include "code/lang/runtime/defs.h"
#undef RT_GV
#undef RT_DEF
  return NULL;
}

const ny_native_runtime_abi_decl_t ny_native_runtime_abi_decls[] = {
    {"rt_any_add",
     NY_NATIVE_RUNTIME_ABI_TAGGED_ARG(0) | NY_NATIVE_RUNTIME_ABI_TAGGED_ARG(1) |
         NY_NATIVE_RUNTIME_ABI_RESULT_TAGGED_VALUE,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_any_collection_remove_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_TAGGED_ARG(0) |
         NY_NATIVE_RUNTIME_ABI_TAGGED_ARG(1) |
         NY_NATIVE_RUNTIME_ABI_RESULT_TAGGED_VALUE,
     NY_NATIVE_VM_I64_BINARY_TO_I64,
     {.i64_binary_i64 = rt_any_collection_remove_raw}},
    {"rt_any_div",
     NY_NATIVE_RUNTIME_ABI_TAGGED_ARG(0) | NY_NATIVE_RUNTIME_ABI_TAGGED_ARG(1) |
         NY_NATIVE_RUNTIME_ABI_RESULT_TAGGED_VALUE,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_any_eq",
     NY_NATIVE_RUNTIME_ABI_TAGGED_ARG(0) | NY_NATIVE_RUNTIME_ABI_TAGGED_ARG(1),
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_any_len",
     NY_NATIVE_RUNTIME_ABI_TAGGED_ARG(0),
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_any_mod",
     NY_NATIVE_RUNTIME_ABI_TAGGED_ARG(0) | NY_NATIVE_RUNTIME_ABI_TAGGED_ARG(1) |
         NY_NATIVE_RUNTIME_ABI_RESULT_TAGGED_VALUE,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_any_mul",
     NY_NATIVE_RUNTIME_ABI_TAGGED_ARG(0) | NY_NATIVE_RUNTIME_ABI_TAGGED_ARG(1) |
         NY_NATIVE_RUNTIME_ABI_RESULT_TAGGED_VALUE,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_any_tag",
     NY_NATIVE_RUNTIME_ABI_TAGGED_ARG(0),
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_any_to_cstr",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_VALUE_CONVERSION |
         NY_NATIVE_RUNTIME_ABI_TAGGED_ARG(0) |
         NY_NATIVE_RUNTIME_ABI_RESULT_CSTR,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_any_to_f64",
     NY_NATIVE_RUNTIME_ABI_RESULT_F64 | NY_NATIVE_RUNTIME_ABI_VALUE_CONVERSION,
     NY_NATIVE_VM_I64_UNARY_TO_F64,
     {.i64_unary_f64 = rt_any_to_f64}},
    {"rt_any_to_i64",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_VALUE_CONVERSION |
         NY_NATIVE_RUNTIME_ABI_TAGGED_ARG(0) |
         NY_NATIVE_RUNTIME_ABI_RESULT_RAW_INT,
     NY_NATIVE_VM_I64_UNARY_TO_I64,
     {.i64_unary_i64 = rt_any_to_i64}},
    {"rt_arch_name",
     NY_NATIVE_RUNTIME_ABI_RESULT_TAGGED_VALUE,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_args_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_atof",
     NY_NATIVE_RUNTIME_ABI_RESULT_F64,
     NY_NATIVE_VM_I64_UNARY_TO_F64,
     {.i64_unary_f64 = rt_atof}},
    {"rt_bigfloat_abs",
     NY_NATIVE_RUNTIME_ABI_RESULT_TAGGED_VALUE,
     NY_NATIVE_VM_I64_UNARY_TO_I64,
     {.i64_unary_i64 = rt_bigfloat_abs}},
    {"rt_bigfloat_add",
     NY_NATIVE_RUNTIME_ABI_RESULT_TAGGED_VALUE,
     NY_NATIVE_VM_I64_BINARY_TO_I64,
     {.i64_binary_i64 = rt_bigfloat_add}},
    {"rt_bigfloat_cmp_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_RAW_INT,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_bigfloat_div",
     NY_NATIVE_RUNTIME_ABI_RESULT_TAGGED_VALUE,
     NY_NATIVE_VM_I64_BINARY_TO_I64,
     {.i64_binary_i64 = rt_bigfloat_div}},
    {"rt_bigfloat_from_f64",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_TAGGED_ARG(1) |
         NY_NATIVE_RUNTIME_ABI_RESULT_TAGGED_VALUE,
     NY_NATIVE_VM_F64_I64_TO_I64,
     {.f64_i64_i64 = rt_bigfloat_from_f64}},
    {"rt_bigfloat_from_value_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_VALUE_CONVERSION |
         NY_NATIVE_RUNTIME_ABI_TAGGED_ARG(0) |
         NY_NATIVE_RUNTIME_ABI_RESULT_TAGGED_VALUE,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_bigfloat_is",
     NY_NATIVE_RUNTIME_ABI_RESULT_TAGGED_VALUE,
     NY_NATIVE_VM_I64_UNARY_TO_I64,
     {.i64_unary_i64 = rt_bigfloat_is}},
    {"rt_bigfloat_mul",
     NY_NATIVE_RUNTIME_ABI_RESULT_TAGGED_VALUE,
     NY_NATIVE_VM_I64_BINARY_TO_I64,
     {.i64_binary_i64 = rt_bigfloat_mul}},
    {"rt_bigfloat_neg",
     NY_NATIVE_RUNTIME_ABI_RESULT_TAGGED_VALUE,
     NY_NATIVE_VM_I64_UNARY_TO_I64,
     {.i64_unary_i64 = rt_bigfloat_neg}},
    {"rt_bigfloat_one",
     NY_NATIVE_RUNTIME_ABI_RESULT_TAGGED_VALUE,
     NY_NATIVE_VM_I64_UNARY_TO_I64,
     {.i64_unary_i64 = rt_bigfloat_one}},
    {"rt_bigfloat_pow_int_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_TAGGED_VALUE,
     NY_NATIVE_VM_I64_BINARY_TO_I64,
     {.i64_binary_i64 = rt_bigfloat_pow_int_raw}},
    {"rt_bigfloat_precision_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_RAW_WORD,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_bigfloat_sqrt",
     NY_NATIVE_RUNTIME_ABI_RESULT_TAGGED_VALUE,
     NY_NATIVE_VM_I64_UNARY_TO_I64,
     {.i64_unary_i64 = rt_bigfloat_sqrt}},
    {"rt_bigfloat_sub",
     NY_NATIVE_RUNTIME_ABI_RESULT_TAGGED_VALUE,
     NY_NATIVE_VM_I64_BINARY_TO_I64,
     {.i64_binary_i64 = rt_bigfloat_sub}},
    {"rt_bigfloat_to_f64_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_TAGGED_ARG(0) |
         NY_NATIVE_RUNTIME_ABI_RESULT_F64 |
         NY_NATIVE_RUNTIME_ABI_VALUE_CONVERSION,
     NY_NATIVE_VM_I64_UNARY_TO_F64,
     {.i64_unary_f64 = rt_bigfloat_to_f64_raw}},
    {"rt_bigfloat_to_str",
     NY_NATIVE_RUNTIME_ABI_RESULT_TAGGED_VALUE,
     NY_NATIVE_VM_I64_UNARY_TO_I64,
     {.i64_unary_i64 = rt_bigfloat_to_str}},
    {"rt_bigfloat_zero",
     NY_NATIVE_RUNTIME_ABI_RESULT_TAGGED_VALUE,
     NY_NATIVE_VM_I64_UNARY_TO_I64,
     {.i64_unary_i64 = rt_bigfloat_zero}},
    {"rt_bigint_add",
     NY_NATIVE_RUNTIME_ABI_BIGINT_VALUES | NY_NATIVE_RUNTIME_ABI_RESULT_BIGINT,
     NY_NATIVE_VM_I64_BINARY_TO_I64,
     {.i64_binary_i64 = rt_bigint_add}},
    {"rt_bigint_and",
     NY_NATIVE_RUNTIME_ABI_BIGINT_VALUES | NY_NATIVE_RUNTIME_ABI_RESULT_BIGINT,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_bigint_bitlen_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_RAW_INT |
         NY_NATIVE_RUNTIME_ABI_BIGINT_VALUES,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_bigint_clz",
     NY_NATIVE_RUNTIME_ABI_RESULT_TAGGED_INT,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_bigint_cmp_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_RAW_INT |
         NY_NATIVE_RUNTIME_ABI_BIGINT_VALUES,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_bigint_ctz",
     NY_NATIVE_RUNTIME_ABI_RESULT_TAGGED_INT,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_bigint_div",
     NY_NATIVE_RUNTIME_ABI_BIGINT_VALUES | NY_NATIVE_RUNTIME_ABI_RESULT_BIGINT,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_bigint_from_i64_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_VALUE_CONVERSION |
         NY_NATIVE_RUNTIME_ABI_RESULT_BIGINT,
     NY_NATIVE_VM_I64_UNARY_TO_I64,
     {.i64_unary_i64 = rt_bigint_from_i64_raw}},
    {"rt_bigint_from_int",
     NY_NATIVE_RUNTIME_ABI_TAGGED_ARG(0) |
         NY_NATIVE_RUNTIME_ABI_VALUE_CONVERSION |
         NY_NATIVE_RUNTIME_ABI_RESULT_BIGINT,
     NY_NATIVE_VM_I64_UNARY_TO_I64,
     {.i64_unary_i64 = rt_bigint_from_int}},
    {"rt_bigint_iroot",
     NY_NATIVE_RUNTIME_ABI_BIGINT_VALUES | NY_NATIVE_RUNTIME_ABI_RESULT_BIGINT,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_bigint_jacobi",
     NY_NATIVE_RUNTIME_ABI_RESULT_TAGGED_INT,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_bigint_kronecker",
     NY_NATIVE_RUNTIME_ABI_RESULT_TAGGED_INT,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_bigint_legendre",
     NY_NATIVE_RUNTIME_ABI_RESULT_TAGGED_INT,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_bigint_mod",
     NY_NATIVE_RUNTIME_ABI_BIGINT_VALUES | NY_NATIVE_RUNTIME_ABI_RESULT_BIGINT,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_bigint_modinv",
     NY_NATIVE_RUNTIME_ABI_BIGINT_VALUES | NY_NATIVE_RUNTIME_ABI_RESULT_BIGINT,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_bigint_mul",
     NY_NATIVE_RUNTIME_ABI_BIGINT_VALUES | NY_NATIVE_RUNTIME_ABI_RESULT_BIGINT,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_bigint_neg_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_BIGINT_VALUES |
         NY_NATIVE_RUNTIME_ABI_RESULT_BIGINT,
     NY_NATIVE_VM_I64_UNARY_TO_I64,
     {.i64_unary_i64 = rt_bigint_neg_raw}},
    {"rt_bigint_or",
     NY_NATIVE_RUNTIME_ABI_BIGINT_VALUES | NY_NATIVE_RUNTIME_ABI_RESULT_BIGINT,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_bigint_pow",
     NY_NATIVE_RUNTIME_ABI_BIGINT_VALUES | NY_NATIVE_RUNTIME_ABI_RESULT_BIGINT,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_bigint_sub",
     NY_NATIVE_RUNTIME_ABI_BIGINT_VALUES | NY_NATIVE_RUNTIME_ABI_RESULT_BIGINT,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_bigint_submul",
     NY_NATIVE_RUNTIME_ABI_BIGINT_VALUES | NY_NATIVE_RUNTIME_ABI_RESULT_BIGINT,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_bigint_to_cstr_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_BIGINT_VALUES |
         NY_NATIVE_RUNTIME_ABI_RESULT_CSTR,
     NY_NATIVE_VM_I64_UNARY_TO_I64,
     {.i64_unary_i64 = rt_bigint_to_cstr_raw}},
    {"rt_bigint_to_i64_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_RAW_INT |
         NY_NATIVE_RUNTIME_ABI_VALUE_CONVERSION |
         NY_NATIVE_RUNTIME_ABI_BIGINT_VALUES,
     NY_NATIVE_VM_I64_UNARY_TO_I64,
     {.i64_unary_i64 = rt_bigint_to_i64_raw}},
    {"rt_bigint_to_int",
     NY_NATIVE_RUNTIME_ABI_BIGINT_VALUES |
         NY_NATIVE_RUNTIME_ABI_RESULT_TAGGED_VALUE,
     NY_NATIVE_VM_I64_UNARY_TO_I64,
     {.i64_unary_i64 = rt_bigint_to_int}},
    {"rt_bigint_to_str",
     NY_NATIVE_RUNTIME_ABI_BIGINT_VALUES | NY_NATIVE_RUNTIME_ABI_RESULT_CSTR,
     NY_NATIVE_VM_I64_UNARY_TO_I64,
     {.i64_unary_i64 = rt_bigint_to_str}},
    {"rt_bigint_xor",
     NY_NATIVE_RUNTIME_ABI_BIGINT_VALUES | NY_NATIVE_RUNTIME_ABI_RESULT_BIGINT,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_bool_to_cstr",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_CSTR,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_box_i64",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_TAGGED_VALUE,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_bshr_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_RAW_WORD,
     NY_NATIVE_VM_I64_BINARY_TO_I64,
     {.i64_binary_i64 = rt_bshr_raw}},
    {"rt_bytes_get_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_bytes_index_read_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_bytes_new",
     NY_NATIVE_RUNTIME_ABI_RESULT_CSTR,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_bytes_new_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_CSTR,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_bytes_set_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_CSTR,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_call0_i32",
     NY_NATIVE_RUNTIME_ABI_TAGGED_VALUES |
         NY_NATIVE_RUNTIME_ABI_RESULT_TAGGED_INT,
     NY_NATIVE_VM_I64_UNARY_TO_I64,
     {.i64_unary_i64 = rt_call0_i32}},
    {"rt_call0_ptr",
     NY_NATIVE_RUNTIME_ABI_TAGGED_VALUES,
     NY_NATIVE_VM_I64_UNARY_TO_I64,
     {.i64_unary_i64 = rt_call0_ptr}},
    {"rt_call0_void",
     NY_NATIVE_RUNTIME_ABI_TAGGED_VALUES,
     NY_NATIVE_VM_I64_UNARY_TO_I64,
     {.i64_unary_i64 = rt_call0_void}},
    {"rt_call10_void",
     NY_NATIVE_RUNTIME_ABI_TAGGED_VALUES,
     NY_NATIVE_VM_I64_UNDECENARY_TO_I64,
     {.i64_undecenary_i64 = rt_call10_void}},
    {"rt_call1_f32_void",
     NY_NATIVE_RUNTIME_ABI_TAGGED_VALUES,
     NY_NATIVE_VM_I64_BINARY_TO_I64,
     {.i64_binary_i64 = rt_call1_f32_void}},
    {"rt_call1_i64",
     NY_NATIVE_RUNTIME_ABI_TAGGED_VALUES,
     NY_NATIVE_VM_I64_BINARY_TO_I64,
     {.i64_binary_i64 = rt_call1_i64}},
    {"rt_call1_ptr",
     NY_NATIVE_RUNTIME_ABI_TAGGED_VALUES,
     NY_NATIVE_VM_I64_BINARY_TO_I64,
     {.i64_binary_i64 = rt_call1_ptr}},
    {"rt_call1_u32",
     NY_NATIVE_RUNTIME_ABI_TAGGED_VALUES,
     NY_NATIVE_VM_I64_BINARY_TO_I64,
     {.i64_binary_i64 = rt_call1_u32}},
    {"rt_call1_u32_void",
     NY_NATIVE_RUNTIME_ABI_TAGGED_VALUES,
     NY_NATIVE_VM_I64_BINARY_TO_I64,
     {.i64_binary_i64 = rt_call1_u32_void}},
    {"rt_call1_void",
     NY_NATIVE_RUNTIME_ABI_TAGGED_VALUES,
     NY_NATIVE_VM_I64_BINARY_TO_I64,
     {.i64_binary_i64 = rt_call1_void}},
    {"rt_call2_f32_void",
     NY_NATIVE_RUNTIME_ABI_TAGGED_VALUES,
     NY_NATIVE_VM_I64_TERNARY_TO_I64,
     {.i64_ternary_i64 = rt_call2_f32_void}},
    {"rt_call2_ptr",
     NY_NATIVE_RUNTIME_ABI_TAGGED_VALUES,
     NY_NATIVE_VM_I64_TERNARY_TO_I64,
     {.i64_ternary_i64 = rt_call2_ptr}},
    {"rt_call2_ptr_u32",
     NY_NATIVE_RUNTIME_ABI_TAGGED_VALUES,
     NY_NATIVE_VM_I64_TERNARY_TO_I64,
     {.i64_ternary_i64 = rt_call2_ptr_u32}},
    {"rt_call2_void",
     NY_NATIVE_RUNTIME_ABI_TAGGED_VALUES,
     NY_NATIVE_VM_I64_TERNARY_TO_I64,
     {.i64_ternary_i64 = rt_call2_void}},
    {"rt_call3_f32_void",
     NY_NATIVE_RUNTIME_ABI_TAGGED_VALUES,
     NY_NATIVE_VM_I64_QUATERNARY_TO_I64,
     {.i64_quaternary_i64 = rt_call3_f32_void}},
    {"rt_call3_ptr",
     NY_NATIVE_RUNTIME_ABI_TAGGED_VALUES,
     NY_NATIVE_VM_I64_QUATERNARY_TO_I64,
     {.i64_quaternary_i64 = rt_call3_ptr}},
    {"rt_call3_ptr_ptr_u32",
     NY_NATIVE_RUNTIME_ABI_TAGGED_VALUES,
     NY_NATIVE_VM_I64_QUATERNARY_TO_I64,
     {.i64_quaternary_i64 = rt_call3_ptr_ptr_u32}},
    {"rt_call3_ptr_u32_ptr",
     NY_NATIVE_RUNTIME_ABI_TAGGED_VALUES,
     NY_NATIVE_VM_I64_QUATERNARY_TO_I64,
     {.i64_quaternary_i64 = rt_call3_ptr_u32_ptr}},
    {"rt_call3_ptr_u64_ptr",
     NY_NATIVE_RUNTIME_ABI_TAGGED_VALUES,
     NY_NATIVE_VM_I64_QUATERNARY_TO_I64,
     {.i64_quaternary_i64 = rt_call3_ptr_u64_ptr}},
    {"rt_call3_ptr_u64_ptr_i32",
     NY_NATIVE_RUNTIME_ABI_TAGGED_VALUES |
         NY_NATIVE_RUNTIME_ABI_RESULT_TAGGED_INT,
     NY_NATIVE_VM_I64_QUATERNARY_TO_I64,
     {.i64_quaternary_i64 = rt_call3_ptr_u64_ptr_i32}},
    {"rt_call3_void",
     NY_NATIVE_RUNTIME_ABI_TAGGED_VALUES,
     NY_NATIVE_VM_I64_QUATERNARY_TO_I64,
     {.i64_quaternary_i64 = rt_call3_void}},
    {"rt_call4_f32_void",
     NY_NATIVE_RUNTIME_ABI_TAGGED_VALUES,
     NY_NATIVE_VM_I64_QUINARY_TO_I64,
     {.i64_quinary_i64 = rt_call4_f32_void}},
    {"rt_call4_ptr",
     NY_NATIVE_RUNTIME_ABI_TAGGED_VALUES,
     NY_NATIVE_VM_I64_QUINARY_TO_I64,
     {.i64_quinary_i64 = rt_call4_ptr}},
    {"rt_call4_ptr_ptr_ptr_ptr_i32",
     NY_NATIVE_RUNTIME_ABI_TAGGED_VALUES |
         NY_NATIVE_RUNTIME_ABI_RESULT_TAGGED_INT,
     NY_NATIVE_VM_I64_QUINARY_TO_I64,
     {.i64_quinary_i64 = rt_call4_ptr_ptr_ptr_ptr_i32}},
    {"rt_call4_ptr_ptr_ptr_ptr_void",
     NY_NATIVE_RUNTIME_ABI_TAGGED_VALUES,
     NY_NATIVE_VM_I64_QUINARY_TO_I64,
     {.i64_quinary_i64 = rt_call4_ptr_ptr_ptr_ptr_void}},
    {"rt_call4_ptr_ptr_ptr_u64_i32",
     NY_NATIVE_RUNTIME_ABI_TAGGED_VALUES |
         NY_NATIVE_RUNTIME_ABI_RESULT_TAGGED_INT,
     NY_NATIVE_VM_I64_QUINARY_TO_I64,
     {.i64_quinary_i64 = rt_call4_ptr_ptr_ptr_u64_i32}},
    {"rt_call4_ptr_u32_u64_ptr_i32",
     NY_NATIVE_RUNTIME_ABI_TAGGED_VALUES |
         NY_NATIVE_RUNTIME_ABI_RESULT_TAGGED_INT,
     NY_NATIVE_VM_I64_QUINARY_TO_I64,
     {.i64_quinary_i64 = rt_call4_ptr_u32_u64_ptr_i32}},
    {"rt_call4_ptr_u64_ptr_ptr_i32",
     NY_NATIVE_RUNTIME_ABI_TAGGED_VALUES |
         NY_NATIVE_RUNTIME_ABI_RESULT_TAGGED_INT,
     NY_NATIVE_VM_I64_QUINARY_TO_I64,
     {.i64_quinary_i64 = rt_call4_ptr_u64_ptr_ptr_i32}},
    {"rt_call4_void",
     NY_NATIVE_RUNTIME_ABI_TAGGED_VALUES,
     NY_NATIVE_VM_I64_QUINARY_TO_I64,
     {.i64_quinary_i64 = rt_call4_void}},
    {"rt_call5_ptr",
     NY_NATIVE_RUNTIME_ABI_TAGGED_VALUES,
     NY_NATIVE_VM_I64_SENARY_TO_I64,
     {.i64_senary_i64 = rt_call5_ptr}},
    {"rt_call5_ptr_ptr_ptr_u64_i32_i32",
     NY_NATIVE_RUNTIME_ABI_TAGGED_VALUES |
         NY_NATIVE_RUNTIME_ABI_RESULT_TAGGED_INT,
     NY_NATIVE_VM_I64_SENARY_TO_I64,
     {.i64_senary_i64 = rt_call5_ptr_ptr_ptr_u64_i32_i32}},
    {"rt_call5_void",
     NY_NATIVE_RUNTIME_ABI_TAGGED_VALUES,
     NY_NATIVE_VM_I64_SENARY_TO_I64,
     {.i64_senary_i64 = rt_call5_void}},
    {"rt_call6_void",
     NY_NATIVE_RUNTIME_ABI_TAGGED_VALUES,
     NY_NATIVE_VM_I64_SEPTENARY_TO_I64,
     {.i64_septenary_i64 = rt_call6_void}},
    {"rt_call7_void",
     NY_NATIVE_RUNTIME_ABI_TAGGED_VALUES,
     NY_NATIVE_VM_I64_OCTARY_TO_I64,
     {.i64_octary_i64 = rt_call7_void}},
    {"rt_call8_void",
     NY_NATIVE_RUNTIME_ABI_TAGGED_VALUES,
     NY_NATIVE_VM_I64_NONARY_TO_I64,
     {.i64_nonary_i64 = rt_call8_void}},
    {"rt_call9_void",
     NY_NATIVE_RUNTIME_ABI_TAGGED_VALUES,
     NY_NATIVE_VM_I64_DECENARY_TO_I64,
     {.i64_decenary_i64 = rt_call9_void}},
    {"rt_canvas", 0, NY_NATIVE_VM_CALL_NONE, {0}},
    {"rt_canvas_clear", 0, NY_NATIVE_VM_CALL_NONE, {0}},
    {"rt_canvas_free", 0, NY_NATIVE_VM_CALL_NONE, {0}},
    {"rt_canvas_print", 0, NY_NATIVE_VM_CALL_NONE, {0}},
    {"rt_canvas_refresh", 0, NY_NATIVE_VM_CALL_NONE, {0}},
    {"rt_canvas_set", 0, NY_NATIVE_VM_CALL_NONE, {0}},
    {"rt_clear_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_clone_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_close_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_RAW_INT,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_color_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_complex_abs2",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_TAGGED_VALUE,
     NY_NATIVE_VM_I64_UNARY_TO_I64,
     {.i64_unary_i64 = rt_complex_abs2}},
    {"rt_complex_add",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_I64_BINARY_TO_I64,
     {.i64_binary_i64 = rt_complex_add}},
    {"rt_complex_conj",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_I64_UNARY_TO_I64,
     {.i64_unary_i64 = rt_complex_conj}},
    {"rt_complex_div",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_I64_BINARY_TO_I64,
     {.i64_binary_i64 = rt_complex_div}},
    {"rt_complex_eq",
     0,
     NY_NATIVE_VM_I64_BINARY_TO_I64,
     {.i64_binary_i64 = rt_complex_eq}},
    {"rt_complex_im_bits",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_RAW_WORD,
     NY_NATIVE_VM_I64_UNARY_TO_I64,
     {.i64_unary_i64 = rt_complex_im_bits}},
    {"rt_complex_im_f64",
     NY_NATIVE_RUNTIME_ABI_RESULT_F64,
     NY_NATIVE_VM_I64_UNARY_TO_F64,
     {.i64_unary_f64 = rt_complex_im_f64}},
    {"rt_complex_imag",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_TAGGED_VALUE,
     NY_NATIVE_VM_I64_UNARY_TO_I64,
     {.i64_unary_i64 = rt_complex_imag}},
    {"rt_complex_mul",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_I64_BINARY_TO_I64,
     {.i64_binary_i64 = rt_complex_mul}},
    {"rt_complex_new",
     NY_NATIVE_RUNTIME_ABI_TAGGED_ARG(0) | NY_NATIVE_RUNTIME_ABI_TAGGED_ARG(1),
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_complex_new_bits",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_I64_BINARY_TO_I64,
     {.i64_binary_i64 = rt_complex_new_bits}},
    {"rt_complex_re_bits",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_RAW_WORD,
     NY_NATIVE_VM_I64_UNARY_TO_I64,
     {.i64_unary_i64 = rt_complex_re_bits}},
    {"rt_complex_re_f64",
     NY_NATIVE_RUNTIME_ABI_RESULT_F64,
     NY_NATIVE_VM_I64_UNARY_TO_F64,
     {.i64_unary_f64 = rt_complex_re_f64}},
    {"rt_complex_real",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_TAGGED_VALUE,
     NY_NATIVE_VM_I64_UNARY_TO_I64,
     {.i64_unary_i64 = rt_complex_real}},
    {"rt_contains_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_copy_mem_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_cos_f64",
     NY_NATIVE_RUNTIME_ABI_RESULT_F64,
     NY_NATIVE_VM_F64_UNARY_TO_F64,
     {.f64_unary_f64 = rt_cos_f64}},
    {"rt_cstr_builder_append",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_I64_BINARY_TO_I64,
     {.i64_binary_i64 = rt_cstr_builder_append}},
    {"rt_cstr_builder_append_known",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_I64_TERNARY_TO_I64,
     {.i64_ternary_i64 = rt_cstr_builder_append_known}},
    {"rt_cstr_builder_finalize",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_I64_UNARY_TO_I64,
     {.i64_unary_i64 = rt_cstr_builder_finalize}},
    {"rt_cstr_builder_new",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_POINTER,
     NY_NATIVE_VM_I64_UNARY_TO_I64,
     {.i64_unary_i64 = rt_cstr_builder_new}},
    {"rt_cstr_builder_repeat_known",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_POINTER,
     NY_NATIVE_VM_I64_QUATERNARY_TO_I64,
     {.i64_quaternary_i64 = rt_cstr_builder_repeat_known}},
    {"rt_cstr_concat",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_CSTR,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_cstr_eq", 0, NY_NATIVE_VM_CALL_NONE, {0}},
    {"rt_cstr_get_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_cstr_index_read_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_cstr_len",
     NY_NATIVE_RUNTIME_ABI_RESULT_RAW_INT,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_cstr_replace",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_CSTR,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_cstr_to_str", 0, NY_NATIVE_VM_CALL_NONE, {0}},
    {"rt_dict_clear_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_dict_clone_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_dict_delete_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_dict_delete_str_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_dict_get_i64_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_dict_get_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_TAGGED_ARG(0) |
         NY_NATIVE_RUNTIME_ABI_TAGGED_ARG(2),
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_dict_get_str_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_TAGGED_ARG(2) |
         NY_NATIVE_RUNTIME_ABI_RESULT_TAGGED_VALUE,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_dict_has_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_dict_has_str_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_dict_items_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_TYPED_BUFFER,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_dict_keys_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_TYPED_BUFFER,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_dict_len_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_RAW_WORD,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_dict_merge_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_TAGGED_ARG(0) |
         NY_NATIVE_RUNTIME_ABI_TAGGED_ARG(1) |
         NY_NATIVE_RUNTIME_ABI_RESULT_TAGGED_VALUE,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_dict_new_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_dict_set_i64_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_dict_set_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_dict_set_str_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_dict_values_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_TYPED_BUFFER,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_dir_open",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_POINTER,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_envc_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_execve",
     NY_NATIVE_RUNTIME_ABI_RESULT_TAGGED_INT,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_exit", 0, NY_NATIVE_VM_CALL_NONE, {0}},
    {"rt_f64_bits",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_RAW_WORD,
     NY_NATIVE_VM_F64_UNARY_TO_I64,
     {.f64_unary_i64 = rt_f64_bits}},
    {"rt_f64_ceil",
     NY_NATIVE_RUNTIME_ABI_RESULT_F64,
     NY_NATIVE_VM_F64_UNARY_TO_F64,
     {.f64_unary_f64 = rt_f64_ceil}},
    {"rt_f64_floor",
     NY_NATIVE_RUNTIME_ABI_RESULT_F64,
     NY_NATIVE_VM_F64_UNARY_TO_F64,
     {.f64_unary_f64 = rt_f64_floor}},
    {"rt_f64_from_bits",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING | NY_NATIVE_RUNTIME_ABI_RESULT_F64,
     NY_NATIVE_VM_I64_UNARY_TO_F64,
     {.i64_unary_f64 = rt_f64_from_bits}},
    {"rt_f64_max",
     NY_NATIVE_RUNTIME_ABI_RESULT_F64,
     NY_NATIVE_VM_F64_BINARY_TO_F64,
     {.f64_binary_f64 = rt_f64_max}},
    {"rt_f64_min",
     NY_NATIVE_RUNTIME_ABI_RESULT_F64,
     NY_NATIVE_VM_F64_BINARY_TO_F64,
     {.f64_binary_f64 = rt_f64_min}},
    {"rt_f64_pow",
     NY_NATIVE_RUNTIME_ABI_RESULT_F64,
     NY_NATIVE_VM_F64_BINARY_TO_F64,
     {.f64_binary_f64 = rt_f64_pow}},
    {"rt_f64_round",
     NY_NATIVE_RUNTIME_ABI_RESULT_F64,
     NY_NATIVE_VM_F64_UNARY_TO_F64,
     {.f64_unary_f64 = rt_f64_round}},
    {"rt_f64_to_cstr_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_CSTR,
     NY_NATIVE_VM_F64_UNARY_TO_I64,
     {.f64_unary_i64 = rt_f64_to_cstr_raw}},
    {"rt_f64_to_i64",
     NY_NATIVE_RUNTIME_ABI_RESULT_RAW_INT,
     NY_NATIVE_VM_F64_UNARY_TO_I64,
     {.f64_unary_i64 = rt_f64_to_i64}},
    {"rt_f64_to_json_str",
     NY_NATIVE_RUNTIME_ABI_TAGGED_ARG(0) | NY_NATIVE_RUNTIME_ABI_RESULT_CSTR,
     NY_NATIVE_VM_I64_UNARY_TO_I64,
     {.i64_unary_i64 = rt_f64_to_json_str}},
    {"rt_ffi_call0",
     NY_NATIVE_RUNTIME_ABI_TAGGED_VALUES,
     NY_NATIVE_VM_I64_UNARY_TO_I64,
     {.i64_unary_i64 = rt_ffi_call0}},
    {"rt_ffi_call1",
     NY_NATIVE_RUNTIME_ABI_TAGGED_VALUES,
     NY_NATIVE_VM_I64_BINARY_TO_I64,
     {.i64_binary_i64 = rt_ffi_call1}},
    {"rt_file_exists",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_RAW_INT,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_file_exists_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_RAW_INT,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_flt_box_val",
     NY_NATIVE_RUNTIME_ABI_RESULT_TAGGED_VALUE,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_flt_unbox_double",
     NY_NATIVE_RUNTIME_ABI_RESULT_F64,
     NY_NATIVE_VM_I64_UNARY_TO_F64,
     {.i64_unary_f64 = rt_flt_unbox_double}},
    {"rt_fmod_f64",
     NY_NATIVE_RUNTIME_ABI_RESULT_F64,
     NY_NATIVE_VM_F64_BINARY_TO_F64,
     {.f64_binary_f64 = rt_fmod_f64}},
    {"rt_free", 0, NY_NATIVE_VM_CALL_NONE, {0}},
    {"rt_free_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_gc_stat_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_gc_tag_bytes_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_globals_get", 0, NY_NATIVE_VM_CALL_NONE, {0}},
    {"rt_globals_set", 0, NY_NATIVE_VM_CALL_NONE, {0}},
    {"rt_i64_max",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_RAW_INT,
     NY_NATIVE_VM_I64_BINARY_TO_I64,
     {.i64_binary_i64 = rt_i64_max}},
    {"rt_i64_to_cstr_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_CSTR,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_index_key_error", 0, NY_NATIVE_VM_CALL_NONE, {0}},
    {"rt_init_str", 0, NY_NATIVE_VM_CALL_NONE, {0}},
    {"rt_init_str_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_is_dir",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_RAW_INT,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_is_float_obj", 0, NY_NATIVE_VM_CALL_NONE, {0}},
    {"rt_len",
     NY_NATIVE_RUNTIME_ABI_RESULT_RAW_INT,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_len_strict",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_RAW_INT,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_list_new_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_TYPED_BUFFER,
     NY_NATIVE_VM_I64_UNARY_TO_I64,
     {.i64_unary_i64 = rt_list_new_raw}},
    {"rt_list_new_sized",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_TYPED_BUFFER,
     NY_NATIVE_VM_I64_UNARY_TO_I64,
     {.i64_unary_i64 = rt_list_new_sized}},
    {"rt_list_new_sized_uninit",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_TYPED_BUFFER,
     NY_NATIVE_VM_I64_UNARY_TO_I64,
     {.i64_unary_i64 = rt_list_new_sized_uninit}},
    {"rt_list_set_len_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_load32_f64",
     NY_NATIVE_RUNTIME_ABI_RESULT_F64,
     NY_NATIVE_VM_I64_BINARY_TO_F64,
     {.i64_binary_f64 = rt_load32_f64}},
    {"rt_load_item", 0, NY_NATIVE_VM_CALL_NONE, {0}},
    {"rt_load_item_any", 0, NY_NATIVE_VM_CALL_NONE, {0}},
    {"rt_long",
     NY_NATIVE_RUNTIME_ABI_VALUE_CONVERSION,
     NY_NATIVE_VM_I64_UNARY_TO_I64,
     {.i64_unary_i64 = rt_long}},
    {"rt_long_list_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_malloc",
     NY_NATIVE_RUNTIME_ABI_RESULT_POINTER,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_malloc_i64",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_POINTER,
     NY_NATIVE_VM_I64_UNARY_TO_I64,
     {.i64_unary_i64 = rt_malloc_i64}},
    {"rt_malloc_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_POINTER,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_malloc_raw_i64",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_POINTER,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_memcmp_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_memcpy_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_memset_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_monotonic_ns_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_RAW_WORD |
         NY_NATIVE_RUNTIME_ABI_RESULT_RAW_INT,
     NY_NATIVE_VM_I64_NULLARY_TO_I64,
     {.i64_nullary_i64 = rt_monotonic_ns_raw}},
    {"rt_mutex_free", 0, NY_NATIVE_VM_CALL_NONE, {0}},
    {"rt_mutex_lock64",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_mutex_new", 0, NY_NATIVE_VM_CALL_NONE, {0}},
    {"rt_mutex_unlock64",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_native_dict_set_nir_i64",
     0,
     NY_NATIVE_VM_I64_TERNARY_TO_I64,
     {.i64_ternary_i64 = rt_native_dict_set_nir_i64}},
    {"rt_native_dict_set_nir_i64_raw_int",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_I64_TERNARY_TO_I64,
     {.i64_ternary_i64 = rt_native_dict_set_nir_i64_raw_int}},
    {"rt_native_dict_set_nir_i64_raw_int_key",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_I64_TERNARY_TO_I64,
     {.i64_ternary_i64 = rt_native_dict_set_nir_i64_raw_int_key}},
    {"rt_native_dict_set_str_compact",
     0,
     NY_NATIVE_VM_I64_TERNARY_TO_I64,
     {.i64_ternary_i64 = rt_native_dict_set_str_compact}},
    {"rt_native_has_tag",
     0,
     NY_NATIVE_VM_I64_BINARY_TO_I64,
     {.i64_binary_i64 = rt_native_has_tag}},
    {"rt_native_is_int",
     0,
     NY_NATIVE_VM_I64_UNARY_TO_I64,
     {.i64_unary_i64 = rt_native_is_int}},
    {"rt_open_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_RAW_WORD |
         NY_NATIVE_RUNTIME_ABI_RESULT_RAW_INT,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_os_name",
     NY_NATIVE_RUNTIME_ABI_RESULT_TAGGED_VALUE,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_print_cstr", 0, NY_NATIVE_VM_CALL_NONE, {0}},
    {"rt_print_f64_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_F64_UNARY_TO_I64,
     {.f64_unary_i64 = rt_print_f64_raw}},
    {"rt_print_flush_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_print_i64_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_print_str_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_proof_cert_check",
     NY_NATIVE_RUNTIME_ABI_TAGGED_VALUES,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_proof_cert_digest",
     NY_NATIVE_RUNTIME_ABI_TAGGED_VALUES |
         NY_NATIVE_RUNTIME_ABI_RESULT_TAGGED_INT,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_proof_policy_check",
     NY_NATIVE_RUNTIME_ABI_TAGGED_VALUES,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_proof_policy_digest_hex",
     NY_NATIVE_RUNTIME_ABI_TAGGED_VALUES |
         NY_NATIVE_RUNTIME_ABI_RESULT_TAGGED_VALUE,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_proof_policy_solve",
     NY_NATIVE_RUNTIME_ABI_TAGGED_VALUES |
         NY_NATIVE_RUNTIME_ABI_RESULT_TAGGED_VALUE,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_ptr_add_i64",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_POINTER,
     NY_NATIVE_VM_I64_BINARY_TO_I64,
     {.i64_binary_i64 = rt_ptr_add_i64}},
    {"rt_ptr_sub_i64",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_POINTER,
     NY_NATIVE_VM_I64_BINARY_TO_I64,
     {.i64_binary_i64 = rt_ptr_sub_i64}},
    {"rt_range_index_read_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_range_len_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_RAW_INT,
     NY_NATIVE_VM_I64_UNARY_TO_I64,
     {.i64_unary_i64 = rt_range_len_raw}},
    {"rt_range_new_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_range_values_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_read_fd_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_RAW_WORD |
         NY_NATIVE_RUNTIME_ABI_RESULT_RAW_INT,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_read_off_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_RAW_WORD |
         NY_NATIVE_RUNTIME_ABI_RESULT_RAW_INT,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_realloc",
     NY_NATIVE_RUNTIME_ABI_RESULT_POINTER,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_realloc_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_POINTER,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_request_native_oracle_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_request_tv_checks_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_result_err", 0, NY_NATIVE_VM_CALL_NONE, {0}},
    {"rt_result_ok", 0, NY_NATIVE_VM_CALL_NONE, {0}},
    {"rt_result_unwrap_or_any",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_TAGGED_VALUE,
     NY_NATIVE_VM_I64_BINARY_TO_I64,
     {.i64_binary_i64 = rt_result_unwrap_or_any}},
    {"rt_result_unwrap_or_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_I64_BINARY_TO_I64,
     {.i64_binary_i64 = rt_result_unwrap_or_raw}},
    {"rt_result_unwrap_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_runtime_alloc_count_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_runtime_realloc_count_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_runtime_tag_raw_value",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_RAW_INT,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_sequence_len_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_I64_UNARY_TO_I64,
     {.i64_unary_i64 = rt_sequence_len_raw}},
    {"rt_set_add_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_TYPED_BUFFER,
     NY_NATIVE_VM_I64_BINARY_TO_I64,
     {.i64_binary_i64 = rt_set_add_raw}},
    {"rt_set_from_sequence",
     0,
     NY_NATIVE_VM_I64_UNARY_TO_I64,
     {.i64_unary_i64 = rt_set_from_sequence}},
    {"rt_set_len_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_RAW_WORD,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_set_new",
     0,
     NY_NATIVE_VM_I64_UNARY_TO_I64,
     {.i64_unary_i64 = rt_set_new}},
    {"rt_set_remove",
     0,
     NY_NATIVE_VM_I64_BINARY_TO_I64,
     {.i64_binary_i64 = rt_set_remove}},
    {"rt_set_remove_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_TYPED_BUFFER,
     NY_NATIVE_VM_I64_BINARY_TO_I64,
     {.i64_binary_i64 = rt_set_remove_raw}},
    {"rt_shl_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_RAW_WORD,
     NY_NATIVE_VM_I64_BINARY_TO_I64,
     {.i64_binary_i64 = rt_shl_raw}},
    {"rt_shr_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_RAW_WORD,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_simmd_bswap32_i64",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_RAW_INT,
     NY_NATIVE_VM_I64_UNARY_TO_I64,
     {.i64_unary_i64 = rt_simmd_bswap32_i64}},
    {"rt_simmd_bswap64_i64",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_RAW_INT,
     NY_NATIVE_VM_I64_UNARY_TO_I64,
     {.i64_unary_i64 = rt_simmd_bswap64_i64}},
    {"rt_simmd_byte_class_reduce_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_RAW_INT,
     NY_NATIVE_VM_I64_SEPTENARY_TO_I64,
     {.i64_septenary_i64 = rt_simmd_byte_class_reduce_raw}},
    {"rt_simmd_clz32_i64",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_RAW_INT,
     NY_NATIVE_VM_I64_UNARY_TO_I64,
     {.i64_unary_i64 = rt_simmd_clz32_i64}},
    {"rt_simmd_clz64_i64",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_RAW_INT,
     NY_NATIVE_VM_I64_UNARY_TO_I64,
     {.i64_unary_i64 = rt_simmd_clz64_i64}},
    {"rt_simmd_ctz32_i64",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_RAW_INT,
     NY_NATIVE_VM_I64_UNARY_TO_I64,
     {.i64_unary_i64 = rt_simmd_ctz32_i64}},
    {"rt_simmd_ctz64_i64",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_RAW_INT,
     NY_NATIVE_VM_I64_UNARY_TO_I64,
     {.i64_unary_i64 = rt_simmd_ctz64_i64}},
    {"rt_simmd_i32_sqlscan_sum_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_RAW_INT,
     NY_NATIVE_VM_I64_SENARY_TO_I64,
     {.i64_senary_i64 = rt_simmd_i32_sqlscan_sum_raw}},
    {"rt_simmd_popcnt32_i64",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_RAW_INT,
     NY_NATIVE_VM_I64_UNARY_TO_I64,
     {.i64_unary_i64 = rt_simmd_popcnt32_i64}},
    {"rt_simmd_popcnt64_i64",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_RAW_INT,
     NY_NATIVE_VM_I64_UNARY_TO_I64,
     {.i64_unary_i64 = rt_simmd_popcnt64_i64}},
    {"rt_simmd_rotl32_i64",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_RAW_INT,
     NY_NATIVE_VM_I64_BINARY_TO_I64,
     {.i64_binary_i64 = rt_simmd_rotl32_i64}},
    {"rt_simmd_rotl64_i64",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_RAW_INT,
     NY_NATIVE_VM_I64_BINARY_TO_I64,
     {.i64_binary_i64 = rt_simmd_rotl64_i64}},
    {"rt_simmd_rotr32_i64",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_RAW_INT,
     NY_NATIVE_VM_I64_BINARY_TO_I64,
     {.i64_binary_i64 = rt_simmd_rotr32_i64}},
    {"rt_simmd_rotr64_i64",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_RAW_INT,
     NY_NATIVE_VM_I64_BINARY_TO_I64,
     {.i64_binary_i64 = rt_simmd_rotr64_i64}},
    {"rt_sin_f64",
     NY_NATIVE_RUNTIME_ABI_RESULT_F64,
     NY_NATIVE_VM_F64_UNARY_TO_F64,
     {.f64_unary_f64 = rt_sin_f64}},
    {"rt_sound_backend_name",
     NY_NATIVE_RUNTIME_ABI_SOURCE_PREFERRED,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_sound_init",
     NY_NATIVE_RUNTIME_ABI_SOURCE_PREFERRED,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_sound_shutdown",
     NY_NATIVE_RUNTIME_ABI_SOURCE_PREFERRED,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_sound_web_play",
     NY_NATIVE_RUNTIME_ABI_SOURCE_PREFERRED,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_spawn_pipe",
     NY_NATIVE_RUNTIME_ABI_TAGGED_VALUES | NY_NATIVE_RUNTIME_ABI_RESULT_RAW_INT,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_sqrt_f64",
     NY_NATIVE_RUNTIME_ABI_RESULT_F64,
     NY_NATIVE_VM_F64_UNARY_TO_F64,
     {.f64_unary_f64 = rt_sqrt_f64}},
    {"rt_store32_f64",
     0,
     NY_NATIVE_VM_I64_I64_F64_TO_I64,
     {.i64_i64_f64_i64 = rt_store32_f64}},
    {"rt_store_item", 0, NY_NATIVE_VM_CALL_NONE, {0}},
    {"rt_store_item_fast", 0, NY_NATIVE_VM_CALL_NONE, {0}},
    {"rt_store_item_fast_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_str_builder_append",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_TAGGED_ARG(1) |
         NY_NATIVE_RUNTIME_ABI_RESULT_POINTER,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_str_builder_append_byte",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_POINTER,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_str_builder_free",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_RAW_WORD,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_str_builder_new",
     NY_NATIVE_RUNTIME_ABI_RESULT_POINTER,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_str_builder_to_str",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_str_byte", 0, NY_NATIVE_VM_CALL_NONE, {0}},
    {"rt_syscall",
     NY_NATIVE_RUNTIME_ABI_TAGGED_VALUES,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_syscall_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_RAW_INT |
         NY_NATIVE_RUNTIME_ABI_TAGGED_ARG(1) |
         NY_NATIVE_RUNTIME_ABI_TAGGED_ARG(2) |
         NY_NATIVE_RUNTIME_ABI_TAGGED_ARG(3) |
         NY_NATIVE_RUNTIME_ABI_TAGGED_ARG(4) |
         NY_NATIVE_RUNTIME_ABI_TAGGED_ARG(5) |
         NY_NATIVE_RUNTIME_ABI_TAGGED_ARG(6),
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_tag",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_TAGGED_VALUE,
     NY_NATIVE_VM_I64_UNARY_TO_I64,
     {.i64_unary_i64 = rt_tag}},
    {"rt_tag_or_raw_int", 0, NY_NATIVE_VM_CALL_NONE, {0}},
    {"rt_tbuf_append_i64_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_TYPED_BUFFER,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_tbuf_append_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_TYPED_BUFFER,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_tbuf_append_tagged",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_TYPED_BUFFER,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_tbuf_clear_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_TYPED_BUFFER,
     NY_NATIVE_VM_I64_UNARY_TO_I64,
     {.i64_unary_i64 = rt_tbuf_clear_raw}},
    {"rt_tbuf_clone_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_TYPED_BUFFER,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_tbuf_concat_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_TYPED_BUFFER,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_tbuf_extend",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_TYPED_BUFFER,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_tbuf_get_any",
     0,
     NY_NATIVE_VM_I64_TERNARY_TO_I64,
     {.i64_ternary_i64 = rt_tbuf_get_any}},
    {"rt_tbuf_index_any",
     0,
     NY_NATIVE_VM_I64_BINARY_TO_I64,
     {.i64_binary_i64 = rt_tbuf_index_any}},
    {"rt_tbuf_index_any_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_TAGGED_VALUE,
     NY_NATIVE_VM_I64_BINARY_TO_I64,
     {.i64_binary_i64 = rt_tbuf_index_any_raw}},
    {"rt_tbuf_index_f64",
     NY_NATIVE_RUNTIME_ABI_RESULT_F64,
     NY_NATIVE_VM_I64_BINARY_TO_F64,
     {.i64_binary_f64 = rt_tbuf_index_f64}},
    {"rt_tbuf_index_read_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_tbuf_len_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_RAW_INT,
     NY_NATIVE_VM_I64_UNARY_TO_I64,
     {.i64_unary_i64 = rt_tbuf_len_raw}},
    {"rt_tbuf_new_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_TYPED_BUFFER,
     NY_NATIVE_VM_I64_BINARY_TO_I64,
     {.i64_binary_i64 = rt_tbuf_new_raw}},
    {"rt_tbuf_new_typed_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_TYPED_BUFFER,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_tbuf_pop_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_tbuf_reserve",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_TYPED_BUFFER,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_tbuf_set_f64_bits",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_TYPED_BUFFER,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_tbuf_set_i64_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_TYPED_BUFFER,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_tbuf_set_tagged",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_TYPED_BUFFER,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_tbuf_swap",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_TYPED_BUFFER,
     NY_NATIVE_VM_I64_TERNARY_TO_I64,
     {.i64_ternary_i64 = rt_tbuf_swap}},
    {"rt_tbuf_to_cstr",
     0,
     NY_NATIVE_VM_I64_UNARY_TO_I64,
     {.i64_unary_i64 = rt_tbuf_to_cstr}},
    {"rt_terminal_size_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_thread_join", 0, NY_NATIVE_VM_CALL_NONE, {0}},
    {"rt_thread_launch_call", 0, NY_NATIVE_VM_CALL_NONE, {0}},
    {"rt_thread_spawn_call", 0, NY_NATIVE_VM_CALL_NONE, {0}},
    {"rt_thread_spawn_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_ticks_ns",
     0,
     NY_NATIVE_VM_I64_NULLARY_TO_I64,
     {.i64_nullary_i64 = rt_ticks_ns}},
    {"rt_time_milliseconds",
     NY_NATIVE_RUNTIME_ABI_RESULT_TAGGED_INT,
     NY_NATIVE_VM_I64_NULLARY_TO_I64,
     {.i64_nullary_i64 = rt_time_milliseconds}},
    {"rt_time_seconds",
     NY_NATIVE_RUNTIME_ABI_RESULT_TAGGED_INT,
     NY_NATIVE_VM_I64_NULLARY_TO_I64,
     {.i64_nullary_i64 = rt_time_seconds}},
    {"rt_to_str",
     0,
     NY_NATIVE_VM_I64_UNARY_TO_I64,
     {.i64_unary_i64 = rt_to_str}},
    {"rt_trace_allocations_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_trace_arg_f64",
     0,
     NY_NATIVE_VM_F64_UNARY_TO_I64,
     {.f64_unary_i64 = rt_trace_arg_f64}},
    {"rt_trace_arg_f64_bits", 0, NY_NATIVE_VM_CALL_NONE, {0}},
    {"rt_trace_arg_i64", 0, NY_NATIVE_VM_CALL_NONE, {0}},
    {"rt_trace_arg_tagged", 0, NY_NATIVE_VM_CALL_NONE, {0}},
    {"rt_trace_count_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_trace_cpu_ns_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_trace_dump_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_trace_enter_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_trace_exclusive_cpu_ns_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_trace_exclusive_ns_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_trace_exit_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_trace_func_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_trace_inclusive_ns_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_trace_loc_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_trace_reset_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_trace_ret_i64_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_RAW_INT,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_trace_ret_u64_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_RAW_INT,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_trace_ret_void_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_trace_sampled_rss_delta_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_trace_sampled_rss_peak_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_tty_isatty_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_tty_pending_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_tty_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_tty_raw_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_type_name_tagged", 0, NY_NATIVE_VM_CALL_NONE, {0}},
    {"rt_untag",
     NY_NATIVE_RUNTIME_ABI_RESULT_RAW_INT,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_unwrap",
     NY_NATIVE_RUNTIME_ABI_RESULT_TAGGED_VALUE,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_value_get_index_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_I64_TERNARY_TO_I64,
     {.i64_ternary_i64 = rt_value_get_index_raw}},
    {"rt_value_is_ptr", 0, NY_NATIVE_VM_CALL_NONE, {0}},
    {"rt_value_tag",
     0,
     NY_NATIVE_VM_I64_UNARY_TO_I64,
     {.i64_unary_i64 = rt_value_tag}},
    {"rt_value_to_f64",
     NY_NATIVE_RUNTIME_ABI_RESULT_F64,
     NY_NATIVE_VM_I64_BINARY_TO_F64,
     {.i64_binary_f64 = rt_value_to_f64}},
    {"rt_vec_add_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_vec_div_component_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_vec_div_scalar_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_vec_dot_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING | NY_NATIVE_RUNTIME_ABI_RESULT_F64,
     NY_NATIVE_VM_I64_BINARY_TO_F64,
     {.i64_binary_f64 = rt_vec_dot_raw}},
    {"rt_vec_mark_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_vec_mul_scalar_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_I64_F64_TO_I64,
     {.i64_f64_i64 = rt_vec_mul_scalar_raw}},
    {"rt_vec_sub_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_wait4",
     NY_NATIVE_RUNTIME_ABI_TAGGED_VALUES |
         NY_NATIVE_RUNTIME_ABI_RESULT_TAGGED_INT,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_wait_process",
     NY_NATIVE_RUNTIME_ABI_TAGGED_VALUES |
         NY_NATIVE_RUNTIME_ABI_RESULT_TAGGED_INT,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_web_test_report_gamepad",
     NY_NATIVE_RUNTIME_ABI_SOURCE_PREFERRED,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_web_test_report_touch",
     NY_NATIVE_RUNTIME_ABI_SOURCE_PREFERRED,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_write_buffered_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_write_fd_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_RAW_WORD |
         NY_NATIVE_RUNTIME_ABI_RESULT_RAW_INT,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_write_off",
     NY_NATIVE_RUNTIME_ABI_TAGGED_VALUES |
         NY_NATIVE_RUNTIME_ABI_RESULT_TAGGED_INT,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_write_off_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_RAW_WORD |
         NY_NATIVE_RUNTIME_ABI_RESULT_RAW_INT,
     NY_NATIVE_VM_CALL_NONE,
     {0}},
    {"rt_zalloc_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING |
         NY_NATIVE_RUNTIME_ABI_RESULT_POINTER,
     NY_NATIVE_VM_I64_UNARY_TO_I64,
     {.i64_unary_i64 = rt_zalloc_raw}},
    {"rt_zfree_raw",
     NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING,
     NY_NATIVE_VM_I64_UNARY_TO_I64,
     {.i64_unary_i64 = rt_zfree_raw}},
};
const size_t ny_native_runtime_abi_decl_count =
    sizeof(ny_native_runtime_abi_decls) /
    sizeof(ny_native_runtime_abi_decls[0]);

/*
 * These dictionary setters intentionally bridge raw native slots and
 * tagged managed values. Descriptors encode the receiver-specific argument
 * unions: raw integer keys are valid for native dictionaries, while managed
 * integer keys must already be tagged; raw C-string keys are normalized to
 * managed keys by the receiver path. Raw item integers are stored directly
 * in native dictionaries and tagged by the managed bridge. All inputs are
 * borrowed; the result aliases the receiver without transferring ownership.
 * The C call shape alone cannot express these representation unions.
 */
const ny_native_runtime_value_abi_decl_t ny_native_runtime_value_abi_decls[] = {
    {"rt_native_dict_set_nir_i64",
     3,
     {{NY_NATIVE_ABI_VALUE_OBJECT_HANDLE,
       NY_NATIVE_ABI_VALUE_RAW_I64 | NY_NATIVE_ABI_VALUE_TAGGED |
           NY_NATIVE_ABI_VALUE_OBJECT_HANDLE | NY_NATIVE_ABI_VALUE_RAW_CSTR,
       NY_NATIVE_ABI_VALUE_RAW_I64 | NY_NATIVE_ABI_VALUE_TAGGED |
           NY_NATIVE_ABI_VALUE_OBJECT_HANDLE},
      {NY_NATIVE_ABI_VALUE_OBJECT_HANDLE,
       NY_NATIVE_ABI_VALUE_TAGGED | NY_NATIVE_ABI_VALUE_OBJECT_HANDLE |
           NY_NATIVE_ABI_VALUE_RAW_CSTR,
       NY_NATIVE_ABI_VALUE_RAW_I64 | NY_NATIVE_ABI_VALUE_TAGGED |
           NY_NATIVE_ABI_VALUE_OBJECT_HANDLE}},
     {NY_NATIVE_ABI_BORROWED, NY_NATIVE_ABI_BORROWED, NY_NATIVE_ABI_BORROWED},
     NY_NATIVE_ABI_VALUE_ALIAS_ARG0,
     NY_NATIVE_ABI_BORROWED_ALIAS},
    {"rt_native_dict_set_str_compact",
     3,
     {{NY_NATIVE_ABI_VALUE_OBJECT_HANDLE,
       NY_NATIVE_ABI_VALUE_RAW_I64 | NY_NATIVE_ABI_VALUE_TAGGED |
           NY_NATIVE_ABI_VALUE_OBJECT_HANDLE | NY_NATIVE_ABI_VALUE_RAW_CSTR,
       NY_NATIVE_ABI_VALUE_RAW_I64 | NY_NATIVE_ABI_VALUE_TAGGED |
           NY_NATIVE_ABI_VALUE_OBJECT_HANDLE},
      {NY_NATIVE_ABI_VALUE_OBJECT_HANDLE,
       NY_NATIVE_ABI_VALUE_TAGGED | NY_NATIVE_ABI_VALUE_OBJECT_HANDLE |
           NY_NATIVE_ABI_VALUE_RAW_CSTR,
       NY_NATIVE_ABI_VALUE_RAW_I64 | NY_NATIVE_ABI_VALUE_TAGGED |
           NY_NATIVE_ABI_VALUE_OBJECT_HANDLE}},
     {NY_NATIVE_ABI_BORROWED, NY_NATIVE_ABI_BORROWED, NY_NATIVE_ABI_BORROWED},
     NY_NATIVE_ABI_VALUE_ALIAS_ARG0,
     NY_NATIVE_ABI_BORROWED_ALIAS},
};
const size_t ny_native_runtime_value_abi_decl_count =
    sizeof(ny_native_runtime_value_abi_decls) /
    sizeof(ny_native_runtime_value_abi_decls[0]);

#define NY_NATIVE_CALLBACK_CALLABLE_REPRS                                      \
  (NY_NATIVE_ABI_VALUE_RAW_I64 | NY_NATIVE_ABI_VALUE_TAGGED |                  \
   NY_NATIVE_ABI_VALUE_OBJECT_HANDLE | NY_NATIVE_ABI_VALUE_CALLBACK_MARKED)
#define NY_NATIVE_CALLBACK_VALUE_REPRS                                         \
  (NY_NATIVE_ABI_VALUE_RAW_I64 | NY_NATIVE_ABI_VALUE_TAGGED |                  \
   NY_NATIVE_ABI_VALUE_OBJECT_HANDLE | NY_NATIVE_ABI_VALUE_RAW_CSTR)
#define NY_NATIVE_CALLBACK_DISPATCH_REPRS                                      \
  (NY_NATIVE_CALLBACK_VALUE_REPRS | NY_NATIVE_ABI_VALUE_CALLBACK_MARKED)
const ny_native_runtime_callback_abi_decl_t
    ny_native_runtime_callback_abi_decls[] = {
        {"rt_call0",
         0,
         NY_NATIVE_CALLBACK_CALLABLE_REPRS,
         0,
         NY_NATIVE_ABI_OWNERSHIP_UNSPECIFIED,
         NY_NATIVE_CALLBACK_DISPATCH_REPRS,
         NY_NATIVE_ABI_BORROWED,
         NY_NATIVE_ABI_BORROWED,
         NY_NATIVE_ABI_RESULT_ABI,
         true,
         {.call0 = rt_call0}},
        {"rt_call_any1",
         1,
         NY_NATIVE_CALLBACK_CALLABLE_REPRS,
         NY_NATIVE_CALLBACK_DISPATCH_REPRS,
         NY_NATIVE_ABI_BORROWED,
         NY_NATIVE_CALLBACK_DISPATCH_REPRS,
         NY_NATIVE_ABI_BORROWED,
         NY_NATIVE_ABI_BORROWED,
         NY_NATIVE_ABI_RESULT_ABI,
         false,
         {.call1 = rt_call_any1}},
        {"rt_call_any10",
         10,
         NY_NATIVE_CALLBACK_CALLABLE_REPRS,
         NY_NATIVE_CALLBACK_DISPATCH_REPRS,
         NY_NATIVE_ABI_BORROWED,
         NY_NATIVE_CALLBACK_DISPATCH_REPRS,
         NY_NATIVE_ABI_BORROWED,
         NY_NATIVE_ABI_BORROWED,
         NY_NATIVE_ABI_RESULT_ABI,
         false,
         {.call10 = rt_call_any10}},
        {"rt_call_any11",
         11,
         NY_NATIVE_CALLBACK_CALLABLE_REPRS,
         NY_NATIVE_CALLBACK_DISPATCH_REPRS,
         NY_NATIVE_ABI_BORROWED,
         NY_NATIVE_CALLBACK_DISPATCH_REPRS,
         NY_NATIVE_ABI_BORROWED,
         NY_NATIVE_ABI_BORROWED,
         NY_NATIVE_ABI_RESULT_ABI,
         false,
         {.call11 = rt_call_any11}},
        {"rt_call_any12",
         12,
         NY_NATIVE_CALLBACK_CALLABLE_REPRS,
         NY_NATIVE_CALLBACK_DISPATCH_REPRS,
         NY_NATIVE_ABI_BORROWED,
         NY_NATIVE_CALLBACK_DISPATCH_REPRS,
         NY_NATIVE_ABI_BORROWED,
         NY_NATIVE_ABI_BORROWED,
         NY_NATIVE_ABI_RESULT_ABI,
         false,
         {.call12 = rt_call_any12}},
        {"rt_call_any13",
         13,
         NY_NATIVE_CALLBACK_CALLABLE_REPRS,
         NY_NATIVE_CALLBACK_DISPATCH_REPRS,
         NY_NATIVE_ABI_BORROWED,
         NY_NATIVE_CALLBACK_DISPATCH_REPRS,
         NY_NATIVE_ABI_BORROWED,
         NY_NATIVE_ABI_BORROWED,
         NY_NATIVE_ABI_RESULT_ABI,
         false,
         {.call13 = rt_call_any13}},
        {"rt_call_any14",
         14,
         NY_NATIVE_CALLBACK_CALLABLE_REPRS,
         NY_NATIVE_CALLBACK_DISPATCH_REPRS,
         NY_NATIVE_ABI_BORROWED,
         NY_NATIVE_CALLBACK_DISPATCH_REPRS,
         NY_NATIVE_ABI_BORROWED,
         NY_NATIVE_ABI_BORROWED,
         NY_NATIVE_ABI_RESULT_ABI,
         false,
         {.call14 = rt_call_any14}},
        {"rt_call_any15",
         15,
         NY_NATIVE_CALLBACK_CALLABLE_REPRS,
         NY_NATIVE_CALLBACK_DISPATCH_REPRS,
         NY_NATIVE_ABI_BORROWED,
         NY_NATIVE_CALLBACK_DISPATCH_REPRS,
         NY_NATIVE_ABI_BORROWED,
         NY_NATIVE_ABI_BORROWED,
         NY_NATIVE_ABI_RESULT_ABI,
         false,
         {.call15 = rt_call_any15}},
        {"rt_call_any2",
         2,
         NY_NATIVE_CALLBACK_CALLABLE_REPRS,
         NY_NATIVE_CALLBACK_DISPATCH_REPRS,
         NY_NATIVE_ABI_BORROWED,
         NY_NATIVE_CALLBACK_DISPATCH_REPRS,
         NY_NATIVE_ABI_BORROWED,
         NY_NATIVE_ABI_BORROWED,
         NY_NATIVE_ABI_RESULT_ABI,
         false,
         {.call2 = rt_call_any2}},
        {"rt_call_any3",
         3,
         NY_NATIVE_CALLBACK_CALLABLE_REPRS,
         NY_NATIVE_CALLBACK_DISPATCH_REPRS,
         NY_NATIVE_ABI_BORROWED,
         NY_NATIVE_CALLBACK_DISPATCH_REPRS,
         NY_NATIVE_ABI_BORROWED,
         NY_NATIVE_ABI_BORROWED,
         NY_NATIVE_ABI_RESULT_ABI,
         false,
         {.call3 = rt_call_any3}},
        {"rt_call_any4",
         4,
         NY_NATIVE_CALLBACK_CALLABLE_REPRS,
         NY_NATIVE_CALLBACK_DISPATCH_REPRS,
         NY_NATIVE_ABI_BORROWED,
         NY_NATIVE_CALLBACK_DISPATCH_REPRS,
         NY_NATIVE_ABI_BORROWED,
         NY_NATIVE_ABI_BORROWED,
         NY_NATIVE_ABI_RESULT_ABI,
         false,
         {.call4 = rt_call_any4}},
        {"rt_call_any5",
         5,
         NY_NATIVE_CALLBACK_CALLABLE_REPRS,
         NY_NATIVE_CALLBACK_DISPATCH_REPRS,
         NY_NATIVE_ABI_BORROWED,
         NY_NATIVE_CALLBACK_DISPATCH_REPRS,
         NY_NATIVE_ABI_BORROWED,
         NY_NATIVE_ABI_BORROWED,
         NY_NATIVE_ABI_RESULT_ABI,
         false,
         {.call5 = rt_call_any5}},
        {"rt_call_any6",
         6,
         NY_NATIVE_CALLBACK_CALLABLE_REPRS,
         NY_NATIVE_CALLBACK_DISPATCH_REPRS,
         NY_NATIVE_ABI_BORROWED,
         NY_NATIVE_CALLBACK_DISPATCH_REPRS,
         NY_NATIVE_ABI_BORROWED,
         NY_NATIVE_ABI_BORROWED,
         NY_NATIVE_ABI_RESULT_ABI,
         false,
         {.call6 = rt_call_any6}},
        {"rt_call_any7",
         7,
         NY_NATIVE_CALLBACK_CALLABLE_REPRS,
         NY_NATIVE_CALLBACK_DISPATCH_REPRS,
         NY_NATIVE_ABI_BORROWED,
         NY_NATIVE_CALLBACK_DISPATCH_REPRS,
         NY_NATIVE_ABI_BORROWED,
         NY_NATIVE_ABI_BORROWED,
         NY_NATIVE_ABI_RESULT_ABI,
         false,
         {.call7 = rt_call_any7}},
        {"rt_call_any8",
         8,
         NY_NATIVE_CALLBACK_CALLABLE_REPRS,
         NY_NATIVE_CALLBACK_DISPATCH_REPRS,
         NY_NATIVE_ABI_BORROWED,
         NY_NATIVE_CALLBACK_DISPATCH_REPRS,
         NY_NATIVE_ABI_BORROWED,
         NY_NATIVE_ABI_BORROWED,
         NY_NATIVE_ABI_RESULT_ABI,
         false,
         {.call8 = rt_call_any8}},
        {"rt_call_any9",
         9,
         NY_NATIVE_CALLBACK_CALLABLE_REPRS,
         NY_NATIVE_CALLBACK_DISPATCH_REPRS,
         NY_NATIVE_ABI_BORROWED,
         NY_NATIVE_CALLBACK_DISPATCH_REPRS,
         NY_NATIVE_ABI_BORROWED,
         NY_NATIVE_ABI_BORROWED,
         NY_NATIVE_ABI_RESULT_ABI,
         false,
         {.call9 = rt_call_any9}},
};
#undef NY_NATIVE_CALLBACK_DISPATCH_REPRS
#undef NY_NATIVE_CALLBACK_VALUE_REPRS
#undef NY_NATIVE_CALLBACK_CALLABLE_REPRS
const size_t ny_native_runtime_callback_abi_decl_count =
    sizeof(ny_native_runtime_callback_abi_decls) /
    sizeof(ny_native_runtime_callback_abi_decls[0]);

const ny_native_runtime_callback_abi_decl_t *
ny_native_runtime_callback_abi_decl(const char *symbol) {
  if (!symbol)
    return NULL;
  size_t lo = 0, hi = ny_native_runtime_callback_abi_decl_count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    int cmp = strcmp(symbol, ny_native_runtime_callback_abi_decls[mid].symbol);
    if (cmp == 0)
      return &ny_native_runtime_callback_abi_decls[mid];
    if (cmp < 0)
      hi = mid;
    else
      lo = mid + 1;
  }
  return NULL;
}

const ny_native_runtime_callback_abi_decl_t *
ny_native_runtime_callback_abi_decl_for_arity(size_t source_arity) {
  for (size_t i = 0; i < ny_native_runtime_callback_abi_decl_count; ++i)
    if (ny_native_runtime_callback_abi_decls[i].source_arity == source_arity)
      return &ny_native_runtime_callback_abi_decls[i];
  return NULL;
}

/*
 * Runtime argument entries describe per-source-argument representation and
 * formal ownership at intrinsic call boundaries. Designated C strings are
 * promoted before generic boxing; these C declarations use one i64 slot per
 * operand, so no length/tag side slots are appended. An unspecified
 * ownership field remains unknown and must not be inferred from its C ABI.
 */
const ny_native_runtime_arg_abi_decl_t ny_native_runtime_arg_abi_decls[] = {
    {"rt_any_add", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_RETURNS_BORROW | (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_any_collection_remove_raw", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_RETURNS_BORROW | (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_any_div", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_BORROWS | (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_any_eq", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_BORROWS | (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_any_len", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_any_mod", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_BORROWS | (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_any_mul", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_BORROWS | (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_any_tag", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_any_to_cstr", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_RETURNS_BORROW},
    {"rt_any_to_f64", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_any_to_i64", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_bigfloat_abs", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_bigfloat_add", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_BORROWS | (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_bigfloat_cmp_raw", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_BORROWS | (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_bigfloat_div", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_BORROWS | (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_bigfloat_from_value_raw", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_bigfloat_is", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_bigfloat_mul", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_BORROWS | (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_bigfloat_neg", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_bigfloat_pow_int_raw", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_bigfloat_precision_raw", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_bigfloat_sqrt", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_bigfloat_sub", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_BORROWS | (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_bigfloat_to_f64_raw", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_bigfloat_to_str", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_bigint_add", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_BORROWS | (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_bigint_and", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_BORROWS | (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_bigint_bitlen_raw", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_bigint_clz", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_bigint_cmp_raw", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_BORROWS | (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_bigint_ctz", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_bigint_div", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_BORROWS | (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_bigint_iroot", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_BORROWS | (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_bigint_jacobi", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_BORROWS | (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_bigint_kronecker", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_BORROWS | (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_bigint_legendre", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_BORROWS | (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_bigint_mod", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_BORROWS | (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_bigint_modinv", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_BORROWS | (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_bigint_mul", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_BORROWS | (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_bigint_neg_raw", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_bigint_or", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_BORROWS | (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_bigint_pow", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_BORROWS | (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_bigint_sub", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_BORROWS | (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_bigint_submul", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_RETURNS_BORROW | (NYIR_ARG_CONTRACT_BORROWS << 2) |
         (NYIR_ARG_CONTRACT_BORROWS << 4)},
    {"rt_bigint_to_cstr_raw", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_bigint_to_i64_raw", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_bigint_to_int", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_bigint_to_str", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_bigint_xor", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_BORROWS | (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_bytes_get_raw", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_BORROWS | (NYIR_ARG_CONTRACT_RETURNS_BORROW << 4)},
    {"rt_bytes_index_read_raw", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_bytes_set_raw", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_RETURNS_BORROW},
    {"rt_canvas_clear", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_RETURNS_BORROW},
    {"rt_canvas_free", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_CONSUMES},
    {"rt_canvas_print", 0, 0, 0, 0, 0, 0,
     (NYIR_ARG_CONTRACT_RETURNS_BORROW << 0) |
         (NYIR_ARG_CONTRACT_BORROWS << 6)},
    {"rt_canvas_refresh", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_RETURNS_BORROW},
    {"rt_canvas_set", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_RETURNS_BORROW | (NYIR_ARG_CONTRACT_BORROWS << 6)},
    {"rt_clear_raw", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_RETURNS_BORROW},
    {"rt_clone_raw", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_RETURNS_BORROW},
    {"rt_close", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_CONSUMES},
    {"rt_close_raw", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_CONSUMES},
    {"rt_closesocket", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_CONSUMES},
    {"rt_complex_abs2", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_complex_add", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_BORROWS | (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_complex_conj", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_complex_div", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_BORROWS | (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_complex_eq", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_BORROWS | (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_complex_im_bits", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_complex_im_f64", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_complex_imag", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_complex_mul", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_BORROWS | (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_complex_new", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_BORROWS | (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_complex_re_bits", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_complex_re_f64", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_complex_real", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_contains_raw", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_BORROWS | (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_copy_mem", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_RETURNS_BORROW | (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_copy_mem_raw", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_RETURNS_BORROW | (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_cstr_builder_append", 0, 0, 0, (uint16_t)((1u << 0) | (1u << 1)), 0, 0,
     NYIR_ARG_CONTRACT_RETURNS_BORROW | (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_cstr_builder_append_known", 0, 0, 0, (uint16_t)((1u << 0) | (1u << 1)),
     0, 0,
     NYIR_ARG_CONTRACT_RETURNS_BORROW | (NYIR_ARG_CONTRACT_BORROWS << 2) |
         (NYIR_ARG_CONTRACT_BORROWS << 4)},
    {"rt_cstr_builder_finalize", 0, 0, 0, (uint16_t)(1u << 0), 0, 0,
     NYIR_ARG_CONTRACT_CONSUMES},
    {"rt_cstr_builder_new", 0, 0, 0, (uint16_t)(1u << 0), 0, 0,
     NYIR_ARG_CONTRACT_BORROWS},
    {"rt_cstr_builder_repeat_known", 0, 0, 0,
     (uint16_t)((1u << 0) | (1u << 1) | (1u << 2)), 0, 0,
     NYIR_ARG_CONTRACT_RETURNS_BORROW | (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_cstr_concat", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_BORROWS | (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_cstr_eq", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_BORROWS | (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_cstr_get_raw", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_cstr_index_read_raw", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_cstr_len", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_cstr_replace", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_BORROWS | (NYIR_ARG_CONTRACT_BORROWS << 2) |
         (NYIR_ARG_CONTRACT_BORROWS << 4)},
    {"rt_cstr_to_str", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_RETURNS_BORROW},
    {"rt_dict_clear_raw", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_RETURNS_BORROW},
    {"rt_dict_clone_raw", 0, 0, 0, 0, (uint16_t)(1u << 0), 0,
     NYIR_ARG_CONTRACT_BORROWS},
    {"rt_dict_delete_raw", 0, 3, 2, 0, 0, 0,
     (NYIR_ARG_CONTRACT_RETURNS_BORROW << 0) |
         (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_dict_delete_str_raw", 0, 0, 0, 0, 0, 0,
     (NYIR_ARG_CONTRACT_RETURNS_BORROW << 0) |
         (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_dict_get_i64_raw", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_RETURNS_BORROW |
         (NYIR_ARG_CONTRACT_RETURNS_BORROW << 4)},
    {"rt_dict_get_raw", 0, 7, 7, 0, 0, 0,
     (NYIR_ARG_CONTRACT_RETURNS_BORROW << 0) |
         (NYIR_ARG_CONTRACT_BORROWS << 2) |
         (NYIR_ARG_CONTRACT_RETURNS_BORROW << 4)},
    {"rt_dict_get_str_raw", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_RETURNS_BORROW | (NYIR_ARG_CONTRACT_BORROWS << 2) |
         (NYIR_ARG_CONTRACT_RETURNS_BORROW << 4)},
    {"rt_dict_has_raw", 0, 3, 2, 0, 0, 0,
     (NYIR_ARG_CONTRACT_BORROWS << 0) | (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_dict_has_str_raw", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_BORROWS | (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_dict_items_raw", 0, 0, 0, 0, (uint16_t)(1u << 0), 0,
     NYIR_ARG_CONTRACT_BORROWS},
    {"rt_dict_keys_raw", 0, 0, 0, 0, (uint16_t)(1u << 0), 0,
     NYIR_ARG_CONTRACT_BORROWS},
    {"rt_dict_len_raw", 0, 0, 0, 0, (uint16_t)(1u << 0), 0,
     NYIR_ARG_CONTRACT_BORROWS},
    {"rt_dict_merge_raw", 0, 0, 0, 0, 0, 0,
     (NYIR_ARG_CONTRACT_RETURNS_BORROW << 0) |
         (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_dict_set_i64_raw", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_RETURNS_BORROW},
    {"rt_dict_set_raw", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_RETURNS_BORROW},
    {"rt_dict_set_str_raw", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_RETURNS_BORROW},
    {"rt_dict_values_raw", 0, 0, 0, 0, (uint16_t)(1u << 0), 0,
     NYIR_ARG_CONTRACT_BORROWS},
    {"rt_dir_close", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_CONSUMES},
    {"rt_dir_open", 0, 0, 0, (uint16_t)(1u << 0), 0, 0,
     NYIR_ARG_CONTRACT_BORROWS},
    {"rt_dir_read", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_dlclose", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_CONSUMES},
    {"rt_dlopen", 0, 0, 0, (uint16_t)(1u << 0), 0, 0,
     NYIR_ARG_CONTRACT_BORROWS},
    {"rt_dlsym", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_BORROWS | (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_drop_owned", 0, 0, 0, (uint16_t)(1u << 0), 0, 0,
     NYIR_ARG_CONTRACT_CONSUMES},
    {"rt_drop_owned_slot", 0, 0, 0, (uint16_t)(1u << 0), 0, 0,
     NYIR_ARG_CONTRACT_BORROWS},
    {"rt_file_exists_raw", 0, 0, 0, (uint16_t)(1u << 0), 0, 0,
     NYIR_ARG_CONTRACT_BORROWS},
    {"rt_free", 0, 0, 0, (uint16_t)(1u << 0), 0, 0, NYIR_ARG_CONTRACT_CONSUMES},
    {"rt_free_raw", 0, 0, 0, (uint16_t)(1u << 0), 0, 0,
     NYIR_ARG_CONTRACT_CONSUMES},
    {"rt_init_str", 0, 0, 0, (uint16_t)(1u << 0), 0, 0,
     NYIR_ARG_CONTRACT_RETURNS_BORROW},
    {"rt_init_str_raw", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_RETURNS_BORROW},
    {"rt_kqueue_close", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_CONSUMES},
    {"rt_len_strict", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_list_set_len_raw", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_RETURNS_BORROW},
    {"rt_load16_idx", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_load32_f64", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_load32_h", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_load32_idx", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_load64_h", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_load64_idx", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_load8_idx", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_load8_raw", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_load_item", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_RETURNS_BORROW},
    {"rt_load_item_any", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_RETURNS_BORROW},
    {"rt_memcmp", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_BORROWS | (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_memcmp_raw", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_BORROWS | (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_memcpy", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_RETURNS_BORROW | (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_memcpy_raw", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_RETURNS_BORROW | (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_memset", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_RETURNS_BORROW},
    {"rt_memset_raw", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_RETURNS_BORROW},
    {"rt_mutex_free", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_CONSUMES},
    {"rt_mutex_lock64", 0, 0, 0, 1, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_mutex_unlock64", 0, 0, 0, 1, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_native_dict_set_fast_i64", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_RETURNS_BORROW},
    {"rt_native_dict_set_native_i64", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_RETURNS_BORROW},
    {"rt_native_dict_set_nir_i64", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_RETURNS_BORROW | (NYIR_ARG_CONTRACT_BORROWS << 2) |
         (NYIR_ARG_CONTRACT_BORROWS << 4)},
    {"rt_native_dict_set_nir_i64_raw_int", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_RETURNS_BORROW | (NYIR_ARG_CONTRACT_BORROWS << 2) |
         (NYIR_ARG_CONTRACT_BORROWS << 4)},
    {"rt_native_dict_set_nir_i64_raw_int_key", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_RETURNS_BORROW | (NYIR_ARG_CONTRACT_BORROWS << 2) |
         (NYIR_ARG_CONTRACT_BORROWS << 4)},
    {"rt_native_dict_set_raw_i64", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_RETURNS_BORROW},
    {"rt_native_dict_set_str_compact", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_RETURNS_BORROW | (NYIR_ARG_CONTRACT_BORROWS << 2) |
         (NYIR_ARG_CONTRACT_BORROWS << 4)},
    {"rt_open_raw", 0, 0, 0, (uint16_t)(1u << 0), 0, 0,
     NYIR_ARG_CONTRACT_BORROWS},
    {"rt_print_cstr", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_RETURNS_BORROW},
    {"rt_print_str_raw", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_RETURNS_BORROW},
    {"rt_proof_cert_check",
     (uint16_t)((1u << 0) | (1u << 2) | (1u << 3) | (1u << 4)),
     (uint16_t)((1u << 10) - 1u), (uint16_t)((1u << 10) - 1u), 0, 0, 0,
     NYIR_ARG_CONTRACT_BORROWS},
    {"rt_proof_cert_digest", (uint16_t)((1u << 4) - 1u),
     (uint16_t)((1u << 4) - 1u), 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_proof_policy_check", (uint16_t)((1u << 2) - 1u),
     (uint16_t)((1u << 2) - 1u), 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_proof_policy_digest_hex", (uint16_t)(1u << 0), (uint16_t)(1u << 0), 0,
     0, 0, 0, NYIR_ARG_CONTRACT_RETURNS_BORROW},
    {"rt_proof_policy_solve", (uint16_t)((1u << 3) - 1u),
     (uint16_t)((1u << 3) - 1u), 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_RETURNS_BORROW | NYIR_ARG_CONTRACT_BORROWS},
    {"rt_ptr_add", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_RETURNS_BORROW},
    {"rt_ptr_add_i64", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_RETURNS_BORROW},
    {"rt_ptr_key", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_ptr_sub", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_RETURNS_BORROW},
    {"rt_ptr_sub_i64", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_RETURNS_BORROW},
    {"rt_range_index_read_raw", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_range_len_raw", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_range_values_raw", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_rc_count", 0, 0, 0, (uint16_t)(1u << 0), 0, 0,
     NYIR_ARG_CONTRACT_BORROWS},
    {"rt_read_fd_raw", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS << 2},
    {"rt_read_off_raw", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS << 2},
    {"rt_release_owned", 0, 0, 0, (uint16_t)(1u << 0), 0, 0,
     NYIR_ARG_CONTRACT_CONSUMES},
    {"rt_result_err", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_result_ok", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_result_unwrap_or_any", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_RETURNS_BORROW |
         (NYIR_ARG_CONTRACT_RETURNS_BORROW << 2)},
    {"rt_result_unwrap_or_raw", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_result_unwrap_raw", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_retain_owned", 0, 0, 0, (uint16_t)(1u << 0), 0, 0,
     NYIR_ARG_CONTRACT_BORROWS},
    {"rt_sequence_len_raw", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_set_add_raw", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_RETURNS_BORROW | (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_set_from_sequence", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_RETURNS_BORROW},
    {"rt_set_len_raw", 0, 0, 0, 0, (uint16_t)(1u << 0), 0,
     NYIR_ARG_CONTRACT_BORROWS},
    {"rt_set_remove", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_RETURNS_BORROW | (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_set_remove_raw", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_RETURNS_BORROW | (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_store16_idx", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_BORROWS | (NYIR_ARG_CONTRACT_RETURNS_BORROW << 4)},
    {"rt_store32_f64", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_store32_idx", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_BORROWS | (NYIR_ARG_CONTRACT_RETURNS_BORROW << 4)},
    {"rt_store64_h", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_BORROWS | (NYIR_ARG_CONTRACT_RETURNS_BORROW << 4)},
    {"rt_store64_idx", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_BORROWS | (NYIR_ARG_CONTRACT_RETURNS_BORROW << 4)},
    {"rt_store8_idx", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_BORROWS | (NYIR_ARG_CONTRACT_RETURNS_BORROW << 4)},
    {"rt_store8_raw", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_BORROWS | (NYIR_ARG_CONTRACT_RETURNS_BORROW << 4)},
    {"rt_store_item", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_BORROWS | (NYIR_ARG_CONTRACT_RETURNS_BORROW << 4)},
    {"rt_store_item_fast", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_BORROWS | (NYIR_ARG_CONTRACT_RETURNS_BORROW << 4)},
    {"rt_store_item_fast_raw", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_BORROWS | (NYIR_ARG_CONTRACT_RETURNS_BORROW << 4)},
    {"rt_str_builder_append", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_RETURNS_BORROW | (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_str_builder_append_byte", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_RETURNS_BORROW},
    {"rt_str_builder_free", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_CONSUMES},
    {"rt_str_builder_to_str", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_str_byte", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_syscall_raw", 0, (uint16_t)((1u << 7) - 1u),
     (uint16_t)(((1u << 7) - 1u) & ~1u), 0, 0, 0, 0},
    {"rt_tbuf_append_i64_raw", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_RETURNS_BORROW},
    {"rt_tbuf_append_raw", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_RETURNS_BORROW | (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_tbuf_append_tagged", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_RETURNS_BORROW | (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_tbuf_clear_raw", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_RETURNS_BORROW},
    {"rt_tbuf_clone_raw", 0, 0, 0, 0, 0, (uint16_t)(1u << 0),
     NYIR_ARG_CONTRACT_BORROWS},
    {"rt_tbuf_concat_raw", 0, 0, 0, 0, 0, 0,
     (NYIR_ARG_CONTRACT_BORROWS << 0) | (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_tbuf_extend", 0, 0, 0, 0, 0, 0,
     (NYIR_ARG_CONTRACT_RETURNS_BORROW << 0) |
         (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_tbuf_get_any", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_RETURNS_BORROW |
         (NYIR_ARG_CONTRACT_RETURNS_BORROW << 4)},
    {"rt_tbuf_index_any", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_RETURNS_BORROW},
    {"rt_tbuf_index_any_raw", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_RETURNS_BORROW},
    {"rt_tbuf_index_f64", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_tbuf_index_read_raw", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_tbuf_len_raw", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_tbuf_pop_raw", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_tbuf_reserve", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_RETURNS_BORROW},
    {"rt_tbuf_set_f64_bits", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_RETURNS_BORROW},
    {"rt_tbuf_set_i64_raw", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_RETURNS_BORROW},
    {"rt_tbuf_set_tagged", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_RETURNS_BORROW},
    {"rt_tbuf_swap", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_RETURNS_BORROW},
    {"rt_tbuf_tag", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_tbuf_to_cstr", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_thread_join", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_CONSUMES},
    {"rt_to_str", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_RETURNS_BORROW},
    {"rt_unwrap", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_RETURNS_BORROW},
    {"rt_value_get_index_raw", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_RETURNS_BORROW |
         (NYIR_ARG_CONTRACT_RETURNS_BORROW << 4)},
    {"rt_value_is_ptr", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_value_tag", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_value_to_f64", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_vec_add_raw", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_RETURNS_BORROW | (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_vec_div_component_raw", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_RETURNS_BORROW | (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_vec_div_scalar_raw", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_RETURNS_BORROW},
    {"rt_vec_dot_raw", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_BORROWS | (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_vec_mark_raw", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS},
    {"rt_vec_mul_scalar_raw", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_RETURNS_BORROW},
    {"rt_vec_sub_raw", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_RETURNS_BORROW | (NYIR_ARG_CONTRACT_BORROWS << 2)},
    {"rt_websocket_close", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_CONSUMES},
    {"rt_win32_find_close_change", 0, 0, 0, 0, 0, 0,
     NYIR_ARG_CONTRACT_CONSUMES},
    {"rt_write_buffered_raw", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS << 2},
    {"rt_write_fd_raw", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS << 2},
    {"rt_write_off", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS << 2},
    {"rt_write_off_raw", 0, 0, 0, 0, 0, 0, NYIR_ARG_CONTRACT_BORROWS << 2},
    {"rt_zfree_raw", 0, 0, 0, (uint16_t)(1u << 0), 0, 0,
     NYIR_ARG_CONTRACT_CONSUMES},
};
const size_t ny_native_runtime_arg_abi_decl_count =
    sizeof(ny_native_runtime_arg_abi_decls) /
    sizeof(ny_native_runtime_arg_abi_decls[0]);

const ny_native_runtime_arg_abi_decl_t *
ny_native_runtime_arg_abi_decl(const char *symbol) {
  if (!symbol)
    return NULL;
  size_t lo = 0, hi = ny_native_runtime_arg_abi_decl_count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    int cmp = strcmp(symbol, ny_native_runtime_arg_abi_decls[mid].symbol);
    if (cmp == 0)
      return &ny_native_runtime_arg_abi_decls[mid];
    if (cmp < 0)
      hi = mid;
    else
      lo = mid + 1;
  }
  return NULL;
}

const ny_native_runtime_overload_abi_decl_t
    ny_native_runtime_overload_abi_decls[] = {
        {"rt_bigfloat_from_value_raw", "rt_bigfloat_from_f64", NULL, NULL},
        {"rt_bigint_from_i64_raw", NULL, "rt_bigint_from_int", NULL},
        {"rt_long", NULL, NULL, "rt_bigint_from_i64_raw"},
};
const size_t ny_native_runtime_overload_abi_decl_count =
    sizeof(ny_native_runtime_overload_abi_decls) /
    sizeof(ny_native_runtime_overload_abi_decls[0]);

const ny_native_runtime_overload_abi_decl_t *
ny_native_runtime_overload_abi_decl(const char *symbol) {
  if (!symbol)
    return NULL;
  size_t lo = 0, hi = ny_native_runtime_overload_abi_decl_count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    int cmp = strcmp(symbol, ny_native_runtime_overload_abi_decls[mid].symbol);
    if (cmp == 0)
      return &ny_native_runtime_overload_abi_decls[mid];
    if (cmp < 0)
      hi = mid;
    else
      lo = mid + 1;
  }
  return NULL;
}

const ny_native_runtime_abi_decl_t *
ny_native_runtime_abi_decl(const char *symbol) {
  if (!symbol)
    return NULL;
  size_t lo = 0;
  size_t hi = ny_native_runtime_abi_decl_count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    int cmp = strcmp(symbol, ny_native_runtime_abi_decls[mid].symbol);
    if (cmp == 0)
      return &ny_native_runtime_abi_decls[mid];
    if (cmp < 0)
      hi = mid;
    else
      lo = mid + 1;
  }
  return NULL;
}

const ny_native_runtime_value_abi_decl_t *
ny_native_runtime_value_abi_decl(const char *symbol) {
  if (!symbol)
    return NULL;
  size_t lo = 0;
  size_t hi = ny_native_runtime_value_abi_decl_count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    int cmp = strcmp(symbol, ny_native_runtime_value_abi_decls[mid].symbol);
    if (cmp == 0)
      return &ny_native_runtime_value_abi_decls[mid];
    if (cmp < 0)
      hi = mid;
    else
      lo = mid + 1;
  }
  return NULL;
}

static bool ny_native_runtime_symbol_returns_f64(const char *symbol) {
  const ny_native_runtime_abi_decl_t *decl = ny_native_runtime_abi_decl(symbol);
  return decl && (decl->flags & NY_NATIVE_RUNTIME_ABI_RESULT_F64) != 0;
}

static bool ny_native_runtime_symbol_skips_dynamic_boxing(const char *symbol) {
  const ny_native_runtime_abi_decl_t *decl = ny_native_runtime_abi_decl(symbol);
  return decl && (decl->flags & NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING) != 0;
}

static bool ny_native_runtime_symbol_arg_is_tagged(const char *symbol,
                                                   size_t index) {
  return ny_native_runtime_abi_arg_is_tagged(ny_native_runtime_abi_decl(symbol),
                                             index);
}

/*
 * Source preference is explicit ABI metadata; symbol spelling is not policy.
 */
static bool ny_native_runtime_stub_yields_to_source(const char *sym) {
  const ny_native_runtime_abi_decl_t *decl = ny_native_runtime_abi_decl(sym);
  return decl && (decl->flags & NY_NATIVE_RUNTIME_ABI_SOURCE_PREFERRED) != 0;
}
/*
 * Semantic-first runtime symbol resolution. When typing resolved a direct
 * attached method or dynamic-contract call to a canonical callee, prefer that
 * fact over the surface identifier spelling (aliases, re-exports, and
 * attached-method sugar can all spell the same callee differently). Fall back
 * to spelling only when no semantic callee exists or it maps to nothing.
 */
static const char *ny_native_runtime_symbol_for_expr(const char *name,
                                                     const char *leaf,
                                                     const expr_t *e) {
  /*
   * A typed call may carry the provider's fully-qualified spelling in the
   * semantic declaration even though the intrinsic registry is keyed by its
   * public leaf.  Use that declaration only for stdlib declarations; this
   * keeps a user-defined `Thing.memcmp` (or another qualified source method)
   * from being mistaken for a runtime intrinsic while still making imported
   * intrinsic wrappers stable under module expansion and overload imports.
   */
  if (e && e->semantic.canonical_callee_stmt &&
      e->semantic.canonical_callee_stmt->kind == NY_S_FUNC &&
      ny_is_stdlib_tok(e->semantic.canonical_callee_stmt->tok)) {
    const char *decl_name = e->semantic.canonical_callee_stmt->as.fn.name;
    const char *decl_leaf = ny_name_leaf(decl_name);
    const char *decl_sym = nyr_runtime_symbol(decl_leaf);
    if (decl_sym)
      return decl_sym;
  }
  if (e && e->semantic.member_call_kind != NY_SEM_CALL_NONE &&
      e->semantic.canonical_callee) {
    const char *canon = nyr_runtime_symbol(e->semantic.canonical_callee);
    if (canon)
      return canon;
    const char *canon_leaf = ny_name_leaf(e->semantic.canonical_callee);
    if (canon_leaf && canon_leaf != e->semantic.canonical_callee) {
      canon = nyr_runtime_symbol(canon_leaf);
      if (canon)
        return canon;
    }
  }
  if (name) {
    const char *sym = nyr_runtime_symbol(name);
    if (sym)
      return sym;
  }
  /*
   * A qualified source function is not the unqualified builtin merely
   * because both names share a leaf.  In particular, an attached method such
   * as `bigint.bytes` must reach its selected declaration; falling back from
   * `core.bigint.bytes` to the constructor intrinsic `bytes` silently
   * changed the receiver into a length and returned the wrong value.  Runtime
   * aliases that are genuinely intrinsic have explicit qualified entries in
   * the table above.
   */
  if (leaf && (!name || !strchr(name, '.'))) {
    const char *sym = nyr_runtime_symbol(leaf);
    if (sym)
      return sym;
  }
  return NULL;
}
static bool ny_native_ffi_symbol_name(const char *name) {
  if (!name || !*name)
    return false;
  return strncmp(name, "vk", 2) == 0 || strncmp(name, "gl", 2) == 0 ||
         strncmp(name, "egl", 3) == 0 || strncmp(name, "X", 1) == 0 ||
         strncmp(name, "wl_", 3) == 0 || strncmp(name, "xcb_", 4) == 0 ||
         strncmp(name, "FT_", 3) == 0;
}

typedef struct {
  const expr_t *expr;
  const char *type_name;
  bool resolved;
} ny_native_type_query_cache_entry_t;

typedef struct {
  const program_t *prog;
  const expr_t *receiver;
  const char *receiver_type;
  const char *method;
  const stmt_t *result;
  uint64_t hash;
  bool resolved;
} ny_attached_cache_entry_t;

/*
 * Memo for attached-operator resolution.  The key strings (owner, op,
 * right operand type) are interned through the owner builder's
 * owned-string table: the type-query memo returns one stable string per
 * expression node, but distinct nodes of the same type hold distinct
 * string instances, so pointer-keyed entries would miss every query.
 * Content interning gives equal types one owner-lifetime key, and the
 * program pointer is part of the key so probe builders (shallow copies
 * sharing the compilation's program) hit the same entries as the owner.
 */
typedef struct {
  const program_t *prog;
  const char *owner;
  const char *op;
  const char *right; /* NULL when the right operand has no resolved type */
  const stmt_t *result;
  uint64_t hash;
  bool resolved;
} ny_operator_cache_entry_t;

typedef struct {
  const program_t *prog;
  const char *owner;
  const char *op;
  bool has_overload;
} ny_operator_pair_cache_entry_t;

typedef struct {
  const char *name;
  const char *type_name; /* declared nominal owner type, when available */
  int slot;
  ny_sem_rep_t semantic_rep;
  nyr_scalar_class_t scalar_class; /* current machine-word representation */
  bool scalar_class_mixed; /* assignments disagree; do not guess at loads */
  ny_sem_ownership_t semantic_ownership;
  uint32_t alias_class;
  uint32_t alias_count;
  bool semantic_mutable;
  bool is_f64;
  bool is_f32;
  bool is_cstr;
  bool is_bytes;
  bool is_bool; /* raw 0/1 i64 from a comparison/logical/boolean literal */
  bool is_sb;   /* cstr backed by an amortized-growth builder (self-concat) */
  bool sb_candidate; /* unobserved mutable local initialized from a literal */
  int sb_slot;       /* slot holding the native builder handle, or -1 */
  bool is_any;       /* any-typed: runtime tag dispatch required, not raw i64 */
  bool is_bigint;    /* bigint handle, not a raw integer payload */
  bool is_dict;      /* dictionary handle used by string/integer indexing */
  bool is_capture;   /* value lives in a lambda environment slot */
  size_t capture_index;
  bool is_list; /* shared native list value: pointer slot + length slot */
  const expr_t *callable_expr; /* direct lambda/function value initializer */
  const char *callable_name;   /* direct named function value initializer */
  bool is_dyn_list;        /* stride 24 (descriptors) vs stride 8 (scalars) */
  bool list_started_empty; /* declared from `[]`: first raw-int append may
                            * settle the elem-8 scalar representation */
  bool raw_dynamic_index;  /* scalar payload awaiting a dynamic callback */
  const expr_t *static_string_literal; /* immutable direct string initializer */
  const expr_t *list_literal; /* current declaration's non-escaping literal */
  const expr_t
      *dict_literal; /* current declaration's non-escaping dict literal */
  int list_len_slot;
  int dyn_str_len_slot;
  int dyn_tag_slot;
  int arg_slot;             /* incoming argument index for this parameter */
  int list_len_arg_slot;    /* incoming argument index for list length */
  int dyn_str_len_arg_slot; /* incoming argument index for str length */
  int dyn_tag_arg_slot;     /* incoming argument index for str/any tag */
  int64_t buffer_byte_len;  /* comptime-known allocation size in bytes, or 0 */
  int64_t fin_bound;        /* literal Fin<N> bound, or 0 when not known */
  uint32_t syntax_ctx;

} nyr_local_t;
typedef struct {
  const expr_t *expr;
  stmt_t *fn;
  char *name;
  const stmt_t *defer_body;
  bool owns_expr;
  char *capture_names[16];
  size_t capture_count;
  nyr_local_t capture_types[16];
} ny_native_lambda_entry_t;
typedef struct {
  ny_native_lambda_entry_t *entries;
  size_t count;
  size_t cap;
} ny_native_lambda_cache_t;
static bool ny_native_type_name_is_f64(const char *name);
static bool ny_native_type_name_is_f32(const char *name);
static bool ny_native_type_name_is_int(const char *name);
static bool ny_native_type_name_is_list(const char *name);
typedef struct nyr_builder_impl_t nyr_builder_impl_t;
typedef nyr_builder_impl_t nyr_builder_t;
typedef nyr_builder_t nyr_builder_t;
static const stmt_t *ny_native_fn_cache_lookup_exact_with_builder(
    const struct nyr_builder_impl_t *builder, const char *name);
static bool ny_native_type_name_is_str(const char *name);
static bool ny_native_type_name_is_any(const char *name);
static bool ny_native_type_name_is_ptr(const char *name);

static ny_native_lambda_entry_t *
ny_native_lambda_find(ny_native_lambda_cache_t *cache, const expr_t *expr) {
  if (!cache)
    return NULL;
  for (size_t i = 0; i < cache->count; ++i)
    if (cache->entries[i].expr == expr)
      return &cache->entries[i];
  return NULL;
}

static ny_native_lambda_entry_t *
ny_native_lambda_find_defer(ny_native_lambda_cache_t *cache,
                            const stmt_t *body) {
  if (!cache || !body)
    return NULL;
  for (size_t i = 0; i < cache->count; ++i)
    if (cache->entries[i].defer_body == body)
      return &cache->entries[i];
  return NULL;
}

static ny_native_lambda_entry_t *
ny_native_lambda_find_name(ny_native_lambda_cache_t *cache, const char *name) {
  if (!cache || !name)
    return NULL;
  for (size_t i = 0; i < cache->count; ++i)
    if (cache->entries[i].name && strcmp(cache->entries[i].name, name) == 0)
      return &cache->entries[i];
  return NULL;
}

static bool ny_native_lambda_param_named(const expr_t *e, const char *name) {
  if (!e || !name || (e->kind != NY_E_FN && e->kind != NY_E_LAMBDA))
    return false;
  for (size_t i = 0; i < e->as.lambda.params.len; ++i)
    if (e->as.lambda.params.data[i].name &&
        strcmp(e->as.lambda.params.data[i].name, name) == 0)
      return true;
  return false;
}

static bool ny_native_lambda_capture_add(ny_native_lambda_entry_t *entry,
                                         const char *name) {
  if (!entry || !name || !*name || entry->capture_count >= 16)
    return entry && entry->capture_count < 16;
  for (size_t i = 0; i < entry->capture_count; ++i)
    if (strcmp(entry->capture_names[i], name) == 0)
      return true;
  char *copy = strdup(name);
  if (!copy)
    return false;
  entry->capture_names[entry->capture_count++] = copy;
  return true;
}

static stmt_t *ny_native_lambda_create(ny_native_lambda_cache_t *cache,
                                       const expr_t *e) {
  if (!e || (e->kind != NY_E_FN && e->kind != NY_E_LAMBDA) ||
      !e->as.lambda.body || !cache)
    return NULL;
  ny_native_lambda_entry_t *existing = ny_native_lambda_find(cache, e);
  if (existing)
    return existing->fn;
  if (cache->count == cache->cap) {
    size_t cap = cache->cap ? cache->cap * 2 : 64;
    ny_native_lambda_entry_t *grown =
        realloc(cache->entries, cap * sizeof(*grown));
    if (!grown)
      return NULL;
    cache->entries = grown;
    cache->cap = cap;
  }
  stmt_t *fn = calloc(1, sizeof(*fn));
  if (!fn)
    return NULL;
  char name[96];
  /*
   * Source coordinates are diagnostic information, not expression identity:
   * imported files and expanded syntax can contain distinct lambdas at the
   * same position.  The registry already interns each AST expression once;
   * use its program-wide ordinal to keep their emitted symbols distinct.
   */
  int n = snprintf(name, sizeof(name), "__ny_lambda_%d_%d_%zu", e->tok.line,
                   e->tok.col, cache->count);
  if (n <= 0 || (size_t)n >= sizeof(name)) {
    free(fn);
    return NULL;
  }
  fn->kind = NY_S_FUNC;
  fn->tok = e->tok;
  fn->as.fn.name = strdup(name);
  /*
   * Preserve the lambda's declared result ABI.  Generated functions used to
   * lose `fn(int x) int { ... }` here, so callers saw an untyped/raw result
   * and the dynamic print boundary decoded 42 as 21.
   */
  fn->as.fn.return_type = e->as.lambda.return_type;
  if (fn->as.fn.return_type) {
    fn->as.fn.return_semantic.resolved = true;
    /*
     * One shared classification: aggregate/pointer/typed-buffer result names
     * must not fall back to the raw-int scalar ABI.
     */
    fn->as.fn.return_semantic.rep =
        ny_semantic_rep_for_type(fn->as.fn.return_type);
  } else {
    /*
     * Untyped lambda results use the canonical dynamic callback ABI.
     */
    fn->as.fn.return_semantic.resolved = true;
    fn->as.fn.return_semantic.rep = NY_SEM_REP_TAGGED_DYNAMIC;
  }
  fn->as.fn.body = e->as.lambda.body;
  fn->as.fn.params = e->as.lambda.params;
  cache->entries[cache->count++] = (ny_native_lambda_entry_t){
      .expr = e, .fn = fn, .name = (char *)fn->as.fn.name};
  return fn;
}

static ny_native_lambda_entry_t *
ny_native_defer_lambda_create(ny_native_lambda_cache_t *cache,
                              const stmt_t *body) {
  ny_native_lambda_entry_t *entry = ny_native_lambda_find_defer(cache, body);
  if (entry)
    return entry;
  expr_t *expr = calloc(1, sizeof(*expr));
  if (!expr)
    return NULL;
  expr->kind = NY_E_LAMBDA;
  expr->tok = body ? body->tok : (token_t){0};
  expr->as.lambda.body = (stmt_t *)body;
  if (!ny_native_lambda_create(cache, expr)) {
    free(expr);
    return NULL;
  }
  entry = ny_native_lambda_find(cache, expr);
  if (!entry) {
    free(expr);
    return NULL;
  }
  entry->defer_body = body;
  entry->owns_expr = true;
  return entry;
}

static void ny_native_lambda_cache_free(ny_native_lambda_cache_t *cache) {
  if (!cache)
    return;
  for (size_t i = 0; i < cache->count; ++i) {
    for (size_t j = 0; j < cache->entries[i].capture_count; ++j)
      free(cache->entries[i].capture_names[j]);
    free(cache->entries[i].name);
    free(cache->entries[i].fn);
    if (cache->entries[i].owns_expr)
      free((void *)cache->entries[i].expr);
  }
  free(cache->entries);
  free(cache);
}

typedef struct {
  int head_label;
  int continue_label;
  int end_label;
  size_t defer_mark;
} nyr_loop_frame_t;

typedef struct {
  const char *name;
  int label;
  bool emitted;
} nyr_label_t;

typedef enum {
  NYR_FACT_ALLOC = 1,
  NYR_FACT_FIN = 2,
  NYR_FACT_LIST_LEN = 3,
  NYR_FACT_DYN_STR_LEN = 4,
  NYR_FACT_DYN_TAG = 5,
} nyr_fact_kind_t;

typedef struct {
  int value;
  int64_t payload;
  nyr_fact_kind_t kind;
} nyr_fact_t;

#define NY_EXTERN_MAX 1024

typedef enum {
  NY_SYSV_AGG_NONE = 0,
  NY_SYSV_AGG_INTEGER,
  NY_SYSV_AGG_SSE,
  NY_SYSV_AGG_MEMORY,
  NY_SYSV_AGG_UNSUPPORTED,
  NY_SYSV_AGG_HFA_F32,
  NY_SYSV_AGG_HFA_F64,
  NY_SYSV_AGG_HVA_V128,
  NY_SYSV_AGG_AAPCS_INTEGER_A16,
} ny_sysv_agg_class_t;

typedef struct {
  const char *ny_name;
  const char *c_symbol;
  unsigned param_count;
  bool owned;
  /*
   * Non-zero if the function returns an aggregate by value.
   */
  uint32_t ret_aggregate_size;
  ny_sysv_agg_class_t ret_aggregate_classes[2];
  /*
   * Per-argument byval sizes; 0 = scalar, >0 = aggregate of that byte size.
   */
  uint32_t arg_aggregate_sizes[NY_C_MAX_PARAMS];
  bool ret_f64;
  bool ret_f32;
  /*
   * C returns this as raw i64; dynamic Ny call results need boxing.
   */
  bool ret_signed_i64;
  bool param_f64[NY_C_MAX_PARAMS];
  bool param_f32[NY_C_MAX_PARAMS];
} ny_extern_entry_t;

typedef struct {
  char *name;
  size_t offset;
  size_t size;
  size_t align;
} ny_native_c_layout_field_t;

typedef struct ny_native_c_layout_t {
  char *name;
  size_t size;
  size_t align;
  size_t field_count;
  ny_native_c_layout_field_t fields[NY_C_MAX_FIELDS];
} ny_native_c_layout_t;

#define NY_NATIVE_C_LAYOUT_MAX 512
typedef struct ny_native_c_layout_table_t {
  ny_native_c_layout_t entries[NY_NATIVE_C_LAYOUT_MAX];
  size_t count;
} ny_native_c_layout_table_t;

typedef struct {
  char *name;
  int64_t value;
} ny_native_c_define_t;

typedef struct {
  ny_native_c_define_t *entries;
  size_t count;
  size_t cap;
  size_t *slots;
  size_t slot_cap;
} ny_native_c_define_table_t;

typedef struct {
  ny_extern_entry_t entries[NY_EXTERN_MAX];
  size_t count;
  ny_native_c_layout_table_t layouts;
  ny_native_c_define_table_t defines;
} ny_extern_table_t;

static void ny_extern_table_init(ny_extern_table_t *t) {
  if (t) {
    t->count = 0;
    memset(&t->layouts, 0, sizeof(t->layouts));
    memset(&t->defines, 0, sizeof(t->defines));
  }
}

static void ny_extern_table_free(ny_extern_table_t *t) {
  if (!t)
    return;
  for (size_t i = 0; i < t->count; ++i) {
    if (t->entries[i].owned) {
      free((void *)t->entries[i].ny_name);
      free((void *)t->entries[i].c_symbol);
    }
  }
  for (size_t i = 0; i < t->layouts.count; ++i) {
    ny_native_c_layout_t *layout = &t->layouts.entries[i];
    free(layout->name);
    for (size_t j = 0; j < layout->field_count; ++j)
      free(layout->fields[j].name);
  }
  memset(&t->layouts, 0, sizeof(t->layouts));
  for (size_t i = 0; i < t->defines.count; ++i)
    free(t->defines.entries[i].name);
  free(t->defines.entries);
  free(t->defines.slots);
  memset(&t->defines, 0, sizeof(t->defines));
  t->count = 0;
}

static bool ny_native_c_token_equal(ny_ctok_t a, ny_ctok_t b) {
  return a.kind == NY_CTOK_IDENT && b.kind == NY_CTOK_IDENT && a.len == b.len &&
         a.start && b.start && memcmp(a.start, b.start, a.len) == 0;
}

static char *ny_native_c_token_dup(ny_ctok_t tok) {
  if (!tok.start || tok.len == 0)
    return NULL;
  char *copy = malloc(tok.len + 1);
  if (!copy)
    return NULL;
  memcpy(copy, tok.start, tok.len);
  copy[tok.len] = '\0';
  return copy;
}

static bool ny_native_c_type_is_f64(const ny_ctype_t *type) {
  return type && type->ptr_depth == 0 && type->array_elems == 0 &&
         !type->array_unknown && type->kind == NY_CTYPE_DOUBLE &&
         !(type->flags & NY_CTYPEF_COMPLEX);
}

static bool ny_native_c_type_is_f32(const ny_ctype_t *type) {
  return type && type->ptr_depth == 0 && type->array_elems == 0 &&
         !type->array_unknown && type->kind == NY_CTYPE_FLOAT &&
         !(type->flags & NY_CTYPEF_COMPLEX);
}

static bool ny_native_c_type_is_signed_i64(const ny_ctype_t *type,
                                           const char *abi) {
  if (!type || type->ptr_depth != 0 || type->array_elems != 0 ||
      type->array_unknown || (type->flags & NY_CTYPEF_UNSIGNED) ||
      (type->kind != NY_CTYPE_CHAR && type->kind != NY_CTYPE_SHORT &&
       type->kind != NY_CTYPE_INT && type->kind != NY_CTYPE_LONG))
    return false;
  ny_c_layout_t layout = {0};
  return ny_ctype_layout(type, abi, &layout) && layout.is_integer &&
         layout.size == sizeof(int64_t);
}

static ny_native_c_layout_t *
ny_native_c_layout_lookup(const ny_native_c_layout_table_t *table,
                          const char *name) {
  if (!table || !name || !*name)
    return NULL;
  for (size_t i = 0; i < table->count; ++i)
    if (table->entries[i].name && strcmp(table->entries[i].name, name) == 0)
      return (ny_native_c_layout_t *)&table->entries[i];
  return NULL;
}

static bool ny_native_c_layout_add(ny_native_c_layout_table_t *table,
                                   const char *name, const ny_ctype_t *type) {
  if (!table || !name || !*name || !type || !type->aggregate_has_layout ||
      type->aggregate_size == 0)
    return true;
  if (ny_native_c_layout_lookup(table, name))
    return true;
  if (table->count >= NY_NATIVE_C_LAYOUT_MAX)
    return false;
  ny_native_c_layout_t *layout = &table->entries[table->count];
  memset(layout, 0, sizeof(*layout));
  layout->name = ny_strdup(name);
  if (!layout->name)
    return false;
  layout->size = (type->flags & NY_CTYPEF_PACKED) && type->aggregate_packed_size
                     ? type->aggregate_packed_size
                     : type->aggregate_size;
  layout->align = (type->flags & NY_CTYPEF_PACKED)
                      ? 1
                      : (type->aggregate_align ? type->aggregate_align : 1);
  layout->field_count =
      type->field_count < NY_C_MAX_FIELDS ? type->field_count : NY_C_MAX_FIELDS;
  for (size_t i = 0; i < layout->field_count; ++i) {
    const ny_c_field_t *field = &type->fields[i];
    layout->fields[i].name = ny_native_c_token_dup(field->name);
    if (field->name.len && !layout->fields[i].name) {
      for (size_t j = 0; j <= i; ++j)
        free(layout->fields[j].name);
      free(layout->name);
      memset(layout, 0, sizeof(*layout));
      return false;
    }
    layout->fields[i].offset = field->offset;
    layout->fields[i].size = field->size;
    layout->fields[i].align = field->align ? field->align : 1;
  }
  table->count++;
  return true;
}

static bool ny_native_c_layout_collect_parser(ny_native_c_layout_table_t *table,
                                              const ny_parser_t *parser) {
  if (!table || !parser)
    return true;
  for (unsigned i = 0; i < parser->typedef_count; ++i) {
    char *name = ny_native_c_token_dup(parser->typedef_names[i]);
    if (name) {
      bool ok = ny_native_c_layout_add(table, name, &parser->typedef_types[i]);
      free(name);
      if (!ok)
        return false;
    }
    const ny_ctype_t *type = &parser->typedef_types[i];
    if (type->name.kind == NY_CTOK_IDENT) {
      name = ny_native_c_token_dup(type->name);
      if (name) {
        bool ok = ny_native_c_layout_add(table, name, type);
        free(name);
        if (!ok)
          return false;
      }
    }
  }
  for (unsigned i = 0; i < parser->tag_count; ++i) {
    char *name = ny_native_c_token_dup(parser->tag_names[i]);
    if (name) {
      bool ok = ny_native_c_layout_add(table, name, &parser->tag_types[i]);
      free(name);
      if (!ok)
        return false;
    }
  }
  return true;
}

static size_t ny_native_c_define_hash(const char *name) {
  size_t hash = (size_t)1469598103934665603ULL;
  for (const unsigned char *p = (const unsigned char *)name; *p; ++p) {
    hash ^= *p;
    hash *= (size_t)1099511628211ULL;
  }
  return hash;
}

static bool ny_native_c_define_rehash(ny_native_c_define_table_t *table,
                                      size_t slot_cap) {
  size_t *slots = calloc(slot_cap, sizeof(*slots));
  if (!slots)
    return false;
  for (size_t i = 0; i < table->count; ++i) {
    size_t slot =
        ny_native_c_define_hash(table->entries[i].name) & (slot_cap - 1);
    while (slots[slot])
      slot = (slot + 1) & (slot_cap - 1);
    slots[slot] = i + 1;
  }
  free(table->slots);
  table->slots = slots;
  table->slot_cap = slot_cap;
  return true;
}

static size_t ny_native_c_define_find(const ny_native_c_define_table_t *table,
                                      const char *name) {
  if (!table || !name || !table->slot_cap)
    return SIZE_MAX;
  size_t slot = ny_native_c_define_hash(name) & (table->slot_cap - 1);
  while (table->slots[slot]) {
    size_t index = table->slots[slot] - 1;
    if (strcmp(table->entries[index].name, name) == 0)
      return index;
    slot = (slot + 1) & (table->slot_cap - 1);
  }
  return SIZE_MAX;
}

static bool ny_native_c_define_add(ny_native_c_define_table_t *table,
                                   const char *name, int64_t value) {
  if (!table || !name || !*name)
    return true;
  size_t found = ny_native_c_define_find(table, name);
  if (found != SIZE_MAX) {
    table->entries[found].value = value;
    return true;
  }
  if (table->count + 1 > (table->slot_cap * 7) / 10) {
    size_t slot_cap = table->slot_cap ? table->slot_cap * 2 : 256;
    if (slot_cap < table->slot_cap ||
        !ny_native_c_define_rehash(table, slot_cap))
      return false;
  }
  char *copy = ny_strdup(name);
  if (!copy)
    return false;
  if (table->count == table->cap) {
    size_t cap = table->cap ? table->cap * 2 : 128;
    if (cap < table->cap || cap > SIZE_MAX / sizeof(*table->entries)) {
      free(copy);
      return false;
    }
    ny_native_c_define_t *entries =
        realloc(table->entries, cap * sizeof(*entries));
    if (!entries) {
      free(copy);
      return false;
    }
    table->entries = entries;
    table->cap = cap;
  }
  size_t index = table->count++;
  table->entries[index] = (ny_native_c_define_t){copy, value};
  size_t slot = ny_native_c_define_hash(copy) & (table->slot_cap - 1);
  while (table->slots[slot])
    slot = (slot + 1) & (table->slot_cap - 1);
  table->slots[slot] = index + 1;
  return true;
}

static bool ny_native_c_define_lookup(const ny_native_c_define_table_t *table,
                                      const char *name, int64_t *value) {
  if (!table || !name)
    return false;
  size_t found = ny_native_c_define_find(table, name);
  if (found != SIZE_MAX) {
    if (value)
      *value = table->entries[found].value;
    return true;
  }
  return false;
}

static const ny_ctype_t *ny_native_c_nested_type(const ny_parser_t *parser,
                                                 const ny_c_field_t *field) {
  if (!parser || !field || field->type_name.kind != NY_CTOK_IDENT)
    return NULL;
  if (field->kind == NY_CTYPE_NAMED) {
    for (unsigned i = parser->typedef_count; i > 0; --i)
      if (ny_native_c_token_equal(parser->typedef_names[i - 1],
                                  field->type_name))
        return &parser->typedef_types[i - 1];
  }
  if (field->kind == NY_CTYPE_STRUCT || field->kind == NY_CTYPE_UNION) {
    for (unsigned i = parser->tag_count; i > 0; --i)
      if (parser->tag_types[i - 1].kind == field->kind &&
          ny_native_c_token_equal(parser->tag_names[i - 1], field->type_name))
        return &parser->tag_types[i - 1];
  }
  return NULL;
}

static void ny_native_sysv_merge_class(ny_sysv_agg_class_t *dst,
                                       ny_sysv_agg_class_t src) {
  if (*dst == NY_SYSV_AGG_NONE)
    *dst = src;
  else if (*dst != src)
    *dst = NY_SYSV_AGG_INTEGER;
}

static bool ny_native_sysv_classify_aggregate_depth(
    const ny_parser_t *parser, const ny_ctype_t *ty,
    ny_sysv_agg_class_t classes[2], unsigned depth) {
  classes[0] = NY_SYSV_AGG_NONE;
  classes[1] = NY_SYSV_AGG_NONE;
  if (!ty || !ty->aggregate_has_layout || ty->aggregate_size == 0 || depth > 8)
    return false;
  if (ty->aggregate_size > 16) {
    classes[0] = NY_SYSV_AGG_MEMORY;
    return true;
  }
  for (unsigned i = 0; i < ty->field_count; ++i) {
    const ny_c_field_t *field = &ty->fields[i];
    if ((field->align > 1 && field->offset % field->align != 0) ||
        field->offset + field->size > ty->aggregate_size) {
      classes[0] = NY_SYSV_AGG_MEMORY;
      classes[1] = NY_SYSV_AGG_NONE;
      return true;
    }
    ny_sysv_agg_class_t field_classes[2] = {NY_SYSV_AGG_INTEGER,
                                            NY_SYSV_AGG_NONE};
    if (field->ptr_depth == 0 &&
        (field->kind == NY_CTYPE_FLOAT || field->kind == NY_CTYPE_DOUBLE)) {
      field_classes[0] = NY_SYSV_AGG_SSE;
      if (field->size > 8)
        field_classes[1] = NY_SYSV_AGG_SSE;
    } else if (field->ptr_depth == 0 && field->kind == NY_CTYPE_LONG_DOUBLE)
      return false;
    else if (field->ptr_depth == 0 &&
             (field->kind == NY_CTYPE_STRUCT || field->kind == NY_CTYPE_UNION ||
              field->kind == NY_CTYPE_NAMED)) {
      const ny_ctype_t *nested = ny_native_c_nested_type(parser, field);
      if (!nested || !ny_native_sysv_classify_aggregate_depth(
                         parser, nested, field_classes, depth + 1))
        return false;
      if (field_classes[0] == NY_SYSV_AGG_MEMORY)
        return false;
    }
    size_t field_remaining = field->size;
    for (size_t nested_chunk = 0; nested_chunk < 2 && field_remaining > 0;
         ++nested_chunk) {
      ny_sysv_agg_class_t field_class = field_classes[nested_chunk];
      size_t nested_bytes = field_remaining > 8 ? 8 : field_remaining;
      size_t start = field->offset + nested_chunk * 8;
      size_t end = start + nested_bytes - 1;
      if (field_class == NY_SYSV_AGG_NONE || end / 8 > 1)
        return false;
      for (size_t chunk = start / 8; chunk <= end / 8; ++chunk)
        ny_native_sysv_merge_class(&classes[chunk], field_class);
      field_remaining -= nested_bytes;
    }
  }
  if (classes[0] == NY_SYSV_AGG_NONE)
    classes[0] = NY_SYSV_AGG_INTEGER;
  if (ty->aggregate_size > 8 && classes[1] == NY_SYSV_AGG_NONE)
    classes[1] = NY_SYSV_AGG_INTEGER;
  return true;
}

static bool ny_native_sysv_classify_aggregate(const ny_parser_t *parser,
                                              const ny_ctype_t *ty,
                                              ny_sysv_agg_class_t classes[2]) {
  return ny_native_sysv_classify_aggregate_depth(parser, ty, classes, 0);
}

static bool ny_native_aapcs_hfa_type(const ny_parser_t *parser,
                                     const ny_ctype_t *ty,
                                     ny_sysv_agg_class_t *kind, unsigned *count,
                                     unsigned depth) {
  if (!ty || !kind || !count || depth > 8 || ty->ptr_depth)
    return false;
  if (ty->kind == NY_CTYPE_FLOAT || ty->kind == NY_CTYPE_DOUBLE ||
      ty->kind == NY_CTYPE_HALF) {
    ny_sysv_agg_class_t k = ty->kind == NY_CTYPE_FLOAT  ? NY_SYSV_AGG_HFA_F32
                            : ty->kind == NY_CTYPE_HALF ? NY_SYSV_AGG_HFA_F32
                                                        : NY_SYSV_AGG_HFA_F64;
    unsigned n = ty->array_elems ? (unsigned)ty->array_elems : 1u;
    if (n == 0 || n > 4)
      return false;
    *kind = k;
    *count = n;
    return true;
  }
  if (ty->kind == NY_CTYPE_NAMED && ty->name.kind == NY_CTOK_IDENT) {
    for (unsigned i = parser ? parser->typedef_count : 0; i > 0; --i) {
      if (ny_native_c_token_equal(parser->typedef_names[i - 1], ty->name))
        return ny_native_aapcs_hfa_type(parser, &parser->typedef_types[i - 1],
                                        kind, count, depth + 1);
    }
  }
  if ((ty->kind != NY_CTYPE_STRUCT && ty->kind != NY_CTYPE_NAMED) ||
      !ty->aggregate_has_layout || !ty->aggregate_size || !ty->field_count)
    return false;
  ny_sysv_agg_class_t aggregate_kind = NY_SYSV_AGG_NONE;
  unsigned aggregate_count = 0;
  size_t expected_offset = 0;
  for (unsigned i = 0; i < ty->field_count; ++i) {
    const ny_c_field_t *field = &ty->fields[i];
    if (field->ptr_depth || field->kind == NY_CTYPE_UNION)
      return false;
    ny_sysv_agg_class_t field_kind = NY_SYSV_AGG_NONE;
    unsigned field_count = 0;
    size_t elem_size = 0;
    if (field->kind == NY_CTYPE_FLOAT || field->kind == NY_CTYPE_DOUBLE ||
        field->kind == NY_CTYPE_HALF) {
      field_kind = field->kind == NY_CTYPE_FLOAT  ? NY_SYSV_AGG_HFA_F32
                   : field->kind == NY_CTYPE_HALF ? NY_SYSV_AGG_HFA_F32
                                                  : NY_SYSV_AGG_HFA_F64;
      elem_size = field->kind == NY_CTYPE_FLOAT  ? 4u
                  : field->kind == NY_CTYPE_HALF ? 2u
                                                 : 8u;
      if (!field->size || field->size % elem_size)
        return false;
      field_count = (unsigned)(field->size / elem_size);
    } else if (field->kind == NY_CTYPE_STRUCT ||
               field->kind == NY_CTYPE_NAMED) {
      const ny_ctype_t *nested = ny_native_c_nested_type(parser, field);
      if (!nested || !ny_native_aapcs_hfa_type(parser, nested, &field_kind,
                                               &field_count, depth + 1))
        return false;
      elem_size = field_kind == NY_SYSV_AGG_HFA_F32 ? 4u : 8u;
      size_t nested_size = nested->aggregate_size;
      if (!nested_size || !field->size || field->size % nested_size)
        return false;
      size_t repeat = field->size / nested_size;
      if (!repeat || repeat > 4 || field_count > 4 / repeat)
        return false;
      field_count *= (unsigned)repeat;
    } else {
      return false;
    }
    if (!field_count || field_count > 4 ||
        (aggregate_kind != NY_SYSV_AGG_NONE && aggregate_kind != field_kind) ||
        field->offset != expected_offset)
      return false;
    aggregate_kind = field_kind;
    aggregate_count += field_count;
    if (aggregate_count > 4)
      return false;
    expected_offset += (size_t)field_count * elem_size;
  }
  if (aggregate_kind == NY_SYSV_AGG_NONE || !aggregate_count ||
      expected_offset != ty->aggregate_size)
    return false;
  *kind = aggregate_kind;
  *count = aggregate_count;
  return true;
}

static bool ny_native_aapcs_classify_aggregate(const ny_parser_t *parser,
                                               const ny_ctype_t *ty,
                                               ny_sysv_agg_class_t classes[2]) {
  classes[0] = NY_SYSV_AGG_NONE;
  classes[1] = NY_SYSV_AGG_NONE;
  if (!ty || !ty->aggregate_has_layout || !ty->aggregate_size)
    return false;
  ny_sysv_agg_class_t hfa = NY_SYSV_AGG_NONE;
  unsigned count = 0;
  if (ny_native_aapcs_hfa_type(parser, ty, &hfa, &count, 0) && count >= 1 &&
      count <= 4) {
    classes[0] = hfa;
    return true;
  }
  if (ty->aggregate_size > 16) {
    classes[0] = NY_SYSV_AGG_MEMORY;
    return true;
  }
  classes[0] = ty->aggregate_align > 8 ? NY_SYSV_AGG_AAPCS_INTEGER_A16
                                       : NY_SYSV_AGG_INTEGER;
  if (ty->aggregate_size > 8)
    classes[1] = NY_SYSV_AGG_INTEGER;
  return true;
}

static bool ny_extern_table_add(ny_extern_table_t *t, const char *ny_name,
                                const char *c_symbol, unsigned param_count,
                                bool owned, uint32_t ret_agg_size,
                                const ny_sysv_agg_class_t ret_agg_classes[2],
                                const uint32_t *arg_agg_sizes, bool ret_f64,
                                bool ret_f32, bool ret_signed_i64,
                                const bool *param_f64, const bool *param_f32) {
  if (!t || !ny_name || !c_symbol)
    return false;
  /*
   * Dedup: identical redeclarations are silently accepted.
   */
  for (size_t i = 0; i < t->count; ++i) {
    if (t->entries[i].ny_name && strcmp(t->entries[i].ny_name, ny_name) == 0) {
      if (t->entries[i].c_symbol &&
          strcmp(t->entries[i].c_symbol, c_symbol) == 0) {
        if (owned) {
          free((void *)ny_name);
          free((void *)c_symbol);
        }
        return true; /* exact duplicate — ok */
      }
      return false; /* conflicting extern: same NY name, different C symbol */
    }
  }
  if (t->count >= NY_EXTERN_MAX) {
    fprintf(stderr, "native NYIR lower: extern table full (%zu entries)\n",
            t->count);
    return false;
  }
  t->entries[t->count].ny_name = ny_name;
  t->entries[t->count].c_symbol = c_symbol;
  t->entries[t->count].param_count = param_count;
  t->entries[t->count].owned = owned;
  t->entries[t->count].ret_aggregate_size = ret_agg_size;
  t->entries[t->count].ret_aggregate_classes[0] =
      ret_agg_classes ? ret_agg_classes[0] : NY_SYSV_AGG_NONE;
  t->entries[t->count].ret_aggregate_classes[1] =
      ret_agg_classes ? ret_agg_classes[1] : NY_SYSV_AGG_NONE;
  memset(t->entries[t->count].arg_aggregate_sizes, 0,
         sizeof(t->entries[t->count].arg_aggregate_sizes));
  if (arg_agg_sizes && param_count > 0) {
    size_t n = param_count < NY_C_MAX_PARAMS ? param_count : NY_C_MAX_PARAMS;
    for (size_t k = 0; k < n; ++k)
      t->entries[t->count].arg_aggregate_sizes[k] = arg_agg_sizes[k];
  }
  t->entries[t->count].ret_f64 = ret_f64;
  t->entries[t->count].ret_f32 = ret_f32;
  t->entries[t->count].ret_signed_i64 = ret_signed_i64;
  if (param_f64)
    memcpy(t->entries[t->count].param_f64, param_f64,
           sizeof(t->entries[t->count].param_f64));
  if (param_f32)
    memcpy(t->entries[t->count].param_f32, param_f32,
           sizeof(t->entries[t->count].param_f32));
  t->count++;
  return true;
}

static const ny_extern_entry_t *
ny_extern_table_lookup(const ny_extern_table_t *t, const char *ny_name) {
  if (!t || !ny_name)
    return NULL;
  for (size_t i = 0; i < t->count; ++i) {
    if (t->entries[i].ny_name && strcmp(t->entries[i].ny_name, ny_name) == 0) {
      return &t->entries[i];
    }
  }
  /*
   * Extern declarations inside modules retain their qualified table key,
   * while calls in that module commonly use the declaration leaf.  The
   * target-filtered table has already removed inactive duplicates, so a
   * unique leaf fallback is both deterministic and ABI-safe.
   */
  const ny_extern_entry_t *leaf_match = NULL;
  for (size_t i = 0; i < t->count; ++i) {
    const char *decl = t->entries[i].ny_name;
    const char *dot = decl ? strrchr(decl, '.') : NULL;
    if (!dot || strcmp(dot + 1, ny_name) != 0)
      continue;
    if (leaf_match)
      return NULL;
    leaf_match = &t->entries[i];
  }
  if (leaf_match)
    return leaf_match;
  return NULL;
}

typedef struct {
  const program_t *prog;
  const char *name;
  const char *source_file;
  const char *module_name;
  bool result;
} ny_native_user_fn_cache_entry_t;
#define NY_USER_FN_CACHE_SIZE 512

typedef struct {
  const program_t *prog;
  const char *source_file;
  const char *module_name;
  const char *name;
  const expr_t *result;
  bool resolved;
} ny_top_val_cache_entry_t;
#define NY_TOP_VAL_CACHE_SIZE 2048

typedef struct {
  const char *raw;
  const char *canon;
} ny_use_canon_ent_t;

typedef struct {
  const stmt_t *fn;
  size_t index;
  bool value;
} ny_try_dynamic_cache_ent_t;

#define NY_ALIAS_CACHE_SIZE 16384
typedef struct {
  const char *source_file;
  const char *module_name;
  const char *alias;
  const char *result;
  bool resolved;
  bool is_null;
} ny_resolve_alias_cache_entry_t;

typedef struct {
  const stmt_t *root;
  ny_sym_id name_id;
  const expr_t *result;
  bool resolved;
} ny_top_stmt_cache_entry_t;

enum { NY_DYNLIST_CACHE_MAX = 8192, NY_DYNLIST_NAME = 512 };
typedef struct {
  char name[NY_DYNLIST_NAME];
  unsigned param;
  bool dyn_list;
} ny_dynlist_cache_ent_t;

#define NY_TOP_MUT_FACTS_MAX 512
typedef struct {
  const program_t *prog;
  ny_sym_id name_id;
  bool result;
} ny_top_mut_fact_t;

typedef struct ny_top_mut_int_visit {
  const char *name;
  const struct ny_top_mut_int_visit *parent;
} ny_top_mut_int_visit_t;

#define NY_TLV_BUCKETS 65536
typedef struct {
  char *name;
  const expr_t *val;
  const char *file;
  const stmt_t *root;
  unsigned depth;
  bool qualified;
  bool is_mut;
  int32_t next;
} ny_tlv_ent_t;

#define NY_PROG_QUERY_CACHE_SIZE 32768
typedef struct {
  const program_t *prog;
  char name[96];
  char src[96];
  const stmt_t *result;
  uint8_t kind;
  bool resolved;
} ny_prog_query_entry_t;

#define NY_IMPORTED_VAL_CACHE_SIZE 16384
typedef struct {
  const program_t *prog;
  char module_name[128];
  char source_file[512];
  char name[128];
  const expr_t *result;
  bool resolved;
} ny_imported_val_cache_entry_t;

#define NY_LAYOUT_CACHE_SIZE 16384
typedef struct {
  const program_t *prog;
  const stmt_t *root;
  char name[128];
  const stmt_t *result;
  bool resolved;
} ny_layout_cache_entry_t;

typedef struct {
  const char *key;
  const stmt_t *stmt;
  size_t source_order;
} ny_layout_name_index_entry_t;

typedef struct {
  const char *key;
  const char *source_file;
  const stmt_t *stmt;
  size_t source_order;
} ny_top_var_index_entry_t;

typedef struct {
  const char *key;
  const stmt_t *stmt;
  size_t source_order;
} ny_extern_index_entry_t;

#define NY_REFINE_CACHE_SIZE 16384
typedef struct {
  const program_t *prog;
  const stmt_t *root;
  char name[128];
  const stmt_t *result;
  bool resolved;
} ny_refine_cache_entry_t;

#define NY_FN_RESOLVE_HASH_SIZE 131072
typedef struct {
  ny_sym_id source_id;
  ny_sym_id module_id;
  char name[256];
  const stmt_t *fn;
  uint32_t generation;
  bool resolved;
} ny_native_fn_resolve_cache_entry_t;

#define NY_FN_CONTEXT_ID_CACHE_SIZE 4096u
typedef struct {
  const char *source_file;
  const char *module_name;
  ny_sym_id source_id;
  ny_sym_id module_id;
  uint32_t generation;
} ny_native_fn_context_id_cache_entry_t;

#define NY_SCALAR_PROBE_CACHE_SIZE 4096
typedef struct {
  const expr_t *expr;
  uint32_t generation;
  uint8_t kind;
  uint8_t depth;
  bool value;
} ny_native_scalar_probe_cache_entry_t;

#define NY_CSTR_QUERY_CACHE_SIZE 8192u
typedef struct {
  const expr_t *expr;
  uint32_t generation;
  uint8_t kind;
  uint8_t depth;
  bool value;
} ny_native_cstr_query_cache_entry_t;

typedef struct {
  const expr_t *expr;
  const struct nyr_builder_impl_t *scope;
  const char *type_name;
  uint64_t generation;
} ny_native_type_context_entry_t;

typedef struct ny_native_fn_index_t ny_native_fn_index_t;

struct nyr_builder_impl_t {
  nyr_func_t nyir;
  nyr_local_t *locals;
  size_t local_count;
  size_t local_cap;
  int next_local_slot;
  int next_label;
  int last_value;
  bool last_value_raw_scalar;
  bool last_value_f64;
  size_t scope_depth;
  int resolve_depth;
  nyr_fact_t *facts;
  size_t fact_count;
  size_t fact_cap;
  nyr_loop_frame_t *loop_frames;
  size_t loop_depth;
  size_t loop_cap;
  nyr_label_t *labels;
  size_t label_count;
  size_t label_cap;
  stmt_t **defers;
  size_t defer_count;
  size_t defer_cap;
  /*
   * During try lowering, retain lexical defers so a longjmp catch edge can
   * replay them after the normal linear path was skipped.
   */
  bool capture_defers;
  size_t defer_capture_mark;
  bool emitted_return;
  /*
   * Function return type is carried on NYIR_RET so type constraints survive
   * control-flow joins that contain no floating-point arithmetic themselves.
   */
  nyr_scalar_class_t return_class;
  /*
   * Any-return functions use a tagged incoming ABI but expose raw scalar
   * payloads to native NYIR callers.
   */
  bool return_any;
  /*
   * Set while lowering a `case` expression whose result feeds a dynamic
   * consumer: raw scalar arm values must be boxed once at the join.
   */
  bool match_result_any;
  const char *return_type;
  const ny_extern_table_t *externs;
  const program_t *prog;
  arena_t default_arg_arena;
  const ny_options *options;
  const char *profile_name;
  const char *current_fn_name;
  const char *source_file;
  const char *source_main_file;
  const char *module_name;
  const ny_native_lambda_entry_t *lambda_entry;
  ny_native_lambda_cache_t *lambda_cache;
  ny_native_fn_index_t *fn_index;
  int closure_env_slot;
  /*
   * C aggregate layouts discovered while building the native extern table.
   */
  const struct ny_native_c_layout_table_t *c_layouts;
  int64_t current_list_elem_size;
  bool current_list_elem_f64;
  bool force_dynamic_list;
  bool force_dynamic_get_any;
  /*
   * Preserve an enclosing floating-point context through untyped arithmetic
   * subexpressions such as `x * y * 1.0`.
   */
  unsigned f64_context_depth;
  /*
   * Emit runtime source frames for native --trace/--debug execution.
   */
  bool trace_instrumented;
  /*
   * A direct @thread statement call is detached; value calls are joined.
   */
  bool thread_detach_stmt_call;
  /*
   * When lowering an argument to an `any` callback, sequence indexing must
   * use the representation-aware dynamic accessor.  Typed native list reads
   * normally stay raw for arithmetic and stores.
   */
  bool index_for_any_call;
  /*
   * A raw native producer is being lowered for a dynamic formal.  Runtime
   * primitives with both raw and canonical accessors use this context to
   * preserve the language value representation at the call boundary.
   */
  bool value_for_any_call;
  /*
   * Representation fact for the integer binary expression lowered most
   * recently: 1 when its NYIR result is a raw i64 payload, 0 when the result
   * is tagged or no integer binary was lowered.  Dynamic destinations (any
   * locals, any formals) must box a raw payload once at their boundary;
   * typing stamps dynamic arithmetic TAGGED_DYNAMIC while the tail emitter
   * deliberately keeps it raw, so the store/arg boundary needs this fact
   * instead of a second syntactic guess.
   */
  int last_arith_raw;
  /*
   * True only when the latest binary operator deliberately returned a
   * canonical tagged dynamic value.  This lets inferred locals follow the
   * selected operation ABI when stale float context suggested a scalar.
   */
  bool last_arith_tagged;
  /*
   * The expression whose representation `last_arith_raw` describes.  The
   * boundary checks compare pointers so a const-folded initializer that
   * never reached the binary emitter cannot inherit a stale fact.
   */
  const expr_t *last_arith_expr;
  /*
   * Bounded self-tail recursion lowering.  These fields are enabled only for
   * scalar-parameter functions; aggregate ABI values remain ordinary calls.
   */
  int tail_loop_label;
  int *tail_param_slots;
  size_t tail_param_count;
  const stmt_t *tail_body;
  int tail_recur_enabled;
  int opt_level;
  uint32_t current_syntax_ctx;
  /*
   * Splice operands must not fall back to a binding with a different mark.
   */
  bool exact_syntax_ctx;
  /*
   * Active expression queries used by overload/type discovery.  Operator
   * lookup asks for operand types, while binary type discovery asks for the
   * selected operator; keep that dependency graph cycle-safe per builder.
   */
  const expr_t **type_query_stack;
  size_t type_query_depth;
  size_t type_query_cap;
  ny_native_type_query_cache_entry_t *type_query_cache;
  ny_native_user_fn_cache_entry_t *user_fn_cache;
  ny_top_val_cache_entry_t *top_val_cache;
  ny_use_canon_ent_t *use_canon_cache;
  size_t use_canon_count;
  size_t use_canon_cap;
  ny_try_dynamic_cache_ent_t *try_dynamic_cache;
  size_t try_dynamic_cache_cap;
  ny_resolve_alias_cache_entry_t *resolve_alias_cache;
  size_t resolve_alias_cache_cap;
  size_t resolve_alias_cache_len;
  /*
   * Opt-in compile-wide counters for the expensive import-alias resolver.
   */
  bool resolve_alias_stats_enabled;
  uint64_t resolve_alias_queries;
  uint64_t resolve_alias_local_shadows;
  uint64_t resolve_alias_prefilter_rejects;
  uint64_t resolve_alias_cache_hits;
  uint64_t resolve_alias_cache_misses;
  uint64_t resolve_alias_uncached_scans;
  /*
   * Opt-in compile-wide counters for layout-name memoization.
   */
  bool layout_stats_enabled;
  uint64_t layout_queries;
  uint64_t layout_cache_hits;
  uint64_t layout_cache_misses;
  uint64_t layout_uncached_scans;
  uint64_t layout_prog_queries;
  uint64_t layout_prog_cache_hits;
  uint64_t layout_prog_cache_misses;
  uint64_t layout_prog_scans;
  uint64_t layout_prog_index_lookups;
  ny_layout_name_index_entry_t *layout_name_index;
  size_t layout_name_index_len;
  const program_t *layout_name_index_prog;
  uint8_t layout_name_index_state;
  ny_top_var_index_entry_t *top_var_index;
  size_t top_var_index_len;
  const program_t *top_var_index_prog;
  uint8_t top_var_index_state;
  ny_extern_index_entry_t *extern_index;
  size_t extern_index_len;
  const program_t *extern_index_prog;
  const ny_options *extern_index_opt;
  uint8_t extern_index_state;
  const char **resolve_alias_name_slots;
  size_t resolve_alias_name_cap;
  size_t resolve_alias_name_len;
  bool resolve_alias_name_index_ready;
  bool resolve_alias_name_index_valid;
  ny_native_fn_resolve_cache_entry_t *fn_resolve_cache;
  size_t fn_resolve_cache_cap;
  ny_top_stmt_cache_entry_t *top_stmt_cache;
  size_t top_stmt_cache_cap;
  ny_prog_query_entry_t *prog_query_cache;
  size_t prog_query_cache_cap;
  bool prog_query_stats_initialized;
  bool prog_query_stats_enabled;
  uint64_t prog_query_queries;
  uint64_t prog_query_hits;
  uint64_t prog_query_misses;
  uint64_t prog_query_evictions;
  ny_imported_val_cache_entry_t *imported_val_cache;
  size_t imported_val_cache_cap;
  size_t imported_val_cache_len;
  ny_layout_cache_entry_t *layout_cache;
  size_t layout_cache_cap;
  ny_refine_cache_entry_t *refine_cache;
  size_t refine_cache_cap;
  ny_dynlist_cache_ent_t *dynlist_cache;
  size_t dynlist_cache_len;
  size_t dynlist_cache_cap;
  size_t *dynlist_name_slots;
  size_t dynlist_name_slot_cap;
  ny_top_mut_fact_t *top_mut_facts;
  size_t top_mut_facts_len;
  size_t top_mut_facts_cap;
  const char *top_mut_active_name;
  const ny_top_mut_int_visit_t *top_mut_int_active;
  ny_tlv_ent_t *tlv_ents;
  size_t tlv_len;
  size_t tlv_cap;
  int32_t *tlv_head;
  int32_t *tlv_tail;
  const program_t *tlv_prog;
  bool tlv_complete;
  ny_attached_cache_entry_t *attached_cache;
  size_t attached_cache_count;
  size_t attached_cache_cap;
  ny_operator_cache_entry_t *operator_cache;
  size_t operator_cache_count;
  size_t operator_cache_cap;
  ny_operator_pair_cache_entry_t *operator_pair_cache;
  size_t operator_pair_cache_count;
  size_t operator_pair_cache_cap;
  struct nyr_builder_impl_t *cache_owner;
  ny_native_scalar_probe_cache_entry_t
      scalar_probe_cache[NY_SCALAR_PROBE_CACHE_SIZE];
  uint32_t scalar_probe_generation;
  size_t scalar_probe_active;
  ny_native_cstr_query_cache_entry_t cstr_query_cache[NY_CSTR_QUERY_CACHE_SIZE];
  uint32_t cstr_query_generation;
  size_t type_query_cache_count;
  size_t type_query_cache_cap;
  size_t *type_query_hash;
  size_t type_query_hash_cap;
  ny_native_type_context_entry_t *type_query_context_cache;
  size_t type_query_context_count;
  size_t type_query_context_cap;
  size_t *type_query_context_hash;
  size_t type_query_context_hash_cap;
  uint64_t type_query_context_generation;
  size_t transient_query_depth;
  /*
   * Builders that delegated the query at each transient depth.  A delegated
   * subtree is resolved against the cache owner, which has no lexical
   * locals; identifier nodes inside that subtree (for example the receiver
   * of an indexed `list<f64>` read) must still consult the delegating
   * builder's local table or their element type collapses to `any`.
   */
  const struct nyr_builder_impl_t *delegate_sources[16];
  char **owned_strings;
  char **owned_string_slots;
  size_t owned_string_slot_cap;
  size_t owned_string_count;
  size_t owned_string_cap;
  char *err;
  size_t err_len;
};

static ny_native_lambda_cache_t *
ny_native_lambda_cache_for(const nyr_builder_impl_t *builder) {
  nyr_builder_impl_t *owner =
      builder ? (builder->cache_owner ? builder->cache_owner
                                      : (nyr_builder_t *)builder)
              : NULL;
  if (!owner)
    return NULL;
  if (!owner->lambda_cache)
    owner->lambda_cache = calloc(1, sizeof(*owner->lambda_cache));
  return owner->lambda_cache;
}

static ny_native_fn_index_t *
ny_native_fn_index_for(const nyr_builder_impl_t *builder);
static uint32_t
ny_native_fn_index_generation(const nyr_builder_impl_t *builder);

static inline uint32_t ny_resolve_hash(const char *name, ny_sym_id source_id) {
  uint32_t h = 2166136261u;
  for (const char *p = name; *p; ++p)
    h = (h ^ (uint8_t)*p) * 16777619u;
  h ^= source_id;
  h *= 16777619u;
  return h;
}

static const stmt_t *
ny_native_fn_resolve_cache_get(const nyr_builder_impl_t *builder,
                               const char *name, const char *source_file,
                               bool *known) {
  if (known)
    *known = false;
  if (!name || !*name)
    return NULL;
  const nyr_builder_impl_t *owner =
      builder && builder->cache_owner ? builder->cache_owner : builder;
  if (!owner || !owner->fn_resolve_cache)
    return NULL;
  uint32_t generation = ny_native_fn_index_generation(builder);
  ny_sym_id source_id = ny_intern_cstr(source_file);
  ny_sym_id module_id = ny_intern_cstr(builder->module_name);
  uint32_t slot =
      (ny_resolve_hash(name, source_id) ^ (module_id * UINT32_C(0x9e3779b1))) &
      (NY_FN_RESOLVE_HASH_SIZE - 1);
  for (size_t probe = 0; probe < 16; ++probe) {
    ny_native_fn_resolve_cache_entry_t *entry =
        &owner
             ->fn_resolve_cache[(slot + probe) & (NY_FN_RESOLVE_HASH_SIZE - 1)];
    if (!entry->resolved)
      return NULL;
    if (entry->generation == generation && entry->source_id == source_id &&
        entry->module_id == module_id && entry->name[0] == name[0] &&
        strcmp(entry->name, name) == 0) {
      if (known)
        *known = true;
      return entry->fn;
    }
  }
  return NULL;
}

static void ny_native_fn_resolve_cache_put(const nyr_builder_impl_t *builder,
                                           const char *name,
                                           const char *source_file,
                                           const stmt_t *fn) {
  if (!builder || !name || !*name || strlen(name) >= 256)
    return;
  nyr_builder_impl_t *owner =
      builder->cache_owner ? builder->cache_owner : (nyr_builder_t *)builder;
  owner->cache_owner = owner;
  if (!owner->fn_resolve_cache) {
    owner->fn_resolve_cache =
        calloc(NY_FN_RESOLVE_HASH_SIZE, sizeof(*owner->fn_resolve_cache));
    if (owner->fn_resolve_cache)
      owner->fn_resolve_cache_cap = NY_FN_RESOLVE_HASH_SIZE;
  }
  if (!owner->fn_resolve_cache)
    return;
  uint32_t generation = ny_native_fn_index_generation(builder);
  ny_sym_id source_id = ny_intern_cstr(source_file);
  ny_sym_id module_id = ny_intern_cstr(builder->module_name);
  uint32_t slot =
      (ny_resolve_hash(name, source_id) ^ (module_id * UINT32_C(0x9e3779b1))) &
      (NY_FN_RESOLVE_HASH_SIZE - 1);
  for (size_t probe = 0; probe < 16; ++probe) {
    ny_native_fn_resolve_cache_entry_t *entry =
        &owner
             ->fn_resolve_cache[(slot + probe) & (NY_FN_RESOLVE_HASH_SIZE - 1)];
    if (!entry->resolved || entry->generation != generation ||
        (entry->source_id == source_id && entry->module_id == module_id &&
         strcmp(entry->name, name) == 0)) {
      entry->source_id = source_id;
      entry->module_id = module_id;
      snprintf(entry->name, sizeof(entry->name), "%s", name);
      entry->fn = fn;
      entry->generation = generation;
      entry->resolved = true;
      return;
    }
  }
}

static const stmt_t *
ny_native_fn_resolve_cache_put_both(const nyr_builder_impl_t *builder,
                                    const char *name, const char *lookup_name,
                                    const stmt_t *fn) {
  if (builder) {
    if (lookup_name)
      ny_native_fn_resolve_cache_put(builder, lookup_name, builder->source_file,
                                     fn);
    if (name && (!lookup_name || strcmp(name, lookup_name) != 0))
      ny_native_fn_resolve_cache_put(builder, name, builder->source_file, fn);
  }
  return fn;
}

/*
 * Keep dynamically assembled lookup names alive for the whole lowering
 * builder, without process-global rings whose slots can be overwritten.
 */
static bool ny_native_owned_string_index_grow(nyr_builder_impl_t *b,
                                              size_t capacity) {
  if (capacity < 16 || (capacity & (capacity - 1)) != 0 ||
      capacity > SIZE_MAX / sizeof(*b->owned_string_slots))
    return false;
  char **slots = calloc(capacity, sizeof(*slots));
  if (!slots)
    return false;
  for (size_t i = 0; i < b->owned_string_count; ++i) {
    char *text = b->owned_strings[i];
    size_t at = ny_resolve_hash(text, 0) & (capacity - 1);
    while (slots[at])
      at = (at + 1) & (capacity - 1);
    slots[at] = text;
  }
  free(b->owned_string_slots);
  b->owned_string_slots = slots;
  b->owned_string_slot_cap = capacity;
  return true;
}

static const char *nyr_builder_own_string(const nyr_builder_impl_t *builder,
                                          const char *text) {
  if (!text)
    return NULL;
  if (!builder)
    return NULL;
  nyr_builder_impl_t *b = (nyr_builder_t *)builder;
  /*
   * Transient reachability probes share their compilation's memo owner so
   * per-probe callocs/owned strings do not accumulate unbounded RSS across
   * whole-stdlib scans (std.bc grew past 27 GiB at ~2 GiB/min).
   */
  if (b->cache_owner && b->cache_owner != b)
    return nyr_builder_own_string(b->cache_owner, text);
  if (!b->owned_string_slot_cap ||
      b->owned_string_count >= b->owned_string_slot_cap / 2) {
    size_t cap = b->owned_string_slot_cap ? b->owned_string_slot_cap * 2 : 64;
    if (cap < b->owned_string_slot_cap ||
        !ny_native_owned_string_index_grow(b, cap)) {
      /*
       * Keep lookup and stable-pointer interning correct after allocation
       * failure; the owned-string array remains the authoritative storage.
       */
      free(b->owned_string_slots);
      b->owned_string_slots = NULL;
      b->owned_string_slot_cap = 0;
    }
  }
  size_t slot = 0;
  if (b->owned_string_slot_cap) {
    slot = ny_resolve_hash(text, 0) & (b->owned_string_slot_cap - 1);
    while (b->owned_string_slots[slot]) {
      if (strcmp(b->owned_string_slots[slot], text) == 0)
        return b->owned_string_slots[slot];
      slot = (slot + 1) & (b->owned_string_slot_cap - 1);
    }
  } else {
    for (size_t i = 0; i < b->owned_string_count; ++i)
      if (strcmp(b->owned_strings[i], text) == 0)
        return b->owned_strings[i];
  }
  size_t len = strlen(text);
  if (len == SIZE_MAX)
    return NULL;
  char *copy = malloc(len + 1);
  if (!copy)
    return NULL;
  memcpy(copy, text, len + 1);
  if (b->owned_string_count == b->owned_string_cap) {
    size_t cap = b->owned_string_cap ? b->owned_string_cap * 2 : 16;
    if (cap < b->owned_string_cap ||
        cap > SIZE_MAX / sizeof(*b->owned_strings)) {
      free(copy);
      return NULL;
    }
    char **grown = realloc(b->owned_strings, cap * sizeof(*grown));
    if (!grown) {
      free(copy);
      return NULL;
    }
    b->owned_strings = grown;
    b->owned_string_cap = cap;
  }
  b->owned_strings[b->owned_string_count++] = copy;
  if (b->owned_string_slot_cap)
    b->owned_string_slots[slot] = copy;
  return copy;
}

static const char *nyr_builder_own_substring(const nyr_builder_impl_t *builder,
                                             const char *text, size_t len) {
  if (!builder || !text || len == SIZE_MAX)
    return NULL;
  char *copy = malloc(len + 1);
  if (!copy)
    return NULL;
  memcpy(copy, text, len);
  copy[len] = '\0';
  const char *owned = nyr_builder_own_string(builder, copy);
  free(copy);
  return owned;
}

static const char *nyr_builder_own_qualified_prefix(const nyr_builder_impl_t *b,
                                                    const char *prefix,
                                                    size_t prefix_len,
                                                    const char *suffix) {
  if (!prefix || !suffix)
    return NULL;
  size_t suffix_len = strlen(suffix);
  if (suffix_len > SIZE_MAX - 2 || prefix_len > SIZE_MAX - suffix_len - 2)
    return NULL;
  size_t len = prefix_len + 1 + suffix_len;
  char *joined = malloc(len + 1);
  if (!joined)
    return NULL;
  memcpy(joined, prefix, prefix_len);
  joined[prefix_len] = '.';
  memcpy(joined + prefix_len + 1, suffix, suffix_len + 1);
  const char *owned = nyr_builder_own_string(b, joined);
  free(joined);
  return owned;
}

static const char *nyr_builder_own_qualified(const nyr_builder_impl_t *b,
                                             const char *prefix,
                                             const char *suffix) {
  return prefix ? nyr_builder_own_qualified_prefix(b, prefix, strlen(prefix),
                                                   suffix)
                : NULL;
}

static bool nyr_fn_has_thread_attr(const stmt_t *fn) {
  if (!fn || fn->kind != NY_S_FUNC)
    return false;
  if (fn->as.fn.attr_thread)
    return true;
  for (size_t i = 0; i < fn->attributes.len; ++i)
    if (fn->attributes.data[i].name &&
        strcmp(fn->attributes.data[i].name, "thread") == 0)
      return true;
  return false;
}

/*
 * Parse a value-indexed type name and extract constructor and bound.
 * Handles "Ctor<42>" (literal); symbolic bounds are resolved via
 * ny_native_resolve_indexed_bound.
 */
static int64_t ny_native_parse_indexed_bound(const char *type_name,
                                             char *out_ctor, size_t ctor_cap) {
  if (!type_name)
    return -1;
  const char *start = strchr(type_name, '<');
  if (!start || start == type_name)
    return -1;
  const char *end = strrchr(start, '>');
  if (!end || end == start + 1)
    return -1;
  char *check = NULL;
  long long val = strtoll(start + 1, &check, 10);
  if (check != end || val < 0)
    return -1;
  if (out_ctor && ctor_cap > 0) {
    size_t ctor_len = (size_t)(start - type_name);
    if (ctor_len >= ctor_cap)
      ctor_len = ctor_cap - 1;
    memcpy(out_ctor, type_name, ctor_len);
    out_ctor[ctor_len] = '\0';
  }
  return (int64_t)val;
}

static int64_t ny_native_parse_fin_bound(const char *type_name) {
  char ctor[64] = {0};
  int64_t val = ny_native_parse_indexed_bound(type_name, ctor, sizeof(ctor));
  if (val < 0)
    return -1;
  const char *leaf = strrchr(ctor, '.');
  leaf = leaf ? leaf + 1 : ctor;
  return strcmp(leaf, "Fin") == 0 ? val : -1;
}

/*
 * Scan one statement tree for top-level `def NAME = ...` facts. A mutable
 * declaration, reassignment, or non-literal/non-ident definition of NAME
 * poisons the lookup: bounds-check elision must never use a stale bound.
 * An immutable ident definition (`def M = N`) records a chain link.
 */
static void ny_native_scan_fin_def(const stmt_t *s, const char *name,
                                   size_t name_len, int64_t *out_value,
                                   char *out_chain, size_t chain_cap,
                                   bool *out_poisoned, unsigned depth) {
  if (!s || !name || !out_value || !out_chain || !out_poisoned || depth > 16 ||
      *out_poisoned)
    return;
  if (s->kind == NY_S_VAR) {
    for (size_t n = 0; n < s->as.var.names.len && n < s->as.var.exprs.len;
         ++n) {
      const char *decl = s->as.var.names.data[n];
      if (!decl || strlen(decl) != name_len ||
          memcmp(decl, name, name_len) != 0)
        continue;
      const expr_t *init =
          n < s->as.var.exprs.len ? s->as.var.exprs.data[n] : NULL;
      if (!s->as.var.is_decl || s->as.var.is_mut || !init) {
        *out_poisoned = true;
        return;
      }
      if (init->kind == NY_E_LITERAL && init->as.literal.kind == NY_LIT_INT &&
          init->tok.kind != NY_T_NIL && init->as.literal.as.i > 0) {
        if (*out_value < 0)
          *out_value = init->as.literal.as.i;
        else if (*out_value != init->as.literal.as.i)
          *out_poisoned = true;
        return;
      }
      if (init->kind == NY_E_IDENT && init->as.ident.name) {
        size_t link_len = strlen(init->as.ident.name);
        if (out_chain[0] != '\0' || link_len == 0 || link_len >= chain_cap) {
          *out_poisoned = true;
          return;
        }
        memcpy(out_chain, init->as.ident.name, link_len + 1);
        return;
      }
      *out_poisoned = true;
      return;
    }
    return;
  }
  if (s->kind == NY_S_MODULE) {
    for (size_t i = 0; i < s->as.module.body.len; ++i)
      ny_native_scan_fin_def(s->as.module.body.data[i], name, name_len,
                             out_value, out_chain, chain_cap, out_poisoned,
                             depth + 1);
    return;
  }
  if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      ny_native_scan_fin_def(s->as.block.body.data[i], name, name_len,
                             out_value, out_chain, chain_cap, out_poisoned,
                             depth + 1);
    return;
  }
}

/*
 * Resolve a bare bound symbol through immutable top-level defs.
 */
static int64_t ny_native_resolve_fin_sym_depth(const nyr_builder_impl_t *b,
                                               const char *sym,
                                               unsigned depth) {
  if (!b || !b->prog || !sym || !*sym || depth > 8)
    return -1;
  size_t name_len = strlen(sym);
  if (name_len == 0 || name_len >= 256)
    return -1;
  int64_t value = -1;
  char chain[256] = {0};
  bool poisoned = false;
  for (size_t i = 0; i < b->prog->body.len && !poisoned; ++i)
    ny_native_scan_fin_def(b->prog->body.data[i], sym, name_len, &value, chain,
                           sizeof(chain), &poisoned, 0);
  if (poisoned)
    return -1;
  if (value > 0)
    return value;
  if (chain[0] != '\0')
    return ny_native_resolve_fin_sym_depth(b, chain, depth + 1);
  return -1;
}

/*
 * Resolve a value-indexed bound (Ctor<N>) to a positive integer:
 * `Ctor<42>` resolves directly; `Ctor<NAME>` resolves through an immutable
 * top-level `def NAME = <positive int>`. Returns -1 when the bound is not
 * compile-time known.
 */
static int64_t ny_native_resolve_indexed_bound(const nyr_builder_impl_t *b,
                                               const char *type_name,
                                               char *out_ctor,
                                               size_t ctor_cap) {
  int64_t lit = ny_native_parse_indexed_bound(type_name, out_ctor, ctor_cap);
  if (lit >= 0)
    return lit;
  if (!b || !type_name)
    return -1;
  const char *lt = strchr(type_name, '<');
  if (!lt || lt == type_name)
    return -1;
  const char *gt = strrchr(lt, '>');
  if (!gt || gt == lt + 1)
    return -1;
  if (out_ctor && ctor_cap > 0) {
    size_t clen = (size_t)(lt - type_name);
    if (clen >= ctor_cap)
      clen = ctor_cap - 1;
    memcpy(out_ctor, type_name, clen);
    out_ctor[clen] = '\0';
  }
  const char *start = lt + 1;
  const char *end = gt;
  while (start < end && (*start == ' ' || *start == '\t'))
    ++start;
  while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
    --end;
  size_t name_len = (size_t)(end - start);
  if (name_len == 0 || name_len >= 256)
    return -1;
  char first = start[0];
  if (!(first == '_' || (first >= 'A' && first <= 'Z') ||
        (first >= 'a' && first <= 'z')))
    return -1;
  for (size_t i = 1; i < name_len; ++i) {
    char c = start[i];
    if (!(c == '_' || c == '.' || (c >= '0' && c <= '9') ||
          (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')))
      return -1;
  }
  char sym[256];
  memcpy(sym, start, name_len);
  sym[name_len] = '\0';
  return ny_native_resolve_fin_sym_depth(b, sym, 0);
}

static int64_t ny_native_resolve_fin_bound(const nyr_builder_impl_t *b,
                                           const char *type_name) {
  int64_t lit = ny_native_parse_fin_bound(type_name);
  if (lit >= 0)
    return lit;
  char ctor[64] = {0};
  int64_t bound =
      ny_native_resolve_indexed_bound(b, type_name, ctor, sizeof(ctor));
  if (bound < 0)
    return -1;
  const char *leaf = strrchr(ctor, '.');
  leaf = leaf ? leaf + 1 : ctor;
  return strcmp(leaf, "Fin") == 0 ? bound : -1;
}

/*
 * Check whether an intrinsic call's index operand has a Fin type whose
 * bound allows bounds-check elision.  buffer_byte_len must be > 0 (pass
 * the known allocation / tbuf capacity in bytes; 0 = unknown).
 * Checks both function parameter types and local variable type annotations.
 */
static nyr_local_t *nyr_find_local(nyr_builder_impl_t *b, const char *name);

static int64_t nyr_fin_bound_for_name(const nyr_builder_impl_t *b,
                                      const char *name, unsigned depth) {
  if (!b || !name || !name[0] || depth > 16)
    return 0;
  nyr_local_t *local = nyr_find_local((nyr_builder_t *)b, name);
  if (local && local->fin_bound > 0)
    return local->fin_bound;
  if (!b->prog)
    return 0;
  for (size_t i = 0; i < b->prog->body.len; ++i) {
    const stmt_t *s = b->prog->body.data[i];
    if (!s)
      continue;
    if (strcmp(b->current_fn_name ? b->current_fn_name : "", "rt_main") == 0 &&
        s->kind == NY_S_VAR) {
      for (size_t vi = 0; vi < s->as.var.names.len; ++vi) {
        if (!s->as.var.names.data[vi] ||
            strcmp(s->as.var.names.data[vi], name) != 0)
          continue;
        if (vi < s->as.var.types.len) {
          int64_t bound =
              ny_native_resolve_fin_bound(b, s->as.var.types.data[vi]);
          if (bound > 0)
            return bound;
        }
        if (vi < s->as.var.exprs.len && s->as.var.exprs.data[vi] &&
            s->as.var.exprs.data[vi]->kind == NY_E_IDENT)
          return nyr_fin_bound_for_name(
              b, s->as.var.exprs.data[vi]->as.ident.name, depth + 1);
      }
    }
    if (s->kind != NY_S_FUNC || !s->as.fn.name ||
        strcmp(s->as.fn.name, b->current_fn_name ? b->current_fn_name : "") !=
            0)
      continue;
    for (size_t pi = 0; pi < s->as.fn.params.len; ++pi) {
      if (s->as.fn.params.data[pi].name &&
          strcmp(s->as.fn.params.data[pi].name, name) == 0)
        return ny_native_resolve_fin_bound(b, s->as.fn.params.data[pi].type);
    }
    if (!s->as.fn.body || s->as.fn.body->kind != NY_S_BLOCK)
      continue;
    for (size_t si = 0; si < s->as.fn.body->as.block.body.len; ++si) {
      const stmt_t *vs = s->as.fn.body->as.block.body.data[si];
      if (!vs || vs->kind != NY_S_VAR)
        continue;
      for (size_t vi = 0; vi < vs->as.var.names.len; ++vi) {
        if (!vs->as.var.names.data[vi] ||
            strcmp(vs->as.var.names.data[vi], name) != 0)
          continue;
        if (vi < vs->as.var.types.len) {
          int64_t bound =
              ny_native_resolve_fin_bound(b, vs->as.var.types.data[vi]);
          if (bound > 0)
            return bound;
        }
        if (vi < vs->as.var.exprs.len && vs->as.var.exprs.data[vi] &&
            vs->as.var.exprs.data[vi]->kind == NY_E_IDENT)
          return nyr_fin_bound_for_name(
              b, vs->as.var.exprs.data[vi]->as.ident.name, depth + 1);
      }
    }
  }
  return 0;
}

static bool nyr_index_fin_bound_elision(const nyr_builder_impl_t *b,
                                        const expr_t *e, size_t index_arg_pos,
                                        int64_t buffer_byte_len) {
  if (!b || !b->current_fn_name || !e || buffer_byte_len <= 0 ||
      index_arg_pos >= e->as.call.args.len)
    return false;
  const expr_t *idx_expr = e->as.call.args.data[index_arg_pos].val;
  if (!idx_expr)
    return false;
  int64_t scale = 1;
  const char *idx_name = NULL;
  if (idx_expr->kind == NY_E_IDENT) {
    idx_name = idx_expr->as.ident.name;
  } else if (idx_expr->kind == NY_E_BINARY && idx_expr->as.binary.op &&
             strcmp(idx_expr->as.binary.op, "*") == 0) {
    const expr_t *l = idx_expr->as.binary.left;
    const expr_t *r = idx_expr->as.binary.right;
    if (l && l->kind == NY_E_IDENT && r && r->kind == NY_E_LITERAL &&
        r->as.literal.kind == NY_LIT_INT && r->as.literal.as.i > 0) {
      idx_name = l->as.ident.name;
      scale = r->as.literal.as.i;
    } else if (r && r->kind == NY_E_IDENT && l && l->kind == NY_E_LITERAL &&
               l->as.literal.kind == NY_LIT_INT && l->as.literal.as.i > 0) {
      idx_name = r->as.ident.name;
      scale = l->as.literal.as.i;
    }
  }
  int64_t fin_bound = nyr_fin_bound_for_name(b, idx_name, 0);
  return fin_bound > 0 && fin_bound <= INT64_MAX / scale &&
         fin_bound * scale <= buffer_byte_len;
}

/*
 * Resolve the comptime buffer byte length from the first argument of a
 * tbuf intrinsic call (__load64_idx, f64buf_load, etc.).  Returns 0 if
 * the pointer argument isn't a named local with known buffer_byte_len.
 */
static int64_t nyr_resolve_buf_byte_len(const nyr_builder_impl_t *b,
                                        const expr_t *e) {
  if (!b || !e || e->as.call.args.len == 0)
    return 0;
  const expr_t *ptr_arg = e->as.call.args.data[0].val;
  if (!ptr_arg || ptr_arg->kind != NY_E_IDENT)
    return 0;
  nyr_local_t *bl = nyr_find_local((nyr_builder_t *)b, ptr_arg->as.ident.name);
  return (bl && bl->buffer_byte_len > 0) ? bl->buffer_byte_len : 0;
}

static bool nyr_fail(nyr_builder_impl_t *b, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void ny_native_fn_index_free(ny_native_fn_index_t *index);

static void nyr_builder_dispose(nyr_builder_impl_t *b) {
  if (!b)
    return;
  if (b->resolve_alias_stats_enabled && b->cache_owner == b)
    ny_trace_line(
        "IMPORT_PROFILE",
        "alias queries=%llu local-shadow=%llu prefilter-reject=%llu "
        "cache-hit=%llu cache-miss=%llu uncached-scan=%llu entries=%zu",
        (unsigned long long)b->resolve_alias_queries,
        (unsigned long long)b->resolve_alias_local_shadows,
        (unsigned long long)b->resolve_alias_prefilter_rejects,
        (unsigned long long)b->resolve_alias_cache_hits,
        (unsigned long long)b->resolve_alias_cache_misses,
        (unsigned long long)b->resolve_alias_uncached_scans,
        b->resolve_alias_cache_len);
  if (b->layout_stats_enabled && b->cache_owner == b)
    ny_trace_line("LAYOUT_PROFILE",
                  "ast-queries=%llu ast-cache-hit=%llu ast-cache-miss=%llu "
                  "ast-scans=%llu program-queries=%llu program-cache-hit=%llu "
                  "program-cache-miss=%llu program-scans=%llu "
                  "program-index-lookups=%llu index-entries=%zu ast-cache=%zu",
                  (unsigned long long)b->layout_queries,
                  (unsigned long long)b->layout_cache_hits,
                  (unsigned long long)b->layout_cache_misses,
                  (unsigned long long)b->layout_uncached_scans,
                  (unsigned long long)b->layout_prog_queries,
                  (unsigned long long)b->layout_prog_cache_hits,
                  (unsigned long long)b->layout_prog_cache_misses,
                  (unsigned long long)b->layout_prog_scans,
                  (unsigned long long)b->layout_prog_index_lookups,
                  b->layout_name_index_len, b->layout_cache_cap);
  if (b->prog_query_stats_enabled && b->cache_owner == b)
    fprintf(stderr,
            "[native-lookup] program-query=%llu/%llu hits misses=%llu "
            "evictions=%llu slots=%u\n",
            (unsigned long long)b->prog_query_hits,
            (unsigned long long)b->prog_query_queries,
            (unsigned long long)b->prog_query_misses,
            (unsigned long long)b->prog_query_evictions,
            (unsigned)NY_PROG_QUERY_CACHE_SIZE);
  free(b->locals);
  free(b->loop_frames);
  free(b->labels);
  free(b->facts);
  free(b->defers);
  free(b->tail_param_slots);
  free(b->type_query_stack);
  free(b->type_query_cache);
  free(b->type_query_hash);
  free(b->type_query_context_cache);
  free(b->type_query_context_hash);
  free(b->user_fn_cache);
  free(b->top_val_cache);
  free(b->use_canon_cache);
  free(b->try_dynamic_cache);
  free(b->resolve_alias_cache);
  free(b->resolve_alias_name_slots);
  free(b->fn_resolve_cache);
  free(b->top_stmt_cache);
  free(b->prog_query_cache);
  free(b->imported_val_cache);
  free(b->layout_cache);
  free(b->layout_name_index);
  free(b->top_var_index);
  free(b->extern_index);
  free(b->refine_cache);
  free(b->dynlist_cache);
  free(b->dynlist_name_slots);
  free(b->top_mut_facts);
  arena_free(&b->default_arg_arena);
  ny_native_lambda_cache_free(b->lambda_cache);
  ny_native_fn_index_free(b->fn_index);
  for (size_t i = 0; i < b->tlv_len; ++i)
    free(b->tlv_ents[i].name);
  free(b->tlv_ents);
  free(b->tlv_head);
  free(b->tlv_tail);
  free(b->attached_cache);
  free(b->operator_cache);
  free(b->operator_pair_cache);
  for (size_t i = 0; i < b->owned_string_count; ++i)
    free(b->owned_strings[i]);
  free(b->owned_strings);
  free(b->owned_string_slots);
  b->locals = NULL;
  b->loop_frames = NULL;
  b->labels = NULL;
  b->facts = NULL;
  b->defers = NULL;
  b->tail_param_slots = NULL;
  b->type_query_stack = NULL;
  b->type_query_cache = NULL;
  b->type_query_hash = NULL;
  b->type_query_context_cache = NULL;
  b->type_query_context_hash = NULL;
  b->user_fn_cache = NULL;
  b->top_val_cache = NULL;
  b->use_canon_cache = NULL;
  b->use_canon_count = 0;
  b->use_canon_cap = 0;
  b->try_dynamic_cache = NULL;
  b->try_dynamic_cache_cap = 0;
  b->resolve_alias_cache = NULL;
  b->resolve_alias_cache_cap = 0;
  b->resolve_alias_cache_len = 0;
  b->resolve_alias_name_slots = NULL;
  b->resolve_alias_name_cap = 0;
  b->fn_resolve_cache = NULL;
  b->fn_resolve_cache_cap = 0;
  b->top_stmt_cache = NULL;
  b->top_stmt_cache_cap = 0;
  b->prog_query_cache = NULL;
  b->prog_query_cache_cap = 0;
  b->imported_val_cache = NULL;
  b->imported_val_cache_cap = 0;
  b->imported_val_cache_len = 0;
  b->layout_cache = NULL;
  b->layout_cache_cap = 0;
  b->refine_cache = NULL;
  b->refine_cache_cap = 0;
  b->dynlist_cache = NULL;
  b->dynlist_cache_len = 0;
  b->dynlist_cache_cap = 0;
  b->dynlist_name_slots = NULL;
  b->dynlist_name_slot_cap = 0;
  b->top_mut_facts = NULL;
  b->top_mut_facts_len = 0;
  b->top_mut_facts_cap = 0;
  b->top_mut_active_name = NULL;
  b->lambda_cache = NULL;
  b->tlv_ents = NULL;
  b->tlv_len = 0;
  b->tlv_cap = 0;
  b->tlv_head = NULL;
  b->tlv_tail = NULL;
  b->tlv_prog = NULL;
  b->tlv_complete = false;
  b->attached_cache = NULL;
  b->attached_cache_count = 0;
  b->attached_cache_cap = 0;
  b->operator_cache = NULL;
  b->operator_cache_count = 0;
  b->operator_cache_cap = 0;
  b->operator_pair_cache = NULL;
  b->operator_pair_cache_count = 0;
  b->operator_pair_cache_cap = 0;
  b->owned_strings = NULL;
  b->owned_string_slots = NULL;
  b->owned_string_slot_cap = 0;
  b->local_count = 0;
  b->local_cap = 0;
  b->loop_depth = 0;
  b->loop_cap = 0;
  b->fact_count = 0;
  b->fact_cap = 0;
  b->defer_count = 0;
  b->defer_cap = 0;
  b->tail_param_count = 0;
  b->type_query_depth = 0;
  b->type_query_cap = 0;
  b->type_query_cache_count = 0;
  b->type_query_cache_cap = 0;
  b->type_query_hash = NULL;
  b->type_query_hash_cap = 0;
  b->type_query_context_count = 0;
  b->type_query_context_cap = 0;
  b->type_query_context_hash_cap = 0;
  b->transient_query_depth = 0;
  b->owned_string_count = 0;
  b->owned_string_cap = 0;
}

static bool nyr_push_loop(nyr_builder_impl_t *b, int head_label,
                          int continue_label, int end_label) {
  if (!b)
    return false;
  if (b->loop_depth == b->loop_cap) {
    size_t cap = b->loop_cap ? b->loop_cap * 2 : 16;
    if (cap < b->loop_cap || cap > SIZE_MAX / sizeof(*b->loop_frames))
      return nyr_fail(b, "native NYIR lower: loop table is too large");
    nyr_loop_frame_t *frames = realloc(b->loop_frames, cap * sizeof(*frames));
    if (!frames)
      return nyr_fail(b, NY_NATIVE_ALLOC_FAIL);
    b->loop_frames = frames;
    b->loop_cap = cap;
  }
  b->loop_frames[b->loop_depth++] = (nyr_loop_frame_t){
      .head_label = head_label,
      .continue_label = continue_label,
      .end_label = end_label,
      .defer_mark = b->defer_count,
  };
  return true;
}

static int nyr_temp_slot(nyr_builder_impl_t *b) {
  return b ? b->next_local_slot++ : -1;
}

static bool nyr_mark_temp_slot(nyr_builder_impl_t *b, int slot,
                               const char *type_name, ny_sem_rep_t semantic_rep,
                               nyr_scalar_class_t scalar_class, bool is_any) {
  if (!b || slot < 0)
    return false;
  if (b->local_count == b->local_cap) {
    size_t cap = b->local_cap ? b->local_cap * 2 : 64;
    if (cap < b->local_cap || cap > SIZE_MAX / sizeof(*b->locals))
      return nyr_fail(b, NY_NATIVE_ALLOC_FAIL);
    nyr_local_t *locals = realloc(b->locals, cap * sizeof(*b->locals));
    if (!locals)
      return nyr_fail(b, NY_NATIVE_ALLOC_FAIL);
    b->locals = locals;
    b->local_cap = cap;
  }
  b->locals[b->local_count++] = (nyr_local_t){
      .type_name = type_name,
      .slot = slot,
      .semantic_rep = semantic_rep,
      .scalar_class = scalar_class,
      .alias_count = 1,
      .is_any = is_any,
      .sb_slot = -1,
      .list_len_slot = -1,
      .dyn_str_len_slot = -1,
      .dyn_tag_slot = -1,
      .arg_slot = -1,
      .list_len_arg_slot = -1,
      .dyn_str_len_arg_slot = -1,
      .dyn_tag_arg_slot = -1,
  };
  return true;
}

static bool nyr_mark_temp_slot_raw(nyr_builder_impl_t *b, int slot) {
  return nyr_mark_temp_slot(b, slot, "int", NY_SEM_REP_RAW_INT,
                            NYIR_SCALAR_I64_RAW, false);
}

static bool nyr_mark_temp_slot_tagged(nyr_builder_impl_t *b, int slot) {
  return nyr_mark_temp_slot(b, slot, "any", NY_SEM_REP_TAGGED_DYNAMIC,
                            NYIR_SCALAR_I64_TAGGED, true);
}

static size_t nyr_scope_mark(nyr_builder_impl_t *b) {
  return b ? b->local_count : 0;
}

static void nyr_scope_restore(nyr_builder_impl_t *b, size_t mark) {
  if (b && mark <= b->local_count)
    b->local_count = mark;
}

/*
 * Forward declarations (the statement lowerer is defined later in this file).
 */
static bool nyr_lower_stmt(nyr_builder_impl_t *b, const stmt_t *s);
static int nyr_emit_runtime_call(nyr_builder_impl_t *b, const char *symbol,
                                 int a, int b_arg, int c, int argc,
                                 nyr_scalar_class_t return_class);

/*
 * Push a deferred body so it runs LIFO at the enclosing scope's exit.
 */
static bool nyr_push_defer(nyr_builder_impl_t *b, stmt_t *body) {
  if (!b || !body)
    return true;
  if (b->defer_count == b->defer_cap) {
    size_t cap = b->defer_cap ? b->defer_cap * 2 : 16;
    if (cap < b->defer_cap || cap > SIZE_MAX / sizeof(*b->defers))
      return nyr_fail(b, "native NYIR lower: defer table is too large");
    stmt_t **defers = realloc(b->defers, cap * sizeof(*defers));
    if (!defers)
      return nyr_fail(b, NY_NATIVE_ALLOC_FAIL);
    b->defers = defers;
    b->defer_cap = cap;
  }
  b->defers[b->defer_count++] = body;
  return true;
}

/*
 * Run deferred bodies registered since mark, LIFO, then drop them.
 */
static bool nyr_emit_defers(nyr_builder_impl_t *b, size_t mark) {
  if (!b || b->defer_count <= mark)
    return true;
  size_t saved_return = b->emitted_return;
  while (b->defer_count > mark) {
    --b->defer_count;
    if (nyr_emit_runtime_call(b, "rt_pop_run_defer", -1, -1, -1, 0, 0) < 0) {
      b->emitted_return = saved_return;
      return false;
    }
  }
  b->emitted_return = saved_return;
  return true;
}

/*
 * Lower a statement body inside a synthetic scope: defers registered by the
 * body are emitted (LIFO) when the body finishes, so single-statement loop/if
 * bodies get per-iteration / per-branch defer semantics.  For block bodies
 * the block already emits its own defers at exit, so this is a no-op.
 * Returns the body's success; last_value is preserved across defer emission.
 */
static bool nyr_lower_scoped_body(nyr_builder_impl_t *b, const stmt_t *body) {
  size_t defer_mark = b->defer_count;
  if (!nyr_lower_stmt(b, body))
    return false;
  if (b->emitted_return)
    return true; /* return/break/continue already ran the defers */
  int saved_last = b->last_value;
  if (!nyr_emit_defers(b, defer_mark))
    return false;
  b->last_value = saved_last;
  return true;
}

static bool nyr_fail(nyr_builder_impl_t *b, const char *fmt, ...) {
  if (!b || !b->err || b->err_len == 0)
    return false;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b->err, b->err_len, fmt, ap);
  va_end(ap);
  return false;
}

/*
 * Like nyr_fail, but also reports a source-located diagnostic at TOK so
 * native-route proof failures carry file:line:col and a caret, matching
 * the legacy proof checker.
 */
static bool nyr_fail_at(nyr_builder_impl_t *b, token_t tok, const char *fmt,
                        ...) {
  char msg[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  ny_diag_error(tok, "%s", msg);
  return nyr_fail(b, "%s", msg);
}

static bool nyr_ignored_stmt(const stmt_t *s) {
  return !s || s->kind == NY_S_USE || s->kind == NY_S_LINK ||
         s->kind == NY_S_INCLUDE || s->kind == NY_S_DEFINE ||
         s->kind == NY_S_EXPORT || s->kind == NY_S_MODULE ||
         s->kind == NY_S_EXTERN || s->kind == NY_S_LAYOUT ||
         s->kind == NY_S_STRUCT || s->kind == NY_S_ENUM ||
         s->kind == NY_S_MACRO || s->kind == NY_S_OPERATOR ||
         s->kind == NY_S_IMPL || s->kind == NY_S_REFINE ||
         s->kind == NY_S_TRAIT;
}

static nyr_local_t *nyr_find_local_ctx(nyr_builder_impl_t *b, const char *name,
                                       uint32_t syntax_ctx) {
  if (!b || !name)
    return NULL;
  for (size_t i = b->local_count; i > 0; --i) {
    nyr_local_t *l = &b->locals[i - 1];
    if (l->name && strcmp(l->name, name) == 0) {
      if (l->syntax_ctx == syntax_ctx)
        return l;
    }
  }
  return NULL;
}

static nyr_local_t *nyr_find_local(nyr_builder_impl_t *b, const char *name) {
  if (!b || !name)
    return NULL;
  nyr_local_t *local = nyr_find_local_ctx(b, name, b->current_syntax_ctx);
  if (local || b->exact_syntax_ctx)
    return local;
  /*
   * Parser nodes may carry the syntax context from the flattened module
   * that produced them rather than the active function context.  A failed
   * context match must still resolve the nearest lexical binding; falling
   * through to a module-level value makes parameter names such as `a` and
   * `b` depend on unrelated top-level declarations.
   */
  for (size_t i = b->local_count; i > 0; --i) {
    nyr_local_t *candidate = &b->locals[i - 1];
    if (candidate->name && strcmp(candidate->name, name) == 0)
      return candidate;
  }
  return NULL;
}

/*
 * Assignment and member-call lowering operate on an existing lexical
 * binding.  Parser-generated syntax contexts can differ between the binding
 * and a later assignment even though the source name is the same.  Prefer the
 * active context, then resolve the nearest older binding by name; declarations
 * continue to use the context-sensitive lookup above so shadowing is intact.
 */
static nyr_local_t *nyr_find_local_nearest(nyr_builder_impl_t *b,
                                           const char *name) {
  nyr_local_t *local = nyr_find_local(b, name);
  if (local || !b || !name || b->exact_syntax_ctx)
    return local;
  for (size_t i = b->local_count; i > 0; --i) {
    nyr_local_t *candidate = &b->locals[i - 1];
    if (candidate->name && strcmp(candidate->name, name) == 0)
      return candidate;
  }
  return NULL;
}

static nyr_local_t *nyr_find_expr_local(nyr_builder_impl_t *b,
                                        const expr_t *e) {
  if (!b || !e || e->kind != NY_E_IDENT || !e->as.ident.name)
    return NULL;
  nyr_local_t *local =
      nyr_find_local_ctx(b, e->as.ident.name, e->as.ident.syntax_ctx);
  if (!local && !b->exact_syntax_ctx)
    local = nyr_find_local_nearest(b, e->as.ident.name);
  return local;
}

/*
 * Forward declarations needed by nyr_expr_is_dyn_list which is
 * defined here but calls helpers defined later in the file.
 */
static bool ny_native_type_name_is_list(const char *name);
static bool nyr_expr_is_list(const nyr_builder_impl_t *b, const expr_t *e);
static bool nyr_expr_is_dict(const nyr_builder_impl_t *b, const expr_t *e);
static bool nyr_expr_is_set(const nyr_builder_impl_t *b, const expr_t *e);
static bool nyr_expr_is_tuple(const nyr_builder_impl_t *b, const expr_t *e);
static bool nyr_expr_is_f64(nyr_builder_impl_t *b, const expr_t *e);
static bool nyr_expr_is_f32(nyr_builder_impl_t *b, const expr_t *e);
static bool nyr_expr_is_bigint(nyr_builder_impl_t *b, const expr_t *e);
static bool nyr_expr_is_bigfloat(nyr_builder_impl_t *b, const expr_t *e);
static bool nyr_user_defined_fn(nyr_builder_impl_t *b, const char *name);
/*
 * Global top-level value index: name -> defining expressions in program
 * pre-order, built once per program.  find_top_level_value's fallback and
 * the constant folder otherwise pay a full subtree walk per (root, name)
 * pair — thousands of full walks per compile on the core.os.ui fixtures.
 */
bool nyr_target_eval_bool(const ny_options *opt, const expr_t *e, bool *out);
static void ny_tlv_collect_stmt(nyr_builder_impl_t *owner, const stmt_t *s,
                                const char *prefix, const stmt_t *root,
                                unsigned depth);

static void ny_tlv_add(nyr_builder_impl_t *owner, const char *name,
                       const expr_t *val, const stmt_t *root, unsigned depth,
                       bool qualified, bool is_mut) {
  if (!name || !val)
    return;
  uint32_t h = 2166136261u;
  for (const char *p = name; *p; ++p)
    h = (h ^ (uint8_t)*p) * 16777619u;
  h %= NY_TLV_BUCKETS;
  if (owner->tlv_len == owner->tlv_cap) {
    size_t cap = owner->tlv_cap ? owner->tlv_cap * 2u : 4096u;
    ny_tlv_ent_t *grown =
        (ny_tlv_ent_t *)realloc(owner->tlv_ents, cap * sizeof(*grown));
    if (!grown) {
      owner->tlv_complete = false;
      return;
    }
    owner->tlv_ents = grown;
    owner->tlv_cap = cap;
  }
  ny_tlv_ent_t *ent = &owner->tlv_ents[owner->tlv_len];
  ent->name = strdup(name);
  if (!ent->name) {
    owner->tlv_complete = false;
    return;
  }
  ent->val = val;
  ent->file = val->tok.filename;
  ent->root = root;
  ent->depth = depth;
  ent->qualified = qualified;
  ent->is_mut = is_mut;
  ent->next = -1;
  /*
   * Chain in insertion order: head stays at the FIRST entry, new entries
   * append at the tail so pre-order preference is preserved.
   */
  if (owner->tlv_head[h] < 0)
    owner->tlv_head[h] = (int32_t)owner->tlv_len;
  else
    owner->tlv_ents[owner->tlv_tail[h]].next = (int32_t)owner->tlv_len;
  owner->tlv_tail[h] = (int32_t)owner->tlv_len;
  ++owner->tlv_len;
}

static void ny_tlv_collect_stmt(nyr_builder_impl_t *owner, const stmt_t *s,
                                const char *prefix, const stmt_t *root,
                                unsigned depth) {
  if (!s)
    return;
  if (s->kind == NY_S_VAR) {
    for (size_t n = 0; n < s->as.var.names.len && n < s->as.var.exprs.len;
         ++n) {
      ny_tlv_add(owner, s->as.var.names.data[n], s->as.var.exprs.data[n], root,
                 depth, false, s->as.var.is_mut);
      if (prefix && *prefix) {
        char qualified[512];
        int n2 = snprintf(qualified, sizeof(qualified), "%s.%s", prefix,
                          s->as.var.names.data[n]);
        if (n2 > 0 && (size_t)n2 < sizeof(qualified))
          ny_tlv_add(owner, qualified, s->as.var.exprs.data[n], root, depth,
                     true, s->as.var.is_mut);
      }
    }
    return;
  }
  if (s->kind == NY_S_MODULE) {
    const char *mn =
        s->as.module.name && *s->as.module.name ? s->as.module.name : prefix;
    for (size_t i = 0; i < s->as.module.body.len; ++i)
      ny_tlv_collect_stmt(owner, s->as.module.body.data[i], mn, root,
                          depth + 1);
    return;
  }
  if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      ny_tlv_collect_stmt(owner, s->as.block.body.data[i], prefix, root,
                          depth + 1);
    return;
  }
  if (s->kind == NY_S_IF) {
    bool selected = false;
    if (s->as.iff.test && s->as.iff.test->kind == NY_E_COMPTIME &&
        nyr_target_eval_bool(NULL, s->as.iff.test, &selected)) {
      ny_tlv_collect_stmt(owner, selected ? s->as.iff.conseq : s->as.iff.alt,
                          prefix, root, depth + 1);
    } else {
      ny_tlv_collect_stmt(owner, s->as.iff.conseq, prefix, root, depth + 1);
      ny_tlv_collect_stmt(owner, s->as.iff.alt, prefix, root, depth + 1);
    }
  }
}

static void ny_native_tlv_ensure_build(const nyr_builder_impl_t *b,
                                       const program_t *prog) {
  if (!b || !prog)
    return;
  nyr_builder_impl_t *owner =
      b->cache_owner ? b->cache_owner : (nyr_builder_t *)b;
  if (owner->tlv_prog == prog && owner->tlv_head)
    return;
  for (size_t i = 0; i < owner->tlv_len; ++i)
    free(owner->tlv_ents[i].name);
  free(owner->tlv_ents);
  free(owner->tlv_head);
  free(owner->tlv_tail);
  owner->tlv_ents = NULL;
  owner->tlv_len = owner->tlv_cap = 0;
  owner->tlv_tail = NULL;
  owner->tlv_head = malloc(NY_TLV_BUCKETS * sizeof(*owner->tlv_head));
  owner->tlv_tail = malloc(NY_TLV_BUCKETS * sizeof(*owner->tlv_tail));
  if (!owner->tlv_head || !owner->tlv_tail) {
    free(owner->tlv_head);
    free(owner->tlv_tail);
    owner->tlv_head = owner->tlv_tail = NULL;
    owner->tlv_prog = NULL;
    return;
  }
  memset(owner->tlv_head, 0xFF, NY_TLV_BUCKETS * sizeof(*owner->tlv_head));
  memset(owner->tlv_tail, 0xFF, NY_TLV_BUCKETS * sizeof(*owner->tlv_tail));
  owner->tlv_prog = prog;
  owner->tlv_complete = true;
  for (size_t i = 0; i < prog->body.len; ++i)
    ny_tlv_collect_stmt(owner, prog->body.data[i], NULL, prog->body.data[i], 0);
}

static bool nyr_fold_top_level_int(const nyr_builder_impl_t *b,
                                   const program_t *prog, const expr_t *e,
                                   int64_t *out, unsigned depth);
static bool nyr_fold_top_level_int_in_module(const nyr_builder_impl_t *b,
                                             const program_t *prog,
                                             const char *module_name,
                                             const expr_t *e, int64_t *out,
                                             unsigned depth);
static const char *nyr_module_for_initializer_expr(const program_t *prog,
                                                   const expr_t *e);
static const stmt_t *nyr_find_imported_function(const nyr_builder_impl_t *b,
                                                const char *name);
static const stmt_t *nyr_find_top_level_var_stmt(const nyr_builder_impl_t *b,
                                                 const char *name);
static const stmt_t *
nyr_find_top_level_var_in_source_stmt(const stmt_t *s, const char *name,
                                      const char *source_file, unsigned depth);
static const stmt_t *nyr_find_top_level_var_in_source_in_prog(
    const nyr_builder_impl_t *b, const program_t *prog, const char *name,
    const char *source_file);
static bool nyr_index_of_typed_global_int_list(nyr_builder_impl_t *b,
                                               const expr_t *e);
static bool nyr_expr_is_cstr(nyr_builder_impl_t *b, const expr_t *e);
static bool nyr_expr_is_bytes(const nyr_builder_impl_t *b, const expr_t *e);
static bool nyr_expr_is_ptr(const nyr_builder_impl_t *b, const expr_t *e);
static bool nyr_expr_is_bool(nyr_builder_impl_t *b, const expr_t *e);
static int nyr_box_bool(nyr_builder_impl_t *b, int reg);
static const char *nyr_resolve_use_alias(const nyr_builder_impl_t *b,
                                         const char *alias);
static bool nyr_expr_is_any(nyr_builder_impl_t *b, const expr_t *e);
static bool nyr_expr_is_raw_integer_conversion(nyr_builder_impl_t *b,
                                               const expr_t *e);
static bool nyr_is_bitwise_operator(const char *op);
static const stmt_t *nyr_find_attached_method(const nyr_builder_impl_t *b,
                                              const expr_t *receiver,
                                              const char *method);
static const stmt_t *nyr_find_attached_owner(const nyr_builder_impl_t *b,
                                             const char *owner,
                                             const char *method);
static const stmt_t *nyr_find_operator(const nyr_builder_impl_t *b,
                                       const expr_t *e);
static bool nyr_expr_is_raw_dynamic_read(nyr_builder_impl_t *b,
                                         const expr_t *e);
static bool nyr_expr_is_canonical_dynamic_value(nyr_builder_impl_t *b,
                                                const expr_t *e);
static bool nyr_expr_is_dict_read(nyr_builder_impl_t *b, const expr_t *e);
static bool nyr_stmt_uses_ident(const stmt_t *s, const char *name);
static bool nyr_expr_is_range(const nyr_builder_impl_t *b, const expr_t *e);
static const expr_t *nyr_find_top_level_value(const nyr_builder_impl_t *b,
                                              const char *name);
static const expr_t *
nyr_find_top_level_value_in_source(const nyr_builder_impl_t *b,
                                   const char *name, const char *source_file);
static const expr_t *nyr_find_imported_value(const nyr_builder_impl_t *b,
                                             const char *name);
static const expr_t *nyr_resolve_member_expr(const nyr_builder_impl_t *b,
                                             const expr_t *e);
static const stmt_t *nyr_find_user_function(const nyr_builder_impl_t *b,
                                            const char *name);
static const stmt_t *
nyr_find_user_function_for_arity(const nyr_builder_impl_t *b, const char *name,
                                 size_t argc);
static ny_sem_rep_t nyr_rep_for_type(const nyr_builder_impl_t *b,
                                     const char *type_name);
static const stmt_t *nyr_find_layout_stmt(const nyr_builder_impl_t *b,
                                          const stmt_t *s, const char *name);
static const stmt_t *nyr_find_refine_stmt(const nyr_builder_impl_t *b,
                                          const stmt_t *s, const char *name);
static const char *ny_native_call_leaf(const expr_t *e);
static bool nyr_record_dyn_fact(nyr_builder_impl_t *b, int value,
                                nyr_fact_kind_t kind, int reg);

static bool ny_native_type_name_is_dyn_list(const char *name) {
  if (!name || !ny_native_type_name_is_list(name))
    return false;
  if (strcmp(name, "list") == 0 || strcmp(name, "list[]") == 0)
    return true;
  /*
   * Only the ELEMENT type decides the accessor ABI.  A substring scan made
   * `list<list<int>>` classify as a scalar list (the nested "<int>" matched),
   * so typed-list gets handed consumers a raw inner-list pointer as an int
   * (`type(sub)` printed "int" and the chained get panicked).  Parse the
   * element after the first '<' and require a scalar element for the raw
   * elem-8 accessor; sequence/object elements take the canonical path.
   */
  const char *elem = strchr(name, '<');
  if (!elem)
    return true;
  ++elem;
  static const char *const scalar_elems[] = {
      "int", "i64", "i32", "i16",   "i8",     "u64",  "u32", "u16",
      "u8",  "f64", "f32", "float", "double", "bool", NULL};
  for (size_t i = 0; scalar_elems[i]; ++i) {
    size_t len = strlen(scalar_elems[i]);
    if (strncmp(elem, scalar_elems[i], len) == 0 &&
        (elem[len] == '>' || elem[len] == '['))
      return false;
  }
  return true;
}

static bool
nyr_function_param_is_dyn_list(const nyr_builder_impl_t *cache_owner,
                               const program_t *prog, const stmt_t *fn,
                               size_t param_index);

/*
 * Whole-program shape facts for top-level `mut` lists.
 *
 * Top-level mutables are module storage (ADDR_SYMBOL), not builder locals,
 * so the per-local representation facts never apply to them: every indexed
 * read, subscript store, and comparison resolves the identifier back to its
 * source initializer through the expression oracles.  A list declared as
 * `mut xs = []` and only ever extended through `append(xs, <int>)` (with
 * int-typed subscript stores) keeps the elem-8 scalar representation for its
 * whole lifetime: `[]` materializes rt_tbuf_new_raw(0, 8), the raw append
 * preserves elem_size, and the tagged setter untags integers back into
 * elem-8 storage.  The scanners below prove that shape once per (program,
 * name) so the representation oracles can route such lists through the raw
 * elem-8 paths instead of the boxed dynamic boundary on every access.
 */

static bool nyr_stmt_uses_ident(const stmt_t *s, const char *name);
static bool nyr_expr_uses_ident(const nyr_builder_impl_t *b, const expr_t *e,
                                const char *name);

static bool ny_top_mut_stmt_ok(const nyr_builder_impl_t *b, const stmt_t *s,
                               const char *name, unsigned depth);
static bool ny_top_mut_int_value(const nyr_builder_impl_t *b, const expr_t *e,
                                 const char *name, unsigned depth);
static bool ny_top_mut_ident_sites_int(const nyr_builder_impl_t *b,
                                       const stmt_t *s, const char *x,
                                       const char *name, unsigned depth,
                                       bool *found);

/*
 * True when every reference to `name` inside `e` reads scalars out of the
 * list (element indexing, .len, read-only methods) and never leaks the
 * buffer itself into another binding, container, or call.
 */
static bool ny_top_mut_reads_only(const nyr_builder_impl_t *b, const expr_t *e,
                                  const char *name, unsigned depth) {
  if (!e)
    return true;
  if (depth > 64)
    return false;
  if (e->kind == NY_E_IDENT)
    return !(e->as.ident.name && strcmp(e->as.ident.name, name) == 0);
  if (e->kind == NY_E_INDEX) {
    if (e->as.index.target && e->as.index.target->kind == NY_E_IDENT &&
        e->as.index.target->as.ident.name &&
        strcmp(e->as.index.target->as.ident.name, name) == 0)
      return ny_top_mut_reads_only(b, e->as.index.start, name, depth + 1) &&
             ny_top_mut_reads_only(b, e->as.index.stop, name, depth + 1) &&
             ny_top_mut_reads_only(b, e->as.index.step, name, depth + 1);
    return ny_top_mut_reads_only(b, e->as.index.target, name, depth + 1) &&
           ny_top_mut_reads_only(b, e->as.index.start, name, depth + 1) &&
           ny_top_mut_reads_only(b, e->as.index.stop, name, depth + 1) &&
           ny_top_mut_reads_only(b, e->as.index.step, name, depth + 1);
  }
  if (e->kind == NY_E_MEMBER) {
    if (e->as.member.target && e->as.member.target->kind == NY_E_IDENT &&
        e->as.member.target->as.ident.name &&
        strcmp(e->as.member.target->as.ident.name, name) == 0)
      return true;
    return ny_top_mut_reads_only(b, e->as.member.target, name, depth + 1);
  }
  if (e->kind == NY_E_MEMCALL) {
    /*
     * These methods preserve the proven element representation. `pop` is
     * mutating, but only removes an element and cannot introduce a new shape.
     */
    bool shape_preserving_method =
        e->as.memcall.name && (strcmp(e->as.memcall.name, "len") == 0 ||
                               strcmp(e->as.memcall.name, "get") == 0 ||
                               strcmp(e->as.memcall.name, "contains") == 0 ||
                               strcmp(e->as.memcall.name, "first") == 0 ||
                               strcmp(e->as.memcall.name, "last") == 0 ||
                               strcmp(e->as.memcall.name, "pop") == 0 ||
                               strcmp(e->as.memcall.name, "min") == 0 ||
                               strcmp(e->as.memcall.name, "max") == 0 ||
                               strcmp(e->as.memcall.name, "sum") == 0);
    if (e->as.memcall.target && e->as.memcall.target->kind == NY_E_IDENT &&
        e->as.memcall.target->as.ident.name &&
        strcmp(e->as.memcall.target->as.ident.name, name) == 0) {
      if (!shape_preserving_method)
        return false;
    } else if (!ny_top_mut_reads_only(b, e->as.memcall.target, name,
                                      depth + 1)) {
      return false;
    }
    for (size_t i = 0; i < e->as.memcall.args.len; ++i)
      if (!ny_top_mut_reads_only(b, e->as.memcall.args.data[i].val, name,
                                 depth + 1))
        return false;
    return true;
  }
  if (e->kind == NY_E_CALL) {
    for (size_t i = 0; i < e->as.call.args.len; ++i) {
      const expr_t *arg = e->as.call.args.data[i].val;
      if (!arg)
        continue;
      /*
       * Passing the list itself into a user function is acceptable when the
       * callee handles its corresponding formal with the same int-only
       * discipline (recursive, depth-capped): the elem-8 invariant is
       * runtime-maintained, so a disciplined callee can never convert the
       * buffer, and downstream elem_size==8 fast arms stay sound.  Without
       * this, heapsort/matrix-style `sort(a)` calls disqualified the
       * promotion entirely.
       */
      if (arg->kind == NY_E_IDENT && arg->as.ident.name &&
          strcmp(arg->as.ident.name, name) == 0) {
        const char *leaf =
            e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT
                ? e->as.call.callee->as.ident.name
                : ny_native_call_leaf(e);
        const stmt_t *callee =
            leaf ? nyr_find_user_function((nyr_builder_t *)b, leaf) : NULL;
        if (!callee || callee->kind != NY_S_FUNC ||
            i >= callee->as.fn.params.len || !callee->as.fn.params.data[i].name)
          return false;
        if (!ny_top_mut_stmt_ok(b, callee->as.fn.body,
                                callee->as.fn.params.data[i].name, depth + 1))
          return false;
        continue;
      }
      if (!ny_top_mut_reads_only(b, arg, name, depth + 1))
        return false;
    }
    return true;
  }
  if (e->kind == NY_E_LAMBDA || e->kind == NY_E_FN) {
    for (size_t i = 0; i < e->as.lambda.params.len; ++i)
      if (e->as.lambda.params.data[i].name &&
          strcmp(e->as.lambda.params.data[i].name, name) == 0)
        return true;
    return ny_top_mut_stmt_ok(b, e->as.lambda.body, name, depth + 1);
  }
  switch (e->kind) {
  case NY_E_LITERAL:
    return true;
  case NY_E_UNARY:
    return ny_top_mut_reads_only(b, e->as.unary.right, name, depth + 1);
  case NY_E_BINARY:
    return ny_top_mut_reads_only(b, e->as.binary.left, name, depth + 1) &&
           ny_top_mut_reads_only(b, e->as.binary.right, name, depth + 1);
  case NY_E_LOGICAL:
    return ny_top_mut_reads_only(b, e->as.logical.left, name, depth + 1) &&
           ny_top_mut_reads_only(b, e->as.logical.right, name, depth + 1);
  case NY_E_TERNARY:
    return ny_top_mut_reads_only(b, e->as.ternary.cond, name, depth + 1) &&
           ny_top_mut_reads_only(b, e->as.ternary.true_expr, name, depth + 1) &&
           ny_top_mut_reads_only(b, e->as.ternary.false_expr, name, depth + 1);
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
    for (size_t i = 0; i < e->as.list_like.len; ++i)
      if (!ny_top_mut_reads_only(b, e->as.list_like.data[i], name, depth + 1))
        return false;
    return true;
  case NY_E_DICT:
    for (size_t i = 0; i < e->as.dict.pairs.len; ++i)
      if (!ny_top_mut_reads_only(b, e->as.dict.pairs.data[i].key, name,
                                 depth + 1) ||
          !ny_top_mut_reads_only(b, e->as.dict.pairs.data[i].value, name,
                                 depth + 1))
        return false;
    return true;
  case NY_E_FSTRING:
    for (size_t i = 0; i < e->as.fstring.parts.len; ++i)
      if (e->as.fstring.parts.data[i].kind == NY_FSP_EXPR &&
          !ny_top_mut_reads_only(b, e->as.fstring.parts.data[i].as.e, name,
                                 depth + 1))
        return false;
    return true;
  case NY_E_ASM:
    for (size_t i = 0; i < e->as.as_asm.args.len; ++i)
      if (!ny_top_mut_reads_only(b, e->as.as_asm.args.data[i], name, depth + 1))
        return false;
    return true;
  case NY_E_COMPTIME:
    return ny_top_mut_stmt_ok(b, e->as.comptime_expr.body, name, depth + 1);
  case NY_E_MATCH:
    if (!ny_top_mut_reads_only(b, e->as.match.test, name, depth + 1))
      return false;
    for (size_t i = 0; i < e->as.match.arms.len; ++i) {
      const match_arm_t *arm = &e->as.match.arms.data[i];
      for (size_t j = 0; j < arm->patterns.len; ++j)
        if (!ny_top_mut_reads_only(b, arm->patterns.data[j], name, depth + 1))
          return false;
      if (!ny_top_mut_reads_only(b, arm->guard, name, depth + 1) ||
          !ny_top_mut_stmt_ok(b, arm->conseq, name, depth + 1))
        return false;
    }
    return ny_top_mut_stmt_ok(b, e->as.match.default_conseq, name, depth + 1);
  case NY_E_PTR_TYPE:
    return ny_top_mut_reads_only(b, e->as.ptr_type.target, name, depth + 1);
  case NY_E_DEREF:
    return ny_top_mut_reads_only(b, e->as.deref.target, name, depth + 1);
  case NY_E_SIZEOF:
    return ny_top_mut_reads_only(b, e->as.szof.target, name, depth + 1);
  case NY_E_TRY:
    return ny_top_mut_reads_only(b, e->as.try_expr.target, name, depth + 1);
  default:
    return false;
  }
}

/*
 * Provable-int producers for append items and subscript-store values.  The
 * shape is int when it is an int literal, arithmetic over int shapes, an
 * element of the same list, or a top-level binding whose initializer is an
 * int shape.  Strings, floats, bools, bigints, containers, calls, and
 * anything dynamically annotated reject the promotion.
 */
static bool ny_top_mut_int_value(const nyr_builder_impl_t *b, const expr_t *e,
                                 const char *name, unsigned depth);
static bool nyr_top_mut_is_append_int_list(const nyr_builder_impl_t *b,
                                           const char *name);
static bool ny_top_mut_int_value(const nyr_builder_impl_t *b, const expr_t *e,
                                 const char *name, unsigned depth) {
  if (!e || depth > 64)
    return false;
  if (e->kind == NY_E_INDEX && e->as.index.target &&
      e->as.index.target->kind == NY_E_IDENT &&
      e->as.index.target->as.ident.name &&
      strcmp(e->as.index.target->as.ident.name, name) == 0)
    return ny_top_mut_reads_only(b, e->as.index.start, name, depth + 1);
  /*
   * A read of a DIFFERENT top-level mut that the whole-program scan itself
   * proves is an elem-8 int list yields an int element.  This is what lets
   * pipeline shapes such as `mapped = append(mapped, xs[i] + 1)` promote
   * when xs was promoted first; the active-name guard in the scanner keeps
   * cyclic mutual references conservative rather than unsound.
   */
  if (e->kind == NY_E_INDEX && !e->as.index.stop && !e->as.index.step &&
      e->as.index.target && e->as.index.target->kind == NY_E_IDENT &&
      e->as.index.target->as.ident.name &&
      strcmp(e->as.index.target->as.ident.name, name) != 0 &&
      nyr_top_mut_is_append_int_list(b, e->as.index.target->as.ident.name) &&
      ny_top_mut_reads_only(b, e->as.index.start, name, depth + 1))
    return true;
  if (e->kind == NY_E_LITERAL)
    return e->as.literal.kind == NY_LIT_INT && e->tok.kind != NY_T_NIL;
  if (e->kind == NY_E_UNARY)
    return ny_top_mut_int_value(b, e->as.unary.right, name, depth + 1);
  if (e->kind == NY_E_BINARY) {
    if (e->as.binary.op && strcmp(e->as.binary.op, "..") == 0)
      return false;
    return ny_top_mut_int_value(b, e->as.binary.left, name, depth + 1) &&
           ny_top_mut_int_value(b, e->as.binary.right, name, depth + 1);
  }
  if (e->kind == NY_E_TERNARY)
    return ny_top_mut_int_value(b, e->as.ternary.true_expr, name, depth + 1) &&
           ny_top_mut_int_value(b, e->as.ternary.false_expr, name, depth + 1);
  if (e->kind != NY_E_IDENT || !e->as.ident.name)
    return false;
  nyr_builder_impl_t *mb = (nyr_builder_t *)b;
  if (nyr_expr_is_cstr(mb, e) || nyr_expr_is_f64(mb, e) ||
      nyr_expr_is_f32(mb, e) || nyr_expr_is_bool(mb, e) ||
      nyr_expr_is_bigint(mb, e)) {
    return false;
  }
  const expr_t *init =
      e->tok.filename ? nyr_find_top_level_value_in_source(b, e->as.ident.name,
                                                           e->tok.filename)
                      : NULL;
  if (!init)
    init = nyr_find_top_level_value(b, e->as.ident.name);
  if (init && init != e) {
    if (init->kind == NY_E_LITERAL)
      return init->as.literal.kind == NY_LIT_INT && init->tok.kind != NY_T_NIL;
    if (init->semantic.resolved && init->semantic.rep == NY_SEM_REP_RAW_INT)
      return true;
    return ny_top_mut_int_value(b, init, name, depth + 1);
  }
  /*
   * A local scalar: its int-ness must come from every binding site in the
   * program having an int-shaped initializer, not from the builder's live
   * local table (the scan can run long before this statement is lowered).
   */
  nyr_builder_impl_t *owner =
      b->cache_owner ? b->cache_owner : (nyr_builder_t *)b;
  for (const ny_top_mut_int_visit_t *visit = owner->top_mut_int_active; visit;
       visit = visit->parent)
    if (strcmp(visit->name, e->as.ident.name) == 0)
      return false;
  /*
   * Assignment-site scans restart AST depth at the program root. An integer
   * recurrence (x = x + 1) must not recursively rescan itself forever.
   * Cycles have no independent representation proof, so reject conservatively
   * rather than guessing an integer shape from an in-progress query.
   */
  ny_top_mut_int_visit_t visit = {e->as.ident.name, owner->top_mut_int_active};
  owner->top_mut_int_active = &visit;
  bool found = false;
  bool result = true;
  for (size_t i = 0; b->prog && i < b->prog->body.len; ++i)
    if (!ny_top_mut_ident_sites_int(b, b->prog->body.data[i], e->as.ident.name,
                                    name, 0, &found)) {
      result = false;
      break;
    }
  owner->top_mut_int_active = visit.parent;
  return result && found;
}

/*
 * `name = append(name, <int shape>)` or `name = name.append(<int shape>)`.
 */
static bool ny_top_mut_is_append_shape(const nyr_builder_impl_t *b,
                                       const expr_t *rhs, const char *name) {
  if (rhs && rhs->kind == NY_E_MEMCALL && rhs->as.memcall.name &&
      strcmp(rhs->as.memcall.name, "append") == 0 &&
      rhs->as.memcall.args.len == 1 && rhs->as.memcall.target &&
      rhs->as.memcall.target->kind == NY_E_IDENT &&
      rhs->as.memcall.target->as.ident.name &&
      strcmp(rhs->as.memcall.target->as.ident.name, name) == 0) {
    const expr_t *item = rhs->as.memcall.args.data[0].val;
    return item && ny_top_mut_int_value(b, item, name, 0) &&
           ny_top_mut_reads_only(b, item, name, 0);
  }
  if (!rhs || rhs->kind != NY_E_CALL || rhs->as.call.args.len != 2 ||
      rhs->as.call.args.data[0].name || rhs->as.call.args.data[1].name)
    return false;
  const expr_t *callee = rhs->as.call.callee;
  if (!callee || callee->kind != NY_E_IDENT || !callee->as.ident.name ||
      strcmp(callee->as.ident.name, "append") != 0)
    return false;
  const expr_t *recv = rhs->as.call.args.data[0].val;
  const expr_t *item = rhs->as.call.args.data[1].val;
  if (!recv || recv->kind != NY_E_IDENT || !recv->as.ident.name ||
      strcmp(recv->as.ident.name, name) != 0)
    return false;
  if (!item || !ny_top_mut_int_value(b, item, name, 0))
    return false;
  return ny_top_mut_reads_only(b, item, name, 0);
}

static bool ny_top_mut_fn_shadows(const stmt_t *fn, const char *name) {
  if (!fn || fn->kind != NY_S_FUNC || !name)
    return false;
  for (size_t i = 0; i < fn->as.fn.params.len; ++i)
    if (fn->as.fn.params.data[i].name &&
        strcmp(fn->as.fn.params.data[i].name, name) == 0)
      return true;
  return false;
}

/*
 * Int-shape analysis for scalar identifiers referenced by append items and
 * subscript-store values.  A local binding's int-ness cannot come from the
 * builder's live local table: the scan runs once, before later statements
 * bind their locals.  Instead, every binding site in the program (and every
 * assignment to the name) must itself have an int-shaped initializer, and
 * the name must never be bound as an untyped parameter/iterator/catch.
 */
static bool ny_top_mut_ident_expr_sites_int(const nyr_builder_impl_t *b,
                                            const expr_t *e, const char *x,
                                            const char *name, unsigned depth,
                                            bool *found) {
  if (!e)
    return true;
  if (depth > 64)
    return false;
  /*
   * Plain references to `x` are uses, not bindings: only binding sites
   * (declarations, assignments, parameters, iterators) decide int-ness.
   */
  if (e->kind == NY_E_IDENT)
    return true;
  if (e->kind == NY_E_LAMBDA || e->kind == NY_E_FN) {
    for (size_t i = 0; i < e->as.lambda.params.len; ++i) {
      const param_t *p = &e->as.lambda.params.data[i];
      if (p->name && strcmp(p->name, x) == 0) {
        *found = true;
        if (!(p->type && strcmp(p->type, "int") == 0))
          return false;
      }
    }
    return ny_top_mut_ident_sites_int(b, e->as.lambda.body, x, name, depth + 1,
                                      found);
  }
  if (e->kind == NY_E_MATCH) {
    if (!ny_top_mut_ident_expr_sites_int(b, e->as.match.test, x, name,
                                         depth + 1, found))
      return false;
    for (size_t i = 0; i < e->as.match.arms.len; ++i) {
      const match_arm_t *arm = &e->as.match.arms.data[i];
      for (size_t j = 0; j < arm->patterns.len; ++j) {
        const expr_t *pat = arm->patterns.data[j];
        if (pat && pat->kind == NY_E_IDENT && pat->as.ident.name &&
            strcmp(pat->as.ident.name, x) == 0) {
          *found = true;
          return false;
        }
        if (!ny_top_mut_ident_expr_sites_int(b, pat, x, name, depth + 1, found))
          return false;
      }
      if (!ny_top_mut_ident_expr_sites_int(b, arm->guard, x, name, depth + 1,
                                           found) ||
          !ny_top_mut_ident_sites_int(b, arm->conseq, x, name, depth + 1,
                                      found))
        return false;
    }
    return ny_top_mut_ident_sites_int(b, e->as.match.default_conseq, x, name,
                                      depth + 1, found);
  }
  switch (e->kind) {
  case NY_E_UNARY:
    return ny_top_mut_ident_expr_sites_int(b, e->as.unary.right, x, name,
                                           depth + 1, found);
  case NY_E_BINARY:
    return ny_top_mut_ident_expr_sites_int(b, e->as.binary.left, x, name,
                                           depth + 1, found) &&
           ny_top_mut_ident_expr_sites_int(b, e->as.binary.right, x, name,
                                           depth + 1, found);
  case NY_E_LOGICAL:
    return ny_top_mut_ident_expr_sites_int(b, e->as.logical.left, x, name,
                                           depth + 1, found) &&
           ny_top_mut_ident_expr_sites_int(b, e->as.logical.right, x, name,
                                           depth + 1, found);
  case NY_E_TERNARY:
    return ny_top_mut_ident_expr_sites_int(b, e->as.ternary.cond, x, name,
                                           depth + 1, found) &&
           ny_top_mut_ident_expr_sites_int(b, e->as.ternary.true_expr, x, name,
                                           depth + 1, found) &&
           ny_top_mut_ident_expr_sites_int(b, e->as.ternary.false_expr, x, name,
                                           depth + 1, found);
  case NY_E_CALL:
    for (size_t i = 0; i < e->as.call.args.len; ++i)
      if (!ny_top_mut_ident_expr_sites_int(b, e->as.call.args.data[i].val, x,
                                           name, depth + 1, found))
        return false;
    return true;
  case NY_E_MEMCALL:
    if (!ny_top_mut_ident_expr_sites_int(b, e->as.memcall.target, x, name,
                                         depth + 1, found))
      return false;
    for (size_t i = 0; i < e->as.memcall.args.len; ++i)
      if (!ny_top_mut_ident_expr_sites_int(b, e->as.memcall.args.data[i].val, x,
                                           name, depth + 1, found))
        return false;
    return true;
  case NY_E_INDEX:
    return ny_top_mut_ident_expr_sites_int(b, e->as.index.target, x, name,
                                           depth + 1, found) &&
           ny_top_mut_ident_expr_sites_int(b, e->as.index.start, x, name,
                                           depth + 1, found) &&
           ny_top_mut_ident_expr_sites_int(b, e->as.index.stop, x, name,
                                           depth + 1, found) &&
           ny_top_mut_ident_expr_sites_int(b, e->as.index.step, x, name,
                                           depth + 1, found);
  case NY_E_MEMBER:
    return ny_top_mut_ident_expr_sites_int(b, e->as.member.target, x, name,
                                           depth + 1, found);
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
    for (size_t i = 0; i < e->as.list_like.len; ++i)
      if (!ny_top_mut_ident_expr_sites_int(b, e->as.list_like.data[i], x, name,
                                           depth + 1, found))
        return false;
    return true;
  case NY_E_DICT:
    for (size_t i = 0; i < e->as.dict.pairs.len; ++i)
      if (!ny_top_mut_ident_expr_sites_int(b, e->as.dict.pairs.data[i].key, x,
                                           name, depth + 1, found) ||
          !ny_top_mut_ident_expr_sites_int(b, e->as.dict.pairs.data[i].value, x,
                                           name, depth + 1, found))
        return false;
    return true;
  case NY_E_FSTRING:
    for (size_t i = 0; i < e->as.fstring.parts.len; ++i)
      if (e->as.fstring.parts.data[i].kind == NY_FSP_EXPR &&
          !ny_top_mut_ident_expr_sites_int(b, e->as.fstring.parts.data[i].as.e,
                                           x, name, depth + 1, found))
        return false;
    return true;
  case NY_E_ASM:
    for (size_t i = 0; i < e->as.as_asm.args.len; ++i)
      if (!ny_top_mut_ident_expr_sites_int(b, e->as.as_asm.args.data[i], x,
                                           name, depth + 1, found))
        return false;
    return true;
  case NY_E_COMPTIME:
    return ny_top_mut_ident_sites_int(b, e->as.comptime_expr.body, x, name,
                                      depth + 1, found);
  case NY_E_PTR_TYPE:
    return ny_top_mut_ident_expr_sites_int(b, e->as.ptr_type.target, x, name,
                                           depth + 1, found);
  case NY_E_DEREF:
    return ny_top_mut_ident_expr_sites_int(b, e->as.deref.target, x, name,
                                           depth + 1, found);
  case NY_E_SIZEOF:
    return ny_top_mut_ident_expr_sites_int(b, e->as.szof.target, x, name,
                                           depth + 1, found);
  case NY_E_TRY:
    return ny_top_mut_ident_expr_sites_int(b, e->as.try_expr.target, x, name,
                                           depth + 1, found);
  default:
    return true;
  }
}

static bool ny_top_mut_ident_sites_int(const nyr_builder_impl_t *b,
                                       const stmt_t *s, const char *x,
                                       const char *name, unsigned depth,
                                       bool *found) {
  if (!s || depth > 64)
    return true;
  switch (s->kind) {
  case NY_S_VAR: {
    const stmt_var_t *v = &s->as.var;
    if (v->is_destructure)
      return false;
    for (size_t i = 0; i < v->names.len; ++i) {
      const char *n = v->names.data[i];
      if (!n || strcmp(n, x) != 0)
        continue;
      *found = true;
      const char *ty = i < v->types.len ? v->types.data[i] : NULL;
      if (ty && ty[0] && strcmp(ty, "int") != 0) {
        return false;
      }
      const expr_t *rhs = i < v->exprs.len ? v->exprs.data[i] : NULL;
      if (!rhs || !ny_top_mut_int_value(b, rhs, name, depth + 1)) {
        return false;
      }
    }
    for (size_t i = 0; i < v->exprs.len; ++i)
      if (!ny_top_mut_ident_expr_sites_int(b, v->exprs.data[i], x, name,
                                           depth + 1, found))
        return false;
    return true;
  }
  case NY_S_EXPR:
    return ny_top_mut_ident_expr_sites_int(b, s->as.expr.expr, x, name,
                                           depth + 1, found);
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      if (!ny_top_mut_ident_sites_int(b, s->as.block.body.data[i], x, name,
                                      depth + 1, found))
        return false;
    return true;
  case NY_S_IF:
    return ny_top_mut_ident_expr_sites_int(b, s->as.iff.test, x, name,
                                           depth + 1, found) &&
           ny_top_mut_ident_sites_int(b, s->as.iff.init, x, name, depth + 1,
                                      found) &&
           ny_top_mut_ident_sites_int(b, s->as.iff.conseq, x, name, depth + 1,
                                      found) &&
           ny_top_mut_ident_sites_int(b, s->as.iff.alt, x, name, depth + 1,
                                      found);
  case NY_S_WHILE:
    return ny_top_mut_ident_expr_sites_int(b, s->as.whl.test, x, name,
                                           depth + 1, found) &&
           ny_top_mut_ident_expr_sites_int(b, s->as.whl.invariant, x, name,
                                           depth + 1, found) &&
           ny_top_mut_ident_sites_int(b, s->as.whl.init, x, name, depth + 1,
                                      found) &&
           ny_top_mut_ident_sites_int(b, s->as.whl.update, x, name, depth + 1,
                                      found) &&
           ny_top_mut_ident_sites_int(b, s->as.whl.body, x, name, depth + 1,
                                      found);
  case NY_S_FOR:
    if ((s->as.fr.iter_var && strcmp(s->as.fr.iter_var, x) == 0) ||
        (s->as.fr.iter_index_var && strcmp(s->as.fr.iter_index_var, x) == 0)) {
      *found = true;
      return false;
    }
    return ny_top_mut_ident_expr_sites_int(b, s->as.fr.iterable, x, name,
                                           depth + 1, found) &&
           ny_top_mut_ident_expr_sites_int(b, s->as.fr.cond, x, name, depth + 1,
                                           found) &&
           ny_top_mut_ident_sites_int(b, s->as.fr.init, x, name, depth + 1,
                                      found) &&
           ny_top_mut_ident_sites_int(b, s->as.fr.update, x, name, depth + 1,
                                      found) &&
           ny_top_mut_ident_sites_int(b, s->as.fr.body, x, name, depth + 1,
                                      found);
  case NY_S_GUARD:
    if (s->as.guard.name && strcmp(s->as.guard.name, x) == 0) {
      *found = true;
      return false;
    }
    return ny_top_mut_ident_expr_sites_int(b, s->as.guard.value, x, name,
                                           depth + 1, found) &&
           ny_top_mut_ident_sites_int(b, s->as.guard.fallback, x, name,
                                      depth + 1, found);
  case NY_S_TRY:
    if (s->as.tr.err && strcmp(s->as.tr.err, x) == 0) {
      *found = true;
      return false;
    }
    return ny_top_mut_ident_sites_int(b, s->as.tr.body, x, name, depth + 1,
                                      found) &&
           ny_top_mut_ident_sites_int(b, s->as.tr.handler, x, name, depth + 1,
                                      found);
  case NY_S_DEFER:
    return ny_top_mut_ident_sites_int(b, s->as.de.body, x, name, depth + 1,
                                      found);
  case NY_S_RETURN:
    return ny_top_mut_ident_expr_sites_int(b, s->as.ret.value, x, name,
                                           depth + 1, found);
  case NY_S_FUNC: {
    for (size_t i = 0; i < s->as.fn.params.len; ++i) {
      const param_t *p = &s->as.fn.params.data[i];
      if (p->name && strcmp(p->name, x) == 0) {
        *found = true;
        if (!(p->type && strcmp(p->type, "int") == 0))
          return false;
      }
    }
    return ny_top_mut_ident_sites_int(b, s->as.fn.body, x, name, depth + 1,
                                      found);
  }
  case NY_S_MATCH: {
    if (!ny_top_mut_ident_expr_sites_int(b, s->as.match.test, x, name,
                                         depth + 1, found))
      return false;
    for (size_t i = 0; i < s->as.match.arms.len; ++i) {
      const match_arm_t *arm = &s->as.match.arms.data[i];
      for (size_t j = 0; j < arm->patterns.len; ++j) {
        const expr_t *pat = arm->patterns.data[j];
        if (pat && pat->kind == NY_E_IDENT && pat->as.ident.name &&
            strcmp(pat->as.ident.name, x) == 0) {
          *found = true;
          return false;
        }
        if (!ny_top_mut_ident_expr_sites_int(b, pat, x, name, depth + 1, found))
          return false;
      }
      if (!ny_top_mut_ident_expr_sites_int(b, arm->guard, x, name, depth + 1,
                                           found) ||
          !ny_top_mut_ident_sites_int(b, arm->conseq, x, name, depth + 1,
                                      found))
        return false;
    }
    return ny_top_mut_ident_sites_int(b, s->as.match.default_conseq, x, name,
                                      depth + 1, found);
  }
  default:
    return true;
  }
}

static bool ny_top_mut_stmt_ok(const nyr_builder_impl_t *b, const stmt_t *s,
                               const char *name, unsigned depth) {
  if (!s)
    return true;
  if (depth > 64)
    return false;
  switch (s->kind) {
  case NY_S_VAR: {
    const stmt_var_t *v = &s->as.var;
    if (v->is_destructure || v->is_del)
      return false;
    for (size_t i = 0; i < v->names.len; ++i) {
      const char *n = v->names.data[i];
      if (!n)
        continue;
      if (strcmp(n, name) == 0) {
        /*
         * A nested declaration shadows the module binding; assignments after
         * it bind to the shadow, so a nested decl simply retires the proof.
         * The root declaration is validated by the scan entry instead.
         */
        if (v->is_decl) {
          return false;
        }
        const expr_t *rhs = i < v->exprs.len ? v->exprs.data[i] : NULL;
        if (!ny_top_mut_is_append_shape(b, rhs, name)) {
          return false;
        }
      } else if (i < v->exprs.len && v->exprs.data[i]) {
        const expr_t *rhs = v->exprs.data[i];
        /*
         * An alias to the container itself must retain shared managed-list
         * semantics. The elem-8 promotion uses a replaceable buffer handle;
         * append can return a new handle after growth, which cannot update
         * other copied handles.
         */
        if (rhs->kind == NY_E_IDENT && rhs->as.ident.name &&
            strcmp(rhs->as.ident.name, name) == 0)
          return false;
        if (!ny_top_mut_reads_only(b, rhs, name, 0))
          return false;
      }
    }
    return true;
  }
  case NY_S_EXPR: {
    const expr_t *e = s->as.expr.expr;
    if (!e)
      return true;
    /*
     * `name[key] = value` parses as set_idx(name, key, value).
     */
    if (e->kind == NY_E_CALL && e->as.call.callee &&
        e->as.call.callee->kind == NY_E_IDENT &&
        e->as.call.callee->as.ident.name &&
        strcmp(e->as.call.callee->as.ident.name, "set_idx") == 0 &&
        e->as.call.args.len == 3 && !e->as.call.args.data[0].name &&
        !e->as.call.args.data[1].name && !e->as.call.args.data[2].name) {
      const expr_t *target = e->as.call.args.data[0].val;
      if (target && target->kind == NY_E_IDENT && target->as.ident.name &&
          strcmp(target->as.ident.name, name) == 0) {
        const expr_t *key = e->as.call.args.data[1].val;
        const expr_t *value = e->as.call.args.data[2].val;
        bool ok = ny_top_mut_reads_only(b, key, name, 0) &&
                  ny_top_mut_int_value(b, value, name, 0) &&
                  ny_top_mut_reads_only(b, value, name, 0);
        return ok;
      }
    }
    bool ro = ny_top_mut_reads_only(b, e, name, 0);
    return ro;
  }
  case NY_S_BLOCK: {
    /*
     * From a declaration of `name` onward, references in this block bind to
     * the shadow, not the module storage: stop validating the rest.
     */
    bool shadowed = false;
    for (size_t i = 0; i < s->as.block.body.len; ++i) {
      const stmt_t *item = s->as.block.body.data[i];
      if (shadowed)
        continue;
      if (item && item->kind == NY_S_VAR && item->as.var.is_decl) {
        for (size_t j = 0; j < item->as.var.names.len; ++j)
          if (item->as.var.names.data[j] &&
              strcmp(item->as.var.names.data[j], name) == 0) {
            shadowed = true;
            break;
          }
      }
      if (!shadowed && !ny_top_mut_stmt_ok(b, item, name, depth + 1))
        return false;
    }
    return true;
  }
  case NY_S_IF:
    return ny_top_mut_reads_only(b, s->as.iff.test, name, 0) &&
           ny_top_mut_stmt_ok(b, s->as.iff.init, name, depth + 1) &&
           ny_top_mut_stmt_ok(b, s->as.iff.conseq, name, depth + 1) &&
           ny_top_mut_stmt_ok(b, s->as.iff.alt, name, depth + 1);
  case NY_S_WHILE: {
    bool ok = ny_top_mut_reads_only(b, s->as.whl.test, name, 0) &&
              ny_top_mut_reads_only(b, s->as.whl.invariant, name, 0) &&
              ny_top_mut_stmt_ok(b, s->as.whl.init, name, depth + 1) &&
              ny_top_mut_stmt_ok(b, s->as.whl.update, name, depth + 1) &&
              ny_top_mut_stmt_ok(b, s->as.whl.body, name, depth + 1);
    return ok;
  }
  case NY_S_FOR:
    return ny_top_mut_reads_only(b, s->as.fr.iterable, name, 0) &&
           ny_top_mut_reads_only(b, s->as.fr.cond, name, 0) &&
           ny_top_mut_stmt_ok(b, s->as.fr.init, name, depth + 1) &&
           ny_top_mut_stmt_ok(b, s->as.fr.update, name, depth + 1) &&
           ny_top_mut_stmt_ok(b, s->as.fr.body, name, depth + 1);
  case NY_S_GUARD:
    if (s->as.guard.name && strcmp(s->as.guard.name, name) == 0)
      return true;
    return ny_top_mut_reads_only(b, s->as.guard.value, name, 0) &&
           ny_top_mut_stmt_ok(b, s->as.guard.fallback, name, depth + 1);
  case NY_S_TRY:
    if (s->as.tr.err && strcmp(s->as.tr.err, name) == 0)
      return false;
    return ny_top_mut_stmt_ok(b, s->as.tr.body, name, depth + 1) &&
           ny_top_mut_stmt_ok(b, s->as.tr.handler, name, depth + 1);
  case NY_S_DEFER:
    return ny_top_mut_stmt_ok(b, s->as.de.body, name, depth + 1);
  case NY_S_RETURN:
    return ny_top_mut_reads_only(b, s->as.ret.value, name, 0);
  case NY_S_FUNC:
    /*
     * References inside a function whose parameter shadows the name bind to
     * the parameter; otherwise the body can still assign the module binding
     * (closures capture it lexically), so validate the body.
     */
    if (ny_top_mut_fn_shadows(s, name))
      return true;
    return ny_top_mut_stmt_ok(b, s->as.fn.body, name, depth + 1);
  case NY_S_MATCH: {
    if (!ny_top_mut_reads_only(b, s->as.match.test, name, 0))
      return false;
    for (size_t i = 0; i < s->as.match.arms.len; ++i) {
      const match_arm_t *arm = &s->as.match.arms.data[i];
      for (size_t j = 0; j < arm->patterns.len; ++j)
        if (!ny_top_mut_reads_only(b, arm->patterns.data[j], name, 0))
          return false;
      if (!ny_top_mut_reads_only(b, arm->guard, name, 0) ||
          !ny_top_mut_stmt_ok(b, arm->conseq, name, depth + 1))
        return false;
    }
    return ny_top_mut_stmt_ok(b, s->as.match.default_conseq, name, depth + 1);
  }
  default:
    /*
     * Scaffolding statements (use/link/extern/layout/...) cannot rebind or
     * leak the list when they do not mention it at all.
     */
    return !nyr_stmt_uses_ident(s, name);
  }
}

static bool ny_top_mut_literal_is_raw_int_list(const expr_t *init) {
  if (!init || init->kind != NY_E_LIST)
    return false;
  for (size_t i = 0; i < init->as.list_like.len; ++i) {
    const expr_t *item = init->as.list_like.data[i];
    if (!item || item->kind != NY_E_LITERAL ||
        item->as.literal.kind != NY_LIT_INT || item->tok.kind == NY_T_NIL ||
        item->as.literal.as.i <= -(INT64_C(1) << 62) ||
        item->as.literal.as.i >= (INT64_C(1) << 62))
      return false;
  }
  return true;
}

static bool ny_top_mut_scan(const nyr_builder_impl_t *b, const char *name,
                            bool allow_declared) {
  const program_t *prog = b->prog;
  const stmt_t *decl = NULL;
  size_t decl_count = 0;
  for (size_t i = 0; i < prog->body.len; ++i) {
    const stmt_t *s = prog->body.data[i];
    if (!s || s->kind != NY_S_VAR || !s->as.var.is_decl)
      continue;
    for (size_t j = 0; j < s->as.var.names.len; ++j) {
      if (s->as.var.names.data[j] &&
          strcmp(s->as.var.names.data[j], name) == 0) {
        decl = s;
        ++decl_count;
      }
    }
  }
  if (decl_count != 1 || !decl->as.var.is_mut) {
    return false;
  }
  size_t idx = 0;
  for (size_t j = 0; j < decl->as.var.names.len; ++j)
    if (decl->as.var.names.data[j] &&
        strcmp(decl->as.var.names.data[j], name) == 0)
      idx = j;
  const expr_t *init =
      idx < decl->as.var.exprs.len ? decl->as.var.exprs.data[idx] : NULL;
  if (!init || init->kind != NY_E_LIST ||
      (init->as.list_like.len != 0 &&
       !ny_top_mut_literal_is_raw_int_list(init)))
    return false;
  /*
   * Representation promotion is restricted to untyped mutable lists.
   * Entry-frame localization may reuse the escape proof for declared lists
   * without changing their established element ABI or descriptor tags.
   */
  if (!allow_declared && idx < decl->as.var.types.len &&
      decl->as.var.types.data[idx] && decl->as.var.types.data[idx][0])
    return false;
  for (size_t i = 0; i < prog->body.len; ++i) {
    const stmt_t *s = prog->body.data[i];
    if (s == decl) {
      /*
       * The validated declaration itself: only co-declared siblings'
       * initializers could leak the fresh empty list.
       */
      for (size_t j = 0; j < s->as.var.names.len; ++j)
        if (j != idx && j < s->as.var.exprs.len &&
            !ny_top_mut_reads_only(b, s->as.var.exprs.data[j], name, 0)) {
          return false;
        }
      continue;
    }
    if (!ny_top_mut_stmt_ok(b, s, name, 0)) {
      return false;
    }
  }
  return true;
}

static bool nyr_top_mut_is_append_int_list(const nyr_builder_impl_t *b,
                                           const char *name) {
  if (!b || !b->prog || !name || !*name)
    return false;
  nyr_builder_impl_t *owner =
      b->cache_owner ? b->cache_owner : (nyr_builder_t *)b;
  if (owner->top_mut_active_name &&
      strcmp(name, owner->top_mut_active_name) == 0)
    return false;
  ny_sym_id name_id = ny_intern_cstr(name);
  for (size_t i = 0; i < owner->top_mut_facts_len; ++i)
    if (owner->top_mut_facts[i].prog == b->prog &&
        owner->top_mut_facts[i].name_id == name_id)
      return owner->top_mut_facts[i].result;
  /*
   * Cheap prefilter: only a name whose top-level definition is an empty list
   * or raw-integer list literal can possibly qualify. Representation oracles
   * consult this entry for nearly every identifier in large flattened programs,
   * so a full program scan per non-candidate name would be quadratic at compile
   * time (module-heavy imports exceeded their fixture budget).  The hash
   * indexed lookup is O(1) and rejects functions, imports, typed and
   * non-list globals before any walk.  A same-named def from another module
   * can only cause a conservative false negative here, never a wrong
   * promotion: the scan itself re-resolves the declaration in source.
   */
  const expr_t *init0 = nyr_find_top_level_value(b, name);
  if (!init0 || init0->kind != NY_E_LIST ||
      (init0->as.list_like.len != 0 &&
       !ny_top_mut_literal_is_raw_int_list(init0)))
    return false;
  const char *prev_name = owner->top_mut_active_name;
  owner->top_mut_active_name = name;
  bool result = ny_top_mut_scan(b, name, false);
  owner->top_mut_active_name = prev_name;
  if (owner->top_mut_facts_len < NY_TOP_MUT_FACTS_MAX) {
    if (owner->top_mut_facts_len == owner->top_mut_facts_cap) {
      size_t next =
          owner->top_mut_facts_cap ? owner->top_mut_facts_cap * 2 : 32;
      if (next > NY_TOP_MUT_FACTS_MAX)
        next = NY_TOP_MUT_FACTS_MAX;
      ny_top_mut_fact_t *grown =
          realloc(owner->top_mut_facts, next * sizeof(*grown));
      if (grown) {
        owner->top_mut_facts = grown;
        owner->top_mut_facts_cap = next;
      }
    }
    if (owner->top_mut_facts_len < owner->top_mut_facts_cap) {
      owner->top_mut_facts[owner->top_mut_facts_len++] = (ny_top_mut_fact_t){
          .prog = b->prog, .name_id = name_id, .result = result};
    }
  }
  return result;
}

/*
 * True for an element read whose receiver is a whole-program proven elem-8
 * int list.  Arithmetic flag computation consults this before the operand's
 * own lowering publishes the raw-int semantic stamp.
 */
static bool nyr_index_of_promoted_top_mut(nyr_builder_impl_t *b,
                                          const expr_t *e) {
  return b && e && e->kind == NY_E_INDEX && e->as.index.target &&
         e->as.index.target->kind == NY_E_IDENT &&
         e->as.index.target->as.ident.name &&
         (!b->current_fn_name || strcmp(b->current_fn_name, "rt_main") == 0 ||
          !nyr_find_local_nearest(b, e->as.index.target->as.ident.name)) &&
         nyr_top_mut_is_append_int_list(b, e->as.index.target->as.ident.name);
}

static bool nyr_expr_is_dyn_list(nyr_builder_impl_t *b, const expr_t *e) {
  if (!e)
    return false;
  if (e->kind == NY_E_LIST || e->kind == NY_E_TUPLE) {
    /*
     * Float literals use descriptor stride 24 so nested/dynamic reads retain
     * an explicit TAG_FLOAT instead of exposing IEEE bits as raw integers.
     */
    if (e->as.list_like.len > 0) {
      bool all_f64 = true;
      for (size_t i = 0; i < e->as.list_like.len; ++i)
        if (!nyr_expr_is_f64(b, e->as.list_like.data[i])) {
          all_f64 = false;
          break;
        }
      if (all_f64)
        return true;
    }
  }
  /*
   * A declared `list` parameter has the raw typed-buffer ABI even when the
   * enclosing member expression was conservatively annotated as dynamic.
   * Preserve the local's ABI fact before consulting expression-level `any`
   * metadata (and before considering an imported module with the same name).
   */
  if (e->kind == NY_E_IDENT && e->as.ident.name) {
    const nyr_local_t *local = nyr_find_local_nearest(b, e->as.ident.name);
    if (local && local->is_list && !local->is_any && !local->is_dyn_list &&
        local->type_name && *local->type_name)
      return false;
    /*
     * A top-level `mut xs = []` that the whole program only ever extends
     * with int appends keeps the elem-8 scalar representation; it is not a
     * descriptor-backed dynamic sequence.
     */
    if (!local && nyr_top_mut_is_append_int_list(b, e->as.ident.name))
      return false;
  }
  /*
   * Literal list construction has stronger per-element representation
   * evidence than the expression-level `any` fallback.  Let the list branch
   * below decide from its elements; genuinely dynamic/heterogeneous literals
   * still return true there.
   */
  if (e->kind != NY_E_LIST && e->kind != NY_E_TUPLE && nyr_expr_is_list(b, e) &&
      nyr_expr_is_any(b, e))
    return true;
  if (e->kind == NY_E_MEMCALL) {
    if (e->as.memcall.name &&
        (strcmp(e->as.memcall.name, "keys") == 0 ||
         strcmp(e->as.memcall.name, "values") == 0 ||
         strcmp(e->as.memcall.name, "items") == 0) &&
        nyr_expr_is_dict(b, e->as.memcall.target))
      return true;
    if (e->as.memcall.name && strcmp(e->as.memcall.name, "get") == 0 &&
        e->as.memcall.args.len >= 1) {
      /*
       * Dictionary `get` returns the stored dynamic value.  The fallback's
       * type does not change that ABI: using it as the representation fact
       * made `dict.get("limit", 0) > raw_count` compare a raw integer with
       * its tagged payload.  Sequence accessors are the exception because a
       * proven list/bytes/range receiver has a scalar element ABI.
       */
      const expr_t *target = e->as.memcall.target;
      if (target && (nyr_expr_is_dict(b, target) || nyr_expr_is_any(b, target)))
        return true;
      if (target &&
          (nyr_expr_is_list(b, target) || nyr_expr_is_dyn_list(b, target) ||
           nyr_expr_is_bytes(b, target) || nyr_expr_is_range(b, target)))
        return false;
      /*
       * A receiver bound to a top-level `mut list<int>` has the elem-8
       * scalar ABI (its indexed reads and `.get` lower through the raw
       * accessor) even though the binding itself is not in the local fact
       * table.  The declared type is authoritative here too; without it the
       * conservative `return true` below makes an integer `.get` pair such
       * as `drop_y.get(x, 0) + drop_speed.get(x, 0)` select the list
       * concatenation ABI and corrupt the add.
       */
      if (target && target->kind == NY_E_IDENT && target->as.ident.name &&
          !nyr_find_local_nearest(b, target->as.ident.name)) {
        const stmt_t *decl =
            nyr_find_top_level_var_stmt(b, target->as.ident.name);
        if (decl && decl->kind == NY_S_VAR) {
          for (size_t di = 0; di < decl->as.var.names.len; ++di) {
            if (!decl->as.var.names.data[di] ||
                strcmp(decl->as.var.names.data[di], target->as.ident.name) != 0)
              continue;
            if (di < decl->as.var.types.len && decl->as.var.types.data[di] &&
                strcmp(decl->as.var.types.data[di], "list<int>") == 0)
              return false;
            break;
          }
        }
        if (nyr_top_mut_is_append_int_list(b, target->as.ident.name))
          return false;
      }
      return true;
    }
    /*
     * Attached methods can share a leaf name with a generic helper.  Resolve
     * the result representation from the typechecker's canonical declaration
     * before falling back to name-based lookup (for example `dict.clone` and
     * the generic `clone(any)` both have the leaf name `clone`).
     */
    const stmt_t *canonical = e->semantic.canonical_callee_stmt;
    if (canonical && canonical->kind == NY_S_FUNC &&
        canonical->as.fn.return_type)
      return strcmp(canonical->as.fn.return_type, "dict") == 0;
    if (e->semantic.canonical_callee) {
      canonical = nyr_find_user_function((nyr_builder_t *)b,
                                         e->semantic.canonical_callee);
      if (canonical && canonical->kind == NY_S_FUNC &&
          canonical->as.fn.return_type)
        return strcmp(canonical->as.fn.return_type, "dict") == 0;
    }
    const stmt_t *fn =
        e->as.memcall.name
            ? nyr_find_user_function((nyr_builder_t *)b, e->as.memcall.name)
            : NULL;
    return fn && ny_native_type_name_is_dyn_list(fn->as.fn.return_type);
  }
  if (e->kind == NY_E_CALL) {
    const char *leaf = ny_native_call_leaf(e);
    if (leaf && (strcmp(leaf, "list") == 0 || strcmp(leaf, "color") == 0 ||
                 strcmp(leaf, "get_terminal_size") == 0))
      return true;
    /*
     * These dictionary projections return tagged descriptor buffers even
     * though their source declarations use the unparameterized `list` type.
     * Keep the descriptor ABI fact attached to the producing expression so
     * indexing and any-boundary consumers decode each element exactly once.
     */
    if (leaf && e->as.call.args.len == 1 &&
        nyr_expr_is_dict(b, e->as.call.args.data[0].val) &&
        (strcmp(leaf, "dict_keys") == 0 || strcmp(leaf, "dict_values") == 0 ||
         strcmp(leaf, "dict_items") == 0 || strcmp(leaf, "keys") == 0 ||
         strcmp(leaf, "values") == 0 || strcmp(leaf, "items") == 0))
      return true;
    /*
     * The canonical target is authoritative for attached calls such as
     * `iter.chunk(xs, n)`.  Looking up only the leaf (`chunk`) misses
     * namespace and impl-qualified definitions, which made a subsequent
     * `.get()` look like a raw scalar accessor and lowered nested equality
     * as pointer comparison.
     */
    const stmt_t *fn = e->semantic.canonical_callee_stmt;
    if (!fn && e->semantic.canonical_callee)
      fn = ny_native_fn_cache_lookup_exact_with_builder(
          (nyr_builder_t *)b, e->semantic.canonical_callee);
    if (!fn && leaf)
      fn = nyr_find_user_function((nyr_builder_t *)b, leaf);
    if (!fn && e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT)
      fn = nyr_find_user_function((nyr_builder_t *)b,
                                  e->as.call.callee->as.ident.name);
    return fn && ny_native_type_name_is_dyn_list(fn->as.fn.return_type);
  }
  /*
   * An index expression produces an element, not the indexed container.  A
   * previous unconditional `true` here made numeric `xs[i] + ys[i]` select
   * the list-concatenation ABI.  Unknown element types are handled by the
   * dynamic scalar path below.
   */
  if (e->kind == NY_E_INDEX)
    return false;
  if (e->kind == NY_E_LIST) {
    /*
     * An empty literal is the dynamic tbuf representation's initial value.
     * It has no element evidence, but it is still a 24-byte descriptor list.
     */
    if (e->as.list_like.len == 0)
      return true;
    bool has_f64 = false, has_int = false;
    for (size_t i = 0; i < e->as.list_like.len; ++i) {
      const expr_t *el = e->as.list_like.data[i];
      const expr_t *scalar = el;
      if (scalar && scalar->kind == NY_E_UNARY && scalar->as.unary.op &&
          (strcmp(scalar->as.unary.op, "+") == 0 ||
           strcmp(scalar->as.unary.op, "-") == 0))
        scalar = scalar->as.unary.right;
      /*
       * Local representation facts are stronger than conservative `any`
       * expression metadata.  In particular, arithmetic state variables can
       * retain an `any` semantic fallback while their native local slot is a
       * proven raw integer.  Classify that fact first so homogeneous computed
       * integer literals (for example SHA-256's [a..h] state vector) stay on
       * the elem-8 scalar-list ABI instead of acquiring an unreachable
       * descriptor fallback on every index.
       */
      bool raw_i64 = scalar && scalar->kind == NY_E_LITERAL &&
                     (scalar->as.literal.kind == NY_LIT_INT ||
                      scalar->as.literal.kind == NY_LIT_BOOL);
      raw_i64 = raw_i64 || (el && el->semantic.resolved &&
                            el->semantic.rep == NY_SEM_REP_RAW_INT);
      if (!raw_i64 && el && el->kind == NY_E_IDENT && el->as.ident.name) {
        const nyr_local_t *element_local = nyr_find_local(b, el->as.ident.name);
        raw_i64 = element_local && !element_local->is_list &&
                  !element_local->is_dict && !element_local->is_cstr &&
                  !element_local->is_f64 && !element_local->is_f32 &&
                  element_local->semantic_rep == NY_SEM_REP_RAW_INT;
      }
      if (raw_i64) {
        has_int = true;
        continue;
      }
      if (nyr_expr_is_cstr(b, el) || nyr_expr_is_any(b, el) ||
          nyr_expr_is_dict(b, el) || nyr_expr_is_list(b, el) ||
          nyr_expr_is_bytes(b, el))
        return true;
      if (nyr_expr_is_f64(b, el) || nyr_expr_is_f32(b, el)) {
        has_f64 = true;
        continue;
      }
      return true;
    }
    return has_f64 && has_int;
  }
  if (e->kind == NY_E_BINARY && e->as.binary.op &&
      strcmp(e->as.binary.op, "**") == 0) {
    bool left_list = nyr_expr_is_list(b, e->as.binary.left);
    bool right_list = nyr_expr_is_list(b, e->as.binary.right);
    if (left_list != right_list)
      return nyr_expr_is_dyn_list(b, left_list ? e->as.binary.left
                                               : e->as.binary.right);
  }
  if (e->kind == NY_E_IDENT) {
    nyr_local_t *l = nyr_find_local_nearest(b, e->as.ident.name);
    if (l)
      return l->is_dyn_list ||
             (l->is_list && (l->is_any || !l->type_name || !*l->type_name));
    /*
     * A top-level `mut list<int>` declared with the elem-8 scalar ABI keeps
     * that ABI no matter which initializer shaped its first value: indexed
     * reads lower through the raw accessor, so classifying the binding as a
     * descriptor-backed dynamic sequence here would make consumers decode
     * raw payloads a second time.  The declared type is authoritative, the
     * same fact nyr_index_of_typed_global_int_list proves.
     */
    const stmt_t *decl = nyr_find_top_level_var_stmt(b, e->as.ident.name);
    if (decl && decl->kind == NY_S_VAR) {
      for (size_t di = 0; di < decl->as.var.names.len; ++di) {
        if (!decl->as.var.names.data[di] ||
            strcmp(decl->as.var.names.data[di], e->as.ident.name) != 0)
          continue;
        if (di < decl->as.var.types.len && decl->as.var.types.data[di] &&
            strcmp(decl->as.var.types.data[di], "list<int>") == 0)
          return false;
        break;
      }
    }
    const expr_t *g = e->tok.filename
                          ? nyr_find_top_level_value_in_source(
                                b, e->as.ident.name, e->tok.filename)
                          : nyr_find_top_level_value(b, e->as.ident.name);
    if (!g)
      g = nyr_find_top_level_value(b, e->as.ident.name);
    if (g && g != e)
      return nyr_expr_is_dyn_list(b, g);
  }
  if (e->kind == NY_E_MEMBER) {
    const expr_t *v = nyr_resolve_member_expr(b, e);
    if (v && v != e)
      return nyr_expr_is_dyn_list(b, v);
  }
  return false;
}

/*
 * An unparameterized list is the dynamic sequence ABI: its scalar slots are
 * raw payloads, but an index crosses the any boundary and therefore returns a
 * tagged value.  Keep this fact separate from `is_dyn_list`, which describes
 * descriptor-stride storage for lists whose elements are themselves dynamic
 * objects.
 */
static bool nyr_local_is_dynamic_sequence(const nyr_local_t *local) {
  if (!local)
    return false;
  if (local->is_any)
    return true;
  if (!local->is_list)
    return false;
  if (local->is_dyn_list)
    return true;
  /*
   * A list whose element type is a scalar primitive uses the compact raw
   * slot ABI.  An unparameterized list has no such proof: it may be promoted
   * by a later append, so it must keep the dynamic sequence ABI.
   */
  if (!local->type_name || !*local->type_name)
    return true;
  return ny_native_type_name_is_dyn_list(local->type_name);
}
static bool nyr_stmt_iterates_param(const stmt_t *s, const char *name,
                                    unsigned depth) {
  if (!s || !name || depth > 64)
    return false;
  if (s->kind == NY_S_FOR) {
    if (s->as.fr.iterable && s->as.fr.iterable->kind == NY_E_IDENT &&
        s->as.fr.iterable->as.ident.name &&
        strcmp(s->as.fr.iterable->as.ident.name, name) == 0)
      return true;
    return nyr_stmt_iterates_param(s->as.fr.body, name, depth + 1);
  }
  if (s->kind == NY_S_BLOCK || s->kind == NY_S_MODULE) {
    const ny_stmt_list *body =
        s->kind == NY_S_BLOCK ? &s->as.block.body : &s->as.module.body;
    for (size_t i = 0; i < body->len; ++i)
      if (nyr_stmt_iterates_param(body->data[i], name, depth + 1))
        return true;
  }
  return false;
}

/*
 * Infer a list ABI from operations that are specific to sequence mutation or
 * traversal.  Generic `get`, `set`, and `contains` are deliberately
 * excluded: dictionaries expose the same surface, and guessing a tbuf ABI
 * for an arbitrary `any` value silently turns `node.set("field", value)` into
 * a raw list write.
 */
static bool nyr_expr_uses_list_surface(const expr_t *e, const char *name,
                                       unsigned depth) {
  if (!e || !name || depth > 64)
    return false;
  switch (e->kind) {
  case NY_E_MEMCALL: {
    const char *method = e->as.memcall.name;
    if (e->as.memcall.target && e->as.memcall.target->kind == NY_E_IDENT &&
        e->as.memcall.target->as.ident.name &&
        strcmp(e->as.memcall.target->as.ident.name, name) == 0 && method &&
        (strcmp(method, "append") == 0 || strcmp(method, "extend") == 0 ||
         strcmp(method, "push") == 0 || strcmp(method, "pop") == 0 ||
         strcmp(method, "slice") == 0 || strcmp(method, "clear") == 0 ||
         strcmp(method, "sort") == 0 || strcmp(method, "sorted") == 0 ||
         strcmp(method, "clone") == 0 || strcmp(method, "insert") == 0))
      return true;
    if (nyr_expr_uses_list_surface(e->as.memcall.target, name, depth + 1))
      return true;
    for (size_t i = 0; i < e->as.memcall.args.len; ++i)
      if (nyr_expr_uses_list_surface(e->as.memcall.args.data[i].val, name,
                                     depth + 1))
        return true;
    return false;
  }
  case NY_E_CALL:
    if (e->as.call.callee && e->as.call.callee->kind == NY_E_MEMBER &&
        e->as.call.callee->as.member.target &&
        e->as.call.callee->as.member.target->kind == NY_E_IDENT &&
        e->as.call.callee->as.member.target->as.ident.name &&
        strcmp(e->as.call.callee->as.member.target->as.ident.name, name) == 0 &&
        e->as.call.callee->as.member.name &&
        (strcmp(e->as.call.callee->as.member.name, "append") == 0 ||
         strcmp(e->as.call.callee->as.member.name, "push") == 0 ||
         strcmp(e->as.call.callee->as.member.name, "pop") == 0 ||
         strcmp(e->as.call.callee->as.member.name, "slice") == 0))
      return true;
    if (nyr_expr_uses_list_surface(e->as.call.callee, name, depth + 1))
      return true;
    for (size_t i = 0; i < e->as.call.args.len; ++i)
      if (nyr_expr_uses_list_surface(e->as.call.args.data[i].val, name,
                                     depth + 1))
        return true;
    return false;
  case NY_E_BINARY:
    return nyr_expr_uses_list_surface(e->as.binary.left, name, depth + 1) ||
           nyr_expr_uses_list_surface(e->as.binary.right, name, depth + 1);
  case NY_E_UNARY:
    return nyr_expr_uses_list_surface(e->as.unary.right, name, depth + 1);
  case NY_E_INDEX:
    if (e->as.index.target && e->as.index.target->kind == NY_E_IDENT &&
        e->as.index.target->as.ident.name &&
        strcmp(e->as.index.target->as.ident.name, name) == 0)
      return true;
    return nyr_expr_uses_list_surface(e->as.index.target, name, depth + 1) ||
           nyr_expr_uses_list_surface(e->as.index.start, name, depth + 1) ||
           nyr_expr_uses_list_surface(e->as.index.stop, name, depth + 1) ||
           nyr_expr_uses_list_surface(e->as.index.step, name, depth + 1);
  case NY_E_MEMBER:
    return nyr_expr_uses_list_surface(e->as.member.target, name, depth + 1);
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
    for (size_t i = 0; i < e->as.list_like.len; ++i)
      if (nyr_expr_uses_list_surface(e->as.list_like.data[i], name, depth + 1))
        return true;
    return false;
  default:
    return false;
  }
}

static bool nyr_stmt_uses_list_surface(const stmt_t *s, const char *name,
                                       unsigned depth) {
  if (!s || !name || depth > 64)
    return false;
  switch (s->kind) {
  case NY_S_EXPR:
    return nyr_expr_uses_list_surface(s->as.expr.expr, name, depth + 1);
  case NY_S_RETURN:
    return nyr_expr_uses_list_surface(s->as.ret.value, name, depth + 1);
  case NY_S_VAR:
    for (size_t i = 0; i < s->as.var.exprs.len; ++i)
      if (nyr_expr_uses_list_surface(s->as.var.exprs.data[i], name, depth + 1))
        return true;
    return false;
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      if (nyr_stmt_uses_list_surface(s->as.block.body.data[i], name, depth + 1))
        return true;
    return false;
  case NY_S_WHILE:
    return nyr_expr_uses_list_surface(s->as.whl.test, name, depth + 1) ||
           nyr_stmt_uses_list_surface(s->as.whl.body, name, depth + 1) ||
           nyr_stmt_uses_list_surface(s->as.whl.update, name, depth + 1) ||
           nyr_stmt_uses_list_surface(s->as.whl.init, name, depth + 1);
  case NY_S_IF:
    return nyr_expr_uses_list_surface(s->as.iff.test, name, depth + 1) ||
           nyr_stmt_uses_list_surface(s->as.iff.conseq, name, depth + 1) ||
           nyr_stmt_uses_list_surface(s->as.iff.alt, name, depth + 1) ||
           nyr_stmt_uses_list_surface(s->as.iff.init, name, depth + 1);
  case NY_S_FOR:
    return nyr_expr_uses_list_surface(s->as.fr.iterable, name, depth + 1) ||
           nyr_expr_uses_list_surface(s->as.fr.cond, name, depth + 1) ||
           nyr_stmt_uses_list_surface(s->as.fr.init, name, depth + 1) ||
           nyr_stmt_uses_list_surface(s->as.fr.update, name, depth + 1) ||
           nyr_stmt_uses_list_surface(s->as.fr.body, name, depth + 1);
  case NY_S_GUARD:
    return nyr_expr_uses_list_surface(s->as.guard.value, name, depth + 1) ||
           nyr_stmt_uses_list_surface(s->as.guard.fallback, name, depth + 1);
  case NY_S_TRY:
    return nyr_stmt_uses_list_surface(s->as.tr.body, name, depth + 1) ||
           nyr_stmt_uses_list_surface(s->as.tr.handler, name, depth + 1);
  case NY_S_MODULE:
    for (size_t i = 0; i < s->as.module.body.len; ++i)
      if (nyr_stmt_uses_list_surface(s->as.module.body.data[i], name,
                                     depth + 1))
        return true;
    return false;
  default:
    return false;
  }
}

static bool nyr_param_is_inferred_list(const stmt_t *fn, size_t index) {
  if (!fn || fn->kind != NY_S_FUNC || index >= fn->as.fn.params.len)
    return false;
  const char *type = fn->as.fn.params.data[index].type;
  const char *name = fn->as.fn.params.data[index].name;
  /*
   * Bare `list`/`seq` annotations are still polymorphic: they specify the
   * container kind but not its element representation.  Treat them like an
   * inferred sequence so list<int>, list<f64>, strings, and bytes can cross
   * the same generic function boundary without selecting an ABI from the
   * first call site.
   */
  bool unparameterized =
      !type || strcmp(type, "list") == 0 || strcmp(type, "seq") == 0;
  return name && unparameterized &&
         (nyr_stmt_iterates_param(fn->as.fn.body, name, 0) ||
          nyr_stmt_uses_list_surface(fn->as.fn.body, name, 0));
}

/*
 * An unannotated parameter used through sequence syntax is polymorphic at
 * the source level.  Its ABI must therefore carry the ordinary dynamic
 * value/length/tag triple: a later call is allowed to pass a list, string, or
 * bytes value.  Keeping the old two-word list ABI here made the first call
 * site decide the machine representation for every later call.
 */
static bool nyr_param_uses_dynamic_sequence_abi(const stmt_t *fn,
                                                size_t index) {
  return fn && fn->kind == NY_S_FUNC && index < fn->as.fn.params.len &&
         !(fn->as.fn.is_variadic && index + 1 == fn->as.fn.params.len) &&
         (!fn->as.fn.params.data[index].type ||
          strcmp(fn->as.fn.params.data[index].type, "list") == 0 ||
          strcmp(fn->as.fn.params.data[index].type, "seq") == 0) &&
         nyr_param_is_inferred_list(fn, index);
}

/*
 * A fallible operation protected by try/catch is intentionally a dynamic
 * type test: code such as `try { mapcat(f, x) } catch { [x] }` must accept
 * both sequences and scalar leaves.  HM may still infer a concrete sequence
 * from the protected call, but using that fact as the native ABI would make
 * the catch path unreachable (or reinterpret scalar callback words as list
 * handles).  Keep unannotated parameters dynamic when they are observed in a
 * try body; explicit source types remain authoritative.
 */
static bool nyr_stmt_try_uses_ident(const stmt_t *s, const char *name,
                                    unsigned depth) {
  if (!s || !name || depth > 64)
    return false;
  switch (s->kind) {
  case NY_S_TRY:
    if (s->as.tr.body && nyr_stmt_uses_ident(s->as.tr.body, name))
      return true;
    return nyr_stmt_try_uses_ident(s->as.tr.handler, name, depth + 1);
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      if (nyr_stmt_try_uses_ident(s->as.block.body.data[i], name, depth + 1))
        return true;
    return false;
  case NY_S_IF:
    return nyr_stmt_try_uses_ident(s->as.iff.init, name, depth + 1) ||
           nyr_stmt_try_uses_ident(s->as.iff.conseq, name, depth + 1) ||
           nyr_stmt_try_uses_ident(s->as.iff.alt, name, depth + 1);
  case NY_S_WHILE:
    return nyr_stmt_try_uses_ident(s->as.whl.init, name, depth + 1) ||
           nyr_stmt_try_uses_ident(s->as.whl.body, name, depth + 1) ||
           nyr_stmt_try_uses_ident(s->as.whl.update, name, depth + 1);
  case NY_S_FOR:
    return nyr_stmt_try_uses_ident(s->as.fr.init, name, depth + 1) ||
           nyr_stmt_try_uses_ident(s->as.fr.body, name, depth + 1) ||
           nyr_stmt_try_uses_ident(s->as.fr.update, name, depth + 1);
  case NY_S_GUARD:
    return nyr_stmt_try_uses_ident(s->as.guard.fallback, name, depth + 1);
  case NY_S_MODULE:
    for (size_t i = 0; i < s->as.module.body.len; ++i)
      if (nyr_stmt_try_uses_ident(s->as.module.body.data[i], name, depth + 1))
        return true;
    return false;
  default:
    return false;
  }
}

#define NY_TRY_DYNAMIC_CACHE_MAX 4096u

static bool nyr_param_is_try_dynamic(const nyr_builder_impl_t *b,
                                     const stmt_t *fn, size_t index) {
  if (!fn || fn->kind != NY_S_FUNC || index >= fn->as.fn.params.len ||
      fn->as.fn.params.data[index].type)
    return false;

  /*
   * This fact is queried from function ABI setup and from every call edge.
   * Scanning the callee AST at each call site turns import-heavy modules into
   * quadratic work, so memoize by the immutable function node + parameter.
   * Store the result on the shared builder owner so it cannot outlive the
   * AST/program snapshot that established it.
   */
  uintptr_t key = ((uintptr_t)fn >> 4) ^ (index * UINT64_C(0x9e3779b97f4a7c15));
  size_t slot = (size_t)(key % NY_TRY_DYNAMIC_CACHE_MAX);
  nyr_builder_impl_t *owner =
      b && b->cache_owner ? b->cache_owner : (nyr_builder_t *)b;
  if (!owner)
    goto uncached;
  owner->cache_owner = owner;
  if (!owner->try_dynamic_cache) {
    owner->try_dynamic_cache =
        calloc(NY_TRY_DYNAMIC_CACHE_MAX, sizeof(*owner->try_dynamic_cache));
    if (owner->try_dynamic_cache)
      owner->try_dynamic_cache_cap = NY_TRY_DYNAMIC_CACHE_MAX;
  }
  if (!owner->try_dynamic_cache)
    goto uncached;
  for (size_t probe = 0; probe < NY_TRY_DYNAMIC_CACHE_MAX; ++probe) {
    ny_try_dynamic_cache_ent_t *ent =
        &owner->try_dynamic_cache[(slot + probe) % NY_TRY_DYNAMIC_CACHE_MAX];
    if (ent->fn == fn && ent->index == index)
      return ent->value;
    if (!ent->fn) {
      const char *name = fn->as.fn.params.data[index].name;
      ent->fn = fn;
      ent->index = index;
      ent->value = name && nyr_stmt_try_uses_ident(fn->as.fn.body, name, 0);
      return ent->value;
    }
  }

  /*
   * Cache saturation is harmless: retain correctness and only lose speed.
   */
uncached:;
  const char *name = fn->as.fn.params.data[index].name;
  return name && nyr_stmt_try_uses_ident(fn->as.fn.body, name, 0);
}

/*
 * The native float predicates consult the shared semantic layer before the
 * spelling check. `number`/`numeric` own the machine-f64 scalar ABI by
 * declaration, so every ABI decision driven by these helpers treats a
 * `-> number` result like f64 instead of falling through to the raw-int or
 * tagged path. Aggregate extended floats (f128) stay non-scalar.
 */
static bool ny_native_type_name_is_f64(const char *name) {
  return ny_type_name_is_f64(name) ||
         ny_semantic_rep_for_type(name) == NY_SEM_REP_F64;
}

static bool ny_native_type_name_is_f32(const char *name) {
  return ny_type_name_is_f32(name) ||
         ny_semantic_rep_for_type(name) == NY_SEM_REP_F32;
}

static bool ny_native_type_name_is_int(const char *name) {
  return ny_type_name_is_int(name);
}

static bool ny_native_type_name_is_list(const char *name) {
  return ny_type_name_is_list(name);
}

static bool ny_native_type_name_is_str(const char *name) {
  return ny_type_name_is_str(name);
}

static bool ny_native_type_name_is_bytes(const char *name) {
  return ny_type_name_is_bytes(name);
}

static bool ny_native_type_name_is_any(const char *name) {
  return ny_type_name_is_any(name);
}

static bool ny_native_type_name_is_ptr(const char *name) {
  return ny_type_name_is_ptr(name);
}

/*
 * Call-result f64 classification shared by every callee-contract site: the
 * declared return annotation stays authoritative, and a resolved semantic
 * fact may only refine it while it remains coherent with that annotation.
 */
static bool nyr_fn_return_is_f64(const stmt_t *fn) {
  const char *rtype = fn->as.fn.return_type;
  const ny_expr_semantic_t *rsem = &fn->as.fn.return_semantic;
  return ny_native_type_name_is_f64(rtype) ||
         (rsem->resolved && rsem->rep == NY_SEM_REP_F64 &&
          ny_sem_rep_coherent_with_type(rtype, rsem->rep));
}

/*
 * Thread and async trampolines return through a dynamic-value boundary.
 * Result kinds: raw scalar integer, canonical dynamic value, and raw bool.
 */
static int nyr_fn_result_kind(nyr_builder_impl_t *b, const stmt_t *fn) {
  if (!fn || fn->kind != NY_S_FUNC)
    return 0;
  if (ny_type_name_is_bool(fn->as.fn.return_type))
    return 2;
  /*
   * Unannotated lambda bodies can still have a statically known boolean
   * result. Preserve that raw 0/1 callback ABI when the closure escapes into
   * an `any` container; otherwise the dynamic trampoline treats raw true (1)
   * as the tagged integer zero.
   */
  if (b && !fn->as.fn.return_type && fn->as.fn.body) {
    const stmt_t *body = fn->as.fn.body;
    const expr_t *tail = NULL;
    if (body->kind == NY_S_EXPR)
      tail = body->as.expr.expr;
    else if (body->kind == NY_S_BLOCK && body->as.block.body.len > 0) {
      const stmt_t *last =
          body->as.block.body.data[body->as.block.body.len - 1];
      if (last && last->kind == NY_S_EXPR)
        tail = last->as.expr.expr;
      else if (last && last->kind == NY_S_RETURN)
        tail = last->as.ret.value;
    }
    if (tail && nyr_expr_is_bool(b, tail))
      return 2;
    /*
     * Parser/query callbacks frequently return dictionaries, sequences, or
     * values fetched from an `any` container without an explicit return
     * annotation. Their trampoline must preserve the tagged object ABI.
     */
    if (tail && (nyr_expr_is_dict(b, tail) || nyr_expr_is_list(b, tail) ||
                 nyr_expr_is_any(b, (expr_t *)tail)))
      return 1;
  }
  if (fn->as.fn.return_semantic.resolved) {
    ny_sem_rep_t rep = fn->as.fn.return_semantic.rep;
    if (rep == NY_SEM_REP_TAGGED_DYNAMIC || rep == NY_SEM_REP_STRING ||
        rep == NY_SEM_REP_TYPED_BUFFER || rep == NY_SEM_REP_OBJECT ||
        rep == NY_SEM_REP_POINTER || rep == NY_SEM_REP_CLOSURE ||
        rep == NY_SEM_REP_AGGREGATE)
      return 1;
    return 0;
  }
  const char *type = fn->as.fn.return_type;
  if (ny_native_type_name_is_any(type) || ny_native_type_name_is_str(type) ||
      ny_native_type_name_is_list(type) || ny_native_type_name_is_bytes(type) ||
      ny_native_type_name_is_ptr(type) ||
      (type && (strcmp(type, "dict") == 0 || strcmp(type, "set") == 0 ||
                strcmp(type, "tuple") == 0)))
    return 1;
  return 0;
}

typedef struct nyr_param_abi_t {
  ny_sem_rep_t rep;
  nyr_param_type_t value_type;
  size_t extra_slots;
  bool try_dynamic;
  bool inferred_list;
  bool dynamic_sequence;
  bool is_list;
  bool is_str;
  bool is_any;
} nyr_param_abi_t;

static bool nyr_param_abi(const nyr_builder_impl_t *b, const stmt_t *fn,
                          size_t index, nyr_param_abi_t *out) {
  if (!fn || fn->kind != NY_S_FUNC || index >= fn->as.fn.params.len || !out)
    return false;
  const param_t *param = &fn->as.fn.params.data[index];
  const char *type = param->type;
  bool try_dynamic = nyr_param_is_try_dynamic(b, fn, index);
  bool inferred_list = !try_dynamic && nyr_param_is_inferred_list(fn, index);
  bool dynamic_sequence =
      !try_dynamic && nyr_param_uses_dynamic_sequence_abi(fn, index);
  ny_sem_rep_t rep = try_dynamic
                         ? NY_SEM_REP_TAGGED_DYNAMIC
                         : (param->semantic.resolved ? param->semantic.rep
                                                     : NY_SEM_REP_UNRESOLVED);
  if (!try_dynamic && type && *type)
    rep = nyr_rep_for_type(b, type);
  bool is_list = !try_dynamic && !dynamic_sequence &&
                 (ny_native_type_name_is_list(type) || inferred_list ||
                  (!type && rep == NY_SEM_REP_TYPED_BUFFER));
  bool is_str = !try_dynamic && (ny_native_type_name_is_str(type) ||
                                 (!type && rep == NY_SEM_REP_STRING));
  bool is_any = try_dynamic || dynamic_sequence ||
                ny_native_type_name_is_any(type) ||
                (!type && !is_list && !is_str && rep != NY_SEM_REP_RAW_INT &&
                 rep != NY_SEM_REP_F64 && rep != NY_SEM_REP_F32);
  nyr_param_type_t value_type =
      (ny_native_type_name_is_f64(type) || (!type && rep == NY_SEM_REP_F64))
          ? NYIR_PARAM_F64
      : (ny_native_type_name_is_f32(type) || (!type && rep == NY_SEM_REP_F32))
          ? NYIR_PARAM_F32
          : NYIR_PARAM_I64;
  *out = (nyr_param_abi_t){
      .rep = rep,
      .value_type = value_type,
      .extra_slots = is_list ? 1 : ((is_str || is_any) ? 2 : 0),
      .try_dynamic = try_dynamic,
      .inferred_list = inferred_list,
      .dynamic_sequence = dynamic_sequence,
      .is_list = is_list,
      .is_str = is_str,
      .is_any = is_any,
  };
  return true;
}

static bool nyr_set_param_types(nyr_builder_impl_t *b, const stmt_t *fn) {
  if (!b || !fn || fn->kind != NY_S_FUNC)
    return false;
  if (fn->as.fn.return_semantic.resolved &&
      !ny_sem_rep_coherent_with_type(fn->as.fn.return_type,
                                     fn->as.fn.return_semantic.rep))
    return nyr_fail(b,
                    "native NYIR lower: function return type and semantic ABI "
                    "representation disagree");
  for (size_t i = 0; i < fn->as.fn.params.len; ++i) {
    const param_t *param = &fn->as.fn.params.data[i];
    if (param->semantic.resolved &&
        !ny_sem_rep_coherent_with_type(param->type, param->semantic.rep))
      return nyr_fail(b, "native NYIR lower: parameter type and semantic ABI "
                         "representation disagree");
  }
  size_t source_count = fn->as.fn.params.len;
  const ny_native_lambda_entry_t *lambda =
      ny_native_lambda_find_name(ny_native_lambda_cache_for(b), fn->as.fn.name);
  bool hidden_env = lambda && lambda->capture_count > 0;
  size_t count = source_count + (hidden_env ? 1 : 0);
  nyr_param_abi_t *param_abis =
      source_count ? calloc(source_count, sizeof(*param_abis)) : NULL;
  if (source_count && !param_abis)
    return nyr_fail(b, NY_NATIVE_ALLOC_FAIL);
  for (size_t i = 0; i < source_count; ++i) {
    if (!nyr_param_abi(b, fn, i, &param_abis[i])) {
      free(param_abis);
      return nyr_fail(b, "native NYIR lower: cannot resolve parameter ABI");
    }
    if (param_abis[i].extra_slots > SIZE_MAX - count) {
      free(param_abis);
      return nyr_fail(b, "native NYIR lower: parameter ABI is too large");
    }
    count += param_abis[i].extra_slots;
  }
  if (!count) {
    free(param_abis);
    return true;
  }
  b->nyir.param_types = calloc(count, sizeof(*b->nyir.param_types));
  if (!b->nyir.param_types) {
    free(param_abis);
    return nyr_fail(b, NY_NATIVE_ALLOC_FAIL);
  }
  b->nyir.param_count = count;
  if (source_count) {
    b->nyir.source_param_widths = calloc(source_count, sizeof(uint8_t));
    if (!b->nyir.source_param_widths) {
      free(param_abis);
      return nyr_fail(b, NY_NATIVE_ALLOC_FAIL);
    }
    b->nyir.source_param_count = source_count;
  }
  size_t abi_index = 0;
  if (hidden_env)
    b->nyir.param_types[abi_index++] = NYIR_PARAM_I64;
  for (size_t i = 0; i < source_count; ++i) {
    const nyr_param_abi_t *param_abi = &param_abis[i];
    b->nyir.source_param_widths[i] = (uint8_t)(1 + param_abi->extra_slots);
    b->nyir.param_types[abi_index++] = param_abi->value_type;
    if (param_abi->is_list)
      b->nyir.param_types[abi_index++] = NYIR_PARAM_I64;
    else if (param_abi->is_str || param_abi->is_any) {
      b->nyir.param_types[abi_index++] = NYIR_PARAM_I64;
      b->nyir.param_types[abi_index++] = NYIR_PARAM_I64;
    }
  }
  NY_COMPILER_ASSERT(
      abi_index == count,
      "native parameter count disagrees with parameter ABI slots");
  free(param_abis);
  return true;
}

static int64_t ny_native_f64_bits(double v) { return nyr_f64_to_bits(v); }

static int64_t ny_native_f32_bits(float v) {
  uint32_t bits = 0;
  memcpy(&bits, &v, sizeof(bits));
  return (int64_t)bits;
}

static nyr_local_t *nyr_bind_local_typed(nyr_builder_impl_t *b,
                                         const char *name, bool is_f64,
                                         bool is_f32, bool is_cstr) {
  if (!name || !name[0] || strcmp(name, "_") == 0)
    return NULL;
  if (b->local_count == b->local_cap) {
    size_t cap = b->local_cap ? b->local_cap * 2 : 64;
    if (cap < b->local_cap || cap > SIZE_MAX / sizeof(*b->locals)) {
      nyr_fail(b, "native NYIR lower: local table is too large");
      return NULL;
    }
    nyr_local_t *locals = realloc(b->locals, cap * sizeof(*locals));
    if (!locals) {
      nyr_fail(b, NY_NATIVE_ALLOC_FAIL);
      return NULL;
    }
    b->locals = locals;
    b->local_cap = cap;
  }
  nyr_local_t *l = &b->locals[b->local_count];
  *l = (nyr_local_t){.name = name,
                     .slot = b->next_local_slot++,
                     .semantic_rep =
                         is_f64 ? NY_SEM_REP_F64
                                : (is_f32 ? NY_SEM_REP_F32
                                          : (is_cstr ? NY_SEM_REP_STRING
                                                     : NY_SEM_REP_UNRESOLVED)),
                     .is_f64 = is_f64,
                     .is_f32 = is_f32,
                     .is_cstr = is_cstr,
                     .alias_count = 1,
                     .is_bytes = false,
                     .callable_expr = NULL,
                     .callable_name = NULL,
                     .sb_slot = -1,
                     .list_literal = NULL,
                     .dict_literal = NULL,
                     .list_len_slot = -1,
                     .dyn_str_len_slot = -1,
                     .dyn_tag_slot = -1,
                     .arg_slot = -1,
                     .list_len_arg_slot = -1,
                     .dyn_str_len_arg_slot = -1,
                     .dyn_tag_arg_slot = -1,
                     .syntax_ctx = b ? b->current_syntax_ctx : 0};
  b->local_count++;
  return l;
}

static nyr_local_t *nyr_bind_local(nyr_builder_impl_t *b, const char *name) {
  return nyr_bind_local_typed(b, name, false, false, false);
}

static nyr_local_t *nyr_add_local(nyr_builder_impl_t *b, const char *name) {
  if (!name || !name[0] || strcmp(name, "_") == 0)
    return NULL;
  nyr_local_t *old = nyr_find_local_nearest(b, name);
  if (old)
    return old;
  /*
   * Assignments do not introduce a lexical binding.  A statement nested in
   * a syntax context may therefore update an outer local whose context token
   * differs from the assignment's token.  Reuse the nearest existing name
   * before allocating a fresh slot, while declarations still go through the
   * context-sensitive binder above.
   */
  if (b) {
    for (size_t i = b->local_count; i > 0; --i) {
      nyr_local_t *candidate = &b->locals[i - 1];
      if (candidate->name && strcmp(candidate->name, name) == 0)
        return candidate;
    }
  }
  return nyr_bind_local(b, name);
}

/*
 * Compact NYIR emit helpers: one push path for control/value forms.
 */
static bool nyr_push_ctrl(nyr_builder_impl_t *b, nyr_op_t op, int a,
                          int64_t imm) {
  size_t before = b->nyir.len;
  nyr_ir_emit(&b->nyir,
            (nyr_inst_t){.op = op, .dst = -1, .a = a, .b = -1, .imm = imm});
  return b->nyir.len != before || nyr_fail(b, NY_NATIVE_ALLOC_FAIL);
}

static int nyr_push_val(nyr_builder_impl_t *b, nyr_op_t op, int a, int b_arg,
                        int64_t imm, const char *symbol) {
  int v = nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = op,
                                            .dst = -1,
                                            .a = a,
                                            .b = b_arg,
                                            .imm = imm,
                                            .symbol = symbol});
  if (v < 0)
    nyr_fail(b, NY_NATIVE_ALLOC_FAIL);
  return v;
}

static bool nyr_emit_label(nyr_builder_impl_t *b, int label) {
  return nyr_push_ctrl(b, NYIR_LABEL, -1, label);
}
static bool nyr_emit_br(nyr_builder_impl_t *b, int label) {
  return nyr_push_ctrl(b, NYIR_BR, -1, label);
}
static int nyr_emit_const(nyr_builder_impl_t *b, int64_t value);
static int nyr_load_local_value(nyr_builder_impl_t *b, int slot);
static int nyr_emit_enum_tag(nyr_builder_impl_t *b, const stmt_t *enum_stmt,
                             int64_t tag) {
  int value = nyr_emit_const(b, tag);
  if (value < 0 || !enum_stmt || enum_stmt->kind != NY_S_ENUM)
    return value;
  for (size_t i = 0; i < enum_stmt->as.enu.items.len; ++i)
    if (enum_stmt->as.enu.items.data[i].fields.len)
      return value;
  return nyr_ir_emit(&b->nyir,
                   (nyr_inst_t){.op = NYIR_COPY,
                                 .dst = -1,
                                 .a = value,
                                 .b = -1,
                                 .scalar_class = NYIR_SCALAR_I64_RAW});
}

static inline int nyr_emit_binop(nyr_builder_impl_t *b, nyr_op_t op, int a,
                                 int rhs);
static inline int nyr_emit_cmp_i64(nyr_builder_impl_t *b, nyr_cmp_t cmp, int a,
                                   int rhs) {
  int v = nyr_ir_emit(
      &b->nyir,
      (nyr_inst_t){
          .op = NYIR_CMP_I64, .dst = -1, .a = a, .b = rhs, .cmp = cmp});
  if (v < 0)
    nyr_fail(b, NY_NATIVE_ALLOC_FAIL);
  return v;
}

static int nyr_box_bool(nyr_builder_impl_t *b, int reg) {
  if (!b || reg < 0)
    return -1;
  for (size_t i = b->nyir.len; i > 0; --i) {
    const nyr_inst_t *in = &b->nyir.data[i - 1];
    if (in->dst == reg) {
      if (in->op == NYIR_CONST_I64) {
        bool truthy = in->imm != 0 && in->imm != NY_IMM_FALSE;
        return nyr_emit_const(b, truthy ? NY_IMM_TRUE : NY_IMM_FALSE);
      }
      break;
    }
  }
  int zero = nyr_emit_const(b, 0);
  int false_imm = nyr_emit_const(b, NY_IMM_FALSE);
  int six = nyr_emit_const(b, 6);
  if (zero < 0 || false_imm < 0 || six < 0)
    return -1;
  int got = nyr_emit_cmp_i64(b, NYIR_CMP_NE, reg, zero);
  int not_false = nyr_emit_cmp_i64(b, NYIR_CMP_NE, reg, false_imm);
  int truthy = nyr_emit_binop(b, NYIR_AND_I64, got, not_false);
  int mul = nyr_emit_binop(b, NYIR_MUL_I64, truthy, six);
  return nyr_emit_binop(b, NYIR_ADD_I64, mul, false_imm);
}

static bool nyr_def_of(const nyr_builder_impl_t *b, int value,
                       const nyr_inst_t **out) {
  if (!b || value < 0)
    return false;
  for (size_t i = b->nyir.len; i > 0; --i) {
    if (b->nyir.data[i - 1].dst == value) {
      *out = &b->nyir.data[i - 1];
      return true;
    }
  }
  return false;
}

/*
 * Apply a proven source-local ABI fact to a LOAD_LOCAL producer. This is
 * intentionally limited to local loads and COPY chains; a semantic hint
 * must never relabel arithmetic or a runtime call's actual result.
 */
static void nyr_stamp_local_load_class(nyr_builder_impl_t *b, int value,
                                       nyr_scalar_class_t scalar_class) {
  if (!b || value < 0 || scalar_class == NYIR_SCALAR_UNKNOWN)
    return;
  for (size_t i = b->nyir.len; i > 0; --i) {
    nyr_inst_t *in = &b->nyir.data[i - 1];
    if (in->dst != value)
      continue;
    if (in->op == NYIR_LOAD_LOCAL)
      in->scalar_class = scalar_class;
    else if (in->op == NYIR_COPY)
      nyr_stamp_local_load_class(b, in->a, scalar_class);
    return;
  }
}

static bool nyr_const_is(const nyr_builder_impl_t *b, int reg, int64_t imm) {
  const nyr_inst_t *in = NULL;
  return nyr_def_of(b, reg, &in) && in->op == NYIR_CONST_I64 && in->imm == imm;
}

/*
 * Recognize the defining shape nyr_box_bool emits for a boxed
 * boolean: AND of (!= 0) and (!= NY_IMM_FALSE), times 6, plus NY_IMM_FALSE.
 * Branch lowering must normalize these to 0/1 before NYIR_BR_IF, whose
 * contract is a raw non-zero test that would otherwise read boxed false
 * (NY_IMM_FALSE == 2) as truthy.
 */
static bool nyr_is_boxed_bool_value(const nyr_builder_impl_t *b, int value) {
  const nyr_inst_t *add = NULL;
  const nyr_inst_t *mul = NULL;
  if (!nyr_def_of(b, value, &add) || add->op != NYIR_ADD_I64)
    return false;
  if (!nyr_const_is(b, add->b, NY_IMM_FALSE))
    return false;
  if (!nyr_def_of(b, add->a, &mul) || mul->op != NYIR_MUL_I64)
    return false;
  return nyr_const_is(b, mul->b, 6);
}

static bool nyr_value_is_dynamic(const nyr_builder_impl_t *b, int value) {
  if (!b || value < 0)
    return false;
  if (nyr_is_boxed_bool_value(b, value))
    return true;
  for (size_t i = b->nyir.len; i > 0; --i) {
    const nyr_inst_t *in = &b->nyir.data[i - 1];
    if (in->dst == value) {
      if (in->op == NYIR_CALL)
        return true;
      if (in->op == NYIR_CONST_I64 &&
          (in->imm == NY_IMM_FALSE || in->imm == NY_IMM_TRUE))
        return true;
      if (in->op == NYIR_LOAD_LOCAL && in->imm >= 0) {
        for (size_t j = b->local_count; j > 0; --j) {
          if (b->locals[j - 1].slot == in->imm)
            return b->locals[j - 1].is_any || b->locals[j - 1].is_bool;
        }
      }
      return false;
    }
  }
  return false;
}

static nyr_scalar_class_t nyr_value_scalar_class(const nyr_builder_impl_t *b,
                                                  int value);

static int nyr_emit_truthy(nyr_builder_impl_t *b, int value) {
  if (value < 0)
    return -1;
  nyr_scalar_class_t scalar_class = nyr_value_scalar_class(b, value);
  bool dynamic = nyr_value_is_dynamic(b, value);
  int truthy = nyr_emit_cmp_i64(b, NYIR_CMP_NE, value, nyr_emit_const(b, 0));
  if (truthy < 0)
    return -1;
  if (dynamic || scalar_class == NYIR_SCALAR_I64_TAGGED) {
    int not_false = nyr_emit_cmp_i64(b, NYIR_CMP_NE, value,
                                     nyr_emit_const(b, NY_IMM_FALSE));
    if (not_false < 0)
      return -1;
    truthy = nyr_emit_binop(b, NYIR_AND_I64, truthy, not_false);
    if (truthy < 0)
      return -1;
  }
  /*
   * Tagged integer zero is 1, whereas a comparison's raw true is also 1.
   * Only the tagged carrier may test that word as false.
   */
  bool tagged_integer = dynamic && scalar_class == NYIR_SCALAR_I64_TAGGED;
  const nyr_inst_t *producer = NULL;
  if (dynamic && nyr_def_of(b, value, &producer) &&
      producer->op == NYIR_LOAD_LOCAL) {
    for (size_t i = b->local_count; i > 0; --i) {
      const nyr_local_t *local = &b->locals[i - 1];
      if (local->slot == producer->imm) {
        tagged_integer = local->is_any && !local->is_bool;
        break;
      }
    }
  }
  if (tagged_integer) {
    int not_zero_integer =
        nyr_emit_cmp_i64(b, NYIR_CMP_NE, value, nyr_emit_const(b, 1));
    if (not_zero_integer < 0)
      return -1;
    truthy = nyr_emit_binop(b, NYIR_AND_I64, truthy, not_zero_integer);
  }
  return truthy;
}

static bool nyr_emit_br_if(nyr_builder_impl_t *b, int value, int label) {
  int truthy = nyr_emit_truthy(b, value);
  return truthy >= 0 && nyr_push_ctrl(b, NYIR_BR_IF, truthy, label);
}

static bool nyr_emit_cmp_br_if(nyr_builder_impl_t *b, int predicate,
                               int label) {
  /*
   * Comparison results are canonical i64 booleans; avoid re-testing them
   * through the dynamic truthiness path so the backend can fuse CMP/BR_IF.
   */
  return predicate >= 0 && nyr_push_ctrl(b, NYIR_BR_IF, predicate, label);
}

static int nyr_emit_runtime_call(nyr_builder_impl_t *b, const char *symbol,
                                 int a, int b_arg, int c, int argc,
                                 nyr_scalar_class_t return_class);

/*
 * Five-value variant used by the indirect-closure dispatch (rt_call_any3 /
 * rt_call_any4 forward env + four values to the closure body).
 */
static int nyr_emit_runtime_call4(nyr_builder_impl_t *b, const char *symbol,
                                  int a, int b_arg, int c, int d, int e_arg,
                                  int argc, nyr_scalar_class_t return_class);

static bool nyr_value_is_f64(const nyr_builder_impl_t *b, int value,
                             unsigned depth);
static bool nyr_value_is_f32(const nyr_builder_impl_t *b, int value,
                             unsigned depth);
static nyr_scalar_class_t nyr_value_scalar_class(const nyr_builder_impl_t *b,
                                                  int value);
static int nyr_emit_i64_to_f64(nyr_builder_impl_t *b, int v);
static int nyr_emit_f32_to_f64(nyr_builder_impl_t *b, int value);
static int nyr_emit_f64_to_f32(nyr_builder_impl_t *b, int value);
static int nyr_emit_f64_to_f32(nyr_builder_impl_t *b, int v);
static bool nyr_emit_ret(nyr_builder_impl_t *b, int value) {
  if (!b)
    return false;
  /*
   * `any` functions have an i64 tagged return ABI even when the final
   * expression was lowered directly in the f64/f32 register class. Keep this
   * at the return boundary as well as the ordinary implicit-tail path: some
   * statement/control-flow shapes bypass that path, but must still return a
   * tagged dynamic value.
   */
  if (b->return_any && value >= 0) {
    nyr_scalar_class_t value_class = nyr_value_scalar_class(b, value);
    if (value_class == NYIR_SCALAR_STRING) {
      value = nyr_emit_runtime_call(b, "rt_cstr_to_str", value, -1, -1, 1,
                                    NYIR_SCALAR_STRING);
      if (value < 0)
        return false;
    } else if (value_class == NYIR_SCALAR_F64 ||
               value_class == NYIR_SCALAR_F32) {
      int as_f64 = value_class == NYIR_SCALAR_F32
                       ? nyr_emit_f32_to_f64(b, value)
                       : value;
      int bits = as_f64 < 0 ? -1
                            : nyr_emit_runtime_call(b, "rt_f64_bits", as_f64,
                                                    -1, -1, 1, 0);
      value = bits < 0 ? -1
                       : nyr_emit_runtime_call(b, "rt_flt_box_val", bits, -1,
                                               -1, 1, NYIR_SCALAR_I64_TAGGED);
      if (value < 0)
        return false;
    }
  }
  /*
   * Central return-ABI reconciliation: a function whose return ABI is f64
   * must return an f64-classified register on every path — including cold
   * exits and ternary merges through unclassified temporaries
   * (core.str.atof's `return sign < 0 ? 0.0 - v : v`).  An int-class
   * producer at an f64 ret is a numeric value by contract, so the
   * promotion is always numerically correct.
   */
  if (b->return_class == NYIR_SCALAR_F64 && value >= 0 &&
      !nyr_value_is_f64(b, value, 0) && !nyr_value_is_f32(b, value, 0)) {
    int promoted = nyr_emit_i64_to_f64(b, value);
    if (promoted < 0)
      return false;
    value = promoted;
  }
  /*
   * Same contract at a declared f32 ret: an f64 producer narrows, and an
   * int-class producer promotes through f64 first so the numeric value is
   * preserved.
   */
  if (b->return_class == NYIR_SCALAR_F32 && value >= 0) {
    if (nyr_value_is_f64(b, value, 0)) {
      int narrowed = nyr_emit_f64_to_f32(b, value);
      if (narrowed < 0)
        return false;
      value = narrowed;
    } else if (!nyr_value_is_f32(b, value, 0)) {
      int promoted = nyr_emit_i64_to_f64(b, value);
      if (promoted < 0)
        return false;
      int narrowed = nyr_emit_f64_to_f32(b, promoted);
      if (narrowed < 0)
        return false;
      value = narrowed;
    }
  }
  if (b && b->trace_instrumented &&
      nyr_emit_runtime_call(b, "rt_trace_exit", -1, -1, -1, 0, 0) < 0)
    return false;
  /*
   * A program-level entry (rt_main) has no declared return type, but its
   * result expression can still be a typed float (top-level `x + y` with
   * f32 locals). Stamp the ret with the value's own float class so the ABI
   * contract matches the producer instead of reading as unknown.
   */
  nyr_scalar_class_t ret_class =
      b->return_any ? NYIR_SCALAR_I64_TAGGED : b->return_class;
  if (b->return_any && value >= 0 &&
      nyr_value_scalar_class(b, value) == NYIR_SCALAR_STRING)
    ret_class = NYIR_SCALAR_STRING;
  if (ret_class == NYIR_SCALAR_UNKNOWN && value >= 0) {
    nyr_scalar_class_t value_class = nyr_value_scalar_class(b, value);
    if (value_class == NYIR_SCALAR_F64 || value_class == NYIR_SCALAR_F32)
      ret_class = value_class;
  }
  size_t before = b->nyir.len;
  nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_RET,
                                    .dst = -1,
                                    .a = value,
                                    .b = -1,
                                    .flags = 0,
                                    .scalar_class = ret_class});
  if (b->nyir.len == before)
    return nyr_fail(b, NY_NATIVE_ALLOC_FAIL);
  b->emitted_return = true;
  b->last_value = value;
  return true;
}

static bool nyr_record_dyn_fact(nyr_builder_impl_t *b, int value,
                                nyr_fact_kind_t kind, int reg);
static bool nyr_record_list_len_fact(nyr_builder_impl_t *b, int value,
                                     int length_value);
static int nyr_emit_add_i64(nyr_builder_impl_t *b, int a, int rhs);
static bool nyr_emit_store_i64(nyr_builder_impl_t *b, int addr, int value);
static int nyr_emit_runtime_call(nyr_builder_impl_t *b, const char *symbol,
                                 int a, int b_arg, int c, int argc,
                                 nyr_scalar_class_t return_class);
static int nyr_emit_const(nyr_builder_impl_t *b, int64_t value) {
  return nyr_push_val(b, NYIR_CONST_I64, -1, -1, value, NULL);
}
static int nyr_emit_cstr_const(nyr_builder_impl_t *b, const char *s) {
  size_t len = s ? strlen(s) : 0;
  const char *sym = nyr_strtab_intern(s ? s : "", len, NULL, 0);
  if (!sym) {
    return nyr_fail(b, "native NYIR lower: string table full or OOM"), -1;
  }
  int addr = nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL,
                                               .dst = -1,
                                               .a = -1,
                                               .b = -1,
                                               .imm = 0,
                                               .symbol = sym});
  if (addr < 0) {
    return nyr_fail(b, NY_NATIVE_ALLOC_FAIL), -1;
  }
  int length = nyr_emit_const(b, (int64_t)len);
  int tag = nyr_emit_const(b, TAG_STR_CONST);
  if (length >= 0 && tag >= 0) {
    nyr_record_dyn_fact(b, addr, NYR_FACT_DYN_STR_LEN, length);
    nyr_record_dyn_fact(b, addr, NYR_FACT_DYN_TAG, tag);
  }
  return addr;
}

static int nyr_emit_const_f64(nyr_builder_impl_t *b, double value) {
  return nyr_push_val(b, NYIR_CONST_F64, -1, -1, ny_native_f64_bits(value),
                      NULL);
}
static int nyr_emit_const_f32(nyr_builder_impl_t *b, double value) {
  return nyr_push_val(b, NYIR_CONST_F32, -1, -1,
                      ny_native_f32_bits((float)value), NULL);
}
static int nyr_emit_i64_to_f64(nyr_builder_impl_t *b, int value) {
  return nyr_push_val(b, NYIR_I64_TO_F64, value, -1, 0, NULL);
}
static int nyr_emit_f64_to_i64(nyr_builder_impl_t *b, int value) {
  return nyr_emit_runtime_call(b, "rt_f64_to_i64", value, -1, -1, 1, 0);
}
static __attribute__((unused)) int nyr_emit_f32_to_i64(nyr_builder_impl_t *b,
                                                       int value) {
  int widened = nyr_push_val(b, NYIR_F32_TO_F64, value, -1, 0, NULL);
  if (widened < 0)
    return -1;
  return nyr_emit_f64_to_i64(b, widened);
}
static int nyr_emit_i64_to_f32(nyr_builder_impl_t *b, int value) {
  return nyr_push_val(b, NYIR_I64_TO_F32, value, -1, 0, NULL);
}
static int nyr_emit_f32_to_f64(nyr_builder_impl_t *b, int value) {
  return nyr_push_val(b, NYIR_F32_TO_F64, value, -1, 0, NULL);
}
static int nyr_emit_f64_to_f32(nyr_builder_impl_t *b, int value) {
  return nyr_push_val(b, NYIR_F64_TO_F32, value, -1, 0, NULL);
}
static int nyr_emit_add_i64(nyr_builder_impl_t *b, int a, int rhs) {
  return nyr_push_val(b, NYIR_ADD_I64, a, rhs, 0, NULL);
}
static int nyr_emit_load_i64(nyr_builder_impl_t *b, int addr) {
  return nyr_push_val(b, NYIR_LOAD_I64, addr, -1, 0, NULL);
}
static int64_t nyr_static_dyn_tag(const expr_t *e) {
  if (!e)
    return -1;
  if (e->kind == NY_E_LITERAL) {
    if (e->as.literal.kind == NY_LIT_STR)
      return TAG_STR_CONST;
    if (e->as.literal.kind == NY_LIT_FLOAT)
      return TAG_FLOAT;
    return -1;
  }
  if (e->kind == NY_E_LIST)
    return TAG_LIST;
  if (e->kind == NY_E_DICT)
    return TAG_DICT;
  if (e->kind == NY_E_CALL) {
    const char *leaf = ny_native_call_leaf(e);
    if (leaf && strcmp(leaf, "set") == 0)
      return TAG_SET;
    if (leaf && strcmp(leaf, "dict") == 0)
      return TAG_DICT;
    if (leaf && (strcmp(leaf, "list") == 0 || strcmp(leaf, "tbuf_new") == 0))
      return TAG_LIST;
  }
  return -1;
}
static void nyr_record_global_dyn_tag(nyr_builder_impl_t *b, const char *name,
                                      int value) {
  if (!b || !name || value < 0)
    return;
  const expr_t *init = nyr_find_top_level_value(b, name);
  /*
   * A global declared with an integer literal is a raw slot — the store
   * side already decodes tagged assignments for exactly this shape.  The
   * loaded word must be stamped the same way: recording a dynamic-tag fact
   * here made every read reinterpret the raw cell as an encoded value
   * (`mut j = 1` produced 0/false/1/nil across loop iterations), and
   * string building through `b = b + to_str(j)` concatenated garbage.
   */
  if (init && init->kind == NY_E_LITERAL &&
      init->as.literal.kind == NY_LIT_INT && init->tok.kind != NY_T_NIL &&
      b->nyir.len > 0 && b->nyir.data[b->nyir.len - 1].dst == value) {
    b->nyir.data[b->nyir.len - 1].scalar_class = NYIR_SCALAR_I64_RAW;
    return;
  }
  int64_t raw = nyr_static_dyn_tag(init);
  if (raw < 0)
    return;
  int tag = nyr_emit_const(b, raw);
  if (tag >= 0)
    (void)nyr_record_dyn_fact(b, value, NYR_FACT_DYN_TAG, tag);
}
static int nyr_emit_load8(nyr_builder_impl_t *b, int addr) {
  int value =
      nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                        .dst = -1,
                                        .a = addr,
                                        .b = -1,
                                        .scalar_class = NYIR_SCALAR_I64_RAW,
                                        .flags = NYIR_INST_F_MEM_BYTE});
  if (value < 0)
    nyr_fail(b, NY_NATIVE_ALLOC_FAIL);
  return value;
}

static int nyr_emit_load_f64(nyr_builder_impl_t *b, int addr) {
  int value =
      nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                        .dst = -1,
                                        .a = addr,
                                        .b = -1,
                                        .scalar_class = NYIR_SCALAR_F64});
  if (value < 0)
    nyr_fail(b, NY_NATIVE_ALLOC_FAIL);
  return value;
}
static int nyr_emit_addr_local(nyr_builder_impl_t *b, int slot,
                               const char *symbol) {
  return nyr_push_val(b, NYIR_ADDR_LOCAL, -1, -1, slot, symbol);
}
static bool nyr_emit_store_i64(nyr_builder_impl_t *b, int addr, int value) {
  size_t before = b->nyir.len;
  nyr_ir_emit(&b->nyir,
            (nyr_inst_t){.op = NYIR_STORE_I64,
                          .dst = -1,
                          .a = addr,
                          .b = -1,
                          .c = value,
                          .scalar_class = nyr_value_scalar_class(b, value)});
  return b->nyir.len != before || nyr_fail(b, NY_NATIVE_ALLOC_FAIL);
}
static bool nyr_emit_store8(nyr_builder_impl_t *b, int addr, int value) {
  size_t before = b->nyir.len;
  nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_STORE_I64,
                                    .dst = -1,
                                    .a = addr,
                                    .b = -1,
                                    .c = value,
                                    .flags = NYIR_INST_F_MEM_BYTE});
  return b->nyir.len != before || nyr_fail(b, NY_NATIVE_ALLOC_FAIL);
}

static bool nyr_emit_store_f64(nyr_builder_impl_t *b, int addr, int value) {
  size_t before = b->nyir.len;
  nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_STORE_I64,
                                    .dst = -1,
                                    .a = addr,
                                    .b = -1,
                                    .c = value,
                                    .scalar_class = NYIR_SCALAR_F64});
  return b->nyir.len != before || nyr_fail(b, NY_NATIVE_ALLOC_FAIL);
}

/*
 * Emit a bounds check: verifies (base + offset) is within [base,
 * base+byte_len). Elided at lowering time for Fin-typed indices.
 */
static bool nyr_emit_bounds_check_value(nyr_builder_impl_t *b, int base,
                                        int offset, int byte_len_value,
                                        int64_t byte_len) {
  /*
   * Raw pointers without retained provenance have neither a dynamic nor a
   * static capacity.  A zero static bound is the sentinel for that case.
   */
  if (byte_len_value < 0 && byte_len <= 0)
    return true;
  size_t before = b->nyir.len;
  nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_BOUNDS_CHECK,
                                    .dst = -1,
                                    .a = base,
                                    .b = offset,
                                    .c = byte_len_value,
                                    .imm = byte_len});
  return b->nyir.len != before || nyr_fail(b, NY_NATIVE_ALLOC_FAIL);
}

static bool nyr_emit_bounds_check(nyr_builder_impl_t *b, int base, int offset,
                                  int64_t byte_len) {
  return nyr_emit_bounds_check_value(b, base, offset, -1, byte_len);
}

static nyr_scalar_class_t nyr_value_scalar_class(const nyr_builder_impl_t *b,
                                                  int value);
static bool nyr_value_is_f64(const nyr_builder_impl_t *b, int value,
                             unsigned depth);
static bool nyr_value_is_f32(const nyr_builder_impl_t *b, int value,
                             unsigned depth);
static int nyr_emit_f32_to_f64(nyr_builder_impl_t *b, int v);

static void
nyr_runtime_formal_contracts(const char *symbol, int argc,
                             uint64_t contracts[NYIR_CALL_MAX_ARGS / 32]) {
  if (!symbol || !contracts || argc <= 0)
    return;
  if (argc > NYIR_CALL_MAX_ARGS)
    argc = NYIR_CALL_MAX_ARGS;
  const ny_native_runtime_callback_abi_decl_t *callback_abi =
      ny_native_runtime_callback_abi_decl(symbol);
  const rt_ffi_callback_abi_decl_t *ffi_callback_abi =
      rt_ffi_callback_abi_decl(symbol);
  const ny_native_runtime_arg_abi_decl_t *arg_abi =
      ny_native_runtime_arg_abi_decl(symbol);
  for (size_t i = 0; arg_abi && i < 16 && i < (size_t)argc; ++i) {
    uint8_t contract = ny_native_runtime_arg_formal_contract(arg_abi, i);
    if (contract != NYIR_ARG_CONTRACT_UNKNOWN)
      nyr_call_arg_contract_set_packed(contracts, i, contract);
  }
  if (callback_abi && argc > 0 &&
      callback_abi->callable_ownership == NY_NATIVE_ABI_BORROWED)
    nyr_call_arg_contract_set_packed(contracts, 0, NYIR_ARG_CONTRACT_BORROWS);
  if (callback_abi) {
    for (size_t i = 0; i < callback_abi->source_arity && i + 1 < (size_t)argc;
         ++i) {
      if (callback_abi->arg_ownership == NY_NATIVE_ABI_BORROWED)
        nyr_call_arg_contract_set_packed(contracts, i + 1,
                                          NYIR_ARG_CONTRACT_BORROWS);
    }
  }
  if (ffi_callback_abi && argc > 0 &&
      ffi_callback_abi->callable_ownership ==
          RT_FFI_CALLBACK_OWNERSHIP_BORROWED)
    nyr_call_arg_contract_set_packed(contracts, 0, NYIR_ARG_CONTRACT_BORROWS);
  if (ffi_callback_abi) {
    for (size_t i = 0;
         i < ffi_callback_abi->source_arity && i + 1 < (size_t)argc; ++i) {
      if (ffi_callback_abi->arg_ownership[i] ==
          RT_FFI_CALLBACK_OWNERSHIP_BORROWED)
        nyr_call_arg_contract_set_packed(contracts, i + 1,
                                          NYIR_ARG_CONTRACT_BORROWS);
    }
  }
}

static int nyr_emit_runtime_call(nyr_builder_impl_t *b, const char *symbol,
                                 int a, int b_arg, int c, int argc,
                                 nyr_scalar_class_t return_class) {
  const char *canonical = nyr_runtime_symbol(symbol);
  if (canonical)
    symbol =
        canonical; /*
                    * The managed dictionary bridge consumes a tagged Ny value
                    * in its third slot. Some source paths reach this shared
                    * funnel after local/branch lowering has discarded the AST's
                    * integer fact, so enforce the runtime call contract from
                    * the physical producer class here as well.
                    */
  if (strcmp(symbol, "rt_native_dict_set_nir_i64") == 0 && argc >= 3 &&
      c >= 0 && nyr_value_scalar_class(b, c) == NYIR_SCALAR_I64_RAW) {
    c = nyr_emit_runtime_call(b, "rt_tag", c, -1, -1, 1,
                              NYIR_SCALAR_I64_TAGGED);
    if (c < 0)
      return -1;
    if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
      ny_dbg_heuristic_event(
          "native-abi",
          "function=%s call=rt_native_dict_set_nir_i64 arg=3 actual=i64-raw "
          "formal=i64-tagged action=box-at-runtime-funnel",
          b->current_fn_name ? b->current_fn_name : "<entry>");
  }
  if ((strcmp(symbol, "rt_native_dict_set_nir_i64") == 0 ||
       strcmp(symbol, "rt_native_dict_set_str_compact") == 0) &&
      argc >= 3 && c >= 0 && nyr_value_is_f64(b, c, 0)) {
    int bits = nyr_emit_runtime_call(b, "rt_f64_bits", c, -1, -1, 1,
                                     NYIR_SCALAR_I64_RAW);
    c = bits < 0 ? -1
                 : nyr_emit_runtime_call(b, "rt_flt_box_val", bits, -1, -1, 1,
                                         NYIR_SCALAR_I64_TAGGED);
    if (c < 0)
      return -1;
  }
  if (strcmp(symbol, "rt_tbuf_append_tagged") == 0 && argc >= 2 && b_arg >= 0) {
    if (nyr_value_is_f32(b, b_arg, 0)) {
      b_arg = nyr_emit_f32_to_f64(b, b_arg);
      if (b_arg < 0)
        return -1;
    }
    if (nyr_value_is_f64(b, b_arg, 0)) {
      int bits = nyr_emit_runtime_call(b, "rt_f64_bits", b_arg, -1, -1, 1,
                                       NYIR_SCALAR_I64_RAW);
      b_arg = bits < 0 ? -1
                       : nyr_emit_runtime_call(b, "rt_flt_box_val", bits, -1,
                                               -1, 1, NYIR_SCALAR_I64_TAGGED);
      if (b_arg < 0)
        return -1;
    }
  }
  if ((strcmp(symbol, "rt_tbuf_get_any") == 0 ||
       strcmp(symbol, "rt_value_get_index_raw") == 0) &&
      argc >= 3 && c >= 0) {
    nyr_scalar_class_t fallback_class = nyr_value_scalar_class(b, c);
    if (fallback_class == NYIR_SCALAR_F32 ||
        fallback_class == NYIR_SCALAR_F64) {
      int as_f64 =
          fallback_class == NYIR_SCALAR_F32 ? nyr_emit_f32_to_f64(b, c) : c;
      int bits = as_f64 < 0
                     ? -1
                     : nyr_emit_runtime_call(b, "rt_f64_bits", as_f64, -1, -1,
                                             1, NYIR_SCALAR_I64_RAW);
      c = bits < 0 ? -1
                   : nyr_emit_runtime_call(b, "rt_flt_box_val", bits, -1, -1, 1,
                                           NYIR_SCALAR_I64_TAGGED);
      if (c < 0)
        return -1;
    }
  }
  /*
   * Raw-allocator and typed-buffer constructor results keep their semantic
   * ABI class through the result-kind rows in ny_native_runtime_abi_decls[]
   * (RESULT_TYPED_BUFFER / RESULT_POINTER), classified below next to the
   * other record-driven result kinds.
   *
   * rt_tag encodes raw integers only. A float-class producer already holds
   * IEEE bits in the floating register ABI, so box it as a language float
   * instead of letting an integer tag operation reinterpret those bits. Keep
   * this correction at the shared call funnel because generic return and
   * container paths can lose the AST-level float fact before they request
   * rt_tag.
   */
  if (strcmp(symbol, "rt_tag") == 0 && argc > 0 && a >= 0) {
    nyr_scalar_class_t arg_class = nyr_value_scalar_class(b, a);
    if (arg_class == NYIR_SCALAR_F32 || arg_class == NYIR_SCALAR_F64) {
      int as_f64 = arg_class == NYIR_SCALAR_F32 ? nyr_emit_f32_to_f64(b, a) : a;
      int bits = as_f64 < 0 ? -1
                            : nyr_emit_runtime_call(b, "rt_f64_bits", as_f64,
                                                    -1, -1, 1, 0);
      return bits < 0 ? -1
                      : nyr_emit_runtime_call(b, "rt_flt_box_val", bits, -1, -1,
                                              1, 0);
    }
  }
  /*
   * Runtime helpers with a C double parameter must receive the value through
   * the floating-point ABI.  Keeping this contract at the intrinsic boundary
   * prevents each lowering site from having to remember a different flag and
   * avoids passing an f64 register as an integer argument.
   */
  const ny_native_runtime_abi_decl_t *abi = ny_native_runtime_abi_decl(symbol);
  /*
   * Helpers that consume a canonical language value (VALUE_CONVERSION rows
   * and per-argument TAGGED marks) must never receive an f64/f32-classified
   * register directly: those slots hold raw IEEE bits in a floating class,
   * and the helper would reinterpret them as a tagged word or a pointer.
   * AST-level float classification misses producers stamped by the lowering
   * itself (a member load re-boxed through rt_any_to_f64), so the register
   * class is the authority.  Box the value once at this single funnel —
   * every conversion/compare/hash/print helper inherits the guarantee
   * without each emission site needing its own guard.
   */
  bool consumes_canonical =
      abi && (abi->flags & NY_NATIVE_RUNTIME_ABI_VALUE_CONVERSION) != 0;
  if (consumes_canonical || ny_native_runtime_symbol_arg_is_tagged(symbol, 0) ||
      ny_native_runtime_symbol_arg_is_tagged(symbol, 1) ||
      ny_native_runtime_symbol_arg_is_tagged(symbol, 2)) {
    int regs[3] = {a, b_arg, c};
    for (int i = 0; i < argc && i < 3; ++i) {
      if (regs[i] < 0)
        continue;
      bool tagged_slot =
          consumes_canonical ||
          ny_native_runtime_symbol_arg_is_tagged(symbol, (unsigned)i);
      if (!tagged_slot)
        continue;
      nyr_scalar_class_t arg_class = nyr_value_scalar_class(b, regs[i]);
      bool is_f32 = arg_class == NYIR_SCALAR_F32;
      bool is_f64 = arg_class == NYIR_SCALAR_F64;
      if (!is_f32 && !is_f64)
        continue;
      if (ny_trace_enabled("NY_TRACE_ABI"))
        ny_trace_line("ABI_BOX",
                      "%s arg %d of %s was %s register; boxing to "
                      "heap float",
                      b->current_fn_name ? b->current_fn_name : "?", i, symbol,
                      is_f32 ? "f32" : "f64");
      int as_f64 = is_f32 ? nyr_emit_f32_to_f64(b, regs[i]) : regs[i];
      int bits = as_f64 < 0 ? -1
                            : nyr_emit_runtime_call(b, "rt_f64_bits", as_f64,
                                                    -1, -1, 1, 0);
      int boxed = bits < 0 ? -1
                           : nyr_emit_runtime_call(b, "rt_flt_box_val", bits,
                                                   -1, -1, 1, 0);
      if (boxed < 0)
        return -1;
      regs[i] = boxed;
    }
    a = regs[0];
    b_arg = regs[1];
    c = regs[2];
  }
  uint64_t packed_arg_classes[NYIR_CALL_MAX_ARGS / 16] = {0};
  uint64_t packed_arg_contracts[NYIR_CALL_MAX_ARGS / 32] = {0};
  nyr_runtime_formal_contracts(symbol, argc, packed_arg_contracts);
  /*
   * This polymorphic receiver accepts managed lists/dictionaries and their
   * canonical dynamic carrier. Keep its formal class stable across joins.
   */
  if (strcmp(symbol, "rt_dict_get_raw") == 0 && argc > 0) {
    nyr_scalar_class_t receiver = nyr_value_scalar_class(b, a);
    if (receiver == NYIR_SCALAR_I64_RAW || receiver == NYIR_SCALAR_UNKNOWN) {
      /*
       * The container accessor consumes a handle, including raw addresses
       * from scalar native sequence slots. Refine its carrier without a
       * numeric tagging operation that would corrupt the address.
       */
      a = nyr_ir_emit(&b->nyir,
                    (nyr_inst_t){.op = NYIR_COPY,
                                  .dst = -1,
                                  .a = a,
                                  .b = -1,
                                  .scalar_class = NYIR_SCALAR_OBJECT});
      if (a < 0)
        return -1;
    }
    nyr_call_arg_class_set(packed_arg_classes, 0, NYIR_SCALAR_I64_TAGGED);
  }
  for (int i = 0; i < argc && i < NYIR_CALL_MAX_ARGS; ++i)
    if (ny_native_runtime_abi_arg_is_f64(abi, (unsigned)i))
      nyr_call_arg_class_set(packed_arg_classes, (size_t)i, NYIR_SCALAR_F64);
  /*
   * A runtime helper with a C double parameter must receive an f64-class
   * register.  Promote only PROVEN raw-int producers: polymorphic helpers
   * (to_str(any)) branch on the runtime tag, and an unconditional
   * i64.to.f64 here broke their string/int dispatch.  A TAGGED producer is
   * likewise not a numeric raw word: decode the canonical dynamic through
   * rt_any_to_f64 instead of SIToFP-converting its handle word.
   */
  {
    int f64_args[3] = {a, b_arg, c};
    for (int i = 0; i < argc && i < 3; ++i) {
      if (f64_args[i] < 0 ||
          !ny_native_runtime_abi_arg_is_f64(abi, (unsigned)i))
        continue;
      /*
       * The semantic slot stamp can claim f64 for a local whose reaching
       * physical class is the tagged dynamic word (the classifier stamped
       * `any + 0.0` f64 while the emitted producer stayed on the rt_any_add
       * bridge). The verifier checks the producing instruction, so the
       * promotion must too: a TAGGED load decodes through rt_any_to_f64
       * instead of passing the handle word under an f64 ABI class.
       */
      if (nyr_value_is_f64(b, f64_args[i], 0) ||
          nyr_value_is_f32(b, f64_args[i], 0))
        continue;
      int promoted;
      if (nyr_value_scalar_class(b, f64_args[i]) == NYIR_SCALAR_I64_TAGGED)
        promoted = nyr_emit_runtime_call(b, "rt_any_to_f64", f64_args[i], -1,
                                         -1, 1, NYIR_SCALAR_F64);
      else
        promoted = nyr_emit_i64_to_f64(b, f64_args[i]);
      if (promoted < 0)
        return -1;
      if (i == 0)
        a = promoted;
      else if (i == 1)
        b_arg = promoted;
      else
        c = promoted;
    }
  }
  /*
   * These typed-buffer helpers have raw integer index formals despite their
   * dynamic element semantics. Some imported generic-list expressions arrive
   * here through an f64-classified arithmetic producer; normalize that scalar
   * at the intrinsic boundary so NYIR never describes an XMM value as an
   * ordinary i64 call operand.
   */
  bool tbuf_index_is_raw = false;
  if ((strcmp(symbol, "rt_tbuf_get_any") == 0 ||
       strcmp(symbol, "rt_tbuf_tag") == 0) &&
      argc >= 2 && b_arg >= 0) {
    nyr_scalar_class_t index_class = nyr_value_scalar_class(b, b_arg);
    if (index_class == NYIR_SCALAR_I64_RAW) {
      tbuf_index_is_raw = true;
    } else if (index_class == NYIR_SCALAR_F32 ||
               index_class == NYIR_SCALAR_F64) {
      int as_f64 = index_class == NYIR_SCALAR_F32
                       ? nyr_emit_f32_to_f64(b, b_arg)
                       : b_arg;
      b_arg = as_f64 < 0 ? -1 : nyr_emit_f64_to_i64(b, as_f64);
      if (b_arg < 0)
        return -1;
      tbuf_index_is_raw = true;
    }
  }
  /*
   * Runtime C declarations with handle parameters carry those scalar classes
   * separately from the C function signature.  Record the contract on the
   * call and normalize tagged dynamic handles exactly once at this boundary.
   */
  {
    const ny_native_runtime_arg_abi_decl_t *arg_abi =
        ny_native_runtime_arg_abi_decl(symbol);
    int args[3] = {a, b_arg, c};
    for (int i = 0; arg_abi && i < argc && i < 3; ++i) {
      if (ny_native_runtime_abi_arg_is_tagged(abi, (unsigned)i))
        continue;
      uint16_t bit = (uint16_t)(1u << (unsigned)i);
      nyr_scalar_class_t formal =
          (arg_abi->pointer_arg_mask & bit)        ? NYIR_SCALAR_POINTER
          : (arg_abi->object_arg_mask & bit)       ? NYIR_SCALAR_OBJECT
          : (arg_abi->typed_buffer_arg_mask & bit) ? NYIR_SCALAR_TYPED_BUFFER
                                                   : NYIR_SCALAR_UNKNOWN;
      if (formal == NYIR_SCALAR_UNKNOWN || args[i] < 0)
        continue;
      nyr_scalar_class_t actual = nyr_value_scalar_class(b, args[i]);
      if (actual == NYIR_SCALAR_I64_TAGGED) {
        /*
         * Dynamic handles and their concrete native formals share the same
         * one-word bits.  Preserve that handle here: scalar unboxing is wrong
         * for odd object/pointer addresses.
         */
        args[i] = nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_COPY,
                                                    .dst = -1,
                                                    .a = args[i],
                                                    .b = -1,
                                                    .scalar_class = formal});
        if (args[i] < 0)
          return -1;
      }
      nyr_call_arg_class_set(packed_arg_classes, (size_t)i, formal);
    }
    a = args[0];
    b_arg = args[1];
    c = args[2];
  }
  /*
   * The same contract applies to the result: a helper whose ABI row says the
   * C return is a double (rt_any_to_f64, rt_value_to_f64, ...) must carry
   * scalar_class=f64 so the SSA value is f64-classified.  Intermediate
   * conversion calls used to be emitted unclassified, which made later integer
   * arithmetic bit-mangle their results — the machine-form gate caught the
   * mismatch (vec3-normalize) — and the scalar-class facts disagreed with
   * the runtime between routes.
   */
  if (return_class == NYIR_SCALAR_UNKNOWN &&
      ny_native_runtime_symbol_returns_f64(symbol))
    return_class = NYIR_SCALAR_F64;
  if (return_class == NYIR_SCALAR_UNKNOWN && abi) {
    if (ny_native_runtime_abi_result_is_raw_int(abi))
      return_class = NYIR_SCALAR_I64_RAW;
    else if (ny_native_runtime_abi_result_is_tagged_int(abi) ||
             ny_native_runtime_abi_result_is_tagged_value(abi) ||
             ny_native_runtime_abi_result_is_bigint(abi))
      return_class = NYIR_SCALAR_I64_TAGGED;
    else if (ny_native_runtime_abi_result_is_cstr(abi))
      return_class = NYIR_SCALAR_STRING;
    else if (ny_native_runtime_abi_result_is_pointer(abi))
      return_class = NYIR_SCALAR_POINTER;
    else if (ny_native_runtime_abi_result_is_typed_buffer(abi))
      return_class = NYIR_SCALAR_TYPED_BUFFER;
  }
  if (tbuf_index_is_raw)
    nyr_call_arg_class_set(packed_arg_classes, 1, NYIR_SCALAR_I64_RAW);
  if (strcmp(symbol, "rt_value_get_index_raw") == 0) {
    if (a >= 0 && nyr_value_scalar_class(b, a) == NYIR_SCALAR_TYPED_BUFFER)
      nyr_call_arg_class_set(packed_arg_classes, 0, NYIR_SCALAR_I64_RAW);
    if (b_arg >= 0 && nyr_value_scalar_class(b, b_arg) == NYIR_SCALAR_I64_RAW)
      nyr_call_arg_class_set(packed_arg_classes, 1, NYIR_SCALAR_I64_RAW);
  }
  /*
   * `rt_len` is representation-polymorphic. Source overload resolution can
   * select an aggregate formal even when an indexed list read reaches the
   * runtime as a tagged value; the call contract must describe the physical
   * carrier, not that source-level formal.
   */
  if ((strcmp(symbol, "rt_len") == 0 || strcmp(symbol, "rt_len_strict") == 0) &&
      argc > 0 && a >= 0) {
    nyr_scalar_class_t actual = nyr_value_scalar_class(b, a);
    if (actual != NYIR_SCALAR_UNKNOWN)
      nyr_call_arg_class_set(packed_arg_classes, 0, actual);
  }
  int value = nyr_ir_emit(
      &b->nyir,
      (nyr_inst_t){
          .op = NYIR_CALL,
          .dst = -1,
          .a = a,
          .b = b_arg,
          .c = c,
          .imm = argc,
          .flags = NYIR_INST_F_EXTERN,
          .scalar_class = return_class,
          .arg_scalar_classes = {packed_arg_classes[0], packed_arg_classes[1],
                                 packed_arg_classes[2], packed_arg_classes[3]},
          .arg_contracts = {packed_arg_contracts[0], packed_arg_contracts[1]},
          .symbol = symbol});
  if (value < 0)
    nyr_fail(b, NY_NATIVE_ALLOC_FAIL);
  return value;
}

/*
 * Five-value variant used by the indirect-closure dispatch: rt_call_any3 /
 * rt_call_any4 forward (env + value/len/tag per argument) to the closure
 * body, and four argument values do not fit emit_runtime_call's three
 * register slots.
 */
static int nyr_emit_runtime_call4(nyr_builder_impl_t *b, const char *symbol,
                                  int a, int b_arg, int c, int d, int e_arg,
                                  int argc, nyr_scalar_class_t return_class) {
  const char *canonical = nyr_runtime_symbol(symbol);
  if (canonical)
    symbol = canonical;
  uint64_t packed_arg_classes[NYIR_CALL_MAX_ARGS / 16] = {0};
  uint64_t packed_arg_contracts[NYIR_CALL_MAX_ARGS / 32] = {0};
  nyr_runtime_formal_contracts(symbol, argc, packed_arg_contracts);
  const ny_native_runtime_abi_decl_t *abi = ny_native_runtime_abi_decl(symbol);
  for (int i = 0; i < argc && i < NYIR_CALL_MAX_ARGS; ++i)
    if (ny_native_runtime_abi_arg_is_f64(abi, (unsigned)i))
      nyr_call_arg_class_set(packed_arg_classes, (size_t)i, NYIR_SCALAR_F64);
  int value = nyr_ir_emit(
      &b->nyir,
      (nyr_inst_t){
          .op = NYIR_CALL,
          .dst = -1,
          .a = a,
          .b = b_arg,
          .c = c,
          .d = d,
          .e = e_arg,
          .imm = argc,
          .flags = NYIR_INST_F_EXTERN,
          .scalar_class = return_class,
          .arg_scalar_classes = {packed_arg_classes[0], packed_arg_classes[1],
                                 packed_arg_classes[2], packed_arg_classes[3]},
          .arg_contracts = {packed_arg_contracts[0], packed_arg_contracts[1]},
          .symbol = symbol});
  if (value < 0)
    nyr_fail(b, NY_NATIVE_ALLOC_FAIL);
  return value;
}

/*
 * N-argument extern call for the callback trampolines beyond four source
 * arguments: the first six words occupy the fixed NYIR slots, the rest ride
 * the instruction-owned extra_args array (freed by nyr_ir_func_free, cloned by
 * the duplicator).
 */
static int nyr_emit_runtime_call_argv(nyr_builder_impl_t *b, const char *symbol,
                                      const int *args, int argc,
                                      nyr_scalar_class_t return_class) {
  const char *canonical = nyr_runtime_symbol(symbol);
  if (canonical)
    symbol = canonical;
  if (argc <= 0 || argc > 64) {
    return nyr_fail(b, "native NYIR lower: callback argv out of range"), -1;
  }
  /*
   * extra_args ride the instruction itself: nyr_ir_emit stores the inst by
   * value and nyr_ir_func_free releases the array, so the heap copy is
   * attached before emitting. (nyr_ir_emit returns the SSA dst, not the
   * instruction index — indexing data[] by it corrupted a random slot.)
   */
  int *extra = NULL;
  uint64_t packed_arg_classes[NYIR_CALL_MAX_ARGS / 16] = {0};
  uint64_t packed_arg_contracts[NYIR_CALL_MAX_ARGS / 32] = {0};
  nyr_runtime_formal_contracts(symbol, argc, packed_arg_contracts);
  const ny_native_runtime_abi_decl_t *abi = ny_native_runtime_abi_decl(symbol);
  for (int i = 0; i < argc; ++i)
    if (ny_native_runtime_abi_arg_is_f64(abi, (unsigned)i))
      nyr_call_arg_class_set(packed_arg_classes, (size_t)i, NYIR_SCALAR_F64);
  if (argc > 6) {
    extra = (int *)malloc((size_t)(argc - 6) * sizeof(*extra));
    if (!extra) {
      return nyr_fail(b, NY_NATIVE_ALLOC_FAIL), -1;
    }
    for (int k = 6; k < argc; ++k)
      extra[k - 6] = args[k];
  }
  nyr_inst_t inst = (nyr_inst_t){
      .op = NYIR_CALL,
      .dst = -1,
      .a = args[0],
      .b = argc > 1 ? args[1] : -1,
      .c = argc > 2 ? args[2] : -1,
      .d = argc > 3 ? args[3] : -1,
      .e = argc > 4 ? args[4] : -1,
      .f = argc > 5 ? args[5] : -1,
      .imm = argc,
      .extra_args = extra,
      .extra_args_len = argc > 6 ? argc - 6 : 0,
      .flags = NYIR_INST_F_EXTERN,
      .scalar_class = return_class,
      .arg_scalar_classes = {packed_arg_classes[0], packed_arg_classes[1],
                             packed_arg_classes[2], packed_arg_classes[3]},
      .arg_contracts = {packed_arg_contracts[0], packed_arg_contracts[1]},
      .symbol = symbol};
  int value = nyr_ir_emit(&b->nyir, inst);
  if (value < 0) {
    free(extra);
    nyr_fail(b, NY_NATIVE_ALLOC_FAIL);
    return value;
  }
  return value;
}

/*
 * Forward declaration — defined below with module/block recursion.
 */
static const stmt_t *nyr_find_user_function(const nyr_builder_impl_t *b,
                                            const char *name);
static const stmt_t *nyr_find_user_function_exact(const stmt_t *s,
                                                  const char *name,
                                                  const ny_options *opt,
                                                  ny_native_fn_index_t *index,
                                                  unsigned depth);
static const stmt_t *nyr_find_extern_decl_in_stmt(const stmt_t *s,
                                                  const char *name,
                                                  const ny_options *opt,
                                                  unsigned depth);
static const stmt_t *nyr_find_extern_decl_in_prog(const nyr_builder_impl_t *b,
                                                  const program_t *prog,
                                                  const char *name,
                                                  const ny_options *opt);
bool nyr_target_eval_bool(const ny_options *opt, const expr_t *e, bool *out);
/*
 * Defined below with the top-level value helpers; used by expr_is_f64 so a
 * member access (gfx.WHITE) or a const list literal types as f64 for the
 * native index/load lowering.
 */
static const expr_t *nyr_find_top_level_value(const nyr_builder_impl_t *b,
                                              const char *name);
static const expr_t *
nyr_find_top_level_value_in_source(const nyr_builder_impl_t *b,
                                   const char *name, const char *source_file);
static const expr_t *nyr_resolve_member_expr(const nyr_builder_impl_t *b,
                                             const expr_t *e);
static const expr_t *nyr_resolve_list_literal(const nyr_builder_impl_t *b,
                                              const expr_t *e, unsigned depth);
static const expr_t *nyr_resolve_dict_literal(const nyr_builder_impl_t *b,
                                              const expr_t *e, unsigned depth);
static bool nyr_dict_literal_all_keys_constant(const expr_t *dict_lit);
static const expr_t *nyr_dict_literal_lookup(const expr_t *dict_lit,
                                             const char *key_str,
                                             int64_t key_int, bool is_int_key,
                                             bool *found);
static bool nyr_expr_is_raw_int(const expr_t *e);
static bool nyr_expr_is_raw_scalar(const nyr_builder_impl_t *b,
                                   const expr_t *expr, unsigned depth);
static const char *nyr_expr_type_name(const nyr_builder_impl_t *b,
                                      const expr_t *e);
static const char *nyr_layout_member_type(const nyr_builder_impl_t *b,
                                          const expr_t *member);
static bool nyr_expr_is_f32_depth(nyr_builder_impl_t *b, const expr_t *e,
                                  unsigned depth);
static bool nyr_expr_is_f64_depth(nyr_builder_impl_t *b, const expr_t *e,
                                  unsigned depth);
static bool nyr_expr_has_float_literal(const expr_t *e, unsigned depth);
static bool nyr_expr_is_f32(nyr_builder_impl_t *b, const expr_t *e);
static const stmt_t *nyr_find_imported_function(const nyr_builder_impl_t *b,
                                                const char *name);
static const stmt_t *nyr_find_top_level_var_in_prog(const nyr_builder_impl_t *b,
                                                    const program_t *prog,
                                                    const char *name);
static const stmt_t *nyr_find_top_level_var_in_source_in_prog(
    const nyr_builder_impl_t *b, const program_t *prog, const char *name,
    const char *source_file);

static bool nyr_expr_has_float_literal(const expr_t *e, unsigned depth) {
  if (!e || depth > 16)
    return false;
  if (e->kind == NY_E_LITERAL)
    return e->as.literal.kind == NY_LIT_FLOAT &&
           e->as.literal.hint != NY_LIT_HINT_F32;
  switch (e->kind) {
  case NY_E_TERNARY:
    return nyr_expr_has_float_literal(e->as.ternary.true_expr, depth + 1) ||
           nyr_expr_has_float_literal(e->as.ternary.false_expr, depth + 1);
  case NY_E_UNARY:
    return nyr_expr_has_float_literal(e->as.unary.right, depth + 1);
  case NY_E_BINARY:
    return nyr_expr_has_float_literal(e->as.binary.left, depth + 1) ||
           nyr_expr_has_float_literal(e->as.binary.right, depth + 1);
  case NY_E_LOGICAL:
    return nyr_expr_has_float_literal(e->as.logical.left, depth + 1) ||
           nyr_expr_has_float_literal(e->as.logical.right, depth + 1);
  case NY_E_CALL:
    for (size_t i = 0; i < e->as.call.args.len; ++i)
      if (nyr_expr_has_float_literal(e->as.call.args.data[i].val, depth + 1))
        return true;
    return false;
  case NY_E_MEMCALL:
    for (size_t i = 0; i < e->as.memcall.args.len; ++i)
      if (nyr_expr_has_float_literal(e->as.memcall.args.data[i].val, depth + 1))
        return true;
    return false;
  case NY_E_INDEX:
    return nyr_expr_has_float_literal(e->as.index.target, depth + 1) ||
           nyr_expr_has_float_literal(e->as.index.start, depth + 1) ||
           nyr_expr_has_float_literal(e->as.index.stop, depth + 1) ||
           nyr_expr_has_float_literal(e->as.index.step, depth + 1);
  default:
    return false;
  }
}

static size_t ny_native_scalar_probe_slot(const expr_t *e, unsigned kind,
                                          unsigned depth) {
  uintptr_t key = (uintptr_t)e;
  key ^= key >> 17;
  key ^= (uintptr_t)kind * (uintptr_t)0x9e3779b1U;
  key ^= (uintptr_t)depth * (uintptr_t)0x85ebca6bU;
  return (size_t)key & (NY_SCALAR_PROBE_CACHE_SIZE - 1);
}

static void ny_native_scalar_probe_enter(nyr_builder_impl_t *b) {
  if (b->scalar_probe_active++ != 0)
    return;
  if (++b->scalar_probe_generation == 0) {
    memset(b->scalar_probe_cache, 0, sizeof(b->scalar_probe_cache));
    b->scalar_probe_generation = 1;
  }
}

static bool ny_native_scalar_probe_get(const nyr_builder_impl_t *b,
                                       const expr_t *e, unsigned kind,
                                       unsigned depth, bool *value) {
  const ny_native_scalar_probe_cache_entry_t *entry =
      &b->scalar_probe_cache[ny_native_scalar_probe_slot(e, kind, depth)];
  if (entry->generation != b->scalar_probe_generation || entry->expr != e ||
      entry->kind != kind || entry->depth != depth)
    return false;
  *value = entry->value;
  return true;
}

static void ny_native_scalar_probe_put(nyr_builder_impl_t *b, const expr_t *e,
                                       unsigned kind, unsigned depth,
                                       bool value) {
  ny_native_scalar_probe_cache_entry_t *entry =
      &b->scalar_probe_cache[ny_native_scalar_probe_slot(e, kind, depth)];
  *entry = (ny_native_scalar_probe_cache_entry_t){
      .expr = e,
      .generation = b->scalar_probe_generation,
      .kind = (uint8_t)kind,
      .depth = (uint8_t)depth,
      .value = value};
}

static void ny_native_scalar_probe_leave(nyr_builder_impl_t *b) {
  if (b->scalar_probe_active)
    --b->scalar_probe_active;
}

/*
 * The builder normally carries the declared return type directly.  Imported
 * stdlib functions can be lowered from a cloned declaration where that field
 * is absent, while the canonical function record still has the contract.
 * Recover the contract once here so representation queries remain ABI based
 * instead of depending on the spelling of a particular library function.
 */
static bool nyr_current_returns_cstr(nyr_builder_impl_t *b) {
  if (!b)
    return false;
  if (b->return_type && (strcmp(b->return_type, "str") == 0 ||
                         strcmp(b->return_type, "string") == 0))
    return true;
  if (!b->current_fn_name)
    return false;
  const stmt_t *fn = nyr_find_user_function(b, b->current_fn_name);
  return fn && fn->kind == NY_S_FUNC && fn->as.fn.return_type &&
         (strcmp(fn->as.fn.return_type, "str") == 0 ||
          strcmp(fn->as.fn.return_type, "string") == 0);
}

static bool nyr_qualified_expr(const expr_t *e, char *out, size_t out_len);
static const stmt_t *nyr_source_attached_result(const nyr_builder_impl_t *b,
                                                const expr_t *e) {
  const expr_t *member = e;
  if (e && e->kind == NY_E_CALL)
    member = e->as.call.callee;
  const expr_t *receiver = NULL;
  const char *method = NULL;
  if (member && member->kind == NY_E_MEMBER) {
    receiver = member->as.member.target;
    method = member->as.member.name;
  } else if (member && member->kind == NY_E_MEMCALL) {
    receiver = member->as.memcall.target;
    method = member->as.memcall.name;
  }
  const stmt_t *fn =
      receiver && method ? nyr_find_attached_method(b, receiver, method) : NULL;
  return fn && !ny_is_stdlib_tok(fn->tok) && fn->as.fn.return_type ? fn : NULL;
}
static bool nyr_expr_is_f64_core(nyr_builder_impl_t *b, const expr_t *e,
                                 unsigned depth) {
  if (!e || depth > 8)
    return false;

  if (e->kind == NY_E_BINARY && e->semantic.resolved_type_name)
    return ny_native_type_name_is_f64(e->semantic.resolved_type_name);
  /*
   * min/max select their machine lane from their operands during NYIR
   * lowering. Their generic `number` declaration is not itself an f64 ABI
   * promise: an integer result assigned to a local must stay in an integer
   * slot before the call has been lowered.
   */
  const char *builtin_leaf =
      e->kind == NY_E_CALL ? ny_native_call_leaf(e) : NULL;
  const char *builtin_name = e->kind == NY_E_CALL && e->as.call.callee &&
                                     e->as.call.callee->kind == NY_E_IDENT
                                 ? e->as.call.callee->as.ident.name
                                 : NULL;
  const stmt_t *builtin_decl =
      e->kind == NY_E_CALL ? e->semantic.canonical_callee_stmt : NULL;
  if (!builtin_decl && builtin_name)
    builtin_decl = nyr_find_user_function(b, builtin_name);
  bool source_decl = builtin_decl && !ny_is_stdlib_tok(builtin_decl->tok);
  if (builtin_leaf && !source_decl &&
      (strcmp(builtin_leaf, "min") == 0 || strcmp(builtin_leaf, "max") == 0) &&
      e->as.call.args.len == 2 && e->as.call.args.data[0].val &&
      e->as.call.args.data[1].val)
    return nyr_expr_is_f64_depth(b, e->as.call.args.data[0].val, depth + 1) ||
           nyr_expr_is_f64_depth(b, e->as.call.args.data[1].val, depth + 1);
  const stmt_t *source_attached = nyr_source_attached_result(b, e);
  if (source_attached)
    return nyr_fn_return_is_f64(source_attached);
  /*
   * Source namespace declarations take precedence over contextual and
   * same-spelled stdlib return facts.
   */
  const expr_t *namespace_target = NULL;
  const char *namespace_method = NULL;
  if (e->kind == NY_E_MEMCALL) {
    namespace_target = e->as.memcall.target;
    namespace_method = e->as.memcall.name;
  } else if (e->kind == NY_E_CALL && e->as.call.callee &&
             e->as.call.callee->kind == NY_E_MEMBER) {
    namespace_target = e->as.call.callee->as.member.target;
    namespace_method = e->as.call.callee->as.member.name;
  }
  char namespace_name[512];
  if (e->kind == NY_E_CALL && e->as.call.callee &&
      e->as.call.callee->kind == NY_E_IDENT) {
    const stmt_t *fn =
        nyr_find_user_function(b, e->as.call.callee->as.ident.name);
    if (fn && !ny_is_stdlib_tok(fn->tok) && fn->as.fn.return_type)
      return nyr_fn_return_is_f64(fn);
  }
  if (namespace_target && namespace_method &&
      nyr_qualified_expr(namespace_target, namespace_name,
                         sizeof(namespace_name))) {
    size_t used = strlen(namespace_name);
    size_t method_len = strlen(namespace_method);
    if (used + method_len + 2 <= sizeof(namespace_name)) {
      namespace_name[used++] = '.';
      memcpy(namespace_name + used, namespace_method, method_len + 1);
      const stmt_t *fn = nyr_find_user_function(b, namespace_name);
      if (fn && !ny_is_stdlib_tok(fn->tok) && fn->as.fn.return_type)
        return nyr_fn_return_is_f64(fn);
    }
  }
  /*
   * Native string locals hold C-string pointers in an i64 slot.  Semantic
   * annotations on reads can be inherited from a generic/contextual use and
   * must not reinterpret that pointer as an IEEE f64 payload.
   */
  if (e->kind == NY_E_TERNARY && nyr_expr_is_cstr(b, e))
    return false;
  /*
   * A string default is part of the result contract of dynamic `get`: the
   * successful dictionary/sequence value must cross the native boundary as a
   * string, never through the floating fallback used by a surrounding
   * comparison or return.  Check this before semantic/contextual metadata,
   * which may describe the generic helper as `any` or f64.
   */
  if (e->kind == NY_E_MEMCALL && e->as.memcall.name &&
      strcmp(e->as.memcall.name, "get") == 0 && nyr_current_returns_cstr(b))
    return false;
  if (e->kind == NY_E_MEMCALL && e->as.memcall.name &&
      strcmp(e->as.memcall.name, "get") == 0 && e->as.memcall.args.len >= 2 &&
      e->as.memcall.args.data[1].val &&
      nyr_expr_is_cstr(b, e->as.memcall.args.data[1].val))
    return false;
  if (e->kind == NY_E_CALL && ny_native_call_leaf(e) &&
      strcmp(ny_native_call_leaf(e), "get") == 0 && nyr_current_returns_cstr(b))
    return false;
  if (e->kind == NY_E_CALL && ny_native_call_leaf(e) &&
      strcmp(ny_native_call_leaf(e), "get") == 0 && e->as.call.args.len >= 3 &&
      e->as.call.args.data[2].val &&
      nyr_expr_is_cstr(b, e->as.call.args.data[2].val))
    return false;
  /*
   * Integer/bool literals are raw NYIR scalars even when contextual typing
   * attached a floating semantic representation to the literal node.  The
   * arithmetic lowering must see the raw operand and emit the i64->f64
   * conversion; otherwise an f64 instruction consumes the integer word as
   * IEEE bits (or the optimizer folds the operation in the wrong domain).
   */
  if (e->kind == NY_E_LITERAL &&
      (e->as.literal.kind == NY_LIT_INT || e->as.literal.kind == NY_LIT_BOOL ||
       e->tok.kind == NY_T_NIL))
    return false;
  /*
   * The native `+` string path returns a C-string pointer even when the
   * contextual semantic result is still unknown.  Its operands determine
   * that physical result before the generic arithmetic/f64 fallback.
   */
  if (e->kind == NY_E_BINARY && e->as.binary.op &&
      strcmp(e->as.binary.op, "+") == 0 &&
      (nyr_expr_is_cstr(b, e->as.binary.left) ||
       nyr_expr_is_cstr(b, e->as.binary.right)))
    return false;
  /*
   * Conditional expressions join the representation of their arms.  A
   * generated shape conversion commonly selects between dynamic container
   * reads with an f64 fallback; without this structural rule the enclosing
   * f64 field was lowered as an i64 word and its tagged float was later
   * bitcast instead of decoded.
   */
  if (e->kind == NY_E_TERNARY && nyr_expr_has_float_literal(e, depth))
    return true;
  if (e->kind == NY_E_TERNARY)
    return nyr_expr_is_f64_depth(b, e->as.ternary.true_expr, depth + 1) ||
           nyr_expr_is_f64_depth(b, e->as.ternary.false_expr, depth + 1);
  /*
   * Arithmetic representation is a property of the concrete operands, not
   * of a possibly stale joined semantic result.  Check it before the
   * nominal result-type fallbacks below so compound assignments keep a
   * declared floating accumulator in the f64 domain.
   */
  if (e->kind == NY_E_BINARY && e->as.binary.op) {
    nyr_cmp_t binary_cmp;
    if (!nyr_cmp(e->as.binary.op, &binary_cmp) &&
        !nyr_is_bitwise_operator(e->as.binary.op) &&
        (nyr_expr_is_f64_depth(b, e->as.binary.left, depth + 1) ||
         nyr_expr_is_f64_depth(b, e->as.binary.right, depth + 1)))
      return true;
  }
  /*
   * A dynamic/descriptor-backed receiver always returns a canonical tagged
   * value.  Its fallback argument is only used for a missing key/element; it
   * does not change the ABI of a successful read.  Treating
   * `dynamic.get(i, 0.0)` as raw f64 reinterprets the returned boxed value as
   * IEEE bits and corrupts the value at the next dynamic boundary.  Proven
   * typed scalar sequences still use the fallback to select their raw scalar
   * element ABI below.
   */
  if (e->kind == NY_E_MEMCALL && e->as.memcall.name &&
      strcmp(e->as.memcall.name, "get") == 0) {
    const expr_t *target = e->as.memcall.target;
    if (target &&
        (nyr_expr_is_any(b, target) || nyr_expr_is_dyn_list(b, target)))
      return false;
    if (target && nyr_expr_is_dict(b, target))
      return false;
    if (e->as.memcall.args.len >= 2 && e->as.memcall.args.data[1].val &&
        nyr_expr_is_f64_depth(b, e->as.memcall.args.data[1].val, depth + 1))
      return true;
  }
  /*
   * Namespace/member calls must use the selected declaration's ABI before
   * consulting contextual semantic metadata.  Otherwise a string-returning
   * imported helper such as `ty.normalize_type_name(...)` can inherit a stale
   * f64 fallback and its result is written into a dynamic list as TAG_FLOAT.
   */
  if (e->kind == NY_E_MEMCALL && e->as.memcall.name) {
    const stmt_t *fn = e->semantic.canonical_callee_stmt;
    if (!fn && e->as.memcall.target &&
        e->as.memcall.target->kind == NY_E_IDENT &&
        e->as.memcall.target->as.ident.name) {
      const char *module =
          nyr_resolve_use_alias(b, e->as.memcall.target->as.ident.name);
      if (module) {
        char qualified[512];
        int n = snprintf(qualified, sizeof(qualified), "%s.%s", module,
                         e->as.memcall.name);
        if (n > 0 && (size_t)n < sizeof(qualified)) {
          fn = nyr_find_user_function(b, qualified);
          if (!fn)
            fn = nyr_find_imported_function(b, qualified);
        }
      }
    }
    if (!fn)
      fn =
          nyr_find_attached_method(b, e->as.memcall.target, e->as.memcall.name);
    if (fn && fn->kind == NY_S_FUNC && fn->as.fn.return_type)
      return nyr_fn_return_is_f64(fn);
  }
  if (e->kind == NY_E_CALL && e->semantic.canonical_callee_stmt &&
      e->semantic.canonical_callee_stmt->kind == NY_S_FUNC &&
      e->semantic.canonical_callee_stmt->as.fn.return_type)
    return nyr_fn_return_is_f64(e->semantic.canonical_callee_stmt);
  if (e->kind == NY_E_CALL && e->semantic.canonical_callee_stmt &&
      e->semantic.canonical_callee_stmt->kind == NY_S_FUNC &&
      !e->semantic.canonical_callee_stmt->as.fn.return_type)
    return e->semantic.canonical_callee_stmt->as.fn.return_semantic.resolved &&
           e->semantic.canonical_callee_stmt->as.fn.return_semantic.rep ==
               NY_SEM_REP_F64;
  if (e->kind == NY_E_CALL && e->as.call.callee &&
      e->as.call.callee->kind == NY_E_IDENT) {
    const stmt_t *direct_fn = e->semantic.canonical_callee_stmt;
    if (!direct_fn)
      direct_fn = nyr_find_user_function(b, e->as.call.callee->as.ident.name);
    if (!direct_fn)
      direct_fn =
          nyr_find_imported_function(b, e->as.call.callee->as.ident.name);
    if (direct_fn && direct_fn->kind == NY_S_FUNC &&
        !direct_fn->as.fn.return_type)
      return direct_fn->as.fn.return_semantic.resolved &&
             direct_fn->as.fn.return_semantic.rep == NY_SEM_REP_F64;
  }
  /*
   * A direct call's resolved result type is the same ABI fact used for
   * members.  Do this before expression-level fallback metadata: imported
   * wrappers such as complex.real are declared f64 even when their call node
   * retained a conservative dynamic annotation.
   */
  if (e->kind == NY_E_CALL) {
    const char *result_type = nyr_expr_type_name(b, e);
    if (result_type && ny_native_type_name_is_f64(result_type))
      return true;
    if (result_type && ny_native_type_name_is_f32(result_type))
      return false;
    const char *callee_name =
        e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT
            ? e->as.call.callee->as.ident.name
            : NULL;
    const char *runtime =
        callee_name ? ny_native_runtime_symbol_for_expr(callee_name, NULL, e)
                    : NULL;
    if (ny_native_runtime_symbol_returns_f64(runtime))
      return true;
  }
  /*
   * A dynamic local may currently contain a float, but its slot carries the
   * tagged-value ABI.  Its semantic expression annotation can still say f64
   * because of a contextual default/assignment; treating that annotation as
   * a raw f64 load reinterprets every non-f64 value in the same slot.
   */
  if (e->kind == NY_E_IDENT) {
    nyr_local_t *local =
        nyr_find_local_ctx(b, e->as.ident.name, e->as.ident.syntax_ctx);
    if (!local)
      local = nyr_find_local_nearest(b, e->as.ident.name);
    if (local && (local->is_cstr || local->semantic_rep == NY_SEM_REP_STRING ||
                  ny_native_type_name_is_str(local->type_name)))
      return false;
    if (local &&
        (local->is_any || ny_native_type_name_is_any(local->type_name)))
      return false;
    if (local && !local->scalar_class_mixed &&
        local->scalar_class != NYIR_SCALAR_UNKNOWN)
      return local->scalar_class == NYIR_SCALAR_F64;
    if (local && local->semantic_rep != NY_SEM_REP_UNRESOLVED &&
        local->semantic_rep != NY_SEM_REP_TAGGED_DYNAMIC)
      return local->semantic_rep == NY_SEM_REP_F64;
    if (local && local->is_f64)
      return true;
    if (local && local->type_name &&
        !ny_native_type_name_is_any(local->type_name))
      return ny_native_type_name_is_f64(local->type_name);
    /*
     * Root declarations are lowered through module storage and therefore do
     * not necessarily have a native local record.  Their explicit type is
     * still the ABI contract for every later read.  Consult the declaration
     * before falling back to the initializer's conservative expression type;
     * overloaded values such as `def f64 d = a * b` otherwise get loaded as
     * an integer word and are numerically converted from their IEEE bits.
     */
    if (!local && e->as.ident.name) {
      const stmt_t *decl = NULL;
      if (e->tok.filename && b->prog)
        decl = nyr_find_top_level_var_in_source_in_prog(
            b, b->prog, e->as.ident.name, e->tok.filename);
      if (!decl)
        decl = nyr_find_top_level_var_stmt(b, e->as.ident.name);
      if (decl && decl->kind == NY_S_VAR)
        for (size_t i = 0; i < decl->as.var.names.len; ++i) {
          if (decl->as.var.names.data[i] &&
              strcmp(decl->as.var.names.data[i], e->as.ident.name) == 0) {
            if (i < decl->as.var.types.len && decl->as.var.types.data[i] &&
                (ny_native_type_name_is_f64(decl->as.var.types.data[i]) ||
                 ny_semantic_rep_for_type(decl->as.var.types.data[i]) ==
                     NY_SEM_REP_F64))
              return true;
          }
        }
      /*
       * The expression type resolver also follows qualified/module bindings
       * and operator return contracts.  Keep it as the generic fallback for
       * flattened programs whose declaration node is not the same AST root
       * as the identifier being lowered.
       */
      const char *resolved_type = nyr_expr_type_name(b, e);
      if (resolved_type &&
          (ny_native_type_name_is_f64(resolved_type) ||
           ny_semantic_rep_for_type(resolved_type) == NY_SEM_REP_F64))
        return true;
    }
  }
  /*
   * Power lowering has an explicit representation split: a provably
   * non-negative integer power is raw i64, while a negative or fractional
   * exponent is evaluated by the f64 runtime bridge.  Decide that before
   * consulting contextual semantic metadata; the latter can conservatively
   * report `any`/`int` for a dynamic call argument and otherwise suppress the
   * f64 boxing required at the next dynamic boundary.
   */
  if (e->kind == NY_E_BINARY && e->as.binary.op &&
      strcmp(e->as.binary.op, "**") == 0 && !nyr_expr_is_bigint(b, e)) {
    return nyr_expr_is_f64_depth(b, e->as.binary.left, depth + 1) ||
           nyr_expr_is_f64_depth(b, e->as.binary.right, depth + 1);
  }
  /*
   * The semantic pass already records the result representation for ordinary
   * binary expressions.  Use it before nominal operator lookup: looking up a
   * string concatenation asks for both operand types, which can call this
   * classifier again through type-name inference and turn a linear query into
   * a full AST walk for every `+` node.  Custom unresolved operators still
   * use the nominal lookup below.
   */
  if (e->kind == NY_E_BINARY && e->semantic.resolved) {
    /*
     * The semantic pass may retain the use-site's conservative dynamic
     * representation for a compound assignment.  The native ABI is still
     * determined by the operands that will actually be loaded: a machine
     * f64 global updated with a f64 expression must remain in the floating
     * domain.  Recover that concrete arithmetic fact before honoring a stale
     * joined semantic representation.
     */
    nyr_cmp_t binary_cmp;
    if (!nyr_cmp(e->as.binary.op, &binary_cmp) &&
        !nyr_is_bitwise_operator(e->as.binary.op) &&
        (nyr_expr_is_f64_depth(b, e->as.binary.left, depth + 1) ||
         nyr_expr_is_f64_depth(b, e->as.binary.right, depth + 1)))
      return true;
    /*
     * A resolved string expression is explicitly outside the floating ABI.
     * Returning true here routes `any + "text"` through the f64 boxing
     * bridge; the native result is then a float-tagged string pointer.  The
     * string path below must remain authoritative for concatenation.
     */
    if (e->semantic.rep == NY_SEM_REP_STRING)
      return false;
    if (e->semantic.rep != NY_SEM_REP_UNRESOLVED)
      return false;
  }
  /*
   * String concatenation is structurally closed over its operands.  Resolve
   * that common case without nominal operator lookup; the latter asks for
   * operand type names and is needlessly quadratic for a concatenation chain
   * such as a generated cache key.
   */
  if (e->kind == NY_E_BINARY && e->as.binary.op &&
      strcmp(e->as.binary.op, "+") == 0)
    return nyr_expr_is_cstr(b, e->as.binary.left) ||
           nyr_expr_is_cstr(b, e->as.binary.right);
  if (e->kind == NY_E_BINARY) {
    /*
     * Operator dispatch is the representation authority for overloaded
     * arithmetic.  The HM expression may conservatively remain `any`, while
     * the selected impl (for example vec3 * vec3) has an explicit f64 return
     * ABI.  Consult that declaration before rejecting the expression from
     * the floating path.
     */
    const stmt_t *op_stmt = nyr_find_operator(b, e);
    if (op_stmt && op_stmt->as.oper.return_type)
      return ny_native_type_name_is_f64(op_stmt->as.oper.return_type);
    const char *result_type = nyr_expr_type_name(b, e);
    if (result_type && !ny_native_type_name_is_f64(result_type) &&
        !ny_native_type_name_is_f32(result_type))
      return false;
  }
  /*
   * Attached/module member calls are resolved by the same declaration
   * contract as direct calls, but their AST node is NY_E_MEMCALL (or a call
   * whose callee is NY_E_MEMBER).  Consult that resolved return type here so
   * a typed f64 method result remains an f64 in an enclosing comparison; the
   * old path bitcast the f64 register to i64 and then converted the bits as a
   * number.
   */
  if (e->kind == NY_E_MEMCALL || e->kind == NY_E_CALL) {
    const char *result_type = nyr_expr_type_name(b, e);
    if (result_type && ny_native_type_name_is_f64(result_type))
      return true;
    if (result_type && ny_native_type_name_is_f32(result_type))
      return false;
  }
  /*
   * An index inherits the sequence element ABI, not the carrier's semantic
   * rep.  A generic list parameter may be conservatively stamped f64 even
   * though its runtime scalar slots are integers; only an explicit f64
   * element type may select the floating-point load path.
   */
  if (e->kind == NY_E_INDEX) {
    const char *result_type = nyr_expr_type_name(b, e);
    const expr_t *literal_target =
        nyr_resolve_list_literal(b, e->as.index.target, 0);
    if (literal_target && literal_target->kind == NY_E_LIST &&
        !nyr_expr_is_dyn_list(b, literal_target))
      return false;
    if (e->as.index.target && e->as.index.target->kind == NY_E_IDENT &&
        e->as.index.target->as.ident.name) {
      const nyr_local_t *sequence =
          nyr_find_local_nearest(b, e->as.index.target->as.ident.name);
      if (sequence && sequence->is_list) {
        const char *decl = sequence->type_name;
        bool explicit_f64 = decl && strstr(decl, "<f64>") != NULL;
        if (!explicit_f64)
          return false;
      }
    }
    if (!result_type || !ny_native_type_name_is_f64(result_type))
      return false;
    /*
     * Every conservative guard above passed and the resolved element type is
     * f64: the read is a floating value whether or not the checker stamped
     * the index node (an unstamped node used to fall through to the scalar
     * operator ladder, which then bit-mangled `xs[0] + xs[1]` over a
     * `list<f64>` as integer arithmetic on the IEEE payloads).
     */
    return true;
  }
  /*
   * A call's result representation is determined by its callee contract.
   * The expression node can retain the representation of an earlier
   * contextual inference (for example, a float argument to a function that
   * actually returns int).  Do not let that stale context reclassify the
   * call before the callee is inspected below.
   * A concrete local declaration is the ABI authority for its reads. The
   * semantic annotation can be a stale contextual float (for example, an
   * integer helper result used in a generic arithmetic expression); honoring
   * it first emits fcmp on an i64 and later widens that local's type root.
   */
  if (e->kind == NY_E_IDENT && e->as.ident.name) {
    const nyr_local_t *local =
        nyr_find_local_ctx(b, e->as.ident.name, e->as.ident.syntax_ctx);
    if (!local)
      local = nyr_find_local_nearest(b, e->as.ident.name);
    if (local && local->type_name &&
        !ny_native_type_name_is_any(local->type_name))
      return ny_native_type_name_is_f64(local->type_name) ||
             local->semantic_rep == NY_SEM_REP_F64;
    if (local)
      return !local->is_any &&
             (local->is_f64 || local->semantic_rep == NY_SEM_REP_F64);
  }
  if (e->kind == NY_E_BINARY) {
    const expr_t *left = e->as.binary.left;
    const expr_t *right = e->as.binary.right;
    bool left_int_cast =
        left && left->kind == NY_E_CALL && ny_native_call_leaf(left) &&
        strcmp(ny_native_call_leaf(left), "int") == 0 &&
        left->as.call.args.len == 1 && !left->as.call.args.data[0].name &&
        !nyr_user_defined_fn(b, "int");
    bool right_int_cast =
        right && right->kind == NY_E_CALL && ny_native_call_leaf(right) &&
        strcmp(ny_native_call_leaf(right), "int") == 0 &&
        right->as.call.args.len == 1 && !right->as.call.args.data[0].name &&
        !nyr_user_defined_fn(b, "int");
    if (left_int_cast && right_int_cast)
      return false;
    if (left_int_cast)
      return nyr_expr_is_f64_depth(b, right, depth + 1);
    if (right_int_cast)
      return nyr_expr_is_f64_depth(b, left, depth + 1);
  }
  if (e->kind == NY_E_MEMCALL ||
      (e->kind == NY_E_CALL &&
       ((e->as.call.callee && e->as.call.callee->kind == NY_E_MEMBER) ||
        e->semantic.member_call_kind != NY_SEM_CALL_NONE))) {
    const expr_t *target =
        e->kind == NY_E_MEMCALL
            ? e->as.memcall.target
            : (e->as.call.callee && e->as.call.callee->kind == NY_E_MEMBER
                   ? e->as.call.callee->as.member.target
                   : (e->as.call.args.len ? e->as.call.args.data[0].val
                                          : NULL));
    const char *method =
        e->kind == NY_E_MEMCALL
            ? e->as.memcall.name
            : (e->as.call.callee && e->as.call.callee->kind == NY_E_MEMBER
                   ? e->as.call.callee->as.member.name
                   : (e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT
                          ? e->as.call.callee->as.ident.name
                          : NULL));
    const stmt_t *resolved = e->semantic.canonical_callee_stmt;
    if (!resolved || resolved->kind != NY_S_FUNC)
      resolved =
          target && method ? nyr_find_attached_method(b, target, method) : NULL;
    if (!resolved && method)
      resolved = nyr_find_attached_owner(b, "any", method);
    if (resolved && resolved->kind == NY_S_FUNC && resolved->as.fn.return_type)
      return ny_native_type_name_is_f64(resolved->as.fn.return_type);
    if (e->kind == NY_E_MEMCALL && method &&
        (strcmp(method, "abs") == 0 || strcmp(method, "min") == 0 ||
         strcmp(method, "max") == 0 || strcmp(method, "clamp") == 0 ||
         strcmp(method, "lerp") == 0)) {
      for (size_t i = 0; i < e->as.memcall.args.len; ++i)
        if (nyr_expr_is_f64_depth(b, e->as.memcall.args.data[i].val, depth + 1))
          return true;
    }
    return false;
  }
  if (e->kind != NY_E_CALL && e->kind != NY_E_IDENT && e->semantic.resolved &&
      e->semantic.rep == NY_SEM_REP_F64)
    return true;
  /*
   * The float() cast intrinsic always yields a machine f64, whatever the
   * argument's static type says (`float((ij*(ij+1)) >> 1)` holds an
   * int-typed shift whose register was computed in the f64 domain).  A
   * missing classification here double-converted the divisor
   * (i64.to.f64 of an f64 register) and spectral divided by garbage.
   */
  if (e->kind == NY_E_CALL && ny_native_call_leaf(e) &&
      strcmp(ny_native_call_leaf(e), "float") == 0) {
    return true;
  }
  /*
   * Do not reject a dynamic semantic classification before inspecting the
   * expression tree: `any + 0.0` is represented as tagged-dynamic by the
   * type pass but is still an f64 operation in native lowering.
   */
  if (e->kind == NY_E_LITERAL)
    return e->as.literal.kind == NY_LIT_FLOAT &&
           e->as.literal.hint != NY_LIT_HINT_F32;
  if (e->kind == NY_E_CALL) {
    const char *leaf = ny_native_call_leaf(e);
    /*
     * A user function owning the spelling decides from its return type
     * before the builtin math-name check fires.
     */
    const stmt_t *ufn =
        leaf ? nyr_find_user_function((nyr_builder_t *)b, leaf) : NULL;
    if (ufn && ufn->as.fn.return_type)
      return ny_native_type_name_is_f64(ufn->as.fn.return_type);
    if (leaf && (strcmp(leaf, "__flt_sin") == 0 ||
                 strcmp(leaf, "__flt_cos") == 0 || strcmp(leaf, "sin") == 0 ||
                 strcmp(leaf, "cos") == 0 || strcmp(leaf, "sqrt") == 0))
      return true;
  }
  if (e->kind == NY_E_MEMBER && e->as.member.target && e->as.member.name &&
      (!strcmp(e->as.member.name, "x") || !strcmp(e->as.member.name, "y") ||
       !strcmp(e->as.member.name, "z") || !strcmp(e->as.member.name, "w"))) {
    const char *type = nyr_expr_type_name(b, e->as.member.target);
    if (!type && e->as.member.target->kind == NY_E_IDENT &&
        e->as.member.target->as.ident.name) {
      const nyr_local_t *local =
          nyr_find_local_nearest(b, e->as.member.target->as.ident.name);
      if (local)
        type = local->type_name;
    }
    if (ny_type_vector_dimension(type) > 0)
      return true;
  }
  /*
   * A bound parameter/local is authoritative even when the semantic pass
   * attached a stale generic float representation to its identifier node.
   */
  if (e->kind == NY_E_IDENT) {
    nyr_local_t *l = nyr_find_local_nearest(b, e->as.ident.name);
    if (l && l->type_name && !ny_native_type_name_is_any(l->type_name) &&
        ny_semantic_rep_for_type(l->type_name) != NY_SEM_REP_UNRESOLVED &&
        ny_semantic_rep_for_type(l->type_name) != NY_SEM_REP_TAGGED_DYNAMIC)
      return ny_semantic_rep_for_type(l->type_name) == NY_SEM_REP_F64;
    if (l)
      return !l->is_any && (l->semantic_rep == NY_SEM_REP_F64 || l->is_f64);
    const expr_t *g = e->tok.filename
                          ? nyr_find_top_level_value_in_source(
                                b, e->as.ident.name, e->tok.filename)
                          : nyr_find_top_level_value(b, e->as.ident.name);
    if (!g)
      g = nyr_find_top_level_value(b, e->as.ident.name);
    if (g && g != e && g->kind == NY_E_CALL) {
      const char *leaf = ny_native_call_leaf(g);
      /*
       * A user function owning the math spelling decides from its return
       * type before the builtin math-name check fires.
       */
      const stmt_t *ufn =
          leaf ? nyr_find_user_function((nyr_builder_t *)b, leaf) : NULL;
      if (ufn && ufn->as.fn.return_type)
        return ny_native_type_name_is_f64(ufn->as.fn.return_type);
      if (leaf && (strcmp(leaf, "__flt_sin") == 0 ||
                   strcmp(leaf, "__flt_cos") == 0 || strcmp(leaf, "sin") == 0 ||
                   strcmp(leaf, "cos") == 0 || strcmp(leaf, "sqrt") == 0))
        return true;
    }
  }
  if (e->kind == NY_E_BINARY) {
    /*
     * The native power lowering has one deliberately split ABI: provably
     * non-negative integer powers become raw i64 constants, while every
     * other power is evaluated as f64.  Keep the type query in lock-step
     * with that decision so a dynamic `any` call boxes the latter instead
     * of applying the integer tag encoding to its IEEE-754 bits.
     */
    if (e->as.binary.op && strcmp(e->as.binary.op, "**") == 0) {
      return nyr_expr_is_f64_depth(b, e->as.binary.left, depth + 1) ||
             nyr_expr_is_f64_depth(b, e->as.binary.right, depth + 1);
    }
    bool f32 = nyr_expr_is_f32_depth(b, e->as.binary.left, depth + 1) ||
               nyr_expr_is_f32_depth(b, e->as.binary.right, depth + 1);
    bool f64 = nyr_expr_is_f64_depth(b, e->as.binary.left, depth + 1) ||
               nyr_expr_is_f64_depth(b, e->as.binary.right, depth + 1);
    if (f32 && !f64)
      return false;
  }
  if (e->kind == NY_E_LITERAL && e->as.literal.kind == NY_LIT_FLOAT)
    return e->as.literal.hint != NY_LIT_HINT_F32;
  /*
   * Identifiers with a lexical binding always returned from the typed-local
   * check earlier in this function, so no f32 local branch is needed here.
   */
  if (e->kind != NY_E_IDENT && e->kind != NY_E_CALL && e->semantic.resolved &&
      e->semantic.rep == NY_SEM_REP_F64)
    return true;
  switch (e->kind) {
  case NY_E_IDENT: {
    nyr_local_t *l = nyr_find_local_nearest(b, e->as.ident.name);
    if (l && l->type_name && !ny_native_type_name_is_any(l->type_name) &&
        ny_semantic_rep_for_type(l->type_name) != NY_SEM_REP_UNRESOLVED &&
        ny_semantic_rep_for_type(l->type_name) != NY_SEM_REP_TAGGED_DYNAMIC)
      return ny_semantic_rep_for_type(l->type_name) == NY_SEM_REP_F64;
    if (l)
      return !l->is_any && (l->semantic_rep == NY_SEM_REP_F64 || l->is_f64);
    const expr_t *g = nyr_find_top_level_value(b, e->as.ident.name);
    return g && g != e && g->kind != NY_E_IDENT &&
           nyr_expr_is_f64_depth(b, g, depth + 1);
  }
  case NY_E_BINARY: {
    nyr_cmp_t ignored;
    if (nyr_cmp(e->as.binary.op, &ignored))
      return false;
    if (e->as.binary.op && strcmp(e->as.binary.op, "**") == 0) {
      if (nyr_expr_is_dict(b, e->as.binary.left) &&
          nyr_expr_is_dict(b, e->as.binary.right))
        return true;
    }
    return nyr_expr_is_f64_depth(b, e->as.binary.left, depth + 1) ||
           nyr_expr_is_f64_depth(b, e->as.binary.right, depth + 1);
  }
  case NY_E_UNARY:
    return nyr_expr_is_f64_depth(b, e->as.unary.right, depth + 1);
  case NY_E_CALL: {
    const char *leaf = ny_native_call_leaf(e);
    /*
     * HM can leave the cast expression carrying the source float fact even
     * though builtin int(x) has an explicit raw-i64 result ABI.  The parent
     * arithmetic query runs before lowering the cast (which refreshes its
     * semantic.rep), so recognize the syntax here as a hard float-domain
     * barrier.  User-defined functions named `int` retain their declaration
     * semantics.
     */
    if (leaf && strcmp(leaf, "int") == 0 && e->as.call.args.len == 1 &&
        !e->as.call.args.data[0].name && !nyr_user_defined_fn(b, leaf))
      return false;
    if (leaf && strcmp(leaf, "get") == 0 && e->as.call.args.len >= 3) {
      if (nyr_expr_is_f64_depth(b, e->as.call.args.data[2].val, depth + 1))
        return true;
    }
    /*
     * The leaf kinds and math spellings below describe builtin runtime
     * leaves.  A user function owning the spelling decides from its
     * declaration at the resolved-callee check below, so the builtin
     * float assumption must not fire first.
     */
    if (!leaf || !nyr_user_defined_fn(b, leaf)) {
      ny_native_leaf_kind_t kind = ny_native_leaf_kind(leaf);
      if (kind == NY_NATIVE_LEAF_FLOAT || kind == NY_NATIVE_LEAF_F64BUF_LOAD ||
          kind == NY_NATIVE_LEAF_FLT_SQRT ||
          (leaf &&
           (strcmp(leaf, "load32_f32") == 0 ||
            strcmp(leaf, "load64_f64") == 0 || strcmp(leaf, "atof") == 0 ||
            strcmp(leaf, "strtod") == 0 || strcmp(leaf, "sin") == 0 ||
            strcmp(leaf, "cos") == 0 || strcmp(leaf, "sqrt") == 0 ||
            strcmp(leaf, "__flt_inf") == 0 || strcmp(leaf, "__flt_nan") == 0 ||
            strcmp(leaf, "__flt_abs") == 0 || strcmp(leaf, "__flt_sin") == 0 ||
            strcmp(leaf, "__flt_cos") == 0 || strcmp(leaf, "__flt_tan") == 0 ||
            strcmp(leaf, "__flt_exp") == 0 || strcmp(leaf, "__flt_log") == 0 ||
            strcmp(leaf, "__flt_pow") == 0 ||
            strcmp(leaf, "__flt_atan2") == 0 ||
            strcmp(leaf, "__flt_fmod") == 0)))
        return true;
    }
    if (e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT &&
        b->externs) {
      const ny_extern_entry_t *ext =
          ny_extern_table_lookup(b->externs, e->as.call.callee->as.ident.name);
      if (ext && ext->ret_f64)
        return true;
    }
    if (leaf && (strcmp(leaf, "abs") == 0 || strcmp(leaf, "min") == 0 ||
                 strcmp(leaf, "max") == 0 || strcmp(leaf, "clamp") == 0 ||
                 strcmp(leaf, "lerp") == 0)) {
      for (size_t i = 0; i < e->as.call.args.len; ++i)
        if (nyr_expr_is_f64_depth(b, e->as.call.args.data[i].val, depth + 1))
          return true;
      /*
       * The NYIR intrinsic selects its result lane from the operand class.
       * A nominal `number` return annotation is not enough to turn an integer
       * abs/min/max result into IEEE bits; that made abs(-9) print as zero
       * and let an i64 helper masquerade as an f64 producer.
       */
      if (strcmp(leaf, "abs") == 0 || strcmp(leaf, "min") == 0 ||
          strcmp(leaf, "max") == 0 || strcmp(leaf, "clamp") == 0 ||
          strcmp(leaf, "lerp") == 0) {
        const stmt_t *resolved = e->semantic.canonical_callee_stmt;
        if (resolved && resolved->kind != NY_S_FUNC)
          resolved = NULL;
        const char *callee_name =
            e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT
                ? e->as.call.callee->as.ident.name
                : NULL;
        if (!resolved && callee_name) {
          resolved = nyr_find_user_function(b, callee_name);
          if (!resolved)
            resolved = nyr_find_imported_function(b, callee_name);
        }
        if (!resolved)
          return false;
      }
    }
    if (e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT) {
      const stmt_t *fn =
          nyr_find_user_function(b, e->as.call.callee->as.ident.name);
      if (!fn)
        fn = nyr_find_imported_function(b, e->as.call.callee->as.ident.name);
      if (fn) {
        /*
         * An explicit return annotation is the callee's ABI contract.  The
         * return semantic cache may describe the expression in a contextual
         * use, so a resolved fact only refines the annotation when it stays
         * coherent with it.
         */
        if (fn->as.fn.return_type)
          return nyr_fn_return_is_f64(fn);
        if (fn->as.fn.return_semantic.resolved)
          return fn->as.fn.return_semantic.rep == NY_SEM_REP_F64;
        return false;
      }
    }
    if (e->semantic.resolved && e->semantic.rep == NY_SEM_REP_F64) {
      bool lane_polymorphic_intrinsic =
          leaf && (strcmp(leaf, "abs") == 0 || strcmp(leaf, "min") == 0 ||
                   strcmp(leaf, "max") == 0 || strcmp(leaf, "clamp") == 0 ||
                   strcmp(leaf, "lerp") == 0);
      if (!lane_polymorphic_intrinsic)
        return true;
    }
    return false;
  }
  case NY_E_MEMCALL:
    if (e->as.memcall.name && (strcmp(e->as.memcall.name, "abs") == 0 ||
                               strcmp(e->as.memcall.name, "min") == 0 ||
                               strcmp(e->as.memcall.name, "max") == 0 ||
                               strcmp(e->as.memcall.name, "clamp") == 0 ||
                               strcmp(e->as.memcall.name, "lerp") == 0)) {
      for (size_t i = 0; i < e->as.memcall.args.len; ++i)
        if (nyr_expr_is_f64_depth(b, e->as.memcall.args.data[i].val, depth + 1))
          return true;
    }
    if (e->as.memcall.name && strcmp(e->as.memcall.name, "get") == 0 &&
        e->as.memcall.args.len >= 2) {
      if (nyr_expr_is_f64_depth(b, e->as.memcall.args.data[1].val, depth + 1))
        return true;
    }
    return false;
  case NY_E_LIST:
    /*
     * A sequence literal is never itself an f64 value.  Its element type is
     * queried by the list-lowering path; recursively classifying an all-f64
     * nested list as f64 makes the enclosing descriptor stamp a container
     * pointer with TAG_FLOAT and corrupts the next index/member operation.
     */
    return false;
  case NY_E_INDEX: {
    const char *result_type = nyr_expr_type_name(b, e);
    if (result_type && ny_native_type_name_is_f64(result_type))
      return true;
  }
    if (e->as.index.target && e->as.index.target->kind == NY_E_IDENT &&
        e->as.index.target->as.ident.name) {
      const nyr_local_t *sequence =
          nyr_find_local(b, e->as.index.target->as.ident.name);
      const char *type = sequence ? sequence->type_name : NULL;
      const char *lt = type ? strchr(type, '<') : NULL;
      if (lt) {
        const char *elem = lt + 1;
        while (*elem == ' ' || *elem == '\t')
          ++elem;
        if (strncmp(elem, "f64", 3) == 0 &&
            (elem[3] == '>' || isspace((unsigned char)elem[3])))
          return true;
      }
    }
    return nyr_expr_is_f64_depth(b, e->as.index.target, depth + 1);
  case NY_E_MEMBER: {
    if (ny_native_type_name_is_f64(nyr_layout_member_type(b, e)))
      return true;
    const expr_t *v = nyr_resolve_member_expr(b, e);
    if (v && v != e)
      return nyr_expr_is_f64_depth(b, v, depth + 1);
    return false;
  }
  default:
    return false;
  }
}

static bool nyr_expr_is_f64_depth_uncached(nyr_builder_impl_t *b,
                                           const expr_t *e, unsigned depth) {
  if (!b || !e || depth > 8)
    return false;
  ny_native_scalar_probe_enter(b);
  bool value = false;
  /*
   * Identifier classification depends on the active lexical binding. The
   * same AST node can be queried before and after a local is registered or
   * refined, so caching it by node address alone can preserve a stale f64
   * annotation over an explicitly integer local.
   */
  bool member_call =
      e->kind == NY_E_MEMCALL ||
      (e->kind == NY_E_CALL &&
       ((e->as.call.callee && e->as.call.callee->kind == NY_E_MEMBER) ||
        e->semantic.member_call_kind != NY_SEM_CALL_NONE));
  bool cacheable = e->kind != NY_E_IDENT && !member_call;
  if (cacheable && ny_native_scalar_probe_get(b, e, 64, depth, &value)) {
    ny_native_scalar_probe_leave(b);
    return value;
  }
  value = nyr_expr_is_f64_core(b, e, depth);
  if (cacheable)
    ny_native_scalar_probe_put(b, e, 64, depth, value);
  ny_native_scalar_probe_leave(b);
  return value;
}

static bool nyr_expr_is_f64_depth(nyr_builder_impl_t *b, const expr_t *e,
                                  unsigned depth) {
  if (!b || !e || depth > 8)
    return false;
  bool member_call =
      e->kind == NY_E_MEMCALL ||
      (e->kind == NY_E_CALL &&
       ((e->as.call.callee && e->as.call.callee->kind == NY_E_MEMBER) ||
        e->semantic.member_call_kind != NY_SEM_CALL_NONE));
  if (!b->type_query_depth || e->kind == NY_E_IDENT || member_call)
    return nyr_expr_is_f64_depth_uncached(b, e, depth);
  const uint8_t kind = 2;
  const uint8_t query_depth = (uint8_t)depth;
  size_t mask = NY_CSTR_QUERY_CACHE_SIZE - 1u;
  size_t slot = (((uintptr_t)e >> 4) * UINT64_C(11400714819323198485)) & mask;
  for (size_t probe = 0; probe < NY_CSTR_QUERY_CACHE_SIZE; ++probe) {
    ny_native_cstr_query_cache_entry_t *entry = &b->cstr_query_cache[slot];
    if (entry->generation != b->cstr_query_generation)
      break;
    if (entry->expr == e && entry->kind == kind && entry->depth == query_depth)
      return entry->value;
    slot = (slot + 1) & mask;
  }
  bool value = nyr_expr_is_f64_depth_uncached(b, e, depth);
  slot = (((uintptr_t)e >> 4) * UINT64_C(11400714819323198485)) & mask;
  for (size_t probe = 0; probe < NY_CSTR_QUERY_CACHE_SIZE; ++probe) {
    ny_native_cstr_query_cache_entry_t *entry = &b->cstr_query_cache[slot];
    if (entry->generation != b->cstr_query_generation ||
        (entry->expr == e && entry->kind == kind &&
         entry->depth == query_depth)) {
      *entry = (ny_native_cstr_query_cache_entry_t){
          .expr = e,
          .generation = b->cstr_query_generation,
          .kind = kind,
          .depth = query_depth,
          .value = value};
      break;
    }
    slot = (slot + 1) & mask;
  }
  return value;
}

static bool nyr_expr_is_f64(nyr_builder_impl_t *b, const expr_t *e) {
  if (!b || !e)
    return false;
  return nyr_expr_is_f64_depth(b, e, 0);
}

static bool nyr_expr_is_f32_core(nyr_builder_impl_t *b, const expr_t *e,
                                 unsigned depth) {
  if (!e || depth > 8)
    return false;
  if (e->kind == NY_E_BINARY && e->semantic.resolved_type_name)
    return ny_native_type_name_is_f32(e->semantic.resolved_type_name);
  /*
   * Literal hints determine the stored scalar class even when HM records the
   * default floating type (f64) on the literal expression.
   */
  if (e->kind == NY_E_LITERAL && e->as.literal.kind == NY_LIT_FLOAT)
    return e->as.literal.hint == NY_LIT_HINT_F32;
  /*
   * Indexing inherits the selected element ABI, not a stale carrier
   * annotation. Only an explicit f32 sequence selects the f32 load path.
   */
  if (e->kind == NY_E_INDEX) {
    const char *target_type =
        e->as.index.target ? nyr_expr_type_name(b, e->as.index.target) : NULL;
    bool explicit_f32 = target_type && (strstr(target_type, "<f32>") != NULL ||
                                        strstr(target_type, "f32[]") != NULL);
    if (e->as.index.target && e->as.index.target->kind == NY_E_IDENT &&
        e->as.index.target->as.ident.name) {
      const nyr_local_t *sequence =
          nyr_find_local(b, e->as.index.target->as.ident.name);
      explicit_f32 = explicit_f32 ||
                     (sequence && sequence->is_list && sequence->type_name &&
                      (strstr(sequence->type_name, "<f32>") != NULL ||
                       strstr(sequence->type_name, "f32[]") != NULL));
    }
    return explicit_f32;
  }
  if (e->kind == NY_E_IDENT) {
    nyr_local_t *local = nyr_find_local(b, e->as.ident.name);
    if (local && local->type_name &&
        !ny_native_type_name_is_any(local->type_name) &&
        ny_semantic_rep_for_type(local->type_name) != NY_SEM_REP_UNRESOLVED &&
        ny_semantic_rep_for_type(local->type_name) != NY_SEM_REP_TAGGED_DYNAMIC)
      return ny_native_type_name_is_f32(local->type_name);
    if (local)
      return !local->is_any &&
             (local->is_f32 || local->semantic_rep == NY_SEM_REP_F32);
  }
  /*
   * Resolve attached/module member-call return types through the declaration
   * contract.  Without this, an f32 method result is treated as an i64 word
   * when compared with an f64 literal, producing bitcast-then-sitofp code
   * instead of the required f32-to-f64 conversion.
   */
  if (e->kind == NY_E_MEMCALL || e->kind == NY_E_CALL) {
    const char *result_type = nyr_expr_type_name(b, e);
    if (result_type && ny_native_type_name_is_f32(result_type))
      return true;
    if (result_type && ny_native_type_name_is_f64(result_type))
      return false;
  }
  if (e->kind == NY_E_BINARY) {
    nyr_cmp_t ignored;
    if (nyr_cmp(e->as.binary.op, &ignored))
      return false;
    bool f32 = nyr_expr_is_f32_depth(b, e->as.binary.left, depth + 1) ||
               nyr_expr_is_f32_depth(b, e->as.binary.right, depth + 1);
    bool f64 = nyr_expr_is_f64_depth(b, e->as.binary.left, depth + 1) ||
               nyr_expr_is_f64_depth(b, e->as.binary.right, depth + 1);
    if (f32 && !f64)
      return true;
  }
  if (e->semantic.resolved && e->semantic.rep == NY_SEM_REP_F32)
    return true;
  if (e->semantic.resolved && e->semantic.rep != NY_SEM_REP_TAGGED_DYNAMIC) {
    /*
     * A module-cell global has no local-fact entry, so this resolved
     * semantic (an integer-cell fallback) is not authoritative for its
     * declared float storage.  Consult the top-level initializer before
     * rejecting: otherwise `mut g = 0.0f32` classified as int made every
     * compare bitcast the f32 words (scalar-classes "f32 global store").
     * f64 storage keeps the false — an f64 global compare needs no f32
     * conversion.
     */
    if (e->kind == NY_E_IDENT && e->as.ident.name &&
        !nyr_find_local(b, e->as.ident.name)) {
      const expr_t *g_init = e->tok.filename
                                 ? nyr_find_top_level_value_in_source(
                                       b, e->as.ident.name, e->tok.filename)
                                 : NULL;
      if (!g_init)
        g_init = nyr_find_top_level_value(b, e->as.ident.name);
      if (g_init && g_init != e) {
        /*
         * The initializer's HM semantic can be f64 even for an f32-hinted
         * literal (Nytrix floats default to f64); the literal hint is the
         * declared f32 storage fact.
         */
        if (nyr_expr_is_f32_depth(b, g_init, depth + 1))
          return true;
        if (g_init->kind == NY_E_LITERAL &&
            g_init->as.literal.kind == NY_LIT_FLOAT &&
            g_init->as.literal.hint == NY_LIT_HINT_F32 &&
            !nyr_expr_is_f64_depth(b, g_init, depth + 1))
          return true;
      }
    }
    return false;
  }
  if (e->kind == NY_E_LITERAL)
    return false;
  /*
   * Keep explicit local/parameter types ahead of stale semantic annotations.
   */
  if (e->kind == NY_E_IDENT) {
    nyr_local_t *l = nyr_find_local(b, e->as.ident.name);
    if (l)
      return !l->is_any && (l->semantic_rep == NY_SEM_REP_F32 || l->is_f32);
  }
  if (e->kind == NY_E_BINARY) {
    bool f32 = nyr_expr_is_f32_depth(b, e->as.binary.left, depth + 1) ||
               nyr_expr_is_f32_depth(b, e->as.binary.right, depth + 1);
    bool f64 = nyr_expr_is_f64_depth(b, e->as.binary.left, depth + 1) ||
               nyr_expr_is_f64_depth(b, e->as.binary.right, depth + 1);
    if (f32 && !f64)
      return true;
  }
  if (e->kind == NY_E_INDEX) {
    const expr_t *literal_target =
        nyr_resolve_list_literal(b, e->as.index.target, 0);
    if (literal_target && literal_target->kind == NY_E_LIST &&
        !nyr_expr_is_dyn_list(b, literal_target))
      return false;
    /*
     * Semantic inference may stamp the index node with the carrier's raw
     * representation even when its selected element is itself a container.
     * Recover the element type before that broad fallback so chained reads
     * preserve a nested list as a dynamic value but keep scalar leaves raw.
     * Dynamic/descriptor-backed sequence reads return a tagged value.  The
     * carrier may currently be a compact list, but that does not establish
     * an f32 element ABI; treating it as one makes `to_str(xs[i])` reinterpret
     * the tagged pointer as a float.
     */
    if (e->as.index.target && (nyr_expr_is_any(b, e->as.index.target) ||
                               nyr_expr_is_dyn_list(b, e->as.index.target)))
      return false;
    const char *type = nyr_expr_type_name(b, e);
    if (type &&
        (strcmp(type, "any") == 0 || strncmp(type, "list<", 5) == 0 ||
         strncmp(type, "dict<", 5) == 0 || strncmp(type, "set<", 4) == 0 ||
         strcmp(type, "tuple") == 0 || strcmp(type, "list") == 0))
      return true;
    return false;
  }
  if (e->semantic.resolved)
    return e->semantic.rep == NY_SEM_REP_F32;
  switch (e->kind) {
  case NY_E_IDENT: {
    nyr_local_t *l = nyr_find_local(b, e->as.ident.name);
    if (l)
      return !l->is_any && (l->semantic_rep == NY_SEM_REP_F32 || l->is_f32);
    const expr_t *g = nyr_find_top_level_value(b, e->as.ident.name);
    return g && g != e && g->kind != NY_E_IDENT &&
           nyr_expr_is_f32_depth(b, g, depth + 1);
  }
  case NY_E_BINARY: {
    nyr_cmp_t ignored;
    if (nyr_cmp(e->as.binary.op, &ignored))
      return false;
    return nyr_expr_is_f32_depth(b, e->as.binary.left, depth + 1) ||
           nyr_expr_is_f32_depth(b, e->as.binary.right, depth + 1);
  }
  case NY_E_UNARY:
    return nyr_expr_is_f32_depth(b, e->as.unary.right, depth + 1);
  case NY_E_CALL:
    if (e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT) {
      const stmt_t *fn =
          nyr_find_user_function(b, e->as.call.callee->as.ident.name);
      if (!fn)
        fn = nyr_find_imported_function(b, e->as.call.callee->as.ident.name);
      if (fn)
        return (fn->as.fn.return_semantic.resolved &&
                fn->as.fn.return_semantic.rep == NY_SEM_REP_F32) ||
               ny_native_type_name_is_f32(fn->as.fn.return_type);
    }
    return false;
  case NY_E_MEMBER:
    return ny_native_type_name_is_f32(nyr_layout_member_type(b, e));
  default:
    return false;
  }
}

static bool nyr_expr_is_f32_depth(nyr_builder_impl_t *b, const expr_t *e,
                                  unsigned depth) {
  if (!b || !e || depth > 8)
    return false;
  /*
   * Literal suffixes are source-level ABI facts. Do not let an earlier
   * conservative semantic query cache the default f64 class for an f32
   * literal.
   */
  if (e->kind == NY_E_LITERAL && e->as.literal.kind == NY_LIT_FLOAT)
    return e->as.literal.hint == NY_LIT_HINT_F32;
  ny_native_scalar_probe_enter(b);
  bool value = false;
  bool cacheable = e->kind != NY_E_IDENT;
  if (cacheable && ny_native_scalar_probe_get(b, e, 32, depth, &value)) {
    ny_native_scalar_probe_leave(b);
    return value;
  }
  value = nyr_expr_is_f32_core(b, e, depth);
  if (cacheable)
    ny_native_scalar_probe_put(b, e, 32, depth, value);
  ny_native_scalar_probe_leave(b);
  return value;
}

static bool nyr_expr_is_f32(nyr_builder_impl_t *b, const expr_t *e) {
  return nyr_expr_is_f32_depth(b, e, 0);
}

/*
 * Bigints are represented by runtime handles, not by the raw i64 arithmetic
 * domain.  Keep unary negation in that domain when the source expression is
 * statically known to produce a bigint (large literals, aliases, or bigint
 * returning calls).
 */
static bool nyr_expr_is_bigint(nyr_builder_impl_t *b, const expr_t *e) {
  if (!b || !e)
    return false;
  const stmt_t *source_attached = nyr_source_attached_result(b, e);
  if (source_attached)
    return ny_type_is_bigint(source_attached->as.fn.return_type);
  /*
   * A resolved raw-int identifier is a scalar even if a same-named global,
   * enclosing impl, or conservative type query suggests a heap object.  This
   * must precede expr_type_name: that memo can return the bigint type of a
   * different binding for an impl method's int parameter.
   */
  if (e->kind == NY_E_IDENT && e->semantic.resolved &&
      e->semantic.rep == NY_SEM_REP_RAW_INT) {
    /*
     * The lexical record outranks the stale use-site stamp: an untyped
     * local bound from a bigint-returning call (`def x = bigint_abs(Z(n))`)
     * keeps a RAW_INT HM fact on its identifier, but the slot holds a
     * bigint handle.  Trusting the stamp sent `x & Z(255)` through the raw
     * bitwise path and mangled the handle into a scalar.
     */
    const nyr_local_t *bigint_local =
        nyr_find_local((nyr_builder_t *)b, e->as.ident.name);
    if (bigint_local && !bigint_local->is_bigint &&
        bigint_local->syntax_ctx != 0)
      return false;
  }

  /*
   * Identifier bindings outrank the use-site type memo. A dynamic `any`
   * parameter can have a conservative BigInt type fact from an operator
   * result even though its runtime value is not statically a BigInt.
   */
  if (e->kind == NY_E_IDENT && e->as.ident.name) {
    nyr_local_t *local = nyr_find_local(b, e->as.ident.name);
    if (local) {
      if (local->is_bigint ||
          (local->type_name && ny_type_is_bigint(local->type_name)))
        return true;
      /*
       * Root bindings live in the entry builder's local table while their
       * storage remains a module global. Recover a proven BigInt initializer
       * without letting an enclosing/global binding override lexical locals.
       */
      if (b->current_fn_name && strcmp(b->current_fn_name, "rt_main") == 0 &&
          local->syntax_ctx == 0 && local->is_any) {
        const expr_t *global = e->tok.filename
                                   ? nyr_find_top_level_value_in_source(
                                         b, e->as.ident.name, e->tok.filename)
                                   : NULL;
        if (!global)
          global = nyr_find_top_level_value(b, e->as.ident.name);
        if (global && global != e && global->kind != NY_E_IDENT &&
            nyr_expr_is_bigint(b, global))
          return true;
      }
      return false;
    }
    const expr_t *value = e->tok.filename
                              ? nyr_find_top_level_value_in_source(
                                    b, e->as.ident.name, e->tok.filename)
                              : nyr_find_top_level_value(b, e->as.ident.name);
    if (!value)
      value = nyr_find_top_level_value(b, e->as.ident.name);
    return value && value != e && value->kind != NY_E_IDENT &&
           nyr_expr_is_bigint(b, value);
  }

  /*
   * Some low-level BigInt intrinsics have intentionally broad source
   * declarations, but their runtime result is an opaque BigInt handle.  Use
   * the ABI record before comparison/call lowering chooses an `any` path for
   * an inline intrinsic result.
   */
  if (e->kind == NY_E_CALL) {
    const char *leaf = ny_native_call_leaf(e);
    const char *symbol =
        leaf ? ny_native_runtime_symbol_for_expr(leaf, leaf, e) : NULL;
    const ny_native_runtime_abi_decl_t *decl =
        symbol ? ny_native_runtime_abi_decl(symbol) : NULL;
    if (ny_native_runtime_abi_result_is_bigint(decl))
      return true;
  }
  /*
   * The semantic resolver already knows the return type for qualified
   * module calls, while the syntactic fallback below only sees a subset of
   * spellings.  Use that resolved type first so an imported constructor such
   * as `core.math.nt.Z` is treated as the heap handle it returns when it
   * crosses an `any` boundary.
   */
  const char *resolved_type = nyr_expr_type_name(b, e);
  if (resolved_type && ny_type_is_bigint(resolved_type))
    return true;
  /*
   * Memo-poisoning guard: expr_type_name consults the type-query memo,
   * whose persistent entries can hold a miss from an early query that ran
   * before the function index was collected.  For a direct call, resolve
   * the callee's declared return type straight from the function table —
   * the authoritative source that bypasses the memo — so operator dispatch
   * cannot lose a bigint operand to a stale miss (the `5^Z(2)` xor-zero
   * regression).
   */
  if (e->kind == NY_E_CALL && e->as.call.callee &&
      e->as.call.callee->kind == NY_E_IDENT &&
      e->as.call.callee->as.ident.name) {
    /*
     * The callee name may be qualified (module expansion rewrites Z to
     * core.math.nt.Z); resolve through the LEAF so the guard fires for
     * qualified spellings too.
     */
    const char *guard_leaf =
        ny_name_leaf(e->as.call.callee->as.ident.name);
    const stmt_t *callee_fn =
        guard_leaf ? nyr_find_user_function((nyr_builder_t *)b, guard_leaf)
                   : NULL;
    /*
     * Only a declared bigint return type proves the handle ABI here. A
     * resolved TAGGED_DYNAMIC semantic is shared by every `any` return, so
     * treating it as bigint routed to_str(any) and operator dispatch
     * through the bigint lane for ordinary dynamic callees (nil printed as
     * "0", `bigint.ne`/`vec3.divs` unresolved).
     */
    if (callee_fn && callee_fn->kind == NY_S_FUNC &&
        callee_fn->as.fn.return_type &&
        ny_type_is_bigint(callee_fn->as.fn.return_type))
      return true;
  }
  if (e->kind == NY_E_LITERAL && e->as.literal.kind == NY_LIT_INT &&
      e->tok.kind != NY_T_NIL)
    return e->as.literal.as.i >= (INT64_C(1) << 62) ||
           e->as.literal.as.i <= -(INT64_C(1) << 62);
  if (e->kind == NY_E_UNARY && e->as.unary.right) {
    return nyr_expr_is_bigint(b, e->as.unary.right);
  }
  if (e->kind == NY_E_BINARY) {
    if (e->as.binary.op &&
        (strcmp(e->as.binary.op, "+") == 0 ||
         strcmp(e->as.binary.op, "-") == 0 ||
         strcmp(e->as.binary.op, "*") == 0 ||
         strcmp(e->as.binary.op, "/") == 0 ||
         strcmp(e->as.binary.op, "%") == 0 ||
         strcmp(e->as.binary.op, "**") == 0 ||
         strcmp(e->as.binary.op, "^") == 0 ||
         strcmp(e->as.binary.op, "<<") == 0 ||
         strcmp(e->as.binary.op, ">>") == 0 ||
         strcmp(e->as.binary.op, "&") == 0 ||
         strcmp(e->as.binary.op, "|") == 0) &&
        (nyr_expr_is_bigint(b, e->as.binary.left) ||
         nyr_expr_is_bigint(b, e->as.binary.right)))
      return true;
    int64_t value = 0;
    /*
     * A folded arithmetic result can cross the tagged-small-int boundary even
     * when neither source operand is large (e.g. MAX_SMALL + 1).
     */
    if (nyr_fold_top_level_int(b, NULL, e, &value, 0))
      return value >= (INT64_C(1) << 62) || value <= -(INT64_C(1) << 62);
  }
  if (e->kind == NY_E_COMPTIME && e->as.comptime_expr.body) {
    const stmt_t *body = e->as.comptime_expr.body;
    if (body->kind == NY_S_RETURN && body->as.ret.value)
      return nyr_expr_is_bigint(b, body->as.ret.value);
    if (body->kind == NY_S_EXPR && body->as.expr.expr)
      return nyr_expr_is_bigint(b, body->as.expr.expr);
    if (body->kind == NY_S_BLOCK) {
      for (size_t i = body->as.block.body.len; i-- > 0;) {
        const stmt_t *item = body->as.block.body.data[i];
        if (!item)
          continue;
        if (item->kind == NY_S_RETURN && item->as.ret.value)
          return nyr_expr_is_bigint(b, item->as.ret.value);
        if (item->kind == NY_S_EXPR && item->as.expr.expr)
          return nyr_expr_is_bigint(b, item->as.expr.expr);
      }
    }
  }
  if (e->kind == NY_E_CALL && e->as.call.callee &&
      e->as.call.callee->kind == NY_E_IDENT) {
    const char *name = e->as.call.callee->as.ident.name;
    bool qualified_z =
        name &&
        ((strlen(name) >= 2 && strcmp(name + strlen(name) - 2, ".Z") == 0) ||
         (strlen(name) >= 9 &&
          strcmp(name + strlen(name) - 9, ".bigint") == 0));
    if (name &&
        (qualified_z || strcmp(name, "bigint_from_int") == 0 ||
         strcmp(name, "__bigint_from_int") == 0 ||
         strcmp(name, "nt_bigint") == 0 || strcmp(name, "Z") == 0 ||
         strcmp(name, "bigint") == 0 || strcmp(name, "long") == 0 ||
         strcmp(name, "__long") == 0 || strcmp(name, "bigint_add") == 0 ||
         strcmp(name, "bigint_sub") == 0 || strcmp(name, "bigint_mul") == 0 ||
         strcmp(name, "bigint_div") == 0 || strcmp(name, "bigint_mod") == 0 ||
         strcmp(name, "bigint_pow") == 0 ||
         strcmp(name, "bigint_from_str") == 0))
      return true;
    const stmt_t *fn = nyr_find_user_function(b, name);
    if (!fn)
      fn = nyr_find_imported_function(b, name);
    return fn && fn->kind == NY_S_FUNC &&
           ny_type_is_bigint(fn->as.fn.return_type);
  }
  if (e->kind == NY_E_CALL && e->as.call.callee &&
      e->as.call.callee->kind == NY_E_MEMBER &&
      e->as.call.callee->as.member.name) {
    const char *mname = e->as.call.callee->as.member.name;
    /*
     * Qualified imports (`core.math.nt.Z`, `core.math.big.bigint`, etc.)
     * resolve to a member callee.  They have the same handle ABI as their
     * unqualified spelling; do not let the dynamic argument bridge treat the
     * returned pointer as a raw integer and shift it.
     */
    if (strcmp(mname, "long") == 0 || strcmp(mname, "__long") == 0 ||
        strcmp(mname, "Z") == 0 || strcmp(mname, "ZZ") == 0 ||
        strcmp(mname, "Int") == 0 || strcmp(mname, "Integer") == 0 ||
        strcmp(mname, "bigint") == 0 || strcmp(mname, "bigint_from_int") == 0 ||
        strcmp(mname, "bigint_from_str") == 0 ||
        strcmp(mname, "nt_bigint") == 0 || strcmp(mname, "bigint_add") == 0 ||
        strcmp(mname, "bigint_sub") == 0 || strcmp(mname, "bigint_mul") == 0 ||
        strcmp(mname, "bigint_div") == 0 || strcmp(mname, "bigint_mod") == 0 ||
        strcmp(mname, "bigint_pow") == 0)
      return true;
  }
  if (e->kind == NY_E_MEMBER && e->as.member.name &&
      strcmp(e->as.member.name, "long") == 0)
    return true;
  if (e->kind == NY_E_MEMCALL && e->as.memcall.name &&
      (strcmp(e->as.memcall.name, "long") == 0 ||
       strcmp(e->as.memcall.name, "__long") == 0))
    return true;
  return false;
}

/*
 * BigFloat values are heap handles carried through the dynamic ABI.  Keep a
 * small semantic classifier alongside the bigint classifier so formatting and
 * other value consumers can select the handle-preserving bridge without
 * relying on source spelling at the call site.
 */
static bool nyr_expr_is_bigfloat(nyr_builder_impl_t *b, const expr_t *e) {
  if (!b || !e)
    return false;
  if (e->kind == NY_E_IDENT && e->as.ident.name) {
    nyr_local_t *local = nyr_find_local(b, e->as.ident.name);
    if (local && local->type_name && strcmp(local->type_name, "bigfloat") == 0)
      return true;
    const expr_t *value = nyr_find_top_level_value(b, e->as.ident.name);
    return value && value != e && nyr_expr_is_bigfloat(b, value);
  }
  if (e->kind == NY_E_CALL && e->as.call.callee &&
      e->as.call.callee->kind == NY_E_IDENT) {
    const char *name = e->as.call.callee->as.ident.name;
    if (name && (strstr(name, "bigfloat") || strncmp(name, "bf_", 3) == 0))
      return strcmp(name, "bf_to_float") != 0 &&
             strcmp(name, "bf_to_str") != 0 &&
             strcmp(name, "bf_precision") != 0;
    const stmt_t *fn = name ? nyr_find_user_function(b, name) : NULL;
    return fn && fn->kind == NY_S_FUNC && fn->as.fn.return_type &&
           strcmp(fn->as.fn.return_type, "bigfloat") == 0;
  }
  return false;
}

static bool nyr_expr_is_cstr_impl(nyr_builder_impl_t *b, const expr_t *e) {
  if (!e)
    return false;
  if (nyr_expr_is_bytes((const nyr_builder_impl_t *)b, e))
    return false;
  /*
   * Dynamic dictionary lookup returns a tagged value.  The generic `get`
   * declaration can acquire a contextual string type from its consumer, but
   * that does not change the runtime representation returned by the native
   * dictionary bridge.  Decide this before type-name inference can feed that
   * contextual type back into the representation query.
   */
  const expr_t *get_receiver = NULL;
  const char *get_name = NULL;
  if (e->kind == NY_E_MEMCALL) {
    get_receiver = e->as.memcall.target;
    get_name = e->as.memcall.name;
  } else if (e->kind == NY_E_CALL && e->as.call.callee &&
             e->as.call.callee->kind == NY_E_MEMBER) {
    get_receiver = e->as.call.callee->as.member.target;
    get_name = e->as.call.callee->as.member.name;
  } else if (e->kind == NY_E_CALL && ny_native_call_leaf(e) &&
             strcmp(ny_native_call_leaf(e), "get") == 0 &&
             e->as.call.args.len >= 2) {
    /*
     * Member calls may already have been canonicalized to get(receiver, key,
     * default) by the time representation inference sees them.
     */
    get_receiver = e->as.call.args.data[0].val;
    get_name = "get";
  }
  if (get_receiver && get_name && strcmp(get_name, "get") == 0) {
    /*
     * A dictionary or `any` lookup always keeps the tagged result ABI, even
     * when its caller supplies a string fallback or returns a string. For a
     * statically known sequence/string, keep the normal semantic query below:
     * it carries the element result type (and avoids treating every list get
     * as a dynamic value merely because the fallback is numeric).
     */
    if (nyr_expr_is_dict(b, get_receiver) || nyr_expr_is_any(b, get_receiver))
      return false;
  }
  /*
   * Literal strings are C-string values regardless of a conservative
   * unresolved semantic annotation attached by the front end.
   */
  if (e->kind == NY_E_LITERAL)
    return e->as.literal.kind == NY_LIT_STR;
  /*
   * `expr_type_name` may ask whether the very expression currently on its
   * query stack is a string before it tries nominal operator resolution.  Do
   * not start that same binary query again; the enclosing type query will
   * resolve it from its operands.  This guard is per builder, so independent
   * native functions and compiler threads do not share classifier state.
   */
  if (b && b->type_query_depth > 0) {
    for (size_t i = 0; i < b->type_query_depth; ++i) {
      if (b->type_query_stack[i] == e)
        return e->semantic.resolved && e->semantic.rep == NY_SEM_REP_STRING;
    }
  }
  /*
   * These builtins return borrowed native C strings, not managed Ny `str`
   * values.  Resolve them before the generic imported-function type check.
   */
  if (e->kind == NY_E_CALL && ny_native_call_leaf(e)) {
    const char *leaf = ny_native_call_leaf(e);
    if (strcmp(leaf, "to_str") == 0 || strcmp(leaf, "type") == 0)
      return true;
  }
  /*
   * Type-name inference consults representation predicates while walking an
   * unresolved call.  Only let the classifier start that query from outside
   * an active type query: once type-name inference is already asking about
   * this expression, calling back into it is a cyclic query, not additional
   * type information.
   */
  if (e->kind == NY_E_CALL || e->kind == NY_E_MEMCALL) {
    if (!b || b->type_query_depth == 0) {
      const char *type = nyr_expr_type_name(b, e);
      /*
       * Declared native `str` results use a character pointer whether the
       * storage is a managed string or a borrowed C string. Imported source
       * functions follow the same ABI as local source functions.
       */
      if (type && (strcmp(type, "str") == 0 || strcmp(type, "string") == 0))
        return true;
    }
    /*
     * Native formatter bridges return an untagged C string even though their
     * source declarations are intentionally dynamic for the interpreter.
     * Keep the result representation attached to the resolved runtime symbol
     * so a later consumer such as print/equality does not reinterpret the
     * pointer as a raw integer.
     */
    const char *callee_name = NULL;
    const char *callee_leaf = NULL;
    if (e->kind == NY_E_CALL && e->as.call.callee &&
        e->as.call.callee->kind == NY_E_IDENT) {
      callee_name = e->as.call.callee->as.ident.name;
      callee_leaf = ny_native_call_leaf(e);
    } else if (e->kind == NY_E_MEMCALL) {
      callee_name = e->as.memcall.name;
      callee_leaf = e->as.memcall.name;
    }
    const char *cstr_symbol =
        callee_name
            ? ny_native_runtime_symbol_for_expr(callee_name, callee_leaf, e)
            : NULL;
    const ny_native_runtime_abi_decl_t *cstr_abi =
        ny_native_runtime_abi_decl(cstr_symbol);
    if (ny_native_runtime_abi_result_is_cstr(cstr_abi))
      return true;
  }
  /*
   * The semantic pass can retain the dynamic representation of the
   * implementation selected for an attached method (`str.upper` is backed
   * by an `any`-parameter helper).  The selected method's declared return is
   * the ABI fact that matters here: a `str` result is a raw C-string pointer
   * in native lowering.  Consult the resolved target before the generic
   * semantic fallback so a local initialized from `"x".upper()` is printed
   * through the string path instead of as an integer address.
   */
  if (e->kind == NY_E_MEMCALL && e->as.memcall.name) {
    const stmt_t *fn =
        nyr_find_attached_method(b, e->as.memcall.target, e->as.memcall.name);
    if (!fn && e->as.memcall.target &&
        e->as.memcall.target->kind == NY_E_IDENT &&
        e->as.memcall.target->as.ident.name) {
      const char *module =
          nyr_resolve_use_alias(b, e->as.memcall.target->as.ident.name);
      if (module) {
        /*
         * Resolution caches may retain the lookup spelling beyond this
         * classifier call, so retain it in the active builder's arena.
         */
        const char *qualified =
            nyr_builder_own_qualified(b, module, e->as.memcall.name);
        if (qualified)
          fn = nyr_find_user_function(b, qualified);
      }
    }
    if (fn && fn->as.fn.return_type &&
        strcmp(fn->as.fn.return_type, "str") == 0)
      return true;
  }
  /*
   * Property syntax lowers through the same attached-method ABI as a
   * zero-argument member call.  Keep the selected method's declared return
   * representation visible to consumers such as print/to_str; otherwise a
   * raw C-string result is mistaken for an opaque integer pointer.
   */
  if (e->kind == NY_E_MEMBER && e->as.member.name) {
    const stmt_t *resolved = e->semantic.canonical_callee_stmt;
    if (resolved && resolved->kind == NY_S_FUNC &&
        resolved->as.fn.return_type &&
        strcmp(resolved->as.fn.return_type, "str") == 0)
      return true;
    const stmt_t *fn =
        nyr_find_attached_method(b, e->as.member.target, e->as.member.name);
    if (fn && fn->as.fn.return_type &&
        strcmp(fn->as.fn.return_type, "str") == 0)
      return true;
  }
  if (e->kind == NY_E_CALL) {
    if (nyr_expr_is_set(b, e) || nyr_expr_is_tuple(b, e))
      return false;
    const stmt_t *fn = e->semantic.canonical_callee_stmt;
    if (!fn && e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT &&
        e->as.call.callee->as.ident.name)
      fn = nyr_find_user_function(b, e->as.call.callee->as.ident.name);
    if (!fn && e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT &&
        e->as.call.callee->as.ident.name)
      fn = nyr_find_imported_function(b, e->as.call.callee->as.ident.name);
    if (!fn && e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT &&
        e->as.call.callee->as.ident.name && b->current_fn_name) {
      const char *sep = strrchr(b->current_fn_name, '.');
      if (sep && sep != b->current_fn_name) {
        char qualified[512];
        int n = snprintf(qualified, sizeof(qualified), "%.*s.%s",
                         (int)(sep - b->current_fn_name), b->current_fn_name,
                         e->as.call.callee->as.ident.name);
        if (n > 0 && (size_t)n < sizeof(qualified)) {
          fn = nyr_find_user_function(b, qualified);
          if (!fn)
            fn = nyr_find_imported_function(b, qualified);
        }
      }
    }
    if (fn && fn->kind == NY_S_FUNC && fn->as.fn.return_type &&
        strcmp(fn->as.fn.return_type, "str") == 0)
      return true;
  }
  /*
   * Do not start overload resolution from the representation classifier.
   * Operator lookup asks for both operand types, whose classifiers can
   * re-enter this function for nested expressions. The type checker owns the
   * selected overload result when it is known; unresolved `+` concatenation
   * is handled structurally below.
   */
  if (e->kind == NY_E_BINARY) {
    if (e->semantic.resolved_type_name)
      return strcmp(e->semantic.resolved_type_name, "str") == 0 ||
             strcmp(e->semantic.resolved_type_name, "string") == 0;
    if (e->semantic.resolved)
      return e->semantic.rep == NY_SEM_REP_STRING;
  }
  if (e->kind == NY_E_INDEX) {
    if (!e->as.index.target || nyr_expr_is_bytes(b, e->as.index.target))
      return false;
    /*
     * An annotated `list<str>` local stores raw C-string pointers in its
     * element slots; index reads from it are C strings.
     */
    if (e->as.index.target->kind == NY_E_IDENT &&
        e->as.index.target->as.ident.name) {
      nyr_local_t *l = nyr_find_local(b, e->as.index.target->as.ident.name);
      if (l && l->type_name && strstr(l->type_name, "list<str>"))
        return true;
    }
    return nyr_expr_is_cstr(b, e->as.index.target);
  }
  /*
   * FFI math calls return raw f64 values even when the semantic pass has
   * conservatively marked an unresolved C declaration as dynamic/string.
   */
  if (e->kind == NY_E_CALL) {
    const char *leaf = ny_native_call_leaf(e);
    if (leaf && (strcmp(leaf, "sin") == 0 || strcmp(leaf, "cos") == 0 ||
                 strcmp(leaf, "sqrt") == 0))
      return false;
  }
  /*
   * A sequence `get(index, string_default)` can yield a raw C string even
   * when semantic resolution records the generic method result as `any`.
   * Preserve the string ABI for proven list/string receivers; dictionary
   * lookup has separate key/value semantics.  The string ABI claim must be
   * backed by a string-valued default: inside a str-returning helper every
   * `io.get("int_key", 4096)` used to classify cstr, and the following
   * integer compares went through rt_cstr_cmp on raw words (the net tube
   * probes segfaulted in _read_more).
   */
  if (e->kind == NY_E_MEMCALL && e->as.memcall.name &&
      strcmp(e->as.memcall.name, "get") == 0 && e->as.memcall.args.len == 2 &&
      nyr_current_returns_cstr(b) && e->as.memcall.args.data[1].val &&
      nyr_expr_is_cstr(b, e->as.memcall.args.data[1].val))
    return true;
  if (e->kind == NY_E_MEMCALL && e->as.memcall.name &&
      strcmp(e->as.memcall.name, "get") == 0 && e->as.memcall.args.len == 2 &&
      e->as.memcall.args.data[1].val &&
      nyr_expr_is_cstr(b, e->as.memcall.args.data[1].val))
    return true;
  if (e->kind == NY_E_MEMCALL && e->as.memcall.name &&
      strcmp(e->as.memcall.name, "get") == 0 && e->as.memcall.args.len == 2 &&
      (nyr_expr_is_list(b, e->as.memcall.target) ||
       nyr_expr_is_cstr(b, e->as.memcall.target)))
    return nyr_expr_is_cstr(b, e->as.memcall.args.data[1].val);
  if (e->kind == NY_E_MEMCALL && e->as.memcall.name &&
      strcmp(e->as.memcall.name, "slice") == 0 &&
      nyr_expr_is_cstr(b, e->as.memcall.target))
    return true;
  if (e->kind == NY_E_CALL && ny_native_call_leaf(e) &&
      strcmp(ny_native_call_leaf(e), "get") == 0 &&
      nyr_current_returns_cstr(b) &&
      ((e->as.call.args.len == 2 && e->as.call.args.data[1].val &&
        nyr_expr_is_cstr(b, e->as.call.args.data[1].val)) ||
       (e->as.call.args.len >= 3 && e->as.call.args.data[2].val &&
        nyr_expr_is_cstr(b, e->as.call.args.data[2].val))))
    return true;
  if (e->kind == NY_E_CALL && ny_native_call_leaf(e) &&
      strcmp(ny_native_call_leaf(e), "get") == 0 &&
      ((e->as.call.args.len == 2 && e->as.call.args.data[1].val &&
        nyr_expr_is_cstr(b, e->as.call.args.data[1].val)) ||
       (e->as.call.args.len >= 3 && e->as.call.args.data[2].val &&
        nyr_expr_is_cstr(b, e->as.call.args.data[2].val))))
    return true;
  /*
   * A parameter or local takes precedence over a module global with the
   * same spelling.  Looking up the global initializer first can classify an
   * `int value` parameter as a string when the module also has `str value`,
   * sending `value + 1` to rt_cstr_concat with a raw integer pointer.
   */
  if (e->kind == NY_E_IDENT && e->as.ident.name) {
    nyr_local_t *l = nyr_find_local(b, e->as.ident.name);
    if (l) {
      if (l->is_bytes)
        return false;
      if (l->type_name &&
          (!strcmp(l->type_name, "set") || !strcmp(l->type_name, "tuple")))
        return false;
      return l->semantic_rep == NY_SEM_REP_STRING || l->is_cstr;
    }
  }
  /*
   * Resolve imported/module constants from their initializer before trusting
   * a conservative semantic representation. String constants such as
   * ERR_DIV_ZERO can otherwise enter the numeric comparison ABI.
   */
  if (e->kind == NY_E_IDENT && e->as.ident.name) {
    const expr_t *g = nyr_find_top_level_value(b, e->as.ident.name);
    if (!g)
      g = nyr_find_imported_value(b, e->as.ident.name);
    if (g && g != e && g->kind != NY_E_IDENT)
      return nyr_expr_is_cstr(b, g);
  }
  /*
   * String concatenation is selected from its operands in native lowering.
   * A stale/unknown joined semantic fact must not hide that raw C-string ABI
   * from an `any` call boundary such as assert_eq.
   */
  if (e->kind == NY_E_BINARY && e->as.binary.op &&
      strcmp(e->as.binary.op, "+") == 0 &&
      (nyr_expr_is_cstr(b, e->as.binary.left) ||
       nyr_expr_is_cstr(b, e->as.binary.right)))
    return true;
  if (e->semantic.resolved)
    return e->semantic.rep == NY_SEM_REP_STRING;
  switch (e->kind) {
  case NY_E_LITERAL:
    return e->as.literal.kind == NY_LIT_STR;
  case NY_E_FSTRING:
    return true; /* lowered to a runtime C string */
  case NY_E_IDENT: {
    nyr_local_t *l = nyr_find_local(b, e->as.ident.name);
    if (l) {
      if (l->is_bytes)
        return false;
      if (l->type_name &&
          (!strcmp(l->type_name, "set") || !strcmp(l->type_name, "tuple")))
        return false;
      return l->semantic_rep == NY_SEM_REP_STRING || l->is_cstr;
    }
    /*
     * Module-level global: classify by its initializer.  Do not chase
     * ident->ident global chains — that keeps this bounded and cycle-free.
     */
    const expr_t *g = nyr_find_top_level_value(b, e->as.ident.name);
    if (!g)
      g = nyr_find_imported_value(b, e->as.ident.name);
    if (g && g != e && g->kind != NY_E_IDENT)
      return nyr_expr_is_cstr(b, g);
    return false;
  }
  case NY_E_BINARY:
    return e->as.binary.op && strcmp(e->as.binary.op, "+") == 0 &&
           (nyr_expr_is_cstr(b, e->as.binary.left) ||
            nyr_expr_is_cstr(b, e->as.binary.right));
  case NY_E_TERNARY:
    /*
     * A ternary yields a string only when both arms do.
     */
    return nyr_expr_is_cstr(b, e->as.ternary.true_expr) &&
           nyr_expr_is_cstr(b, e->as.ternary.false_expr);
  case NY_E_CALL: {
    /*
     * Calls through module aliases carry a member callee.  Resolve both
     * direct and member calls through the same leaf-name path used by native
     * call lowering, then classify from the function's declared return.
     * Deliberately limited to same-file functions: an imported `str` return
     * may still materialize as a managed handle (not a raw C string), and
     * routing that word into strlen-based concat crashes.
     */
    const char *leaf = ny_native_call_leaf(e);
    if (leaf && (strcmp(leaf, "to_str") == 0 || strcmp(leaf, "type") == 0))
      return true;
    const stmt_t *fn = leaf ? nyr_find_user_function(b, leaf) : NULL;
    return fn && fn->as.fn.return_type &&
           strcmp(fn->as.fn.return_type, "str") == 0;
  }
  case NY_E_MEMCALL: {
    const stmt_t *fn = e->as.memcall.name
                           ? nyr_find_user_function(b, e->as.memcall.name)
                           : NULL;
    return fn && fn->as.fn.return_type &&
           strcmp(fn->as.fn.return_type, "str") == 0;
  }
  default:
    return false;
  }
}

/*
 * C-string classification is recursive and is also part of type inference.
 * A binary expression asks whether its whole subtree is a string before
 * operator resolution asks for the left and right subtree types. Keep the
 * answers memoized for that top-level type query so a left-associated
 * expression is classified in linear time instead of rescanning each prefix.
 */
static bool nyr_expr_is_cstr(nyr_builder_impl_t *b, const expr_t *e) {
  if (!b || !e)
    return false;
  if (!b->type_query_depth)
    return nyr_expr_is_cstr_impl(b, e);
  size_t mask = NY_CSTR_QUERY_CACHE_SIZE - 1u;
  size_t slot = (((uintptr_t)e >> 4) * UINT64_C(11400714819323198485)) & mask;
  for (size_t probe = 0; probe < NY_CSTR_QUERY_CACHE_SIZE; ++probe) {
    ny_native_cstr_query_cache_entry_t *entry = &b->cstr_query_cache[slot];
    if (entry->generation != b->cstr_query_generation)
      break;
    if (entry->expr == e && entry->kind == 1)
      return entry->value;
    slot = (slot + 1) & mask;
  }
  bool value = nyr_expr_is_cstr_impl(b, e);
  slot = (((uintptr_t)e >> 4) * UINT64_C(11400714819323198485)) & mask;
  for (size_t probe = 0; probe < NY_CSTR_QUERY_CACHE_SIZE; ++probe) {
    ny_native_cstr_query_cache_entry_t *entry = &b->cstr_query_cache[slot];
    if (entry->generation != b->cstr_query_generation || entry->expr == e) {
      *entry = (ny_native_cstr_query_cache_entry_t){
          .expr = e,
          .generation = b->cstr_query_generation,
          .kind = 1,
          .value = value};
      break;
    }
    slot = (slot + 1) & mask;
  }
  return value;
}

/*
 * True when the expression produces a boolean value (a comparison, logical
 * operation, boolean literal, or boolean unary).  These lower to a raw 0/1
 * i64 in NYIR, so string formatting must route them through
 * rt_bool_to_cstr rather than the i64 formatter, which would print
 * the raw 0/1 bits.
 */
static bool nyr_expr_is_bool(nyr_builder_impl_t *b, const expr_t *e) {
  if (!e)
    return false;
  /*
   * A resolved call's declared result is the representation authority.  In
   * particular, a qualified/module call is still a bool even when its AST
   * callee has no direct identifier spelling and the fallback name lookup
   * below cannot find it.
   */
  if ((e->kind == NY_E_CALL || e->kind == NY_E_MEMCALL) &&
      e->semantic.canonical_callee_stmt &&
      e->semantic.canonical_callee_stmt->kind == NY_S_FUNC &&
      e->semantic.canonical_callee_stmt->as.fn.return_type &&
      strcmp(e->semantic.canonical_callee_stmt->as.fn.return_type, "bool") == 0)
    return true;
  if (e->kind == NY_E_CALL || e->kind == NY_E_MEMCALL) {
    const char *type = nyr_expr_type_name(b, e);
    if (type && strcmp(type, "bool") == 0)
      return true;
  }
  switch (e->kind) {
  case NY_E_LITERAL:
    return e->as.literal.kind == NY_LIT_BOOL;
  case NY_E_BINARY:
    return nyr_cmp(e->as.binary.op, &(nyr_cmp_t){0});
  case NY_E_LOGICAL:
    return true;
  case NY_E_UNARY:
    return e->as.unary.op && strcmp(e->as.unary.op, "!") == 0;
  case NY_E_TERNARY:
    return nyr_expr_is_bool(b, e->as.ternary.true_expr) &&
           nyr_expr_is_bool(b, e->as.ternary.false_expr);
  case NY_E_CALL: {
    const char *leaf = ny_native_call_leaf(e);
    if (leaf &&
        (strcmp(leaf, "is_str") == 0 || strcmp(leaf, "is_float") == 0 ||
         strcmp(leaf, "is_int") == 0 || strcmp(leaf, "is_nil") == 0 ||
         strcmp(leaf, "is_none") == 0 || strcmp(leaf, "is_ptr") == 0 ||
         strcmp(leaf, "is_list") == 0 || strcmp(leaf, "is_dict") == 0 ||
         strcmp(leaf, "is_set") == 0 || strcmp(leaf, "is_tuple") == 0 ||
         strcmp(leaf, "is_range") == 0 || strcmp(leaf, "is_bytes") == 0))
      return true;
    const stmt_t *fn = e->semantic.canonical_callee_stmt;
    if (!fn && e->semantic.canonical_callee)
      fn = nyr_find_user_function(b, e->semantic.canonical_callee);
    if (!fn && e->semantic.canonical_callee)
      fn = nyr_find_imported_function(b, e->semantic.canonical_callee);
    if (!fn && e->as.call.callee && e->as.call.callee->kind == NY_E_MEMBER &&
        e->as.call.callee->as.member.target &&
        e->as.call.callee->as.member.target->kind == NY_E_IDENT &&
        e->as.call.callee->as.member.target->as.ident.name &&
        e->as.call.callee->as.member.name) {
      const char *module = nyr_resolve_use_alias(
          b, e->as.call.callee->as.member.target->as.ident.name);
      if (module) {
        char qualified[512];
        int n = snprintf(qualified, sizeof(qualified), "%s.%s", module,
                         e->as.call.callee->as.member.name);
        if (n > 0 && (size_t)n < sizeof(qualified)) {
          fn = nyr_find_user_function(b, qualified);
          if (!fn)
            fn = nyr_find_imported_function(b, qualified);
        }
      }
    }
    if (!fn && e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT &&
        e->as.call.callee->as.ident.name)
      fn = nyr_find_user_function(b, e->as.call.callee->as.ident.name);
    if (!fn && e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT &&
        e->as.call.callee->as.ident.name) {
      const char *qualified_name = e->as.call.callee->as.ident.name;
      const char *dot = strrchr(qualified_name, '.');
      if (dot && dot != qualified_name && dot[1]) {
        size_t prefix_len = (size_t)(dot - qualified_name);
        char prefix[512];
        if (prefix_len < sizeof(prefix)) {
          memcpy(prefix, qualified_name, prefix_len);
          prefix[prefix_len] = '\0';
          const char *module = nyr_resolve_use_alias(b, prefix);
          const char *owner = module ? module : prefix;
          char resolved[512];
          int n = snprintf(resolved, sizeof(resolved), "%s.%s", owner, dot + 1);
          if (n > 0 && (size_t)n < sizeof(resolved)) {
            fn = nyr_find_user_function(b, resolved);
            if (!fn)
              fn = nyr_find_imported_function(b, resolved);
          }
        }
      }
    }
    if (!fn && leaf)
      fn = nyr_find_imported_function(b, leaf);
    return fn && fn->as.fn.return_type &&
           strcmp(fn->as.fn.return_type, "bool") == 0;
  }
  case NY_E_MEMCALL: {
    const char *method = e->as.memcall.name;
    if (method &&
        (strcmp(method, "contains") == 0 || strcmp(method, "has") == 0))
      return true;
    const stmt_t *fn = e->semantic.canonical_callee_stmt;
    if (!fn && method && e->as.memcall.target &&
        e->as.memcall.target->kind == NY_E_IDENT &&
        e->as.memcall.target->as.ident.name) {
      const char *module =
          nyr_resolve_use_alias(b, e->as.memcall.target->as.ident.name);
      if (module) {
        char qualified[512];
        int n = snprintf(qualified, sizeof(qualified), "%s.%s", module, method);
        if (n > 0 && (size_t)n < sizeof(qualified)) {
          fn = nyr_find_user_function(b, qualified);
          if (!fn)
            fn = nyr_find_imported_function(b, qualified);
        }
      }
    }
    if (!fn && method)
      fn = nyr_find_user_function(b, method);
    if (!fn && method)
      fn = nyr_find_attached_method(b, e->as.memcall.target, method);
    return fn && fn->as.fn.return_type &&
           strcmp(fn->as.fn.return_type, "bool") == 0;
  }
  case NY_E_IDENT: {
    /*
     * A binding initialized from a boolean expression keeps the raw 0/1
     * value, so follow its initializer for classification.  Do not chase
     * ident->ident chains (bounded, cycle-free).
     */
    nyr_local_t *l = nyr_find_local(b, e->as.ident.name);
    if (l)
      return l->is_bool;
    const expr_t *g = nyr_find_top_level_value(b, e->as.ident.name);
    if (g && g != e && g->kind != NY_E_IDENT)
      return nyr_expr_is_bool(b, g);
    return false;
  }
  default:
    return false;
  }
}

static const stmt_t *nyr_find_imported_function(const nyr_builder_impl_t *b,
                                                const char *name);
static const stmt_t *nyr_find_extern_decl_in_stmt(const stmt_t *s,
                                                  const char *name,
                                                  const ny_options *opt,
                                                  unsigned depth);

/*
 * True when the expression is any-typed: the value is tagged and requires
 * runtime type dispatch, so it cannot be treated as a raw i64 or pointer.
 */
static bool nyr_result_has_dynamic_ok(const char *type_name) {
  if (!type_name)
    return true;
  const char *lt = strchr(type_name, '<');
  const char *comma = lt ? strchr(lt + 1, ',') : NULL;
  if (!lt || !comma || comma <= lt + 1)
    return true;
  const char *start = lt + 1;
  while (start < comma && (*start == ' ' || *start == '\t'))
    ++start;
  const char *end = comma;
  while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
    --end;
  size_t len = (size_t)(end - start);
  return (len == 3 && memcmp(start, "any", 3) == 0) ||
         (len == 8 && memcmp(start, "sequence", 8) == 0) ||
         (len == 3 && memcmp(start, "seq", 3) == 0);
}

static const expr_t *nyr_comptime_sequence_get_target(const expr_t *value) {
  if (!value)
    return NULL;
  if (value->kind == NY_E_CALL && value->as.call.callee &&
      value->as.call.callee->kind == NY_E_MEMCALL)
    value = value->as.call.callee;
  if (value->kind == NY_E_MEMCALL && value->as.memcall.name &&
      strcmp(value->as.memcall.name, "get") == 0)
    return value->as.memcall.target;
  if (value->kind == NY_E_CALL && ny_native_call_leaf(value) &&
      strcmp(ny_native_call_leaf(value), "get") == 0 &&
      value->as.call.args.len > 0)
    return value->as.call.args.data[0].val;
  return NULL;
}

static bool nyr_expr_is_any_core(nyr_builder_impl_t *b, const expr_t *e) {
  if (!e)
    return false;
  if (e->kind == NY_E_IDENT && e->as.ident.name) {
    const nyr_local_t *local = nyr_find_local_nearest(b, e->as.ident.name);
    /*
     * A dynamic conditional can keep a numeric inference fallback while
     * every reaching store writes a tagged carrier. All consumers, including
     * comparisons and subsequent joins, must use the stored representation.
     */
    if (local && !local->scalar_class_mixed &&
        local->scalar_class == NYIR_SCALAR_I64_TAGGED)
      return true;
  }
  if (e->kind == NY_E_MEMBER) {
    const char *field_type = nyr_layout_member_type(b, e);
    if (field_type)
      return ny_native_type_name_is_any(field_type);
  }
  /*
   * A comptime block preserves the representation of its returned/tail
   * expression. In particular, an aggregate materialized through runtime
   * calls is an object handle, never a raw integer word.
   */
  if (e->kind == NY_E_COMPTIME && e->as.comptime_expr.body) {
    const stmt_t *body = e->as.comptime_expr.body;
    if (body->kind == NY_S_BLOCK) {
      for (size_t i = body->as.block.body.len; i-- > 0;) {
        const stmt_t *item = body->as.block.body.data[i];
        if (!item)
          continue;
        if (item->kind == NY_S_RETURN && item->as.ret.value) {
          /*
           * The comptime interpreter materializes `get` results as dynamic
           * values. Keep the declared any result boxed even when ordinary
           * native list.get lowering can use a raw element word.
           */
          const expr_t *value = item->as.ret.value;
          const expr_t *target = nyr_comptime_sequence_get_target(value);
          if (target && nyr_expr_is_list(b, target) &&
              !nyr_expr_is_dyn_list(b, target))
            return true;
          return nyr_expr_is_any(b, item->as.ret.value);
        }
        if (item->kind == NY_S_EXPR && item->as.expr.expr) {
          const expr_t *value = item->as.expr.expr;
          const expr_t *target = nyr_comptime_sequence_get_target(value);
          if (target && nyr_expr_is_list(b, target) &&
              !nyr_expr_is_dyn_list(b, target))
            return true;
          return nyr_expr_is_any(b, value);
        }
      }
    } else if (body->kind == NY_S_RETURN && body->as.ret.value) {
      const expr_t *value = body->as.ret.value;
      const expr_t *target = nyr_comptime_sequence_get_target(value);
      if (target && nyr_expr_is_list(b, target) &&
          !nyr_expr_is_dyn_list(b, target))
        return true;
      return nyr_expr_is_any(b, body->as.ret.value);
    } else if (body->kind == NY_S_EXPR && body->as.expr.expr) {
      const expr_t *value = body->as.expr.expr;
      const expr_t *target = nyr_comptime_sequence_get_target(value);
      if (target && nyr_expr_is_list(b, target) &&
          !nyr_expr_is_dyn_list(b, target))
        return true;
      return nyr_expr_is_any(b, body->as.expr.expr);
    }
  }
  /*
   * An element read of a whole-program proven elem-8 int list is a raw i64
   * payload, regardless of the conservative semantic annotation the checker
   * attached when the initializer was an untyped `[]`.
   */
  if (e->kind == NY_E_INDEX && e->as.index.target &&
      e->as.index.target->kind == NY_E_IDENT &&
      e->as.index.target->as.ident.name &&
      nyr_top_mut_is_append_int_list(b, e->as.index.target->as.ident.name))
    return false;
  /*
   * Dictionary accessors expose canonical tagged values even when a
   * parameterized dict's source element type is scalar. Keep that physical
   * result ABI visible to arithmetic lowering; it must decode once before
   * entering the raw integer domain.
   */
  if (e->kind == NY_E_INDEX && e->as.index.target &&
      nyr_expr_is_dict(b, e->as.index.target))
    return true;
  /*
   * Raw runtime scalar results remain raw even when their source declaration
   * is an untyped compatibility wrapper.  Let the enclosing dynamic boundary
   * box/decode them once instead of routing the raw word through any helpers.
   */
  if (e->kind == NY_E_CALL) {
    const char *symbol = ny_native_runtime_symbol_for_expr(
        e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT
            ? e->as.call.callee->as.ident.name
            : NULL,
        ny_native_call_leaf(e), e);
    /*
     * Raw syscall counters stay unboxed until a typed Result constructor
     * deliberately converts them to the language-value ABI.  Their runtime
     * declarations are intentionally broad for the interpreter, so the
     * native classifier must not infer a dynamic object from that spelling.
     */
    const ny_native_runtime_abi_decl_t *runtime_abi =
        ny_native_runtime_abi_decl(symbol);
    if (ny_native_runtime_abi_result_is_raw_word(runtime_abi))
      return false;
    /*
     * The public bitwise primitives are compatibility wrappers around raw
     * integer runtime operations.  Their broad `any` declarations describe
     * the interpreter surface, not the native result ABI.  Treating a raw
     * odd result (for example 11) as tagged and passing it through
     * rt_any_to_i64 silently changes it to 5.
     */
    const char *leaf = ny_native_call_leaf(e);
    if (leaf && (strcmp(leaf, "band") == 0 || strcmp(leaf, "bor") == 0 ||
                 strcmp(leaf, "bxor") == 0 || strcmp(leaf, "bshl") == 0 ||
                 strcmp(leaf, "bshr") == 0 || strcmp(leaf, "__and") == 0 ||
                 strcmp(leaf, "__or") == 0 || strcmp(leaf, "__xor") == 0 ||
                 strcmp(leaf, "__shl") == 0 || strcmp(leaf, "__shr") == 0))
      return false;
  }
  if (e->kind == NY_E_CALL && ny_native_call_leaf(e) &&
      (strcmp(ny_native_call_leaf(e), "unwrap") == 0 ||
       strcmp(ny_native_call_leaf(e), "__unwrap") == 0 ||
       strcmp(ny_native_call_leaf(e), "unwrap_or") == 0 ||
       strcmp(ny_native_call_leaf(e), "__unwrap_or") == 0) &&
      e->as.call.args.len >= 1) {
    const char *result_type =
        nyr_expr_type_name(b, e->as.call.args.data[0].val);
    if (result_type && strncmp(result_type, "Result<", 7) == 0 &&
        !nyr_result_has_dynamic_ok(result_type))
      return false;
  }
  /*
   * A declared `any` parameter owns a tagged ABI even when HM leaves the
   * individual identifier expression with a stale scalar representation.
   * The local fact is the representation at the function boundary; trusting
   * the expression annotation here makes callbacks receive tagged 21 but
   * execute raw arithmetic on it as if it were 10.
   */
  if (e->kind == NY_E_IDENT && e->as.ident.name) {
    const nyr_local_t *local = nyr_find_local(b, e->as.ident.name);
    if (local && local->is_any &&
        (ny_native_type_name_is_any(local->type_name) ||
         local->semantic_rep == NY_SEM_REP_TAGGED_DYNAMIC))
      return true;
    /*
     * A concrete local declaration fixes its slot ABI even when HM leaves a
     * stale TAGGED_DYNAMIC fact on a later read. Without this precedence, a
     * raw `int` accumulator is sent to rt_any_* beside a dynamic result.
     */
    if (local && local->type_name &&
        !ny_native_type_name_is_any(local->type_name))
      return false;
  }
  /*
   * Calls through lambda-valued locals/globals must follow the ABI of the
   * synthetic function that native lowering actually emits.  Source-level
   * inference can refine `f(0)` to RAW_INT even when an unannotated lambda
   * intentionally uses the tagged dynamic return ABI.  Letting that stale
   * call-site fact win makes consumers treat boxed 32 as raw 65, and breaks
   * captured closures in direct calls, iterator callbacks, and comptime.
   */
  if (e->kind == NY_E_CALL && e->as.call.callee &&
      e->as.call.callee->kind == NY_E_IDENT &&
      e->as.call.callee->as.ident.name) {
    const char *callee_name = e->as.call.callee->as.ident.name;
    const expr_t *callable = NULL;
    nyr_local_t *local = nyr_find_local(b, callee_name);
    if (local && local->callable_expr)
      callable = local->callable_expr;
    if (!callable) {
      const expr_t *global = nyr_find_top_level_value(b, callee_name);
      if (global && (global->kind == NY_E_LAMBDA || global->kind == NY_E_FN))
        callable = global;
    }
    ny_native_lambda_entry_t *lambda =
        callable
            ? ny_native_lambda_find(ny_native_lambda_cache_for(b), callable)
            : NULL;
    if (getenv("NY_DEBUG_CMP"))
      fprintf(stderr,
              "[is_any-alias] %s local=%p cexpr=%p cname=%s lambda=%p\n",
              callee_name, (void *)local, (const void *)callable,
              local && local->callable_name ? local->callable_name : "(null)",
              (void *)lambda);
    if (lambda && lambda->fn) {
      const stmt_t *fn = lambda->fn;
      if (fn->as.fn.return_type)
        return ny_native_type_name_is_any(fn->as.fn.return_type);
      if (fn->as.fn.return_semantic.resolved)
        return fn->as.fn.return_semantic.rep == NY_SEM_REP_TAGGED_DYNAMIC;
    }
    /*
     * A local alias to a NAMED function (`def fast_ref = fast_add`) has no
     * lambda entry, but the alias call still follows the target's return
     * ABI.  An untyped target whose inferred return fact is the canonical
     * tagged value must classify as dynamic; the stale call-site RAW_INT
     * stamp otherwise makes `fast_ref(7, 8) == 15` compare tagged 15 (31)
     * against the raw literal.
     */
    if (!lambda && local && local->callable_name) {
      const stmt_t *aliased = nyr_find_user_function(b, local->callable_name);
      if (getenv("NY_DEBUG_CMP") && aliased && aliased->kind == NY_S_FUNC)
        fprintf(stderr, "[alias-target] %s -> %s rtype=%s resolved=%d rep=%d\n",
                callee_name, aliased->as.fn.name ? aliased->as.fn.name : "?",
                aliased->as.fn.return_type ? aliased->as.fn.return_type
                                           : "(null)",
                (int)aliased->as.fn.return_semantic.resolved,
                (int)aliased->as.fn.return_semantic.rep);
      if (aliased && aliased->kind == NY_S_FUNC &&
          !aliased->as.fn.return_type &&
          aliased->as.fn.return_semantic.resolved &&
          aliased->as.fn.return_semantic.rep == NY_SEM_REP_TAGGED_DYNAMIC)
        return true;
    }
  }
  /*
   * An immediately-invoked lambda uses the same synthesized callback ABI as
   * a lambda stored in a local.  Preserve its tagged result even when HM
   * refined the enclosing call expression to RAW_INT.
   */
  if (e->kind == NY_E_CALL && e->as.call.callee &&
      (e->as.call.callee->kind == NY_E_LAMBDA ||
       e->as.call.callee->kind == NY_E_FN)) {
    if (!e->as.call.callee->as.lambda.return_type)
      return true;
    ny_native_lambda_entry_t *lambda =
        ny_native_lambda_find(ny_native_lambda_cache_for(b), e->as.call.callee);
    if (lambda && lambda->fn) {
      const stmt_func_t *fn = &lambda->fn->as.fn;
      if (fn->return_type)
        return ny_native_type_name_is_any(fn->return_type);
      if (fn->return_semantic.resolved)
        return fn->return_semantic.rep == NY_SEM_REP_TAGGED_DYNAMIC ||
               fn->return_semantic.rep == NY_SEM_REP_RAW_INT;
      return true;
    }
  }
  /*
   * Numeric modulo is lowered to the raw NYIR integer domain.  Even when its
   * operand is dynamic, the result is not a tagged value; advertising it as
   * `any` routes a following equality through rt_any_eq and makes raw zero
   * compare equal to the tagged integer-zero immediate.
   */
  if (e->kind == NY_E_BINARY && e->as.binary.op &&
      strcmp(e->as.binary.op, "%") == 0)
    return false;
  /*
   * Mixed scalar arithmetic (a dynamic dict/get read combined with a proven
   * raw peer) decodes the read at the arithmetic boundary and keeps a raw
   * NYIR result; pure raw arithmetic is raw as well.  Only a binary whose
   * operands are both dynamic routes through the rt_any_* helpers and
   * returns the tagged dynamic word.  Classifying a mixed or raw sum as
   * `any` made print/equality re-decode the raw result (3 rendered as 1).
   */
  if (e->kind == NY_E_BINARY && e->as.binary.op &&
      (strcmp(e->as.binary.op, "+") == 0 || strcmp(e->as.binary.op, "-") == 0 ||
       strcmp(e->as.binary.op, "*") == 0 ||
       strcmp(e->as.binary.op, "/") == 0) &&
      !nyr_expr_is_f64(b, e) && !nyr_expr_is_f32(b, e) &&
      !nyr_expr_is_cstr(b, e->as.binary.left) &&
      !nyr_expr_is_cstr(b, e->as.binary.right) &&
      !nyr_expr_is_bool(b, e->as.binary.left) &&
      !nyr_expr_is_bool(b, e->as.binary.right)) {
    bool la = nyr_expr_is_any(b, e->as.binary.left);
    bool ra = nyr_expr_is_any(b, e->as.binary.right);
    bool ld = nyr_expr_is_dict_read(b, e->as.binary.left);
    bool rd = nyr_expr_is_dict_read(b, e->as.binary.right);
    bool l_raw_peer =
        !la && (e->as.binary.left->semantic.resolved
                    ? e->as.binary.left->semantic.rep == NY_SEM_REP_RAW_INT
                    : true);
    bool r_raw_peer =
        !ra && (e->as.binary.right->semantic.resolved
                    ? e->as.binary.right->semantic.rep == NY_SEM_REP_RAW_INT
                    : true);
    if (!la && !ra && !ld && !rd)
      return false;
    if (la && r_raw_peer) {
      /*
       * A scalar `any` parameter uses rt_any_* for arithmetic with a raw
       * literal; those helpers return a tagged Ny value.  Container reads
       * are different: list/dict accessors can be unboxed before the raw
       * operation, so their mixed result remains a scalar.  Distinguish the
       * producer here so an untyped lambda such as `fn(x) { x * 2 }` is not
       * tagged twice at its dynamic return boundary.
       */
      bool container_read =
          nyr_expr_is_dict_read(b, e->as.binary.left) ||
          nyr_expr_is_raw_dynamic_read(b, e->as.binary.left) ||
          e->as.binary.left->kind == NY_E_INDEX ||
          (e->as.binary.left->kind == NY_E_MEMCALL &&
           e->as.binary.left->as.memcall.name &&
           strcmp(e->as.binary.left->as.memcall.name, "get") == 0) ||
          (e->as.binary.left->kind == NY_E_CALL &&
           ny_native_call_leaf(e->as.binary.left) &&
           strcmp(ny_native_call_leaf(e->as.binary.left), "get") == 0);
      if (!container_read)
        return true;
      return false;
    }
    if (ra && l_raw_peer) {
      bool container_read =
          nyr_expr_is_dict_read(b, e->as.binary.right) ||
          nyr_expr_is_raw_dynamic_read(b, e->as.binary.right) ||
          e->as.binary.right->kind == NY_E_INDEX ||
          (e->as.binary.right->kind == NY_E_MEMCALL &&
           e->as.binary.right->as.memcall.name &&
           strcmp(e->as.binary.right->as.memcall.name, "get") == 0) ||
          (e->as.binary.right->kind == NY_E_CALL &&
           ny_native_call_leaf(e->as.binary.right) &&
           strcmp(ny_native_call_leaf(e->as.binary.right), "get") == 0);
      if (!container_read)
        return true;
      return false;
    }
    if (ld && rd)
      return e->semantic.resolved ? e->semantic.rep == NY_SEM_REP_TAGGED_DYNAMIC
                                  : true;
    return true;
  }
  /*
   * Prefer the resolved ABI fact; retain the intrinsic spelling only when
   * semantic resolution has no declaration to consult.
   */
  if ((e->kind == NY_E_CALL || e->kind == NY_E_MEMCALL) &&
      e->semantic.canonical_callee_stmt &&
      e->semantic.canonical_callee_stmt->kind == NY_S_FUNC) {
    const stmt_t *fn = e->semantic.canonical_callee_stmt;
    /*
     * Attached methods with an explicit `any` result return a canonical
     * tagged word.  Their call-site inference can specialize the result to
     * RAW_INT, but that does not change the method's native return ABI.
     * Scalar get/pop accessors keep the raw exception below.
     */
    if (e->kind == NY_E_MEMCALL && e->as.memcall.name &&
        strcmp(e->as.memcall.name, "get") != 0 &&
        strcmp(e->as.memcall.name, "pop") != 0 &&
        ny_native_type_name_is_any(fn->as.fn.return_type))
      return true;
    if (fn->as.fn.return_semantic.resolved &&
        fn->as.fn.return_semantic.rep == NY_SEM_REP_RAW_INT)
      return false;
    /*
     * The raw memory loads return the machine word at the address.  Their
     * stdlib declaration says `any` for the VM's dynamic layer, but the
     * native ABI hands back an untagged word; classifying it as `any` makes
     * the next consumer untag it (17 loaded, printed as 8, and compared
     * unequal to the literal 17 in the thread fixtures).
     */
    if (fn->as.fn.return_type && strcmp(fn->as.fn.return_type, "any") == 0) {
      const char *leaf = ny_name_leaf(fn->as.fn.name);
      if (leaf && strcmp(leaf, "load64") == 0)
        return false;
    }
  } else if (e->kind == NY_E_CALL && e->as.call.callee &&
             e->as.call.callee->kind == NY_E_IDENT &&
             e->as.call.callee->as.ident.name &&
             strcmp(e->as.call.callee->as.ident.name, "__list_sum_int_range") ==
                 0) {
    return false;
  }
  /*
   * Module values lower through their initializer, including re-exports.
   * Use the same representation here: a conservative dynamic annotation on
   * the member must not turn its raw integer constant into a tagged value.
   */
  if (e->kind == NY_E_MEMBER && e->as.member.target &&
      e->as.member.target->kind == NY_E_IDENT &&
      !nyr_find_local(b, e->as.member.target->as.ident.name)) {
    const expr_t *value = nyr_resolve_member_expr(b, e);
    if (value && value != e)
      return nyr_expr_is_any(b, value);
  }
  if (e->kind == NY_E_MEMBER || e->kind == NY_E_MEMCALL) {
    const expr_t *receiver =
        e->kind == NY_E_MEMBER ? e->as.member.target : e->as.memcall.target;
    const char *method =
        e->kind == NY_E_MEMBER ? e->as.member.name : e->as.memcall.name;
    /*
     * Element access on bytes/range returns the raw slot payload (a byte or
     * a range step); a conservatively-`any` attached declaration must not
     * re-tag it.  List elements are tagged dynamics and keep the any ABI;
     * dict.get has its own dynamic rule further down.
     */
    if (receiver && method &&
        (strcmp(method, "get") == 0 || strcmp(method, "pop") == 0) &&
        ((nyr_expr_is_list(b, receiver) &&
          !nyr_expr_is_dyn_list(b, receiver)) ||
         nyr_expr_is_bytes(b, receiver) || nyr_expr_is_range(b, receiver)))
      return false;
    const stmt_t *fn = nyr_find_attached_method(b, receiver, method);
    if (fn && fn->as.fn.return_type)
      return ny_native_type_name_is_any(fn->as.fn.return_type);
  }
  if ((e->kind == NY_E_CALL || e->kind == NY_E_MEMCALL) &&
      e->semantic.canonical_callee_stmt &&
      e->semantic.canonical_callee_stmt->kind == NY_S_FUNC) {
    const stmt_t *fn = e->semantic.canonical_callee_stmt;
    if (fn->as.fn.return_type)
      return ny_native_type_name_is_any(fn->as.fn.return_type);
    if (fn->as.fn.return_semantic.resolved)
      return fn->as.fn.return_semantic.rep == NY_SEM_REP_TAGGED_DYNAMIC;
    return true;
  }
  /*
   * A resolved floating representation is a raw IEEE value in NYIR.  Do not
   * let a conservative dynamic annotation override it: treating an f64 as
   * `any` routes it through integer unboxing before a later comparison.
   */
  if (nyr_expr_is_f64(b, e) || nyr_expr_is_f32(b, e))
    return false;
  /*
   * Dictionary indexing returns a dynamic slot value.  Keep the tag through
   * callers such as print/equality; treating it as a raw scalar causes a
   * second tag operation at the consumer boundary.
   */
  if (e->kind == NY_E_INDEX && e->as.index.target &&
      nyr_expr_is_dict(b, e->as.index.target))
    return true;
  if (e->kind == NY_E_INDEX && e->as.index.target &&
      !nyr_expr_is_any(b, e->as.index.target) &&
      (nyr_expr_is_list(b, e->as.index.target) ||
       nyr_expr_is_range(b, e->as.index.target) ||
       nyr_expr_is_bytes(b, e->as.index.target)))
    return false;
  /*
   * Indexing an `any`-classified call result crosses the representation-aware
   * accessor (no proven stride), so the read yields the canonical dynamic
   * value.  Consumers must decode rather than print/compare the raw word:
   * keeping this any here lets println(f(9)[0]) route through rt_any_to_cstr
   * instead of rendering the tagged encoding 19 as an integer.
   */
  if (e->kind == NY_E_INDEX && e->as.index.target &&
      (e->as.index.target->kind == NY_E_CALL ||
       e->as.index.target->kind == NY_E_MEMCALL) &&
      nyr_expr_is_any(b, e->as.index.target))
    return true;
  /*
   * A concrete function ABI is more precise than a propagated expression
   * fallback.  This is especially important for `extern "c"` calls in
   * -no-std objects: lowering an explicitly-i64 result as tagged `any` both
   * corrupts arithmetic semantics and introduces an unwanted runtime link.
   */
  if (e->kind == NY_E_CALL && e->as.call.callee &&
      e->as.call.callee->kind == NY_E_IDENT &&
      e->as.call.callee->as.ident.name) {
    const char *cname = e->as.call.callee->as.ident.name;
    /*
     * Runtime result representation is declared independently from source
     * type inference: tagged values stay dynamic and raw-machine-word results
     * (lengths, counts, opaque handles) stay unboxed.
     */
    {
      const char *sym = ny_native_runtime_symbol_for_expr(cname, NULL, e);
      const ny_native_runtime_abi_decl_t *runtime_abi =
          ny_native_runtime_abi_decl(sym);
      if (ny_native_runtime_abi_result_is_tagged_value(runtime_abi))
        return true;
      if (ny_native_runtime_abi_result_is_raw_word(runtime_abi))
        return false;
    }
    /*
     * Native scalar container stores return their payload in the raw NYIR
     * ABI.  The stdlib declaration is intentionally dynamic for the VM, but
     * letting that annotation win here causes the native caller to tag the
     * raw return a second time (and breaks `__store_item(...) == value`).
     */
    const char *store_leaf =
        e->semantic.canonical_callee
            ? ny_name_leaf(e->semantic.canonical_callee)
            : ny_native_call_leaf(e);
    if (store_leaf && (strcmp(store_leaf, "__store_item") == 0 ||
                       strcmp(store_leaf, "__store_item_fast") == 0))
      return false;
    const stmt_t *fn = nyr_find_user_function(b, cname);
    if (!fn)
      fn = nyr_find_imported_function(b, cname);
    if (fn && fn->as.fn.return_type) {
      /*
       * Same raw-load bridge as the canonical-callee check above: the
       * stdlib `any` annotation must not classify the native machine-word
       * result of load64 as a tagged dynamic.
       */
      if (strcmp(fn->as.fn.return_type, "any") == 0) {
        const char *leaf = ny_name_leaf(fn->as.fn.name);
        if (leaf && strcmp(leaf, "load64") == 0)
          return false;
      }
      return strcmp(fn->as.fn.return_type, "any") == 0;
    }
    if (fn && fn->as.fn.return_semantic.resolved) {
      /*
       * A proven raw-integer return stays raw; but an UNTYPED declaration
       * whose inference landed on any other representation (object, string,
       * tagged dynamic) still compiles to the canonical dynamic return ABI.
       * Treating that call as non-any made inline consumers print list
       * handles as decimal addresses and index reads re-tag slot payloads
       * (println(f(9)) printed a pointer, f(9)[0] printed the tagged word).
       */
      if (fn->as.fn.return_semantic.rep == NY_SEM_REP_RAW_INT)
        return false;
      return !fn->as.fn.return_type ||
             fn->as.fn.return_semantic.rep == NY_SEM_REP_TAGGED_DYNAMIC;
    }
    if (fn && !fn->as.fn.return_type)
      return true;
    nyr_local_t *local = nyr_find_local(b, cname);
    if (local)
      return true;
    /*
     * Extern signatures are already indexed before lowering starts.  Avoid a
     * repeated recursive AST scan here; this predicate is queried for nearly
     * every call argument in large stdlib-expanded programs.
     */
  }
  if (e->kind == NY_E_IDENT) {
    nyr_local_t *local = nyr_find_local(b, e->as.ident.name);
    bool top_entry = !b->current_fn_name ||
                     strcmp(b->current_fn_name, "rt_main") == 0 ||
                     strcmp(b->current_fn_name, "_ny_top_entry") == 0;
    if (top_entry && e->as.ident.name) {
      const stmt_t *decl = nyr_find_top_level_var_stmt(b, e->as.ident.name);
      if (decl && decl->kind == NY_S_VAR) {
        for (size_t i = 0; i < decl->as.var.names.len; ++i) {
          const char *name = decl->as.var.names.data[i];
          if (!name || strcmp(name, e->as.ident.name) != 0)
            continue;
          if (i < decl->as.var.types.len && decl->as.var.types.data[i])
            return ny_native_type_name_is_any(decl->as.var.types.data[i]);
          if (i < decl->as.var.exprs.len && decl->as.var.exprs.data[i] &&
              decl->as.var.exprs.data[i]->kind == NY_E_COMPTIME)
            return nyr_expr_is_any(b, decl->as.var.exprs.data[i]);
        }
      }
    }
    if (local) {
      if (local->is_bigint)
        return false;
      /*
       * An explicitly dynamic local (function parameter or a read from an
       * `any` receiver) must retain the tagged ABI even when semantic
       * inference conservatively records RAW_INT for its storage slot.  The
       * local's is_any bit is the provenance fact; letting RAW_INT win here
       * turns dynamic strings into decimal pointer text.
       */
      if (local->is_any)
        return true;
      if (local->is_list || local->is_dict || local->is_bytes)
        return true;
      /*
       * A local may have started as an untyped literal and still carry the
       * conservative `is_any` bit.  Once semantic analysis proved its slot
       * is a raw integer, preserve scalar arithmetic instead of routing it
       * through the tagged dynamic helper.
       */
      if (local->semantic_rep == NY_SEM_REP_RAW_INT)
        return false;
      return local->is_any;
    }
    /*
     * Module scalar storage contains raw payloads, even when a compound
     * assignment from a generic accessor widened the semantic annotation.
     * Use the defining source to avoid borrowing a same-named module's fact.
     */
    if (nyr_top_mut_is_append_int_list(b, e->as.ident.name))
      return false;
    const expr_t *init = e->tok.filename
                             ? nyr_find_top_level_value_in_source(
                                   b, e->as.ident.name, e->tok.filename)
                             : NULL;
    if (!init)
      init = nyr_find_top_level_value(b, e->as.ident.name);
    /*
     * A top-level def bound to an element read of a proven elem-8 int list
     * holds the raw i64 payload.  The checker annotated the identifier as
     * dynamic while the list's element type was still undetermined.
     */
    if (init && init != e && init->kind == NY_E_INDEX &&
        init->as.index.target && init->as.index.target->kind == NY_E_IDENT &&
        init->as.index.target->as.ident.name &&
        nyr_top_mut_is_append_int_list(b,
                                       init->as.index.target->as.ident.name)) {
      return false;
    }
    if (init && init->kind == NY_E_LITERAL &&
        init->as.literal.kind == NY_LIT_INT && init->tok.kind != NY_T_NIL)
      return false;
    if (init && init != e && init->kind == NY_E_COMPTIME)
      return nyr_expr_is_any(b, init);
    /*
     * A global initialized from an untyped user function holds that
     * function's tagged dynamic return in its slot.  Inference may still
     * refine the use-site identifier to a raw integer, but the storage
     * representation is the initializer's: consult the callee's return
     * fact before the refined semantic can misclassify the slot (raw
     * consumers then decoded the tagged word, printing 99 for 49).
     * Runtime-bridge calls (no user declaration) keep the refined fact.
     */
    if (init && init->kind == NY_E_CALL && init->as.call.callee &&
        init->as.call.callee->kind == NY_E_IDENT &&
        init->as.call.callee->as.ident.name) {
      const stmt_t *gfn =
          nyr_find_user_function(b, init->as.call.callee->as.ident.name);
      if (!gfn)
        gfn =
            nyr_find_imported_function(b, init->as.call.callee->as.ident.name);
      if (gfn && !gfn->as.fn.return_type &&
          gfn->as.fn.return_semantic.resolved &&
          gfn->as.fn.return_semantic.rep == NY_SEM_REP_TAGGED_DYNAMIC)
        return true;
    }
  }
  /*
   * Length is always a raw scalar count.  In particular, a dynamically
   * constructed string can still have a conservative tagged receiver
   * annotation, but `value.len()` itself must never inherit that ABI.  If it
   * does, later integer arithmetic unboxes the already-raw count a second
   * time (e.g. 693 becomes 346), truncating loops over long strings.
   */
  if (e->kind == NY_E_MEMCALL && e->as.memcall.name &&
      strcmp(e->as.memcall.name, "len") == 0 && e->as.memcall.args.len == 0)
    return false;
  /*
   * Mutating container methods return the receiver, not a scalar payload.
   * The generic `any` methods are lowered to the same in-place runtime
   * operation as their concrete dict/list/bytes overloads, so preserve the
   * receiver representation at an any-return boundary.  Otherwise a raw
   * container handle is fed to rt_tag and becomes an integer-looking value
   * that loses its container identity.
   */
  if (e->kind == NY_E_MEMCALL && e->as.memcall.name &&
      (strcmp(e->as.memcall.name, "set") == 0 ||
       strcmp(e->as.memcall.name, "put") == 0) &&
      e->as.memcall.target &&
      (nyr_expr_is_any(b, e->as.memcall.target) ||
       nyr_expr_is_dict(b, e->as.memcall.target) ||
       nyr_expr_is_list(b, e->as.memcall.target) ||
       nyr_expr_is_bytes(b, e->as.memcall.target) ||
       nyr_expr_is_set(b, e->as.memcall.target)))
    return true;
  /*
   * Semantic analysis may conservatively label a member call `any` even
   * when its receiver has already been proven to be a typed buffer.  The
   * native list/bytes/range accessors return raw element values, so this
   * representation check must precede the generic semantic fallback.
   */
  if (e->kind == NY_E_CALL && e->as.call.callee &&
      e->as.call.callee->kind == NY_E_MEMBER &&
      e->as.call.callee->as.member.name &&
      (strcmp(e->as.call.callee->as.member.name, "get") == 0 ||
       strcmp(e->as.call.callee->as.member.name, "pop") == 0) &&
      e->as.call.callee->as.member.target &&
      (nyr_expr_is_list(b, e->as.call.callee->as.member.target) ||
       nyr_expr_is_dyn_list(b, e->as.call.callee->as.member.target) ||
       nyr_expr_is_any(b, e->as.call.callee->as.member.target) ||
       nyr_expr_is_bytes(b, e->as.call.callee->as.member.target) ||
       nyr_expr_is_range(b, e->as.call.callee->as.member.target)))
    return false;
  /*
   * Element access on bytes/range yields a raw slot value even when the
   * receiver itself is conservatively tagged (`def b = bytes(3)` records the
   * bytes ABI from its initializer while inference also marks the slot
   * dynamic).  The concrete-container fact wins, otherwise every consumer
   * re-decodes an already-raw byte (`b.get(0)` prints 32).  List/dict
   * elements are tagged dynamics and keep the any ABI.
   */
  if (e->kind == NY_E_MEMCALL && e->as.memcall.name &&
      (strcmp(e->as.memcall.name, "get") == 0 ||
       strcmp(e->as.memcall.name, "pop") == 0) &&
      e->as.memcall.target &&
      (nyr_expr_is_bytes(b, e->as.memcall.target) ||
       nyr_expr_is_range(b, e->as.memcall.target)))
    return false;
  if (e->kind == NY_E_MEMCALL && e->as.memcall.name &&
      (strcmp(e->as.memcall.name, "get") == 0 ||
       strcmp(e->as.memcall.name, "pop") == 0) &&
      e->as.memcall.target &&
      (nyr_expr_is_dyn_list(b, e->as.memcall.target) ||
       nyr_expr_is_dict(b, e->as.memcall.target) ||
       nyr_expr_is_any(b, e->as.memcall.target)))
    return true;
  if (e->kind == NY_E_BINARY && e->as.binary.op &&
      strcmp(e->as.binary.op, "**") == 0) {
    int64_t folded = 0;
    return !nyr_fold_const_pow(e->as.binary.left, e->as.binary.right, &folded);
  }
  if (e->kind == NY_E_BINARY && !nyr_expr_is_any(b, e->as.binary.left) &&
      !nyr_expr_is_any(b, e->as.binary.right))
    return false;
  /*
   * A namespace call's DECLARED return type outranks the inferred semantic
   * rep: syntax.expand_macro declares `any`, but HM refines the use site to
   * the macro body's int, so the tagged dynamic result crossed into raw
   * locals (println 85 instead of 42, type() "unknown") and comparisons
   * against literals failed.
   */
  if (e->kind == NY_E_MEMCALL && e->as.memcall.target &&
      e->as.memcall.target->kind == NY_E_IDENT && e->as.memcall.name) {
    const char *ns_alias = e->as.memcall.target->as.ident.name;
    const stmt_t *ns_fn = e->semantic.canonical_callee_stmt;
    const char *ns_module =
        ns_alias ? nyr_resolve_use_alias(b, ns_alias) : NULL;
    if (!ns_module && ns_alias && !nyr_find_local(b, ns_alias)) {
      nyr_builder_impl_t probe = *b;
      probe.cache_owner = b->cache_owner ? b->cache_owner : b;
      probe.source_file = NULL;
      probe.locals = NULL;
      probe.local_count = probe.local_cap = 0;
      ns_module = nyr_resolve_use_alias(&probe, ns_alias);
    }
    if (!ns_fn && ns_module) {
      char qualified[512];
      int qn = snprintf(qualified, sizeof(qualified), "%s.%s", ns_module,
                        e->as.memcall.name);
      if (qn > 0 && (size_t)qn < sizeof(qualified)) {
        ns_fn = nyr_find_user_function(b, qualified);
        if (!ns_fn)
          ns_fn = nyr_find_imported_function(b, qualified);
      }
    }
    if (ns_fn && ns_fn->kind == NY_S_FUNC)
      return !ns_fn->as.fn.return_type ||
             strcmp(ns_fn->as.fn.return_type, "any") == 0;
  }
  if (e->kind == NY_E_INDEX) {
    const expr_t *literal_target =
        nyr_resolve_list_literal(b, e->as.index.target, 0);
    if (literal_target && literal_target->kind == NY_E_LIST &&
        !nyr_expr_is_dyn_list(b, literal_target))
      return false;
    /*
     * Semantic inference may stamp the index node with the carrier's raw
     * representation even when its selected element is itself a container.
     * Recover the element type before that broad fallback so chained reads
     * preserve a nested list as a dynamic value but keep scalar leaves raw.
     */
    if (e->as.index.target && e->as.index.target->kind == NY_E_IDENT &&
        e->as.index.target->as.ident.name) {
      const nyr_local_t *sequence =
          nyr_find_local(b, e->as.index.target->as.ident.name);
      if (nyr_local_is_dynamic_sequence(sequence))
        return true;
    }
    const char *type = nyr_expr_type_name(b, e);
    if (type &&
        (strcmp(type, "any") == 0 || strncmp(type, "list<", 5) == 0 ||
         strncmp(type, "dict<", 5) == 0 || strncmp(type, "set<", 4) == 0 ||
         strcmp(type, "tuple") == 0 || strcmp(type, "list") == 0))
      return true;
    return false;
  }
  /*
   * A top-level mutable scalar keeps the physical representation established
   * by its declaration across later assignments.  Semantic inference can
   * widen the assignment expression to a tagged value after seeing a generic
   * accessor, but an integer-initialized global is still a raw NYIR slot; use
   * the storage declaration before the use-site fact to avoid decoding its
   * raw payload a second time.
   */
  if (e->kind == NY_E_IDENT && e->as.ident.name &&
      !nyr_find_local(b, e->as.ident.name)) {
    /*
     * An explicit top-level scalar type is the storage ABI for every read of
     * that global.  Initializers such as `int flags = bor(...)` can retain a
     * conservative `any` expression fact because the stdlib primitive is
     * broadly declared; reusing that fact at the read boundary would decode
     * the raw global a second time (raw 11 becomes tagged 5).
     */
    const stmt_t *decl =
        e->tok.filename ? nyr_find_top_level_var_in_source_in_prog(
                              b, b->prog, e->as.ident.name, e->tok.filename)
                        : NULL;
    if (!decl)
      decl = nyr_find_top_level_var_stmt(b, e->as.ident.name);
    if (decl && decl->kind == NY_S_VAR) {
      for (size_t i = 0; i < decl->as.var.names.len; ++i) {
        if (!decl->as.var.names.data[i] ||
            strcmp(decl->as.var.names.data[i], e->as.ident.name) != 0 ||
            i >= decl->as.var.types.len || !decl->as.var.types.data[i])
          continue;
        return ny_native_type_name_is_any(decl->as.var.types.data[i]);
      }
      /*
       * Mutable globals keep the initializer's scalar ABI. A raw integer
       * initializer is stored raw by the declaration lowerer; treating later
       * reads as tagged made arithmetic and formatting decode that raw word
       * as a dynamic value.
       */
      if (decl->as.var.is_mut) {
        const expr_t *init = nyr_find_top_level_value(b, e->as.ident.name);
        return !init || !nyr_expr_is_raw_int(init);
      }
    }
    if (e->semantic.resolved && e->semantic.rep == NY_SEM_REP_TAGGED_DYNAMIC)
      return true;
    const expr_t *init = e->tok.filename
                             ? nyr_find_top_level_value_in_source(
                                   b, e->as.ident.name, e->tok.filename)
                             : nyr_find_top_level_value(b, e->as.ident.name);
    if (!init)
      init = nyr_find_top_level_value(b, e->as.ident.name);
    if (init && init->kind == NY_E_LITERAL &&
        init->as.literal.kind == NY_LIT_INT && init->tok.kind != NY_T_NIL)
      return false;
  }
  if (e->semantic.resolved && e->semantic.rep == NY_SEM_REP_TAGGED_DYNAMIC) {
    /*
     * A call node can retain the use-site's conservative dynamic fact even
     * when its resolved callee has a concrete floating return ABI.  The
     * producer declaration is authoritative here: treating that raw f64
     * result as an already-boxed dynamic word makes the next typed call
     * decode IEEE bits as an integer.
     */
    if (e->kind == NY_E_CALL || e->kind == NY_E_MEMCALL)
      return !nyr_expr_is_f64(b, e) && !nyr_expr_is_f32(b, e);
    return true;
  }
  switch (e->kind) {
  case NY_E_IDENT: {
    nyr_local_t *l = nyr_find_local(b, e->as.ident.name);
    if (l) {
      if (l->semantic_rep == NY_SEM_REP_RAW_INT)
        return false;
      return l->semantic_rep == NY_SEM_REP_TAGGED_DYNAMIC || l->is_any;
    }
    const expr_t *g = nyr_find_top_level_value(b, e->as.ident.name);
    if (g && g != e) {
      if (ny_expr_is_nil_literal(g))
        return true;
      return nyr_expr_is_any(b, g);
    }
    /*
     * Imported stdlib module state is compiled from its function body, while
     * the defining module variable may not be present in the caller's
     * flattened program tree. Private state names are the only unresolved
     * identifiers admitted here, and only the dynamic-container method path
     * consumes this classification.
     */
    return e->as.ident.name && e->as.ident.name[0] == '_';
  }
  case NY_E_BINARY:
    /*
     * Bitwise operators are defined in the raw integer domain.  A dynamic
     * operand is decoded at the operator boundary, but the result itself is
     * raw; propagating `any` to the enclosing store would decode that result
     * a second time (65 became 32 in byte masks).
     */
    if (nyr_is_bitwise_operator(e->as.binary.op))
      return false;
    return e->as.binary.op && (nyr_expr_is_any(b, e->as.binary.left) ||
                               nyr_expr_is_any(b, e->as.binary.right));
  case NY_E_INDEX: {
    if (e->as.index.target && e->as.index.target->kind == NY_E_IDENT &&
        e->as.index.target->as.ident.name) {
      const nyr_local_t *target =
          nyr_find_local(b, e->as.index.target->as.ident.name);
      /*
       * An `any`/dynamic-sequence receiver crosses the representation-aware
       * accessor and yields a canonical tagged value even when inference has
       * narrowed the element type to a scalar at one call site.
       */
      if (target && ((target->is_any && !target->is_list && !target->is_dict) ||
                     target->is_dyn_list))
        return true;
    }
    /*
     * Indexing a typed sequence preserves the element's declared ABI.  A
     * container/any element remains dynamic, while scalar elements stay raw;
     * this distinguishes `list<list<int>>[1][0]` correctly.
     */
    const char *type = nyr_expr_type_name(b, e);
    return type &&
           (strcmp(type, "any") == 0 || strncmp(type, "list<", 5) == 0 ||
            strncmp(type, "dict<", 5) == 0 || strncmp(type, "set<", 4) == 0 ||
            strcmp(type, "tuple") == 0 || strcmp(type, "list") == 0);
  }
  case NY_E_UNARY:
    return nyr_expr_is_any(b, e->as.unary.right);
  case NY_E_MEMBER: {
    const expr_t *v = nyr_resolve_member_expr(b, e);
    return v && v != e && nyr_expr_is_any(b, v);
  }
  case NY_E_CALL:
    /*
     * An explicit integer conversion produces a raw scalar even when its
     * argument has a dynamic ABI.
     */
    if (nyr_expr_is_raw_integer_conversion(b, e))
      return false;
    /*
     * `unwrap`/`unwrap_or` return the payload in native representation.  A
     * typed Result<int, E> therefore produces raw i64, not a dynamic word;
     * keep comparisons and prints on the scalar path.  Dynamic Result
     * payloads retain the ordinary any ABI.
     */
    if (ny_native_call_leaf(e) &&
        (strcmp(ny_native_call_leaf(e), "unwrap") == 0 ||
         strcmp(ny_native_call_leaf(e), "__unwrap") == 0 ||
         strcmp(ny_native_call_leaf(e), "unwrap_or") == 0 ||
         strcmp(ny_native_call_leaf(e), "__unwrap_or") == 0)) {
      const char *result_type = nyr_expr_type_name(b, e);
      if (result_type && !ny_native_type_name_is_any(result_type))
        return false;
    }
    /*
     * The native load64 intrinsics return raw machine i64 values. The
     * language-level `any` return annotations describe the tagged VM path,
     * not the shared NYIR representation.
     */
    if (ny_native_call_leaf(e) &&
        (ny_native_leaf_kind(ny_native_call_leaf(e)) == NY_NATIVE_LEAF_LOAD8 ||
         ny_native_leaf_kind(ny_native_call_leaf(e)) == NY_NATIVE_LEAF_LOAD16 ||
         ny_native_leaf_kind(ny_native_call_leaf(e)) == NY_NATIVE_LEAF_STORE8 ||
         ny_native_leaf_kind(ny_native_call_leaf(e)) ==
             NY_NATIVE_LEAF_STORE16 ||
         ny_native_leaf_kind(ny_native_call_leaf(e)) == NY_NATIVE_LEAF_LOAD64 ||
         ny_native_leaf_kind(ny_native_call_leaf(e)) ==
             NY_NATIVE_LEAF_LOAD64_IDX ||
         ny_native_leaf_kind(ny_native_call_leaf(e)) ==
             NY_NATIVE_LEAF_STORE64 ||
         ny_native_leaf_kind(ny_native_call_leaf(e)) ==
             NY_NATIVE_LEAF_STORE64_H ||
         ny_native_leaf_kind(ny_native_call_leaf(e)) ==
             NY_NATIVE_LEAF_STORE64_IDX))
      return false;
    if (e->as.call.callee && e->as.call.callee->kind == NY_E_MEMBER) {
      if (e->as.call.callee->as.member.name &&
          (strcmp(e->as.call.callee->as.member.name, "get") == 0 ||
           strcmp(e->as.call.callee->as.member.name, "pop") == 0))
        return !(e->as.call.callee->as.member.target &&
                 (nyr_expr_is_list(b, e->as.call.callee->as.member.target) ||
                  nyr_expr_is_bytes(b, e->as.call.callee->as.member.target) ||
                  nyr_expr_is_range(b, e->as.call.callee->as.member.target)));
    }
    if (e->as.call.callee && e->as.call.callee->kind == NY_E_MEMBER &&
        e->as.call.callee->as.member.name &&
        e->as.call.callee->as.member.target &&
        e->as.call.callee->as.member.target->kind == NY_E_IDENT) {
      /*
       * A module-qualified call (`syntax.expand_macro(...)`) carries a
       * MEMBER callee, so the ident branch below never fires and the call
       * fell through to "not any": its tagged dynamic return then flowed
       * into raw locals (println printed the tagged word 85 instead of 42
       * and type() answered "unknown").  Resolve the alias to the
       * module-qualified function and classify from its declared return.
       */
      const stmt_t *qfn = e->semantic.canonical_callee_stmt;
      if (!qfn) {
        const char *alias = e->as.call.callee->as.member.target->as.ident.name;
        const char *module = alias ? nyr_resolve_use_alias(b, alias) : NULL;
        if (module) {
          char qualified[512];
          int qn = snprintf(qualified, sizeof(qualified), "%s.%s", module,
                            e->as.call.callee->as.member.name);
          if (qn > 0 && (size_t)qn < sizeof(qualified)) {
            qfn = nyr_find_user_function(b, qualified);
            if (!qfn)
              qfn = nyr_find_imported_function(b, qualified);
          }
        }
      }
      if (qfn && qfn->kind == NY_S_FUNC)
        return !qfn->as.fn.return_type ||
               strcmp(qfn->as.fn.return_type, "any") == 0;
    }
    if (e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT &&
        e->as.call.callee->as.ident.name) {
      const char *cname = e->as.call.callee->as.ident.name;
      const stmt_t *fn = nyr_find_user_function(b, cname);
      if (!fn)
        fn = nyr_find_imported_function(b, cname);
      if (fn)
        return !fn->as.fn.return_type ||
               strcmp(fn->as.fn.return_type, "any") == 0;
      nyr_local_t *local = nyr_find_local(b, cname);
      if (local)
        return true;
    }
    break;
  case NY_E_MEMCALL:
    if (e->as.memcall.name && (strcmp(e->as.memcall.name, "get") == 0 ||
                               strcmp(e->as.memcall.name, "pop") == 0 ||
                               strcmp(e->as.memcall.name, "slice") == 0))
      return !(e->as.memcall.target &&
               (nyr_expr_is_list(b, e->as.memcall.target) ||
                nyr_expr_is_bytes(b, e->as.memcall.target) ||
                nyr_expr_is_range(b, e->as.memcall.target)));
    /*
     * `module_alias.fn(...)` is a namespace call: resolve the alias exactly
     * like the lowerer's canonicalization (fresh probe when the first
     * lookup is defeated by bound locals) and classify from the target
     * function's declared return.  Without this, an `any`-returning
     * qualified helper (syntax.expand_macro) classified as raw and its
     * tagged dynamic result reached println/type as a decimal word.
     */
    if (e->as.memcall.target && e->as.memcall.name &&
        e->as.memcall.target->kind == NY_E_IDENT) {
      const char *alias = e->as.memcall.target->as.ident.name;
      const stmt_t *qfn = e->semantic.canonical_callee_stmt;
      const char *module = alias ? nyr_resolve_use_alias(b, alias) : NULL;
      if (!module && alias && !nyr_find_local(b, alias)) {
        nyr_builder_impl_t probe = *b;
        probe.cache_owner = b->cache_owner ? b->cache_owner : b;
        probe.source_file = NULL;
        probe.locals = NULL;
        probe.local_count = probe.local_cap = 0;
        module = nyr_resolve_use_alias(&probe, alias);
      }
      if (!qfn && module) {
        char qualified[512];
        int qn = snprintf(qualified, sizeof(qualified), "%s.%s", module,
                          e->as.memcall.name);
        if (qn > 0 && (size_t)qn < sizeof(qualified)) {
          qfn = nyr_find_user_function(b, qualified);
          if (!qfn)
            qfn = nyr_find_imported_function(b, qualified);
        }
      }
      if (qfn && qfn->kind == NY_S_FUNC)
        return !qfn->as.fn.return_type ||
               strcmp(qfn->as.fn.return_type, "any") == 0;
    }
    break;
  default:
    break;
  }
  return false;
}

static bool nyr_expr_is_any(nyr_builder_impl_t *b, const expr_t *e) {
  if (!b || !e)
    return false;
  ny_native_scalar_probe_enter(b);
  bool value = false;
  bool cacheable = e->kind != NY_E_IDENT;
  if (cacheable && ny_native_scalar_probe_get(b, e, 16, 0, &value)) {
    ny_native_scalar_probe_leave(b);
    return value;
  }
  value = nyr_expr_is_any_core(b, e);
  if (cacheable)
    ny_native_scalar_probe_put(b, e, 16, 0, value);
  ny_native_scalar_probe_leave(b);
  return value;
}

/*
 * Native dictionary/list accessors return the payload stored in the native
 * slot.  That payload is raw even when the source expression is `any`, unlike
 * an any-valued function parameter which enters the ABI tagged.  Keep this
 * provenance fact local to lowering so arithmetic does not guess from the
 * low bit of an ambiguous integer (raw 1 and tagged integer 0 are identical
 * to the old runtime heuristic).
 */
static bool nyr_expr_is_raw_dynamic_read(nyr_builder_impl_t *b,
                                         const expr_t *e) {
  if (!e)
    return false;
  if (e->kind == NY_E_CALL && ny_native_call_leaf(e)) {
    const char *leaf = ny_native_call_leaf(e);
    ny_native_leaf_kind_t kind = ny_native_leaf_kind(leaf);
    if (strcmp(leaf, "__load64_tagged_idx") == 0)
      return false;
    if (kind == NY_NATIVE_LEAF_LOAD8 || kind == NY_NATIVE_LEAF_LOAD16 ||
        kind == NY_NATIVE_LEAF_LOAD64 || kind == NY_NATIVE_LEAF_LOAD64_IDX)
      return true;
    /*
     * __any_to_i64 is an explicit representation boundary: its result is a
     * raw machine integer even when the input expression is dynamic.  Keep
     * that provenance through list append/store lowering so odd values are
     * not interpreted as tagged integers a second time.
     */
    if (leaf &&
        (strcmp(leaf, "__any_to_i64") == 0 ||
         strcmp(leaf, "rt_any_to_i64") == 0 || strcmp(leaf, "any_to_i64") == 0))
      return true;
  }
  /*
   * Indexing an `any` local is lowered through rt_tbuf_get.  Like a
   * dynamic get/pop, that accessor returns the native slot payload (raw i64
   * for scalar typed buffers), not the tagged any-call ABI.  Dictionary
   * subscript reads are the exception: they keep the canonical tagged slot
   * word exactly like .get, so arithmetic must decode them at this
   * boundary instead of treating the word as a raw payload.
   */
  if (e->kind == NY_E_INDEX) {
    if (e->as.index.target) {
      if (nyr_expr_is_dict(b, e->as.index.target))
        return false;
      if (e->as.index.target->kind == NY_E_IDENT &&
          e->as.index.target->as.ident.name) {
        const nyr_local_t *target_local =
            nyr_find_local(b, e->as.index.target->as.ident.name);
        if (target_local && (target_local->is_any || target_local->is_dyn_list))
          return false;
        if (nyr_local_is_dynamic_sequence(target_local))
          return false;
        if (target_local && target_local->is_dict)
          return false;
        if (target_local && target_local->is_list)
          return true;
      }
      /*
       * Dynamic sequence receivers return canonical any values.  Only a
       * proven scalar list local has the raw-slot contract used by the
       * arithmetic fast path.
       */
      if (nyr_expr_is_any(b, e->as.index.target) ||
          nyr_expr_is_dyn_list(b, e->as.index.target))
        return false;
    }
    return false;
  }
  /*
   * Preserve raw-slot provenance through arithmetic trees.  A compound
   * accumulator commonly arrives as `total += 3 - list[i]`; the outer `+`
   * must not dispatch to the boxed any helper merely because its RHS is an
   * expression whose leaf is an indexed native buffer value.
   */
  if (e->kind == NY_E_BINARY) {
    /*
     * A compound expression is a raw dynamic read only when one of its
     * leaves is an actual raw load/index boundary.  The expression's final
     * RAW_INT semantic fact alone is not enough: ordinary arithmetic such as
     * `SMALL_MIN - 1` is raw typed arithmetic, not a value that needs a
     * second unboxing at an `any` boundary.
     */
    return nyr_expr_is_raw_dynamic_read(b, e->as.binary.left) ||
           nyr_expr_is_raw_dynamic_read(b, e->as.binary.right);
  }
  const expr_t *target = NULL;
  const char *method = NULL;
  if (e->kind == NY_E_MEMCALL) {
    target = e->as.memcall.target;
    method = e->as.memcall.name;
  } else if (e->kind == NY_E_CALL && e->as.call.callee &&
             e->as.call.callee->kind == NY_E_MEMBER) {
    target = e->as.call.callee->as.member.target;
    method = e->as.call.callee->as.member.name;
  } else if (e->kind == NY_E_CALL && e->as.call.callee &&
             e->as.call.callee->kind == NY_E_IDENT &&
             e->as.call.callee->as.ident.name &&
             strcmp(e->as.call.callee->as.ident.name, "get") == 0 &&
             e->as.call.args.len >= 2) {
    target = e->as.call.args.data[0].val;
    method = "get";
  }
  if (!target || !method ||
      (strcmp(method, "get") != 0 && strcmp(method, "pop") != 0))
    return false;
  if (nyr_expr_is_bytes(b, target) || nyr_expr_is_range(b, target))
    return false;
  if (nyr_expr_is_list(b, target) && !nyr_expr_is_any(b, target) &&
      !nyr_expr_is_dyn_list(b, target) && !nyr_expr_is_dict(b, target))
    return true;
  if (target->kind == NY_E_IDENT && target->as.ident.name) {
    const nyr_local_t *local = nyr_find_local(b, target->as.ident.name);
    if (local && local->is_list && !local->is_any && !local->is_dyn_list &&
        !local->is_dict)
      return true;
  }
  return false;
}

/*
 * A descriptor-backed dynamic sequence accessor returns the canonical Ny
 * value (value/length/tag), even when HM attached a numeric fallback type to
 * the expression.  Keep this representation fact separate from semantic
 * type inference: callers must not reinterpret the returned word as an
 * unboxed float and box it a second time at the next dynamic boundary.
 */
static bool nyr_expr_is_canonical_dynamic_value(nyr_builder_impl_t *b,
                                                const expr_t *e) {
  if (!e)
    return false;
  /*
   * A non-folded integer power is lowered through core.pow, whose
   * result may be a boxed float or bigint even if HM kept an integer fact
   * on the operator node.  The callee has already produced the dynamic ABI;
   * an any argument must forward that word without tagging it again.
   */
  if (e->kind == NY_E_BINARY && e->as.binary.op &&
      strcmp(e->as.binary.op, "**") == 0 &&
      !nyr_expr_is_f64(b, e->as.binary.left) &&
      !nyr_expr_is_f64(b, e->as.binary.right)) {
    int64_t folded = 0;
    if (!nyr_fold_const_pow(e->as.binary.left, e->as.binary.right, &folded))
      return true;
  }
  if (nyr_index_of_typed_global_int_list(b, e))
    return false;
  /*
   * An unparameterized/dynamic sequence index uses the canonical accessor
   * (`rt_tbuf_index_any_raw`) even when HM left a stale RAW_INT fact on the
   * index node. Forwarding that result through an `any` formal must preserve
   * object handles such as ADTs; treating the stale fact as a scalar boxes
   * the pointer as an integer and loses its heap tag.
   */
  if (e->kind == NY_E_INDEX && e->as.index.target) {
    const expr_t *target = e->as.index.target;
    const nyr_local_t *local =
        target->kind == NY_E_IDENT && target->as.ident.name
            ? nyr_find_local_nearest(b, target->as.ident.name)
            : NULL;
    if (nyr_expr_is_dict(b, target) ||
        (local && (nyr_local_is_dynamic_sequence(local) ||
                   (local->is_list && local->type_name &&
                    strcmp(local->type_name, "list") == 0))) ||
        nyr_expr_is_dyn_list(b, target) || nyr_expr_is_any(b, target))
      return true;
  }
  /*
   * A comptime block lowers by inlining its tail expression, so the value's
   * representation is the inner expression's.  Without this passthrough an
   * untyped binding (`def x = comptime{ return [1,2,3].long }`) classified
   * the block's tagged result as a raw int and later integer compares
   * matched the tag encoding against the raw constant.
   */
  if (e->kind == NY_E_COMPTIME && e->as.comptime_expr.body) {
    const stmt_t *ct_body = e->as.comptime_expr.body;
    if (ct_body->kind == NY_S_BLOCK && ct_body->as.block.body.len == 1)
      ct_body = ct_body->as.block.body.data[0];
    const expr_t *ct_inner = NULL;
    if (ct_body && ct_body->kind == NY_S_RETURN)
      ct_inner = ct_body->as.ret.value;
    else if (ct_body && ct_body->kind == NY_S_EXPR)
      ct_inner = ct_body->as.expr.expr;
    if (ct_inner && ct_inner != e)
      return nyr_expr_is_canonical_dynamic_value(b, ct_inner);
  }
  /*
   * unwrap_or has a raw scalar fast path and a canonical any path. A typed
   * use-site fact can still describe the latter as RAW_INT; the callee's
   * selected ABI is authoritative at the next dynamic argument boundary.
   */
  if (e->kind == NY_E_CALL && ny_native_call_leaf(e) &&
      strcmp(ny_native_call_leaf(e), "unwrap_or") == 0 && nyr_expr_is_any(b, e))
    return true;
  /*
   * The `.long` builtin returns through the canonical dynamic ABI in every
   * form: a tagged small int for values that fit and a bigint handle
   * otherwise.  `rt_bigint_to_i64_raw` is the explicit raw escape hatch for
   * typed-integer consumers; everything else must treat the result as a
   * tagged word, or raw compares match the encoding instead of the value.
   */
  if (e->kind == NY_E_CALL && ny_native_call_leaf(e) &&
      strcmp(ny_native_call_leaf(e), "long") == 0)
    return true;
  /*
   * Property syntax lowers `.long` through the same attached-method ABI.
   */
  if (e->kind == NY_E_MEMBER && e->as.member.name &&
      strcmp(e->as.member.name, "long") == 0)
    return true;
  if (e->semantic.resolved && e->semantic.rep == NY_SEM_REP_TAGGED_DYNAMIC) {
    /*
     * A call node may retain a conservative dynamic use-site fact even when
     * its resolved producer has a concrete floating ABI.  The producer's
     * representation wins: its raw f64 result must still reach a typed
     * conversion rather than being decoded as a tagged word.
     */
    if (e->kind == NY_E_CALL || e->kind == NY_E_MEMCALL)
      return !nyr_expr_is_f64(b, e) && !nyr_expr_is_f32(b, e);
    return true;
  }
  /*
   * Indexing an unparameterized sequence uses the same canonical any ABI as
   * its `.get` spelling.  Keep that fact on the expression itself so an
   * indexed value assigned back into the sequence is not mistaken for an
   * already-unboxed scalar.
   */
  if (e->kind == NY_E_INDEX && e->as.index.target) {
    const expr_t *target = e->as.index.target;
    if (target->kind == NY_E_IDENT && target->as.ident.name) {
      const nyr_local_t *local =
          nyr_find_local_nearest(b, target->as.ident.name);
      if (nyr_local_is_dynamic_sequence(local))
        return true;
    }
    if (nyr_expr_is_any(b, target) || nyr_expr_is_dyn_list(b, target))
      return true;
  }
  const expr_t *target = NULL;
  const char *method = NULL;
  if (e->kind == NY_E_MEMCALL) {
    target = e->as.memcall.target;
    method = e->as.memcall.name;
  } else if (e->kind == NY_E_CALL && e->as.call.callee &&
             e->as.call.callee->kind == NY_E_MEMBER) {
    target = e->as.call.callee->as.member.target;
    method = e->as.call.callee->as.member.name;
  } else if (e->kind == NY_E_CALL && e->as.call.callee &&
             e->as.call.callee->kind == NY_E_IDENT &&
             e->as.call.callee->as.ident.name &&
             strcmp(e->as.call.callee->as.ident.name, "get") == 0 &&
             e->as.call.args.len >= 2) {
    /*
     * `get(sequence, index, fallback)` is the desugared form used by
     * imported/attached methods.  It has the same descriptor-backed ABI as
     * `sequence.get(index, fallback)`; keep the representation fact attached
     * to the accessor rather than to a particular library call site.
     */
    target = e->as.call.args.data[0].val;
    method = "get";
  }
  if (!target || !method ||
      (strcmp(method, "get") != 0 && strcmp(method, "pop") != 0))
    return false;
  if (target->kind == NY_E_IDENT && target->as.ident.name) {
    const nyr_local_t *local = nyr_find_local_nearest(b, target->as.ident.name);
    /*
     * The local record is the physical ABI fact.  An untyped list may be
     * conservatively described as dynamic by expression inference while its
     * lowered storage is still the scalar tbuf path.
     */
    if (local) {
      if (local->is_any || local->is_dyn_list)
        return true;
      if (local->is_list)
        return nyr_expr_is_dyn_list(b, target);
    }
  }
  return nyr_expr_is_dyn_list(b, target) || nyr_expr_is_any(b, target) ||
         nyr_local_is_dynamic_sequence(
             target->kind == NY_E_IDENT && target->as.ident.name
                 ? nyr_find_local_nearest(b, target->as.ident.name)
                 : NULL);
}

static const char *nyr_dict_get_symbol(nyr_builder_impl_t *b,
                                       const expr_t *key) {
  if (ny_expr_is_nil_literal(key))
    return "rt_dict_get_raw";
  if (nyr_expr_is_cstr(b, key))
    return "rt_dict_get_str_raw";
  /*
   * Mirror the set-side rule: a str-typed call result (to_str(i)) is a
   * string key by type.  Probing with the i64 variant hashed the pointer
   * word instead of the bytes, so half the lookups missed.
   */
  if (key && key->kind == NY_E_CALL) {
    const stmt_t *fn = NULL;
    if (key->semantic.canonical_callee_stmt &&
        key->semantic.canonical_callee_stmt->kind == NY_S_FUNC)
      fn = key->semantic.canonical_callee_stmt;
    const char *leaf = ny_native_call_leaf(key);
    if (leaf && strcmp(leaf, "to_str") == 0)
      return "rt_dict_get_str_raw";
    if (!fn && leaf)
      fn = nyr_find_user_function(b, leaf);
    if (!fn && leaf)
      fn = nyr_find_imported_function(b, leaf);
    if (fn && fn->as.fn.return_type &&
        strcmp(fn->as.fn.return_type, "str") == 0)
      return "rt_dict_get_str_raw";
  }
  if (key && key->semantic.resolved && key->semantic.rep != NY_SEM_REP_RAW_INT)
    return "rt_dict_get_str_raw";
  return "rt_dict_get_raw";
}

static const char *nyr_early_leaf(const expr_t *e);
static bool nyr_expr_has_raw_int_abi(const nyr_builder_t *b, const expr_t *e,
                                     unsigned depth);

static const char *nyr_dict_set_symbol(nyr_builder_impl_t *b,
                                       const expr_t *key) {
  if (ny_expr_is_nil_literal(key))
    return "rt_native_dict_set_nir_i64";
  if (key && key->kind == NY_E_CALL && key->as.call.args.len == 1 &&
      !key->as.call.args.data[0].name && nyr_early_leaf(key) &&
      strcmp(nyr_early_leaf(key), "to_str") == 0 &&
      nyr_expr_has_raw_int_abi(b, key->as.call.args.data[0].val, 0) &&
      !nyr_expr_is_any(b, key->as.call.args.data[0].val) &&
      !nyr_expr_is_bigint(b, key->as.call.args.data[0].val)) {
    const expr_t *callee = key->as.call.callee;
    if (callee && callee->kind == NY_E_IDENT && callee->as.ident.name &&
        strcmp(callee->as.ident.name, "to_str") == 0 &&
        !nyr_user_defined_fn(b, callee->as.ident.name))
      return "rt_dict_set_i64_str_raw";
  }
  if (nyr_expr_is_cstr(b, key))
    return "rt_native_dict_set_str_compact";
  /*
   * A str-typed call result (to_str(i)) is a string key by type even when
   * the value is a managed handle rather than a raw literal.  Choosing the
   * i64 variant stored to_str's reused buffer word, so half the inserted
   * keys aliased later numbers (dict MISMATCH checksum 90674).
   */
  if (key && key->kind == NY_E_CALL) {
    const stmt_t *fn = NULL;
    if (key->semantic.canonical_callee_stmt &&
        key->semantic.canonical_callee_stmt->kind == NY_S_FUNC)
      fn = key->semantic.canonical_callee_stmt;
    const char *leaf = ny_native_call_leaf(key);
    if (!fn && leaf)
      fn = nyr_find_user_function(b, leaf);
    if (!fn && leaf)
      fn = nyr_find_imported_function(b, leaf);
    if (fn && fn->as.fn.return_type &&
        strcmp(fn->as.fn.return_type, "str") == 0)
      return "rt_native_dict_set_str_compact";
  }
  if (key && key->semantic.resolved && key->semantic.rep != NY_SEM_REP_RAW_INT)
    return "rt_native_dict_set_str_compact";
  return "rt_native_dict_set_nir_i64";
}

static const stmt_t *nyr_find_user_function_in_stmt(const stmt_t *s,
                                                    const char *name,
                                                    unsigned depth);
static const stmt_t *
nyr_find_user_function_in_stmt_cached(const nyr_builder_impl_t *b,
                                      const stmt_t *scope, const char *name);
static const stmt_t *nyr_find_user_function_target(ny_native_fn_index_t *index,
                                                   const stmt_t *s,
                                                   const char *name,
                                                   const ny_options *opt,
                                                   unsigned depth);
static const stmt_t *nyr_find_imported_function(const nyr_builder_impl_t *b,
                                                const char *name);
static const stmt_t *nyr_find_reexported_function(const nyr_builder_impl_t *b,
                                                  const stmt_t *mod,
                                                  const char *name);
static const expr_t *
nyr_find_top_level_value_in_stmt(const nyr_builder_impl_t *b, const stmt_t *s,
                                 const char *name, unsigned depth);
static const char *nyr_resolve_use_alias(const nyr_builder_impl_t *b,
                                         const char *alias);
static const char *nyr_use_module_canonical(const nyr_builder_impl_t *b,
                                            const char *raw);
static const char *nyr_module_name_for_use(const stmt_t *s,
                                           const char *basename,
                                           const char *rel, unsigned depth);
static bool nyr_same_source_file(const char *a, const char *b);

static const char *nyr_platform_string(const ny_options *opt,
                                       const char *name) {
  if (!name)
    return NULL;
  const char *triple = opt ? opt->host_triple : NULL;
  const char *os = ny_host_os_name();
  const char *arch = ny_host_arch_name();
  bool windows =
      triple && (strstr(triple, "windows") || strstr(triple, "mingw") ||
                 strstr(triple, "msvc") || strstr(triple, "win32"));
  bool macos = triple && (strstr(triple, "apple") || strstr(triple, "darwin") ||
                          strstr(triple, "macos"));
  bool linux_target = triple && strstr(triple, "linux");
  if (!triple || !*triple) {
    windows = strcmp(os, "windows") == 0;
    macos = strcmp(os, "macos") == 0;
    linux_target = strcmp(os, "linux") == 0;
  }
  if (strcmp(name, "OS") == 0)
    return windows ? "windows" : macos ? "macos" : linux_target ? "linux" : os;
  if (strcmp(name, "ARCH") != 0)
    return NULL;
  if (triple && *triple) {
    if (strstr(triple, "aarch64") || strstr(triple, "arm64"))
      return "aarch64";
    if (strstr(triple, "x86_64") || strstr(triple, "amd64"))
      return "x86_64";
    if (strstr(triple, "riscv"))
      return "riscv";
    if (strstr(triple, "i386") || strstr(triple, "i686"))
      return "x86";
  }
  return arch;
}

/*
 * Compare a known function name against the lookup name, checking both
 * the full path and the leaf (last-dot) component so qualified calls
 * resolve stdlib functions nested inside modules.
 */
static bool ny_native_name_matches(const char *fn_name, const char *lookup) {
  if (!fn_name || !lookup)
    return false;
  /*
   * Every match shape below (equal names, leaf-of-either, dot-suffix) ends
   * with the same character.  Whole-tree resolution walks compare one lookup
   * against thousands of candidate names; this O(1) rejection runs before any
   * strrchr/strcmp, which profiled as the dominant cold-compile cost (the
   * leaf scan re-walked every candidate name to its end).
   */
  size_t fn_len = strlen(fn_name);
  size_t lookup_len = strlen(lookup);
  if (fn_len == 0 || lookup_len == 0 ||
      fn_name[fn_len - 1] != lookup[lookup_len - 1])
    return false;
  if (strcmp(fn_name, lookup) == 0)
    return true;
  const char *fn_leaf = ny_name_leaf(fn_name);
  if (fn_leaf && fn_leaf != fn_name && strcmp(fn_leaf, lookup) == 0)
    return true;
  const char *lookup_leaf = ny_name_leaf(lookup);
  if (lookup_leaf && lookup_leaf != lookup && strcmp(fn_name, lookup_leaf) == 0)
    return true;
  if (fn_len > lookup_len && fn_name[fn_len - lookup_len - 1] == '.' &&
      strcmp(fn_name + fn_len - lookup_len, lookup) == 0)
    return true;
  if (lookup_len > fn_len && lookup[lookup_len - fn_len - 1] == '.' &&
      strcmp(lookup + lookup_len - fn_len, fn_name) == 0)
    return true;
  return false;
}

#define NY_FN_BUCKET_SIZE 8192
#define NY_MODULE_BUCKET_SIZE 1024
#define NY_MODULE_MEMBER_CACHE_SIZE 16384
#define NY_MODULE_MEMBER_CACHE_WAYS 4u
#define NY_MODULE_MEMBER_CACHE_SETS                                            \
  (NY_MODULE_MEMBER_CACHE_SIZE / NY_MODULE_MEMBER_CACHE_WAYS)
#define NY_TREEFALL_CACHE_SIZE 16384

static uint32_t ny_fn_name_hash(const char *name);

static uint32_t ny_module_member_cache_hash(const stmt_t *mod,
                                            const char *member) {
  uint32_t h = ny_fn_name_hash(member);
  uintptr_t key = (uintptr_t)mod;
  h ^= (uint32_t)(key ^ (key >> 32));
  return h * UINT32_C(0x9e3779b1);
}

static size_t ny_module_member_cache_set(uint32_t hash) {
  return (size_t)(hash & (NY_MODULE_MEMBER_CACHE_SETS - 1u)) *
         NY_MODULE_MEMBER_CACHE_WAYS;
}
typedef struct {
  char name[512];
  const stmt_t *fn;
  bool resolved;
} ny_native_treefall_entry_t;

/*
 * Owner names are resolved before this cache is consulted. Unlike the
 * expression cache, a missing method on that nominal owner is immutable
 * within the function-index snapshot and may safely be memoized.
 */
#define NY_ATTACHED_OWNER_CACHE_SIZE 4096
typedef struct {
  ny_sym_id owner;
  ny_sym_id method;
  const stmt_t *fn;
  bool resolved;
} ny_native_attached_owner_entry_t;

#define NY_TYPE_REP_CACHE_SIZE 1024
typedef struct {
  ny_sym_id type;
  ny_sem_rep_t rep;
  bool resolved;
} ny_native_type_rep_entry_t;

/*
 * Module-local enum qualification is queried before the general type
 * representation cache, so memoize both hits and misses by fully-qualified
 * name. The function index is scoped to one immutable program snapshot.
 */
#define NY_MODULE_ENUM_TYPE_CACHE_SIZE 1024
typedef struct {
  ny_sym_id type;
  const stmt_t *decl;
  bool resolved;
} ny_native_module_enum_type_entry_t;

#define NY_SOURCE_VALUE_CACHE_SIZE 4096
typedef struct {
  ny_sym_id name;
  ny_sym_id source;
  const expr_t *value;
  bool resolved;
} ny_native_source_value_entry_t;

#define NY_IMPORTED_FN_CACHE_SIZE 65536
typedef struct {
  ny_sym_id name;
  ny_sym_id source;
  ny_sym_id module;
  const stmt_t *fn;
  bool resolved;
} ny_native_imported_fn_entry_t;

#define NY_IMPL_PROBE_SIZE 2048

#define NY_ENUM_MEMBER_CACHE_BITS 14
#define NY_ENUM_MEMBER_CACHE_SIZE (1u << NY_ENUM_MEMBER_CACHE_BITS)
typedef struct {
  ny_sym_id name;
  const stmt_t *enum_stmt;
  const stmt_enum_item_t *item;
  int64_t tag;
  bool found;
  bool resolved;
} ny_native_enum_member_entry_t;

typedef struct {
  const char *member_name;
  const char *enum_name;
  const stmt_t *enum_stmt;
  const stmt_enum_item_t *item;
  int64_t tag;
  size_t enum_order;
  size_t item_order;
} ny_native_enum_lookup_entry_t;

#define NY_USER_FN_SCOPE_CACHE_BITS 13
#define NY_USER_FN_SCOPE_CACHE_SIZE (1u << NY_USER_FN_SCOPE_CACHE_BITS)
typedef struct {
  const stmt_t *scope;
  uint32_t name_hash;
  char name[128];
  const stmt_t *fn;
  bool resolved;
} ny_native_user_fn_scope_entry_t;

#define NY_EXACT_FN_CACHE_SIZE 524288u
typedef struct {
  const stmt_t *scope;
  ny_sym_id name;
  const stmt_t *fn;
  bool resolved;
} ny_native_exact_fn_entry_t;

#define NY_MODULE_ROOT_CACHE_SIZE 16384u
typedef struct {
  const stmt_t *root;
  char name[256];
  const stmt_t *module;
  bool resolved;
} ny_native_module_root_entry_t;

typedef struct {
  const stmt_t *module;
  const char *member;
  const expr_t *result;
  uint32_t generation;
  bool resolved;
} ny_native_module_member_entry_t;

typedef struct {
  const stmt_t *module;
  const char *member;
  const expr_t *result;
  uint32_t generation;
  bool resolved;
} ny_native_imported_member_entry_t;

typedef struct {
  const program_t *prog;
  const stmt_t **funcs;
  const char **modules;
  size_t count;
  size_t cap;
  size_t *module_slots;
  size_t module_slots_cap;
} ny_native_fn_cache_t;

typedef struct {
  const program_t *prog;
  const stmt_t **modules;
  size_t count;
  size_t cap;
  struct ny_native_module_source_key_t *source_keys;
  size_t source_key_count;
  bool source_keys_ready;
} ny_native_module_cache_t;

typedef struct ny_native_module_source_key_t {
  uint64_t hash;
  const char *key;
  size_t key_len;
  const stmt_t *module;
  size_t module_index;
} ny_native_module_source_key_t;

static uint64_t ny_native_module_source_hash(const char *key, size_t len) {
  uint64_t hash = UINT64_C(14695981039346656037);
  for (size_t i = 0; i < len; ++i) {
    hash ^= (unsigned char)key[i];
    hash *= UINT64_C(1099511628211);
  }
  return hash;
}

static int ny_native_module_source_key_compare(const void *lhs,
                                               const void *rhs) {
  const ny_native_module_source_key_t *a = lhs;
  const ny_native_module_source_key_t *b = rhs;
  return a->hash < b->hash ? -1 : a->hash > b->hash ? 1 : 0;
}

static size_t ny_native_module_source_key_count(const stmt_t *module) {
  if (!module || module->kind != NY_S_MODULE || !module->as.module.name)
    return 0;
  size_t count = 2; /* module name plus the full source path */
  const char *filename = module->tok.filename;
  if (!filename || !*filename)
    return count;
  for (const char *p = filename; *p; ++p)
    if (*p == '/' && p[1]) {
      if (count == SIZE_MAX)
        return SIZE_MAX;
      ++count;
    }
  return count;
}

static bool
ny_native_module_source_keys_build(ny_native_module_cache_t *cache) {
  if (!cache)
    return false;
  size_t total = 0;
  for (size_t i = 0; i < cache->count; ++i) {
    size_t add = ny_native_module_source_key_count(cache->modules[i]);
    if (add > SIZE_MAX - total)
      return false;
    total += add;
  }
  if (total > SIZE_MAX / sizeof(*cache->source_keys))
    return false;
  ny_native_module_source_key_t *keys =
      total ? malloc(total * sizeof(*keys)) : NULL;
  if (total && !keys)
    return false;
  size_t at = 0;
  for (size_t i = 0; i < cache->count; ++i) {
    const stmt_t *module = cache->modules[i];
    const char *name = module->as.module.name;
    size_t name_len = strlen(name);
    keys[at++] = (ny_native_module_source_key_t){
        .hash = ny_native_module_source_hash(name, name_len),
        .key = name,
        .key_len = name_len,
        .module = module,
        .module_index = i,
    };
    const char *filename = module->tok.filename;
    if (!filename || !*filename)
      continue;
    size_t filename_len = strlen(filename);
    const char *suffix = filename;
    for (const char *p = filename; *p; ++p) {
      if (*p == '/' && p[1]) {
        suffix = p + 1;
        size_t suffix_len = filename_len - (size_t)(suffix - filename);
        keys[at++] = (ny_native_module_source_key_t){
            .hash = ny_native_module_source_hash(suffix, suffix_len),
            .key = suffix,
            .key_len = suffix_len,
            .module = module,
            .module_index = i,
        };
      }
    }
    keys[at++] = (ny_native_module_source_key_t){
        .hash = ny_native_module_source_hash(filename, filename_len),
        .key = filename,
        .key_len = filename_len,
        .module = module,
        .module_index = i,
    };
  }
  if (at > 1)
    qsort(keys, at, sizeof(*keys), ny_native_module_source_key_compare);
  free(cache->source_keys);
  cache->source_keys = keys;
  cache->source_key_count = at;
  cache->source_keys_ready = true;
  return true;
}

static const stmt_t *
ny_native_module_source_key_lookup(const ny_native_module_cache_t *cache,
                                   const char *key, size_t *module_index_out) {
  if (!cache || !cache->source_keys_ready || !key || !*key)
    return NULL;
  size_t len = strlen(key);
  uint64_t hash = ny_native_module_source_hash(key, len);
  size_t lo = 0, hi = cache->source_key_count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (cache->source_keys[mid].hash < hash)
      lo = mid + 1;
    else
      hi = mid;
  }
  const stmt_t *found = NULL;
  size_t found_index = SIZE_MAX;
  for (size_t i = lo;
       i < cache->source_key_count && cache->source_keys[i].hash == hash; ++i) {
    const ny_native_module_source_key_t *entry = &cache->source_keys[i];
    if (entry->key_len == len && memcmp(entry->key, key, len) == 0 &&
        entry->module_index < found_index) {
      found = entry->module;
      found_index = entry->module_index;
    }
  }
  if (module_index_out)
    *module_index_out = found_index;
  return found;
}

static const char *
ny_native_module_name_from_source_cache(const ny_native_module_cache_t *cache,
                                        const char *basename,
                                        const char *relative_path) {
  size_t module_index = SIZE_MAX, path_index = SIZE_MAX;
  const stmt_t *module =
      ny_native_module_source_key_lookup(cache, basename, &module_index);
  const stmt_t *by_path =
      ny_native_module_source_key_lookup(cache, relative_path, &path_index);
  if (by_path && path_index < module_index)
    module = by_path;
  return module && module->kind == NY_S_MODULE ? module->as.module.name : NULL;
}

struct ny_native_fn_index_t {
  ny_native_treefall_entry_t *treefall;
  ny_native_exact_fn_entry_t *exact_fns;
  ny_native_exact_fn_entry_t *target_fns;
  ny_native_module_root_entry_t *module_roots;
  ny_native_attached_owner_entry_t *attached_owners;
  ny_native_type_rep_entry_t *type_reps;
  ny_native_module_enum_type_entry_t *module_enum_types;
  ny_native_source_value_entry_t *source_values;
  ny_native_imported_fn_entry_t *imported_fns;
  ny_sym_id *impl_names;
  bool impl_names_built;
  bool impl_names_complete;
  ny_native_enum_member_entry_t *enum_members;
  ny_native_enum_lookup_entry_t *enum_lookup;
  size_t enum_lookup_len;
  const program_t *enum_lookup_prog;
  int enum_lookup_state;
  ny_native_user_fn_scope_entry_t *user_fn_scopes;
  const program_t *enum_prog;
  const program_t *user_fn_scope_prog;
  uint64_t enum_queries;
  uint64_t enum_cache_hits;
  uint64_t enum_index_hits;
  uint64_t enum_tree_scans;
  uint64_t module_member_queries;
  uint64_t module_member_cache_hits;
  uint64_t module_member_tree_scans;
  uint64_t user_fn_scope_queries;
  uint64_t user_fn_scope_hits;
  uint64_t user_fn_scope_scans;
  bool user_fn_scope_cache_disabled;
  bool user_fn_scope_cache_configured;
  bool trace_lookup_stats;
  bool trace_lookup_stats_initialized;
  ny_native_fn_cache_t functions;
  ny_native_module_cache_t modules;
  ny_native_module_member_entry_t *module_members;
  ny_native_imported_member_entry_t *imported_members;
  /*
   * This list mirrors the narrow module/block walk used by package surfaces.
   */
  const stmt_t **package_surface_modules;
  size_t package_surface_module_count;
  size_t package_surface_module_cap;
  const program_t *package_surface_prog;
  uint32_t module_member_generation;
  int32_t fn_exact_user_buckets[NY_FN_BUCKET_SIZE];
  int32_t fn_exact_user_tails[NY_FN_BUCKET_SIZE];
  int32_t fn_exact_stdlib_buckets[NY_FN_BUCKET_SIZE];
  int32_t fn_exact_stdlib_tails[NY_FN_BUCKET_SIZE];
  int32_t fn_leaf_user_buckets[NY_FN_BUCKET_SIZE];
  int32_t fn_leaf_user_tails[NY_FN_BUCKET_SIZE];
  int32_t fn_leaf_stdlib_buckets[NY_FN_BUCKET_SIZE];
  int32_t fn_leaf_stdlib_tails[NY_FN_BUCKET_SIZE];
  int32_t *fn_exact_next;
  int32_t *fn_leaf_next;
  size_t fn_chain_cap;
  int32_t module_exact_buckets[NY_MODULE_BUCKET_SIZE];
  int32_t module_exact_tails[NY_MODULE_BUCKET_SIZE];
  int32_t module_leaf_buckets[NY_MODULE_BUCKET_SIZE];
  int32_t module_leaf_tails[NY_MODULE_BUCKET_SIZE];
  int32_t *module_exact_next;
  int32_t *module_leaf_next;
  size_t module_chain_cap;
  const stmt_t **seen_functions;
  size_t seen_function_count;
  size_t seen_function_cap;
  bool build_failed;
};

static ny_native_fn_index_t *
ny_native_fn_index_for(const nyr_builder_impl_t *builder) {
  nyr_builder_impl_t *owner =
      builder ? (builder->cache_owner ? builder->cache_owner
                                      : (nyr_builder_t *)builder)
              : NULL;
  if (!owner)
    return NULL;
  if (!owner->fn_index)
    owner->fn_index = calloc(1, sizeof(*owner->fn_index));
  return owner->fn_index;
}

static uint32_t
ny_native_fn_index_generation(const nyr_builder_impl_t *builder) {
  ny_native_fn_index_t *index = ny_native_fn_index_for(builder);
  return index ? index->module_member_generation : 0;
}

static void ny_native_fn_index_free(ny_native_fn_index_t *index) {
  if (!index)
    return;
  free(index->functions.funcs);
  free(index->functions.modules);
  free(index->functions.module_slots);
  free(index->modules.modules);
  free(index->modules.source_keys);
  free(index->module_members);
  free(index->imported_members);
  free(index->package_surface_modules);
  free(index->fn_exact_next);
  free(index->fn_leaf_next);
  free(index->module_exact_next);
  free(index->module_leaf_next);
  free(index->seen_functions);
  free(index->treefall);
  free(index->exact_fns);
  free(index->target_fns);
  free(index->module_roots);
  free(index->attached_owners);
  free(index->type_reps);
  free(index->module_enum_types);
  free(index->source_values);
  free(index->imported_fns);
  free(index->impl_names);
  free(index->enum_lookup);
  if (index->trace_lookup_stats)
    fprintf(stderr,
            "[native-lookup] enum=%llu/%llu hits index=%llu tree-scans=%llu "
            "slots=%u; "
            "scoped-fn=%llu/%llu hits tree-scans=%llu slots=%u; "
            "module-member=%llu/%llu hits tree-scans=%llu slots=%u\n",
            (unsigned long long)index->enum_cache_hits,
            (unsigned long long)index->enum_queries,
            (unsigned long long)index->enum_index_hits,
            (unsigned long long)index->enum_tree_scans,
            (unsigned)NY_ENUM_MEMBER_CACHE_SIZE,
            (unsigned long long)index->user_fn_scope_hits,
            (unsigned long long)index->user_fn_scope_queries,
            (unsigned long long)index->user_fn_scope_scans,
            (unsigned)NY_USER_FN_SCOPE_CACHE_SIZE,
            (unsigned long long)index->module_member_cache_hits,
            (unsigned long long)index->module_member_queries,
            (unsigned long long)index->module_member_tree_scans,
            (unsigned)NY_MODULE_MEMBER_CACHE_SIZE);
  free(index->enum_members);
  free(index->user_fn_scopes);
  free(index);
}

static uint32_t ny_fn_name_hash(const char *name) {
  uint32_t h = 2166136261u;
  for (const char *p = name; *p; ++p)
    h = (h ^ (uint8_t)*p) * 16777619u;
  return h;
}

static inline void ny_fn_bucket_append(int32_t *heads, int32_t *tails,
                                       int32_t *nexts, uint32_t slot,
                                       int32_t idx) {
  nexts[idx] = -1;
  if (heads[slot] < 0) {
    heads[slot] = idx;
    tails[slot] = idx;
  } else {
    nexts[tails[slot]] = idx;
    tails[slot] = idx;
  }
}

static const stmt_t *
ny_native_fn_cache_lookup_exact_in_index(ny_native_fn_index_t *index,
                                         const char *name) {
  if (!index || !name || !index->fn_exact_next)
    return NULL;
  uint32_t slot = ny_fn_name_hash(name) & (NY_FN_BUCKET_SIZE - 1);
  for (int32_t i = index->fn_exact_user_buckets[slot]; i >= 0;
       i = index->fn_exact_next[i]) {
    const stmt_t *fn = index->functions.funcs[i];
    if (fn && fn->as.fn.name && strcmp(fn->as.fn.name, name) == 0)
      return fn;
  }
  for (int32_t i = index->fn_exact_stdlib_buckets[slot]; i >= 0;
       i = index->fn_exact_next[i]) {
    const stmt_t *fn = index->functions.funcs[i];
    if (fn && fn->as.fn.name && strcmp(fn->as.fn.name, name) == 0)
      return fn;
  }
  if (strncmp(name, "core.", sizeof("core.") - 1) == 0) {
    const char *short_name = name + sizeof("core.") - 1;
    int core_index = ny_std_find_module_by_name("core");
    ny_module_id_t core_id = core_index >= 0
                                 ? ny_std_module_id((size_t)core_index)
                                 : NY_MODULE_ID_UNKNOWN;
    uint32_t short_slot = ny_fn_name_hash(short_name) & (NY_FN_BUCKET_SIZE - 1);
    for (int32_t i = index->fn_exact_stdlib_buckets[short_slot]; i >= 0;
         i = index->fn_exact_next[i]) {
      const stmt_t *fn = index->functions.funcs[i];
      if (core_id != NY_MODULE_ID_UNKNOWN && fn && fn->as.fn.name &&
          fn->tok.module_id == core_id &&
          (fn->tok.module_features & NY_MODULE_FEATURE_STDLIB) != 0 &&
          strcmp(fn->as.fn.name, short_name) == 0)
        return fn;
    }
  }
  return NULL;
}

static const stmt_t *
ny_native_fn_cache_lookup_exact_with_builder(const nyr_builder_impl_t *builder,
                                             const char *name) {
  return ny_native_fn_cache_lookup_exact_in_index(
      ny_native_fn_index_for(builder), name);
}

static void ny_native_fn_cache_scan_exact_bucket(ny_native_fn_index_t *index,
                                                 const int32_t *heads,
                                                 const char *name,
                                                 const stmt_t **unique,
                                                 bool *ambiguous) {
  if (!index || !heads || !name || !index->fn_exact_next || !unique ||
      !ambiguous || *ambiguous)
    return;
  uint32_t slot = ny_fn_name_hash(name) & (NY_FN_BUCKET_SIZE - 1);
  for (int32_t i = heads[slot]; i >= 0; i = index->fn_exact_next[i]) {
    const stmt_t *candidate = index->functions.funcs[i];
    if (!candidate || !candidate->as.fn.name ||
        strcmp(candidate->as.fn.name, name) != 0)
      continue;
    if (*unique && *unique != candidate) {
      *ambiguous = true;
      return;
    }
    *unique = candidate;
  }
}

static void ny_native_fn_cache_scan_std_core_exact(ny_native_fn_index_t *index,
                                                   const char *name,
                                                   const stmt_t **unique,
                                                   bool *ambiguous) {
  if (!index || !name || !index->fn_exact_next || !unique || !ambiguous ||
      *ambiguous)
    return;
  int core_index = ny_std_find_module_by_name("core");
  ny_module_id_t core_id = core_index >= 0
                               ? ny_std_module_id((size_t)core_index)
                               : NY_MODULE_ID_UNKNOWN;
  if (core_id == NY_MODULE_ID_UNKNOWN)
    return;
  uint32_t slot = ny_fn_name_hash(name) & (NY_FN_BUCKET_SIZE - 1);
  for (int32_t i = index->fn_exact_stdlib_buckets[slot]; i >= 0;
       i = index->fn_exact_next[i]) {
    const stmt_t *candidate = index->functions.funcs[i];
    if (!candidate || !candidate->as.fn.name ||
        candidate->tok.module_id != core_id ||
        (candidate->tok.module_features & NY_MODULE_FEATURE_STDLIB) == 0 ||
        strcmp(candidate->as.fn.name, name) != 0)
      continue;
    if (*unique && *unique != candidate) {
      *ambiguous = true;
      return;
    }
    *unique = candidate;
  }
}

static const stmt_t *
ny_native_fn_cache_find_unique_exact(ny_native_fn_index_t *index,
                                     const char *name, bool *ambiguous) {
  if (ambiguous)
    *ambiguous = false;
  if (!index || !name || !index->fn_exact_next)
    return NULL;
  const stmt_t *unique = NULL;
  bool multiple = false;
  ny_native_fn_cache_scan_exact_bucket(index, index->fn_exact_user_buckets,
                                       name, &unique, &multiple);
  ny_native_fn_cache_scan_exact_bucket(index, index->fn_exact_stdlib_buckets,
                                       name, &unique, &multiple);
  if (strncmp(name, "core.", sizeof("core.") - 1) == 0) {
    ny_native_fn_cache_scan_std_core_exact(index, name + sizeof("core.") - 1,
                                           &unique, &multiple);
  }
  if (ambiguous)
    *ambiguous = multiple;
  return multiple ? NULL : unique;
}

static const stmt_t *
ny_native_fn_cache_find_unique_leaf(ny_native_fn_index_t *index,
                                    const char *name, bool *ambiguous) {
  if (ambiguous)
    *ambiguous = false;
  if (!index || !name || !index->fn_leaf_next)
    return NULL;
  const char *leaf = ny_name_leaf(name);
  if (!leaf)
    leaf = name;
  uint32_t slot = ny_fn_name_hash(leaf) & (NY_FN_BUCKET_SIZE - 1);
  const int32_t *heads[] = {index->fn_leaf_user_buckets,
                            index->fn_leaf_stdlib_buckets};
  const stmt_t *unique = NULL;
  bool multiple = false;
  for (size_t h = 0; h < 2 && !multiple; ++h) {
    for (int32_t i = heads[h][slot]; i >= 0; i = index->fn_leaf_next[i]) {
      const stmt_t *candidate = index->functions.funcs[i];
      if (!candidate || !candidate->as.fn.name ||
          !ny_native_name_matches(candidate->as.fn.name, name))
        continue;
      if (unique && unique != candidate) {
        multiple = true;
        break;
      }
      unique = candidate;
    }
  }
  if (ambiguous)
    *ambiguous = multiple;
  return multiple ? NULL : unique;
}

static const stmt_t *ny_native_fn_cache_lookup(ny_native_fn_index_t *index,
                                               const char *name) {
  if (!index || !name || !index->fn_exact_next)
    return NULL;
  const stmt_t *exact = ny_native_fn_cache_lookup_exact_in_index(index, name);
  if (exact)
    return exact;
  bool ambiguous = false;
  const stmt_t *leaf =
      ny_native_fn_cache_find_unique_leaf(index, name, &ambiguous);
  return ambiguous ? NULL : leaf;
}

static void ny_native_fn_cache_add(ny_native_fn_index_t *index, const stmt_t *s,
                                   const char *module) {
  if (!index || !s || s->kind != NY_S_FUNC || !s->as.fn.name)
    return;
  ny_native_fn_cache_t *cache = &index->functions;
  if (cache->count == cache->cap) {
    size_t next = cache->cap ? cache->cap * 2 : 256;
    const stmt_t **grown_funcs = malloc(next * sizeof(*grown_funcs));
    const char **grown_modules = malloc(next * sizeof(*grown_modules));
    if (!grown_funcs || !grown_modules) {
      free(grown_funcs);
      free(grown_modules);
      return;
    }
    if (cache->count) {
      memcpy(grown_funcs, cache->funcs, cache->count * sizeof(*grown_funcs));
      memcpy(grown_modules, cache->modules,
             cache->count * sizeof(*grown_modules));
    }
    free(cache->funcs);
    free(cache->modules);
    cache->funcs = grown_funcs;
    cache->modules = grown_modules;
    cache->cap = next;
  }
  if (cache->count >= index->fn_chain_cap) {
    size_t next = cache->cap ? cache->cap : 256;
    int32_t *exact = realloc(index->fn_exact_next, next * sizeof(int32_t));
    int32_t *leaf = realloc(index->fn_leaf_next, next * sizeof(int32_t));
    if (exact)
      index->fn_exact_next = exact;
    if (leaf)
      index->fn_leaf_next = leaf;
    if (exact && leaf)
      index->fn_chain_cap = next;
  }
  size_t idx = cache->count++;
  cache->funcs[idx] = s;
  cache->modules[idx] = module;
  bool is_stdlib = ny_is_stdlib_tok(s->tok);
  uint32_t slot = ny_fn_name_hash(s->as.fn.name) & (NY_FN_BUCKET_SIZE - 1);
  if (index->fn_exact_next && idx < index->fn_chain_cap) {
    if (is_stdlib)
      ny_fn_bucket_append(index->fn_exact_stdlib_buckets,
                          index->fn_exact_stdlib_tails, index->fn_exact_next,
                          slot, (int32_t)idx);
    else
      ny_fn_bucket_append(index->fn_exact_user_buckets,
                          index->fn_exact_user_tails, index->fn_exact_next,
                          slot, (int32_t)idx);
  }
  const char *name_leaf = ny_name_leaf(s->as.fn.name);
  if (name_leaf && index->fn_leaf_next && idx < index->fn_chain_cap) {
    uint32_t leaf_slot = ny_fn_name_hash(name_leaf) & (NY_FN_BUCKET_SIZE - 1);
    if (is_stdlib)
      ny_fn_bucket_append(index->fn_leaf_stdlib_buckets,
                          index->fn_leaf_stdlib_tails, index->fn_leaf_next,
                          leaf_slot, (int32_t)idx);
    else
      ny_fn_bucket_append(index->fn_leaf_user_buckets,
                          index->fn_leaf_user_tails, index->fn_leaf_next,
                          leaf_slot, (int32_t)idx);
  }
}

static size_t ny_native_fn_pointer_hash(const stmt_t *fn);

/*
 * Return 1 for a new function node, 0 for an already indexed node, -1 on OOM.
 */
static int ny_native_fn_seen_add(ny_native_fn_index_t *index,
                                 const stmt_t *fn) {
  if (!index || !fn)
    return 0;
  if (!index->seen_function_cap ||
      index->seen_function_count >= index->seen_function_cap * 3 / 4) {
    size_t next = index->seen_function_cap ? index->seen_function_cap * 2 : 512;
    if (next < index->seen_function_cap ||
        next > SIZE_MAX / sizeof(*index->seen_functions)) {
      index->build_failed = true;
      return -1;
    }
    const stmt_t **slots = calloc(next, sizeof(*slots));
    if (!slots) {
      index->build_failed = true;
      return -1;
    }
    size_t mask = next - 1;
    for (size_t i = 0; i < index->seen_function_cap; ++i) {
      const stmt_t *old = index->seen_functions[i];
      if (!old)
        continue;
      size_t slot = ny_native_fn_pointer_hash(old) & mask;
      while (slots[slot])
        slot = (slot + 1) & mask;
      slots[slot] = old;
    }
    free(index->seen_functions);
    index->seen_functions = slots;
    index->seen_function_cap = next;
  }
  size_t mask = index->seen_function_cap - 1;
  size_t slot = ny_native_fn_pointer_hash(fn) & mask;
  while (index->seen_functions[slot]) {
    if (index->seen_functions[slot] == fn)
      return 0;
    slot = (slot + 1) & mask;
  }
  index->seen_functions[slot] = fn;
  index->seen_function_count++;
  return 1;
}

static void ny_native_module_cache_add(ny_native_fn_index_t *index,
                                       const stmt_t *s) {
  if (!s || s->kind != NY_S_MODULE || !s->as.module.name)
    return;
  uint32_t slot =
      ny_fn_name_hash(s->as.module.name) & (NY_MODULE_BUCKET_SIZE - 1);
  if (index->module_exact_next) {
    for (int32_t i = index->module_exact_buckets[slot]; i >= 0;
         i = index->module_exact_next[i]) {
      if (index->modules.modules[i] == s)
        return;
    }
  } else {
    for (size_t i = 0; i < index->modules.count; ++i) {
      if (index->modules.modules[i] == s)
        return;
    }
  }
  if (index->modules.count == index->modules.cap) {
    size_t next = index->modules.cap ? index->modules.cap * 2 : 64;
    if (next < index->modules.cap ||
        next > SIZE_MAX / sizeof(*index->modules.modules)) {
      index->build_failed = true;
      return;
    }
    const stmt_t **grown =
        realloc(index->modules.modules, next * sizeof(*index->modules.modules));
    if (!grown) {
      /*
       * The source-key index must never describe an incomplete module set.
       */
      index->build_failed = true;
      return;
    }
    index->modules.modules = grown;
    index->modules.cap = next;
  }
  if (index->modules.count >= index->module_chain_cap) {
    size_t next_chain = index->modules.cap ? index->modules.cap : 64;
    int32_t *g_exact =
        realloc(index->module_exact_next, next_chain * sizeof(int32_t));
    int32_t *g_leaf =
        realloc(index->module_leaf_next, next_chain * sizeof(int32_t));
    if (g_exact)
      index->module_exact_next = g_exact;
    if (g_leaf)
      index->module_leaf_next = g_leaf;
    if (g_exact && g_leaf)
      index->module_chain_cap = next_chain;
  }
  size_t idx = index->modules.count++;
  index->modules.modules[idx] = s;

  if (index->module_exact_next && idx < index->module_chain_cap) {
    ny_fn_bucket_append(index->module_exact_buckets, index->module_exact_tails,
                        index->module_exact_next, slot, (int32_t)idx);
  }
  const char *leaf = ny_name_leaf(s->as.module.name);
  if (leaf && index->module_leaf_next && idx < index->module_chain_cap) {
    uint32_t lslot = ny_fn_name_hash(leaf) & (NY_MODULE_BUCKET_SIZE - 1);
    ny_fn_bucket_append(index->module_leaf_buckets, index->module_leaf_tails,
                        index->module_leaf_next, lslot, (int32_t)idx);
  }
}

static void ny_native_fn_cache_collect(ny_native_fn_index_t *index,
                                       const stmt_t *s, unsigned depth,
                                       const char *module) {
  if (!s || depth > 64 || index->build_failed)
    return;
  if (s->kind == NY_S_FUNC) {
    /*
     * Expanded imports can expose the same function node through several
     * re-export/package surfaces.  Avoid recursively walking its body again;
     * without this identity guard a broad import graph turns cache building
     * into exponential work before reachable-function collection starts.
     */
    if (ny_native_fn_seen_add(index, s) <= 0)
      return;
    ny_native_fn_cache_add(index, s, module);
    ny_native_fn_cache_collect(index, s->as.fn.body, depth + 1, module);
    return;
  }
  if (s->kind == NY_S_MODULE) {
    ny_native_module_cache_add(index, s);
    for (size_t i = 0; i < s->as.module.body.len; ++i)
      ny_native_fn_cache_collect(index, s->as.module.body.data[i], depth + 1,
                                 s->as.module.name);
    return;
  }
  if (s->kind == NY_S_IMPL) {
    for (size_t i = 0; i < s->as.impl.methods.len; ++i)
      ny_native_fn_cache_collect(index, s->as.impl.methods.data[i], depth + 1,
                                 module);
    return;
  }
  if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      ny_native_fn_cache_collect(index, s->as.block.body.data[i], depth + 1,
                                 module);
    return;
  }
  if (s->kind == NY_S_IF) {
    /*
     * Platform/comptime guards (#macos { } #else { }) select exactly one
     * branch for this compilation.  Collecting both branches put same-named
     * functions from each target into the resolution buckets, and the first
     * entry won: core.os.net.socket resolved _make_sockaddr to its macOS
     * variant on Linux (bind EAFNOSUPPORT).
     */
    bool guard_selected = false;
    if (s->as.iff.test &&
        nyr_target_eval_bool(NULL, s->as.iff.test, &guard_selected)) {
      ny_native_fn_cache_collect(
          index, guard_selected ? s->as.iff.conseq : s->as.iff.alt, depth + 1,
          module);
      return;
    }
    ny_native_fn_cache_collect(index, s->as.iff.conseq, depth + 1, module);
    ny_native_fn_cache_collect(index, s->as.iff.alt, depth + 1, module);
    return;
  }
  if (s->kind == NY_S_WHILE) {
    ny_native_fn_cache_collect(index, s->as.whl.body, depth + 1, module);
    return;
  }
  if (s->kind == NY_S_FOR) {
    ny_native_fn_cache_collect(index, s->as.fr.body, depth + 1, module);
    return;
  }
  if (s->kind == NY_S_MATCH) {
    for (size_t i = 0; i < s->as.match.arms.len; ++i)
      ny_native_fn_cache_collect(index, s->as.match.arms.data[i].conseq,
                                 depth + 1, module);
    ny_native_fn_cache_collect(index, s->as.match.default_conseq, depth + 1,
                               module);
    return;
  }
  if (s->kind == NY_S_TRY) {
    ny_native_fn_cache_collect(index, s->as.tr.body, depth + 1, module);
    ny_native_fn_cache_collect(index, s->as.tr.handler, depth + 1, module);
    return;
  }
  if (s->kind == NY_S_DEFER)
    ny_native_fn_cache_collect(index, s->as.de.body, depth + 1, module);
}
static bool nyr_stmt_has_defer(const stmt_t *s, unsigned depth) {
  if (!s || depth > 64)
    return false;
  switch (s->kind) {
  case NY_S_DEFER:
    return true;
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      if (nyr_stmt_has_defer(s->as.block.body.data[i], depth + 1))
        return true;
    return false;
  case NY_S_IF:
    return nyr_stmt_has_defer(s->as.iff.conseq, depth + 1) ||
           nyr_stmt_has_defer(s->as.iff.alt, depth + 1);
  case NY_S_WHILE:
    return nyr_stmt_has_defer(s->as.whl.body, depth + 1);
  case NY_S_FOR:
    return nyr_stmt_has_defer(s->as.fr.body, depth + 1);
  case NY_S_MATCH:
    for (size_t i = 0; i < s->as.match.arms.len; ++i)
      if (nyr_stmt_has_defer(s->as.match.arms.data[i].conseq, depth + 1))
        return true;
    return nyr_stmt_has_defer(s->as.match.default_conseq, depth + 1);
  case NY_S_TRY:
    return nyr_stmt_has_defer(s->as.tr.body, depth + 1) ||
           nyr_stmt_has_defer(s->as.tr.handler, depth + 1);
  default:
    return false;
  }
}

static uint32_t ny_alias_cache_hash(const char *source_file,
                                    const char *module_name,
                                    const char *alias) {
  uint32_t h = 2166136261u;
  if (source_file) {
    for (const char *p = source_file; *p; ++p)
      h = (h ^ (uint8_t)*p) * 16777619u;
  }
  if (module_name) {
    for (const char *p = module_name; *p; ++p)
      h = (h ^ (uint8_t)*p) * 16777619u;
  }
  for (const char *p = alias; *p; ++p)
    h = (h ^ (uint8_t)*p) * 16777619u;
  return h;
}

static void ny_native_fn_cache_build(ny_native_fn_index_t *index,
                                     const program_t *prog) {
  /*
   * This is called at the start of each whole-program lowering operation.
   * Do not use the program address as a cache lifetime token: incremental
   * callers can mutate a program in place, and an allocator can reuse the
   * same address after an earlier program has been freed.  The function and
   * module indexes below are snapshots, so rebuild them for every operation.
   * The coupled lookup tables share this compilation-owned index, so reset
   * their resolutions along with the indexed entries rather than letting
   * results outlive the snapshot they refer to.
   */
  if (++index->module_member_generation == 0) {
    if (index->module_members)
      memset(index->module_members, 0,
             NY_MODULE_MEMBER_CACHE_SIZE * sizeof(*index->module_members));
    if (index->imported_members)
      memset(index->imported_members, 0,
             NY_MODULE_MEMBER_CACHE_SIZE * sizeof(*index->imported_members));
    index->module_member_generation = 1;
  }
  free(index->exact_fns);
  index->exact_fns = NULL;
  if (index->target_fns)
    memset(index->target_fns, 0,
           NY_EXACT_FN_CACHE_SIZE * sizeof(*index->target_fns));
  free(index->module_roots);
  index->module_roots = NULL;
  memset(index->fn_exact_user_buckets, 0xFF,
         sizeof(index->fn_exact_user_buckets));
  memset(index->fn_exact_user_tails, 0xFF, sizeof(index->fn_exact_user_tails));
  memset(index->fn_exact_stdlib_buckets, 0xFF,
         sizeof(index->fn_exact_stdlib_buckets));
  memset(index->fn_exact_stdlib_tails, 0xFF,
         sizeof(index->fn_exact_stdlib_tails));
  memset(index->fn_leaf_user_buckets, 0xFF,
         sizeof(index->fn_leaf_user_buckets));
  memset(index->fn_leaf_user_tails, 0xFF, sizeof(index->fn_leaf_user_tails));
  memset(index->fn_leaf_stdlib_buckets, 0xFF,
         sizeof(index->fn_leaf_stdlib_buckets));
  memset(index->fn_leaf_stdlib_tails, 0xFF,
         sizeof(index->fn_leaf_stdlib_tails));

  memset(index->module_exact_buckets, 0xFF,
         sizeof(index->module_exact_buckets));
  memset(index->module_exact_tails, 0xFF, sizeof(index->module_exact_tails));
  memset(index->module_leaf_buckets, 0xFF, sizeof(index->module_leaf_buckets));
  memset(index->module_leaf_tails, 0xFF, sizeof(index->module_leaf_tails));

  index->functions.prog = prog;
  if (index->treefall)
    memset(index->treefall, 0,
           NY_TREEFALL_CACHE_SIZE * sizeof(*index->treefall));
  if (index->imported_fns)
    memset(index->imported_fns, 0,
           NY_IMPORTED_FN_CACHE_SIZE * sizeof(*index->imported_fns));
  if (index->impl_names)
    memset(index->impl_names, 0,
           NY_IMPL_PROBE_SIZE * sizeof(*index->impl_names));
  index->impl_names_built = false;
  index->impl_names_complete = false;
  if (index->source_values)
    memset(index->source_values, 0,
           NY_SOURCE_VALUE_CACHE_SIZE * sizeof(*index->source_values));
  if (index->type_reps)
    memset(index->type_reps, 0,
           NY_TYPE_REP_CACHE_SIZE * sizeof(*index->type_reps));
  if (index->module_enum_types)
    memset(index->module_enum_types, 0,
           NY_MODULE_ENUM_TYPE_CACHE_SIZE * sizeof(*index->module_enum_types));
  if (index->enum_members)
    memset(index->enum_members, 0,
           NY_ENUM_MEMBER_CACHE_SIZE * sizeof(*index->enum_members));
  /*
   * The AST may be edited in place between lowering passes, so pointer
   * identity does not make the sorted enum index a valid snapshot.
   */
  free(index->enum_lookup);
  index->enum_lookup = NULL;
  index->enum_lookup_len = 0;
  index->enum_lookup_prog = NULL;
  index->enum_lookup_state = 0;
  if (index->user_fn_scopes)
    memset(index->user_fn_scopes, 0,
           NY_USER_FN_SCOPE_CACHE_SIZE * sizeof(*index->user_fn_scopes));
  index->enum_prog = prog;
  index->user_fn_scope_prog = prog;
  index->enum_queries = 0;
  index->enum_cache_hits = 0;
  index->enum_index_hits = 0;
  index->enum_tree_scans = 0;
  index->user_fn_scope_queries = 0;
  index->user_fn_scope_hits = 0;
  index->user_fn_scope_scans = 0;
  if (index->attached_owners)
    memset(index->attached_owners, 0,
           NY_ATTACHED_OWNER_CACHE_SIZE * sizeof(*index->attached_owners));
  index->functions.count = 0;
  free(index->seen_functions);
  index->seen_functions = NULL;
  index->seen_function_count = 0;
  index->seen_function_cap = 0;
  index->build_failed = false;
  free(index->functions.module_slots);
  index->functions.module_slots = NULL;
  index->functions.module_slots_cap = 0;
  index->modules.prog = prog;
  index->modules.count = 0;
  index->package_surface_prog = NULL;
  index->package_surface_module_count = 0;
  free(index->modules.source_keys);
  index->modules.source_keys = NULL;
  index->modules.source_key_count = 0;
  index->modules.source_keys_ready = false;
  if (!prog)
    return;
  for (size_t i = 0; i < prog->body.len; ++i)
    ny_native_fn_cache_collect(index, prog->body.data[i], 0, NULL);
  free(index->seen_functions);
  index->seen_functions = NULL;
  index->seen_function_count = 0;
  index->seen_function_cap = 0;
  if (index->build_failed)
    return;
  (void)ny_native_module_source_keys_build(&index->modules);
  size_t module_slots_cap = 8;
  while (module_slots_cap < index->functions.count * 2) {
    if (module_slots_cap > SIZE_MAX / 2)
      return;
    module_slots_cap *= 2;
  }
  index->functions.module_slots = calloc(module_slots_cap, sizeof(size_t));
  if (!index->functions.module_slots)
    return;
  index->functions.module_slots_cap = module_slots_cap;
  for (size_t i = 0; i < index->functions.count; ++i) {
    size_t slot = ny_native_fn_pointer_hash(index->functions.funcs[i]) &
                  (module_slots_cap - 1);
    while (index->functions.module_slots[slot])
      slot = (slot + 1) & (module_slots_cap - 1);
    index->functions.module_slots[slot] = i + 1;
  }
}

static size_t ny_native_fn_pointer_hash(const stmt_t *fn) {
  uintptr_t key = (uintptr_t)fn;
  key ^= key >> 17;
  key *= (uintptr_t)0xed5ad4bbU;
  key ^= key >> 11;
  return (size_t)key;
}

/*
 * Find the containing module from the per-operation function snapshot.
 */
static bool ny_native_fn_cache_module(ny_native_fn_index_t *index,
                                      const program_t *prog, const stmt_t *fn,
                                      const char **module_out) {
  if (!prog || prog != index->functions.prog || !fn ||
      !index->functions.module_slots_cap)
    return false;
  size_t mask = index->functions.module_slots_cap - 1;
  size_t slot = ny_native_fn_pointer_hash(fn) & mask;
  for (size_t probe = 0; probe < index->functions.module_slots_cap; ++probe) {
    size_t stored = index->functions.module_slots[slot];
    if (!stored)
      return false;
    size_t i = stored - 1;
    if (index->functions.funcs[i] == fn) {
      if (module_out)
        *module_out = index->functions.modules[i];
      return true;
    }
    slot = (slot + 1) & mask;
  }
  return false;
}

static const stmt_t *nyr_find_module(const stmt_t *s, const char *name,
                                     unsigned depth);
static const stmt_t *nyr_find_nested_module(const stmt_t *s, const char *prefix,
                                            const char *leaf, unsigned depth);

static const stmt_t *nyr_find_module_in_prog(ny_native_fn_index_t *index,
                                             const program_t *prog,
                                             const char *name) {
  if (!prog || !name)
    return NULL;
  if (index && index->modules.prog == prog) {
    if (index->module_exact_next) {
      uint32_t slot = ny_fn_name_hash(name) & (NY_MODULE_BUCKET_SIZE - 1);
      for (int32_t i = index->module_exact_buckets[slot]; i >= 0;
           i = index->module_exact_next[i]) {
        const stmt_t *m = index->modules.modules[i];
        if (m && m->as.module.name && strcmp(m->as.module.name, name) == 0)
          return m;
      }
      const char *leaf = ny_name_leaf(name);
      if (leaf && index->module_leaf_next) {
        uint32_t lslot = ny_fn_name_hash(leaf) & (NY_MODULE_BUCKET_SIZE - 1);
        for (int32_t i = index->module_leaf_buckets[lslot]; i >= 0;
             i = index->module_leaf_next[i]) {
          const stmt_t *m = index->modules.modules[i];
          if (m && m->as.module.name &&
              ny_native_name_matches(m->as.module.name, name))
            return m;
        }
      }
    } else {
      for (size_t i = 0; i < index->modules.count; ++i) {
        const stmt_t *m = index->modules.modules[i];
        if (m && m->as.module.name &&
            ny_native_name_matches(m->as.module.name, name))
          return m;
      }
    }
    return NULL;
  }
  for (size_t i = 0; i < prog->body.len; ++i) {
    const stmt_t *found = nyr_find_module(prog->body.data[i], name, 0);
    if (found)
      return found;
  }
  return NULL;
}

static const stmt_t *nyr_find_module_in_source(ny_native_fn_index_t *index,
                                               const program_t *prog,
                                               const char *name,
                                               const char *source_file) {
  if (!prog || !name || !source_file || !index || index->modules.prog != prog)
    return NULL;
  for (size_t i = 0; i < index->modules.count; ++i) {
    const stmt_t *module = index->modules.modules[i];
    if (module && module->as.module.name &&
        strcmp(module->as.module.name, name) == 0 &&
        nyr_same_source_file(module->tok.filename, source_file))
      return module;
  }
  return NULL;
}

static const stmt_t *nyr_find_nested_module_in_prog(ny_native_fn_index_t *index,
                                                    const program_t *prog,
                                                    const char *prefix,
                                                    const char *leaf) {
  if (!prog || !prefix || !leaf)
    return NULL;
  if (index && index->modules.prog == prog) {
    size_t prefix_len = strlen(prefix);
    if (index->module_leaf_next) {
      uint32_t lslot = ny_fn_name_hash(leaf) & (NY_MODULE_BUCKET_SIZE - 1);
      for (int32_t i = index->module_leaf_buckets[lslot]; i >= 0;
           i = index->module_leaf_next[i]) {
        const stmt_t *m = index->modules.modules[i];
        if (m && m->as.module.name &&
            strncmp(m->as.module.name, prefix, prefix_len) == 0) {
          const char *dot = strrchr(m->as.module.name, '.');
          if (dot && strcmp(dot + 1, leaf) == 0)
            return m;
        }
      }
    } else {
      for (size_t i = 0; i < index->modules.count; ++i) {
        const stmt_t *m = index->modules.modules[i];
        if (m && m->as.module.name) {
          if (strncmp(m->as.module.name, prefix, prefix_len) == 0) {
            const char *dot = strrchr(m->as.module.name, '.');
            if (dot && strcmp(dot + 1, leaf) == 0)
              return m;
          }
        }
      }
    }
    return NULL;
  }
  for (size_t i = 0; i < prog->body.len; ++i) {
    const stmt_t *found =
        nyr_find_nested_module(prog->body.data[i], prefix, leaf, 0);
    if (found)
      return found;
  }
  return NULL;
}
/*
 * A package module's exports can surface its child modules' public functions
 * one level up without a call-site spelling change (`module implicitpkg`
 * exporting `child` exposes `implicitpkg.child.child_value` as the valid
 * reference `implicitpkg.child_value`).  Collect the unique function with the
 * requested leaf from modules whose declared names nest under `prefix.`.
 */
static void nyr_find_package_surface_fn(const nyr_builder_impl_t *b,
                                        const stmt_t *s, const char *prefix,
                                        size_t prefix_len, const char *leaf,
                                        const stmt_t **unique, bool *ambiguous,
                                        unsigned depth) {
  if (!s || depth > 64 || *ambiguous)
    return;
  if (s->kind == NY_S_MODULE) {
    if (s->as.module.name && *s->as.module.name &&
        strncmp(s->as.module.name, prefix, prefix_len) == 0 &&
        s->as.module.name[prefix_len] == '.') {
      const stmt_t *found = nyr_find_user_function_in_stmt_cached(b, s, leaf);
      if (found) {
        if (*unique && *unique != found) {
          *ambiguous = true;
          return;
        }
        *unique = found;
      }
    }
    for (size_t i = 0; i < s->as.module.body.len; ++i) {
      nyr_find_package_surface_fn(b, s->as.module.body.data[i], prefix,
                                  prefix_len, leaf, unique, ambiguous,
                                  depth + 1);
      if (*ambiguous)
        return;
    }
    return;
  }
  if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; ++i) {
      nyr_find_package_surface_fn(b, s->as.block.body.data[i], prefix,
                                  prefix_len, leaf, unique, ambiguous,
                                  depth + 1);
      if (*ambiguous)
        return;
    }
  }
}

static bool nyr_package_surface_modules_collect(ny_native_fn_index_t *index,
                                                const stmt_t *s,
                                                unsigned depth) {
  if (!s || depth > 64)
    return true;
  if (s->kind == NY_S_MODULE) {
    if (index->package_surface_module_count ==
        index->package_surface_module_cap) {
      size_t next = index->package_surface_module_cap
                        ? index->package_surface_module_cap * 2
                        : 32;
      if (next < index->package_surface_module_cap ||
          next > SIZE_MAX / sizeof(*index->package_surface_modules))
        return false;
      const stmt_t **grown =
          realloc(index->package_surface_modules,
                  next * sizeof(*index->package_surface_modules));
      if (!grown)
        return false;
      index->package_surface_modules = grown;
      index->package_surface_module_cap = next;
    }
    index->package_surface_modules[index->package_surface_module_count++] = s;
    for (size_t i = 0; i < s->as.module.body.len; ++i)
      if (!nyr_package_surface_modules_collect(index, s->as.module.body.data[i],
                                               depth + 1))
        return false;
  } else if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      if (!nyr_package_surface_modules_collect(index, s->as.block.body.data[i],
                                               depth + 1))
        return false;
  }
  return true;
}

static const stmt_t *
nyr_find_package_surface(const nyr_builder_impl_t *b, const char *prefix,
                         size_t prefix_len, const char *leaf, bool *ambiguous) {
  if (ambiguous)
    *ambiguous = false;
  if (!b || !b->prog || !prefix || !leaf || !*leaf)
    return NULL;

  const stmt_t *unique = NULL;
  bool is_ambiguous = false;
  ny_native_fn_index_t *index = ny_native_fn_index_for(b);
  if (index && index->modules.prog == b->prog) {
    if (index->package_surface_prog != b->prog) {
      index->package_surface_module_count = 0;
      bool complete = true;
      for (size_t i = 0; i < b->prog->body.len && complete; ++i)
        complete = nyr_package_surface_modules_collect(
            index, b->prog->body.data[i], 0);
      if (complete)
        index->package_surface_prog = b->prog;
    }
    if (index->package_surface_prog == b->prog) {
      for (size_t i = 0;
           i < index->package_surface_module_count && !is_ambiguous; ++i) {
        const stmt_t *module = index->package_surface_modules[i];
        if (!module->as.module.name || !*module->as.module.name ||
            strncmp(module->as.module.name, prefix, prefix_len) != 0 ||
            module->as.module.name[prefix_len] != '.')
          continue;
        const stmt_t *found =
            nyr_find_user_function_in_stmt_cached(b, module, leaf);
        if (found) {
          if (unique && unique != found)
            is_ambiguous = true;
          else
            unique = found;
        }
      }
    } else {
      for (size_t i = 0; i < b->prog->body.len && !is_ambiguous; ++i)
        nyr_find_package_surface_fn(b, b->prog->body.data[i], prefix,
                                    prefix_len, leaf, &unique, &is_ambiguous,
                                    0);
    }
  } else {
    for (size_t i = 0; i < b->prog->body.len && !is_ambiguous; ++i)
      nyr_find_package_surface_fn(b, b->prog->body.data[i], prefix, prefix_len,
                                  leaf, &unique, &is_ambiguous, 0);
  }
  if (ambiguous)
    *ambiguous = is_ambiguous;
  return is_ambiguous ? NULL : unique;
}

static const char *ny_native_fn_module(ny_native_fn_index_t *index,
                                       const program_t *prog, const stmt_t *fn);
static bool nyr_path_ends_with(const char *filename, const char *want);
static const stmt_t *nyr_find_user_function(const nyr_builder_impl_t *b,
                                            const char *name) {
  if (!name)
    return NULL;
  if (strncmp(name, "__ny_lambda_", 12) == 0) {
    ny_native_lambda_entry_t *lambda =
        ny_native_lambda_find_name(ny_native_lambda_cache_for(b), name);
    if (lambda)
      return lambda->fn;
  }
  if (!b || !b->prog)
    return NULL;
  if (b->externs && ny_extern_table_lookup(b->externs, name))
    return NULL;
  ny_native_fn_index_t *resolve_index = ny_native_fn_index_for(b);
  if (resolve_index && resolve_index->functions.prog == b->prog) {
    bool known = false;
    const stmt_t *cached =
        ny_native_fn_resolve_cache_get(b, name, b->source_file, &known);
    if (known)
      return cached;
  }
  /*
   * A source module may deliberately reuse a registered module spelling.
   * Resolve its own qualified declarations before the shared-name cache.
   */
  if ((b->source_file || b->module_name) && resolve_index &&
      resolve_index->functions.prog == b->prog) {
    ny_native_fn_index_t *index = resolve_index;
    const char *last = strrchr(name, '.');
    size_t prefix_len = last ? (size_t)(last - name) : 0;
    const stmt_t *source_leaf = NULL;
    bool ambiguous_source_leaf = false;
    const char *leaf = last ? last + 1 : name;
    uint32_t slot = ny_fn_name_hash(leaf) & (NY_FN_BUCKET_SIZE - 1);
    /*
     * Only user declarations sharing the leaf can belong to this source.
     * Bucket chains preserve declaration order and all ambiguity checks.
     */
    const int32_t *heads[] = {index->fn_leaf_user_buckets,
                              index->fn_leaf_stdlib_buckets};
    for (size_t h = 0; h < sizeof(heads) / sizeof(heads[0]); ++h) {
      for (int32_t i = heads[h][slot]; i >= 0; i = index->fn_leaf_next[i]) {
        const stmt_t *fn = index->functions.funcs[i];
        if (!fn)
          continue;
        const char *module = ny_native_fn_module(index, b->prog, fn);
        bool same_module =
            b->module_name && module && strcmp(module, b->module_name) == 0;
        bool same_source =
            b->source_file &&
            nyr_same_source_file(fn->tok.filename, b->source_file);
        if (!same_module && !same_source)
          continue;
        if (!last && ny_native_name_matches(fn->as.fn.name, name)) {
          if (source_leaf && source_leaf != fn)
            ambiguous_source_leaf = true;
          source_leaf = fn;
        }
        if (last && module && strlen(module) == prefix_len &&
            strncmp(module, name, prefix_len) == 0 &&
            ny_native_name_matches(fn->as.fn.name, last + 1)) {
          ny_native_fn_resolve_cache_put(b, name, b->source_file, fn);
          return fn;
        }
      }
    }
    if (source_leaf && !ambiguous_source_leaf) {
      ny_native_fn_resolve_cache_put(b, name, b->source_file, source_leaf);
      return source_leaf;
    }
  }
  /*
   * Resolve a local declaration before cached import bindings. A module may
   * define a helper whose leaf collides with an imported backend function;
   * the local declaration shadows that import in ordinary lexical lookup.
   *
   * A module's explicit imports shadow same-leaf declarations elsewhere in
   * the expanded program. Resolve that lexical binding before consulting the
   * shared leaf cache, which can otherwise select an unrelated public helper
   * with the same name. The imported-function cache keeps this lookup cheap.
   */
  if (!strchr(name, '.')) {
    const stmt_t *imported = nyr_find_imported_function(b, name);
    if (imported) {
      ny_native_fn_resolve_cache_put(b, name, b->source_file, imported);
      return imported;
    }
  }
  /*
   * Reachability discovery asks the same resolver about a name from many
   * expressions.  Consult both positive and negative results before any AST
   * walk; the expanded standard library is large enough that otherwise this
   * becomes quadratic.
   *
   * The function index is built once per expanded program.  Use it before
   * any recursive AST walk; reachable-call scanning asks this question for
   * the same names repeatedly.
   */
  if (ny_native_fn_index_for(b)->functions.prog == b->prog &&
      !strchr(name, '.')) {
    const stmt_t *indexed =
        ny_native_fn_cache_lookup_exact_with_builder(b, name);
    if (indexed && indexed->as.fn.name &&
        (!ny_is_stdlib_tok(indexed->tok) || !nyr_runtime_symbol(name) ||
         ny_native_runtime_stub_yields_to_source(nyr_runtime_symbol(name)))) {
      ny_native_fn_resolve_cache_put(b, name, b->source_file, indexed);
      return indexed;
    }
  }
  /*
   * The extern table is the authoritative fast path for imported C names.
   * Do not rescan the entire expanded stdlib AST for every identifier during
   * reachable-function discovery: large modules make this quadratic and can
   * effectively hang native compilation.  Exact function lookup below cannot
   * return an NY_S_EXTERN node, so the scan added no correctness guarantee.
   */
  char canonical[512];
  const char *lookup_name = name;
  if (ny_native_fn_index_for(b)->functions.prog != b->prog &&
      !strchr(name, '.')) {
    for (size_t i = 0; i < b->prog->body.len; ++i) {
      const stmt_t *found =
          nyr_find_user_function_exact(b->prog->body.data[i], name, b->options,
                                       ny_native_fn_index_for(b), 0);
      if (found && !ny_is_stdlib_tok(found->tok)) {
        ny_native_fn_resolve_cache_put(b, name, b->source_file, found);
        return found;
      }
    }
  }
  const char *imported_name = nyr_resolve_use_alias(b, name);
  if (imported_name && strchr(imported_name, '.'))
    lookup_name = imported_name;
  const char *dot = strchr(name, '.');
  if (dot && dot != name) {
    size_t alias_len = (size_t)(dot - name);
    if (alias_len < 256) {
      char alias[256];
      memcpy(alias, name, alias_len);
      alias[alias_len] = '\0';
      const char *module = nyr_resolve_use_alias(b, alias);
      if (module) {
        int n = snprintf(canonical, sizeof(canonical), "%s%s", module, dot);
        if (n > 0 && (size_t)n < sizeof(canonical))
          lookup_name = canonical;
      }
    }
  }
  /*
   * A unique exact indexed name is target-independent.  This handles the
   * compiler-generated __result_* helpers without recursively scanning every
   * expanded module.  Ambiguous names still go through target-aware lookup.
   */
  if (ny_native_fn_index_for(b)->functions.prog == b->prog) {
    bool ambiguous = false;
    const stmt_t *unique = ny_native_fn_cache_find_unique_exact(
        ny_native_fn_index_for(b), lookup_name, &ambiguous);
    if (unique && !ambiguous) {
      return ny_native_fn_resolve_cache_put_both(b, name, lookup_name, unique);
    }
  }
  if (!strchr(name, '.')) {
    const stmt_t *imported_fn = nyr_find_imported_function(b, name);
    if (imported_fn) {
      return ny_native_fn_resolve_cache_put_both(
          b, name, imported_fn->as.fn.name, imported_fn);
    }
  }
  /*
   * If the expanded function index has no exact/leaf match, this is a local
   * or data identifier rather than a function.  Avoid the expensive fallback
   * AST walk; qualified/ambiguous names continue through target resolution.
   */
  if (!strchr(name, '.') &&
      ny_native_fn_index_for(b)->functions.prog == b->prog) {
    bool ambiguous = false;
    const stmt_t *unique = ny_native_fn_cache_find_unique_leaf(
        ny_native_fn_index_for(b), name, &ambiguous);
    if (!ambiguous) {
      return ny_native_fn_resolve_cache_put_both(b, name, lookup_name, unique);
    }
  }
  bool resolve_cache_known = false;
  const stmt_t *cached_resolved = ny_native_fn_resolve_cache_get(
      b, lookup_name, b->source_file, &resolve_cache_known);
  if (resolve_cache_known)
    return cached_resolved;
  if (!resolve_cache_known && name && strcmp(name, lookup_name) != 0) {
    cached_resolved = ny_native_fn_resolve_cache_get(b, name, b->source_file,
                                                     &resolve_cache_known);
    if (resolve_cache_known)
      return cached_resolved;
  }
  if (ny_native_fn_index_for(b)->functions.prog == b->prog) {
    const stmt_t *found =
        ny_native_fn_cache_lookup(ny_native_fn_index_for(b), lookup_name);
    if (found) {
      return ny_native_fn_resolve_cache_put_both(b, name, lookup_name, found);
    }
  }
  /*
   * A dotted name resolves authoritatively inside the module its prefix
   * names (index-backed, one module body searched instead of the whole
   * flattened program).  This runs before the full-tree walks: every unique
   * qualified std.* callee otherwise paid three whole-program walks, which
   * dominated cold compiles of import-heavy probes.  If the prefix module
   * does not define the member, the walks below still run unchanged.
   */
  if (strchr(lookup_name, '.')) {
    const char *leaf = strrchr(lookup_name, '.') + 1;
    const char *last = strrchr(lookup_name, '.');
    size_t module_len = (size_t)(last - lookup_name);
    /*
     * Copy the module prefix into a dedicated buffer.  `lookup_name` can
     * alias the canonical buffer, so truncating it here would silently
     * strip the member suffix from every later dot-based fallback.
     */
    char module_prefix[256];
    if (module_len > 0 && module_len < sizeof(module_prefix)) {
      memcpy(module_prefix, lookup_name, module_len);
      module_prefix[module_len] = '\0';
      const stmt_t *module = nyr_find_module_in_prog(ny_native_fn_index_for(b),
                                                     b->prog, module_prefix);
      if (module) {
        const stmt_t *found =
            nyr_find_user_function_in_stmt_cached(b, module, leaf);
        if (!found)
          found = nyr_find_reexported_function(b, module, leaf);
        if (found) {
          return ny_native_fn_resolve_cache_put_both(b, name, lookup_name,
                                                     found);
        }
      }
    }
  }
  /*
   * The whole-tree fallback walks are module-agnostic: their answer depends
   * only on (program, name), never on the querying file.  The per-name
   * resolve cache above is keyed with source_file, so the same absent or
   * uniquely-defined name re-walked the entire flattened program once per
   * stdlib source file that asked (the dominant cold-compile cost).  Memoize
   * exactly this phase by name; hits skip the three walks in either
   * direction.
   * This cache belongs to the compilation's function index. A thread-local
   * cache keyed only by program address can retain declarations from a
   * freed program when a later parse reuses that address.
   */
  ny_native_fn_index_t *treefall_index = ny_native_fn_index_for(b);
  if (!treefall_index->treefall)
    treefall_index->treefall =
        calloc(NY_TREEFALL_CACHE_SIZE, sizeof(*treefall_index->treefall));
  ny_native_treefall_entry_t *treefall = treefall_index->treefall;
  uint32_t tf_h = 2166136261u;
  for (const char *p = lookup_name; *p; ++p)
    tf_h = (tf_h ^ (uint8_t)*p) * 16777619u;
  const stmt_t *treefall_hit = NULL;
  bool treefall_known = false;
  size_t treefall_insert = NY_TREEFALL_CACHE_SIZE;
  for (int probe = 0; treefall && probe < 16; ++probe) {
    size_t slot = (tf_h + (uint32_t)probe) & (NY_TREEFALL_CACHE_SIZE - 1);
    if (!treefall[slot].resolved) {
      treefall_insert = slot;
      break;
    }
    if (strcmp(treefall[slot].name, lookup_name) == 0) {
      treefall_hit = treefall[slot].fn;
      treefall_known = true;
      break;
    }
  }
  if (treefall_known) {
    if (treefall_hit)
      return ny_native_fn_resolve_cache_put_both(b, name, lookup_name,
                                                 treefall_hit);
  } else {
    /*
     * Prefer the branch selected for the current target. Platform modules
     * intentionally provide extern implementations in one branch and local
     * fallbacks in the other; an all-tree cache can otherwise select the
     * inactive extern declaration first.
     */
    const stmt_t *treefall_found = NULL;
    for (size_t i = 0; i < b->prog->body.len && !treefall_found; ++i) {
      treefall_found = nyr_find_user_function_exact(
          b->prog->body.data[i], lookup_name, b->options,
          ny_native_fn_index_for(b), 0);
    }
    for (size_t i = 0; i < b->prog->body.len && !treefall_found; ++i) {
      treefall_found = nyr_find_user_function_target(
          ny_native_fn_index_for(b), b->prog->body.data[i], lookup_name,
          b->options, 0);
    }
    for (size_t i = 0; i < b->prog->body.len && !treefall_found; ++i) {
      treefall_found = nyr_find_user_function_in_stmt_cached(
          b, b->prog->body.data[i], lookup_name);
    }
    size_t slot = treefall_insert;
    /*
     * Alias resolution can put lookup_name in the stack-local canonical
     * buffer. Cache the spelling itself, never that temporary pointer.
     */
    if (treefall && slot < NY_TREEFALL_CACHE_SIZE &&
        strlen(lookup_name) < sizeof(treefall[slot].name)) {
      strcpy(treefall[slot].name, lookup_name);
      treefall[slot].fn = treefall_found;
      treefall[slot].resolved = true;
    }
    if (treefall_found)
      return ny_native_fn_resolve_cache_put_both(b, name, lookup_name,
                                                 treefall_found);
  }
  if (strchr(lookup_name, '.')) {
    const char *leaf = strrchr(lookup_name, '.') + 1;
    const char *last = strrchr(lookup_name, '.');
    size_t module_len = (size_t)(last - lookup_name);
    /*
     * The prefix module did not define the member directly.  A local package
     * module (`module implicitpkg`) may still surface it through an exported
     * child module (`implicitpkg.child`), which the semantic namespace
     * flattens into the parent's public surface.  Only single-segment package
     * prefixes take this route; dotted std prefixes already spell their full
     * module path, so searching under them would widen unrelated lookups.
     */
    if (module_len > 0 && module_len < 256) {
      bool surface_ambiguous = false;
      const stmt_t *unique = nyr_find_package_surface(
          b, lookup_name, module_len, leaf, &surface_ambiguous);
      if (unique) {
        return ny_native_fn_resolve_cache_put_both(b, name, lookup_name,
                                                   unique);
      }
    }
  }
  if (strchr(lookup_name, '.')) {
    const char *leaf = strrchr(lookup_name, '.') + 1;
    bool ambiguous = false;
    const stmt_t *unique = ny_native_fn_cache_find_unique_exact(
        ny_native_fn_index_for(b), leaf, &ambiguous);
    if (ambiguous)
      return NULL;
    if (unique) {
      return ny_native_fn_resolve_cache_put_both(b, name, lookup_name, unique);
    }
  }
  /*
   * Attached methods are semantically named by their owner type
   * (`vec3.add`), while imported declarations retain the defining module
   * prefix (`core.math.vector.vec3.add`).  Resolve that representation
   * difference only when the qualified suffix is unique; a leaf-name lookup
   * would be ambiguous across unrelated types and modules.
   */
  if (strchr(lookup_name, '.')) {
    char suffix[512];
    int sn = snprintf(suffix, sizeof(suffix), ".%s", lookup_name);
    if (sn > 0 && (size_t)sn < sizeof(suffix)) {
      const stmt_t *unique = NULL;
      for (size_t i = 0; i < ny_native_fn_index_for(b)->functions.count; ++i) {
        const stmt_t *candidate = ny_native_fn_index_for(b)->functions.funcs[i];
        const char *candidate_name =
            candidate && candidate->as.fn.name ? candidate->as.fn.name : NULL;
        if (!candidate_name)
          continue;
        size_t candidate_len = strlen(candidate_name);
        if (candidate_len < (size_t)sn ||
            strcmp(candidate_name + candidate_len - (size_t)sn, suffix) != 0)
          continue;
        if (unique)
          return ny_native_fn_resolve_cache_put_both(b, name, lookup_name,
                                                     NULL);
        unique = candidate;
      }
      if (unique)
        return ny_native_fn_resolve_cache_put_both(b, name, lookup_name,
                                                   unique);
    }
  }
  ny_native_fn_resolve_cache_put_both(b, name, lookup_name, NULL);
  return NULL;
}

static bool nyr_native_user_fn_accepts_arity(const stmt_t *fn, size_t argc) {
  if (!fn || fn->kind != NY_S_FUNC)
    return false;
  size_t param_count = fn->as.fn.params.len;
  bool variadic = fn->as.fn.is_variadic && param_count > 0;
  size_t fixed_count = variadic ? param_count - 1 : param_count;
  size_t min_required = 0;
  for (size_t i = 0; i < fixed_count; ++i) {
    if (!fn->as.fn.params.data[i].def)
      min_required = i + 1;
  }
  return argc >= min_required && (variadic || argc <= param_count);
}

/*
 * Select an exact-arity declaration before falling back to name-only lookup.
 * Generated comptime-table accessors can share a leaf with their source
 * wrapper while carrying the accessor's (raw, default) signature.  Name-only
 * lookup then pairs a one-argument source call with the wrong ABI.
 */
static const stmt_t *
nyr_find_user_function_for_arity(const nyr_builder_impl_t *b, const char *name,
                                 size_t argc) {
  if (!b || !b->prog || !name ||
      ny_native_fn_index_for(b)->functions.prog != b->prog) {
    const stmt_t *fallback = nyr_find_user_function(b, name);
    return nyr_native_user_fn_accepts_arity(fallback, argc) ? fallback : NULL;
  }
  ny_native_fn_index_t *index = ny_native_fn_index_for(b);
  const char *last = strrchr(name, '.');
  const char *leaf = last ? last + 1 : name;
  size_t name_module_len = last ? (size_t)(last - name) : 0;
  const char *wanted_module = NULL;
  char module_buf[512];
  if (last) {
    if (name_module_len < sizeof(module_buf)) {
      memcpy(module_buf, name, name_module_len);
      module_buf[name_module_len] = '\0';
      wanted_module = module_buf;
    }
  } else if (b->module_name && *b->module_name) {
    wanted_module = b->module_name;
  }
  uint32_t slot = ny_fn_name_hash(leaf) & (NY_FN_BUCKET_SIZE - 1);
  const int32_t *heads[] = {index->fn_leaf_user_buckets,
                            index->fn_leaf_stdlib_buckets};
  for (size_t h = 0; h < sizeof(heads) / sizeof(heads[0]); ++h) {
    for (int32_t i = heads[h][slot]; i >= 0; i = index->fn_leaf_next[i]) {
      const stmt_t *fn = index->functions.funcs[i];
      if (!fn || !fn->as.fn.name ||
          !nyr_native_user_fn_accepts_arity(fn, argc) ||
          !ny_native_name_matches(fn->as.fn.name, name))
        continue;
      /*
       * Prefer the lexical module over the source filename. Bundled stdlib
       * input is flattened into one generated source file, so unrelated
       * modules all appear to be in the same file. A file-only match there
       * can bind a call to a same-named declaration from an earlier module
       * (for example Wayland's one-argument `pipe` to core.query.pipe).
       */
      const char *module = ny_native_fn_module(index, b->prog, fn);
      if (wanted_module && module && strcmp(module, wanted_module) == 0)
        return fn;
      /*
       * For ordinary source files without a module context, a declaration in
       * the source being lowered is still the lexical binding.
       */
      if (b->source_file && !wanted_module &&
          nyr_same_source_file(fn->tok.filename, b->source_file))
        return fn;
    }
  }
  /*
   * Explicit imports are lexically visible even when the provider module
   * differs from the caller's source and module identity. Preserve that
   * binding before the module-agnostic fallback can choose a same-leaf
   * declaration from an unrelated subsystem.
   */
  if (!last) {
    const stmt_t *imported = nyr_find_imported_function(b, name);
    if (nyr_native_user_fn_accepts_arity(imported, argc))
      return imported;
  }
  const stmt_t *fallback = nyr_find_user_function(b, name);
  return nyr_native_user_fn_accepts_arity(fallback, argc) ? fallback : NULL;
}

/*
 * Recurse into NY_S_MODULE/NY_S_BLOCK/control-flow bodies to find
 * nested function declarations (e.g. stdlib functions inside modules).
 * depth caps at 64 for safety, matching the rest of the codebase.
 */
static const stmt_t *nyr_find_user_function_in_stmt(const stmt_t *s,
                                                    const char *name,
                                                    unsigned depth) {
  if (!s || !name || depth > 64)
    return NULL;
  if (s->kind == NY_S_FUNC && s->as.fn.name &&
      ny_native_name_matches(s->as.fn.name, name))
    return s;
  if (s->kind == NY_S_MODULE) {
    for (size_t i = 0; i < s->as.module.body.len; ++i) {
      const stmt_t *found = nyr_find_user_function_in_stmt(
          s->as.module.body.data[i], name, depth + 1);
      if (found)
        return found;
    }
    return NULL;
  }
  if (s->kind == NY_S_IMPL) {
    for (size_t i = 0; i < s->as.impl.methods.len; ++i) {
      const stmt_t *found = nyr_find_user_function_in_stmt(
          s->as.impl.methods.data[i], name, depth + 1);
      if (found)
        return found;
    }
    return NULL;
  }
  if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; ++i) {
      const stmt_t *found = nyr_find_user_function_in_stmt(
          s->as.block.body.data[i], name, depth + 1);
      if (found)
        return found;
    }
    return NULL;
  }
  if (s->kind == NY_S_IF) {
    if (s->as.iff.conseq) {
      const stmt_t *found =
          nyr_find_user_function_in_stmt(s->as.iff.conseq, name, depth + 1);
      if (found)
        return found;
    }
    if (s->as.iff.alt) {
      const stmt_t *found =
          nyr_find_user_function_in_stmt(s->as.iff.alt, name, depth + 1);
      if (found)
        return found;
    }
    return NULL;
  }
  if (s->kind == NY_S_WHILE && s->as.whl.body)
    return nyr_find_user_function_in_stmt(s->as.whl.body, name, depth + 1);
  if (s->kind == NY_S_FOR && s->as.fr.body)
    return nyr_find_user_function_in_stmt(s->as.fr.body, name, depth + 1);
  if (s->kind == NY_S_MATCH) {
    for (size_t i = 0; i < s->as.match.arms.len; ++i) {
      const stmt_t *found = nyr_find_user_function_in_stmt(
          s->as.match.arms.data[i].conseq, name, depth + 1);
      if (found)
        return found;
    }
    if (s->as.match.default_conseq)
      return nyr_find_user_function_in_stmt(s->as.match.default_conseq, name,
                                            depth + 1);
    return NULL;
  }
  return NULL;
}

static const stmt_t *
nyr_find_user_function_in_stmt_cached(const nyr_builder_impl_t *b,
                                      const stmt_t *scope, const char *name) {
  if (!scope || !name)
    return NULL;
  ny_native_fn_index_t *index = ny_native_fn_index_for(b);
  if (!index)
    return nyr_find_user_function_in_stmt(scope, name, 0);
  if (!index->trace_lookup_stats_initialized) {
    index->trace_lookup_stats = getenv("NYTRIX_TRACE_LOOKUPS") != NULL;
    index->trace_lookup_stats_initialized = true;
  }
  if (!index->user_fn_scope_cache_configured) {
    index->user_fn_scope_cache_disabled =
        getenv("NYTRIX_DISABLE_USER_FN_SCOPE_CACHE") != NULL;
    index->user_fn_scope_cache_configured = true;
  }
  if (index->trace_lookup_stats)
    index->user_fn_scope_queries++;
  if (index->user_fn_scope_cache_disabled) {
    if (index->trace_lookup_stats)
      index->user_fn_scope_scans++;
    return nyr_find_user_function_in_stmt(scope, name, 0);
  }
  /*
   * Module-scoped imports are the common hot path here. The scope walk below
   * examines every declaration in a module for each imported name. The
   * compilation function index already groups declarations by leaf name and
   * records their nearest module, so use that ordered bucket to answer the
   * same search without rescanning the module body. Include descendant module
   * names because package surfaces expose child-module declarations through
   * their parent package.
   */
  if (scope->kind == NY_S_MODULE && scope->as.module.name &&
      scope->tok.filename && index->functions.prog == b->prog &&
      index->fn_leaf_next) {
    const char *leaf = ny_name_leaf(name);
    uint32_t slot =
        ny_fn_name_hash(leaf ? leaf : name) & (NY_FN_BUCKET_SIZE - 1);
    const size_t scope_len = strlen(scope->as.module.name);
    const int32_t *heads[] = {index->fn_leaf_user_buckets,
                              index->fn_leaf_stdlib_buckets};
    for (size_t h = 0; h < sizeof(heads) / sizeof(heads[0]); ++h) {
      for (int32_t i = heads[h][slot]; i >= 0; i = index->fn_leaf_next[i]) {
        const stmt_t *fn = index->functions.funcs[i];
        if (!fn || !fn->as.fn.name ||
            !ny_native_name_matches(fn->as.fn.name, name))
          continue;
        const char *module = index->functions.modules[i];
        if (!module || strncmp(module, scope->as.module.name, scope_len) != 0 ||
            (module[scope_len] != '\0' && module[scope_len] != '.'))
          continue;
        if (!fn->tok.filename ||
            !nyr_same_source_file(scope->tok.filename, fn->tok.filename))
          continue;
        if (index->trace_lookup_stats)
          index->user_fn_scope_hits++;
        return fn;
      }
    }
    if (index->trace_lookup_stats)
      index->user_fn_scope_hits++;
    return NULL;
  }
  if (index->user_fn_scope_prog != b->prog) {
    if (index->user_fn_scopes)
      memset(index->user_fn_scopes, 0,
             NY_USER_FN_SCOPE_CACHE_SIZE * sizeof(*index->user_fn_scopes));
    index->user_fn_scope_prog = b->prog;
  }
  if (!index->user_fn_scopes)
    index->user_fn_scopes =
        calloc(NY_USER_FN_SCOPE_CACHE_SIZE, sizeof(*index->user_fn_scopes));
  size_t name_len = strlen(name);
  if (name_len >= sizeof(((ny_native_user_fn_scope_entry_t *)0)->name)) {
    if (index->trace_lookup_stats)
      index->user_fn_scope_scans++;
    return nyr_find_user_function_in_stmt(scope, name, 0);
  }
  uint32_t name_hash = ny_fn_name_hash(name);
  ny_native_user_fn_scope_entry_t *entry = NULL;
  if (index->user_fn_scopes) {
    uintptr_t scope_hash = (uintptr_t)scope;
    scope_hash ^= scope_hash >> 17;
    scope_hash ^= scope_hash >> 9;
    size_t slot =
        (name_hash ^ (uint32_t)scope_hash) & (NY_USER_FN_SCOPE_CACHE_SIZE - 1u);
    entry = &index->user_fn_scopes[slot];
    if (entry->resolved && entry->scope == scope &&
        entry->name_hash == name_hash && strcmp(entry->name, name) == 0) {
      if (index->trace_lookup_stats)
        index->user_fn_scope_hits++;
      return entry->fn;
    }
  }
  if (index->trace_lookup_stats)
    index->user_fn_scope_scans++;
  const stmt_t *fn = nyr_find_user_function_in_stmt(scope, name, 0);
  if (entry) {
    entry->scope = scope;
    entry->name_hash = name_hash;
    memcpy(entry->name, name, name_len + 1);
    entry->fn = fn;
    entry->resolved = true;
  }
  return fn;
}

/*
 * Follow a module's re-export chain (`use ... (name as alias)`) down to the
 * underlying definition. A re-exporting module declares no body of its own
 * for the name, so a chase through its named-import use statements is the
 * only way to locate the source function. Bounded by depth so inter-module
 * import cycles stay deterministic. Named-import items only to prevent
 * combinatorial fanout across import-all facades.
 */
static const stmt_t *
nyr_find_reexported_function_rec(const nyr_builder_impl_t *b, const stmt_t *mod,
                                 const char *name, unsigned depth,
                                 const stmt_t **visited, size_t *visited_len,
                                 size_t visited_cap) {
  if (!b || !b->prog || !mod || !name || depth > 8)
    return NULL;
  for (size_t v = 0; v < *visited_len; ++v)
    if (visited[v] == mod)
      return NULL;
  if (*visited_len < visited_cap)
    visited[(*visited_len)++] = mod;
  else
    return NULL;
  const ny_stmt_list *body = NULL;
  if (mod->kind == NY_S_MODULE)
    body = &mod->as.module.body;
  else if (mod->kind == NY_S_BLOCK)
    body = &mod->as.block.body;
  if (!body)
    return NULL;
  for (size_t i = 0; i < body->len; ++i) {
    const stmt_t *use = body->data[i];
    if (!use || use->kind != NY_S_USE || !use->as.use.module)
      continue;
    if (use->as.use.imports.len == 0)
      continue;
    for (size_t j = 0; j < use->as.use.imports.len; ++j) {
      const use_item_t *item = &use->as.use.imports.data[j];
      const char *visible = item->alias ? item->alias : item->name;
      if (!visible || strcmp(visible, name) != 0 || !item->name)
        continue;
      const char *target_module_name =
          nyr_use_module_canonical(b, use->as.use.module);
      const stmt_t *target_mod = nyr_find_module_in_prog(
          ny_native_fn_index_for(b), b->prog, target_module_name);
      if (!target_mod)
        continue;
      const stmt_t *found =
          nyr_find_user_function_in_stmt_cached(b, target_mod, item->name);
      if (!found && target_mod->as.module.name &&
          !strchr(target_mod->as.module.name, '.')) {
        size_t plen = strlen(target_mod->as.module.name);
        bool ambiguous = false;
        const stmt_t *unique = nyr_find_package_surface(
            b, target_mod->as.module.name, plen, item->name, &ambiguous);
        if (!ambiguous && unique)
          found = unique;
      }
      if (!found)
        found = nyr_find_reexported_function_rec(b, target_mod, item->name,
                                                 depth + 1, visited,
                                                 visited_len, visited_cap);
      if (found)
        return found;
    }
  }
  return NULL;
}

static const stmt_t *nyr_find_reexported_function(const nyr_builder_impl_t *b,
                                                  const stmt_t *mod,
                                                  const char *name) {
  const stmt_t *visited[32];
  size_t visited_len = 0;
  return nyr_find_reexported_function_rec(b, mod, name, 0, visited,
                                          &visited_len, 32);
}

static const stmt_t *
nyr_find_user_function_exact_impl(const stmt_t *s, const char *name,
                                  const ny_options *opt,
                                  ny_native_fn_index_t *index, unsigned depth);

static const stmt_t *
nyr_find_user_function_exact_impl(const stmt_t *s, const char *name,
                                  const ny_options *opt,
                                  ny_native_fn_index_t *index, unsigned depth) {
  if (!s || !name || depth > 64)
    return NULL;
  if (s->kind == NY_S_FUNC && s->as.fn.name && strcmp(s->as.fn.name, name) == 0)
    return s;
  if (s->kind == NY_S_IF) {
    bool selected = false;
    if (s->as.iff.test && s->as.iff.test->kind == NY_E_COMPTIME &&
        nyr_target_eval_bool(opt, s->as.iff.test, &selected))
      return nyr_find_user_function_exact_impl(selected ? s->as.iff.conseq
                                                        : s->as.iff.alt,
                                               name, opt, index, depth + 1);
    const stmt_t *found = nyr_find_user_function_exact_impl(
        s->as.iff.conseq, name, opt, index, depth + 1);
    return found ? found
                 : nyr_find_user_function_exact_impl(s->as.iff.alt, name, opt,
                                                     index, depth + 1);
  }
  const ny_stmt_list *body = NULL;
  if (s->kind == NY_S_MODULE)
    body = &s->as.module.body;
  else if (s->kind == NY_S_BLOCK)
    body = &s->as.block.body;
  else if (s->kind == NY_S_IMPL)
    body = &s->as.impl.methods;
  if (body) {
    for (size_t i = 0; i < body->len; ++i) {
      const stmt_t *found = nyr_find_user_function_exact_impl(
          body->data[i], name, opt, index, depth + 1);
      if (found)
        return found;
    }
  }
  if (s->kind == NY_S_WHILE)
    return nyr_find_user_function_exact_impl(s->as.whl.body, name, opt, index,
                                             depth + 1);
  if (s->kind == NY_S_FOR)
    return nyr_find_user_function_exact_impl(s->as.fr.body, name, opt, index,
                                             depth + 1);
  return NULL;
}

static const stmt_t *nyr_find_user_function_exact(const stmt_t *s,
                                                  const char *name,
                                                  const ny_options *opt,
                                                  ny_native_fn_index_t *index,
                                                  unsigned depth) {
  if (depth != 0 || !index || !s || !name)
    return nyr_find_user_function_exact_impl(s, name, opt, index, depth);
  if (!index->exact_fns)
    index->exact_fns =
        calloc(NY_EXACT_FN_CACHE_SIZE, sizeof(*index->exact_fns));
  uint32_t hash = ny_fn_name_hash(name);
  hash ^= (uint32_t)((uintptr_t)s >> 4);
  hash *= 16777619u;
  ny_sym_id name_id = ny_intern_cstr(name);
  ny_native_exact_fn_entry_t *cache = index->exact_fns;
  size_t insert = hash & (NY_EXACT_FN_CACHE_SIZE - 1u);
  for (uint32_t probe = 0; cache && probe < 16; ++probe) {
    size_t slot = (hash + probe) & (NY_EXACT_FN_CACHE_SIZE - 1u);
    ny_native_exact_fn_entry_t *entry = &cache[slot];
    if (!entry->resolved) {
      insert = slot;
      break;
    }
    if (entry->scope == s && entry->name == name_id)
      return entry->fn;
  }
  const stmt_t *found =
      nyr_find_user_function_exact_impl(s, name, opt, index, 0);
  if (cache) {
    ny_native_exact_fn_entry_t *entry = &cache[insert];
    entry->scope = s;
    entry->name = name_id;
    entry->fn = found;
    entry->resolved = true;
  }
  return found;
}

static const stmt_t *
nyr_find_user_function_target_impl(ny_native_fn_index_t *index, const stmt_t *s,
                                   const char *name, const ny_options *opt,
                                   unsigned depth) {
  if (!s || !name || depth > 64)
    return NULL;
  /*
   * Unique names use the same hash index as the main resolver. Ambiguous
   * platform declarations still require the target-aware tree walk below.
   */
  if (depth == 0 && index && index->functions.count > 0) {
    bool ambiguous = false;
    const stmt_t *unique =
        ny_native_fn_cache_find_unique_exact(index, name, &ambiguous);
    if (unique && !ambiguous) {
      /*
       * A module-scoped lookup must not accept a globally unique same-leaf
       * function from another source module. Such a result bypasses the
       * module's explicit imports and can silently bind the wrong ABI.
       */
      if (s->kind != NY_S_MODULE ||
          (s->as.module.name &&
           nyr_same_source_file(s->tok.filename, unique->tok.filename) &&
           ny_native_fn_module(index, index->functions.prog, unique) &&
           strcmp(ny_native_fn_module(index, index->functions.prog, unique),
                  s->as.module.name) == 0))
        return unique;
    }
  }
  if (s->kind == NY_S_FUNC && s->as.fn.name &&
      ny_native_name_matches(s->as.fn.name, name))
    return s;
  if (s->kind == NY_S_IF) {
    bool selected = false;
    if (s->as.iff.test && s->as.iff.test->kind == NY_E_COMPTIME &&
        nyr_target_eval_bool(opt, s->as.iff.test, &selected))
      return nyr_find_user_function_target_impl(
          index, selected ? s->as.iff.conseq : s->as.iff.alt, name, opt,
          depth + 1);
    const stmt_t *found = nyr_find_user_function_target_impl(
        index, s->as.iff.conseq, name, opt, depth + 1);
    return found ? found
                 : nyr_find_user_function_target_impl(index, s->as.iff.alt,
                                                      name, opt, depth + 1);
  }
  const ny_stmt_list *body = NULL;
  if (s->kind == NY_S_MODULE)
    body = &s->as.module.body;
  else if (s->kind == NY_S_BLOCK)
    body = &s->as.block.body;
  else if (s->kind == NY_S_IMPL)
    body = &s->as.impl.methods;
  if (body) {
    for (size_t i = 0; i < body->len; ++i) {
      const stmt_t *found = nyr_find_user_function_target_impl(
          index, body->data[i], name, opt, depth + 1);
      if (found)
        return found;
    }
  }
  if (s->kind == NY_S_WHILE)
    return nyr_find_user_function_target_impl(index, s->as.whl.body, name, opt,
                                              depth + 1);
  if (s->kind == NY_S_FOR)
    return nyr_find_user_function_target_impl(index, s->as.fr.body, name, opt,
                                              depth + 1);
  return NULL;
}

static const stmt_t *nyr_find_user_function_target(ny_native_fn_index_t *index,
                                                   const stmt_t *s,
                                                   const char *name,
                                                   const ny_options *opt,
                                                   unsigned depth) {
  if (depth != 0 || !index || !s || !name)
    return nyr_find_user_function_target_impl(index, s, name, opt, depth);
  if (!index->target_fns)
    index->target_fns =
        calloc(NY_EXACT_FN_CACHE_SIZE, sizeof(*index->target_fns));
  ny_native_exact_fn_entry_t *cache = index->target_fns;
  uint32_t hash = ny_fn_name_hash(name);
  hash ^= (uint32_t)((uintptr_t)s >> 4);
  hash *= 16777619u;
  ny_sym_id name_id = ny_intern_cstr(name);
  size_t insert = hash & (NY_EXACT_FN_CACHE_SIZE - 1u);
  for (uint32_t probe = 0; cache && probe < 16; ++probe) {
    size_t slot = (hash + probe) & (NY_EXACT_FN_CACHE_SIZE - 1u);
    ny_native_exact_fn_entry_t *entry = &cache[slot];
    if (!entry->resolved) {
      insert = slot;
      break;
    }
    if (entry->scope == s && entry->name == name_id)
      return entry->fn;
  }
  const stmt_t *found =
      nyr_find_user_function_target_impl(index, s, name, opt, 0);
  if (cache) {
    ny_native_exact_fn_entry_t *entry = &cache[insert];
    entry->scope = s;
    entry->name = name_id;
    entry->fn = found;
    entry->resolved = true;
  }
  return found;
}
static const stmt_t *
nyr_find_imported_function_uncached(const nyr_builder_impl_t *b,
                                    const char *name);

static const stmt_t *nyr_find_imported_function(const nyr_builder_impl_t *b,
                                                const char *name) {
  if (!b || !b->prog || !name || strchr(name, '.'))
    return NULL;
  ny_native_fn_index_t *index = ny_native_fn_index_for(b);
  if (!index || index->functions.prog != b->prog)
    return nyr_find_imported_function_uncached(b, name);
  if (!index->imported_fns)
    index->imported_fns =
        calloc(NY_IMPORTED_FN_CACHE_SIZE, sizeof(*index->imported_fns));
  ny_sym_id name_id = ny_intern_cstr(name);
  ny_sym_id source_id = ny_intern_cstr(b->source_file);
  ny_sym_id module_id = ny_intern_cstr(b->module_name);
  uint32_t hash =
      ny_resolve_hash(name, source_id) ^ (module_id * UINT32_C(0x9e3779b1));
  size_t empty = NY_IMPORTED_FN_CACHE_SIZE;
  for (size_t probe = 0; index->imported_fns && probe < 16; ++probe) {
    size_t slot = (hash + probe) & (NY_IMPORTED_FN_CACHE_SIZE - 1);
    ny_native_imported_fn_entry_t *entry = &index->imported_fns[slot];
    if (!entry->resolved) {
      empty = slot;
      break;
    }
    if (entry->name == name_id && entry->source == source_id &&
        entry->module == module_id)
      return entry->fn;
  }
  const stmt_t *fn = nyr_find_imported_function_uncached(b, name);
  if (empty < NY_IMPORTED_FN_CACHE_SIZE)
    index->imported_fns[empty] =
        (ny_native_imported_fn_entry_t){.name = name_id,
                                        .source = source_id,
                                        .module = module_id,
                                        .fn = fn,
                                        .resolved = true};
  return fn;
}

static const stmt_t *
nyr_find_imported_function_uncached(const nyr_builder_impl_t *b,
                                    const char *name) {
  if (!b || !b->prog || !name || strchr(name, '.'))
    return NULL;
  const stmt_t *owner = NULL;
  if (b->module_name && *b->module_name) {
    owner = nyr_find_module_in_source(ny_native_fn_index_for(b), b->prog,
                                      b->module_name, b->source_file);
    if (!owner)
      owner = nyr_find_module_in_prog(ny_native_fn_index_for(b), b->prog,
                                      b->module_name);
  }
  if (owner && owner->kind == NY_S_MODULE) {
    const stmt_t *local = nyr_find_user_function_in_stmt_cached(b, owner, name);
    if (local) {
      return local;
    }
    for (size_t i = 0; i < owner->as.module.body.len; ++i) {
      const stmt_t *use = owner->as.module.body.data[i];
      if (!use || use->kind != NY_S_USE || !use->as.use.module)
        continue;
      if (b->source_file && use->tok.filename &&
          !nyr_same_source_file(use->tok.filename, b->source_file))
        continue;
      /*
       * `use module as alias` provides a namespace only; it must not expose
       * leaf functions as unqualified bindings.
       */
      if (use->as.use.alias)
        continue;
      const stmt_t *imported = NULL;
      /*
       * A local `use "./file.ny"` stores its path in the use node, while
       * the imported module is indexed by its declared name.  Resolve the
       * path to that declared name (declared module blocks may not match the
       * file basename) so selective imports and renamed functions retain
       * their source-level meaning.
       */
      const char *module_name = nyr_use_module_canonical(b, use->as.use.module);
      imported = nyr_find_module_in_prog(ny_native_fn_index_for(b), b->prog,
                                         module_name);
      if (!imported)
        continue;
      if (use->as.use.imports.len == 0) {
        const stmt_t *found =
            nyr_find_user_function_in_stmt_cached(b, imported, name);
        if (!found && imported->as.module.name &&
            !strchr(imported->as.module.name, '.')) {
          size_t plen = strlen(imported->as.module.name);
          bool ambiguous = false;
          const stmt_t *unique = nyr_find_package_surface(
              b, imported->as.module.name, plen, name, &ambiguous);
          if (!ambiguous && unique)
            found = unique;
        }
        if (found)
          return found;
        continue;
      }
      for (size_t j = 0; j < use->as.use.imports.len; ++j) {
        const use_item_t *item = &use->as.use.imports.data[j];
        const char *visible = item->alias ? item->alias : item->name;
        if (!visible || strcmp(visible, name) != 0 || !item->name)
          continue;
        const stmt_t *found =
            nyr_find_user_function_in_stmt_cached(b, imported, item->name);
        if (!found && imported->as.module.name &&
            !strchr(imported->as.module.name, '.')) {
          size_t plen = strlen(imported->as.module.name);
          bool ambiguous = false;
          const stmt_t *unique = nyr_find_package_surface(
              b, imported->as.module.name, plen, item->name, &ambiguous);
          if (!ambiguous && unique)
            found = unique;
        }
        if (!found)
          found = nyr_find_reexported_function(b, imported, item->name);
        if (found)
          return found;
      }
    }
  }
  /*
   * Expanded module declarations often omit their source file's `use`
   * nodes. If the indexed module body had no binding, continue through the
   * source-filtered top-level imports below instead of treating the module
   * body as a complete lexical scope.
   */
  for (size_t i = 0; i < b->prog->body.len; ++i) {
    const stmt_t *use = b->prog->body.data[i];
    if (!use || use->kind != NY_S_USE || !use->as.use.module)
      continue;
    if (use->as.use.alias)
      continue;
    if (b->source_file && use->tok.filename &&
        !nyr_same_source_file(use->tok.filename, b->source_file))
      continue;
    const stmt_t *imported = NULL;
    const char *module_name = nyr_use_module_canonical(b, use->as.use.module);
    imported = nyr_find_module_in_prog(ny_native_fn_index_for(b), b->prog,
                                       module_name);
    if (!imported)
      continue;
    if (use->as.use.imports.len == 0) {
      const stmt_t *found =
          nyr_find_user_function_in_stmt_cached(b, imported, name);
      if (!found && imported->as.module.name &&
          !strchr(imported->as.module.name, '.')) {
        size_t plen = strlen(imported->as.module.name);
        bool ambiguous = false;
        const stmt_t *unique = nyr_find_package_surface(
            b, imported->as.module.name, plen, name, &ambiguous);
        if (!ambiguous && unique)
          found = unique;
      }
      if (found)
        return found;
      continue;
    }
    for (size_t j = 0; j < use->as.use.imports.len; ++j) {
      const use_item_t *item = &use->as.use.imports.data[j];
      const char *visible = item->alias ? item->alias : item->name;
      if (!visible || strcmp(visible, name) != 0 || !item->name)
        continue;
      const stmt_t *found =
          nyr_find_user_function_in_stmt_cached(b, imported, item->name);
      if (!found && imported->as.module.name &&
          !strchr(imported->as.module.name, '.')) {
        size_t plen = strlen(imported->as.module.name);
        bool ambiguous = false;
        const stmt_t *unique = nyr_find_package_surface(
            b, imported->as.module.name, plen, item->name, &ambiguous);
        if (!ambiguous && unique)
          found = unique;
      }
      if (!found)
        found = nyr_find_reexported_function(b, imported, item->name);
      if (found)
        return found;
    }
  }
  return NULL;
}

static const stmt_t *nyr_find_extern_decl_in_stmt(const stmt_t *s,
                                                  const char *name,
                                                  const ny_options *opt,
                                                  unsigned depth) {
  if (!s || !name || depth > 64)
    return NULL;
  if (s->kind == NY_S_EXTERN && s->as.ext.name &&
      ny_native_name_matches(s->as.ext.name, name))
    return s;
  if (s->kind == NY_S_MODULE || s->kind == NY_S_BLOCK) {
    const ny_stmt_list *body =
        s->kind == NY_S_MODULE ? &s->as.module.body : &s->as.block.body;
    for (size_t i = 0; i < body->len; ++i) {
      const stmt_t *found =
          nyr_find_extern_decl_in_stmt(body->data[i], name, opt, depth + 1);
      if (found)
        return found;
    }
    return NULL;
  }
  if (s->kind == NY_S_IF) {
    bool selected = false;
    if (s->as.iff.test && s->as.iff.test->kind == NY_E_COMPTIME &&
        nyr_target_eval_bool(opt, s->as.iff.test, &selected)) {
      return nyr_find_extern_decl_in_stmt(
          selected ? s->as.iff.conseq : s->as.iff.alt, name, opt, depth + 1);
    }
    const stmt_t *found =
        nyr_find_extern_decl_in_stmt(s->as.iff.conseq, name, opt, depth + 1);
    return found ? found
                 : nyr_find_extern_decl_in_stmt(s->as.iff.alt, name, opt,
                                                depth + 1);
  }
  if (s->kind == NY_S_WHILE) {
    const stmt_t *found =
        nyr_find_extern_decl_in_stmt(s->as.whl.init, name, opt, depth + 1);
    if (!found)
      found =
          nyr_find_extern_decl_in_stmt(s->as.whl.body, name, opt, depth + 1);
    return found ? found
                 : nyr_find_extern_decl_in_stmt(s->as.whl.update, name, opt,
                                                depth + 1);
  }
  if (s->kind == NY_S_FOR) {
    const stmt_t *found =
        nyr_find_extern_decl_in_stmt(s->as.fr.init, name, opt, depth + 1);
    if (!found)
      found = nyr_find_extern_decl_in_stmt(s->as.fr.body, name, opt, depth + 1);
    return found ? found
                 : nyr_find_extern_decl_in_stmt(s->as.fr.update, name, opt,
                                                depth + 1);
  }
  if (s->kind == NY_S_TRY) {
    const stmt_t *found =
        nyr_find_extern_decl_in_stmt(s->as.tr.body, name, opt, depth + 1);
    return found ? found
                 : nyr_find_extern_decl_in_stmt(s->as.tr.handler, name, opt,
                                                depth + 1);
  }
  if (s->kind == NY_S_DEFER)
    return nyr_find_extern_decl_in_stmt(s->as.de.body, name, opt, depth + 1);
  if (s->kind == NY_S_MATCH) {
    for (size_t i = 0; i < s->as.match.arms.len; ++i) {
      const stmt_t *found = nyr_find_extern_decl_in_stmt(
          s->as.match.arms.data[i].conseq, name, opt, depth + 1);
      if (found)
        return found;
    }
    return nyr_find_extern_decl_in_stmt(s->as.match.default_conseq, name, opt,
                                        depth + 1);
  }
  return NULL;
}

/*
 * True when a user (non-stdlib) function named `name` is defined anywhere
 * in the program.  Native leaf names (print, assert, argc, ...) are
 * lowered directly to runtime calls unless a user function shadows the
 * leaf name.  Stdlib definitions must not count: ny_native_add_reachable_fn
 * deliberately skips leaf-named stdlib bodies (they use NY_E_LIST /
 * NY_E_MEMBER constructs the shared lowerer does not support), so the leaf
 * path has to fire for stdlib `print`/`assert`/`argc` or the generic call
 * path would emit a reference to a body that is never compiled.
 */
static bool nyr_user_defined_fn_uncached(nyr_builder_impl_t *b,
                                         const char *name) {
  if (!b || !b->prog || !name)
    return false;
  /*
   * The indexed exact lookup deliberately prefers source functions over
   * same-named stdlib helpers.  Use it before the target-aware resolver,
   * whose qualified-name cache may already contain the stdlib overload.
   */
  const char *surface = strrchr(name, '.');
  surface = surface ? surface + 1 : name;
  if (ny_native_fn_index_for(b)->functions.prog == b->prog && surface &&
      *surface) {
    const stmt_t *indexed =
        ny_native_fn_cache_lookup_exact_with_builder(b, surface);
    if (indexed && !indexed->as.fn.is_extern && !indexed->as.fn.link_name &&
        (!ny_is_stdlib_tok(indexed->tok) ||
         nyr_same_source_file(indexed->tok.filename, b->source_file) ||
         (b->module_name &&
          ny_native_fn_module(ny_native_fn_index_for(b), b->prog, indexed) &&
          strcmp(
              ny_native_fn_module(ny_native_fn_index_for(b), b->prog, indexed),
              b->module_name) == 0))) {
      return true;
    }
  }
  const stmt_t *fn = nyr_find_user_function(b, name);
  if (!fn) {
    const char *leaf = strrchr(name, '.');
    if (leaf && leaf[1])
      fn = nyr_find_user_function(b, leaf + 1);
  }
  if (fn && !fn->as.fn.is_extern && !fn->as.fn.link_name &&
      (!ny_is_stdlib_tok(fn->tok) ||
       nyr_same_source_file(fn->tok.filename, b->source_file) ||
       (b->module_name &&
        ny_native_fn_module(ny_native_fn_index_for(b), b->prog, fn) &&
        strcmp(ny_native_fn_module(ny_native_fn_index_for(b), b->prog, fn),
               b->module_name) == 0) ||
       ny_native_runtime_stub_yields_to_source(nyr_runtime_symbol(name))))
    return true;

  /*
   * Semantic resolution can canonicalize an unqualified builtin spelling to
   * its stdlib name before native lowering sees it.  That is valid only when
   * no source function shadows the surface spelling.  Search the indexed
   * program for a non-stdlib leaf so a user `len`, `set`, or `int` cannot be
   * silently replaced by a runtime intrinsic.
   */
  if (!surface || !*surface ||
      ny_native_fn_index_for(b)->functions.prog != b->prog)
    return false;
  ny_native_fn_index_t *index = ny_native_fn_index_for(b);
  if (index->fn_exact_next) {
    uint32_t slot = ny_fn_name_hash(surface) & (NY_FN_BUCKET_SIZE - 1);
    const int32_t *heads[] = {index->fn_exact_user_buckets,
                              index->fn_exact_stdlib_buckets};
    for (size_t h = 0; h < sizeof(heads) / sizeof(heads[0]); ++h) {
      for (int32_t i = heads[h][slot]; i >= 0; i = index->fn_exact_next[i]) {
        const stmt_t *candidate = index->functions.funcs[i];
        if (!candidate || !candidate->as.fn.name ||
            strcmp(candidate->as.fn.name, surface) != 0 ||
            candidate->as.fn.is_extern || candidate->as.fn.link_name)
          continue;
        const char *module =
            b->module_name ? ny_native_fn_module(index, b->prog, candidate)
                           : NULL;
        if (!ny_is_stdlib_tok(candidate->tok) ||
            nyr_same_source_file(candidate->tok.filename, b->source_file) ||
            (module && strcmp(module, b->module_name) == 0))
          return true;
      }
    }
    return false;
  }
  for (size_t i = 0; i < index->functions.count; ++i) {
    const stmt_t *candidate = index->functions.funcs[i];
    if (candidate && candidate->as.fn.name &&
        strcmp(candidate->as.fn.name, surface) == 0 &&
        !candidate->as.fn.is_extern && !candidate->as.fn.link_name &&
        (!ny_is_stdlib_tok(candidate->tok) ||
         nyr_same_source_file(candidate->tok.filename, b->source_file) ||
         (b->module_name && ny_native_fn_module(index, b->prog, candidate) &&
          strcmp(ny_native_fn_module(index, b->prog, candidate),
                 b->module_name) == 0)))
      return true;
  }
  return false;
}

static bool nyr_user_defined_fn(nyr_builder_impl_t *b, const char *name) {
  if (!b || !b->prog || !name)
    return false;
  uint32_t h = 2166136261u;
  for (const char *p = name; *p; ++p)
    h = (h ^ (uint8_t)*p) * 16777619u;
  if (b->source_file)
    for (const char *p = b->source_file; *p; ++p)
      h = (h ^ (uint8_t)*p) * 16777619u;
  if (b->module_name)
    for (const char *p = b->module_name; *p; ++p)
      h = (h ^ (uint8_t)*p) * 16777619u;
  uint32_t idx = h % NY_USER_FN_CACHE_SIZE;
  nyr_builder_impl_t *cache = b->cache_owner ? b->cache_owner : b;
  cache->cache_owner = cache;
  if (!cache->user_fn_cache) {
    cache->user_fn_cache =
        calloc(NY_USER_FN_CACHE_SIZE, sizeof(*cache->user_fn_cache));
    if (!cache->user_fn_cache)
      return nyr_user_defined_fn_uncached(b, name);
  }
  if (cache->user_fn_cache[idx].prog == b->prog &&
      ((!cache->user_fn_cache[idx].source_file && !b->source_file) ||
       (cache->user_fn_cache[idx].source_file && b->source_file &&
        strcmp(cache->user_fn_cache[idx].source_file, b->source_file) == 0)) &&
      ((!cache->user_fn_cache[idx].module_name && !b->module_name) ||
       (cache->user_fn_cache[idx].module_name && b->module_name &&
        strcmp(cache->user_fn_cache[idx].module_name, b->module_name) == 0)) &&
      cache->user_fn_cache[idx].name &&
      strcmp(cache->user_fn_cache[idx].name, name) == 0)
    return cache->user_fn_cache[idx].result;

  bool res = nyr_user_defined_fn_uncached(b, name);
  const char *owned_name = nyr_builder_own_string(cache, name);
  const char *owned_source =
      b->source_file ? nyr_builder_own_string(cache, b->source_file) : NULL;
  const char *owned_module =
      b->module_name ? nyr_builder_own_string(cache, b->module_name) : NULL;
  if (!owned_name || (b->source_file && !owned_source) ||
      (b->module_name && !owned_module))
    return res;
  cache->user_fn_cache[idx].prog = b->prog;
  cache->user_fn_cache[idx].name = owned_name;
  cache->user_fn_cache[idx].source_file = owned_source;
  cache->user_fn_cache[idx].module_name = owned_module;
  cache->user_fn_cache[idx].result = res;
  return res;
}

static const expr_t *
nyr_find_top_level_value_uncached(const nyr_builder_impl_t *b,
                                  const char *name) {
  if (!b || !b->prog || !name)
    return NULL;
  ny_native_tlv_ensure_build(b, b->prog);
  const nyr_builder_impl_t *owner = b->cache_owner ? b->cache_owner : b;
  const expr_t *fallback = NULL;
  {
    uint32_t h = 2166136261u;
    for (const char *p = name; *p; ++p)
      h = (h ^ (uint8_t)*p) * 16777619u;
    h %= NY_TLV_BUCKETS;
    for (int32_t it = owner->tlv_head ? owner->tlv_head[h] : -1; it >= 0;
         it = owner->tlv_ents[it].next) {
      if (strcmp(owner->tlv_ents[it].name, name) != 0)
        continue;
      const expr_t *found = owner->tlv_ents[it].val;
      if (!found)
        continue;
      /*
       * Imported modules can contain same-named #main/self-test bindings.
       * Prefer the declaration from the source currently being lowering so
       * a user binding cannot resolve to a same-named stdlib def.  The
       * chain is in program pre-order, matching the previous root walk.
       * NOTE: selecting the LAST same-file binding instead (for top-level
       * rebinds) is wrong without use-site ordering — it makes early uses
       * resolve to the final initializer and explodes inference on
       * rebind-heavy sources (primitives.nshape hit 12.8 GiB).
       */
      if (b->source_file && found->tok.filename &&
          strcmp(b->source_file, found->tok.filename) == 0)
        return found;
      if (!fallback)
        fallback = found;
    }
  }
  if (b->module_name && b->module_name[0]) {
    char qualified[512];
    int n =
        snprintf(qualified, sizeof(qualified), "%s.%s", b->module_name, name);
    if (n > 0 && (size_t)n < sizeof(qualified)) {
      uint32_t qh = 2166136261u;
      for (const char *p = qualified; *p; ++p)
        qh = (qh ^ (uint8_t)*p) * 16777619u;
      qh %= NY_TLV_BUCKETS;
      for (int32_t it = owner->tlv_head ? owner->tlv_head[qh] : -1; it >= 0;
           it = owner->tlv_ents[it].next) {
        if (strcmp(owner->tlv_ents[it].name, qualified) == 0)
          return owner->tlv_ents[it].val;
      }
    }
  }
  if (fallback)
    return fallback;
  return nyr_find_imported_value(b, name);
}

static const expr_t *nyr_find_top_level_value(const nyr_builder_impl_t *b,
                                              const char *name) {
  if (!b || !b->prog || !name)
    return NULL;
  nyr_builder_impl_t *mutable_b =
      b->cache_owner ? b->cache_owner : (nyr_builder_t *)b;
  mutable_b->cache_owner = mutable_b;
  uint32_t h = 2166136261u;
  for (const char *p = name; *p; ++p)
    h = (h ^ (uint8_t)*p) * 16777619u;
  uint32_t idx = h % NY_TOP_VAL_CACHE_SIZE;
  if (!mutable_b->top_val_cache) {
    mutable_b->top_val_cache =
        calloc(NY_TOP_VAL_CACHE_SIZE, sizeof(*mutable_b->top_val_cache));
    if (!mutable_b->top_val_cache)
      return nyr_find_top_level_value_uncached(b, name);
  }
  if (mutable_b->top_val_cache[idx].prog == b->prog &&
      mutable_b->top_val_cache[idx].resolved &&
      mutable_b->top_val_cache[idx].source_file == b->source_file &&
      mutable_b->top_val_cache[idx].module_name == b->module_name &&
      mutable_b->top_val_cache[idx].name &&
      strcmp(mutable_b->top_val_cache[idx].name, name) == 0)
    return mutable_b->top_val_cache[idx].result;

  const expr_t *res = nyr_find_top_level_value_uncached(b, name);
  const char *owned_name = nyr_builder_own_string(mutable_b, name);
  if (!owned_name)
    return res;
  mutable_b->top_val_cache[idx].prog = b->prog;
  mutable_b->top_val_cache[idx].source_file = b->source_file;
  mutable_b->top_val_cache[idx].module_name = b->module_name;
  mutable_b->top_val_cache[idx].name = owned_name;
  mutable_b->top_val_cache[idx].result = res;
  mutable_b->top_val_cache[idx].resolved = true;
  return res;
}

static const expr_t *nyr_find_top_level_value_in_source_uncached(
    const nyr_builder_impl_t *b, const char *name, const char *source_file) {
  if (!b || !b->prog || !name || !source_file)
    return NULL;
  ny_native_tlv_ensure_build(b, b->prog);
  const nyr_builder_impl_t *owner = b->cache_owner ? b->cache_owner : b;
  if (owner->tlv_head && owner->tlv_complete) {
    uint32_t slot = 2166136261u;
    for (const char *p = name; *p; ++p)
      slot = (slot ^ (uint8_t)*p) * 16777619u;
    slot %= NY_TLV_BUCKETS;
    const stmt_t *skipped_root = NULL;
    for (int32_t i = owner->tlv_head[slot]; i >= 0;
         i = owner->tlv_ents[i].next) {
      const ny_tlv_ent_t *entry = &owner->tlv_ents[i];
      if (entry->qualified || entry->depth > 64 ||
          entry->root == skipped_root || strcmp(entry->name, name) != 0)
        continue;
      /*
       * Preserve the old walk's first declaration per root, including its
       * source-file check. Later same-name bindings in that root cannot
       * replace the declaration selected by the original walker.
       */
      if (entry->file && strcmp(entry->file, source_file) == 0)
        return entry->val;
      skipped_root = entry->root;
    }
    return NULL;
  }
  for (size_t i = 0; i < b->prog->body.len; ++i) {
    const expr_t *found =
        nyr_find_top_level_value_in_stmt(b, b->prog->body.data[i], name, 0);
    if (found && found->tok.filename &&
        strcmp(found->tok.filename, source_file) == 0)
      return found;
  }
  return NULL;
}

static const expr_t *
nyr_find_top_level_value_in_source(const nyr_builder_impl_t *b,
                                   const char *name, const char *source_file) {
  if (!b || !b->prog || !name || !source_file)
    return NULL;
  ny_native_fn_index_t *index = ny_native_fn_index_for(b);
  if (!index || index->functions.prog != b->prog)
    return nyr_find_top_level_value_in_source_uncached(b, name, source_file);
  if (!index->source_values)
    index->source_values =
        calloc(NY_SOURCE_VALUE_CACHE_SIZE, sizeof(*index->source_values));
  ny_sym_id name_id = ny_intern_cstr(name);
  ny_sym_id source_id = ny_intern_cstr(source_file);
  uint32_t hash = ny_resolve_hash(name, source_id);
  size_t empty = NY_SOURCE_VALUE_CACHE_SIZE;
  for (size_t probe = 0; index->source_values && probe < 8; ++probe) {
    size_t slot = (hash + probe) & (NY_SOURCE_VALUE_CACHE_SIZE - 1);
    ny_native_source_value_entry_t *entry = &index->source_values[slot];
    if (!entry->resolved) {
      empty = slot;
      break;
    }
    if (entry->name == name_id && entry->source == source_id)
      return entry->value;
  }
  const expr_t *value =
      nyr_find_top_level_value_in_source_uncached(b, name, source_file);
  if (empty < NY_SOURCE_VALUE_CACHE_SIZE)
    index->source_values[empty] = (ny_native_source_value_entry_t){
        .name = name_id, .source = source_id, .value = value, .resolved = true};
  return value;
}

/*
 * Global data symbols use raw 8-byte storage. Preserve an f64 value's
 * representation at that symbol boundary so machine lowering selects XMM
 * loads/stores instead of treating the bits as an integer.
 */
static bool nyr_global_is_f64(const nyr_builder_impl_t *b, const char *name,
                              const expr_t *reference) {
  if (!b || !name)
    return false;
  const expr_t *value = NULL;
  if (reference && reference->tok.filename)
    value =
        nyr_find_top_level_value_in_source(b, name, reference->tok.filename);
  if (!value)
    value = nyr_find_top_level_value(b, name);
  if (!value) {
    const char *leaf = strrchr(name, '.');
    if (leaf && leaf[1]) {
      if (reference && reference->tok.filename)
        value = nyr_find_top_level_value_in_source(b, leaf + 1,
                                                   reference->tok.filename);
      if (!value)
        value = nyr_find_top_level_value(b, leaf + 1);
    }
  }
  return value && nyr_expr_is_f64((nyr_builder_t *)b, value);
}

static bool nyr_global_is_f32(const nyr_builder_impl_t *b, const char *name,
                              const expr_t *reference) {
  if (!b || !name)
    return false;
  const expr_t *value = NULL;
  if (reference && reference->tok.filename)
    value =
        nyr_find_top_level_value_in_source(b, name, reference->tok.filename);
  if (!value)
    value = nyr_find_top_level_value(b, name);
  if (!value) {
    const char *leaf = strrchr(name, '.');
    if (leaf && leaf[1]) {
      if (reference && reference->tok.filename)
        value = nyr_find_top_level_value_in_source(b, leaf + 1,
                                                   reference->tok.filename);
      if (!value)
        value = nyr_find_top_level_value(b, leaf + 1);
    }
  }
  return value && nyr_expr_is_f32((nyr_builder_t *)b, value);
}

static bool nyr_global_is_ptr(const nyr_builder_impl_t *b, const char *name,
                              const expr_t *reference) {
  if (!b || !name)
    return false;
  if (reference && reference->kind == NY_E_IDENT && reference->as.ident.name) {
    const nyr_local_t *local =
        nyr_find_local((nyr_builder_t *)b, reference->as.ident.name);
    if (local && !local->is_any && ny_native_type_name_is_ptr(local->type_name))
      return true;
  }
  const expr_t *value = NULL;
  if (reference && reference->tok.filename)
    value =
        nyr_find_top_level_value_in_source(b, name, reference->tok.filename);
  if (!value)
    value = nyr_find_top_level_value(b, name);
  if (!value) {
    const char *leaf = strrchr(name, '.');
    if (leaf && leaf[1]) {
      if (reference && reference->tok.filename)
        value = nyr_find_top_level_value_in_source(b, leaf + 1,
                                                   reference->tok.filename);
      if (!value)
        value = nyr_find_top_level_value(b, leaf + 1);
    }
  }
  if (!value)
    return false;
  const char *type = nyr_expr_type_name((nyr_builder_t *)b, value);
  return nyr_expr_is_ptr(b, value) || ny_native_type_name_is_ptr(type);
}

/*
 * Global scalar slots use an eight-byte representation. Keep f32 source
 * values in the same canonical floating slot as f64, converting at load and
 * store boundaries so the source class remains explicit in NYIR.
 */
static int nyr_load_global_scalar(nyr_builder_impl_t *b, int address,
                                  const char *name, const expr_t *reference) {
  if (nyr_global_is_f32(b, name, reference)) {
    int f64 = nyr_emit_load_f64(b, address);
    if (f64 < 0)
      return -1;
    int f32 = nyr_emit_f64_to_f32(b, f64);
    if (f32 < 0)
      return -1;
    /*
     * Stamp the conversion result F32: consumers (the VM's dynamic compare,
     * the machine-form gate) classify by the defining instruction's class.
     * Unstamped, the f32 bits read back as an integer and `g == 2.5f32`
     * becomes a bit comparison (the scalar-classes "f32 global store").
     */
    if (b->nyir.len > 0 && b->nyir.data[b->nyir.len - 1].dst == f32)
      b->nyir.data[b->nyir.len - 1].scalar_class = NYIR_SCALAR_F32;
    return f32;
  }
  if (nyr_global_is_f64(b, name, reference))
    return nyr_emit_load_f64(b, address);
  int value = nyr_emit_load_i64(b, address);
  if (value >= 0 && nyr_global_is_ptr(b, name, reference) && b->nyir.len &&
      b->nyir.data[b->nyir.len - 1].dst == value) {
    b->nyir.data[b->nyir.len - 1].scalar_class = NYIR_SCALAR_POINTER;
    return value;
  }
  /*
   * Mutable bool globals are stored as canonical tagged booleans, unlike
   * bool locals and arguments, which use the raw 0/1 ABI. Convert the loaded
   * carrier back to that raw ABI here; otherwise a stored false (2) passes
   * NYIR_BR_IF's nonzero test and takes the true branch.
   */
  if (value >= 0 && reference && nyr_expr_is_bool(b, (expr_t *)reference) &&
      !nyr_expr_is_any(b, (expr_t *)reference)) {
    if (b->nyir.len && b->nyir.data[b->nyir.len - 1].dst == value)
      b->nyir.data[b->nyir.len - 1].scalar_class = NYIR_SCALAR_I64_TAGGED;
    return nyr_emit_truthy(b, value);
  }
  /*
   * The storage word of a dynamic global stays tagged even when HM refines
   * its use to int. Preserve that producer fact so scalar consumers cannot
   * override the dynamic comparison path with a default raw load class.
   */
  if (value >= 0 && reference && nyr_expr_is_any(b, reference) && b->nyir.len &&
      b->nyir.data[b->nyir.len - 1].dst == value)
    b->nyir.data[b->nyir.len - 1].scalar_class = NYIR_SCALAR_I64_TAGGED;
  return value;
}

/*
 * Recurse into container statements to find top-level variable values
 * nested inside modules/blocks (e.g. stdlib constants).
 */
static const expr_t *
nyr_find_top_level_value_in_stmt_impl(const nyr_builder_impl_t *b,
                                      const stmt_t *s, const char *name,
                                      unsigned depth);

/*
 * Memoized per (compile, root statement, name): the walker recurses through
 * the entire module subtree for every query, and the re-export resolver plus
 * the constant folder issue thousands of these per compile.  Only depth-0
 * (whole-root) queries are memoized; negative answers are cached too and
 * stay valid while the program lives.
 */
#define NY_TOP_STMT_CACHE_SIZE 65536

static const expr_t *
nyr_find_top_level_value_in_stmt(const nyr_builder_impl_t *b, const stmt_t *s,
                                 const char *name, unsigned depth) {
  if (!s || !name || depth > 64)
    return NULL;
  if (depth != 0)
    return nyr_find_top_level_value_in_stmt_impl(b, s, name, depth);
  nyr_builder_impl_t *owner =
      b && b->cache_owner ? b->cache_owner : (nyr_builder_t *)b;
  if (!owner)
    return nyr_find_top_level_value_in_stmt_impl(b, s, name, depth);
  owner->cache_owner = owner;
  if (!owner->top_stmt_cache) {
    owner->top_stmt_cache =
        calloc(NY_TOP_STMT_CACHE_SIZE, sizeof(*owner->top_stmt_cache));
    if (owner->top_stmt_cache)
      owner->top_stmt_cache_cap = NY_TOP_STMT_CACHE_SIZE;
  }
  if (!owner->top_stmt_cache)
    return nyr_find_top_level_value_in_stmt_impl(b, s, name, depth);
  ny_sym_id name_id = ny_intern_cstr(name);
  uint32_t h = (uint32_t)name_id * UINT32_C(0x9e3779b1);
  uintptr_t root_id = (uintptr_t)s;
  h ^= (uint32_t)(root_id ^ (root_id >> 32));
  size_t insert = NY_TOP_STMT_CACHE_SIZE;
  for (int probe = 0; probe < 8; ++probe) {
    ny_top_stmt_cache_entry_t *hit =
        &owner->top_stmt_cache[(h + (uint32_t)probe) % NY_TOP_STMT_CACHE_SIZE];
    if (!hit->resolved) {
      insert = (h + (uint32_t)probe) % NY_TOP_STMT_CACHE_SIZE;
      break;
    }
    if (hit->root == s && hit->name_id == name_id)
      return hit->result;
  }
  const expr_t *res = nyr_find_top_level_value_in_stmt_impl(b, s, name, depth);
  if (insert == NY_TOP_STMT_CACHE_SIZE)
    return res;
  ny_top_stmt_cache_entry_t *slot = &owner->top_stmt_cache[insert];
  slot->root = s;
  slot->name_id = name_id;
  slot->result = res;
  slot->resolved = true;
  return res;
}

static const expr_t *
nyr_find_top_level_value_in_stmt_impl(const nyr_builder_impl_t *b,
                                      const stmt_t *s, const char *name,
                                      unsigned depth) {
  if (!s || !name || depth > 64)
    return NULL;
  if (s->kind == NY_S_VAR) {
    for (size_t n = 0; n < s->as.var.names.len && n < s->as.var.exprs.len; ++n)
      if (s->as.var.names.data[n] && strcmp(s->as.var.names.data[n], name) == 0)
        return s->as.var.exprs.data[n];
    return NULL;
  }
  if (s->kind == NY_S_MODULE) {
    for (size_t i = 0; i < s->as.module.body.len; ++i) {
      const expr_t *found = nyr_find_top_level_value_in_stmt(
          b, s->as.module.body.data[i], name, depth + 1);
      if (found)
        return found;
    }
    return NULL;
  }
  if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; ++i) {
      const expr_t *found = nyr_find_top_level_value_in_stmt(
          b, s->as.block.body.data[i], name, depth + 1);
      if (found)
        return found;
    }
    return NULL;
  }
  if (s->kind == NY_S_IF) {
    bool selected = false;
    if (s->as.iff.test && s->as.iff.test->kind == NY_E_COMPTIME &&
        nyr_target_eval_bool(NULL, s->as.iff.test, &selected)) {
      return nyr_find_top_level_value_in_stmt(
          b, selected ? s->as.iff.conseq : s->as.iff.alt, name, depth + 1);
    }
    const expr_t *found =
        nyr_find_top_level_value_in_stmt(b, s->as.iff.conseq, name, depth + 1);
    return found ? found
                 : nyr_find_top_level_value_in_stmt(b, s->as.iff.alt, name,
                                                    depth + 1);
  }
  return NULL;
}

static const stmt_t *nyr_find_top_level_var_in_stmt(const stmt_t *s,
                                                    const char *name,
                                                    unsigned depth) {
  if (!s || !name || depth > 64)
    return NULL;
  if (s->kind == NY_S_VAR) {
    for (size_t n = 0; n < s->as.var.names.len; ++n)
      if (s->as.var.names.data[n] && strcmp(s->as.var.names.data[n], name) == 0)
        return s;
    return NULL;
  }
  if (s->kind == NY_S_MODULE) {
    for (size_t i = 0; i < s->as.module.body.len; ++i) {
      const stmt_t *found = nyr_find_top_level_var_in_stmt(
          s->as.module.body.data[i], name, depth + 1);
      if (found)
        return found;
    }
    return NULL;
  }
  if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; ++i) {
      const stmt_t *found = nyr_find_top_level_var_in_stmt(
          s->as.block.body.data[i], name, depth + 1);
      if (found)
        return found;
    }
    return NULL;
  }
  if (s->kind == NY_S_IF) {
    const stmt_t *found =
        nyr_find_top_level_var_in_stmt(s->as.iff.conseq, name, depth + 1);
    return found
               ? found
               : nyr_find_top_level_var_in_stmt(s->as.iff.alt, name, depth + 1);
  }
  return NULL;
}

static const stmt_t *nyr_find_top_level_var_in_prog(const nyr_builder_impl_t *b,
                                                    const program_t *prog,
                                                    const char *name);
static const stmt_t *nyr_find_top_level_var_in_source_in_prog(
    const nyr_builder_impl_t *b, const program_t *prog, const char *name,
    const char *source_file);

static const stmt_t *nyr_find_top_level_var_stmt(const nyr_builder_impl_t *b,
                                                 const char *name) {
  if (!b || !b->prog || !name)
    return NULL;
  return nyr_find_top_level_var_in_prog(b, b->prog, name);
}

static bool nyr_index_of_typed_global_int_list(nyr_builder_impl_t *b,
                                               const expr_t *e) {
  if (!b || !b->prog || !e || e->kind != NY_E_INDEX || !e->as.index.target ||
      e->as.index.target->kind != NY_E_IDENT ||
      !e->as.index.target->as.ident.name || e->as.index.stop ||
      e->as.index.step)
    return false;
  if (!b->current_fn_name || strcmp(b->current_fn_name, "rt_main") != 0)
    return false;
  const char *name = e->as.index.target->as.ident.name;
  const nyr_local_t *local = nyr_find_local_nearest(b, name);
  if (local &&
      (!local->type_name || strcmp(local->type_name, "list<int>") != 0))
    return false;
  const stmt_t *decl = NULL;
  if (e->tok.filename)
    decl = nyr_find_top_level_var_in_source_in_prog(b, b->prog, name,
                                                    e->tok.filename);
  if (!decl)
    decl = nyr_find_top_level_var_stmt(b, name);
  if (!decl || decl->kind != NY_S_VAR)
    return false;
  for (size_t i = 0; i < decl->as.var.names.len; ++i)
    if (decl->as.var.names.data[i] &&
        strcmp(decl->as.var.names.data[i], name) == 0 &&
        i < decl->as.var.types.len && decl->as.var.types.data[i] &&
        strcmp(decl->as.var.types.data[i], "list<int>") == 0)
      return true;
  return false;
}

/*
 * Top-level bindings use the same storage model as locals, but they are not
 * represented in the local alias counters. Count direct list aliases here so
 * indexed mutation can perform copy-on-write exactly once while ordinary
 * in-place methods (notably clear) still observe the shared reference.
 */
static unsigned nyr_count_global_aliases_in_stmt(const stmt_t *s,
                                                 const char *name,
                                                 const char *source_file,
                                                 unsigned depth) {
  if (!s || !name || depth > 64)
    return 0;
  if (s->kind == NY_S_VAR) {
    if (source_file && s->tok.filename &&
        strcmp(source_file, s->tok.filename) != 0)
      return 0;
    unsigned count = 0;
    for (size_t i = 0; i < s->as.var.names.len; ++i) {
      if (i >= s->as.var.exprs.len || !s->as.var.exprs.data[i])
        continue;
      const expr_t *init = s->as.var.exprs.data[i];
      if (init->kind == NY_E_IDENT && init->as.ident.name &&
          strcmp(init->as.ident.name, name) == 0)
        count++;
    }
    return count;
  }
  if (s->kind == NY_S_MODULE || s->kind == NY_S_BLOCK) {
    const ny_stmt_list *body =
        s->kind == NY_S_MODULE ? &s->as.module.body : &s->as.block.body;
    unsigned count = 0;
    for (size_t i = 0; i < body->len; ++i)
      count += nyr_count_global_aliases_in_stmt(body->data[i], name,
                                                source_file, depth + 1);
    return count;
  }
  if (s->kind == NY_S_IF)
    return nyr_count_global_aliases_in_stmt(s->as.iff.conseq, name, source_file,
                                            depth + 1) +
           nyr_count_global_aliases_in_stmt(s->as.iff.alt, name, source_file,
                                            depth + 1);
  return 0;
}

static unsigned nyr_global_alias_count(const nyr_builder_impl_t *b,
                                       const char *name) {
  if (!b || !b->prog || !name)
    return 0;
  unsigned count = 0;
  for (size_t i = 0; i < b->prog->body.len; ++i)
    count += nyr_count_global_aliases_in_stmt(b->prog->body.data[i], name,
                                              b->source_file, 0);
  return count;
}

static const stmt_t *
nyr_find_top_level_var_in_source_stmt(const stmt_t *s, const char *name,
                                      const char *source_file, unsigned depth) {
  if (!s || !name || !source_file || depth > 64)
    return NULL;
  if (s->kind == NY_S_VAR) {
    if (!s->tok.filename || strcmp(s->tok.filename, source_file) != 0)
      return NULL;
    for (size_t i = 0; i < s->as.var.names.len; ++i)
      if (s->as.var.names.data[i] && strcmp(s->as.var.names.data[i], name) == 0)
        return s;
    return NULL;
  }
  if (s->kind == NY_S_MODULE) {
    for (size_t i = 0; i < s->as.module.body.len; ++i) {
      const stmt_t *found = nyr_find_top_level_var_in_source_stmt(
          s->as.module.body.data[i], name, source_file, depth + 1);
      if (found)
        return found;
    }
    return NULL;
  }
  if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; ++i) {
      const stmt_t *found = nyr_find_top_level_var_in_source_stmt(
          s->as.block.body.data[i], name, source_file, depth + 1);
      if (found)
        return found;
    }
    return NULL;
  }
  if (s->kind == NY_S_IF) {
    const stmt_t *found = nyr_find_top_level_var_in_source_stmt(
        s->as.iff.conseq, name, source_file, depth + 1);
    return found ? found
                 : nyr_find_top_level_var_in_source_stmt(
                       s->as.iff.alt, name, source_file, depth + 1);
  }
  return NULL;
}

/*
 * A flattened program can contain same-named globals from several modules.
 * The first declaration is not enough to prove that a list is immutable.
 */
static bool nyr_has_mutable_top_var(const stmt_t *s, const char *name,
                                    unsigned depth) {
  if (!s || !name || depth > 64)
    return false;
  if (s->kind == NY_S_VAR) {
    if (!s->as.var.is_mut)
      return false;
    for (size_t i = 0; i < s->as.var.names.len; ++i)
      if (s->as.var.names.data[i] && strcmp(s->as.var.names.data[i], name) == 0)
        return true;
    return false;
  }
  if (s->kind == NY_S_MODULE) {
    for (size_t i = 0; i < s->as.module.body.len; ++i)
      if (nyr_has_mutable_top_var(s->as.module.body.data[i], name, depth + 1))
        return true;
  } else if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      if (nyr_has_mutable_top_var(s->as.block.body.data[i], name, depth + 1))
        return true;
  } else if (s->kind == NY_S_IF) {
    return nyr_has_mutable_top_var(s->as.iff.conseq, name, depth + 1) ||
           nyr_has_mutable_top_var(s->as.iff.alt, name, depth + 1);
  }
  return false;
}

static bool nyr_has_mutable_top_var_in_prog(const nyr_builder_impl_t *b,
                                            const program_t *prog,
                                            const char *name);

/*
 * Resolve a `use module as alias` declaration (recursively, so aliases
 * declared inside modules resolve too) to the module path.  Returns NULL
 * when no use statement declares the alias.
 */
static bool nyr_same_source_file(const char *a, const char *b) {
  if (!a || !b)
    return true;
  if (strcmp(a, b) == 0)
    return true;
  /*
   * Relative and absolute spellings of the same source are common after
   * module expansion.  Compare complete path suffixes so sibling files with
   * generic names such as mod.ny do not become the same source.
   */
  return nyr_path_ends_with(a, b) || nyr_path_ends_with(b, a);
}

/*
 * True when `filename` ends with the relative import `want` at a path
 * boundary (`.../modules/use/declared.ny` ends with `use/declared.ny`).
 */
static bool nyr_path_ends_with(const char *filename, const char *want) {
  if (!filename || !*filename || !want || !*want)
    return false;
  size_t flen = strlen(filename);
  size_t wlen = strlen(want);
  if (flen < wlen)
    return false;
  if (flen == wlen)
    return strcmp(filename, want) == 0;
  const char *tail = filename + flen - wlen;
  return strcmp(tail, want) == 0 && tail[-1] == '/';
}

/*
 * Find the module statement that a local `use "./dir/file.ny"` targets, and
 * return its declared module name.  The expanded program indexes imported
 * files by declared name (`module DeclaredModuleProvider`), not by the path
 * stored on the use node, so resolution must prefer the declared name over
 * the basename spelling the legacy path normalizer derives.
 */
static const char *nyr_module_name_for_use(const stmt_t *s,
                                           const char *basename,
                                           const char *rel, unsigned depth) {
  if (!s || depth > 64)
    return NULL;
  if (s->kind == NY_S_MODULE) {
    if (s->as.module.name) {
      if (basename && strcmp(s->as.module.name, basename) == 0)
        return s->as.module.name;
      if (rel && *rel && s->tok.filename &&
          nyr_path_ends_with(s->tok.filename, rel))
        return s->as.module.name;
    }
    for (size_t i = 0; i < s->as.module.body.len; ++i) {
      const char *found = nyr_module_name_for_use(s->as.module.body.data[i],
                                                  basename, rel, depth + 1);
      if (found)
        return found;
    }
    return NULL;
  }
  if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; ++i) {
      const char *found = nyr_module_name_for_use(s->as.block.body.data[i],
                                                  basename, rel, depth + 1);
      if (found)
        return found;
    }
  }
  return NULL;
}

/*
 * Canonicalize the module prefix of a local file import to the imported
 * module's declared name. Raw dotted package names (`core.math.float`) are
 * already canonical and pass through untouched. A trailing member suffix
 * (`./sub.ny.member`, produced for a renamed/selective import item) is kept
 * after canonicalizing the path part. Temporary spellings are retained in
 * the builder arena until lowering completes.
 *
 * Memoized on the shared builder owner: representation queries repeatedly
 * walk every use statement for the same raw spelling. Both key and result
 * strings live for the owner lifetime, so entries cannot outlive a compile.
 */
#define NY_USE_CANON_CACHE_MAX 512

static const char *
nyr_use_module_canonical_uncached(const nyr_builder_impl_t *b, const char *raw);

static const char *nyr_use_module_canonical(const nyr_builder_impl_t *b,
                                            const char *raw) {
  if (!b || !b->prog || !raw || !*raw)
    return raw;
  nyr_builder_impl_t *owner =
      b->cache_owner ? b->cache_owner : (nyr_builder_t *)b;
  owner->cache_owner = owner;
  for (size_t i = 0; i < owner->use_canon_count; ++i)
    if (strcmp(owner->use_canon_cache[i].raw, raw) == 0)
      return owner->use_canon_cache[i].canon;
  const char *canon = nyr_use_module_canonical_uncached(b, raw);
  if (canon && owner->use_canon_count < NY_USE_CANON_CACHE_MAX) {
    if (owner->use_canon_count == owner->use_canon_cap) {
      size_t next = owner->use_canon_cap ? owner->use_canon_cap * 2 : 32;
      if (next > NY_USE_CANON_CACHE_MAX)
        next = NY_USE_CANON_CACHE_MAX;
      ny_use_canon_ent_t *grown = realloc(
          owner->use_canon_cache, next * sizeof(*owner->use_canon_cache));
      if (!grown)
        return canon;
      owner->use_canon_cache = grown;
      owner->use_canon_cap = next;
    }
    const char *key = nyr_builder_own_string(b, raw);
    const char *value = nyr_builder_own_string(b, canon);
    if (!key || !value)
      return canon;
    owner->use_canon_cache[owner->use_canon_count++] =
        (ny_use_canon_ent_t){.raw = key, .canon = value};
  }
  return canon;
}

static const char *
nyr_use_module_canonical_uncached(const nyr_builder_impl_t *b,
                                  const char *raw) {
  /*
   * Callers may pass temporary qualified spellings. Keep an owned copy for
   * every lookup retained beyond this call.
   */
  const char *stable_raw = raw;
  if (raw && *raw) {
    stable_raw = nyr_builder_own_string(b, raw);
    if (!stable_raw)
      return NULL;
    raw = stable_raw;
  }
  if (!b || !b->prog || !raw || !*raw)
    return stable_raw;
  char *normalized = normalize_module_name(raw);
  const char *rel = raw;
  while (rel[0] == '.' && rel[1] == '/')
    rel += 2;
  const char *name = NULL;
  ny_native_fn_index_t *index = ny_native_fn_index_for(b);
  bool source_index_ready = index && index->modules.prog == b->prog &&
                            index->modules.source_keys_ready;
  if (source_index_ready) {
    name = ny_native_module_name_from_source_cache(&index->modules, normalized,
                                                   rel);
  } else {
    if (index && index->modules.prog == b->prog) {
      for (size_t i = 0; i < index->modules.count && !name; ++i)
        name = nyr_module_name_for_use(index->modules.modules[i], normalized,
                                       rel, 0);
    }
    for (size_t i = 0; i < b->prog->body.len && !name; ++i)
      name = nyr_module_name_for_use(b->prog->body.data[i], normalized, rel, 0);
  }
  free(normalized);
  if (name)
    return name;
  /*
   * A combined `path.member` string: canonicalize only the path part.
   */
  const char *dot = strrchr(raw, '.');
  if (dot && dot > raw + 1 && dot[1]) {
    size_t prefix_len = (size_t)(dot - raw);
    if (prefix_len < 512) {
      char *prefix = malloc(prefix_len + 1);
      if (!prefix)
        return stable_raw;
      memcpy(prefix, raw, prefix_len);
      prefix[prefix_len] = '\0';
      normalized = normalize_module_name(prefix);
      rel = prefix;
      while (rel[0] == '.' && rel[1] == '/')
        rel += 2;
      const char *mod_name = NULL;
      if (source_index_ready) {
        mod_name = ny_native_module_name_from_source_cache(&index->modules,
                                                           normalized, rel);
      } else {
        if (index && index->modules.prog == b->prog) {
          for (size_t i = 0; i < index->modules.count && !mod_name; ++i)
            mod_name = nyr_module_name_for_use(index->modules.modules[i],
                                               normalized, rel, 0);
        }
        for (size_t i = 0; i < b->prog->body.len && !mod_name; ++i)
          mod_name = nyr_module_name_for_use(b->prog->body.data[i], normalized,
                                             rel, 0);
      }
      free(normalized);
      if (mod_name) {
        const char *out = nyr_builder_own_qualified(b, mod_name, dot + 1);
        free(prefix);
        if (out)
          return out;
      } else {
        free(prefix);
      }
    }
  }
  return stable_raw;
}

static const char *nyr_use_alias_in_stmt(const nyr_builder_impl_t *b,
                                         const stmt_t *s, const char *alias,
                                         unsigned depth,
                                         const char *source_file) {
  if (!s || !alias || depth > 64)
    return NULL;
  if (s->kind == NY_S_USE && source_file && s->tok.filename &&
      !nyr_same_source_file(source_file, s->tok.filename))
    return NULL;
  if (s->kind == NY_S_USE) {
    if (s->as.use.alias && strcmp(s->as.use.alias, alias) == 0)
      return s->as.use.module;
    if (!s->as.use.alias && s->as.use.imports.len == 0 && s->as.use.module) {
      const char *dot = strrchr(s->as.use.module, '.');
      if (dot && strcmp(dot + 1, alias) == 0)
        return s->as.use.module;
    }
    for (size_t i = 0; i < s->as.use.imports.len; ++i) {
      const use_item_t *item = &s->as.use.imports.data[i];
      if (!item->alias || strcmp(item->alias, alias) != 0 ||
          !s->as.use.module || !item->name)
        continue;
      return nyr_builder_own_qualified(b, s->as.use.module, item->name);
    }
    return NULL;
  }
  if (s->kind == NY_S_MODULE) {
    for (size_t i = 0; i < s->as.module.body.len; ++i) {
      const char *found = nyr_use_alias_in_stmt(b, s->as.module.body.data[i],
                                                alias, depth + 1, source_file);
      if (found)
        return found;
    }
  }
  if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; ++i) {
      const char *found = nyr_use_alias_in_stmt(b, s->as.block.body.data[i],
                                                alias, depth + 1, source_file);
      if (found)
        return found;
    }
  }
  return NULL;
}

static const char *nyr_resolve_use_alias_uncached(const nyr_builder_impl_t *b,
                                                  const char *alias) {
  if (!b || !b->prog || !alias)
    return NULL;
  if (b->module_name && *b->module_name) {
    const stmt_t *owner = nyr_find_module_in_prog(ny_native_fn_index_for(b),
                                                  b->prog, b->module_name);
    if (owner && owner->kind == NY_S_MODULE) {
      for (size_t i = 0; i < owner->as.module.body.len; ++i) {
        const stmt_t *use = owner->as.module.body.data[i];
        if (!use || use->kind != NY_S_USE ||
            (!use->as.use.alias && use->as.use.imports.len == 0))
          continue;
        const char *found = nyr_use_alias_in_stmt(b, use, alias, 0, NULL);
        if (found) {
          if (use->as.use.alias && use->as.use.imports.len == 0 &&
              (found[0] == '.' || strchr(found, '/')))
            return nyr_builder_own_string(b, alias);
          const char *canonical = nyr_use_module_canonical(b, found);
          size_t alias_len = strlen(alias);
          if (canonical && strncmp(canonical, alias, alias_len) == 0 &&
              canonical[alias_len] == '.')
            return nyr_builder_own_string(b, alias);
          return canonical;
        }
      }
      for (size_t i = 0; i < owner->as.module.body.len; ++i) {
        const stmt_t *use = owner->as.module.body.data[i];
        if (!use || use->kind != NY_S_USE || use->as.use.alias ||
            use->as.use.imports.len != 0)
          continue;
        const char *found = nyr_use_alias_in_stmt(b, use, alias, 0, NULL);
        if (found) {
          if (use->as.use.alias && use->as.use.imports.len == 0 &&
              (found[0] == '.' || strchr(found, '/')))
            return nyr_builder_own_string(b, alias);
          const char *canonical = nyr_use_module_canonical(b, found);
          size_t alias_len = strlen(alias);
          if (canonical && strncmp(canonical, alias, alias_len) == 0 &&
              canonical[alias_len] == '.')
            return nyr_builder_own_string(b, alias);
          return canonical;
        }
      }
      /*
       * Prefer every explicitly imported module before resolving a nested
       * module exposed by a broad import such as `use core`. Otherwise a
       * short name like `error` can bind to core.error ahead of a later
       * `use core.math.crypto.error`.
       */
      for (size_t i = 0; i < owner->as.module.body.len; ++i) {
        const stmt_t *use = owner->as.module.body.data[i];
        if (!use || use->kind != NY_S_USE || use->as.use.alias ||
            use->as.use.imports.len != 0 || !use->as.use.module)
          continue;
        char prefix[512];
        int pn = snprintf(prefix, sizeof(prefix), "%s.", use->as.use.module);
        if (pn <= 0 || (size_t)pn >= sizeof(prefix))
          continue;
        const stmt_t *m = nyr_find_nested_module_in_prog(
            ny_native_fn_index_for(b), b->prog, prefix, alias);
        if (m && m->as.module.name)
          return nyr_builder_own_string(b, m->as.module.name);
      }
    }
  }
  for (size_t i = 0; i < b->prog->body.len; ++i) {
    const stmt_t *stmt = b->prog->body.data[i];
    if (!stmt || stmt->kind != NY_S_USE)
      continue;
    const char *found =
        nyr_use_alias_in_stmt(b, stmt, alias, 0, b->source_file);
    if (found) {
      if (stmt->as.use.alias && stmt->as.use.imports.len == 0 &&
          (found[0] == '.' || strchr(found, '/')))
        return nyr_builder_own_string(b, alias);
      const char *canonical = nyr_use_module_canonical(b, found);
      size_t alias_len = strlen(alias);
      if (canonical && strncmp(canonical, alias, alias_len) == 0 &&
          canonical[alias_len] == '.')
        return nyr_builder_own_string(b, alias);
      return canonical;
    }
  }
  for (size_t i = 0; i < b->prog->body.len; ++i) {
    const char *found = nyr_use_alias_in_stmt(b, b->prog->body.data[i], alias,
                                              0, b->source_file);
    if (found) {
      const char *canonical = nyr_use_module_canonical(b, found);
      size_t alias_len = strlen(alias);
      if (canonical && strncmp(canonical, alias, alias_len) == 0 &&
          canonical[alias_len] == '.')
        return nyr_builder_own_string(b, alias);
      return canonical;
    }
  }
  for (size_t i = 0; i < b->prog->body.len; ++i) {
    const stmt_t *stmt = b->prog->body.data[i];
    if (!stmt || stmt->kind != NY_S_USE || !stmt->as.use.module)
      continue;
    if (stmt->as.use.alias || stmt->as.use.imports.len != 0)
      continue;
    if (b->source_file && stmt->tok.filename &&
        !nyr_same_source_file(b->source_file, stmt->tok.filename))
      continue;
    char candidate[512];
    int n = snprintf(candidate, sizeof(candidate), "%s.%s", stmt->as.use.module,
                     alias);
    if (n > 0 && (size_t)n < sizeof(candidate)) {
      const stmt_t *m = nyr_find_module_in_prog(ny_native_fn_index_for(b),
                                                b->prog, candidate);
      if (m) {
        return nyr_builder_own_string(b, candidate);
      }
    }
    char prefix[512];
    int pn = snprintf(prefix, sizeof(prefix), "%s.", stmt->as.use.module);
    if (pn > 0 && (size_t)pn < sizeof(prefix)) {
      const stmt_t *m = nyr_find_nested_module_in_prog(
          ny_native_fn_index_for(b), b->prog, prefix, alias);
      if (m && m->as.module.name) {
        return nyr_builder_own_string(b, m->as.module.name);
      }
    }
  }
  return NULL;
}

static bool
ny_native_alias_cache_key_equal(const ny_resolve_alias_cache_entry_t *entry,
                                const char *source_file,
                                const char *module_name, const char *alias) {
  return entry->resolved &&
         ((entry->source_file == source_file) ||
          (entry->source_file && source_file &&
           strcmp(entry->source_file, source_file) == 0)) &&
         ((entry->module_name == module_name) ||
          (entry->module_name && module_name &&
           strcmp(entry->module_name, module_name) == 0)) &&
         strcmp(entry->alias, alias) == 0;
}

/*
 * Negative-prefilter for alias resolution. This is intentionally a superset:
 * it contains every explicit use alias, imported-item alias, plain-use leaf,
 * and module leaf that the scoped resolver could possibly accept. Source-file
 * and module precedence remain in the existing resolver; this index only
 * proves that a spelling cannot match any declaration and may be rejected.
 */
static uint32_t ny_native_alias_name_hash(const char *name) {
  uint32_t hash = 2166136261u;
  for (const unsigned char *p = (const unsigned char *)name; *p; ++p)
    hash = (hash ^ *p) * 16777619u;
  return hash;
}

static bool ny_native_alias_name_grow(nyr_builder_impl_t *owner,
                                      size_t min_cap) {
  size_t cap = 256;
  if (owner->resolve_alias_name_cap) {
    if (owner->resolve_alias_name_cap > SIZE_MAX / 2)
      return false;
    cap = owner->resolve_alias_name_cap * 2;
  }
  while (cap < min_cap) {
    if (cap > SIZE_MAX / 2)
      return false;
    cap *= 2;
  }
  const char **slots = calloc(cap, sizeof(*slots));
  if (!slots)
    return false;
  for (size_t i = 0; i < owner->resolve_alias_name_cap; ++i) {
    const char *name = owner->resolve_alias_name_slots[i];
    if (!name)
      continue;
    size_t slot = ny_native_alias_name_hash(name) & (cap - 1);
    while (slots[slot])
      slot = (slot + 1) & (cap - 1);
    slots[slot] = name;
  }
  free(owner->resolve_alias_name_slots);
  owner->resolve_alias_name_slots = slots;
  owner->resolve_alias_name_cap = cap;
  return true;
}

static bool ny_native_alias_name_add(nyr_builder_impl_t *owner,
                                     const char *name) {
  if (!name || !*name)
    return true;
  if (!owner->resolve_alias_name_cap && !ny_native_alias_name_grow(owner, 256))
    return false;
  size_t cap = owner->resolve_alias_name_cap;
  if (owner->resolve_alias_name_len >= cap - cap / 3) {
    if (!ny_native_alias_name_grow(owner, cap + 1))
      return false;
    cap = owner->resolve_alias_name_cap;
  }
  size_t slot = ny_native_alias_name_hash(name) & (cap - 1);
  while (owner->resolve_alias_name_slots[slot]) {
    if (strcmp(owner->resolve_alias_name_slots[slot], name) == 0)
      return true;
    slot = (slot + 1) & (cap - 1);
  }
  owner->resolve_alias_name_slots[slot] = name;
  owner->resolve_alias_name_len++;
  return true;
}

static bool ny_native_alias_name_collect(nyr_builder_impl_t *owner,
                                         const stmt_t *stmt, unsigned depth) {
  if (!stmt || depth > 64)
    return true;
  if (stmt->kind == NY_S_MODULE) {
    if (stmt->as.module.name) {
      const char *leaf = strrchr(stmt->as.module.name, '.');
      if (!ny_native_alias_name_add(owner,
                                    leaf ? leaf + 1 : stmt->as.module.name))
        return false;
    }
    for (size_t i = 0; i < stmt->as.module.body.len; ++i)
      if (!ny_native_alias_name_collect(owner, stmt->as.module.body.data[i],
                                        depth + 1))
        return false;
  } else if (stmt->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < stmt->as.block.body.len; ++i)
      if (!ny_native_alias_name_collect(owner, stmt->as.block.body.data[i],
                                        depth + 1))
        return false;
  } else if (stmt->kind == NY_S_USE) {
    if (!ny_native_alias_name_add(owner, stmt->as.use.alias))
      return false;
    if (!stmt->as.use.alias && stmt->as.use.imports.len == 0 &&
        stmt->as.use.module) {
      const char *leaf = strrchr(stmt->as.use.module, '.');
      if (!ny_native_alias_name_add(owner,
                                    leaf ? leaf + 1 : stmt->as.use.module))
        return false;
    }
    for (size_t i = 0; i < stmt->as.use.imports.len; ++i)
      if (!ny_native_alias_name_add(owner, stmt->as.use.imports.data[i].alias))
        return false;
  }
  return true;
}

static bool ny_native_alias_name_maybe(nyr_builder_impl_t *owner,
                                       const char *name) {
  if (!owner->resolve_alias_name_index_ready) {
    bool valid = owner->prog != NULL;
    for (size_t i = 0; valid && i < owner->prog->body.len; ++i)
      valid = ny_native_alias_name_collect(owner, owner->prog->body.data[i], 0);
    owner->resolve_alias_name_index_ready = true;
    owner->resolve_alias_name_index_valid = valid;
    if (!valid) {
      free(owner->resolve_alias_name_slots);
      owner->resolve_alias_name_slots = NULL;
      owner->resolve_alias_name_cap = 0;
      owner->resolve_alias_name_len = 0;
    }
  }
  if (!owner->resolve_alias_name_index_valid || !owner->resolve_alias_name_cap)
    return true;
  size_t slot =
      ny_native_alias_name_hash(name) & (owner->resolve_alias_name_cap - 1);
  while (owner->resolve_alias_name_slots[slot]) {
    if (strcmp(owner->resolve_alias_name_slots[slot], name) == 0)
      return true;
    slot = (slot + 1) & (owner->resolve_alias_name_cap - 1);
  }
  return false;
}

static bool ny_native_alias_cache_grow(nyr_builder_impl_t *owner) {
  if (!owner || owner->resolve_alias_cache_cap > SIZE_MAX / 2)
    return false;
  size_t old_cap = owner->resolve_alias_cache_cap;
  size_t new_cap = old_cap ? old_cap * 2 : NY_ALIAS_CACHE_SIZE;
  ny_resolve_alias_cache_entry_t *grown = calloc(new_cap, sizeof(*grown));
  if (!grown)
    return false;
  for (size_t i = 0; i < old_cap; ++i) {
    ny_resolve_alias_cache_entry_t entry = owner->resolve_alias_cache[i];
    if (!entry.resolved)
      continue;
    size_t slot =
        ny_alias_cache_hash(entry.source_file, entry.module_name, entry.alias) &
        (new_cap - 1);
    while (grown[slot].resolved)
      slot = (slot + 1) & (new_cap - 1);
    grown[slot] = entry;
  }
  free(owner->resolve_alias_cache);
  owner->resolve_alias_cache = grown;
  owner->resolve_alias_cache_cap = new_cap;
  return true;
}

static const char *nyr_resolve_use_alias(const nyr_builder_impl_t *b,
                                         const char *alias) {
  if (!b || !b->prog || !alias)
    return NULL;
  nyr_builder_impl_t *owner =
      b->cache_owner ? b->cache_owner : (nyr_builder_t *)b;
  if (owner->resolve_alias_stats_enabled)
    owner->resolve_alias_queries++;
  if (!ny_native_alias_name_maybe(owner, alias)) {
    if (owner->resolve_alias_stats_enabled)
      owner->resolve_alias_prefilter_rejects++;
    return NULL;
  }
  if (nyr_find_local((nyr_builder_t *)b, alias)) {
    if (owner->resolve_alias_stats_enabled)
      owner->resolve_alias_local_shadows++;
    return NULL;
  }
  owner->cache_owner = owner;
  if (!owner->resolve_alias_cache) {
    owner->resolve_alias_cache =
        calloc(NY_ALIAS_CACHE_SIZE, sizeof(*owner->resolve_alias_cache));
    if (owner->resolve_alias_cache)
      owner->resolve_alias_cache_cap = NY_ALIAS_CACHE_SIZE;
  }
  if (!owner->resolve_alias_cache) {
    if (owner->resolve_alias_stats_enabled)
      owner->resolve_alias_uncached_scans++;
    return nyr_resolve_use_alias_uncached(b, alias);
  }
  size_t cap = owner->resolve_alias_cache_cap;
  if (owner->resolve_alias_cache_len >= cap - cap * 3 / 10) {
    if (!ny_native_alias_cache_grow(owner)) {
      if (owner->resolve_alias_stats_enabled)
        owner->resolve_alias_uncached_scans++;
      return nyr_resolve_use_alias_uncached(b, alias);
    }
    cap = owner->resolve_alias_cache_cap;
  }
  size_t slot =
      ny_alias_cache_hash(b->source_file, b->module_name, alias) & (cap - 1);
  while (owner->resolve_alias_cache[slot].resolved) {
    if (ny_native_alias_cache_key_equal(&owner->resolve_alias_cache[slot],
                                        b->source_file, b->module_name,
                                        alias)) {
      if (owner->resolve_alias_stats_enabled)
        owner->resolve_alias_cache_hits++;
      return owner->resolve_alias_cache[slot].result;
    }
    slot = (slot + 1) & (cap - 1);
  }
  if (owner->resolve_alias_stats_enabled) {
    owner->resolve_alias_cache_misses++;
    owner->resolve_alias_uncached_scans++;
  }
  const char *res = nyr_resolve_use_alias_uncached(b, alias);
  const char *owned_alias = nyr_builder_own_string(owner, alias);
  if (owned_alias) {
    owner->resolve_alias_cache[slot] =
        (ny_resolve_alias_cache_entry_t){.source_file = b->source_file,
                                         .module_name = b->module_name,
                                         .alias = owned_alias,
                                         .result = res,
                                         .resolved = true,
                                         .is_null = res == NULL};
    owner->resolve_alias_cache_len++;
  }
  return res;
}

static const stmt_t *nyr_find_module(const stmt_t *s, const char *name,
                                     unsigned depth) {
  if (!s || !name || depth > 64)
    return NULL;
  if (s->kind == NY_S_MODULE) {
    if (s->as.module.name && ny_native_name_matches(s->as.module.name, name))
      return s;
    for (size_t i = 0; i < s->as.module.body.len; ++i) {
      const stmt_t *found =
          nyr_find_module(s->as.module.body.data[i], name, depth + 1);
      if (found)
        return found;
    }
  } else if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; ++i) {
      const stmt_t *found =
          nyr_find_module(s->as.block.body.data[i], name, depth + 1);
      if (found)
        return found;
    }
  }
  return NULL;
}

static const stmt_t *nyr_find_module_in_root_cached(const nyr_builder_impl_t *b,
                                                    const stmt_t *root,
                                                    const char *name) {
  if (!b || !root || !name ||
      strlen(name) >= sizeof(((ny_native_module_root_entry_t *)0)->name))
    return nyr_find_module(root, name, 0);
  ny_native_fn_index_t *index = ny_native_fn_index_for(b);
  if (!index)
    return nyr_find_module(root, name, 0);
  if (!index->module_roots)
    index->module_roots =
        calloc(NY_MODULE_ROOT_CACHE_SIZE, sizeof(*index->module_roots));
  ny_native_module_root_entry_t *cache = index->module_roots;
  uint32_t hash = ny_fn_name_hash(name);
  hash ^= (uint32_t)((uintptr_t)root >> 4);
  hash *= 16777619u;
  size_t insert = hash & (NY_MODULE_ROOT_CACHE_SIZE - 1u);
  for (uint32_t probe = 0; cache && probe < 8; ++probe) {
    size_t slot = (hash + probe) & (NY_MODULE_ROOT_CACHE_SIZE - 1u);
    ny_native_module_root_entry_t *entry = &cache[slot];
    if (!entry->resolved) {
      insert = slot;
      break;
    }
    if (entry->root == root && strcmp(entry->name, name) == 0)
      return entry->module;
  }
  const stmt_t *module = nyr_find_module(root, name, 0);
  if (cache) {
    ny_native_module_root_entry_t *entry = &cache[insert];
    entry->root = root;
    memcpy(entry->name, name, strlen(name) + 1);
    entry->module = module;
    entry->resolved = true;
  }
  return module;
}

static const stmt_t *nyr_find_nested_module(const stmt_t *s, const char *prefix,
                                            const char *leaf, unsigned depth) {
  if (!s || !prefix || !leaf || depth > 64)
    return NULL;
  if (s->kind == NY_S_MODULE) {
    if (s->as.module.name) {
      size_t prefix_len = strlen(prefix);
      if (strncmp(s->as.module.name, prefix, prefix_len) == 0) {
        const char *dot = strrchr(s->as.module.name, '.');
        if (dot && strcmp(dot + 1, leaf) == 0)
          return s;
      }
    }
    for (size_t i = 0; i < s->as.module.body.len; ++i) {
      const stmt_t *found = nyr_find_nested_module(s->as.module.body.data[i],
                                                   prefix, leaf, depth + 1);
      if (found)
        return found;
    }
  } else if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; ++i) {
      const stmt_t *found = nyr_find_nested_module(s->as.block.body.data[i],
                                                   prefix, leaf, depth + 1);
      if (found)
        return found;
    }
  }
  return NULL;
}

static const expr_t *nyr_find_module_member_in(const nyr_builder_impl_t *b,
                                               const stmt_t *mod,
                                               const char *member,
                                               unsigned depth);
static const stmt_t *nyr_find_module_walk_in_prog(const nyr_builder_impl_t *b,
                                                  const program_t *prog,
                                                  const char *name);

/*
 * Look up an already-qualified declaration without resolving imports.  This
 * is used from the import graph walk itself: calling the normal top-level
 * resolver there would start a fresh import walk and discard its visited set,
 * so cyclic re-exports could recurse indefinitely.
 */
static const expr_t *
nyr_find_qualified_value_direct(const nyr_builder_impl_t *b, const char *name) {
  if (!b || !b->prog || !name)
    return NULL;
  ny_native_tlv_ensure_build(b, b->prog);
  const nyr_builder_impl_t *owner = b->cache_owner ? b->cache_owner : b;
  if (!owner->tlv_head || !owner->tlv_ents)
    return NULL;
  uint32_t h = 2166136261u;
  for (const char *p = name; *p; ++p)
    h = (h ^ (uint8_t)*p) * 16777619u;
  for (int32_t it = owner->tlv_head[h % NY_TLV_BUCKETS]; it >= 0;
       it = owner->tlv_ents[it].next) {
    const ny_tlv_ent_t *entry = &owner->tlv_ents[it];
    if (entry->qualified && entry->val && strcmp(entry->name, name) == 0)
      return entry->val;
  }
  return NULL;
}

static const expr_t *nyr_find_module_member_in_uncached(const stmt_t *mod,
                                                        const char *member,
                                                        unsigned depth) {
  if (!mod || !member || depth > 64)
    return NULL;
  if (mod->kind == NY_S_VAR) {
    for (size_t i = 0; i < mod->as.var.names.len && i < mod->as.var.exprs.len;
         ++i) {
      const char *var_name = mod->as.var.names.data[i];
      if (var_name && (strcmp(var_name, member) == 0 ||
                       (strrchr(var_name, '.') &&
                        strcmp(strrchr(var_name, '.') + 1, member) == 0)))
        return mod->as.var.exprs.data[i];
    }
    return NULL;
  }
  if (mod->kind == NY_S_MODULE) {
    for (size_t i = 0; i < mod->as.module.body.len; ++i) {
      const expr_t *found = nyr_find_module_member_in_uncached(
          mod->as.module.body.data[i], member, depth + 1);
      if (found)
        return found;
    }
    return NULL;
  }
  if (mod->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < mod->as.block.body.len; ++i) {
      const expr_t *found = nyr_find_module_member_in_uncached(
          mod->as.block.body.data[i], member, depth + 1);
      if (found)
        return found;
    }
  }
  return NULL;
}

/*
 * Module trees are immutable during one whole-program lowering operation.
 * Imported-value and member resolution often ask for the same leaf many
 * times, so memoize only depth-zero lookups and keep entries scoped to the
 * function-index snapshot.
 */
static const expr_t *nyr_find_module_member_in(const nyr_builder_impl_t *b,
                                               const stmt_t *mod,
                                               const char *member,
                                               unsigned depth) {
  if (!mod || !member || depth > 64)
    return NULL;
  ny_native_fn_index_t *index = ny_native_fn_index_for(b);
  bool cacheable =
      depth == 0 && index && b->prog && index->functions.prog == b->prog;
  ny_native_module_member_entry_t *entries = NULL;
  size_t slot = 0;
  if (cacheable) {
    if (!index->trace_lookup_stats_initialized) {
      index->trace_lookup_stats = getenv("NYTRIX_TRACE_LOOKUPS") != NULL;
      index->trace_lookup_stats_initialized = true;
    }
    if (index->trace_lookup_stats)
      index->module_member_queries++;
    if (!index->module_members)
      index->module_members =
          calloc(NY_MODULE_MEMBER_CACHE_SIZE, sizeof(*index->module_members));
    entries = index->module_members;
    if (entries) {
      uint32_t h = ny_module_member_cache_hash(mod, member);
      size_t base = ny_module_member_cache_set(h);
      slot = base + ((h >> 12) & (NY_MODULE_MEMBER_CACHE_WAYS - 1u));
      size_t stale_slot = SIZE_MAX;
      for (size_t way = 0; way < NY_MODULE_MEMBER_CACHE_WAYS; ++way) {
        size_t candidate = base + way;
        ny_native_module_member_entry_t *entry = &entries[candidate];
        if (entry->resolved &&
            entry->generation == index->module_member_generation &&
            entry->module == mod && entry->member &&
            (entry->member == member || strcmp(entry->member, member) == 0)) {
          if (index->trace_lookup_stats)
            index->module_member_cache_hits++;
          return entry->result;
        }
        if (stale_slot == SIZE_MAX &&
            (!entry->resolved ||
             entry->generation != index->module_member_generation))
          stale_slot = candidate;
      }
      if (stale_slot != SIZE_MAX)
        slot = stale_slot;
    }
  }
  if (cacheable && index->trace_lookup_stats)
    index->module_member_tree_scans++;
  const expr_t *result = nyr_find_module_member_in_uncached(mod, member, depth);
  if (entries) {
    entries[slot] = (ny_native_module_member_entry_t){
        .module = mod,
        .member = member,
        .result = result,
        .generation = index->module_member_generation,
        .resolved = true};
  }
  return result;
}

/*
 * Resolve a constant re-exported through a module's use declarations.
 */
static const expr_t *
nyr_find_imported_module_member_rec(const nyr_builder_impl_t *b,
                                    const stmt_t *mod, const char *member,
                                    unsigned depth, const stmt_t **visited,
                                    size_t *visited_len, size_t visited_cap) {
  if (!b || !b->prog || !mod || !member || depth > 64)
    return NULL;
  for (size_t v = 0; v < *visited_len; ++v)
    if (visited[v] == mod)
      return NULL;
  if (*visited_len < visited_cap)
    visited[(*visited_len)++] = mod;
  else
    return NULL; /* Explicit bound: refuse to search past the visited cap. */
  const ny_stmt_list *body = NULL;
  if (mod->kind == NY_S_MODULE)
    body = &mod->as.module.body;
  else if (mod->kind == NY_S_BLOCK)
    body = &mod->as.block.body;
  if (!body)
    return NULL;
  for (size_t i = 0; i < body->len; ++i) {
    const stmt_t *use = body->data[i];
    if (!use || use->kind != NY_S_USE || !use->as.use.module)
      continue;
    bool imports_member =
        use->as.use.import_all || use->as.use.imports.len == 0;
    const char *target_member = member;
    for (size_t n = 0; n < use->as.use.imports.len; ++n) {
      const use_item_t *item = &use->as.use.imports.data[n];
      const char *visible = item->alias ? item->alias : item->name;
      if (visible && strcmp(visible, member) == 0) {
        imports_member = true;
        if (item->name)
          target_member = item->name;
        break;
      }
    }
    if (!imports_member)
      continue;
    const char *imported_name = use->as.use.module;
    /*
     * Direct: the imported module defines the member itself. Module-local
     * names are stored relative to that module, so inspect its body before
     * trying the flattened qualified lookup.
     */
    const stmt_t *imported_mod =
        nyr_find_module_walk_in_prog(b, b->prog, imported_name);
    const expr_t *direct =
        nyr_find_module_member_in(b, imported_mod, target_member, 0);
    if (direct)
      return direct;
    char qualified[1024];
    int qn = snprintf(qualified, sizeof(qualified), "%s.%s", imported_name,
                      target_member);
    if (qn > 0 && (size_t)qn < sizeof(qualified)) {
      const expr_t *v = nyr_find_qualified_value_direct(b, qualified);
      if (v)
        return v;
    }
    /*
     * Transitive: the imported module itself re-exports the constant.
     */
    const stmt_t *imported = nyr_find_module_in_prog(ny_native_fn_index_for(b),
                                                     b->prog, imported_name);
    if (imported && imported != mod) {
      const expr_t *value = nyr_find_imported_module_member_rec(
          b, imported, target_member, depth + 1, visited, visited_len,
          visited_cap);
      if (value)
        return value;
    }
  }
  return NULL;
}

/*
 * Resolve constants re-exported through a module's use declarations.
 */
static const expr_t *
nyr_find_imported_module_member(const nyr_builder_impl_t *b, const stmt_t *mod,
                                const char *member, unsigned depth) {
  if (!b || !b->prog || !mod || !member)
    return NULL;
  ny_native_fn_index_t *index = ny_native_fn_index_for(b);
  bool cacheable =
      depth == 0 && index && b->prog && index->functions.prog == b->prog;
  size_t slot = 0;
  if (cacheable) {
    if (!index->imported_members)
      index->imported_members =
          calloc(NY_MODULE_MEMBER_CACHE_SIZE, sizeof(*index->imported_members));
    if (index->imported_members) {
      uint32_t h = ny_module_member_cache_hash(mod, member);
      size_t base = ny_module_member_cache_set(h);
      slot = base + ((h >> 12) & (NY_MODULE_MEMBER_CACHE_WAYS - 1u));
      size_t stale_slot = SIZE_MAX;
      for (size_t way = 0; way < NY_MODULE_MEMBER_CACHE_WAYS; ++way) {
        size_t candidate = base + way;
        ny_native_imported_member_entry_t *entry =
            &index->imported_members[candidate];
        if (entry->resolved &&
            entry->generation == index->module_member_generation &&
            entry->module == mod && entry->member &&
            (entry->member == member || strcmp(entry->member, member) == 0))
          return entry->result;
        if (stale_slot == SIZE_MAX &&
            (!entry->resolved ||
             entry->generation != index->module_member_generation))
          stale_slot = candidate;
      }
      if (stale_slot != SIZE_MAX)
        slot = stale_slot;
    }
  }
  const stmt_t *visited[256];
  size_t visited_len = 0;
  const expr_t *result = nyr_find_imported_module_member_rec(
      b, mod, member, depth, visited, &visited_len, 256);
  if (cacheable && index->imported_members)
    index->imported_members[slot] = (ny_native_imported_member_entry_t){
        .module = mod,
        .member = member,
        .result = result,
        .generation = index->module_member_generation,
        .resolved = true};
  return result;
}

/*
 * Memoized answers for nyr_find_imported_value keyed by
 * (program, module, source file, name), positive and negative.  Keys are
 * compared by CONTENT: the same module/source string can be interned at
 * several addresses, and pointer keying thrashed the memo.
 */
static const expr_t *
nyr_find_imported_value_uncached(const nyr_builder_impl_t *b, const char *name);

static const expr_t *nyr_find_imported_value(const nyr_builder_impl_t *b,
                                             const char *name) {
  if (!b || !b->prog || !name)
    return NULL;
  const char *module_name = b->module_name ? b->module_name : "";
  const char *source_file = b->source_file ? b->source_file : "";
  if (strlen(name) >= sizeof(((ny_imported_val_cache_entry_t *)0)->name) ||
      strlen(module_name) >=
          sizeof(((ny_imported_val_cache_entry_t *)0)->module_name) ||
      strlen(source_file) >=
          sizeof(((ny_imported_val_cache_entry_t *)0)->source_file))
    return nyr_find_imported_value_uncached(b, name);
  nyr_builder_impl_t *owner =
      b->cache_owner ? b->cache_owner : (nyr_builder_t *)b;
  if (!owner->imported_val_cache) {
    owner->imported_val_cache =
        calloc(NY_IMPORTED_VAL_CACHE_SIZE, sizeof(*owner->imported_val_cache));
    if (owner->imported_val_cache) {
      owner->imported_val_cache_cap = NY_IMPORTED_VAL_CACHE_SIZE;
      owner->imported_val_cache_len = 0;
    }
  }
  if (!owner->imported_val_cache)
    return nyr_find_imported_value_uncached(b, name);

  /*
   * Keep this table sparse and use real open addressing. The former
   * direct-mapped insertion overwrote colliding answers, which made large
   * expanded programs repeatedly walk the complete import graph.
   */
  if ((owner->imported_val_cache_len + 1) * 10 >=
      owner->imported_val_cache_cap * 7) {
    size_t new_cap = owner->imported_val_cache_cap * 2;
    ny_imported_val_cache_entry_t *grown = calloc(new_cap, sizeof(*grown));
    if (grown) {
      for (size_t i = 0; i < owner->imported_val_cache_cap; ++i) {
        ny_imported_val_cache_entry_t *old = &owner->imported_val_cache[i];
        if (!old->resolved)
          continue;
        uint32_t old_hash = 2166136261u;
        for (const char *p = old->name; *p; ++p)
          old_hash = (old_hash ^ (uint8_t)*p) * 16777619u;
        for (const char *p = old->module_name; *p; ++p)
          old_hash = (old_hash ^ (uint8_t)*p) * 16777619u;
        for (const char *p = old->source_file; *p; ++p)
          old_hash = (old_hash ^ (uint8_t)*p) * 16777619u;
        size_t slot = old_hash % new_cap;
        while (grown[slot].resolved)
          slot = (slot + 1) % new_cap;
        grown[slot] = *old;
      }
      free(owner->imported_val_cache);
      owner->imported_val_cache = grown;
      owner->imported_val_cache_cap = new_cap;
    }
  }
  if (owner->imported_val_cache_len >= owner->imported_val_cache_cap)
    return nyr_find_imported_value_uncached(b, name);
  uint32_t h = 2166136261u;
  for (const char *p = name; *p; ++p)
    h = (h ^ (uint8_t)*p) * 16777619u;
  for (const char *p = module_name; *p; ++p)
    h = (h ^ (uint8_t)*p) * 16777619u;
  for (const char *p = source_file; *p; ++p)
    h = (h ^ (uint8_t)*p) * 16777619u;
  size_t slot = h % owner->imported_val_cache_cap;
  while (owner->imported_val_cache[slot].resolved) {
    ny_imported_val_cache_entry_t *hit = &owner->imported_val_cache[slot];
    if (hit->prog == b->prog && strcmp(hit->module_name, module_name) == 0 &&
        strcmp(hit->source_file, source_file) == 0 &&
        strcmp(hit->name, name) == 0)
      return hit->result;
    slot = (slot + 1) % owner->imported_val_cache_cap;
  }
  const expr_t *res = nyr_find_imported_value_uncached(b, name);
  ny_imported_val_cache_entry_t *entry = &owner->imported_val_cache[slot];
  entry->prog = b->prog;
  memcpy(entry->module_name, module_name, strlen(module_name) + 1);
  memcpy(entry->source_file, source_file, strlen(source_file) + 1);
  memcpy(entry->name, name, strlen(name) + 1);
  entry->result = res;
  entry->resolved = true;
  owner->imported_val_cache_len++;
  return res;
}

static const expr_t *
nyr_find_imported_value_uncached(const nyr_builder_impl_t *b,
                                 const char *name) {
  if (!b || !b->prog || !name)
    return NULL;

  if (b->module_name && b->module_name[0]) {
    const stmt_t *mod = NULL;
    mod = nyr_find_module_in_prog(ny_native_fn_index_for(b), b->prog,
                                  b->module_name);
    if (mod && mod->kind == NY_S_MODULE) {
      for (size_t i = 0; i < mod->as.module.body.len; ++i) {
        const stmt_t *use = mod->as.module.body.data[i];
        if (!use || use->kind != NY_S_USE || !use->as.use.module)
          continue;
        const char *mod_name = nyr_use_module_canonical(b, use->as.use.module);
        const stmt_t *target_mod = NULL;
        target_mod = nyr_find_module_in_prog(ny_native_fn_index_for(b), b->prog,
                                             mod_name);
        if (!target_mod)
          continue;
        for (size_t j = 0; j < use->as.use.imports.len; ++j) {
          const use_item_t *item = &use->as.use.imports.data[j];
          const char *visible = item->alias ? item->alias : item->name;
          if (!visible || strcmp(visible, name) != 0 || !item->name)
            continue;
          const expr_t *val =
              nyr_find_module_member_in(b, target_mod, item->name, 0);
          if (!val)
            val = nyr_find_imported_module_member(b, target_mod, item->name, 0);
          if (val)
            return val;
        }
        if (use->as.use.import_all ||
            (use->as.use.imports.len == 0 && !use->as.use.alias)) {
          const expr_t *val = nyr_find_module_member_in(b, target_mod, name, 0);
          if (!val)
            val = nyr_find_imported_module_member(b, target_mod, name, 0);
          if (val)
            return val;
        }
      }
    }
  }

  /*
   * Root programs keep top-level `use core.math` declarations directly in the
   * program body (rather than inside a module block).  Resolve imported
   * constants from those declarations as well as module-local uses.
   */
  for (size_t i = 0; i < b->prog->body.len; ++i) {
    const stmt_t *use = b->prog->body.data[i];
    if (!use || use->kind != NY_S_USE || !use->as.use.module)
      continue;
    const char *mod_name = nyr_use_module_canonical(b, use->as.use.module);
    const stmt_t *target_mod = NULL;
    target_mod =
        nyr_find_module_in_prog(ny_native_fn_index_for(b), b->prog, mod_name);
    if (!target_mod)
      continue;
    if (use->as.use.imports.len == 0 && !use->as.use.alias &&
        !use->as.use.import_all) {
      const expr_t *val = nyr_find_module_member_in(b, target_mod, name, 0);
      if (val)
        return val;
    }
    for (size_t j = 0; j < use->as.use.imports.len; ++j) {
      const use_item_t *item = &use->as.use.imports.data[j];
      const char *visible = item->alias ? item->alias : item->name;
      if (visible && item->name && strcmp(visible, name) == 0) {
        const expr_t *val =
            nyr_find_module_member_in(b, target_mod, item->name, 0);
        if (!val)
          val = nyr_find_imported_module_member(b, target_mod, item->name, 0);
        if (val)
          return val;
      }
    }
    if (use->as.use.import_all) {
      const expr_t *val = nyr_find_module_member_in(b, target_mod, name, 0);
      if (!val)
        val = nyr_find_imported_module_member(b, target_mod, name, 0);
      if (val)
        return val;
    }
  }

  for (size_t i = 0; i < b->prog->body.len; ++i) {
    const stmt_t *s = b->prog->body.data[i];
    if (!s)
      continue;
    if (s->kind == NY_S_BLOCK) {
      for (size_t k = 0; k < s->as.block.body.len; ++k) {
        const stmt_t *use = s->as.block.body.data[k];
        if (!use || use->kind != NY_S_USE || !use->as.use.module)
          continue;
        if (b->source_file && use->tok.filename &&
            !nyr_same_source_file(use->tok.filename, b->source_file))
          continue;
        const char *mod_name = nyr_use_module_canonical(b, use->as.use.module);
        const stmt_t *target_mod = NULL;
        target_mod = nyr_find_module_in_prog(ny_native_fn_index_for(b), b->prog,
                                             mod_name);
        if (!target_mod)
          continue;
        for (size_t j = 0; j < use->as.use.imports.len; ++j) {
          const use_item_t *item = &use->as.use.imports.data[j];
          const char *visible = item->alias ? item->alias : item->name;
          if (!visible || strcmp(visible, name) != 0 || !item->name)
            continue;
          const expr_t *val =
              nyr_find_module_member_in(b, target_mod, item->name, 0);
          if (!val)
            val = nyr_find_imported_module_member(b, target_mod, item->name, 0);
          if (val)
            return val;
        }
        if (use->as.use.import_all) {
          const expr_t *val = nyr_find_module_member_in(b, target_mod, name, 0);
          if (!val)
            val = nyr_find_imported_module_member(b, target_mod, name, 0);
          if (val)
            return val;
        }
      }
    } else if (s->kind == NY_S_USE && s->as.use.module) {
      const stmt_t *use = s;
      if (b->source_file && use->tok.filename &&
          !nyr_same_source_file(use->tok.filename, b->source_file))
        continue;
      const char *mod_name = nyr_use_module_canonical(b, use->as.use.module);
      const stmt_t *target_mod = NULL;
      target_mod =
          nyr_find_module_in_prog(ny_native_fn_index_for(b), b->prog, mod_name);
      if (!target_mod)
        continue;
      for (size_t j = 0; j < use->as.use.imports.len; ++j) {
        const use_item_t *item = &use->as.use.imports.data[j];
        const char *visible = item->alias ? item->alias : item->name;
        if (!visible || strcmp(visible, name) != 0 || !item->name)
          continue;
        const expr_t *val =
            nyr_find_module_member_in(b, target_mod, item->name, 0);
        if (!val)
          val = nyr_find_imported_module_member(b, target_mod, item->name, 0);
        if (val)
          return val;
      }
      if (use->as.use.import_all) {
        const expr_t *val = nyr_find_module_member_in(b, target_mod, name, 0);
        if (!val)
          val = nyr_find_imported_module_member(b, target_mod, name, 0);
        if (val)
          return val;
      }
    }
  }

  return NULL;
}

/*
 * Resolve a member access (gfx.WHITE) to the referenced top-level def's
 * initializer expr: alias/module path first, then a plain-name global
 * search.  Returns NULL when the member is not a constant/module value.
 */
static const expr_t *nyr_resolve_member_expr(const nyr_builder_impl_t *b,
                                             const expr_t *e) {
  if (!b || !e || e->kind != NY_E_MEMBER || !e->as.member.target ||
      !e->as.member.name)
    return NULL;

  /*
   * Flatten a (possibly nested) dotted member chain such as
   * `core.math.big._TAG_LIST` into a root identifier plus its path
   * components.  The chain is left-nested, so walking from the leaf down
   * collects [leaf, ..., component-nearest-root] and ends at the root IDENT.
   */
  const char *path[16];
  size_t npath = 0;
  const expr_t *cur = e;
  while (cur && cur->kind == NY_E_MEMBER && npath < 16) {
    path[npath++] = cur->as.member.name;
    cur = cur->as.member.target;
  }
  if (!cur || cur->kind != NY_E_IDENT || !cur->as.ident.name || npath == 0)
    return NULL;
  const char *root = cur->as.ident.name;

  /*
   * Build the module path: the root resolved through its use alias, then the
   * intermediate components in reverse order.  path[0] is the member name.
   */
  char module[512];
  const char *resolved = nyr_resolve_use_alias(b, root);
  if (!resolved) {
    nyr_builder_impl_t probe = *b;
    probe.cache_owner = b->cache_owner ? b->cache_owner : (nyr_builder_t *)b;
    probe.source_file = NULL;
    resolved = nyr_resolve_use_alias(&probe, root);
  }
  size_t mlen = (size_t)snprintf(module, sizeof(module), "%s",
                                 resolved ? resolved : root);
  if (mlen >= sizeof(module))
    return NULL;
  for (size_t i = npath; i > 1; --i) {
    int n = snprintf(module + mlen, sizeof(module) - mlen, ".%s", path[i - 1]);
    if (n < 0 || (size_t)n >= sizeof(module) - mlen)
      return NULL;
    mlen += (size_t)n;
  }

  for (size_t i = 0; i < b->prog->body.len; ++i) {
    const stmt_t *mod =
        nyr_find_module_in_root_cached(b, b->prog->body.data[i], module);
    const expr_t *v = nyr_find_module_member_in(b, mod, path[0], 0);
    if (!v)
      v = nyr_find_imported_module_member(b, mod, path[0], 0);
    if (v)
      return v;
  }

  char qualified[1024];
  int n = snprintf(qualified, sizeof(qualified), "%s.%s", module, path[0]);
  if (n > 0 && (size_t)n < sizeof(qualified)) {
    const expr_t *v = nyr_find_top_level_value(b, qualified);
    if (v)
      return v;
  }
  return nyr_find_top_level_value(b, path[0]);
}

/*
 * Resolve an expression to a constant list literal when it is one directly,
 * via a top-level def, or via a member access.  Used for .len folding and
 * index element-type detection.
 */
static bool nyr_expr_is_list(const nyr_builder_impl_t *b, const expr_t *e) {
  if (!b || !e)
    return false;
  if (e->kind == NY_E_COMPTIME && e->as.comptime_expr.body) {
    const stmt_t *body = e->as.comptime_expr.body;
    if (body->kind == NY_S_BLOCK) {
      for (size_t i = body->as.block.body.len; i-- > 0;) {
        const stmt_t *item = body->as.block.body.data[i];
        if (!item)
          continue;
        if (item->kind == NY_S_RETURN && item->as.ret.value)
          return nyr_expr_is_list(b, item->as.ret.value);
        if (item->kind == NY_S_EXPR && item->as.expr.expr)
          return nyr_expr_is_list(b, item->as.expr.expr);
      }
    } else if (body->kind == NY_S_RETURN && body->as.ret.value) {
      return nyr_expr_is_list(b, body->as.ret.value);
    } else if (body->kind == NY_S_EXPR && body->as.expr.expr) {
      return nyr_expr_is_list(b, body->as.expr.expr);
    }
  }
  if (e->kind == NY_E_IDENT && e->as.ident.name &&
      (!b->current_fn_name || strcmp(b->current_fn_name, "rt_main") == 0 ||
       strcmp(b->current_fn_name, "_ny_top_entry") == 0)) {
    const stmt_t *decl = nyr_find_top_level_var_stmt(b, e->as.ident.name);
    if (decl && decl->kind == NY_S_VAR) {
      for (size_t i = 0; i < decl->as.var.names.len; ++i) {
        const char *name = decl->as.var.names.data[i];
        if (name && strcmp(name, e->as.ident.name) == 0 &&
            i < decl->as.var.exprs.len && decl->as.var.exprs.data[i] &&
            decl->as.var.exprs.data[i]->kind == NY_E_COMPTIME)
          return nyr_expr_is_list(b, decl->as.var.exprs.data[i]);
      }
    }
  }
  /*
   * Indexing yields an element, never the container's representation.  The
   * semantic pass may leave the container's typed-buffer rep on the index
   * node; trusting it makes `xs[i] + ys[i]` look like list concatenation.
   */
  if (e->kind == NY_E_INDEX)
    return false;
  if (e->kind == NY_E_CALL) {
    const char *leaf = ny_native_call_leaf(e);
    if (leaf && strcmp(leaf, "set") == 0 && e->as.call.args.len >= 1 &&
        e->as.call.args.data[0].val &&
        nyr_expr_is_dict(b, e->as.call.args.data[0].val))
      return false;
    if (leaf && strcmp(leaf, "get") == 0)
      return false;
  }
  if (e->kind == NY_E_MEMCALL && e->as.memcall.name &&
      strcmp(e->as.memcall.name, "set") == 0 && e->as.memcall.target &&
      nyr_expr_is_dict(b, e->as.memcall.target))
    return false;
  if (e->kind == NY_E_LIST)
    return true;
  /*
   * The semantic pass records the canonical declaration for attached and
   * qualified calls. Prefer that declaration over the surface-name lookup
   * below: module aliases such as `it.repeat(...)` must retain their declared
   * sequence result just like an unqualified user call.
   */
  if (e->semantic.canonical_callee_stmt &&
      e->semantic.canonical_callee_stmt->kind == NY_S_FUNC &&
      ny_native_type_name_is_list(
          e->semantic.canonical_callee_stmt->as.fn.return_type))
    return true;
  if (e->semantic.canonical_callee) {
    const stmt_t *fn = ny_native_fn_cache_lookup_exact_with_builder(
        b, e->semantic.canonical_callee);
    if (fn && ny_native_type_name_is_list(fn->as.fn.return_type))
      return true;
  }
  if (e->semantic.resolved && e->semantic.rep == NY_SEM_REP_TYPED_BUFFER) {
    /*
     * list, tuple, and set share the native typed-buffer carrier, but they
     * are different language types with different attached-method sets.  Do
     * not infer `list` from the carrier alone.
     */
    if (e->kind == NY_E_IDENT && e->as.ident.name) {
      nyr_local_t *local = nyr_find_local((nyr_builder_t *)b, e->as.ident.name);
      if (local && local->is_list)
        return true;
      if (!local) {
        const expr_t *global =
            e->tok.filename ? nyr_find_top_level_value_in_source(
                                  b, e->as.ident.name, e->tok.filename)
                            : nyr_find_top_level_value(b, e->as.ident.name);
        if (!global)
          global = nyr_find_top_level_value(b, e->as.ident.name);
        for (unsigned depth = 0;
             global && global->kind == NY_E_IDENT && depth < 64; ++depth) {
          const char *name = global->as.ident.name;
          if (!name || (global == e && depth == 0))
            break;
          const expr_t *next = global->tok.filename
                                   ? nyr_find_top_level_value_in_source(
                                         b, name, global->tok.filename)
                                   : nyr_find_top_level_value(b, name);
          if (!next)
            next = nyr_find_top_level_value(b, name);
          if (!next || next == global || next == e)
            break;
          global = next;
        }
        if (global && global != e && global->kind != NY_E_IDENT)
          return nyr_expr_is_list(b, global);
      }
    }
    const stmt_t *fn = e->semantic.canonical_callee_stmt;
    if (!fn && e->kind == NY_E_CALL && e->as.call.callee &&
        e->as.call.callee->kind == NY_E_IDENT &&
        e->as.call.callee->as.ident.name)
      fn = nyr_find_user_function((nyr_builder_t *)b,
                                  e->as.call.callee->as.ident.name);
    if (!fn && e->kind == NY_E_MEMCALL && e->as.memcall.target &&
        e->as.memcall.target->kind == NY_E_IDENT && e->as.memcall.name) {
      const char *alias = e->as.memcall.target->as.ident.name;
      const char *module =
          alias ? nyr_resolve_use_alias((nyr_builder_t *)b, alias) : NULL;
      if (module) {
        char qualified[512];
        int n = snprintf(qualified, sizeof(qualified), "%s.%s", module,
                         e->as.memcall.name);
        if (n > 0 && (size_t)n < sizeof(qualified)) {
          fn = nyr_find_user_function((nyr_builder_t *)b, qualified);
          if (!fn)
            fn = nyr_find_imported_function((nyr_builder_t *)b, qualified);
        }
      }
    }
    if (fn && fn->as.fn.return_type)
      return ny_native_type_name_is_list(fn->as.fn.return_type);
    if (e->kind == NY_E_LIST)
      return true;
    return false;
  }
  if (e->kind == NY_E_BINARY && e->as.binary.op &&
      strcmp(e->as.binary.op, "*") == 0) {
    bool left_list = nyr_expr_is_list(b, e->as.binary.left);
    bool right_list = nyr_expr_is_list(b, e->as.binary.right);
    return left_list != right_list;
  }
  if (e->kind == NY_E_CALL) {
    const char *leaf = ny_native_call_leaf(e);
    if (leaf &&
        (strcmp(leaf, "args") == 0 || strcmp(leaf, "get_terminal_size") == 0) &&
        e->as.call.args.len == 0)
      return true;
    if (leaf && strcmp(leaf, "list") == 0)
      return true;
    if (leaf && (strcmp(leaf, "borrow") == 0 || strcmp(leaf, "own") == 0) &&
        e->as.call.args.len == 1)
      return nyr_expr_is_list(b, e->as.call.args.data[0].val);
    if (leaf &&
        (strcmp(leaf, "sort") == 0 || strcmp(leaf, "sorted") == 0 ||
         strcmp(leaf, "reverse") == 0 || strcmp(leaf, "clone") == 0) &&
        e->as.call.args.len >= 1)
      return nyr_expr_is_list(b, e->as.call.args.data[0].val);
    const char *call_name =
        e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT
            ? e->as.call.callee->as.ident.name
            : leaf;
    const stmt_t *fn =
        call_name ? nyr_find_user_function((nyr_builder_t *)b, call_name)
                  : NULL;
    if (!fn && leaf)
      fn = nyr_find_user_function((nyr_builder_t *)b, leaf);
    return fn && ny_native_type_name_is_list(fn->as.fn.return_type);
  }
  if (e->kind == NY_E_MEMCALL) {
    if (e->as.memcall.name && (strcmp(e->as.memcall.name, "slice") == 0 ||
                               strcmp(e->as.memcall.name, "append") == 0 ||
                               strcmp(e->as.memcall.name, "extend") == 0 ||
                               strcmp(e->as.memcall.name, "push") == 0 ||
                               strcmp(e->as.memcall.name, "concat") == 0 ||
                               strcmp(e->as.memcall.name, "insert") == 0 ||
                               strcmp(e->as.memcall.name, "reverse") == 0 ||
                               strcmp(e->as.memcall.name, "sort") == 0 ||
                               strcmp(e->as.memcall.name, "sorted") == 0 ||
                               strcmp(e->as.memcall.name, "clear") == 0))
      return true;
    if (e->as.memcall.name && strcmp(e->as.memcall.name, "clone") == 0)
      return nyr_expr_is_list(b, e->as.memcall.target) &&
             !nyr_expr_is_dict(b, e->as.memcall.target);
    if (e->as.memcall.name && strcmp(e->as.memcall.name, "get") == 0 &&
        e->as.memcall.args.len == 2)
      return nyr_expr_is_list(b, e->as.memcall.args.data[1].val);
    const stmt_t *fn = e->semantic.canonical_callee_stmt;
    if (!fn && e->as.memcall.name)
      fn = nyr_find_user_function((nyr_builder_t *)b, e->as.memcall.name);
    /*
     * A namespace call is represented as a memcall whose target is the
     * imported alias (`it.repeat`). Resolve that alias before the leaf-name
     * fallback; otherwise an overloaded `repeat` can resolve to str.repeat
     * and lose the declared list result.
     */
    if (!fn && e->as.memcall.target &&
        e->as.memcall.target->kind == NY_E_IDENT && e->as.memcall.name) {
      const char *alias = e->as.memcall.target->as.ident.name;
      const char *module =
          alias ? nyr_resolve_use_alias((nyr_builder_t *)b, alias) : NULL;
      if (module) {
        char qualified[512];
        int n = snprintf(qualified, sizeof(qualified), "%s.%s", module,
                         e->as.memcall.name);
        if (n > 0 && (size_t)n < sizeof(qualified)) {
          fn = nyr_find_user_function((nyr_builder_t *)b, qualified);
          if (!fn)
            fn = nyr_find_imported_function((nyr_builder_t *)b, qualified);
        }
      }
    }
    return fn && ny_native_type_name_is_list(fn->as.fn.return_type);
  }
  if (e->kind == NY_E_MEMBER) {
    const expr_t *global = nyr_resolve_member_expr(b, e);
    return global && global != e && nyr_expr_is_list(b, global);
  }
  if (e->kind == NY_E_IDENT) {
    nyr_local_t *local = nyr_find_local((nyr_builder_t *)b, e->as.ident.name);
    if (local) {
      return local->is_list;
    }
    const expr_t *global = e->tok.filename
                               ? nyr_find_top_level_value_in_source(
                                     b, e->as.ident.name, e->tok.filename)
                               : nyr_find_top_level_value(b, e->as.ident.name);
    if (!global)
      global = nyr_find_top_level_value(b, e->as.ident.name);
    /*
     * Follow top-level aliases before classifying the value.  A global
     * `mut alias = source` has no local fact table entry, so stopping at the
     * intermediate identifier loses the container ABI for printing,
     * indexing, calls, and ownership decisions.  Bound the walk to keep
     * malformed cyclic definitions from turning type probing into recursion.
     */
    for (unsigned depth = 0; global && global->kind == NY_E_IDENT && depth < 64;
         ++depth) {
      const char *name = global->as.ident.name;
      if (!name || (global == e && depth == 0))
        break;
      const expr_t *next = global->tok.filename
                               ? nyr_find_top_level_value_in_source(
                                     b, name, global->tok.filename)
                               : nyr_find_top_level_value(b, name);
      if (!next)
        next = nyr_find_top_level_value(b, name);
      if (!next || next == global || next == e)
        break;
      global = next;
    }
    if (global && global != e && global->kind != NY_E_IDENT)
      return nyr_expr_is_list(b, global);
    return false;
  }
  return false;
}

/*
 * Bytes share the string semantic representation, but their methods have a
 * different native ABI. Keep the distinction explicit so `b.get`/`b.set`
 * cannot fall through to the generic dictionary dispatcher.
 */
static bool nyr_expr_is_bytes(const nyr_builder_impl_t *b, const expr_t *e) {
  if (!b || !e)
    return false;
  if (e->kind == NY_E_CALL) {
    const char *leaf = ny_native_call_leaf(e);
    if (leaf &&
        (strcmp(leaf, "bytes") == 0 || strcmp(leaf, "__bytes_new") == 0 ||
         strcmp(leaf, "bytes_new_raw") == 0))
      return true;
    const stmt_t *fn =
        leaf ? nyr_find_user_function((nyr_builder_t *)b, leaf) : NULL;
    return fn && ny_native_type_name_is_bytes(fn->as.fn.return_type);
  }
  if (e->kind == NY_E_MEMCALL) {
    if (e->as.memcall.name &&
        (strcmp(e->as.memcall.name, "bytes") == 0 ||
         strcmp(e->as.memcall.name, "__bytes_new") == 0 ||
         strcmp(e->as.memcall.name, "bytes_new_raw") == 0))
      return true;
    if (e->as.memcall.name && strcmp(e->as.memcall.name, "set") == 0)
      return (e->semantic.resolved && e->semantic.rep == NY_SEM_REP_STRING) ||
             nyr_expr_is_bytes(b, e->as.memcall.target);
    const stmt_t *fn =
        e->as.memcall.name
            ? nyr_find_user_function((nyr_builder_t *)b, e->as.memcall.name)
            : NULL;
    return fn && ny_native_type_name_is_bytes(fn->as.fn.return_type);
  }
  if (e->kind == NY_E_IDENT) {
    nyr_local_t *local = nyr_find_local((nyr_builder_t *)b, e->as.ident.name);
    if (local)
      return local->is_bytes;
    const expr_t *global = nyr_find_top_level_value(b, e->as.ident.name);
    return global && global != e && global->kind != NY_E_IDENT &&
           nyr_expr_is_bytes(b, global);
  }
  if (e->kind == NY_E_MEMBER) {
    const expr_t *global = nyr_resolve_member_expr(b, e);
    return global && global != e && nyr_expr_is_bytes(b, global);
  }
  return false;
}

static bool nyr_expr_is_ptr(const nyr_builder_impl_t *b, const expr_t *e) {
  if (!b || !e)
    return false;
  if (e->semantic.resolved && e->semantic.rep == NY_SEM_REP_POINTER)
    return true;
  if (e->kind == NY_E_CALL) {
    const char *leaf = ny_native_call_leaf(e);
    if (leaf &&
        (strcmp(leaf, "malloc") == 0 || strcmp(leaf, "malloc_raw") == 0 ||
         strcmp(leaf, "__malloc") == 0 || strcmp(leaf, "__malloc_raw") == 0 ||
         strcmp(leaf, "rt_malloc") == 0 || strcmp(leaf, "rt_zalloc_raw") == 0 ||
         strcmp(leaf, "realloc") == 0 || strcmp(leaf, "__realloc") == 0 ||
         strcmp(leaf, "__ptr_add") == 0 || strcmp(leaf, "__ptr_sub") == 0))
      return true;
    const stmt_t *fn =
        leaf ? nyr_find_user_function((nyr_builder_t *)b, leaf) : NULL;
    return fn && ny_native_type_name_is_ptr(fn->as.fn.return_type);
  }
  if (e->kind == NY_E_MEMCALL) {
    if (e->as.memcall.name && strcmp(e->as.memcall.name, "set") == 0 &&
        e->as.memcall.target && nyr_expr_is_dict(b, e->as.memcall.target))
      return true;
    const stmt_t *fn =
        e->as.memcall.name
            ? nyr_find_user_function((nyr_builder_t *)b, e->as.memcall.name)
            : NULL;
    return fn && ny_native_type_name_is_ptr(fn->as.fn.return_type);
  }
  if (e->kind == NY_E_IDENT) {
    nyr_local_t *local = nyr_find_local((nyr_builder_t *)b, e->as.ident.name);
    if (local)
      return local->semantic_rep == NY_SEM_REP_POINTER ||
             ny_native_type_name_is_ptr(local->type_name);
    const expr_t *global = nyr_find_top_level_value(b, e->as.ident.name);
    return global && global != e && global->kind != NY_E_IDENT &&
           nyr_expr_is_ptr(b, global);
  }
  if (e->kind == NY_E_MEMBER) {
    const expr_t *global = nyr_resolve_member_expr(b, e);
    return global && global != e && nyr_expr_is_ptr(b, global);
  }
  return false;
}

static bool nyr_expr_is_dict(const nyr_builder_impl_t *b, const expr_t *e) {
  if (!b || !e)
    return false;
  /*
   * A dictionary lookup returns `any`; semantic object propagation on the
   * call node must not make a nested list/string lookup look like another
   * dictionary to the attached-method fast path.
   */
  if (e->kind == NY_E_CALL && ny_native_call_leaf(e) &&
      (strcmp(ny_native_call_leaf(e), "get") == 0 ||
       strcmp(ny_native_call_leaf(e), "dict_get") == 0))
    return false;
  if (e->kind == NY_E_DICT)
    return true;
  if (e->kind == NY_E_CALL) {
    const char *leaf = ny_native_call_leaf(e);
    /*
     * `dict.set` is a persistent-style expression that returns the same
     * dictionary handle.  Preserve that container fact through a chained
     * call (`dict(...).set(...).set(...)`); otherwise the second call can be
     * mistaken for a typed-buffer store and silently lose its dictionary
     * update.
     */
    if (leaf && strcmp(leaf, "set") == 0 && e->as.call.args.len >= 1 &&
        e->as.call.args.data[0].val &&
        nyr_expr_is_dict(b, e->as.call.args.data[0].val))
      return true;
    if (leaf && strcmp(leaf, "clone") == 0 && e->as.call.args.len == 1)
      return nyr_expr_is_dict(b, e->as.call.args.data[0].val);
    /*
     * `dict(cap)` is the language container constructor.  Module flattening
     * can hide its user-function declaration from the local symbol lookup,
     * but its result representation is still unambiguously a dictionary;
     * keep indexed writes on the dictionary ABI rather than typed-buffer
     * storage.
     */
    if (leaf && strcmp(leaf, "dict") == 0)
      return true;
    if (leaf && (strcmp(leaf, "borrow") == 0 || strcmp(leaf, "own") == 0) &&
        e->as.call.args.len == 1)
      return nyr_expr_is_dict(b, e->as.call.args.data[0].val);
    const stmt_t *fn =
        leaf ? nyr_find_user_function((nyr_builder_t *)b, leaf) : NULL;
    return fn && fn->as.fn.return_type &&
           strcmp(fn->as.fn.return_type, "dict") == 0;
  }
  if (e->kind == NY_E_MEMCALL) {
    if (e->as.memcall.name && strcmp(e->as.memcall.name, "set") == 0 &&
        e->as.memcall.target && nyr_expr_is_dict(b, e->as.memcall.target))
      return true;
    const stmt_t *fn =
        e->as.memcall.name
            ? nyr_find_user_function((nyr_builder_t *)b, e->as.memcall.name)
            : NULL;
    if (fn && fn->as.fn.return_type &&
        strcmp(fn->as.fn.return_type, "dict") == 0)
      return true;
    const expr_t *global = nyr_resolve_member_expr(b, e);
    return global && global != e && nyr_expr_is_dict(b, global);
  }
  if (e->kind == NY_E_IDENT) {
    nyr_local_t *local = nyr_find_local((nyr_builder_t *)b, e->as.ident.name);
    /*
     * Sets share the object representation with dictionaries, but their
     * member names overlap (`delete`/`remove`) while their runtime layouts do
     * not. Preserve the declared container identity before the broad object
     * representation fallback below.
     * A tagged `any` local may hold either a dict or a set. Its generic
     * object carrier is not proof of dictionary layout; choosing a dict
     * intrinsic here skips the runtime set branch in any.remove/delete.
     */
    if (local && local->type_name && strcmp(local->type_name, "set") == 0)
      return false;
    if (local && (local->semantic_rep == NY_SEM_REP_OBJECT || local->is_dict))
      return true;
    /*
     * The type registry globals start as `nil` and are populated with
     * dictionaries by `_types_init`.  Their declaration therefore carries no
     * useful container representation, but all accesses are dictionary
     * operations.  Keep that backend-known invariant explicit so `.set` and
     * `.get` do not fall through to the typed-buffer ABI.
     */
    if (!local && e->as.ident.name &&
        (!strcmp(e->as.ident.name, "TYPE_NAMES") ||
         !strcmp(e->as.ident.name, "TYPE_ALIASES") ||
         !strcmp(e->as.ident.name, "TYPE_GROUPS")))
      return true;
    const expr_t *global = nyr_find_top_level_value(b, e->as.ident.name);
    /*
     * Follow immutable-style global aliases to their initializer.  Stopping
     * at the first identifier made `mut alias = dict_value` lose its dict
     * representation, so reads through the alias used the generic tagged
     * index helper while reads through the original used the dict ABI.
     */
    for (unsigned depth = 0; global && depth < 16; ++depth) {
      if (global == e)
        return false;
      if (global->kind != NY_E_IDENT)
        return nyr_expr_is_dict(b, global);
      if (!global->as.ident.name)
        return false;
      global = nyr_find_top_level_value(b, global->as.ident.name);
    }
  }
  return false;
}

static bool nyr_expr_is_set(const nyr_builder_impl_t *b, const expr_t *e) {
  if (!b || !e)
    return false;
  if (e->kind == NY_E_CALL || e->kind == NY_E_MEMCALL) {
    const char *leaf = ny_native_call_leaf(e);
    if (e->kind == NY_E_CALL && leaf && strcmp(leaf, "set") == 0 &&
        e->as.call.args.len <= 1)
      return true;
    const stmt_t *canonical = e->semantic.canonical_callee_stmt;
    if (canonical && canonical->kind == NY_S_FUNC &&
        canonical->as.fn.return_type &&
        strcmp(canonical->as.fn.return_type, "set") == 0)
      return true;
    if (e->semantic.canonical_callee) {
      const stmt_t *fn = nyr_find_user_function((nyr_builder_t *)b,
                                                e->semantic.canonical_callee);
      if (fn && fn->as.fn.return_type &&
          strcmp(fn->as.fn.return_type, "set") == 0)
        return true;
    }
    const stmt_t *fn =
        leaf ? nyr_find_user_function((nyr_builder_t *)b, leaf) : NULL;
    return fn && fn->as.fn.return_type &&
           strcmp(fn->as.fn.return_type, "set") == 0;
  }
  if (e->kind == NY_E_IDENT && e->as.ident.name) {
    const nyr_local_t *local =
        nyr_find_local((nyr_builder_t *)b, e->as.ident.name);
    if (local && local->type_name && strcmp(local->type_name, "set") == 0)
      return true;
    const expr_t *global = e->tok.filename
                               ? nyr_find_top_level_value_in_source(
                                     b, e->as.ident.name, e->tok.filename)
                               : nyr_find_top_level_value(b, e->as.ident.name);
    if (!global)
      global = nyr_find_top_level_value(b, e->as.ident.name);
    return global && global != e && nyr_expr_is_set(b, global);
  }
  return false;
}

static bool nyr_expr_is_tuple(const nyr_builder_impl_t *b, const expr_t *e) {
  if (!b || !e)
    return false;
  if (e->kind == NY_E_TUPLE)
    return true;
  if (e->kind == NY_E_CALL) {
    const char *leaf = ny_native_call_leaf(e);
    if (leaf && strcmp(leaf, "tuple") == 0)
      return true;
    if (leaf && strcmp(leaf, "clone") == 0 && e->as.call.args.len == 1)
      return nyr_expr_is_tuple(b, e->as.call.args.data[0].val);
    const stmt_t *fn =
        leaf ? nyr_find_user_function((nyr_builder_t *)b, leaf) : NULL;
    return fn && fn->as.fn.return_type &&
           strcmp(fn->as.fn.return_type, "tuple") == 0;
  }
  if (e->kind == NY_E_IDENT && e->as.ident.name) {
    const nyr_local_t *local =
        nyr_find_local((nyr_builder_t *)b, e->as.ident.name);
    if (local && local->type_name && strcmp(local->type_name, "tuple") == 0)
      return true;
    const expr_t *global = e->tok.filename
                               ? nyr_find_top_level_value_in_source(
                                     b, e->as.ident.name, e->tok.filename)
                               : nyr_find_top_level_value(b, e->as.ident.name);
    if (!global)
      global = nyr_find_top_level_value(b, e->as.ident.name);
    return global && global != e && nyr_expr_is_tuple(b, global);
  }
  return false;
}

static const expr_t *nyr_resolve_list_literal(const nyr_builder_impl_t *b,
                                              const expr_t *e, unsigned depth) {
  if (!b || !e || depth > 64)
    return NULL;
  if (e->kind == NY_E_LIST)
    return e;
  if (e->kind == NY_E_IDENT) {
    nyr_local_t *local = nyr_find_local((nyr_builder_t *)b, e->as.ident.name);
    if (local && !local->semantic_mutable && local->list_literal)
      return local->list_literal;
    if (b->prog &&
        nyr_has_mutable_top_var_in_prog(b, b->prog, e->as.ident.name))
      return NULL;
    const stmt_t *top_stmt = nyr_find_top_level_var_stmt(b, e->as.ident.name);
    if (top_stmt && top_stmt->kind == NY_S_VAR && top_stmt->as.var.is_mut)
      return NULL;
    const expr_t *v = nyr_find_top_level_value(b, e->as.ident.name);
    if (v && v != e)
      return nyr_resolve_list_literal(b, v, depth + 1);
    return NULL;
  }
  if (e->kind == NY_E_MEMBER) {
    const expr_t *v = nyr_resolve_member_expr(b, e);
    if (v && v != e)
      return nyr_resolve_list_literal(b, v, depth + 1);
    return NULL;
  }
  return NULL;
}

static const expr_t *nyr_resolve_dict_literal(const nyr_builder_impl_t *b,
                                              const expr_t *e, unsigned depth) {
  if (!b || !e || depth > 64)
    return NULL;
  if (e->kind == NY_E_DICT)
    return e;
  if (e->kind == NY_E_IDENT) {
    nyr_local_t *local = nyr_find_local((nyr_builder_t *)b, e->as.ident.name);
    if (local && !local->semantic_mutable && local->dict_literal)
      return local->dict_literal;
    if (b->prog &&
        nyr_has_mutable_top_var_in_prog(b, b->prog, e->as.ident.name))
      return NULL;
    const stmt_t *top_stmt = nyr_find_top_level_var_stmt(b, e->as.ident.name);
    if (top_stmt && top_stmt->kind == NY_S_VAR && top_stmt->as.var.is_mut)
      return NULL;
    const expr_t *v = nyr_find_top_level_value(b, e->as.ident.name);
    if (v && v != e)
      return nyr_resolve_dict_literal(b, v, depth + 1);
    return NULL;
  }
  if (e->kind == NY_E_MEMBER) {
    const expr_t *v = nyr_resolve_member_expr(b, e);
    if (v && v != e)
      return nyr_resolve_dict_literal(b, v, depth + 1);
    return NULL;
  }
  return NULL;
}

static bool nyr_dict_literal_all_keys_constant(const expr_t *dict_lit) {
  if (!dict_lit || dict_lit->kind != NY_E_DICT)
    return false;
  for (size_t i = 0; i < dict_lit->as.dict.pairs.len; ++i) {
    const expr_t *k = dict_lit->as.dict.pairs.data[i].key;
    if (!k || k->kind != NY_E_LITERAL)
      return false;
    if (k->as.literal.kind != NY_LIT_STR &&
        (k->as.literal.kind != NY_LIT_INT || k->tok.kind == NY_T_NIL))
      return false;
  }
  return true;
}

/*
 * Only a fresh, literal receiver can be folded: a def-bound dictionary may
 * still be mutated through an alias.  Require scalar literal slots so the
 * selected value has the same stable dynamic representation as construction.
 */
static bool nyr_dict_literal_get_foldable(const expr_t *dict_lit) {
  if (!nyr_dict_literal_all_keys_constant(dict_lit))
    return false;
  for (size_t i = 0; i < dict_lit->as.dict.pairs.len; ++i) {
    const expr_t *key = dict_lit->as.dict.pairs.data[i].key;
    const expr_t *value = dict_lit->as.dict.pairs.data[i].value;
    if (key->as.literal.kind == NY_LIT_STR &&
        (!key->as.literal.as.s.data ||
         memchr(key->as.literal.as.s.data, '\0', key->as.literal.as.s.len) !=
             NULL))
      return false;
    if (key->as.literal.kind == NY_LIT_INT &&
        (key->as.literal.as.i <= -(INT64_C(1) << 62) ||
         key->as.literal.as.i >= (INT64_C(1) << 62)))
      return false;
    if (!value || value->kind != NY_E_LITERAL ||
        (value->as.literal.kind != NY_LIT_BOOL &&
         (value->as.literal.kind != NY_LIT_INT || value->tok.kind == NY_T_NIL)))
      return false;
    if (value->as.literal.kind == NY_LIT_INT &&
        (value->as.literal.as.i <= -(INT64_C(1) << 62) ||
         value->as.literal.as.i >= (INT64_C(1) << 62)))
      return false;
  }
  return true;
}

static const expr_t *nyr_dict_literal_lookup(const expr_t *dict_lit,
                                             const char *key_str,
                                             int64_t key_int, bool is_int_key,
                                             bool *found) {
  if (found)
    *found = false;
  if (!dict_lit || dict_lit->kind != NY_E_DICT)
    return NULL;
  for (size_t i = dict_lit->as.dict.pairs.len; i > 0; --i) {
    const expr_t *k = dict_lit->as.dict.pairs.data[i - 1].key;
    const expr_t *val = dict_lit->as.dict.pairs.data[i - 1].value;
    if (!k)
      continue;
    if (is_int_key) {
      if (k->kind == NY_E_LITERAL && k->as.literal.kind == NY_LIT_INT &&
          k->tok.kind != NY_T_NIL && k->as.literal.as.i == key_int) {
        if (found)
          *found = true;
        return val;
      }
    } else if (key_str) {
      if (k->kind == NY_E_LITERAL && k->as.literal.kind == NY_LIT_STR &&
          k->as.literal.as.s.data &&
          strcmp(k->as.literal.as.s.data, key_str) == 0) {
        if (found)
          *found = true;
        return val;
      }
    }
  }
  return NULL;
}

static bool nyr_expr_is_raw_int(const expr_t *e) {
  if (!e)
    return false;
  if (e->kind == NY_E_UNARY && e->as.unary.op &&
      (strcmp(e->as.unary.op, "+") == 0 || strcmp(e->as.unary.op, "-") == 0 ||
       strcmp(e->as.unary.op, "~") == 0))
    return nyr_expr_is_raw_int(e->as.unary.right);
  if (e->kind == NY_E_LITERAL && e->as.literal.kind == NY_LIT_INT &&
      e->tok.kind != NY_T_NIL)
    return true;
  const char *leaf = NULL;
  if (e->kind == NY_E_CALL && e->as.call.callee &&
      e->as.call.callee->kind == NY_E_IDENT && e->as.call.callee->as.ident.name)
    leaf = ny_name_leaf(e->as.call.callee->as.ident.name);
  else if (e->kind == NY_E_MEMCALL && e->as.memcall.name)
    leaf = ny_name_leaf(e->as.memcall.name);
  if (leaf) {
    /*
     * Native lowering gives the raw ABI to the scalar shift intrinsics even
     * though their source declarations remain dynamically typed.
     */
    if (leaf && (strcmp(leaf, "bshr") == 0 || strcmp(leaf, "__shr") == 0 ||
                 strcmp(leaf, "bshl") == 0 || strcmp(leaf, "__shl") == 0))
      return true;
  }
  return e->semantic.resolved && e->semantic.rep == NY_SEM_REP_RAW_INT;
}

/*
 * Fold a top-level def initializer to a raw i64 when it is a compile-time
 * constant (literals, references to other top-level defs, integer unary and
 * binary ops).  Used to register the def's value in the consttab so object
 * emission writes an 8-byte .data definition for the symbol.
 */
static bool nyr_fold_top_level_int(const nyr_builder_impl_t *b,
                                   const program_t *prog, const expr_t *e,
                                   int64_t *out, unsigned depth) {
  if (!e || !out || depth > 64)
    return false;
  if (!prog)
    prog = b ? b->prog : NULL;
  if (!prog)
    return false;
  if (!b) {
    nyr_builder_impl_t temporary = {.prog = prog};
    temporary.cache_owner = &temporary;
    bool result = nyr_fold_top_level_int(&temporary, prog, e, out, depth);
    nyr_builder_dispose(&temporary);
    return result;
  }
  prog = b->prog;
  switch (e->kind) {
  case NY_E_LITERAL:
    if (e->as.literal.kind == NY_LIT_BOOL) {
      *out = e->as.literal.as.b ? 1 : 0;
      return true;
    }
    if (e->as.literal.kind == NY_LIT_INT && e->tok.kind != NY_T_NIL) {
      *out = e->as.literal.as.i;
      return true;
    }
    return false;
  case NY_E_IDENT: {
    if (!e->as.ident.name)
      return false;
    /*
     * A visible local shadows the same-named top-level def; folding the
     * global's initializer here evaluated the wrong binding (a local `step`
     * inside a stdlib function folded to the user program's `step = 0`).
     * The builder is NULL only for consttab registration, which runs
     * outside any function scope.
     * See the eval_proof_i64 twin above: locals shadow same-named defs.
     */
    if (b && (nyr_find_local((nyr_builder_t *)b, e->as.ident.name) ||
              nyr_find_local_nearest((nyr_builder_t *)b, e->as.ident.name)))
      return false;
    /*
     * Mutable module variables also have entries in the top-level value
     * index, but those entries describe only their initializer.  A call
     * argument or arithmetic expression that reads such a binding must keep
     * its runtime load; folding it here freezes loop-carried state.
     */
    if (b) {
      const stmt_t *binding =
          e->tok.filename ? nyr_find_top_level_var_in_source_in_prog(
                                b, prog, e->as.ident.name, e->tok.filename)
                          : NULL;
      if (!binding)
        binding = nyr_find_top_level_var_in_prog(b, prog, e->as.ident.name);
      if (binding && binding->kind == NY_S_VAR && binding->as.var.is_mut)
        return false;
    }
    ny_native_tlv_ensure_build(b, prog);
    const nyr_builder_impl_t *owner = b->cache_owner ? b->cache_owner : b;
    uint32_t h = 2166136261u;
    for (const char *p = e->as.ident.name; *p; ++p)
      h = (h ^ (uint8_t)*p) * 16777619u;
    h %= NY_TLV_BUCKETS;
    for (int32_t it = owner->tlv_head ? owner->tlv_head[h] : -1; it >= 0;
         it = owner->tlv_ents[it].next) {
      if (strcmp(owner->tlv_ents[it].name, e->as.ident.name) != 0)
        continue;
      if (owner->tlv_ents[it].is_mut)
        return false;
      const expr_t *val = owner->tlv_ents[it].val;
      if (val && val != e)
        return nyr_fold_top_level_int(b, prog, val, out, depth + 1);
    }
    return false;
  }
  case NY_E_UNARY: {
    if (!e->as.unary.op || !e->as.unary.right)
      return false;
    int64_t v = 0;
    if (!nyr_fold_top_level_int(b, prog, e->as.unary.right, &v, depth + 1))
      return false;
    if (strcmp(e->as.unary.op, "+") == 0) {
      *out = v;
      return true;
    }
    if (strcmp(e->as.unary.op, "-") == 0) {
      if (v == INT64_MIN)
        return false;
      *out = -v;
      return true;
    }
    if (strcmp(e->as.unary.op, "!") == 0) {
      *out = v ? 0 : 1;
      return true;
    }
    return false;
  }
  case NY_E_BINARY: {
    if (!e->as.binary.op || !e->as.binary.left || !e->as.binary.right)
      return false;
    if (strcmp(e->as.binary.op, "**") == 0)
      return nyr_fold_const_pow(e->as.binary.left, e->as.binary.right, out);
    int64_t l = 0, r = 0;
    if (!nyr_fold_top_level_int(b, prog, e->as.binary.left, &l, depth + 1) ||
        !nyr_fold_top_level_int(b, prog, e->as.binary.right, &r, depth + 1))
      return false;
    nyr_op_t op;
    if (!nyr_binop(e->as.binary.op, &op))
      return false;
    switch (op) {
    case NYIR_ADD_I64:
      return !__builtin_add_overflow(l, r, out);
    case NYIR_SUB_I64:
      return !__builtin_sub_overflow(l, r, out);
    case NYIR_MUL_I64:
      return !__builtin_mul_overflow(l, r, out);
    case NYIR_DIV_I64:
      if (r == 0 || (l == INT64_MIN && r == -1))
        return false;
      *out = l / r;
      return true;
    case NYIR_MOD_I64:
      if (r == 0 || (l == INT64_MIN && r == -1))
        return false;
      *out = l % r;
      return true;
    case NYIR_AND_I64:
      *out = l & r;
      return true;
    case NYIR_OR_I64:
      *out = l | r;
      return true;
    case NYIR_XOR_I64:
      *out = l ^ r;
      return true;
    case NYIR_SHL_I64:
      if (r < 0 || r >= 64)
        return false;
      *out = (int64_t)((uint64_t)l << (unsigned)r);
      return true;
    case NYIR_SAR_I64:
      if (r < 0 || r >= 64)
        return false;
      *out = l >> (unsigned)r;
      return true;
    default:
      return false;
    }
  }
  case NY_E_COMPTIME: {
    const stmt_t *body = e->as.comptime_expr.body;
    if (body && body->kind == NY_S_BLOCK && body->as.block.body.len == 1)
      body = body->as.block.body.data[0];
    const expr_t *value = body && body->kind == NY_S_RETURN ? body->as.ret.value
                          : body && body->kind == NY_S_EXPR ? body->as.expr.expr
                                                            : NULL;
    if (value && value->kind == NY_E_CALL && value->as.call.args.len == 0 &&
        value->as.call.callee && value->as.call.callee->kind == NY_E_IDENT &&
        value->as.call.callee->as.ident.name &&
        strcmp(value->as.call.callee->as.ident.name, "__main") == 0) {
      *out = b->source_main_file && value->tok.filename &&
             nyr_same_source_file(value->tok.filename, b->source_main_file);
      return true;
    }
    return value && value != e &&
           nyr_fold_top_level_int(b, prog, value, out, depth + 1);
  }
  default:
    return false;
  }
}

/*
 * Fold a module initializer with lexical ownership preserved.  The ordinary
 * folder intentionally accepts bare names for root-program constants, but a
 * module initializer must resolve `FLAG` as `owner.FLAG`; otherwise an
 * unrelated same-named export encountered earlier in the global index can
 * silently change the value.
 */
static const char *nyr_module_for_initializer_expr_stmt(const stmt_t *s,
                                                        const expr_t *e,
                                                        const char *module_name,
                                                        unsigned depth) {
  if (!s || !e || depth > 64)
    return NULL;
  if (s->kind == NY_S_MODULE) {
    const char *owner = s->as.module.name ? s->as.module.name : module_name;
    if (s->as.module.name && s->tok.filename && e->tok.filename &&
        strcmp(s->tok.filename, e->tok.filename) == 0)
      return s->as.module.name;
    for (size_t i = 0; i < s->as.module.body.len; ++i) {
      const char *found = nyr_module_for_initializer_expr_stmt(
          s->as.module.body.data[i], e, owner, depth + 1);
      if (found)
        return found;
    }
    return NULL;
  }
  if (s->kind == NY_S_VAR) {
    for (size_t i = 0; i < s->as.var.exprs.len; ++i)
      if (s->as.var.exprs.data[i] == e)
        return module_name;
  } else if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; ++i) {
      const char *found = nyr_module_for_initializer_expr_stmt(
          s->as.block.body.data[i], e, module_name, depth + 1);
      if (found)
        return found;
    }
  } else if (s->kind == NY_S_IF) {
    const char *found = nyr_module_for_initializer_expr_stmt(
        s->as.iff.conseq, e, module_name, depth + 1);
    if (found)
      return found;
    return nyr_module_for_initializer_expr_stmt(s->as.iff.alt, e, module_name,
                                                depth + 1);
  }
  return NULL;
}

static const char *nyr_module_for_initializer_expr(const program_t *prog,
                                                   const expr_t *e) {
  if (!prog || !e)
    return NULL;
  for (size_t i = 0; i < prog->body.len; ++i) {
    const char *found =
        nyr_module_for_initializer_expr_stmt(prog->body.data[i], e, NULL, 0);
    if (found)
      return found;
  }
  return NULL;
}

static bool nyr_fold_top_level_int_in_module(const nyr_builder_impl_t *b,
                                             const program_t *prog,
                                             const char *module_name,
                                             const expr_t *e, int64_t *out,
                                             unsigned depth) {
  if (!e || !out || depth > 64)
    return false;
  if (!b) {
    if (!prog)
      return false;
    nyr_builder_impl_t temporary = {.prog = prog};
    temporary.cache_owner = &temporary;
    bool result = nyr_fold_top_level_int_in_module(&temporary, prog,
                                                   module_name, e, out, depth);
    nyr_builder_dispose(&temporary);
    return result;
  }
  prog = b->prog;
  if (!prog)
    return false;
  if (e->kind == NY_E_MEMBER) {
    const expr_t *initializer = nyr_resolve_member_expr(b, e);
    if (!initializer || initializer == e)
      return false;
    /*
     * Member resolution exposes initial storage for type inspection too.
     * Its initializer is not a constant when the defining binding is mutable.
     */
    ny_native_tlv_ensure_build(b, prog);
    const nyr_builder_impl_t *cache = b->cache_owner ? b->cache_owner : b;
    const char *owner = nyr_module_for_initializer_expr(prog, initializer);
    uint32_t h = 2166136261u;
    for (const char *p = e->as.member.name; *p; ++p)
      h = (h ^ (uint8_t)*p) * 16777619u;
    uint32_t qualified_h = 2166136261u;
    if (owner) {
      for (const char *p = owner; *p; ++p)
        qualified_h = (qualified_h ^ (uint8_t)*p) * 16777619u;
      qualified_h = (qualified_h ^ (uint8_t)'.') * 16777619u;
      for (const char *p = e->as.member.name; *p; ++p)
        qualified_h = (qualified_h ^ (uint8_t)*p) * 16777619u;
    }
    for (unsigned scope = 0; scope < (owner ? 2u : 1u); ++scope) {
      uint32_t bucket = (scope ? qualified_h : h) % NY_TLV_BUCKETS;
      if (scope && bucket == h % NY_TLV_BUCKETS)
        continue;
      for (int32_t it = cache->tlv_head ? cache->tlv_head[bucket] : -1; it >= 0;
           it = cache->tlv_ents[it].next) {
        const ny_tlv_ent_t *entry = &cache->tlv_ents[it];
        if (entry->val == initializer && entry->is_mut) {
          if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
            ny_dbg_heuristic_event(
                "native-constant",
                "function=%s member=%s action=keep-runtime-load "
                "reason=mutable-module-binding",
                b->current_fn_name ? b->current_fn_name : "<entry>",
                e->as.member.name);
          return false;
        }
      }
    }
    return nyr_fold_top_level_int_in_module(
        b, prog, owner ? owner : module_name, initializer, out, depth + 1);
  }
  if (e->kind == NY_E_IDENT && e->as.ident.name && module_name &&
      *module_name) {
    /*
     * Runtime locals shadow module constants before qualified lookup, just
     * as they do in the unqualified folder.
     */
    if (nyr_find_expr_local((nyr_builder_t *)b, e))
      return false;
    /*
     * The top-level value index also contains `mut` initializers so clients
     * can inspect their declared type and initial storage.  That initializer
     * is never a compile-time value: replacing a later load with it freezes
     * loop-carried globals at their first value (for example foo(counter)
     * inside a loop).  Refuse the fold when this identifier names mutable
     * module storage, preferring its source file to avoid same-name imports.
     */
    const stmt_t *binding =
        e->tok.filename ? nyr_find_top_level_var_in_source_in_prog(
                              b, prog, e->as.ident.name, e->tok.filename)
                        : NULL;
    if (!binding)
      binding = nyr_find_top_level_var_in_prog(b, prog, e->as.ident.name);
    if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
      ny_dbg_heuristic_event(
          "native-constant",
          "function=%s identifier=%s module=%s binding=%s mutable=%s "
          "action=%s",
          b->current_fn_name ? b->current_fn_name : "<entry>", e->as.ident.name,
          module_name, binding ? "found" : "missing",
          binding && binding->kind == NY_S_VAR && binding->as.var.is_mut ? "yes"
                                                                         : "no",
          binding && binding->kind == NY_S_VAR && binding->as.var.is_mut
              ? "keep-runtime-load"
              : "inspect-initializer");
    if (binding && binding->kind == NY_S_VAR && binding->as.var.is_mut)
      return false;
    ny_native_tlv_ensure_build(b, prog);
    const nyr_builder_impl_t *owner = b->cache_owner ? b->cache_owner : b;
    char qualified[512];
    int n = (strchr(e->as.ident.name, '.') != NULL)
                ? snprintf(qualified, sizeof(qualified), "%s", e->as.ident.name)
                : snprintf(qualified, sizeof(qualified), "%s.%s", module_name,
                           e->as.ident.name);
    if (n > 0 && (size_t)n < sizeof(qualified)) {
      uint32_t h = 2166136261u;
      for (const char *p = qualified; *p; ++p)
        h = (h ^ (uint8_t)*p) * 16777619u;
      h %= NY_TLV_BUCKETS;
      for (int32_t it = owner->tlv_head ? owner->tlv_head[h] : -1; it >= 0;
           it = owner->tlv_ents[it].next) {
        if (strcmp(owner->tlv_ents[it].name, qualified) == 0 &&
            owner->tlv_ents[it].val != e) {
          if (owner->tlv_ents[it].is_mut) {
            if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
              ny_dbg_heuristic_event(
                  "native-constant",
                  "function=%s identifier=%s action=keep-runtime-load "
                  "reason=mutable-index-entry",
                  b->current_fn_name ? b->current_fn_name : "<entry>",
                  qualified);
            return false;
          }
          return nyr_fold_top_level_int_in_module(
              b, prog, module_name, owner->tlv_ents[it].val, out, depth + 1);
        }
      }
    }
    return nyr_fold_top_level_int(b, prog, e, out, depth);
  }
  if (e->kind == NY_E_UNARY && e->as.unary.op && e->as.unary.right) {
    int64_t v = 0;
    if (!nyr_fold_top_level_int_in_module(b, prog, module_name,
                                          e->as.unary.right, &v, depth + 1))
      return false;
    if (strcmp(e->as.unary.op, "+") == 0)
      *out = v;
    else if (strcmp(e->as.unary.op, "-") == 0) {
      if (v == INT64_MIN)
        return false;
      *out = -v;
    } else if (strcmp(e->as.unary.op, "!") == 0)
      *out = v ? 0 : 1;
    else
      return false;
    return true;
  }
  if (e->kind == NY_E_BINARY && e->as.binary.op && e->as.binary.left &&
      e->as.binary.right) {
    int64_t l = 0, r = 0;
    if (!nyr_fold_top_level_int_in_module(b, prog, module_name,
                                          e->as.binary.left, &l, depth + 1) ||
        !nyr_fold_top_level_int_in_module(b, prog, module_name,
                                          e->as.binary.right, &r, depth + 1))
      return false;
    nyr_op_t op;
    if (!nyr_binop(e->as.binary.op, &op))
      return false;
    switch (op) {
    case NYIR_ADD_I64:
      return !__builtin_add_overflow(l, r, out);
    case NYIR_SUB_I64:
      return !__builtin_sub_overflow(l, r, out);
    case NYIR_MUL_I64:
      return !__builtin_mul_overflow(l, r, out);
    case NYIR_DIV_I64:
      if (r == 0 || (l == INT64_MIN && r == -1))
        return false;
      *out = l / r;
      return true;
    case NYIR_MOD_I64:
      if (r == 0 || (l == INT64_MIN && r == -1))
        return false;
      *out = l % r;
      return true;
    case NYIR_AND_I64:
      *out = l & r;
      return true;
    case NYIR_OR_I64:
      *out = l | r;
      return true;
    case NYIR_XOR_I64:
      *out = l ^ r;
      return true;
    case NYIR_SHL_I64:
      if (r < 0 || r >= 64)
        return false;
      *out = (int64_t)((uint64_t)l << (unsigned)r);
      return true;
    case NYIR_SAR_I64:
      if (r < 0 || r >= 64)
        return false;
      *out = l >> (unsigned)r;
      return true;
    default:
      return false;
    }
  }
  return nyr_fold_top_level_int(b, prog, e, out, depth);
}

/*
 * Fold a call to __runtime_tag("name") / runtime_tag_raw("name") with a
 * string-literal argument to its tag constant.  Shared by the call-site
 * lowering and top-level consttab registration.
 */
static bool nyr_fold_runtime_tag(const expr_t *call, int64_t *out) {
  if (!call || !out || call->kind != NY_E_CALL)
    return false;
  const char *leaf = ny_native_call_leaf(call);
  if (!leaf || (strcmp(leaf, "__runtime_tag") != 0 &&
                strcmp(leaf, "runtime_tag_raw") != 0))
    return false;
  if (call->as.call.args.len != 1 || call->as.call.args.data[0].name ||
      !call->as.call.args.data[0].val ||
      call->as.call.args.data[0].val->kind != NY_E_LITERAL ||
      call->as.call.args.data[0].val->as.literal.kind != NY_LIT_STR)
    return false;
  const char *s = call->as.call.args.data[0].val->as.literal.as.s.data;
  size_t n = call->as.call.args.data[0].val->as.literal.as.s.len;
  if (strcmp(leaf, "runtime_tag_raw") == 0)
    *out = rt_runtime_tag_raw_name(s, n);
  else
    *out = rt_runtime_tag_raw_name(s, n);
  return true;
}

/*
 * Conservative integer evaluator for compile-time proof builtins in the NYIR
 * path. It accepts scalar and zero-field enum constants, recursively checked
 * lemma calls, and user functions whose bodies reduce through expressions,
 * returns, matches, and conditionals. Unsupported statements and unknown
 * values remain unknown instead of being guessed true.
 */
static bool nyr_find_enum_member(const nyr_builder_impl_t *b, const char *name,
                                 const stmt_t **out_enum,
                                 const stmt_enum_item_t **out_item,
                                 int64_t *out_tag);

static bool nyr_eval_proof_enum_member(const nyr_builder_impl_t *b,
                                       const expr_t *e, const stmt_t **out_enum,
                                       int64_t *out_tag) {
  if (!b || !e || !out_enum || !out_tag)
    return false;
  char name[512];
  if (!nyr_qualified_expr(e, name, sizeof(name)))
    return false;
  const stmt_t *enum_stmt = NULL;
  const stmt_enum_item_t *item = NULL;
  int64_t tag = 0;
  if (!nyr_find_enum_member(b, name, &enum_stmt, &item, &tag) || !enum_stmt ||
      !item || item->fields.len != 0)
    return false;
  *out_enum = enum_stmt;
  *out_tag = tag;
  return true;
}

typedef struct {
  const char *const *names;
  const int64_t *values;
  size_t len;
} nyr_proof_eval_env_t;

static bool nyr_eval_proof_stmt_i64_env(const nyr_builder_impl_t *b,
                                        const stmt_t *stmt, unsigned depth,
                                        int64_t *out,
                                        const nyr_proof_eval_env_t *env);
static bool nyr_eval_proof_i64_env(const nyr_builder_impl_t *b, const expr_t *e,
                                   unsigned depth, int64_t *out,
                                   const nyr_proof_eval_env_t *env);

static bool nyr_eval_proof_lemma_call(const nyr_builder_impl_t *b,
                                      const expr_t *call, unsigned depth,
                                      int64_t *out,
                                      const nyr_proof_eval_env_t *outer_env);

static bool
nyr_eval_proof_function_call(const nyr_builder_impl_t *b, const expr_t *call,
                             unsigned depth, int64_t *out,
                             const nyr_proof_eval_env_t *outer_env) {
  if (!b || !call || call->kind != NY_E_CALL || !out || depth >= 64)
    return false;
  const stmt_t *fn = call->semantic.canonical_callee_stmt;
  if (!fn || fn->kind != NY_S_FUNC) {
    const char *name = call->semantic.canonical_callee;
    if (!name && call->as.call.callee &&
        call->as.call.callee->kind == NY_E_IDENT)
      name = call->as.call.callee->as.ident.name;
    if (!name)
      return false;
    fn = nyr_find_user_function((nyr_builder_t *)b, name);
  }
  if (!fn || fn->kind != NY_S_FUNC || !fn->as.fn.body || fn->as.fn.is_extern)
    return false;
  const ny_param_list *params = &fn->as.fn.params;
  const ny_call_arg_list *args = &call->as.call.args;
  if (args->len != params->len || args->len > 64)
    return false;
  const char *names[64];
  int64_t values[64];
  for (size_t i = 0; i < args->len; ++i) {
    if (args->data[i].name || !params->data[i].name ||
        !nyr_eval_proof_i64_env(b, args->data[i].val, depth + 1, &values[i],
                                outer_env))
      return false;
    names[i] = params->data[i].name;
  }
  const nyr_proof_eval_env_t env = {names, values, args->len};
  return nyr_eval_proof_stmt_i64_env(b, fn->as.fn.body, depth + 1, out, &env);
}

static bool nyr_eval_proof_i64_env(const nyr_builder_impl_t *b, const expr_t *e,
                                   unsigned depth, int64_t *out,
                                   const nyr_proof_eval_env_t *env) {
  if (!b || !e || !out || depth > 64)
    return false;
  if (e->kind == NY_E_LITERAL) {
    if (e->as.literal.kind == NY_LIT_INT) {
      if (e->as.literal.integer_outside_i64)
        return false;
      *out = e->as.literal.as.i;
      return true;
    }
    if (e->as.literal.kind == NY_LIT_BOOL) {
      *out = e->as.literal.as.b ? 1 : 0;
      return true;
    }
    return false;
  }
  const stmt_t *enum_stmt = NULL;
  int64_t enum_tag = 0;
  if (nyr_eval_proof_enum_member(b, e, &enum_stmt, &enum_tag)) {
    *out = enum_tag;
    return true;
  }
  if (e->kind == NY_E_IDENT && e->as.ident.name) {
    for (size_t i = 0; env && i < env->len; ++i) {
      if (env->names[i] && strcmp(e->as.ident.name, env->names[i]) == 0) {
        *out = env->values[i];
        return true;
      }
    }
    /*
     * A visible local shadows any same-named top-level def.  Proof folding
     * once replaced reflect.len's local `step` with the merged program's
     * top-level `step = 0`, folding `(-step)` into a constant-zero divisor
     * that the NYIR verifier (rightly) rejects.
     * A visible local shadows any same-named top-level def.  Proof folding
     * once replaced reflect.len's local `step` with the merged program's
     * top-level `step = 0`, folding `(-step)` into a constant-zero divisor
     * that the NYIR verifier (rightly) rejects.
     *
     * A visible local shadows the top-level def only when it belongs to the
     * identifier's own syntax context.  Flattened stdlib bodies introduce
     * same-named locals (loop counters named `i`) into the shared builder;
     * treating those as shadows made top-level scalars like `def int i = 4`
     * unprovable and silently passed assert_compile_index rejections.
     */
    nyr_local_t *shadow = nyr_find_local_ctx(
        (nyr_builder_t *)b, e->as.ident.name, e->as.ident.syntax_ctx);
    if (shadow) {
      /*
       * A top-level immutable def in rt_main binds a module cell plus this
       * local mirror, but only the cell is ever stored, so the compile-time
       * value is the declaration's initializer.  Restricted to rt_main:
       * folding a function-local shadow from the merged program's top-level
       * initializer of the same name is exactly the reflect.len `step`
       * regression.
       */
      if (b->profile_name && strcmp(b->profile_name, "rt_main") == 0) {
        const stmt_t *decl = NULL;
        if (e->tok.filename && b->prog) {
          for (size_t i = 0; i < b->prog->body.len; ++i) {
            decl = nyr_find_top_level_var_in_source_stmt(
                b->prog->body.data[i], e->as.ident.name, e->tok.filename, 0);
            if (decl)
              break;
          }
        }
        if (!decl)
          decl = nyr_find_top_level_var_stmt(b, e->as.ident.name);
        if (decl && decl->kind == NY_S_VAR && !decl->as.var.is_mut) {
          for (size_t n = 0; n < decl->as.var.names.len; ++n) {
            if (!decl->as.var.names.data[n] ||
                strcmp(decl->as.var.names.data[n], e->as.ident.name) != 0)
              continue;
            const expr_t *init =
                n < decl->as.var.exprs.len ? decl->as.var.exprs.data[n] : NULL;
            if (init)
              return nyr_eval_proof_i64_env(b, init, depth + 1, out, env);
          }
        }
      }
      /*
       * A mutable function local is a runtime value: whatever constant its
       * slot currently holds says nothing about later iterations or paths,
       * so the fold must refuse (loop-index soundness).  Only the rt_main
       * immutable module-cell mirror above may fold.
       */
      return false;
    }
    /*
     * Prefer the same source file when resolving the identifier: the plain
     * index lookup can return a same-named module-level binding from the
     * flattened stdlib, which made typed top-level scalars (`def int i = 4`)
     * unprovable and silently passed assert_compile_index rejections.
     */
    const expr_t *value = e->tok.filename
                              ? nyr_find_top_level_value_in_source(
                                    b, e->as.ident.name, e->tok.filename)
                              : NULL;
    if (!value)
      value = nyr_find_top_level_value(b, e->as.ident.name);
    return value && value != e &&
           nyr_eval_proof_i64_env(b, value, depth + 1, out, env);
  }
  if (e->kind == NY_E_UNARY && e->as.unary.op) {
    int64_t value = 0;
    if (!nyr_eval_proof_i64_env(b, e->as.unary.right, depth + 1, &value, env))
      return false;
    if (strcmp(e->as.unary.op, "!") == 0)
      *out = !value;
    else if (strcmp(e->as.unary.op, "+") == 0)
      *out = value;
    else if (strcmp(e->as.unary.op, "-") == 0) {
      if (value == INT64_MIN)
        return false;
      *out = -value;
    } else
      return false;
    return true;
  }
  if (e->kind == NY_E_MATCH) {
    int64_t test = 0;
    if (!e->as.match.test ||
        !nyr_eval_proof_i64_env(b, e->as.match.test, depth + 1, &test, env))
      return false;
    for (size_t i = 0; i < e->as.match.arms.len; ++i) {
      const match_arm_t *arm = &e->as.match.arms.data[i];
      bool matched = false;
      for (size_t j = 0; j < arm->patterns.len; ++j) {
        int64_t pattern = 0;
        if (!nyr_eval_proof_i64_env(b, arm->patterns.data[j], depth + 1,
                                    &pattern, env))
          return false;
        if (pattern == test) {
          matched = true;
          break;
        }
      }
      if (!matched)
        continue;
      if (arm->guard) {
        int64_t guard = 0;
        if (!nyr_eval_proof_i64_env(b, arm->guard, depth + 1, &guard, env))
          return false;
        if (!guard)
          continue;
      }
      return arm->conseq &&
             nyr_eval_proof_stmt_i64_env(b, arm->conseq, depth + 1, out, env);
    }
    return e->as.match.default_conseq &&
           nyr_eval_proof_stmt_i64_env(b, e->as.match.default_conseq, depth + 1,
                                       out, env);
  }
  if (e->kind == NY_E_CALL) {
    if (nyr_eval_proof_lemma_call(b, e, depth, out, env))
      return true;
    return nyr_eval_proof_function_call(b, e, depth, out, env);
  }
  const char *op = NULL;
  const expr_t *left = NULL, *right = NULL;
  if (e->kind == NY_E_BINARY) {
    op = e->as.binary.op;
    left = e->as.binary.left;
    right = e->as.binary.right;
  } else if (e->kind == NY_E_LOGICAL) {
    op = e->as.logical.op;
    left = e->as.logical.left;
    right = e->as.logical.right;
  }
  if (!op || !left || !right)
    return false;
  if (e->kind == NY_E_BINARY &&
      (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0)) {
    const stmt_t *left_enum = NULL, *right_enum = NULL;
    int64_t left_tag = 0, right_tag = 0;
    if (nyr_eval_proof_enum_member(b, left, &left_enum, &left_tag) &&
        nyr_eval_proof_enum_member(b, right, &right_enum, &right_tag)) {
      if (left_enum != right_enum)
        return false;
      bool equal = left_tag == right_tag;
      *out = strcmp(op, "==") == 0 ? equal : !equal;
      return true;
    }
  }
  int64_t a = 0, c = 0;
  if (!nyr_eval_proof_i64_env(b, left, depth + 1, &a, env) ||
      !nyr_eval_proof_i64_env(b, right, depth + 1, &c, env))
    return false;
  if (strcmp(op, "+") == 0) {
    if ((c > 0 && a > INT64_MAX - c) || (c < 0 && a < INT64_MIN - c))
      return false;
    *out = a + c;
  } else if (strcmp(op, "-") == 0) {
    if ((c < 0 && a > INT64_MAX + c) || (c > 0 && a < INT64_MIN + c))
      return false;
    *out = a - c;
  } else if (strcmp(op, "*") == 0) {
    if (a != 0 &&
        (c == INT64_MIN || (c > 0 ? (a > INT64_MAX / c || a < INT64_MIN / c)
                                  : (a == INT64_MIN || -a > INT64_MAX / -c))))
      return false;
    *out = a * c;
  } else if (strcmp(op, "/") == 0) {
    if (c == 0 || (a == INT64_MIN && c == -1))
      return false;
    *out = a / c;
  } else if (strcmp(op, "%") == 0) {
    if (c == 0 || (a == INT64_MIN && c == -1))
      return false;
    *out = a % c;
  } else if (strcmp(op, "==") == 0)
    *out = a == c;
  else if (strcmp(op, "!=") == 0)
    *out = a != c;
  else if (strcmp(op, "<") == 0)
    *out = a < c;
  else if (strcmp(op, "<=") == 0)
    *out = a <= c;
  else if (strcmp(op, ">") == 0)
    *out = a > c;
  else if (strcmp(op, ">=") == 0)
    *out = a >= c;
  else if (strcmp(op, "&&") == 0)
    *out = a && c;
  else if (strcmp(op, "||") == 0)
    *out = a || c;
  else
    return false;
  return true;
}

static bool nyr_eval_proof_stmt_i64_env(const nyr_builder_impl_t *b,
                                        const stmt_t *stmt, unsigned depth,
                                        int64_t *out,
                                        const nyr_proof_eval_env_t *env) {
  if (!b || !stmt || !out || depth > 64)
    return false;
  switch (stmt->kind) {
  case NY_S_RETURN:
    return stmt->as.ret.value &&
           nyr_eval_proof_i64_env(b, stmt->as.ret.value, depth + 1, out, env);
  case NY_S_EXPR:
    return stmt->as.expr.expr &&
           nyr_eval_proof_i64_env(b, stmt->as.expr.expr, depth + 1, out, env);
  case NY_S_BLOCK: {
    bool has_value = false;
    for (size_t i = 0; i < stmt->as.block.body.len; ++i) {
      const stmt_t *child = stmt->as.block.body.data[i];
      if (!child)
        return false;
      if (!nyr_eval_proof_stmt_i64_env(b, child, depth + 1, out, env))
        return false;
      has_value = true;
      if (child->kind == NY_S_RETURN)
        return true;
    }
    return has_value;
  }
  case NY_S_IF: {
    int64_t condition = 0;
    if (stmt->as.iff.init || !stmt->as.iff.test ||
        !nyr_eval_proof_i64_env(b, stmt->as.iff.test, depth + 1, &condition,
                                env))
      return false;
    const stmt_t *branch = condition ? stmt->as.iff.conseq : stmt->as.iff.alt;
    return branch &&
           nyr_eval_proof_stmt_i64_env(b, branch, depth + 1, out, env);
  }
  case NY_S_MATCH: {
    int64_t test = 0;
    if (!stmt->as.match.test ||
        !nyr_eval_proof_i64_env(b, stmt->as.match.test, depth + 1, &test, env))
      return false;
    for (size_t i = 0; i < stmt->as.match.arms.len; ++i) {
      const match_arm_t *arm = &stmt->as.match.arms.data[i];
      bool matched = false;
      for (size_t j = 0; j < arm->patterns.len; ++j) {
        int64_t pattern = 0;
        if (!nyr_eval_proof_i64_env(b, arm->patterns.data[j], depth + 1,
                                    &pattern, env))
          return false;
        if (pattern == test) {
          matched = true;
          break;
        }
      }
      if (!matched)
        continue;
      if (arm->guard) {
        int64_t guard = 0;
        if (!nyr_eval_proof_i64_env(b, arm->guard, depth + 1, &guard, env))
          return false;
        if (!guard)
          continue;
      }
      return arm->conseq &&
             nyr_eval_proof_stmt_i64_env(b, arm->conseq, depth + 1, out, env);
    }
    return stmt->as.match.default_conseq &&
           nyr_eval_proof_stmt_i64_env(b, stmt->as.match.default_conseq,
                                       depth + 1, out, env);
  }
  default:
    return false;
  }
}

static bool nyr_eval_proof_i64(const nyr_builder_impl_t *b, const expr_t *e,
                               unsigned depth, int64_t *out) {
  return nyr_eval_proof_i64_env(b, e, depth, out, NULL);
}

static void nyr_find_lemma_exact_in_stmt(const stmt_t *stmt, const char *name,
                                         const stmt_t **found, bool *ambiguous,
                                         unsigned depth) {
  if (!stmt || !name || !found || !ambiguous || depth > 64 || *ambiguous)
    return;
  if (stmt->kind == NY_S_LEMMA && stmt->as.lemma.name &&
      ny_native_name_matches(stmt->as.lemma.name, name)) {
    if (*found && *found != stmt)
      *ambiguous = true;
    else
      *found = stmt;
    return;
  }
  if (stmt->kind == NY_S_IF) {
    nyr_find_lemma_exact_in_stmt(stmt->as.iff.conseq, name, found, ambiguous,
                                 depth + 1);
    nyr_find_lemma_exact_in_stmt(stmt->as.iff.alt, name, found, ambiguous,
                                 depth + 1);
    return;
  }
  const ny_stmt_list *body = stmt->kind == NY_S_MODULE  ? &stmt->as.module.body
                             : stmt->kind == NY_S_BLOCK ? &stmt->as.block.body
                                                        : NULL;
  for (size_t i = 0; body && i < body->len && !*ambiguous; ++i)
    nyr_find_lemma_exact_in_stmt(body->data[i], name, found, ambiguous,
                                 depth + 1);
}

static void nyr_find_imported_lemma_in_body(const nyr_builder_impl_t *b,
                                            const ny_stmt_list *body,
                                            const char *name,
                                            const stmt_t **found,
                                            bool *ambiguous) {
  if (!b || !body || !name || !found || !ambiguous || *ambiguous)
    return;
  for (size_t i = 0; i < body->len && !*ambiguous; ++i) {
    const stmt_t *use = body->data[i];
    if (!use || use->kind != NY_S_USE || !use->as.use.module)
      continue;
    const char *imported_name =
        (use->as.use.import_all ||
         (use->as.use.imports.len == 0 && !use->as.use.alias))
            ? name
            : NULL;
    for (size_t j = 0; !imported_name && j < use->as.use.imports.len; ++j) {
      const use_item_t *item = &use->as.use.imports.data[j];
      const char *visible = item->alias ? item->alias : item->name;
      if (visible && item->name && strcmp(visible, name) == 0)
        imported_name = item->name;
    }
    if (!imported_name)
      continue;
    const char *module_name = nyr_use_module_canonical(b, use->as.use.module);
    const stmt_t *module =
        nyr_find_module_in_prog(ny_native_fn_index_for(b), b->prog,
                                module_name ? module_name : use->as.use.module);
    nyr_find_lemma_exact_in_stmt(module, imported_name, found, ambiguous, 0);
  }
}

static void nyr_find_imported_lemma(const nyr_builder_impl_t *b,
                                    const char *name, const stmt_t **found,
                                    bool *ambiguous) {
  if (!b || !b->prog || !name || !found || !ambiguous)
    return;
  if (b->module_name && *b->module_name) {
    const stmt_t *module = nyr_find_module_in_prog(ny_native_fn_index_for(b),
                                                   b->prog, b->module_name);
    if (module && module->kind == NY_S_MODULE)
      nyr_find_imported_lemma_in_body(b, &module->as.module.body, name, found,
                                      ambiguous);
  }
  if (!*ambiguous)
    nyr_find_imported_lemma_in_body(b, &b->prog->body, name, found, ambiguous);
}

static bool nyr_eval_proof_lemma_call(const nyr_builder_impl_t *b,
                                      const expr_t *call, unsigned depth,
                                      int64_t *out,
                                      const nyr_proof_eval_env_t *outer_env) {
  if (!b || !call || call->kind != NY_E_CALL || !out || depth >= 64)
    return false;
  const stmt_t *lemma = call->semantic.canonical_callee_stmt;
  const char *name = call->semantic.canonical_callee;
  if (!name && call->as.call.callee && call->as.call.callee->kind == NY_E_IDENT)
    name = call->as.call.callee->as.ident.name;
  if (!lemma || lemma->kind != NY_S_LEMMA) {
    if (!name)
      return false;
    const stmt_t *found = NULL;
    bool ambiguous = false;
    /*
     * The flattened standard-library program can contain same-named lemmas
     * from unrelated modules. Resolve an unqualified witness through the
     * current module's imports before using the whole-program fallback.
     */
    nyr_find_imported_lemma(b, name, &found, &ambiguous);
    if (!found && !ambiguous)
      for (size_t i = 0; b->prog && i < b->prog->body.len && !ambiguous; ++i)
        nyr_find_lemma_exact_in_stmt(b->prog->body.data[i], name, &found,
                                     &ambiguous, 0);
    if (ambiguous)
      return false;
    lemma = found;
  }
  if (!lemma || lemma->kind != NY_S_LEMMA)
    return false;
  const ny_param_list *params = &lemma->as.lemma.params;
  const ny_call_arg_list *args = &call->as.call.args;
  if (args->len != params->len || args->len > 64)
    return false;
  const char *names[64];
  int64_t values[64];
  for (size_t i = 0; i < args->len; ++i) {
    if (args->data[i].name ||
        !nyr_eval_proof_i64_env(b, args->data[i].val, depth + 1, &values[i],
                                outer_env))
      return false;
    names[i] = params->data[i].name;
    if (!names[i])
      return false;
  }
  const nyr_proof_eval_env_t env = {names, values, args->len};
  return nyr_eval_proof_i64_env(b, lemma->as.lemma.proposition, depth + 1, out,
                                &env);
}

/*
 * Register immutable constants and mutable module storage before lowering.
 *
 * Fold `alias.member` initializer chains (def _OFF_U2 = render_shared.OFF_U2)
 * by resolving the alias against the defining module's own use declarations.
 * Without this, cross-module constant aliases never reach the const table and
 * every bare read emits a dangling raw-storage symbol.
 */
static bool nyr_fold_module_alias_int(const program_t *prog,
                                      const char *module_name, const expr_t *e,
                                      int64_t *out, unsigned depth) {
  if (!prog || !module_name || !e || e->kind != NY_E_MEMBER || !out ||
      depth > 16)
    return false;
  const stmt_t *owner = nyr_find_module_in_prog(NULL, prog, module_name);
  if (!owner || owner->kind != NY_S_MODULE)
    return false;
  const expr_t *root = e->as.member.target;
  while (root && root->kind == NY_E_MEMBER)
    root = root->as.member.target;
  if (!root || root->kind != NY_E_IDENT || !root->as.ident.name)
    return false;
  const char *alias = root->as.ident.name;
  const char *target_name = NULL;
  for (size_t i = 0; i < owner->as.module.body.len; ++i) {
    const stmt_t *use = owner->as.module.body.data[i];
    if (!use || use->kind != NY_S_USE || !use->as.use.module)
      continue;
    if (use->as.use.alias && strcmp(use->as.use.alias, alias) == 0) {
      target_name = use->as.use.module;
      break;
    }
    const char *leaf = strrchr(use->as.use.module, '.');
    leaf = leaf ? leaf + 1 : use->as.use.module;
    if (!use->as.use.alias && strcmp(leaf, alias) == 0) {
      target_name = use->as.use.module;
      break;
    }
  }
  if (!target_name)
    return false;
  const stmt_t *target_mod = nyr_find_module_in_prog(NULL, prog, target_name);
  if (!target_mod || target_mod->kind != NY_S_MODULE)
    return false;
  const expr_t *val =
      nyr_find_module_member_in(NULL, target_mod, e->as.member.name, 0);
  if (!val)
    return false;
  if (val->kind == NY_E_MEMBER) {
    const char *tmod = target_mod->as.module.name && *target_mod->as.module.name
                           ? target_mod->as.module.name
                           : target_name;
    return nyr_fold_module_alias_int(prog, tmod, val, out, depth + 1);
  }
  return nyr_fold_top_level_int(NULL, prog, val, out, depth + 1);
}

/*
 * Register compile-time integer values under their defining module scope.
 */
static void ny_native_register_const_defs(nyr_builder_impl_t *fold_cache,
                                          const program_t *prog,
                                          const stmt_t *s,
                                          const char *module_name,
                                          const ny_options *opt) {
  if (!prog || !s)
    return;
  /*
   * `#main` is a runtime entry guard, not a module-global declaration
   * scope. Imported modules may contain self-test locals there; registering
   * them as bare globals makes an unrelated root program resolve those names
   * by accident. Keep the guarded body out of the global/constant tables.
   */
  if (ny_is_stdlib_tok(s->tok) && s->kind == NY_S_IF && s->as.iff.test &&
      s->as.iff.test->kind == NY_E_COMPTIME &&
      s->as.iff.test->as.comptime_expr.body &&
      s->as.iff.test->as.comptime_expr.body->kind == NY_S_BLOCK) {
    const stmt_t *body = s->as.iff.test->as.comptime_expr.body;
    if (body->as.block.body.len == 1 && body->as.block.body.data[0] &&
        body->as.block.body.data[0]->kind == NY_S_RETURN) {
      const expr_t *value = body->as.block.body.data[0]->as.ret.value;
      if (value && value->kind == NY_E_CALL && value->as.call.callee &&
          value->as.call.callee->kind == NY_E_IDENT &&
          value->as.call.callee->as.ident.name &&
          strcmp(value->as.call.callee->as.ident.name, "__main") == 0)
        return;
    }
  }
  if (s->kind == NY_S_DEFINE) {
    if (s->as.def.name && *s->as.def.name && s->as.def.value &&
        *s->as.def.value) {
      char *end = NULL;
      errno = 0;
      long long parsed = strtoll(s->as.def.value, &end, 0);
      while (end && *end && isspace((unsigned char)*end))
        ++end;
      if (end && end != s->as.def.value && *end == '\0' && errno == 0) {
        if (module_name && *module_name) {
          char qualified[512];
          size_t module_len = strlen(module_name);
          int n =
              (strncmp(s->as.def.name, module_name, module_len) == 0 &&
               s->as.def.name[module_len] == '.')
                  ? snprintf(qualified, sizeof(qualified), "%s", s->as.def.name)
                  : snprintf(qualified, sizeof(qualified), "%s.%s", module_name,
                             s->as.def.name);
          if (n > 0 && (size_t)n < sizeof(qualified))
            nyr_consttab_add(qualified, (int64_t)parsed);
        } else {
          nyr_consttab_add(s->as.def.name, (int64_t)parsed);
        }
      }
    }
    return;
  }
  if (s->kind == NY_S_VAR) {
    for (size_t i = 0; i < s->as.var.names.len; ++i) {
      const char *name = s->as.var.names.data[i];
      const expr_t *init =
          i < s->as.var.exprs.len ? s->as.var.exprs.data[i] : NULL;
      if (!name || !*name)
        continue;
      if (s->as.var.is_mut) {
        if (!module_name || !*module_name) {
          nyr_globaltab_add(name);
        } else {
          size_t module_len = strlen(module_name);
          if (strncmp(name, module_name, module_len) == 0 &&
              name[module_len] == '.')
            nyr_globaltab_add(name);
          else {
            char qualified[512];
            int n = snprintf(qualified, sizeof(qualified), "%s.%s", module_name,
                             name);
            if (n > 0 && (size_t)n < sizeof(qualified))
              nyr_globaltab_add(qualified);
          }
        }
      }
      if (!init)
        continue;
      int64_t value = 0;
      bool folded = nyr_fold_top_level_int_in_module(
          fold_cache, prog, module_name, init, &value, 0);
      if (!folded && init->kind == NY_E_MEMBER && module_name && *module_name)
        folded = nyr_fold_module_alias_int(prog, module_name, init, &value, 1);
      bool is_tag = false;
      if (!folded && init->kind == NY_E_LITERAL &&
          init->as.literal.kind == NY_LIT_FLOAT) {
        value = ny_native_f64_bits(init->as.literal.as.f);
        folded = true;
      }
      if (!folded && nyr_fold_runtime_tag(init, &value)) {
        folded = true;
        is_tag = true;
      }
      if (folded) {
        /*
         * Values outside the tagged small-int range are runtime bigint
         * handles, not raw constant i64s.  Leave their initializer visible
         * to lowering so it can materialize the handle.
         */
        if (value < (INT64_C(1) << 62) && value > -(INT64_C(1) << 62)) {
          /*
           * Keep module constants qualified.  Registering every module
           * export under its leaf name makes the first collected `FLAG` win
           * globally, so later functions can silently read another module's
           * value. Root-program constants retain their bare spelling.
           */
          if (module_name && *module_name) {
            char qualified[512];
            int n = (strncmp(name, module_name, strlen(module_name)) == 0 &&
                     name[strlen(module_name)] == '.')
                        ? snprintf(qualified, sizeof(qualified), "%s", name)
                        : snprintf(qualified, sizeof(qualified), "%s.%s",
                                   module_name, name);
            if (n > 0 && (size_t)n < sizeof(qualified))
              nyr_consttab_add(qualified, value);
          } else {
            nyr_consttab_add(name, value);
          }
        }
        if (is_tag) {
          const char *leaf = strrchr(name, '.');
          if (leaf && leaf[1])
            nyr_consttab_add(leaf + 1, value);
        }
      }
    }
    return;
  }
  if (s->kind == NY_S_MODULE) {
    const char *child_module = s->as.module.name && *s->as.module.name
                                   ? s->as.module.name
                                   : module_name;
    for (size_t i = 0; i < s->as.module.body.len; ++i)
      ny_native_register_const_defs(fold_cache, prog, s->as.module.body.data[i],
                                    child_module, opt);
    return;
  }
  if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      ny_native_register_const_defs(fold_cache, prog, s->as.block.body.data[i],
                                    module_name, opt);
    return;
  }
  if (s->kind == NY_S_IF) {
    bool selected = false;
    if (s->as.iff.test && s->as.iff.test->kind == NY_E_COMPTIME &&
        nyr_target_eval_bool(opt, s->as.iff.test, &selected)) {
      ny_native_register_const_defs(fold_cache, prog,
                                    selected ? s->as.iff.conseq : s->as.iff.alt,
                                    module_name, opt);
      return;
    }
    ny_native_register_const_defs(fold_cache, prog, s->as.iff.conseq,
                                  module_name, opt);
    ny_native_register_const_defs(fold_cache, prog, s->as.iff.alt, module_name,
                                  opt);
  }
}

static nyr_local_t *nyr_find_local_slot(nyr_builder_impl_t *b, int slot) {
  if (!b)
    return NULL;
  for (size_t i = b->local_count; i > 0; --i)
    if (b->locals[i - 1].slot == slot)
      return &b->locals[i - 1];
  return NULL;
}

/*
 * Dynamic ABI companion words are machine integers even when the semantic
 * value they describe is tagged.  They have reserved local slots without a
 * nyr_local_t of their own, so preserve that fact on every load and
 * store instead of letting NYIR infer a class from the tagged payload.
 */
static bool nyr_is_raw_companion_slot(const nyr_builder_impl_t *b, int slot) {
  if (!b || slot < 0)
    return false;
  for (size_t i = 0; i < b->local_count; ++i) {
    const nyr_local_t *local = &b->locals[i];
    if (local->dyn_tag_slot == slot || local->dyn_str_len_slot == slot ||
        local->list_len_slot == slot)
      return true;
  }
  return false;
}

static bool nyr_record_fact(nyr_builder_impl_t *b, int value,
                            nyr_fact_kind_t kind, int64_t payload) {
  if (!b || value < 0 || payload <= 0)
    return true;
  if (b->fact_count == b->fact_cap) {
    size_t cap = b->fact_cap ? b->fact_cap * 2 : 16;
    if (cap < b->fact_cap || cap > SIZE_MAX / sizeof(*b->facts))
      return nyr_fail(b, NY_NATIVE_ALLOC_FAIL);
    nyr_fact_t *facts = realloc(b->facts, cap * sizeof(*facts));
    if (!facts)
      return nyr_fail(b, NY_NATIVE_ALLOC_FAIL);
    b->facts = facts;
    b->fact_cap = cap;
  }
  b->facts[b->fact_count++] =
      (nyr_fact_t){.value = value, .payload = payload, .kind = kind};
  return true;
}

static int64_t nyr_peek_fact(const nyr_builder_impl_t *b, int value,
                             nyr_fact_kind_t kind) {
  if (!b || value < 0)
    return 0;
  for (size_t i = b->fact_count; i > 0; --i) {
    size_t index = i - 1;
    if (b->facts[index].value == value && b->facts[index].kind == kind)
      return b->facts[index].payload;
  }
  return 0;
}

static int64_t nyr_take_fact(nyr_builder_impl_t *b, int value,
                             nyr_fact_kind_t kind) {
  if (!b || value < 0)
    return 0;
  for (size_t i = b->fact_count; i > 0; --i) {
    size_t index = i - 1;
    if (b->facts[index].value != value || b->facts[index].kind != kind)
      continue;
    int64_t payload = b->facts[index].payload;
    b->facts[index] = b->facts[--b->fact_count];
    return payload;
  }
  return 0;
}

static bool nyr_record_alloc_fact(nyr_builder_impl_t *b, int value,
                                  int64_t byte_len) {
  return nyr_record_fact(b, value, NYR_FACT_ALLOC, byte_len);
}

static int64_t nyr_peek_alloc_fact(const nyr_builder_impl_t *b, int value) {
  return nyr_peek_fact(b, value, NYR_FACT_ALLOC);
}

static int64_t nyr_take_alloc_fact(nyr_builder_impl_t *b, int value) {
  return nyr_take_fact(b, value, NYR_FACT_ALLOC);
}

static bool nyr_record_fin_fact(nyr_builder_impl_t *b, int value,
                                int64_t bound) {
  return nyr_record_fact(b, value, NYR_FACT_FIN, bound);
}

static int64_t nyr_take_fin_fact(nyr_builder_impl_t *b, int value) {
  return nyr_take_fact(b, value, NYR_FACT_FIN);
}

static bool nyr_record_list_len_fact(nyr_builder_impl_t *b, int value,
                                     int length_value) {
  return length_value < 0 || length_value == INT32_MAX
             ? false
             : nyr_record_fact(b, value, NYR_FACT_LIST_LEN,
                               (int64_t)length_value + 1);
}

static int nyr_peek_list_len_fact(const nyr_builder_impl_t *b, int value) {
  int64_t encoded = nyr_peek_fact(b, value, NYR_FACT_LIST_LEN);
  return encoded > 0 ? (int)(encoded - 1) : -1;
}

static int nyr_take_list_len_fact(nyr_builder_impl_t *b, int value) {
  int64_t encoded = nyr_take_fact(b, value, NYR_FACT_LIST_LEN);
  return encoded > 0 ? (int)(encoded - 1) : -1;
}

static bool nyr_record_dyn_fact(nyr_builder_impl_t *b, int value,
                                nyr_fact_kind_t kind, int reg) {
  return reg >= 0 && nyr_record_fact(b, value, kind, (int64_t)reg + 1);
}

static int nyr_peek_dyn_fact(const nyr_builder_impl_t *b, int value,
                             nyr_fact_kind_t kind) {
  int64_t encoded = nyr_peek_fact(b, value, kind);
  return encoded > 0 ? (int)(encoded - 1) : -1;
}

static int nyr_take_dyn_fact(nyr_builder_impl_t *b, int value,
                             nyr_fact_kind_t kind) {
  int64_t encoded = nyr_take_fact(b, value, kind);
  return encoded > 0 ? (int)(encoded - 1) : -1;
}

static int nyr_emit_known_list_append_len(nyr_builder_impl_t *b, int list,
                                          int out) {
  int length = -1;
  int base_len = nyr_take_list_len_fact(b, list);
  if (base_len >= 0) {
    int one = nyr_emit_const(b, 1);
    length = one < 0 ? -1 : nyr_emit_add_i64(b, base_len, one);
  }
  if (length < 0 && out >= 0) {
    /*
     * An append's return value is always the current buffer handle (never a
     * growth-forwarded one), so the post-append count is the header word at
     * out-24 read directly. rt_tbuf_len_raw re-validates handle provenance
     * and magic on every call, which made each hot-loop append iteration pay
     * a full runtime call just to re-read a count both append arms had just
     * stored into that header.
     */
    int neg24 = nyr_emit_const(b, -24);
    int count_addr = neg24 < 0 ? -1 : nyr_emit_add_i64(b, out, neg24);
    length = count_addr < 0 ? -1 : nyr_emit_load_i64(b, count_addr);
    if (length < 0)
      length = nyr_emit_runtime_call(b, "rt_tbuf_len_raw", out, -1, -1, 1, 0);
  }
  return length;
}

/*
 * A tbuf pop never reallocates; it only decrements the header count when the
 * list is non-empty.  When lowering already carries the list length as an SSA
 * fact, derive the post-pop length directly instead of re-reading the managed
 * header through rt_tbuf_len_raw.  CMP_I64 yields 0/1, so
 *   len - (len > 0)
 * exactly preserves pop's saturating-at-zero length semantics.
 */
static int nyr_emit_known_list_pop_len(nyr_builder_impl_t *b, int list) {
  int base_len = nyr_take_list_len_fact(b, list);
  if (base_len < 0)
    return -1;
  int zero = nyr_emit_const(b, 0);
  if (zero < 0)
    return -1;
  int nonempty = nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_CMP_I64,
                                                   .dst = -1,
                                                   .a = base_len,
                                                   .b = zero,
                                                   .cmp = NYIR_CMP_GT});
  if (nonempty < 0) {
    return nyr_fail(b, NY_NATIVE_ALLOC_FAIL), -1;
  }
  return nyr_push_val(b, NYIR_SUB_I64, base_len, nonempty, 0, NULL);
}

static int nyr_emit_known_cstr_concat_len(nyr_builder_impl_t *b, int lhs,
                                          int rhs, int out) {
  int lhs_len = nyr_peek_dyn_fact(b, lhs, NYR_FACT_DYN_STR_LEN);
  int rhs_len = nyr_peek_dyn_fact(b, rhs, NYR_FACT_DYN_STR_LEN);
  if (lhs_len >= 0 && rhs_len >= 0)
    return nyr_emit_add_i64(b, lhs_len, rhs_len);
  return out < 0 ? -1
                 : nyr_emit_runtime_call(b, "rt_cstr_len", out, -1, -1, 1, 0);
}

static int64_t nyr_literal_allocation_size(const char *leaf,
                                           const expr_t *call) {
  if (!leaf || !call || call->kind != NY_E_CALL)
    return 0;
  ny_builtin_alloc_kind_t kind = ny_builtin_alloc_kind(leaf);
  if (kind == NY_BUILTIN_ALLOC_NONE || kind == NY_BUILTIN_ALLOC_FREE)
    return 0;
  /*
   * malloc(n): n is the byte count. calloc(count, n): the byte count lives
   * in the second argument — reading the count slot sized a calloc(1, 8)
   * buffer as 1 byte, and every Fin-elided store past offset 0 then failed
   * the VM bounds check (alloc-provenance oracle).
   */
  size_t size_arg =
      kind == NY_BUILTIN_ALLOC_REALLOC || kind == NY_BUILTIN_ALLOC_CALLOC ? 1
                                                                          : 0;
  if (call->as.call.args.len <= size_arg)
    return 0;
  const expr_t *size = call->as.call.args.data[size_arg].val;
  if (!size || size->kind != NY_E_LITERAL ||
      size->as.literal.kind != NY_LIT_INT || size->as.literal.as.i <= 0)
    return 0;
  int64_t bytes = size->as.literal.as.i;
  if (kind != NY_BUILTIN_ALLOC_CALLOC)
    return bytes;
  if (call->as.call.args.len != 2)
    return 0;
  const expr_t *count = call->as.call.args.data[0].val;
  if (!count || count->kind != NY_E_LITERAL ||
      count->as.literal.kind != NY_LIT_INT || count->as.literal.as.i <= 0 ||
      count->as.literal.as.i > INT64_MAX / bytes)
    return 0;
  return count->as.literal.as.i * bytes;
}

static nyr_scalar_class_t
ny_native_scalar_class_from_sem_rep(ny_sem_rep_t rep) {
  switch (rep) {
  case NY_SEM_REP_RAW_INT:
    return NYIR_SCALAR_I64_RAW;
  case NY_SEM_REP_F32:
    return NYIR_SCALAR_F32;
  case NY_SEM_REP_F64:
    return NYIR_SCALAR_F64;
  case NY_SEM_REP_TAGGED_DYNAMIC:
    return NYIR_SCALAR_I64_TAGGED;
  case NY_SEM_REP_STRING:
    return NYIR_SCALAR_STRING;
  case NY_SEM_REP_TYPED_BUFFER:
    return NYIR_SCALAR_TYPED_BUFFER;
  case NY_SEM_REP_OBJECT:
    return NYIR_SCALAR_OBJECT;
  case NY_SEM_REP_POINTER:
    return NYIR_SCALAR_POINTER;
  case NY_SEM_REP_CLOSURE:
    return NYIR_SCALAR_CLOSURE;
  case NY_SEM_REP_AGGREGATE:
    return NYIR_SCALAR_AGGREGATE;
  case NY_SEM_REP_UNRESOLVED:
  default:
    return NYIR_SCALAR_UNKNOWN;
  }
}

static const nyr_inst_t *nyr_reg_def_inst(const nyr_builder_impl_t *b,
                                           int reg);

static bool nyr_store_local_value(nyr_builder_impl_t *b, int slot, int value) {
  nyr_local_t *capture = nyr_find_local_slot(b, slot);
  if (capture && capture->is_capture && b->closure_env_slot >= 0) {
    int env = nyr_load_local_value(b, b->closure_env_slot);
    int off =
        env < 0 ? -1 : nyr_emit_const(b, (int64_t)capture->capture_index * 8);
    int addr = env < 0 || off < 0 ? -1 : nyr_emit_add_i64(b, env, off);
    if (addr >= 0 && (capture->is_f64 || capture->is_f32)) {
      nyr_scalar_class_t value_class = nyr_value_scalar_class(b, value);
      int as_f64 = value;
      if (value_class == NYIR_SCALAR_F32)
        as_f64 = nyr_emit_f32_to_f64(b, value);
      else if (value_class == NYIR_SCALAR_I64_TAGGED)
        as_f64 = nyr_emit_runtime_call(b, "rt_any_to_f64", value, -1, -1, 1,
                                       NYIR_SCALAR_F64);
      else if (value_class != NYIR_SCALAR_F64)
        as_f64 = nyr_emit_i64_to_f64(b, value);
      int bits = as_f64 < 0 ? -1
                            : nyr_emit_runtime_call(b, "rt_f64_bits", as_f64,
                                                    -1, -1, 1, 0);
      return bits >= 0 && nyr_emit_store_i64(b, addr, bits);
    }
    return addr >= 0 && nyr_emit_store_i64(b, addr, value);
  }
  nyr_local_t *local = nyr_find_local_slot(b, slot);
  bool declared_int = local && !local->is_capture && !local->is_bigint &&
                      local->type_name &&
                      ny_native_type_name_is_int(local->type_name);
  bool raw_integer_slot =
      declared_int ||
      (local && !local->is_capture && !local->is_any && !local->is_bool &&
       !local->is_bigint && local->semantic_rep == NY_SEM_REP_RAW_INT);
  nyr_scalar_class_t incoming_class = nyr_value_scalar_class(b, value);
  bool inferred_handle_slot = local && !local->is_capture && !local->is_any &&
                              (incoming_class == NYIR_SCALAR_POINTER ||
                               incoming_class == NYIR_SCALAR_TYPED_BUFFER);
  /*
   * A source `any` local has a dynamic slot contract even when later
   * lowering re-infers a concrete numeric fact for one assignment. Normalize
   * each store at the slot boundary so a tagged float cannot be reloaded from
   * the XMM lane as though its handle word were an f64.
   */
  if (local && local->type_name &&
      ny_native_type_name_is_any(local->type_name) && !inferred_handle_slot) {
    local->is_any = true;
    local->is_f64 = false;
    local->is_f32 = false;
    local->semantic_rep = NY_SEM_REP_TAGGED_DYNAMIC;
    nyr_scalar_class_t producer_class = nyr_value_scalar_class(b, value);
    if (producer_class == NYIR_SCALAR_F32 ||
        producer_class == NYIR_SCALAR_F64) {
      int as_f64 = producer_class == NYIR_SCALAR_F32
                       ? nyr_emit_f32_to_f64(b, value)
                       : value;
      int bits = as_f64 < 0 ? -1
                            : nyr_emit_runtime_call(b, "rt_f64_bits", as_f64,
                                                    -1, -1, 1, 0);
      value = bits < 0 ? -1
                       : nyr_emit_runtime_call(b, "rt_flt_box_val", bits, -1,
                                               -1, 1, NYIR_SCALAR_I64_TAGGED);
      if (value < 0)
        return nyr_fail(b, NY_NATIVE_ALLOC_FAIL);
    }
  }
  if (inferred_handle_slot) {
    local->is_f64 = false;
    local->is_f32 = false;
    local->semantic_rep = incoming_class == NYIR_SCALAR_TYPED_BUFFER
                              ? NY_SEM_REP_TYPED_BUFFER
                              : NY_SEM_REP_POINTER;
  }
  if (local && local->type_name &&
      local->semantic_rep == NY_SEM_REP_AGGREGATE &&
      nyr_rep_for_type(b, local->type_name) == NY_SEM_REP_OBJECT) {
    local->semantic_rep = NY_SEM_REP_OBJECT;
    local->is_dict = true;
    local->scalar_class = NYIR_SCALAR_OBJECT;
    local->scalar_class_mixed = false;
  }
  /*
   * An `any` local stores the tagged dynamic carrier. A raw int result can
   * otherwise survive the assignment and only fail later when passed through
   * another dynamic call boundary (for example, assigning waitpid's result
   * before passing it to err).
   */
  if (local && local->is_any && !declared_int &&
      nyr_value_scalar_class(b, value) == NYIR_SCALAR_I64_RAW) {
    value = local->is_bool
                ? nyr_box_bool(b, value)
                : nyr_emit_runtime_call(b, "rt_box_i64", value, -1, -1, 1,
                                        NYIR_SCALAR_I64_TAGGED);
    if (value < 0)
      return nyr_fail(b, NY_NATIVE_ALLOC_FAIL);
  }
  /*
   * A machine-float slot contract must receive machine-float bits. When the
   * initializer's operator lowering kept a dynamic operand in the canonical
   * tagged domain (the rt_any_mul bridge and siblings), decode at this slot
   * boundary instead of storing the tagged handle word under the f64/f32
   * stamp the classifier published for the float-typed operand.
   */
  if (local && !local->is_any && !local->is_capture &&
      (local->is_f64 || local->is_f32 ||
       local->semantic_rep == NY_SEM_REP_F64 ||
       local->semantic_rep == NY_SEM_REP_F32) &&
      nyr_value_scalar_class(b, value) == NYIR_SCALAR_I64_TAGGED) {
    int as_f64 = nyr_emit_runtime_call(b, "rt_any_to_f64", value, -1, -1, 1,
                                       NYIR_SCALAR_F64);
    if (as_f64 < 0)
      return nyr_fail(b, NY_NATIVE_ALLOC_FAIL);
    if (local->is_f32 || local->semantic_rep == NY_SEM_REP_F32) {
      as_f64 = nyr_emit_f64_to_f32(b, as_f64);
      if (as_f64 < 0)
        return nyr_fail(b, NY_NATIVE_ALLOC_FAIL);
    }
    value = as_f64;
  }
  /*
   * A generic Result unwrap can return an opaque one-word enum payload with
   * the raw i64 ABI even when HM has already established the destination's
   * aggregate carrier. Preserve the typed handle class at the local boundary;
   * the bits are unchanged, just as for a pointer load.
   */
  if (local && !local->is_any && !local->is_capture &&
      local->semantic_rep == NY_SEM_REP_AGGREGATE &&
      nyr_value_scalar_class(b, value) == NYIR_SCALAR_I64_RAW) {
    value = nyr_ir_emit(&b->nyir,
                      (nyr_inst_t){.op = NYIR_COPY,
                                    .dst = -1,
                                    .a = value,
                                    .b = -1,
                                    .scalar_class = NYIR_SCALAR_AGGREGATE});
    if (value < 0)
      return nyr_fail(b, NY_NATIVE_ALLOC_FAIL);
  }
  /*
   * A concrete integer local owns a raw-i64 slot even when its initializer
   * or a later assignment was produced through an `any` ABI. Normalize at
   * the write boundary so the slot never contains a tagged word on one path
   * and a raw integer on another (the merge otherwise silently doubles odd
   * values when a later load chooses the raw representation).
   */
  if (raw_integer_slot &&
      nyr_value_scalar_class(b, value) == NYIR_SCALAR_I64_TAGGED) {
    value = nyr_emit_runtime_call(b, "rt_any_to_i64", value, -1, -1, 1,
                                  NYIR_SCALAR_I64_RAW);
    if (value < 0)
      return nyr_fail(b, NY_NATIVE_ALLOC_FAIL);
  }
  /*
   * Nominal values imported through a module alias can arrive with the
   * aggregate producer class while their resolved local contract is an
   * object handle. Materialize that representation boundary in NYIR; merely
   * relabeling the local slot leaves the reaching store aggregate-typed.
   */
  if (local && !local->is_any && !local->is_capture &&
      local->semantic_rep == NY_SEM_REP_OBJECT &&
      nyr_value_scalar_class(b, value) == NYIR_SCALAR_AGGREGATE) {
    value =
        nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_COPY,
                                          .dst = -1,
                                          .a = value,
                                          .b = -1,
                                          .scalar_class = NYIR_SCALAR_OBJECT});
    if (value < 0)
      return nyr_fail(b, NY_NATIVE_ALLOC_FAIL);
  }
  size_t before = b->nyir.len;
  nyr_ir_emit(
      &b->nyir,
      (nyr_inst_t){
          .op = NYIR_STORE_LOCAL, .dst = -1, .a = value, .b = -1, .imm = slot});
  if (b->nyir.len == before)
    return nyr_fail(b, NY_NATIVE_ALLOC_FAIL);
  if (local && b->nyir.len > 0) {
    nyr_inst_t *in = &b->nyir.data[b->nyir.len - 1];
    if (in->op == NYIR_STORE_LOCAL && in->imm == slot) {
      nyr_scalar_class_t local_class = ny_native_scalar_class_from_sem_rep(
          local->is_f64   ? NY_SEM_REP_F64
          : local->is_f32 ? NY_SEM_REP_F32
                          : local->semantic_rep);
      nyr_scalar_class_t producer_class = nyr_value_scalar_class(b, value);
      if (producer_class != NYIR_SCALAR_UNKNOWN && !local->scalar_class_mixed) {
        if (local->scalar_class == NYIR_SCALAR_UNKNOWN)
          local->scalar_class = producer_class;
        else if (local->scalar_class != producer_class) {
          local->scalar_class = NYIR_SCALAR_UNKNOWN;
          local->scalar_class_mixed = true;
        }
      }
      if (ny_trace_enabled("NY_TRACE_ABI") &&
          producer_class != NYIR_SCALAR_UNKNOWN &&
          local_class != producer_class)
        ny_trace_line("[abi-local-store]",
                      "fn=%s slot=%d name=%s class=%d producer=%d "
                      "is_any=%d is_f64=%d is_f32=%d rep=%d mixed=%d",
                      b->current_fn_name ? b->current_fn_name : "?", slot,
                      local->name ? local->name : "?", local_class,
                      producer_class, local->is_any, local->is_f64,
                      local->is_f32, local->semantic_rep,
                      local->scalar_class_mixed);
      if (local_class == NYIR_SCALAR_UNKNOWN && !local->is_any &&
          local->semantic_rep == NY_SEM_REP_UNRESOLVED) {
        if (producer_class == NYIR_SCALAR_F64) {
          local->is_f64 = true;
          local->semantic_rep = NY_SEM_REP_F64;
          local_class = producer_class;
        } else if (producer_class == NYIR_SCALAR_F32) {
          local->is_f32 = true;
          local->semantic_rep = NY_SEM_REP_F32;
          local_class = producer_class;
        }
      }
      /*
       * The local's HM rep describes its source-level type; the stored
       * value's producer class describes the machine word actually written.
       * Keep the latter on NYIR so later loads and call-boundary lowering do
       * not reinterpret a handle as a float/raw integer after slot reuse.
       */
      in->scalar_class =
          producer_class != NYIR_SCALAR_UNKNOWN ? producer_class : local_class;
      /*
       * A mutable local can hold values with different ownership from its
       * initializer.  The producer fact describes this store's value; the
       * local's initial semantic fact is only a fallback when the producer
       * carries no ownership metadata.
       */
      const nyr_inst_t *value_def = nyr_reg_def_inst(b, value);
      if (value_def && value_def->semantic_ownership != NY_SEM_OWN_UNKNOWN) {
        if (local->semantic_ownership != NY_SEM_OWN_UNKNOWN &&
            local->semantic_ownership != value_def->semantic_ownership)
          local->semantic_ownership = NY_SEM_OWN_UNKNOWN;
        in->semantic_ownership = value_def->semantic_ownership;
      } else if (local->semantic_ownership != NY_SEM_OWN_UNKNOWN) {
        in->semantic_ownership = (uint8_t)local->semantic_ownership;
      }
      if (local->alias_class)
        in->alias_class = local->alias_class;
      in->semantic_mutable = local->semantic_mutable;
    } else if (b->nyir.len > 0) {
      /*
       * A temp slot with no local record (lower_f64_arg's ternary join):
       * stamp the store with the value's own class, or the local ABI map
       * reads non-float while the arms stored f64 and the merged load
       * fails the ret scalar-class check (core.str.atof's
       * `return sign < 0 ? 0.0 - v : v`).
       */
      nyr_inst_t *temp_store = &b->nyir.data[b->nyir.len - 1];
      if (temp_store->op == NYIR_STORE_LOCAL && temp_store->imm == slot &&
          temp_store->scalar_class == NYIR_SCALAR_UNKNOWN) {
        nyr_scalar_class_t producer_class =
            nyr_is_raw_companion_slot(b, slot)
                ? NYIR_SCALAR_I64_RAW
                : nyr_value_scalar_class(b, value);
        temp_store->scalar_class = producer_class;
      }
    }
  }
  int64_t byte_len = nyr_take_alloc_fact(b, value);
  int64_t fin_bound = nyr_take_fin_fact(b, value);
  int list_len = nyr_take_list_len_fact(b, value);
  int dyn_str_len = nyr_take_dyn_fact(b, value, NYR_FACT_DYN_STR_LEN);
  int dyn_tag = nyr_take_dyn_fact(b, value, NYR_FACT_DYN_TAG);
  if (local) {
    local->buffer_byte_len = byte_len;
    local->fin_bound = fin_bound;
    if (dyn_str_len >= 0) {
      if (local->dyn_str_len_slot < 0)
        local->dyn_str_len_slot = b->next_local_slot++;
      if (!nyr_store_local_value(b, local->dyn_str_len_slot, dyn_str_len))
        return false;
    }
    if (dyn_tag >= 0) {
      if (local->dyn_tag_slot < 0)
        local->dyn_tag_slot = b->next_local_slot++;
      if (!nyr_store_local_value(b, local->dyn_tag_slot, dyn_tag))
        return false;
    }
    if (local->is_list) {
      if (list_len < 0) {
        list_len =
            nyr_emit_runtime_call(b, "rt_tbuf_len_raw", value, -1, -1, 1, 0);
        if (list_len < 0)
          return nyr_fail(
              b, "native NYIR lower: list assignment length query failed in %s",
              b->current_fn_name ? b->current_fn_name : "<unknown>");
      }
      if (local->list_len_slot < 0)
        local->list_len_slot = b->next_local_slot++;
      size_t length_before = b->nyir.len;
      nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_STORE_LOCAL,
                                        .dst = -1,
                                        .a = list_len,
                                        .b = -1,
                                        .imm = local->list_len_slot});
      if (b->nyir.len == length_before)
        return nyr_fail(b, NY_NATIVE_ALLOC_FAIL);
    }
  }
  return true;
}

static int nyr_load_local_value(nyr_builder_impl_t *b, int slot) {
  nyr_local_t *capture = nyr_find_local_slot(b, slot);
  if (capture && capture->is_capture && b->closure_env_slot >= 0) {
    int env = nyr_load_local_value(b, b->closure_env_slot);
    int off =
        env < 0 ? -1 : nyr_emit_const(b, (int64_t)capture->capture_index * 8);
    int addr = env < 0 || off < 0 ? -1 : nyr_emit_add_i64(b, env, off);
    int value = addr < 0 ? -1 : nyr_emit_load_i64(b, addr);
    if (value >= 0 && (capture->is_f64 || capture->is_f32)) {
      value = nyr_emit_runtime_call(b, "rt_f64_from_bits", value, -1, -1, 1,
                                    NYIR_SCALAR_F64);
      if (value >= 0 && capture->is_f32)
        value = nyr_emit_f64_to_f32(b, value);
      return value;
    }
    if (value >= 0 && capture->semantic_rep == NY_SEM_REP_RAW_INT &&
        b->nyir.len > 0 && b->nyir.data[b->nyir.len - 1].dst == value)
      b->nyir.data[b->nyir.len - 1].scalar_class = NYIR_SCALAR_I64_RAW;
    return value;
  }
  int v = nyr_ir_emit(
      &b->nyir,
      (nyr_inst_t){
          .op = NYIR_LOAD_LOCAL, .dst = -1, .a = -1, .b = -1, .imm = slot});
  if (v < 0) {
    nyr_fail(b, NY_NATIVE_ALLOC_FAIL);
    return v;
  }
  nyr_local_t *local = nyr_find_local_slot(b, slot);
  if (local && b->nyir.len > 0) {
    nyr_inst_t *in = &b->nyir.data[b->nyir.len - 1];
    if (in->op == NYIR_LOAD_LOCAL && in->imm == slot) {
      /*
       * The binder's semantic rep can lag behind a concrete value written
       * into a reused local slot (notably tagged handles and typed buffers).
       * When the immediately reaching store gives an exact producer class,
       * preserve it on the load so subsequent call lowering can insert the
       * required ABI conversion.
       */
      nyr_scalar_class_t reaching_class = nyr_value_scalar_class(b, v);
      nyr_scalar_class_t inferred_class = reaching_class;
      /*
       * A concrete local declaration is the slot ABI across control-flow
       * joins.  The generic value map can infer a floating class for a load
       * that follows a loop/branch label even when every int-typed write is
       * raw (core.dict_mod._pow2's `mut v = 1; while ... v = v << 1`).
       * Let the typed binding win over that merged fallback; otherwise the
       * int return path inserts rt_f64_to_i64 on raw bits and fails verify.
       */
      bool concrete_local_type = local->type_name && !local->is_any &&
                                 !ny_native_type_name_is_any(local->type_name);
      ny_sem_rep_t declared_rep = concrete_local_type
                                      ? nyr_rep_for_type(b, local->type_name)
                                      : NY_SEM_REP_UNRESOLVED;
      /*
       * `any` describes the source level value domain, not a universal
       * physical register class. Numeric stores are normalized to tagged
       * words above, while managed references can retain their pointer or
       * string carrier. Prefer the class observed on the actual slot stores
       * so calls and returns consume the value in the ABI that wrote it.
       */
      if (!local->is_any && concrete_local_type &&
          declared_rep != NY_SEM_REP_UNRESOLVED) {
        /*
         * A concrete source-level local type owns the physical slot ABI.
         * Flags such as is_f64 may have been inferred from a contextual use
         * or a previous slot occupant; allowing those flags to outrank a
         * declared ptr/str/int made loop-carried Builder and index locals
         * reload through XMM registers despite pointer/raw stores.
         */
        nyr_scalar_class_t declared_class =
            ny_native_scalar_class_from_sem_rep(declared_rep);
        if (declared_class != NYIR_SCALAR_UNKNOWN)
          reaching_class = declared_class;
      } else if (!local->scalar_class_mixed &&
                 local->scalar_class != NYIR_SCALAR_UNKNOWN) {
        /*
         * For inferred locals, use the class observed on every store. This
         * is stronger than the type map's contextual guess for a LOAD_LOCAL
         * after a loop edge (for example Builder's pointer return class).
         */
        reaching_class = local->scalar_class;
      } else if (!local->is_any && local->is_f64)
        reaching_class = NYIR_SCALAR_F64;
      else if (!local->is_any && local->is_f32)
        reaching_class = NYIR_SCALAR_F32;
      else if (local->is_cstr ||
               (!local->is_any && local->semantic_rep == NY_SEM_REP_STRING))
        reaching_class = NYIR_SCALAR_STRING;
      else if (!local->is_any && local->semantic_rep == NY_SEM_REP_RAW_INT)
        reaching_class = NYIR_SCALAR_I64_RAW;
      if (ny_trace_enabled("NY_TRACE_ABI"))
        ny_trace_line("[abi-local-load]",
                      "fn=%s slot=%d name=%s type=%s any=%d f64=%d rep=%d "
                      "scalar=%s mixed=%d inferred=%s selected=%s",
                      b->current_fn_name ? b->current_fn_name : "?", slot,
                      local->name ? local->name : "?",
                      local->type_name ? local->type_name : "-", local->is_any,
                      local->is_f64, local->semantic_rep,
                      nyr_scalar_class_name(local->scalar_class),
                      local->scalar_class_mixed,
                      nyr_scalar_class_name(inferred_class),
                      nyr_scalar_class_name(reaching_class));
      /*
       * Same class precedence as nyr_store_local_value: a slot
       * stored through its machine-float fact must load as the same class,
       * or the verifier rejects the store/load pair (`mut g = 0.0` then
       * `g = -0.0` produced store F64 / load semantic-rep).
       */
      in->scalar_class =
          reaching_class != NYIR_SCALAR_UNKNOWN
              ? reaching_class
              : (!local->scalar_class_mixed &&
                         local->scalar_class != NYIR_SCALAR_UNKNOWN
                     ? local->scalar_class
                     : ny_native_scalar_class_from_sem_rep(
                           local->is_f64   ? NY_SEM_REP_F64
                           : local->is_f32 ? NY_SEM_REP_F32
                                           : local->semantic_rep));
      if (local->semantic_ownership)
        in->semantic_ownership = (uint8_t)local->semantic_ownership;
      if (local->alias_class)
        in->alias_class = local->alias_class;
      in->semantic_mutable = local->semantic_mutable;
      if (local->fin_bound > 0) {
        in->range.has_min = true;
        in->range.has_max = true;
        in->range.min = 0;
        in->range.max = local->fin_bound - 1;
      } else if (local->is_bool) {
        in->range.has_min = true;
        in->range.has_max = true;
        in->range.min = 0;
        in->range.max = 1;
      }
    }
  } else if (nyr_is_raw_companion_slot(b, slot) && b->nyir.len > 0) {
    nyr_inst_t *in = &b->nyir.data[b->nyir.len - 1];
    if (in->op == NYIR_LOAD_LOCAL && in->imm == slot)
      in->scalar_class = NYIR_SCALAR_I64_RAW;
  }
  if (local && local->buffer_byte_len > 0 &&
      !nyr_record_alloc_fact(b, v, local->buffer_byte_len))
    return -1;
  if (local && local->fin_bound > 0 &&
      !nyr_record_fin_fact(b, v, local->fin_bound))
    return -1;
  if (local && local->dyn_str_len_slot >= 0) {
    int length = nyr_load_local_value(b, local->dyn_str_len_slot);
    if (length < 0 || !nyr_record_dyn_fact(b, v, NYR_FACT_DYN_STR_LEN, length))
      return -1;
  }
  if (local && local->dyn_tag_slot >= 0) {
    int tag = nyr_load_local_value(b, local->dyn_tag_slot);
    if (tag < 0 || !nyr_record_dyn_fact(b, v, NYR_FACT_DYN_TAG, tag))
      return -1;
  }
  /*
   * Some shared typed-buffer values (notably sets) can acquire the broad
   * list-shaped lowering hint without owning a companion list-length slot.
   * A missing companion is not a local slot: emitting LOAD_LOCAL with -1
   * makes otherwise valid collection reads fail NYIR verification.  Only
   * attach the cached length fact when the slot was actually allocated.
   */
  if (local && local->is_list && local->list_len_slot >= 0) {
    int length =
        nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                          .dst = -1,
                                          .a = -1,
                                          .b = -1,
                                          .imm = local->list_len_slot});
    if (length < 0 || !nyr_record_list_len_fact(b, v, length)) {
      return nyr_fail(b, NY_NATIVE_ALLOC_FAIL), -1;
    }
  } else if (local && local->is_list && ny_dbg_is_enabled(NY_DBG_HEURISTIC)) {
    ny_dbg_heuristic_event("native-abi",
                           "function=%s local=%s type=%s "
                           "decision=skip-unallocated-list-length-slot",
                           b->current_fn_name ? b->current_fn_name : "<entry>",
                           local->name ? local->name : "<unnamed>",
                           local->type_name ? local->type_name : "<inferred>");
  }
  return v;
}

static int nyr_emit_is_zero(nyr_builder_impl_t *b, int value) {
  int zero = nyr_emit_const(b, 0);
  if (zero < 0)
    return -1;
  int v = nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_CMP_I64,
                                            .dst = -1,
                                            .a = value,
                                            .b = zero,
                                            .cmp = NYIR_CMP_EQ});
  if (v < 0)
    nyr_fail(b, NY_NATIVE_ALLOC_FAIL);
  return v;
}

static int nyr_lower_logical(nyr_builder_impl_t *b, const expr_t *left,
                             const expr_t *right, bool is_or);
static int nyr_lower_ternary(nyr_builder_impl_t *b, const expr_t *cond,
                             const expr_t *true_expr, const expr_t *false_expr);
static int nyr_lower_coalesce(nyr_builder_impl_t *b, const expr_t *coalesce,
                              const expr_t *left, const expr_t *fallback);
static bool nyr_lower_match(nyr_builder_impl_t *b, const stmt_t *s);

static int nyr_lower_expr(nyr_builder_impl_t *b, const expr_t *e);
static int nyr_lower_binary(nyr_builder_impl_t *b, const expr_t *e);
static int nyr_lower_call(nyr_builder_impl_t *b, const expr_t *e);

#define NY_NATIVE_ASM_MAX_OPERANDS 32
#define NY_NATIVE_ASM_MAX_TOKEN 96
#define NY_NATIVE_ASM_MAX_TEMPLATE 4096

typedef struct {
  bool output;
  bool input;
  bool clobber;
  bool initialized;
  bool memory;
  bool immediate;
  int match;
  int value;
  unsigned bits;
  char fixed[16];
} ny_native_asm_operand_t;

typedef struct {
  ny_native_asm_operand_t operands[NY_NATIVE_ASM_MAX_OPERANDS];
  size_t count;
  int result;
} ny_native_asm_state_t;

/*
 * Memoized layout-stmt lookups per (compile, root, name): lowering resolves
 * layout constructors and member types on (nearly) every typed expression,
 * and each uncached query walked the whole program subtree.  Mirrors the
 * refine-stmt memo below.
 */
static const stmt_t *nyr_find_layout_stmt_impl(const nyr_builder_impl_t *b,
                                               const stmt_t *s,
                                               const char *name);

static const stmt_t *nyr_find_layout_stmt(const nyr_builder_impl_t *b,
                                          const stmt_t *s, const char *name) {
  if (!b || !s || !name)
    return NULL;
  nyr_builder_impl_t *owner =
      b->cache_owner ? b->cache_owner : (nyr_builder_t *)b;
  if (owner->layout_stats_enabled)
    owner->layout_queries++;
  if (!owner->layout_cache) {
    owner->layout_cache =
        calloc(NY_LAYOUT_CACHE_SIZE, sizeof(*owner->layout_cache));
    if (owner->layout_cache)
      owner->layout_cache_cap = NY_LAYOUT_CACHE_SIZE;
  }
  if (!owner->layout_cache) {
    if (owner->layout_stats_enabled) {
      owner->layout_cache_misses++;
      owner->layout_uncached_scans++;
    }
    return nyr_find_layout_stmt_impl(b, s, name);
  }
  if (strlen(name) >= sizeof(((ny_layout_cache_entry_t *)0)->name)) {
    if (owner->layout_stats_enabled) {
      owner->layout_cache_misses++;
      owner->layout_uncached_scans++;
    }
    return nyr_find_layout_stmt_impl(b, s, name);
  }
  const program_t *prog = b->prog;
  uint32_t h = 2166136261u;
  for (const char *p = name; *p; ++p)
    h = (h ^ (uint8_t)*p) * 16777619u;
  h ^= (uint32_t)((uintptr_t)s >> 3);
  for (int probe = 0; probe < 8; ++probe) {
    ny_layout_cache_entry_t *hit =
        &owner->layout_cache[(h + (uint32_t)probe) % NY_LAYOUT_CACHE_SIZE];
    if (!hit->resolved)
      break;
    if (hit->prog == prog && hit->root == s && strcmp(hit->name, name) == 0) {
      if (owner->layout_stats_enabled)
        owner->layout_cache_hits++;
      return hit->result;
    }
  }
  if (owner->layout_stats_enabled) {
    owner->layout_cache_misses++;
    owner->layout_uncached_scans++;
  }
  const stmt_t *res = nyr_find_layout_stmt_impl(b, s, name);
  size_t insert = h % NY_LAYOUT_CACHE_SIZE;
  for (int probe = 0; probe < 8; ++probe) {
    size_t candidate = (h + (uint32_t)probe) % NY_LAYOUT_CACHE_SIZE;
    if (!owner->layout_cache[candidate].resolved) {
      insert = candidate;
      break;
    }
  }
  ny_layout_cache_entry_t *slot = &owner->layout_cache[insert];
  slot->prog = prog;
  slot->root = s;
  snprintf(slot->name, sizeof(slot->name), "%s", name);
  slot->result = res;
  slot->resolved = true;
  return res;
}

static const stmt_t *nyr_find_layout_stmt_impl(const nyr_builder_impl_t *b,
                                               const stmt_t *s,
                                               const char *name) {
  if (!s || !name)
    return NULL;
  const char *candidate = s->kind == NY_S_LAYOUT   ? s->as.layout.name
                          : s->kind == NY_S_STRUCT ? s->as.struc.name
                                                   : NULL;
  if (candidate && ny_native_name_matches(candidate, name))
    return s;
  const ny_stmt_list *body = s->kind == NY_S_MODULE  ? &s->as.module.body
                             : s->kind == NY_S_BLOCK ? &s->as.block.body
                                                     : NULL;
  if (body)
    for (size_t i = 0; i < body->len; ++i) {
      const stmt_t *found = nyr_find_layout_stmt(b, body->data[i], name);
      if (found)
        return found;
    }
  if (s->kind == NY_S_IF) {
    const stmt_t *found = nyr_find_layout_stmt(b, s->as.iff.conseq, name);
    if (found)
      return found;
    return nyr_find_layout_stmt(b, s->as.iff.alt, name);
  }
  return NULL;
}

/*
 * Memoized refine-stmt lookups per (compile, root, name): call lowering
 * checks refinement obligations on every call, and each check walked the
 * whole program subtree for the refine declaration.
 */
static const stmt_t *nyr_find_refine_stmt_impl(const nyr_builder_impl_t *b,
                                               const stmt_t *s,
                                               const char *name);

static const stmt_t *nyr_find_refine_stmt(const nyr_builder_impl_t *b,
                                          const stmt_t *s, const char *name) {
  if (!b || !s || !name)
    return NULL;
  nyr_builder_impl_t *owner =
      b->cache_owner ? b->cache_owner : (nyr_builder_t *)b;
  if (strlen(name) >= sizeof(((ny_refine_cache_entry_t *)0)->name))
    return nyr_find_refine_stmt_impl(b, s, name);
  if (!owner->refine_cache) {
    owner->refine_cache =
        calloc(NY_REFINE_CACHE_SIZE, sizeof(*owner->refine_cache));
    if (owner->refine_cache)
      owner->refine_cache_cap = NY_REFINE_CACHE_SIZE;
  }
  if (!owner->refine_cache)
    return nyr_find_refine_stmt_impl(b, s, name);
  const program_t *prog = b->prog;
  uint32_t h = 2166136261u;
  for (const char *p = name; *p; ++p)
    h = (h ^ (uint8_t)*p) * 16777619u;
  h ^= (uint32_t)((uintptr_t)s >> 3);
  for (int probe = 0; probe < 8; ++probe) {
    ny_refine_cache_entry_t *hit =
        &owner->refine_cache[(h + (uint32_t)probe) % NY_REFINE_CACHE_SIZE];
    if (!hit->resolved)
      break;
    if (hit->prog == prog && hit->root == s && strcmp(hit->name, name) == 0)
      return hit->result;
  }
  const stmt_t *res = nyr_find_refine_stmt_impl(b, s, name);
  size_t insert = h % NY_REFINE_CACHE_SIZE;
  for (int probe = 0; probe < 8; ++probe) {
    size_t candidate = (h + (uint32_t)probe) % NY_REFINE_CACHE_SIZE;
    if (!owner->refine_cache[candidate].resolved) {
      insert = candidate;
      break;
    }
  }
  ny_refine_cache_entry_t *slot = &owner->refine_cache[insert];
  slot->prog = prog;
  slot->root = s;
  snprintf(slot->name, sizeof(slot->name), "%s", name);
  slot->result = res;
  slot->resolved = true;
  return res;
}

static const stmt_t *nyr_find_refine_stmt_impl(const nyr_builder_impl_t *b,
                                               const stmt_t *s,
                                               const char *name) {
  if (!s || !name)
    return NULL;
  if (s->kind == NY_S_REFINE && s->as.refine.name &&
      ny_native_name_matches(s->as.refine.name, name))
    return s;
  const ny_stmt_list *body = s->kind == NY_S_MODULE  ? &s->as.module.body
                             : s->kind == NY_S_BLOCK ? &s->as.block.body
                                                     : NULL;
  if (body) {
    for (size_t i = 0; i < body->len; ++i) {
      const stmt_t *found = nyr_find_refine_stmt(b, body->data[i], name);
      if (found)
        return found;
    }
  }
  if (s->kind == NY_S_IF) {
    const stmt_t *found = nyr_find_refine_stmt(b, s->as.iff.conseq, name);
    if (found)
      return found;
    return nyr_find_refine_stmt(b, s->as.iff.alt, name);
  }
  return NULL;
}

/*
 * Whole-program layer for the layout/refine/module walks. The per-walk
 * memos above key on (prog, root, name), but every query site loops over
 * the flattened program's top-level statements calling the walk once per
 * statement — one hash+probe+strcmp round per top-level statement, per
 * query.  Memo the whole-program query (prog, name, walk kind) so a
 * repeated lookup is a single probe.  Keyed on the program pointer, the
 * same identity envelope the per-root memos already use.
 */
static ny_prog_query_entry_t *
ny_native_prog_query_slot(const nyr_builder_impl_t *b, const program_t *prog,
                          const char *name, uint8_t kind, const char *src) {
  if (!b || !prog || !name ||
      strlen(name) >= sizeof(((ny_prog_query_entry_t *)0)->name))
    return NULL;
  if (!src)
    src = "";
  if (strlen(src) >= sizeof(((ny_prog_query_entry_t *)0)->src))
    return NULL;
  nyr_builder_impl_t *owner =
      b->cache_owner ? b->cache_owner : (nyr_builder_t *)b;
  if (!owner->prog_query_stats_initialized) {
    owner->prog_query_stats_enabled = getenv("NYTRIX_TRACE_LOOKUPS") != NULL;
    owner->prog_query_stats_initialized = true;
  }
  if (owner->prog_query_stats_enabled)
    owner->prog_query_queries++;
  if (!owner->prog_query_cache) {
    owner->prog_query_cache =
        calloc(NY_PROG_QUERY_CACHE_SIZE, sizeof(*owner->prog_query_cache));
    if (owner->prog_query_cache)
      owner->prog_query_cache_cap = NY_PROG_QUERY_CACHE_SIZE;
  }
  if (!owner->prog_query_cache) {
    if (owner->prog_query_stats_enabled)
      owner->prog_query_misses++;
    return NULL;
  }
  uint32_t h = 2166136261u;
  for (const char *p = name; *p; ++p)
    h = (h ^ (uint8_t)*p) * 16777619u;
  for (const char *p = src; *p; ++p)
    h = (h ^ (uint8_t)*p) * 16777619u;
  h ^= (uint32_t)((uintptr_t)prog >> 4);
  h = h * 16777619u ^ (uint32_t)kind * 2654435761u;
  for (uint32_t probe = 0; probe < 8; ++probe) {
    ny_prog_query_entry_t *hit =
        &owner->prog_query_cache[(h + probe) % NY_PROG_QUERY_CACHE_SIZE];
    if (!hit->resolved)
      break;
    if (hit->kind == kind && hit->prog == prog &&
        strcmp(hit->name, name) == 0 && strcmp(hit->src, src) == 0) {
      if (owner->prog_query_stats_enabled)
        owner->prog_query_hits++;
      return hit;
    }
  }
  if (owner->prog_query_stats_enabled)
    owner->prog_query_misses++;
  return NULL;
}

static void ny_native_prog_query_store(const nyr_builder_impl_t *b,
                                       const program_t *prog, const char *name,
                                       uint8_t kind, const char *src,
                                       const stmt_t *result) {
  if (!b || !prog || !name ||
      strlen(name) >= sizeof(((ny_prog_query_entry_t *)0)->name))
    return;
  if (!src)
    src = "";
  if (strlen(src) >= sizeof(((ny_prog_query_entry_t *)0)->src))
    return;
  nyr_builder_impl_t *owner =
      b->cache_owner ? b->cache_owner : (nyr_builder_t *)b;
  if (!owner->prog_query_cache) {
    owner->prog_query_cache =
        calloc(NY_PROG_QUERY_CACHE_SIZE, sizeof(*owner->prog_query_cache));
    if (owner->prog_query_cache)
      owner->prog_query_cache_cap = NY_PROG_QUERY_CACHE_SIZE;
  }
  if (!owner->prog_query_cache)
    return;
  uint32_t h = 2166136261u;
  for (const char *p = name; *p; ++p)
    h = (h ^ (uint8_t)*p) * 16777619u;
  for (const char *p = src; *p; ++p)
    h = (h ^ (uint8_t)*p) * 16777619u;
  h ^= (uint32_t)((uintptr_t)prog >> 4);
  h = h * 16777619u ^ (uint32_t)kind * 2654435761u;
  size_t insert = h % NY_PROG_QUERY_CACHE_SIZE;
  bool found_empty = false;
  for (uint32_t probe = 0; probe < 8; ++probe) {
    size_t candidate = (h + probe) % NY_PROG_QUERY_CACHE_SIZE;
    if (!owner->prog_query_cache[candidate].resolved) {
      insert = candidate;
      found_empty = true;
      break;
    }
  }
  if (!found_empty && owner->prog_query_stats_enabled)
    owner->prog_query_evictions++;
  ny_prog_query_entry_t *slot = &owner->prog_query_cache[insert];
  slot->prog = prog;
  snprintf(slot->name, sizeof(slot->name), "%s", name);
  snprintf(slot->src, sizeof(slot->src), "%s", src);
  slot->result = result;
  slot->kind = kind;
  slot->resolved = true;
}

static int ny_layout_index_entry_cmp(const void *left, const void *right) {
  const ny_layout_name_index_entry_t *a = left;
  const ny_layout_name_index_entry_t *b = right;
  int order = strcmp(a->key, b->key);
  if (order)
    return order;
  return a->source_order < b->source_order   ? -1
         : a->source_order > b->source_order ? 1
                                             : 0;
}

typedef struct {
  ny_layout_name_index_entry_t *entries;
  size_t len;
  size_t cap;
  size_t source_order;
} ny_layout_index_builder_t;

static bool ny_layout_index_add(ny_layout_index_builder_t *builder,
                                const stmt_t *stmt, const char *name) {
  if (!builder || !stmt || !name)
    return true;
  for (const char *key = name; *key;) {
    if (builder->len == builder->cap) {
      size_t cap = builder->cap ? builder->cap * 2u : 256u;
      if (cap < builder->cap || cap > SIZE_MAX / sizeof(*builder->entries))
        return false;
      ny_layout_name_index_entry_t *entries =
          realloc(builder->entries, cap * sizeof(*entries));
      if (!entries)
        return false;
      builder->entries = entries;
      builder->cap = cap;
    }
    builder->entries[builder->len++] = (ny_layout_name_index_entry_t){
        .key = key, .stmt = stmt, .source_order = builder->source_order};
    const char *dot = strchr(key, '.');
    if (!dot)
      break;
    key = dot + 1;
  }
  builder->source_order++;
  return true;
}

/*
 * Preserve the exact preorder used by find_layout_stmt_impl().
 */
static bool ny_layout_index_collect_stmt(ny_layout_index_builder_t *builder,
                                         const stmt_t *stmt) {
  if (!stmt)
    return true;
  if (stmt->kind == NY_S_LAYOUT)
    return ny_layout_index_add(builder, stmt, stmt->as.layout.name);
  if (stmt->kind == NY_S_STRUCT)
    return ny_layout_index_add(builder, stmt, stmt->as.struc.name);
  const ny_stmt_list *body = stmt->kind == NY_S_MODULE  ? &stmt->as.module.body
                             : stmt->kind == NY_S_BLOCK ? &stmt->as.block.body
                                                        : NULL;
  if (body) {
    for (size_t i = 0; i < body->len; ++i)
      if (!ny_layout_index_collect_stmt(builder, body->data[i]))
        return false;
  }
  if (stmt->kind == NY_S_IF &&
      (!ny_layout_index_collect_stmt(builder, stmt->as.iff.conseq) ||
       !ny_layout_index_collect_stmt(builder, stmt->as.iff.alt)))
    return false;
  return true;
}

static bool ny_native_layout_index_build(nyr_builder_impl_t *owner,
                                         const program_t *prog) {
  if (!owner || !prog)
    return false;
  if (owner->layout_name_index_prog != prog) {
    free(owner->layout_name_index);
    owner->layout_name_index = NULL;
    owner->layout_name_index_len = 0;
    owner->layout_name_index_prog = prog;
    owner->layout_name_index_state = 0;
  }
  if (owner->layout_name_index_state != 0)
    return owner->layout_name_index_state == 1;
  ny_layout_index_builder_t builder = {0};
  bool ok = true;
  for (size_t i = 0; i < prog->body.len && ok; ++i)
    ok = ny_layout_index_collect_stmt(&builder, prog->body.data[i]);
  if (!ok) {
    free(builder.entries);
    owner->layout_name_index_state = 2;
    return false;
  }
  if (builder.len > 1) {
    qsort(builder.entries, builder.len, sizeof(*builder.entries),
          ny_layout_index_entry_cmp);
    size_t unique = 0;
    for (size_t i = 0; i < builder.len; ++i) {
      if (unique &&
          strcmp(builder.entries[unique - 1].key, builder.entries[i].key) == 0)
        continue;
      builder.entries[unique++] = builder.entries[i];
    }
    builder.len = unique;
  }
  owner->layout_name_index = builder.entries;
  owner->layout_name_index_len = builder.len;
  owner->layout_name_index_state = 1;
  return true;
}

static const ny_layout_name_index_entry_t *
ny_native_layout_index_find(const nyr_builder_impl_t *owner, const char *name) {
  if (!owner || !name || owner->layout_name_index_state != 1)
    return NULL;
  size_t low = 0, high = owner->layout_name_index_len;
  while (low < high) {
    size_t mid = low + (high - low) / 2u;
    int cmp = strcmp(owner->layout_name_index[mid].key, name);
    if (cmp < 0)
      low = mid + 1u;
    else
      high = mid;
  }
  return low < owner->layout_name_index_len &&
                 strcmp(owner->layout_name_index[low].key, name) == 0
             ? &owner->layout_name_index[low]
             : NULL;
}

static const stmt_t *
ny_native_layout_index_lookup(const nyr_builder_impl_t *owner,
                              const char *name) {
  const ny_layout_name_index_entry_t *full =
      ny_native_layout_index_find(owner, name);
  const char *dot = strrchr(name, '.');
  const ny_layout_name_index_entry_t *leaf =
      dot ? ny_native_layout_index_find(owner, dot + 1) : NULL;
  if (!full)
    return leaf ? leaf->stmt : NULL;
  if (!leaf || full->source_order <= leaf->source_order)
    return full->stmt;
  return leaf->stmt;
}

static const stmt_t *nyr_find_layout_in_prog(const nyr_builder_impl_t *b,
                                             const program_t *prog,
                                             const char *name) {
  nyr_builder_impl_t *owner =
      b && b->cache_owner ? b->cache_owner : (nyr_builder_t *)b;
  if (owner && owner->layout_stats_enabled)
    owner->layout_prog_queries++;
  ny_prog_query_entry_t *hit =
      ny_native_prog_query_slot(b, prog, name, 0, NULL);
  if (hit) {
    if (owner && owner->layout_stats_enabled)
      owner->layout_prog_cache_hits++;
    return hit->result;
  }
  if (owner && owner->layout_stats_enabled) {
    owner->layout_prog_cache_misses++;
  }
  const stmt_t *res = NULL;
  if (prog && name && owner && ny_native_layout_index_build(owner, prog)) {
    if (owner->layout_stats_enabled)
      owner->layout_prog_index_lookups++;
    res = ny_native_layout_index_lookup(owner, name);
  } else if (prog && name) {
    if (owner && owner->layout_stats_enabled)
      owner->layout_prog_scans++;
    for (size_t i = 0; i < prog->body.len && !res; ++i)
      res = nyr_find_layout_stmt(b, prog->body.data[i], name);
  }
  ny_native_prog_query_store(b, prog, name, 0, NULL, res);
  return res;
}

static const stmt_t *nyr_find_refine_in_prog(const nyr_builder_impl_t *b,
                                             const program_t *prog,
                                             const char *name) {
  ny_prog_query_entry_t *hit =
      ny_native_prog_query_slot(b, prog, name, 1, NULL);
  if (hit)
    return hit->result;
  const stmt_t *res = NULL;
  if (prog && name) {
    for (size_t i = 0; i < prog->body.len && !res; ++i)
      res = nyr_find_refine_stmt(b, prog->body.data[i], name);
  }
  ny_native_prog_query_store(b, prog, name, 1, NULL, res);
  return res;
}

static const stmt_t *nyr_find_module_walk_in_prog(const nyr_builder_impl_t *b,
                                                  const program_t *prog,
                                                  const char *name) {
  ny_prog_query_entry_t *hit =
      ny_native_prog_query_slot(b, prog, name, 2, NULL);
  if (hit)
    return hit->result;
  const stmt_t *res = NULL;
  if (prog && name) {
    if (b && prog == b->prog)
      res = nyr_find_module_in_prog(ny_native_fn_index_for(b), prog, name);
    for (size_t i = 0; i < prog->body.len && !res; ++i)
      res = nyr_find_module(prog->body.data[i], name, 0);
  }
  ny_native_prog_query_store(b, prog, name, 2, NULL, res);
  return res;
}

typedef struct {
  ny_top_var_index_entry_t *entries;
  size_t len;
  size_t cap;
  size_t source_order;
} ny_top_var_index_builder_t;

static bool ny_top_var_index_collect(ny_top_var_index_builder_t *builder,
                                     const stmt_t *stmt, unsigned depth) {
  if (!stmt || depth > 64)
    return true;
  if (stmt->kind == NY_S_VAR) {
    for (size_t i = 0; i < stmt->as.var.names.len; ++i) {
      const char *name = stmt->as.var.names.data[i];
      if (!name)
        continue;
      if (builder->len == builder->cap) {
        size_t cap = builder->cap ? builder->cap * 2u : 256u;
        if (cap < builder->cap || cap > SIZE_MAX / sizeof(*builder->entries))
          return false;
        ny_top_var_index_entry_t *entries =
            realloc(builder->entries, cap * sizeof(*entries));
        if (!entries)
          return false;
        builder->entries = entries;
        builder->cap = cap;
      }
      builder->entries[builder->len++] =
          (ny_top_var_index_entry_t){.key = name,
                                     .source_file = stmt->tok.filename,
                                     .stmt = stmt,
                                     .source_order = builder->source_order++};
    }
    return true;
  }
  const ny_stmt_list *body = stmt->kind == NY_S_MODULE  ? &stmt->as.module.body
                             : stmt->kind == NY_S_BLOCK ? &stmt->as.block.body
                                                        : NULL;
  if (body) {
    for (size_t i = 0; i < body->len; ++i)
      if (!ny_top_var_index_collect(builder, body->data[i], depth + 1))
        return false;
  } else if (stmt->kind == NY_S_IF) {
    return ny_top_var_index_collect(builder, stmt->as.iff.conseq, depth + 1) &&
           ny_top_var_index_collect(builder, stmt->as.iff.alt, depth + 1);
  }
  return true;
}

static int ny_top_var_index_entry_cmp(const void *left, const void *right) {
  const ny_top_var_index_entry_t *a = left;
  const ny_top_var_index_entry_t *b = right;
  int order = strcmp(a->key, b->key);
  if (order)
    return order;
  return a->source_order < b->source_order   ? -1
         : a->source_order > b->source_order ? 1
                                             : 0;
}

static bool ny_native_top_var_index_build(nyr_builder_impl_t *owner,
                                          const program_t *prog) {
  if (!owner || !prog)
    return false;
  if (owner->top_var_index_prog != prog) {
    free(owner->top_var_index);
    owner->top_var_index = NULL;
    owner->top_var_index_len = 0;
    owner->top_var_index_prog = prog;
    owner->top_var_index_state = 0;
  }
  if (owner->top_var_index_state != 0)
    return owner->top_var_index_state == 1;
  ny_top_var_index_builder_t builder = {0};
  bool ok = true;
  for (size_t i = 0; i < prog->body.len && ok; ++i)
    ok = ny_top_var_index_collect(&builder, prog->body.data[i], 0);
  if (!ok) {
    free(builder.entries);
    owner->top_var_index_state = 2;
    return false;
  }
  if (builder.len > 1)
    qsort(builder.entries, builder.len, sizeof(*builder.entries),
          ny_top_var_index_entry_cmp);
  owner->top_var_index = builder.entries;
  owner->top_var_index_len = builder.len;
  owner->top_var_index_state = 1;
  return true;
}

static const ny_top_var_index_entry_t *
ny_native_top_var_index_find(const nyr_builder_impl_t *owner, const char *name,
                             const char *source_file) {
  if (!owner || !name || owner->top_var_index_state != 1)
    return NULL;
  size_t low = 0, high = owner->top_var_index_len;
  while (low < high) {
    size_t mid = low + (high - low) / 2u;
    int cmp = strcmp(owner->top_var_index[mid].key, name);
    if (cmp < 0)
      low = mid + 1u;
    else
      high = mid;
  }
  for (size_t i = low; i < owner->top_var_index_len &&
                       strcmp(owner->top_var_index[i].key, name) == 0;
       ++i) {
    const ny_top_var_index_entry_t *entry = &owner->top_var_index[i];
    if (!source_file ||
        (entry->source_file && strcmp(entry->source_file, source_file) == 0))
      return entry;
  }
  return NULL;
}

static const stmt_t *nyr_find_top_level_var_in_prog(const nyr_builder_impl_t *b,
                                                    const program_t *prog,
                                                    const char *name) {
  ny_prog_query_entry_t *hit =
      ny_native_prog_query_slot(b, prog, name, 3, NULL);
  if (hit)
    return hit->result;
  const stmt_t *res = NULL;
  if (prog && name) {
    nyr_builder_impl_t *owner =
        b && b->cache_owner ? b->cache_owner : (nyr_builder_t *)b;
    if (owner && ny_native_top_var_index_build(owner, prog)) {
      const ny_top_var_index_entry_t *entry =
          ny_native_top_var_index_find(owner, name, NULL);
      res = entry ? entry->stmt : NULL;
    } else {
      for (size_t i = 0; i < prog->body.len && !res; ++i)
        res = nyr_find_top_level_var_in_stmt(prog->body.data[i], name, 0);
    }
  }
  ny_native_prog_query_store(b, prog, name, 3, NULL, res);
  return res;
}

static const stmt_t *nyr_find_top_level_var_in_source_in_prog(
    const nyr_builder_impl_t *b, const program_t *prog, const char *name,
    const char *source_file) {
  ny_prog_query_entry_t *hit =
      ny_native_prog_query_slot(b, prog, name, 4, source_file);
  if (hit)
    return hit->result;
  const stmt_t *res = NULL;
  if (prog && name && source_file) {
    nyr_builder_impl_t *owner =
        b && b->cache_owner ? b->cache_owner : (nyr_builder_t *)b;
    if (owner && ny_native_top_var_index_build(owner, prog)) {
      const ny_top_var_index_entry_t *entry =
          ny_native_top_var_index_find(owner, name, source_file);
      res = entry ? entry->stmt : NULL;
    } else {
      for (size_t i = 0; i < prog->body.len && !res; ++i)
        res = nyr_find_top_level_var_in_source_stmt(prog->body.data[i], name,
                                                    source_file, 0);
    }
  }
  ny_native_prog_query_store(b, prog, name, 4, source_file, res);
  return res;
}

typedef struct {
  ny_extern_index_entry_t *entries;
  size_t len;
  size_t cap;
  size_t source_order;
  const ny_options *opt;
} ny_extern_index_builder_t;

static bool ny_extern_index_add(ny_extern_index_builder_t *builder,
                                const stmt_t *stmt) {
  if (!builder || !stmt || !stmt->as.ext.name)
    return true;
  for (const char *key = stmt->as.ext.name; *key;) {
    if (builder->len == builder->cap) {
      size_t cap = builder->cap ? builder->cap * 2u : 128u;
      if (cap < builder->cap || cap > SIZE_MAX / sizeof(*builder->entries))
        return false;
      ny_extern_index_entry_t *entries =
          realloc(builder->entries, cap * sizeof(*entries));
      if (!entries)
        return false;
      builder->entries = entries;
      builder->cap = cap;
    }
    builder->entries[builder->len++] = (ny_extern_index_entry_t){
        .key = key, .stmt = stmt, .source_order = builder->source_order};
    const char *dot = strchr(key, '.');
    if (!dot)
      break;
    key = dot + 1;
  }
  builder->source_order++;
  return true;
}

static bool ny_extern_index_collect(ny_extern_index_builder_t *builder,
                                    const stmt_t *stmt, unsigned depth) {
  if (!stmt || depth > 64)
    return true;
  if (stmt->kind == NY_S_EXTERN)
    return ny_extern_index_add(builder, stmt);
  if (stmt->kind == NY_S_MODULE || stmt->kind == NY_S_BLOCK) {
    const ny_stmt_list *body = stmt->kind == NY_S_MODULE ? &stmt->as.module.body
                                                         : &stmt->as.block.body;
    for (size_t i = 0; i < body->len; ++i)
      if (!ny_extern_index_collect(builder, body->data[i], depth + 1))
        return false;
  } else if (stmt->kind == NY_S_IF) {
    bool selected = false;
    if (stmt->as.iff.test && stmt->as.iff.test->kind == NY_E_COMPTIME &&
        nyr_target_eval_bool(builder->opt, stmt->as.iff.test, &selected))
      return ny_extern_index_collect(
          builder, selected ? stmt->as.iff.conseq : stmt->as.iff.alt,
          depth + 1);
    return ny_extern_index_collect(builder, stmt->as.iff.conseq, depth + 1) &&
           ny_extern_index_collect(builder, stmt->as.iff.alt, depth + 1);
  } else if (stmt->kind == NY_S_WHILE) {
    return ny_extern_index_collect(builder, stmt->as.whl.init, depth + 1) &&
           ny_extern_index_collect(builder, stmt->as.whl.body, depth + 1) &&
           ny_extern_index_collect(builder, stmt->as.whl.update, depth + 1);
  } else if (stmt->kind == NY_S_FOR) {
    return ny_extern_index_collect(builder, stmt->as.fr.init, depth + 1) &&
           ny_extern_index_collect(builder, stmt->as.fr.body, depth + 1) &&
           ny_extern_index_collect(builder, stmt->as.fr.update, depth + 1);
  } else if (stmt->kind == NY_S_TRY) {
    return ny_extern_index_collect(builder, stmt->as.tr.body, depth + 1) &&
           ny_extern_index_collect(builder, stmt->as.tr.handler, depth + 1);
  } else if (stmt->kind == NY_S_DEFER) {
    return ny_extern_index_collect(builder, stmt->as.de.body, depth + 1);
  } else if (stmt->kind == NY_S_MATCH) {
    for (size_t i = 0; i < stmt->as.match.arms.len; ++i)
      if (!ny_extern_index_collect(builder, stmt->as.match.arms.data[i].conseq,
                                   depth + 1))
        return false;
    return ny_extern_index_collect(builder, stmt->as.match.default_conseq,
                                   depth + 1);
  }
  return true;
}

static int ny_extern_index_entry_cmp(const void *left, const void *right) {
  const ny_extern_index_entry_t *a = left;
  const ny_extern_index_entry_t *b = right;
  int order = strcmp(a->key, b->key);
  if (order)
    return order;
  return a->source_order < b->source_order   ? -1
         : a->source_order > b->source_order ? 1
                                             : 0;
}

static bool ny_native_extern_index_build(nyr_builder_impl_t *owner,
                                         const program_t *prog,
                                         const ny_options *opt) {
  if (!owner || !prog)
    return false;
  if (owner->extern_index_prog != prog || owner->extern_index_opt != opt) {
    free(owner->extern_index);
    owner->extern_index = NULL;
    owner->extern_index_len = 0;
    owner->extern_index_prog = prog;
    owner->extern_index_opt = opt;
    owner->extern_index_state = 0;
  }
  if (owner->extern_index_state != 0)
    return owner->extern_index_state == 1;
  ny_extern_index_builder_t builder = {.opt = opt};
  bool ok = true;
  for (size_t i = 0; i < prog->body.len && ok; ++i)
    ok = ny_extern_index_collect(&builder, prog->body.data[i], 0);
  if (!ok) {
    free(builder.entries);
    owner->extern_index_state = 2;
    return false;
  }
  if (builder.len > 1)
    qsort(builder.entries, builder.len, sizeof(*builder.entries),
          ny_extern_index_entry_cmp);
  owner->extern_index = builder.entries;
  owner->extern_index_len = builder.len;
  owner->extern_index_state = 1;
  return true;
}

static const ny_extern_index_entry_t *
ny_native_extern_index_find_key(const nyr_builder_impl_t *owner,
                                const char *key) {
  if (!owner || !key || owner->extern_index_state != 1)
    return NULL;
  size_t low = 0, high = owner->extern_index_len;
  while (low < high) {
    size_t mid = low + (high - low) / 2u;
    int cmp = strcmp(owner->extern_index[mid].key, key);
    if (cmp < 0)
      low = mid + 1u;
    else
      high = mid;
  }
  return low < owner->extern_index_len &&
                 strcmp(owner->extern_index[low].key, key) == 0
             ? &owner->extern_index[low]
             : NULL;
}

static const stmt_t *
ny_native_extern_index_find(const nyr_builder_impl_t *owner, const char *name) {
  const ny_extern_index_entry_t *match =
      ny_native_extern_index_find_key(owner, name);
  const char *leaf = ny_name_leaf(name);
  if (leaf && leaf != name) {
    const ny_extern_index_entry_t *leaf_match =
        ny_native_extern_index_find_key(owner, leaf);
    if (leaf_match &&
        (!match || leaf_match->source_order < match->source_order))
      match = leaf_match;
  }
  return match ? match->stmt : NULL;
}

static const stmt_t *nyr_find_extern_decl_in_prog(const nyr_builder_impl_t *b,
                                                  const program_t *prog,
                                                  const char *name,
                                                  const ny_options *opt) {
  ny_prog_query_entry_t *hit =
      ny_native_prog_query_slot(b, prog, name, 6, NULL);
  if (hit)
    return hit->result;
  const stmt_t *res = NULL;
  if (prog && name) {
    nyr_builder_impl_t *owner =
        b && b->cache_owner ? b->cache_owner : (nyr_builder_t *)b;
    if (owner && ny_native_extern_index_build(owner, prog, opt))
      res = ny_native_extern_index_find(owner, name);
    else
      for (size_t i = 0; i < prog->body.len && !res; ++i)
        res = nyr_find_extern_decl_in_stmt(prog->body.data[i], name, opt, 0);
  }
  ny_native_prog_query_store(b, prog, name, 6, NULL, res);
  return res;
}

static bool nyr_has_mutable_top_var_in_prog(const nyr_builder_impl_t *b,
                                            const program_t *prog,
                                            const char *name) {
  ny_prog_query_entry_t *hit =
      ny_native_prog_query_slot(b, prog, name, 5, NULL);
  if (hit)
    return hit->result != NULL;
  const stmt_t *res = NULL;
  if (prog && name) {
    for (size_t i = 0; i < prog->body.len && !res; ++i)
      if (nyr_has_mutable_top_var(prog->body.data[i], name, 0))
        res = prog->body.data[i];
  }
  ny_native_prog_query_store(b, prog, name, 5, NULL, res);
  return res != NULL;
}

static bool nyr_check_refine_obligation(nyr_builder_impl_t *b, token_t tok,
                                        const char *type_name, expr_t *expr) {
  if (!b || !b->prog || !type_name || !expr)
    return true;
  const stmt_t *r_stmt = nyr_find_refine_in_prog(b, b->prog, type_name);
  if (!r_stmt)
    return true;
  if (r_stmt->as.refine.base_type) {
    if (!nyr_check_refine_obligation(b, tok, r_stmt->as.refine.base_type, expr))
      return false;
  }
  expr_t *inst = ny_expr_subst(NULL, r_stmt->as.refine.predicate,
                               r_stmt->as.refine.param, expr);
  if (inst) {
    ny_ct_fast_val_t fval = ny_ct_fast_none();
    if (ny_try_eval_comptime_expr_fast(NULL, inst, &fval, 0)) {
      if (fval.kind == NY_CT_FAST_BOOL && !fval.b) {
        ny_diag_error(tok,
                      "refinement obligation failed: value does not satisfy "
                      "'%s' refinement predicate",
                      type_name);
        return nyr_fail(b,
                        "refinement obligation failed: value does "
                        "not satisfy '%s' refinement predicate",
                        type_name);
      }
    }
  }
  return true;
}

/*
 * Return the declared scalar type of a user-layout member.  This is kept
 * separate from the load lowering so representation queries (f64/f32) and
 * the actual memory access use the same source-level contract.
 */
static const char *nyr_layout_member_type(const nyr_builder_impl_t *b,
                                          const expr_t *member) {
  if (!b || !member || member->kind != NY_E_MEMBER ||
      !member->as.member.target || !member->as.member.name)
    return NULL;

  const expr_t *target = member->as.member.target;
  const char *type_name = nyr_expr_type_name(b, target);
  if ((!type_name || strcmp(type_name, "any") == 0) &&
      target->kind == NY_E_IDENT && target->as.ident.name) {
    const nyr_local_t *local =
        nyr_find_local((nyr_builder_t *)b, target->as.ident.name);
    if (local && local->type_name)
      type_name = local->type_name;
    if (!type_name || strcmp(type_name, "any") == 0) {
      const expr_t *value = nyr_find_top_level_value(b, target->as.ident.name);
      if (value && value != target)
        type_name = nyr_expr_type_name(b, value);
    }
  }
  if ((!type_name || strcmp(type_name, "any") == 0) &&
      target->kind == NY_E_CALL && target->as.call.callee &&
      target->as.call.callee->kind == NY_E_IDENT &&
      target->as.call.callee->as.ident.name && b->prog) {
    const stmt_t *ctor = nyr_find_layout_in_prog(
        b, b->prog, target->as.call.callee->as.ident.name);
    if (ctor)
      type_name = ctor->kind == NY_S_STRUCT ? ctor->as.struc.name
                                            : ctor->as.layout.name;
  }
  if (!type_name || !b->prog)
    return NULL;

  const stmt_t *layout = nyr_find_layout_in_prog(b, b->prog, type_name);
  if (!layout)
    return NULL;
  const ny_layout_field_list *fields = layout->kind == NY_S_STRUCT
                                           ? &layout->as.struc.fields
                                           : &layout->as.layout.fields;
  for (size_t i = 0; i < fields->len; ++i)
    if (fields->data[i].name &&
        strcmp(fields->data[i].name, member->as.member.name) == 0)
      return fields->data[i].type_name;
  return NULL;
}

/*
 * Builtin container words name a pointer-sized carrier only when the type
 * ends at the word or continues with a generic/array suffix; a user type
 * that merely starts with the same letters has its own layout.
 */
static bool nyr_container_word(const char *type, const char *word, size_t len) {
  if (strncmp(type, word, len) != 0)
    return false;
  char next = type[len];
  return next == '\0' || next == '<' || next == '[' ||
         isspace((unsigned char)next);
}

static bool nyr_primitive_layout(const char *type, size_t *size,
                                 size_t *align) {
  if (!type || !size || !align)
    return false;
  if (!strcmp(type, "bool") || !strcmp(type, "i8") || !strcmp(type, "u8"))
    *size = *align = 1;
  else if (!strcmp(type, "i16") || !strcmp(type, "u16"))
    *size = *align = 2;
  else if (!strcmp(type, "i32") || !strcmp(type, "u32") || !strcmp(type, "f32"))
    *size = *align = 4;
  else if (!strcmp(type, "i128") || !strcmp(type, "u128") ||
           !strcmp(type, "f128"))
    *size = *align = 16;
  else {
    ny_sem_rep_t rep = ny_semantic_rep_for_type(type);
    bool one_word = rep == NY_SEM_REP_STRING ||
                    rep == NY_SEM_REP_TAGGED_DYNAMIC ||
                    rep == NY_SEM_REP_POINTER;
    if (!strcmp(type, "i64") || !strcmp(type, "u64") || !strcmp(type, "int") ||
        !strcmp(type, "float") || !strcmp(type, "f64") ||
        !strcmp(type, "ptr") || one_word || !strcmp(type, "str") ||
        !strcmp(type, "string") || !strcmp(type, "bytes") ||
        !strcmp(type, "any") || type[0] == '*' || type[0] == '?' ||
        nyr_container_word(type, "list", 4) ||
        nyr_container_word(type, "dict", 4) ||
        nyr_container_word(type, "set", 3))
      *size = *align = 8;
    else
      return false;
  }
  return true;
}

static size_t nyr_align_up(size_t n, size_t a) {
  return a > 1 ? (n + a - 1) / a * a : n;
}

static bool nyr_ast_layout_query_depth(const nyr_builder_impl_t *b,
                                       const char *requested,
                                       const char *field_name, size_t *size_out,
                                       size_t *align_out, size_t *offset_out,
                                       unsigned depth) {
  if (!b || !b->prog || !requested || depth >= 64)
    return false;
  char base[256];
  const char *lt = strchr(requested, '<');
  size_t len = lt ? (size_t)(lt - requested) : strlen(requested);
  if (!len || len >= sizeof(base))
    return false;
  memcpy(base, requested, len);
  base[len] = '\0';

  const ny_native_c_layout_table_t *c_layouts =
      b->externs ? &b->externs->layouts : NULL;
  const ny_native_c_layout_t *c_layout =
      ny_native_c_layout_lookup(c_layouts, base);
  /*
   * Prefer an explicit Nytrix layout declaration when both the C frontend
   * and the source program expose the same name.  The C table may describe
   * an opaque/external ABI view (for example libc's div_t), while
   * load_layout must honor the source field widths used by the caller.
   */
  bool has_ast_layout = nyr_find_layout_in_prog(b, b->prog, base) != NULL;
  if (c_layout && !has_ast_layout) {
    if (!field_name) {
      if (size_out)
        *size_out = c_layout->size;
      if (align_out)
        *align_out = c_layout->align;
      return true;
    }
    for (size_t i = 0; i < c_layout->field_count; ++i) {
      const ny_native_c_layout_field_t *field = &c_layout->fields[i];
      if (field->name && strcmp(field->name, field_name) == 0) {
        if (offset_out)
          *offset_out = field->offset;
        if (size_out)
          *size_out = field->size;
        if (align_out)
          *align_out = field->align;
        return true;
      }
    }
    return false;
  }

  const program_t *prog = b->prog;
  int64_t bound = lt ? strtoll(lt + 1, NULL, 10) : -1;
  const stmt_t *decl = nyr_find_layout_in_prog(b, prog, base);
  if (!decl)
    return false;
  const ny_layout_field_list *fields = decl->kind == NY_S_LAYOUT
                                           ? &decl->as.layout.fields
                                           : &decl->as.struc.fields;
  size_t pack =
      decl->kind == NY_S_LAYOUT ? decl->as.layout.pack : decl->as.struc.pack;
  size_t forced = decl->kind == NY_S_LAYOUT ? decl->as.layout.align_override
                                            : decl->as.struc.align_override;
  if (bound < 0 && decl->kind == NY_S_LAYOUT &&
      decl->as.layout.deftype_params.len) {
    const expr_t *def = decl->as.layout.deftype_params.data[0].def;
    if (def && def->kind == NY_E_LITERAL && def->as.literal.kind == NY_LIT_INT)
      bound = def->as.literal.as.i;
  }
  size_t offset = 0, aggregate_align = forced ? forced : 1;
  for (size_t i = 0; i < fields->len; ++i) {
    const layout_field_t *f = &fields->data[i];
    size_t elem_size = 0, elem_align = 0;
    if (!nyr_primitive_layout(f->type_name, &elem_size, &elem_align) &&
        !nyr_ast_layout_query_depth(b, f->type_name, NULL, &elem_size,
                                    &elem_align, NULL, depth + 1))
      return false;
    size_t count = 1;
    if (f->is_array) {
      if (f->array_len && f->array_len->kind == NY_E_LITERAL &&
          f->array_len->as.literal.kind == NY_LIT_INT)
        count = (size_t)f->array_len->as.literal.as.i;
      else if (bound > 0)
        count = (size_t)bound;
      else
        return false;
    }
    size_t fa = f->width > 0 ? (size_t)f->width : elem_align;
    if (pack && fa > pack)
      fa = pack;
    offset = nyr_align_up(offset, fa);
    if (field_name && f->name && !strcmp(f->name, field_name)) {
      if (offset_out)
        *offset_out = offset;
      if (size_out)
        *size_out = elem_size * count;
      if (align_out)
        *align_out = fa;
      return true;
    }
    offset += elem_size * count;
    if (fa > aggregate_align)
      aggregate_align = fa;
  }
  if (field_name)
    return false;
  if (forced)
    aggregate_align = forced;
  if (size_out)
    *size_out = nyr_align_up(offset, aggregate_align);
  if (align_out)
    *align_out = aggregate_align;
  return true;
}

static bool nyr_ast_layout_query(const nyr_builder_impl_t *b,
                                 const char *requested, const char *field_name,
                                 size_t *size_out, size_t *align_out,
                                 size_t *offset_out) {
  return nyr_ast_layout_query_depth(b, requested, field_name, size_out,
                                    align_out, offset_out, 0);
}

static bool ny_native_find_enum_item_in_stmt(
    const stmt_t *s, const char *enum_name, const char *member_name,
    size_t *enum_idx_counter, const stmt_t **out_enum,
    const stmt_enum_item_t **out_item, int64_t *out_tag) {
  if (!s)
    return false;
  if (s->kind == NY_S_ENUM) {
    size_t e_idx = (*enum_idx_counter)++;
    const stmt_enum_t *enu = &s->as.enu;
    if (enum_name && enu->name && strcmp(enu->name, enum_name) != 0)
      return false;
    int64_t base_tag = 200000 + (int64_t)e_idx * 1024;
    for (size_t j = 0; j < enu->items.len; ++j) {
      const stmt_enum_item_t *item = &enu->items.data[j];
      if (item->name && strcmp(item->name, member_name) == 0) {
        if (out_enum)
          *out_enum = s;
        if (out_item)
          *out_item = item;
        if (out_tag) {
          if (item->fields.len > 0) {
            *out_tag = base_tag + (int64_t)j;
          } else if (item->value && item->value->kind == NY_E_LITERAL &&
                     item->value->as.literal.kind == NY_LIT_INT) {
            *out_tag = item->value->as.literal.as.i;
          } else {
            *out_tag = (int64_t)j;
          }
        }
        return true;
      }
    }
    return false;
  }
  if (s->kind == NY_S_MODULE) {
    for (size_t i = 0; i < s->as.module.body.len; ++i) {
      if (ny_native_find_enum_item_in_stmt(s->as.module.body.data[i], enum_name,
                                           member_name, enum_idx_counter,
                                           out_enum, out_item, out_tag))
        return true;
    }
  }
  return false;
}

static bool
ny_native_enum_lookup_collect_stmt(ny_native_enum_lookup_entry_t **entries,
                                   size_t *len, size_t *cap, const stmt_t *s,
                                   size_t *enum_order) {
  if (!s)
    return true;
  if (s->kind == NY_S_ENUM) {
    size_t this_enum = (*enum_order)++;
    const stmt_enum_t *enu = &s->as.enu;
    for (size_t i = 0; i < enu->items.len; ++i) {
      const stmt_enum_item_t *item = &enu->items.data[i];
      if (!item->name)
        continue;
      if (*len == *cap) {
        size_t next = *cap ? *cap * 2u : 64u;
        if (next < *cap || next > SIZE_MAX / sizeof(**entries))
          return false;
        ny_native_enum_lookup_entry_t *grown =
            realloc(*entries, next * sizeof(**entries));
        if (!grown)
          return false;
        *entries = grown;
        *cap = next;
      }
      int64_t tag;
      if (item->fields.len > 0) {
        tag = 200000 + (int64_t)this_enum * 1024 + (int64_t)i;
      } else if (item->value && item->value->kind == NY_E_LITERAL &&
                 item->value->as.literal.kind == NY_LIT_INT) {
        tag = item->value->as.literal.as.i;
      } else {
        tag = (int64_t)i;
      }
      (*entries)[(*len)++] =
          (ny_native_enum_lookup_entry_t){.member_name = item->name,
                                          .enum_name = enu->name,
                                          .enum_stmt = s,
                                          .item = item,
                                          .tag = tag,
                                          .enum_order = this_enum,
                                          .item_order = i};
    }
    return true;
  }
  if (s->kind == NY_S_MODULE) {
    for (size_t i = 0; i < s->as.module.body.len; ++i)
      if (!ny_native_enum_lookup_collect_stmt(
              entries, len, cap, s->as.module.body.data[i], enum_order))
        return false;
  }
  return true;
}

static int ny_native_enum_lookup_cmp(const void *left, const void *right) {
  const ny_native_enum_lookup_entry_t *a = left;
  const ny_native_enum_lookup_entry_t *b = right;
  int member_cmp = strcmp(a->member_name, b->member_name);
  if (member_cmp)
    return member_cmp;
  if (a->enum_order < b->enum_order)
    return -1;
  if (a->enum_order > b->enum_order)
    return 1;
  if (a->item_order < b->item_order)
    return -1;
  return a->item_order > b->item_order;
}

static bool ny_native_enum_lookup_build(ny_native_fn_index_t *index,
                                        const program_t *prog) {
  if (!index || !prog)
    return false;
  if (index->enum_lookup_prog == prog)
    return index->enum_lookup_state == 1;
  free(index->enum_lookup);
  index->enum_lookup = NULL;
  index->enum_lookup_len = 0;
  index->enum_lookup_prog = prog;
  index->enum_lookup_state = 2;
  size_t cap = 0, enum_order = 0;
  for (size_t i = 0; i < prog->body.len; ++i) {
    if (!ny_native_enum_lookup_collect_stmt(&index->enum_lookup,
                                            &index->enum_lookup_len, &cap,
                                            prog->body.data[i], &enum_order)) {
      free(index->enum_lookup);
      index->enum_lookup = NULL;
      index->enum_lookup_len = 0;
      return false;
    }
  }
  if (index->enum_lookup_len > 1)
    qsort(index->enum_lookup, index->enum_lookup_len,
          sizeof(*index->enum_lookup), ny_native_enum_lookup_cmp);
  index->enum_lookup_state = 1;
  return true;
}

static bool ny_native_enum_lookup_find(const ny_native_fn_index_t *index,
                                       const char *enum_name,
                                       const char *member_name,
                                       const stmt_t **out_enum,
                                       const stmt_enum_item_t **out_item,
                                       int64_t *out_tag) {
  size_t low = 0, high = index->enum_lookup_len;
  while (low < high) {
    size_t mid = low + (high - low) / 2u;
    if (strcmp(index->enum_lookup[mid].member_name, member_name) < 0)
      low = mid + 1u;
    else
      high = mid;
  }
  for (size_t i = low;
       i < index->enum_lookup_len &&
       strcmp(index->enum_lookup[i].member_name, member_name) == 0;
       ++i) {
    const ny_native_enum_lookup_entry_t *entry = &index->enum_lookup[i];
    if (enum_name &&
        (!entry->enum_name || strcmp(entry->enum_name, enum_name) != 0))
      continue;
    if (out_enum)
      *out_enum = entry->enum_stmt;
    if (out_item)
      *out_item = entry->item;
    if (out_tag)
      *out_tag = entry->tag;
    return true;
  }
  return false;
}

static bool nyr_find_enum_member(const nyr_builder_impl_t *b, const char *name,
                                 const stmt_t **out_enum,
                                 const stmt_enum_item_t **out_item,
                                 int64_t *out_tag) {
  if (!b || !b->prog || !name || !*name)
    return false;
  if (b->module_name && *b->module_name) {
    size_t prefix_len = strlen(b->module_name);
    if (strncmp(name, b->module_name, prefix_len) != 0 ||
        name[prefix_len] != '.') {
      char owned[1280];
      int n = snprintf(owned, sizeof(owned), "%s.%s", b->module_name, name);
      if (n > 0 && (size_t)n < sizeof(owned) &&
          nyr_find_enum_member(b, owned, out_enum, out_item, out_tag))
        return true;
    }
  }
  ny_native_fn_index_t *index = ny_native_fn_index_for(b);
  ny_sym_id name_id = 0;
  ny_native_enum_member_entry_t *cache_entry = NULL;
  if (index) {
    if (!index->trace_lookup_stats_initialized) {
      index->trace_lookup_stats = getenv("NYTRIX_TRACE_LOOKUPS") != NULL;
      index->trace_lookup_stats_initialized = true;
    }
    if (index->trace_lookup_stats)
      index->enum_queries++;
    if (index->enum_prog != b->prog) {
      if (index->enum_members)
        memset(index->enum_members, 0,
               NY_ENUM_MEMBER_CACHE_SIZE * sizeof(*index->enum_members));
      index->enum_prog = b->prog;
    }
    if (!index->enum_members)
      index->enum_members =
          calloc(NY_ENUM_MEMBER_CACHE_SIZE, sizeof(*index->enum_members));
    if (index->enum_members) {
      name_id = ny_intern_cstr(name);
      if (name_id) {
        size_t slot = ny_fn_name_hash(name) & (NY_ENUM_MEMBER_CACHE_SIZE - 1u);
        cache_entry = &index->enum_members[slot];
        if (cache_entry->resolved && cache_entry->name == name_id) {
          if (index->trace_lookup_stats)
            index->enum_cache_hits++;
          if (cache_entry->found) {
            if (out_enum)
              *out_enum = cache_entry->enum_stmt;
            if (out_item)
              *out_item = cache_entry->item;
            if (out_tag)
              *out_tag = cache_entry->tag;
          }
          return cache_entry->found;
        }
      }
    }
  }
  const char *dot = strrchr(name, '.');
  char enum_name_buf[128] = {0};
  const char *enum_name = NULL;
  const char *member_name = name;
  if (dot) {
    size_t elen = (size_t)(dot - name);
    if (elen < sizeof(enum_name_buf)) {
      memcpy(enum_name_buf, name, elen);
      enum_name_buf[elen] = '\0';
      enum_name = enum_name_buf;
    }
    member_name = dot + 1;
  }
  size_t counter = 0;
  const stmt_t *found_enum = NULL;
  const stmt_enum_item_t *found_item = NULL;
  int64_t found_tag = 0;
  bool found = false;
  bool indexed = index && index->functions.prog == b->prog &&
                 ny_native_enum_lookup_build(index, b->prog);
  if (indexed) {
    if (index->trace_lookup_stats)
      index->enum_index_hits++;
    found = ny_native_enum_lookup_find(index, enum_name, member_name,
                                       &found_enum, &found_item, &found_tag);
  } else {
    if (index && index->trace_lookup_stats)
      index->enum_tree_scans++;
    for (size_t i = 0; i < b->prog->body.len; ++i) {
      if (ny_native_find_enum_item_in_stmt(b->prog->body.data[i], enum_name,
                                           member_name, &counter, &found_enum,
                                           &found_item, &found_tag)) {
        found = true;
        break;
      }
    }
  }
  if (found) {
    if (out_enum)
      *out_enum = found_enum;
    if (out_item)
      *out_item = found_item;
    if (out_tag)
      *out_tag = found_tag;
  }
  if (cache_entry && name_id) {
    *cache_entry = (ny_native_enum_member_entry_t){.name = name_id,
                                                   .enum_stmt = found_enum,
                                                   .item = found_item,
                                                   .tag = found_tag,
                                                   .found = found,
                                                   .resolved = true};
  }
  return found;
}

static inline int nyr_emit_binop(nyr_builder_impl_t *b, nyr_op_t op, int a,
                                 int rhs) {
  int v =
      nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = op, .dst = -1, .a = a, .b = rhs});
  if (v < 0)
    nyr_fail(b, NY_NATIVE_ALLOC_FAIL);
  return v;
}

static bool ny_native_find_impl_in_stmt(const stmt_t *s, const char *type_name);

static bool ny_native_index_impl_names(ny_sym_id *names, const stmt_t *s) {
  if (!s)
    return true;
  if (s->kind == NY_S_IMPL && s->as.impl.type_name) {
    const char *type_name = s->as.impl.type_name;
    ny_sym_id name = ny_intern_cstr(type_name);
    if (!name)
      return false;
    uint32_t hash = ny_resolve_hash(type_name, 0);
    for (size_t probe = 0; probe < NY_IMPL_PROBE_SIZE; ++probe) {
      size_t slot = (hash + probe) & (NY_IMPL_PROBE_SIZE - 1);
      if (!names[slot] || names[slot] == name) {
        names[slot] = name;
        return true;
      }
    }
    return false;
  }
  if (s->kind == NY_S_MODULE) {
    for (size_t i = 0; i < s->as.module.body.len; ++i)
      if (!ny_native_index_impl_names(names, s->as.module.body.data[i]))
        return false;
  }
  return true;
}

/*
 * Index declarations rather than query results: shared-core compilation asks
 * about thousands of ordinary callees that have no impl. Negative answers
 * must not fill the table and turn every subsequent query into an AST walk.
 * The index is reset with the other compilation snapshots.
 */
static bool ny_native_prog_has_impl(const nyr_builder_impl_t *b,
                                    const char *type_name) {
  if (!b || !b->prog || !type_name || !*type_name)
    return false;
  ny_native_fn_index_t *index = ny_native_fn_index_for(b);
  if (index && !index->impl_names_built) {
    index->impl_names_built = true;
    if (!index->impl_names)
      index->impl_names =
          calloc(NY_IMPL_PROBE_SIZE, sizeof(*index->impl_names));
    if (index->impl_names) {
      index->impl_names_complete = true;
      for (size_t i = 0; i < b->prog->body.len; ++i) {
        if (!ny_native_index_impl_names(index->impl_names,
                                        b->prog->body.data[i])) {
          index->impl_names_complete = false;
          break;
        }
      }
    }
  }
  if (index && index->impl_names_complete) {
    ny_sym_id name = ny_intern_cstr(type_name);
    uint32_t hash = ny_resolve_hash(type_name, 0);
    for (size_t probe = 0; probe < NY_IMPL_PROBE_SIZE; ++probe) {
      size_t slot = (hash + probe) & (NY_IMPL_PROBE_SIZE - 1);
      if (!index->impl_names[slot])
        return false;
      if (index->impl_names[slot] == name)
        return true;
    }
    return false;
  }
  for (size_t i = 0; i < b->prog->body.len; ++i)
    if (ny_native_find_impl_in_stmt(b->prog->body.data[i], type_name))
      return true;
  return false;
}

static bool ny_native_find_impl_in_stmt(const stmt_t *s,
                                        const char *type_name) {
  if (!s || !type_name)
    return false;
  if (s->kind == NY_S_IMPL && s->as.impl.type_name &&
      strcmp(s->as.impl.type_name, type_name) == 0)
    return true;
  if (s->kind == NY_S_MODULE) {
    for (size_t i = 0; i < s->as.module.body.len; ++i) {
      if (ny_native_find_impl_in_stmt(s->as.module.body.data[i], type_name))
        return true;
    }
  }
  return false;
}

/*
 * Resolve an attached operator from the source AST.  The typing pass records
 * the operator's return type, but the compact NYIR expression node does not
 * carry a callable target.  Keep that dispatch decision backend-local and
 * use the declared nominal type captured on each local binding.
 */
static bool ny_native_type_alias_eq(const char *a, const char *b) {
  if (!a || !b)
    return false;
  if (strcmp(a, b) == 0)
    return true;
  int a_dim = ny_type_vector_dimension(a);
  return a_dim > 0 && a_dim == ny_type_vector_dimension(b);
}

typedef struct {
  const stmt_t **modules;
  size_t len;
  size_t cap;
  bool allocation_failed;
} ny_native_operator_walk_t;

/*
 * Module ASTs can contain import cycles. Keep one visited set for the whole
 * operator query so recursive module traversal is bounded by the number of
 * distinct module nodes, not by the number of paths through the graph.
 */
static bool ny_native_operator_walk_mark_module(ny_native_operator_walk_t *walk,
                                                const stmt_t *module) {
  if (!walk || !module)
    return false;
  for (size_t i = 0; i < walk->len; ++i)
    if (walk->modules[i] == module)
      return false;
  if (walk->len == walk->cap) {
    size_t cap = walk->cap ? walk->cap * 2 : 16;
    if (cap < walk->cap || cap > SIZE_MAX / sizeof(*walk->modules)) {
      walk->allocation_failed = true;
      return false;
    }
    const stmt_t **grown = realloc(walk->modules, cap * sizeof(*grown));
    if (!grown) {
      walk->allocation_failed = true;
      return false;
    }
    walk->modules = grown;
    walk->cap = cap;
  }
  walk->modules[walk->len++] = module;
  return true;
}

static const stmt_t *
ny_native_find_impl_operator_in_stmt(const stmt_t *s, const char *owner,
                                     const char *op, const char *right_type,
                                     ny_native_operator_walk_t *walk) {
  if (!s || !op)
    return NULL;
  if (s->kind == NY_S_IMPL && s->as.impl.type_name &&
      (!owner || ny_native_type_alias_eq(s->as.impl.type_name, owner))) {
    for (size_t i = 0; i < s->as.impl.methods.len; ++i) {
      const stmt_t *m = s->as.impl.methods.data[i];
      if (!m || m->kind != NY_S_OPERATOR || !m->as.oper.op ||
          strcmp(m->as.oper.op, op) != 0)
        continue;
      if (right_type && m->as.oper.right_type) {
        const char *expected = strcmp(m->as.oper.right_type, "self") == 0
                                   ? s->as.impl.type_name
                                   : m->as.oper.right_type;
        if (!ny_native_type_alias_eq(expected, right_type))
          continue;
      } else if (right_type || m->as.oper.right_type) {
        continue;
      }
      return m;
    }
  }
  if (s->kind == NY_S_MODULE) {
    if (!ny_native_operator_walk_mark_module(walk, s))
      return NULL;
    for (size_t i = 0; i < s->as.module.body.len; ++i) {
      const stmt_t *found = ny_native_find_impl_operator_in_stmt(
          s->as.module.body.data[i], owner, op, right_type, walk);
      if (found)
        return found;
    }
  }
  return NULL;
}

static const stmt_t *nyr_find_operator(const nyr_builder_impl_t *b,
                                       const expr_t *e);

static const char *nyr_expr_type_name_impl(const nyr_builder_impl_t *b,
                                           const expr_t *e);

static const expr_t *nyr_comptime_result_expr(const stmt_t *body) {
  if (!body)
    return NULL;
  if (body->kind == NY_S_RETURN)
    return body->as.ret.value;
  if (body->kind == NY_S_EXPR)
    return body->as.expr.expr;
  if (body->kind != NY_S_BLOCK)
    return NULL;
  for (size_t i = body->as.block.body.len; i-- > 0;) {
    const stmt_t *item = body->as.block.body.data[i];
    if (!item)
      continue;
    if (item->kind == NY_S_RETURN && item->as.ret.value)
      return item->as.ret.value;
    if (item->kind == NY_S_EXPR && item->as.expr.expr)
      return item->as.expr.expr;
  }
  return NULL;
}

static bool nyr_is_comptime_range_expr(const nyr_builder_impl_t *b,
                                       const expr_t *e) {
  if (!e)
    return false;
  if (e->kind == NY_E_COMPTIME)
    return nyr_is_comptime_range_expr(
        b, nyr_comptime_result_expr(e->as.comptime_expr.body));
  if (e->kind == NY_E_IDENT && e->as.ident.name &&
      (!b->current_fn_name || strcmp(b->current_fn_name, "rt_main") == 0 ||
       strcmp(b->current_fn_name, "_ny_top_entry") == 0)) {
    const stmt_t *decl = nyr_find_top_level_var_stmt(b, e->as.ident.name);
    if (decl && decl->kind == NY_S_VAR) {
      for (size_t i = 0; i < decl->as.var.names.len; ++i) {
        const char *name = decl->as.var.names.data[i];
        if (name && strcmp(name, e->as.ident.name) == 0 &&
            i < decl->as.var.exprs.len && decl->as.var.exprs.data[i] &&
            decl->as.var.exprs.data[i]->kind == NY_E_COMPTIME)
          return nyr_is_comptime_range_expr(b, decl->as.var.exprs.data[i]);
      }
    }
  }
  if (e->kind == NY_E_CALL) {
    const char *leaf = ny_native_call_leaf(e);
    if (leaf && (strcmp(leaf, "range") == 0 || strcmp(leaf, "range2") == 0))
      return true;
  }
  return nyr_expr_is_range(b, e);
}

static size_t ny_type_query_hash_slot(const void *p, size_t mask) {
  uint64_t h = (uint64_t)(uintptr_t)p * 0x9E3779B97F4A7C15ULL;
  return (size_t)((h >> 32) & mask);
}

static void ny_type_query_hash_rebuild(nyr_builder_impl_t *b, size_t want_cap) {
  if (want_cap < 64)
    want_cap = 64;
  size_t *hash = calloc(want_cap, sizeof(*hash));
  if (!hash)
    return;
  size_t mask = want_cap - 1;
  for (size_t i = 0; i < b->type_query_cache_count; ++i) {
    size_t slot = ny_type_query_hash_slot(b->type_query_cache[i].expr, mask);
    while (hash[slot] != 0)
      slot = (slot + 1) & mask;
    hash[slot] = i + 1;
  }
  free(b->type_query_hash);
  b->type_query_hash = hash;
  b->type_query_hash_cap = want_cap;
}

static size_t ny_type_context_hash_slot(const expr_t *expr,
                                        const nyr_builder_impl_t *scope,
                                        size_t mask) {
  uint64_t e = (uint64_t)(uintptr_t)expr * UINT64_C(0x9E3779B97F4A7C15);
  uint64_t s = (uint64_t)(uintptr_t)scope * UINT64_C(0xD6E8FEB86659FD93);
  return (size_t)(((e ^ (e >> 29) ^ s ^ (s >> 31)) >> 32) & mask);
}

static const char *ny_type_context_cache_get(const nyr_builder_impl_t *owner,
                                             const expr_t *expr,
                                             const nyr_builder_impl_t *scope) {
  if (!owner || !owner->type_query_context_hash_cap)
    return NULL;
  size_t mask = owner->type_query_context_hash_cap - 1;
  size_t slot = ny_type_context_hash_slot(expr, scope, mask);
  for (;;) {
    size_t entry_index = owner->type_query_context_hash[slot];
    if (!entry_index)
      return NULL;
    const ny_native_type_context_entry_t *entry =
        &owner->type_query_context_cache[entry_index - 1];
    if (entry->generation != owner->type_query_context_generation)
      return NULL;
    if (entry->expr == expr && entry->scope == scope)
      return entry->type_name;
    slot = (slot + 1) & mask;
  }
}

static void ny_type_context_cache_reset(nyr_builder_impl_t *owner) {
  if (!owner)
    return;
  owner->type_query_context_count = 0;
  /*
   * Hash buckets contain indices into the per-generation entry array.  Clear
   * them along with the count so a new generation starts with genuinely empty
   * probe chains; otherwise stale occupied buckets accumulate across queries
   * and either hide current entries or make lookup scan the entire table.
   */
  if (owner->type_query_context_hash)
    memset(owner->type_query_context_hash, 0,
           owner->type_query_context_hash_cap *
               sizeof(*owner->type_query_context_hash));
  if (owner->type_query_context_generation == UINT64_MAX) {
    owner->type_query_context_generation = 1;
  } else {
    owner->type_query_context_generation++;
    if (owner->type_query_context_generation == 0)
      owner->type_query_context_generation = 1;
  }
}

static bool ny_type_context_cache_rebuild(nyr_builder_impl_t *owner,
                                          size_t capacity) {
  if (capacity < 64 || (capacity & (capacity - 1)) != 0 ||
      capacity > SIZE_MAX / sizeof(*owner->type_query_context_hash))
    return false;
  size_t *hash = calloc(capacity, sizeof(*hash));
  if (!hash)
    return false;
  size_t mask = capacity - 1;
  for (size_t i = 0; i < owner->type_query_context_count; ++i) {
    const ny_native_type_context_entry_t *entry =
        &owner->type_query_context_cache[i];
    size_t slot = ny_type_context_hash_slot(entry->expr, entry->scope, mask);
    while (hash[slot])
      slot = (slot + 1) & mask;
    hash[slot] = i + 1;
  }
  free(owner->type_query_context_hash);
  owner->type_query_context_hash = hash;
  owner->type_query_context_hash_cap = capacity;
  return true;
}

static void ny_type_context_cache_put(nyr_builder_impl_t *owner,
                                      const expr_t *expr,
                                      const nyr_builder_impl_t *scope,
                                      const char *type_name) {
  if (!owner || !expr || !scope || !type_name)
    return;
  size_t next_count = owner->type_query_context_count + 1;
  if (next_count > SIZE_MAX / 4)
    return;
  if (owner->type_query_context_count == owner->type_query_context_cap) {
    size_t capacity =
        owner->type_query_context_cap ? owner->type_query_context_cap * 2 : 64;
    if (capacity < owner->type_query_context_cap ||
        capacity > SIZE_MAX / sizeof(*owner->type_query_context_cache))
      return;
    ny_native_type_context_entry_t *entries =
        realloc(owner->type_query_context_cache,
                capacity * sizeof(*owner->type_query_context_cache));
    if (!entries)
      return;
    owner->type_query_context_cache = entries;
    owner->type_query_context_cap = capacity;
  }
  if (!owner->type_query_context_hash_cap ||
      next_count * 4 >= owner->type_query_context_hash_cap * 3) {
    size_t capacity = owner->type_query_context_hash_cap
                          ? owner->type_query_context_hash_cap * 2
                          : 64;
    if (capacity < owner->type_query_context_hash_cap ||
        !ny_type_context_cache_rebuild(owner, capacity))
      return;
  }
  size_t index = owner->type_query_context_count;
  owner->type_query_context_cache[index] = (ny_native_type_context_entry_t){
      .expr = expr,
      .scope = scope,
      .type_name = type_name,
      .generation = owner->type_query_context_generation};
  size_t mask = owner->type_query_context_hash_cap - 1;
  size_t slot = ny_type_context_hash_slot(expr, scope, mask);
  while (owner->type_query_context_hash[slot]) {
    size_t old_index = owner->type_query_context_hash[slot] - 1;
    if (owner->type_query_context_cache[old_index].generation !=
        owner->type_query_context_generation)
      break;
    slot = (slot + 1) & mask;
  }
  owner->type_query_context_hash[slot] = index + 1;
  owner->type_query_context_count++;
}

static const char *nyr_ident_type_in_builder(const nyr_builder_impl_t *b,
                                             const expr_t *e);

static const char *nyr_expr_type_name(const nyr_builder_impl_t *b,
                                      const expr_t *e) {
  if (!b || !e)
    return NULL;
  /*
   * Share the compilation's type-query memo across transient probes so each
   * probe does not calloc its own stack/cache (std.bc retention path).
   * Mark the delegated subtree transient: probe ASTs are freed when the
   * probe ends, so their expression pointers must never be persisted (they
   * would dangle and later alias fresh allocations).  Reads stay sound —
   * a live probe allocation cannot alias the owner's live AST nodes.
   */
  if (b->cache_owner && b->cache_owner != b) {
    nyr_builder_impl_t *owner = b->cache_owner;
    /*
     * Ident resolution depends on this builder's lexical locals; the cache
     * owner has none.  Resolve identifiers that bind here before delegating,
     * or a locally typed binding loses its nominal owner and an attached
     * operator/method dispatch silently misses.
     */
    if (e->kind == NY_E_IDENT && e->as.ident.name) {
      const char *ident_type = nyr_ident_type_in_builder(b, e);
      if (ident_type) {
        if (ny_trace_enabled("NY_TRACE_OPRESOLVE"))
          ny_trace_line("TYPE_RESOLVE", "%s ident=%s local-owner type=%s",
                        b->current_fn_name ? b->current_fn_name : "?",
                        e->as.ident.name, ident_type);
        return ident_type;
      }
    }
    if (owner->transient_query_depth == 0)
      ny_type_context_cache_reset(owner);
    if (owner->transient_query_depth <
        sizeof(owner->delegate_sources) / sizeof(owner->delegate_sources[0]))
      owner->delegate_sources[owner->transient_query_depth] = b;
    owner->transient_query_depth++;
    const char *delegated = nyr_expr_type_name(owner, e);
    owner->transient_query_depth--;
    return delegated;
  }
  nyr_builder_impl_t *mutable_b = (nyr_builder_t *)b;
  if (mutable_b->type_query_depth == 0 &&
      ++mutable_b->cstr_query_generation == 0) {
    memset(mutable_b->cstr_query_cache, 0, sizeof(mutable_b->cstr_query_cache));
    mutable_b->cstr_query_generation = 1;
  }
  const bool contextual_query = mutable_b->transient_query_depth > 0;
  const nyr_builder_impl_t *context_scope =
      contextual_query && mutable_b->transient_query_depth <=
                              sizeof(mutable_b->delegate_sources) /
                                  sizeof(mutable_b->delegate_sources[0])
          ? mutable_b->delegate_sources[mutable_b->transient_query_depth - 1]
          : NULL;
  if (contextual_query) {
    const char *cached = ny_type_context_cache_get(mutable_b, e, context_scope);
    if (cached)
      return cached;
  }
  /*
   * Resolved type names are pure functions of the immutable expression
   * node and this builder's fixed program (lowering never writes
   * expr->semantic), so owner-created entries persist across root queries
   * instead of being cleared per query; the old per-root clear made every
   * operator probe recompute its whole operand subtree, which dominated
   * import-heavy lowers.  The pointer-keyed hash keeps lookup O(1).
   */
  if (mutable_b->type_query_hash_cap == 0 &&
      mutable_b->type_query_cache_count > 0)
    ny_type_query_hash_rebuild(mutable_b, 64);
  if (!contextual_query && mutable_b->type_query_hash_cap) {
    size_t mask = mutable_b->type_query_hash_cap - 1;
    size_t slot = ny_type_query_hash_slot(e, mask);
    for (;;) {
      size_t idx = mutable_b->type_query_hash[slot];
      if (idx == 0)
        break;
      const ny_native_type_query_cache_entry_t *entry =
          &mutable_b->type_query_cache[idx - 1];
      if (entry->expr == e)
        return entry->resolved ? entry->type_name : NULL;
      slot = (slot + 1) & mask;
    }
  }
  for (size_t i = 0; i < mutable_b->type_query_depth; ++i)
    if (mutable_b->type_query_stack[i] == e)
      return NULL;
  if (mutable_b->type_query_depth == mutable_b->type_query_cap) {
    size_t next =
        mutable_b->type_query_cap ? mutable_b->type_query_cap * 2 : 16;
    if (next < mutable_b->type_query_cap ||
        next > SIZE_MAX / sizeof(*mutable_b->type_query_stack))
      return NULL;
    const expr_t **grown =
        realloc(mutable_b->type_query_stack, next * sizeof(*grown));
    if (!grown)
      return NULL;
    mutable_b->type_query_stack = grown;
    mutable_b->type_query_cap = next;
  }
  mutable_b->type_query_stack[mutable_b->type_query_depth++] = e;
  const char *result = nyr_expr_type_name_impl(b, e);
  --mutable_b->type_query_depth;
  if (contextual_query)
    ny_type_context_cache_put(mutable_b, e, context_scope, result);
  else {
    if (mutable_b->type_query_cache_count == mutable_b->type_query_cache_cap) {
      size_t next = mutable_b->type_query_cache_cap
                        ? mutable_b->type_query_cache_cap * 2
                        : 16;
      if (next >= mutable_b->type_query_cache_cap &&
          next <= SIZE_MAX / sizeof(*mutable_b->type_query_cache)) {
        ny_native_type_query_cache_entry_t *grown =
            realloc(mutable_b->type_query_cache, next * sizeof(*grown));
        if (grown) {
          mutable_b->type_query_cache = grown;
          mutable_b->type_query_cache_cap = next;
        }
      }
    }
    if (mutable_b->type_query_cache_count < mutable_b->type_query_cache_cap) {
      ny_native_type_query_cache_entry_t *entry =
          &mutable_b->type_query_cache[mutable_b->type_query_cache_count];
      entry->expr = e;
      entry->type_name = result;
      entry->resolved = true;
      mutable_b->type_query_cache_count++;
      if (mutable_b->type_query_hash_cap == 0)
        ny_type_query_hash_rebuild(mutable_b, 64);
      else if (mutable_b->type_query_cache_count * 4 >=
               mutable_b->type_query_hash_cap * 3)
        ny_type_query_hash_rebuild(mutable_b,
                                   mutable_b->type_query_hash_cap * 2);
      else {
        size_t mask = mutable_b->type_query_hash_cap - 1;
        size_t slot = ny_type_query_hash_slot(e, mask);
        while (mutable_b->type_query_hash[slot] != 0)
          slot = (slot + 1) & mask;
        mutable_b->type_query_hash[slot] = mutable_b->type_query_cache_count;
      }
    }
  }
  return result;
}

/*
 * Resolve an identifier through the calling builder's own lexical local
 * table (plus the program's top-level bindings).  Ident resolution is the
 * one type query that is NOT a pure function of the expression node: it
 * depends on which locals the active function has bound, and the shared
 * cache owner has no locals at all.  Callers that would otherwise delegate
 * to the cache owner must consult this first, or a locally typed binding
 * such as `def SelfBox a` resolves to NULL and loses its nominal owner for
 * attached-operator and attached-method dispatch.
 */
static const char *nyr_ident_type_in_builder(const nyr_builder_impl_t *b,
                                             const expr_t *e) {
  if (!b || !e || e->kind != NY_E_IDENT || !e->as.ident.name)
    return NULL;
  const nyr_local_t *local =
      nyr_find_local((nyr_builder_t *)b, e->as.ident.name);
  if (!local && b->transient_query_depth > 0 &&
      b->transient_query_depth <=
          sizeof(b->delegate_sources) / sizeof(b->delegate_sources[0])) {
    /*
     * This builder is resolving a subtree delegated by another builder
     * (the cache owner has no locals of its own).  The delegating builder's
     * lexical locals are the authority for identifiers inside that subtree.
     */
    const nyr_builder_impl_t *src =
        b->delegate_sources[b->transient_query_depth - 1];
    if (src && src != b)
      local = nyr_find_local((nyr_builder_t *)src, e->as.ident.name);
  }
  if (local) {
    /*
     * An unparameterized list carrier is only a storage ABI.  If its
     * initializer has a concrete generic return (for example
     * `str.to_bytes -> list<int>`), recover that fact before returning the
     * broad carrier type; otherwise an indexed scalar is lowered as a
     * dynamic value in the unified LLVM path.
     */
    bool broad_list =
        local->type_name && ny_native_type_name_is_dyn_list(local->type_name);
    if (local->type_name && !broad_list)
      return local->type_name;
    if (local->type_name)
      return local->type_name;
    if (local->is_list)
      return "list";
    if (local->is_dict)
      return "dict";
    if (local->is_cstr)
      return "str";
    /*
     * A lexical binding shadows every top-level binding even when its own
     * native type is unresolved. Falling through here can give an untyped
     * parameter the type of an unrelated module/global name (`b` as bytes,
     * for example), changing overload selection inside the function body.
     */
    return NULL;
  }
  /*
   * Top-level bindings do not have a native local record while lowering
   * the entry function.  Recover their producer type before overload/ABI
   * selection; otherwise a handle-valued binding is mistaken for an
   * untyped scalar and an overloaded conversion can shift the pointer.
   */
  const expr_t *global =
      e->tok.filename ? nyr_find_top_level_value_in_source(b, e->as.ident.name,
                                                           e->tok.filename)
                      : nyr_find_top_level_value(b, e->as.ident.name);
  if (!global)
    global = nyr_find_top_level_value(b, e->as.ident.name);
  if (global && global != e && global->kind != NY_E_IDENT)
    return nyr_expr_type_name(b, global);
  return NULL;
}

static const char *nyr_expr_type_name_impl(const nyr_builder_impl_t *b,
                                           const expr_t *e) {
  if (!b || !e)
    return NULL;
  if (e->kind == NY_E_COMPTIME) {
    const expr_t *inner = nyr_comptime_result_expr(e->as.comptime_expr.body);
    if (inner && inner != e && nyr_is_comptime_range_expr(b, inner))
      return "range";
  }
  if (e->kind == NY_E_IDENT && e->as.ident.name &&
      (!b->current_fn_name || strcmp(b->current_fn_name, "rt_main") == 0 ||
       strcmp(b->current_fn_name, "_ny_top_entry") == 0)) {
    const stmt_t *decl = nyr_find_top_level_var_stmt(b, e->as.ident.name);
    if (decl && decl->kind == NY_S_VAR) {
      for (size_t i = 0; i < decl->as.var.names.len; ++i) {
        const char *name = decl->as.var.names.data[i];
        if (!name || strcmp(name, e->as.ident.name) != 0 ||
            i >= decl->as.var.exprs.len || !decl->as.var.exprs.data[i] ||
            decl->as.var.exprs.data[i]->kind != NY_E_COMPTIME)
          continue;
        const expr_t *inner = nyr_comptime_result_expr(
            decl->as.var.exprs.data[i]->as.comptime_expr.body);
        if (inner && nyr_is_comptime_range_expr(b, inner))
          return "range";
      }
    }
  }
  /*
   * Whole-program elem-8 int list facts for top-level mutables outrank the
   * conservative annotation the checker recorded while the element type was
   * still undetermined (`mut xs = []`).
   */
  if (e->kind == NY_E_INDEX && e->as.index.target &&
      e->as.index.target->kind == NY_E_IDENT &&
      e->as.index.target->as.ident.name &&
      nyr_top_mut_is_append_int_list(b, e->as.index.target->as.ident.name))
    return "int";
  if (e->kind == NY_E_IDENT && e->as.ident.name &&
      !nyr_find_local((nyr_builder_t *)b, e->as.ident.name) &&
      nyr_top_mut_is_append_int_list(b, e->as.ident.name))
    return "list<int>";
  if (e->tok.kind == NY_T_NIL)
    return "nil";
  if (e->kind == NY_E_LITERAL) {
    switch (e->as.literal.kind) {
    case NY_LIT_INT:
      return "int";
    case NY_LIT_FLOAT:
      return "f64";
    case NY_LIT_STR:
      return "str";
    case NY_LIT_BOOL:
      return "bool";
    }
  }
  /*
   * The type checker has already selected the representation for a resolved
   * binary expression. Consult that fact before the string/container
   * classifiers below: those classifiers may ask for overload resolution,
   * which in turn asks for operand types and recursively re-enters this
   * query. Besides avoiding redundant work, this preserves the frontend's
   * representation choice for ordinary and overloaded operators.
   */
  if (e->kind == NY_E_BINARY && e->semantic.resolved) {
    switch (e->semantic.rep) {
    case NY_SEM_REP_RAW_INT:
      return "int";
    case NY_SEM_REP_F32:
      return "f32";
    case NY_SEM_REP_F64:
      return "f64";
    case NY_SEM_REP_TAGGED_DYNAMIC:
      return "any";
    case NY_SEM_REP_STRING:
      return "str";
    case NY_SEM_REP_TYPED_BUFFER:
      return "list";
    case NY_SEM_REP_POINTER:
      return "ptr";
    default:
      break;
    }
  }
  if (e->kind == NY_E_BINARY && e->semantic.resolved_type_name) {
    if (strcmp(e->semantic.resolved_type_name, "self") == 0)
      return nyr_expr_type_name(b, e->as.binary.left);
    return e->semantic.resolved_type_name;
  }
  if (e->kind == NY_E_LIST) {
    /*
     * Inferred homogeneous literals have the same element-layout proof as an
     * explicit list<T> declaration.  Keeping them as the broad `list` carrier
     * loses that proof before index lowering, forcing a tagged-any read into
     * an ordinary scalar comparison.  Preserve only an exact homogeneous
     * element type; mixed and empty literals remain dynamically typed.
     */
    if (e->as.list_like.len > 0) {
      const char *element_type = nyr_expr_type_name(b, e->as.list_like.data[0]);
      bool homogeneous = element_type && strcmp(element_type, "any") != 0 &&
                         strcmp(element_type, "unknown") != 0;
      for (size_t i = 1; homogeneous && i < e->as.list_like.len; ++i) {
        const char *next = nyr_expr_type_name(b, e->as.list_like.data[i]);
        if (!next || strcmp(next, element_type) != 0)
          homogeneous = false;
      }
      if (homogeneous) {
        size_t element_len = strlen(element_type);
        if (element_len <= SIZE_MAX - 7u) {
          char *text = (char *)malloc(element_len + 7u);
          if (text) {
            memcpy(text, "list<", 5);
            memcpy(text + 5, element_type, element_len);
            text[element_len + 5] = '>';
            text[element_len + 6] = '\0';
            const char *owned = nyr_builder_own_string(b, text);
            free(text);
            if (owned)
              return owned;
          }
        }
      }
    }
    return "list";
  }
  if (e->kind == NY_E_INDEX && e->as.index.target) {
    const char *container = nyr_expr_type_name(b, e->as.index.target);
    if (!container)
      return "any";
    if (strcmp(container, "list") == 0 || strcmp(container, "seq") == 0 ||
        strcmp(container, "tuple") == 0)
      return "unknown";
    if (strcmp(container, "any") == 0)
      return "any";
    if (strncmp(container, "list<", 5) == 0) {
      const char *start = container + 5;
      const char *p = start;
      int depth = 1;
      while (*p && depth > 0) {
        if (*p == '<')
          depth++;
        else if (*p == '>')
          depth--;
        p++;
      }
      if (depth == 0 && p > start) {
        size_t n = (size_t)(p - 1 - start);
        char *text = (char *)malloc(n + 1u);
        if (text) {
          memcpy(text, start, n);
          text[n] = '\0';
          const char *owned = nyr_builder_own_string(b, text);
          free(text);
          if (owned)
            return owned;
        }
      }
    }
    return "any";
  }
  if (e->kind == NY_E_IDENT && e->as.ident.name) {
    const char *ident_type = nyr_ident_type_in_builder(b, e);
    if (ident_type)
      return ident_type;
  }
  /*
   * Resolve attached-method returns before the broad container predicates
   * below.  Those predicates intentionally classify every list-valued call
   * as `list`, but doing so first discards generic returns such as
   * `str.to_bytes -> list<int>` and makes the following index dynamic.
   */
  if (e->kind == NY_E_MEMBER || e->kind == NY_E_MEMCALL) {
    const expr_t *receiver =
        e->kind == NY_E_MEMBER ? e->as.member.target : e->as.memcall.target;
    const char *method =
        e->kind == NY_E_MEMBER ? e->as.member.name : e->as.memcall.name;
    if (receiver && method) {
      const stmt_t *fn = nyr_find_attached_method(b, receiver, method);
      if (!fn)
        fn = nyr_find_attached_owner(b, "any", method);
      if (fn && fn->as.fn.return_type)
        return strcmp(fn->as.fn.return_type, "self") == 0
                   ? nyr_expr_type_name(b, receiver)
                   : fn->as.fn.return_type;
      /*
       * Module-qualified functions are represented as member calls too.
       * Resolve the import alias here, before `nyr_expr_is_list`
       * erases a concrete return such as `bin.hex_to_bytes: list<int>`.
       */
      if (receiver->kind == NY_E_IDENT && receiver->as.ident.name) {
        const char *module = nyr_resolve_use_alias(b, receiver->as.ident.name);
        if (module) {
          const char *qualified =
              nyr_builder_own_qualified((nyr_builder_t *)b, module, method);
          if (qualified) {
            fn = ny_native_fn_cache_lookup_exact_with_builder(b, qualified);
            if (!fn)
              fn = nyr_find_user_function((nyr_builder_t *)b, qualified);
            if (!fn)
              fn = nyr_find_imported_function((nyr_builder_t *)b, qualified);
            if (fn && fn->kind == NY_S_FUNC && fn->as.fn.return_type)
              return fn->as.fn.return_type;
          }
        }
      }
    }
  }
  if (e->kind == NY_E_MEMBER) {
    const char *field_type = nyr_layout_member_type(b, e);
    if (field_type)
      return field_type;
  }
  /*
   * Resolve a call's declared return before the broad `is_list` fallback
   * below.  In particular, a call through a module alias can be a concrete
   * `list<int>` result; collapsing it to `list` makes every inferred local
   * descriptor-backed and reinterprets raw integer slots as tagged values.
   */
  if (e->kind == NY_E_CALL) {
    const bool has_semantic_callee =
        e->semantic.canonical_callee_stmt || e->semantic.canonical_callee;
    const char *surface_name =
        e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT
            ? e->as.call.callee->as.ident.name
            : NULL;
    const size_t argc = e->as.call.args.len;
    const stmt_t *scoped_fn = NULL;
    char scoped_name[512];
    if (!has_semantic_callee && surface_name && !strchr(surface_name, '.') &&
        b->current_fn_name) {
      const char *module_end = strrchr(b->current_fn_name, '.');
      if (module_end && module_end != b->current_fn_name) {
        size_t module_len = (size_t)(module_end - b->current_fn_name);
        int n =
            module_len < sizeof(scoped_name)
                ? snprintf(scoped_name, sizeof(scoped_name), "%.*s.%s",
                           (int)module_len, b->current_fn_name, surface_name)
                : -1;
        if (n > 0 && (size_t)n < sizeof(scoped_name)) {
          scoped_fn =
              ny_native_fn_cache_lookup_exact_with_builder(b, scoped_name);
          if (!scoped_fn)
            scoped_fn = nyr_find_user_function_for_arity(b, scoped_name, argc);
          const char *resolved_module =
              scoped_fn ? ny_native_fn_module(ny_native_fn_index_for(b),
                                              b->prog, scoped_fn)
                        : NULL;
          if (scoped_fn &&
              (!resolved_module || strlen(resolved_module) != module_len ||
               memcmp(resolved_module, scoped_name, module_len) != 0))
            scoped_fn = NULL;
        }
      }
    }
    const stmt_t *fn = e->semantic.canonical_callee_stmt
                           ? e->semantic.canonical_callee_stmt
                           : scoped_fn;
    if (!fn && e->semantic.canonical_callee) {
      fn = nyr_find_user_function_for_arity(b, e->semantic.canonical_callee,
                                            argc);
      const char *canonical_module =
          fn ? ny_native_fn_module(ny_native_fn_index_for(b), b->prog, fn)
             : NULL;
      const char *canonical_dot = strrchr(e->semantic.canonical_callee, '.');
      if (fn && canonical_dot && canonical_module &&
          (strlen(canonical_module) !=
               (size_t)(canonical_dot - e->semantic.canonical_callee) ||
           memcmp(canonical_module, e->semantic.canonical_callee,
                  (size_t)(canonical_dot - e->semantic.canonical_callee)) != 0))
        fn = NULL;
    }
    if (!fn && surface_name && !has_semantic_callee) {
      if (strchr(surface_name, '.'))
        fn = ny_native_fn_cache_lookup_exact_with_builder(b, surface_name);
      if (!fn)
        fn = nyr_find_user_function_for_arity(b, surface_name, argc);
    }
    if (!fn && e->as.call.callee && e->as.call.callee->kind == NY_E_MEMBER &&
        e->as.call.callee->as.member.target &&
        e->as.call.callee->as.member.target->kind == NY_E_IDENT &&
        e->as.call.callee->as.member.target->as.ident.name &&
        e->as.call.callee->as.member.name) {
      const char *module = nyr_resolve_use_alias(
          b, e->as.call.callee->as.member.target->as.ident.name);
      if (module) {
        const char *qualified = nyr_builder_own_qualified(
            (nyr_builder_t *)b, module, e->as.call.callee->as.member.name);
        if (qualified) {
          fn = ny_native_fn_cache_lookup_exact_with_builder(b, qualified);
          if (!fn)
            fn = nyr_find_user_function((nyr_builder_t *)b, qualified);
          if (!fn)
            fn = nyr_find_imported_function((nyr_builder_t *)b, qualified);
        }
      }
    }
    if (fn && fn->kind == NY_S_FUNC && fn->as.fn.return_type) {
      if (strcmp(fn->as.fn.return_type, "self") == 0) {
        /*
         * A self-returning attached method's result type is the nominal type
         * of its receiver. Member-call desugaring often turns `a.add(b)` into
         * a qualified direct call, so recover the receiver through the
         * selected `self` formal as well as from an explicit member callee.
         */
        const expr_t *receiver =
            e->as.call.callee && e->as.call.callee->kind == NY_E_MEMBER
                ? e->as.call.callee->as.member.target
                : NULL;
        if (!receiver) {
          for (size_t i = 0;
               i < fn->as.fn.params.len && i < e->as.call.args.len; ++i) {
            const param_t *param = &fn->as.fn.params.data[i];
            const call_arg_t *arg = &e->as.call.args.data[i];
            if (param->type && strcmp(param->type, "self") == 0 && arg->val) {
              receiver = arg->val;
              break;
            }
          }
        }
        const char *receiver_type =
            receiver ? nyr_expr_type_name(b, receiver) : NULL;
        if (receiver_type && strcmp(receiver_type, "self") != 0)
          return receiver_type;
        return NULL;
      }
      return fn->as.fn.return_type;
    }
  }
  if (nyr_is_comptime_range_expr(b, e))
    return "range";
  if (e->kind == NY_E_LIST || nyr_expr_is_list(b, e))
    return "list";
  if (e->kind == NY_E_DICT || nyr_expr_is_dict(b, e))
    return "dict";
  if (nyr_expr_is_set(b, e))
    return "set";
  if (nyr_expr_is_tuple(b, e))
    return "tuple";
  if (nyr_expr_is_cstr((nyr_builder_t *)b, (expr_t *)e))
    return "str";
  /*
   * `.long` is the core numeric conversion protocol and always produces a
   * BigInt handle.  Keep that type fact available to subsequent attached
   * method resolution (`value.long.bytes`) even when the imported `any.long`
   * implementation is not present in the flattened module body.
   */
  if ((e->kind == NY_E_MEMBER && e->as.member.name &&
       strcmp(e->as.member.name, "long") == 0) ||
      (e->kind == NY_E_MEMCALL && e->as.memcall.name &&
       strcmp(e->as.memcall.name, "long") == 0)) {
    const stmt_t *source_attached = nyr_source_attached_result(b, e);
    return source_attached ? source_attached->as.fn.return_type : "bigint";
  }
  /*
   * Shape guards refine their value to the literal shape contract.  Preserve
   * that information in native lowering so a checked nested sequence keeps
   * its element type across a binding and chained index expressions do not
   * fall back to the unparameterized `list` ABI.
   */
  if (e->kind == NY_E_CALL && ny_native_call_leaf(e) &&
      (strcmp(ny_native_call_leaf(e), "require_shape") == 0 ||
       strcmp(ny_native_call_leaf(e), "assert_shape") == 0) &&
      e->as.call.args.len >= 2 && e->as.call.args.data[1].val &&
      e->as.call.args.data[1].val->kind == NY_E_LITERAL &&
      e->as.call.args.data[1].val->as.literal.kind == NY_LIT_STR &&
      e->as.call.args.data[1].val->as.literal.as.s.data)
    return e->as.call.args.data[1].val->as.literal.as.s.data;
  /*
   * Binary overload resolution is the representation authority.  A
   * contextual semantic cache can retain the scalar type of a nearby
   * expression (`a * b` is f64 while `b / 2.0` returns a vector), so resolve
   * the selected operator before consulting that cache.
   */
  if (e->kind == NY_E_BINARY) {
    const stmt_t *op_stmt = nyr_find_operator(b, e);
    if (op_stmt && op_stmt->as.oper.return_type) {
      if (strcmp(op_stmt->as.oper.return_type, "self") == 0)
        return nyr_expr_type_name(b, e->as.binary.left);
      return op_stmt->as.oper.return_type;
    }
  }
  /*
   * Attached methods are overloads selected by the receiver's nominal type.
   * The semantic cache can retain the broad `any` implementation that was
   * visible before receiver refinement; consulting that cache first loses
   * precise returns such as `str.to_bytes -> list<int>` and makes later index
   * lowering choose the dynamic-list ABI.  Resolve the concrete receiver
   * overload before using the cached callee as a fallback.
   */
  if (e->kind == NY_E_MEMBER || e->kind == NY_E_MEMCALL ||
      (e->kind == NY_E_CALL && e->as.call.callee &&
       e->as.call.callee->kind == NY_E_MEMBER)) {
    const expr_t *receiver = e->kind == NY_E_MEMBER ? e->as.member.target
                             : e->kind == NY_E_MEMCALL
                                 ? e->as.memcall.target
                                 : e->as.call.callee->as.member.target;
    const char *method = e->kind == NY_E_MEMBER ? e->as.member.name
                         : e->kind == NY_E_MEMCALL
                             ? e->as.memcall.name
                             : e->as.call.callee->as.member.name;
    if (receiver && method) {
      const stmt_t *fn = nyr_find_attached_method(b, receiver, method);
      if (fn && fn->as.fn.return_type)
        return strcmp(fn->as.fn.return_type, "self") == 0
                   ? nyr_expr_type_name(b, receiver)
                   : fn->as.fn.return_type;
    }
  }
  if (e->semantic.canonical_callee) {
    const stmt_t *fn = ny_native_fn_cache_lookup_exact_with_builder(
        b, e->semantic.canonical_callee);
    if (!fn)
      fn = nyr_find_user_function((nyr_builder_t *)b,
                                  e->semantic.canonical_callee);
    if (fn && fn->as.fn.return_type)
      return fn->as.fn.return_type;
  }
  if (e->semantic.canonical_callee_stmt &&
      e->semantic.canonical_callee_stmt->as.fn.return_type)
    return e->semantic.canonical_callee_stmt->as.fn.return_type;
  if (e->kind == NY_E_CALL && e->as.call.callee &&
      e->as.call.callee->kind == NY_E_IDENT) {
    const char *name = e->as.call.callee->as.ident.name;
    const stmt_t *fn =
        nyr_find_user_function_for_arity(b, name, e->as.call.args.len);
    if (fn && fn->as.fn.return_type)
      return fn->as.fn.return_type;
    if (b->prog) {
      if (ny_native_prog_has_impl(b, name))
        return name;
      /*
       * Type/layout constructors have no function-cache entry.  Resolve
       * their result from the declaration so a local initialized with
       * `Point(3.0, 4.0)` retains the record type for subsequent `.x`/`.y`
       * lowering.
       */
      const stmt_t *layout = nyr_find_layout_in_prog(b, b->prog, name);
      if (layout &&
          (layout->kind == NY_S_STRUCT || layout->kind == NY_S_LAYOUT))
        return layout->kind == NY_S_STRUCT ? layout->as.struc.name
                                           : layout->as.layout.name;
    }
  }
  const expr_t *receiver = NULL;
  const char *method = NULL;
  if (e->kind == NY_E_CALL && e->as.call.callee &&
      e->as.call.callee->kind == NY_E_MEMBER) {
    receiver = e->as.call.callee->as.member.target;
    method = e->as.call.callee->as.member.name;
  } else if (e->kind == NY_E_MEMCALL) {
    receiver = e->as.memcall.target;
    method = e->as.memcall.name;
  }
  if (receiver && method) {
    if (receiver->kind == NY_E_IDENT && receiver->as.ident.name) {
      const char *mod = nyr_resolve_use_alias(b, receiver->as.ident.name);
      if (mod) {
        char qual[512];
        snprintf(qual, sizeof(qual), "%s.%s", mod, method);
        const stmt_t *fn =
            ny_native_fn_cache_lookup_exact_with_builder(b, qual);
        if (!fn)
          fn = nyr_find_user_function((nyr_builder_t *)b, qual);
        if (fn && fn->as.fn.return_type)
          return fn->as.fn.return_type;
      }
    }
    const stmt_t *fn = nyr_find_attached_method(b, receiver, method);
    if (fn && fn->as.fn.return_type)
      return strcmp(fn->as.fn.return_type, "self") == 0
                 ? nyr_expr_type_name(b, receiver)
                 : fn->as.fn.return_type;
  }
  if (e->kind == NY_E_BINARY) {
    const stmt_t *op_stmt = nyr_find_operator(b, e);
    if (op_stmt && op_stmt->as.oper.return_type) {
      if (strcmp(op_stmt->as.oper.return_type, "self") == 0)
        return nyr_expr_type_name(b, e->as.binary.left);
      return op_stmt->as.oper.return_type;
    }
  }
  return NULL;
}

static uint64_t ny_native_operator_cache_hash(const program_t *prog,
                                              const char *owner, const char *op,
                                              const char *right) {
  uint64_t h = UINT64_C(1469598103934665603);
  uintptr_t p = (uintptr_t)prog;
  for (size_t i = 0; i < sizeof(p); ++i)
    h = (h ^ (uint8_t)(p >> (i * 8))) * UINT64_C(1099511628211);
  const char *parts[3] = {owner, op, right};
  for (size_t k = 0; k < 3; ++k) {
    if (!parts[k]) {
      h = (h ^ 0xFF) * UINT64_C(1099511628211);
      continue;
    }
    for (const unsigned char *s = (const unsigned char *)parts[k]; *s; ++s)
      h = (h ^ *s) * UINT64_C(1099511628211);
    h = (h ^ 0x1F) * UINT64_C(1099511628211);
  }
  return h ? h : 1;
}

static bool ny_native_operator_cache_grow(nyr_builder_impl_t *owner,
                                          size_t capacity) {
  if (!owner || capacity < 16 || (capacity & (capacity - 1)) != 0 ||
      capacity > SIZE_MAX / sizeof(*owner->operator_cache))
    return false;
  ny_operator_cache_entry_t *entries = calloc(capacity, sizeof(*entries));
  if (!entries)
    return false;
  for (size_t i = 0; i < owner->operator_cache_cap; ++i) {
    ny_operator_cache_entry_t entry = owner->operator_cache[i];
    if (!entry.resolved)
      continue;
    size_t at = (size_t)entry.hash & (capacity - 1);
    while (entries[at].resolved)
      at = (at + 1) & (capacity - 1);
    entries[at] = entry;
  }
  free(owner->operator_cache);
  owner->operator_cache = entries;
  owner->operator_cache_cap = capacity;
  return true;
}

static bool
ny_native_impl_has_operator_in_stmt(const stmt_t *s, const char *owner,
                                    const char *op,
                                    ny_native_operator_walk_t *walk) {
  if (!s || !owner || !op)
    return false;
  if (s->kind == NY_S_IMPL && s->as.impl.type_name &&
      ny_native_type_alias_eq(s->as.impl.type_name, owner)) {
    for (size_t i = 0; i < s->as.impl.methods.len; ++i) {
      const stmt_t *method = s->as.impl.methods.data[i];
      if (method && method->kind == NY_S_OPERATOR && method->as.oper.op &&
          strcmp(method->as.oper.op, op) == 0)
        return true;
    }
  }
  if (s->kind == NY_S_MODULE) {
    if (!ny_native_operator_walk_mark_module(walk, s))
      return false;
    for (size_t i = 0; i < s->as.module.body.len; ++i)
      if (ny_native_impl_has_operator_in_stmt(s->as.module.body.data[i], owner,
                                              op, walk))
        return true;
  }
  return false;
}

static bool ny_native_operator_pair_has_overload(nyr_builder_impl_t *cache,
                                                 const program_t *prog,
                                                 const char *owner,
                                                 const char *op,
                                                 bool *has_overload) {
  if (!cache || !prog || !owner || !op || !has_overload)
    return false;
  for (size_t i = 0; i < cache->operator_pair_cache_count; ++i) {
    const ny_operator_pair_cache_entry_t *entry =
        &cache->operator_pair_cache[i];
    if (entry->prog == prog && entry->owner == owner && entry->op == op) {
      *has_overload = entry->has_overload;
      return true;
    }
  }

  ny_native_operator_walk_t walk = {0};
  bool found = false;
  for (size_t i = 0; i < prog->body.len && !found; ++i)
    found = ny_native_impl_has_operator_in_stmt(prog->body.data[i], owner, op,
                                                &walk);
  free(walk.modules);
  if (walk.allocation_failed)
    return false;

  if (cache->operator_pair_cache_count == cache->operator_pair_cache_cap) {
    size_t next = cache->operator_pair_cache_cap
                      ? cache->operator_pair_cache_cap * 2
                      : 16;
    if (next < cache->operator_pair_cache_cap ||
        next > SIZE_MAX / sizeof(*cache->operator_pair_cache))
      return false;
    ny_operator_pair_cache_entry_t *grown =
        realloc(cache->operator_pair_cache, next * sizeof(*grown));
    if (!grown)
      return false;
    cache->operator_pair_cache = grown;
    cache->operator_pair_cache_cap = next;
  }
  cache->operator_pair_cache[cache->operator_pair_cache_count++] =
      (ny_operator_pair_cache_entry_t){
          .prog = prog, .owner = owner, .op = op, .has_overload = found};
  *has_overload = found;
  return true;
}

static const stmt_t *nyr_find_operator(const nyr_builder_impl_t *b,
                                       const expr_t *e) {
  if (!b || !b->prog || !e || e->kind != NY_E_BINARY || !e->as.binary.op)
    return NULL;
  if (e->semantic.canonical_callee_stmt &&
      e->semantic.canonical_callee_stmt->kind == NY_S_OPERATOR)
    return e->semantic.canonical_callee_stmt;
  /*
   * The frontend's resolved semantic fact is definitive for ordinary
   * operators. An overloaded operator carries its selected declaration in
   * canonical_callee_stmt above; probing the overload set again here only
   * re-enters operand type discovery. On long arithmetic expressions that
   * creates a recursive, quadratic walk through the same AST.
   */
  if (e->semantic.resolved)
    return NULL;
  /*
   * Type queries for an unresolved binary operator ask for operand types,
   * which can recursively probe the same operator families on both sides.
   * Large flattened stdlib expressions can otherwise produce an unbounded
   * recursive walk here. The frontend-selected overload above remains
   * authoritative; beyond this depth, leave unresolved expressions to the
   * conservative builtin representation path.
   */
  const nyr_builder_impl_t *query_owner = b->cache_owner ? b->cache_owner : b;
  if (query_owner->type_query_depth >= 4)
    return NULL;
  const char *owner = nyr_expr_type_name(b, e->as.binary.left);
  if (!owner)
    return NULL;
  /*
   * Skip right-operand type inference for built-in operators with no matching
   * implementation. In the flattened stdlib this avoids recursively
   * re-resolving a large right expression at every built-in operator site.
   */
  nyr_builder_impl_t *cache =
      b->cache_owner ? b->cache_owner : (nyr_builder_t *)b;
  cache->cache_owner = cache;
  owner = nyr_builder_own_string(cache, owner);
  const char *op = nyr_builder_own_string(cache, e->as.binary.op);
  if (!owner || !op)
    return NULL;
  bool has_overload = false;
  if (!ny_native_operator_pair_has_overload(cache, b->prog, owner, op,
                                            &has_overload)) {
    nyr_fail((nyr_builder_t *)b,
             "native NYIR: operator lookup exhausted memory while indexing");
    return NULL;
  }
  if (!has_overload)
    return NULL;
  if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
    ny_dbg_heuristic_event("native-opresolve", "l=%s", owner);
  const char *right_type = nyr_expr_type_name(b, e->as.binary.right);
  if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
    ny_dbg_heuristic_event("native-opresolve", "r=%s",
                           right_type ? right_type : "?");
  const char *right =
      right_type ? nyr_builder_own_string(cache, right_type) : NULL;
  if (right_type && !right) {
    nyr_fail((nyr_builder_t *)b,
             "native NYIR: could not retain operator operand type");
    return NULL;
  }
  uint64_t hash = ny_native_operator_cache_hash(b->prog, owner, op, right);
  if (!cache->operator_cache_cap)
    (void)ny_native_operator_cache_grow(cache, 64);
  if (cache->operator_cache_cap) {
    size_t at = (size_t)hash & (cache->operator_cache_cap - 1);
    for (size_t n = 0; n < cache->operator_cache_cap; ++n) {
      ny_operator_cache_entry_t *entry = &cache->operator_cache[at];
      if (!entry->resolved)
        break;
      if (entry->hash == hash && entry->prog == b->prog &&
          entry->owner == owner && entry->op == op && entry->right == right)
        return entry->result;
      at = (at + 1) & (cache->operator_cache_cap - 1);
    }
  }
  const stmt_t *found = NULL;
  ny_native_operator_walk_t walk = {0};
  if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
    ny_dbg_heuristic_event("native-opresolve", "mw=%zu", b->prog->body.len);
  for (size_t i = 0; i < b->prog->body.len && !found; ++i)
    found = ny_native_find_impl_operator_in_stmt(b->prog->body.data[i], owner,
                                                 op, right, &walk);
  free(walk.modules);

  if (walk.allocation_failed) {
    nyr_fail((nyr_builder_t *)b, "native NYIR: operator lookup exhausted "
                                 "memory while walking modules");
    return NULL;
  }
  if (ny_trace_enabled("NY_TRACE_OPRESOLVE"))
    ny_trace_line(
        "OP_RESOLVE", "%s owner=%s op=%s right=%s -> %s%s",
        b->current_fn_name ? b->current_fn_name : "?", owner ? owner : "(null)",
        op ? op : "(null)", right ? right : "(null)",
        found && found->as.oper.target ? found->as.oper.target : "builtin",
        found && found->as.oper.target ? "" : " (no overload)");
  if (cache->operator_cache_cap &&
      cache->operator_cache_count >= cache->operator_cache_cap / 2 &&
      cache->operator_cache_cap <= SIZE_MAX / 2)
    (void)ny_native_operator_cache_grow(cache, cache->operator_cache_cap * 2);
  if (cache->operator_cache_cap) {
    size_t at = (size_t)hash & (cache->operator_cache_cap - 1);
    size_t n = 0;
    while (n < cache->operator_cache_cap &&
           cache->operator_cache[at].resolved) {
      at = (at + 1) & (cache->operator_cache_cap - 1);
      ++n;
    }
    if (n < cache->operator_cache_cap) {
      cache->operator_cache[at] = (ny_operator_cache_entry_t){.prog = b->prog,
                                                              .owner = owner,
                                                              .op = op,
                                                              .right = right,
                                                              .result = found,
                                                              .hash = hash,
                                                              .resolved = true};
      cache->operator_cache_count++;
    }
  }
  return found;
}

static const stmt_t *
ny_native_find_attached_method_in_stmt(const stmt_t *s, const char *owner,
                                       const char *method) {
  if (!s || !owner || !method)
    return NULL;
  if (s->kind == NY_S_IMPL && s->as.impl.type_name &&
      strcmp(s->as.impl.type_name, owner) == 0) {
    for (size_t i = 0; i < s->as.impl.methods.len; ++i) {
      const stmt_t *m = s->as.impl.methods.data[i];
      const char *mleaf =
          m && m->as.fn.name ? ny_name_leaf(m->as.fn.name) : NULL;
      if (m && m->kind == NY_S_FUNC && m->as.fn.name &&
          (strcmp(m->as.fn.name, method) == 0 ||
           (mleaf && strcmp(mleaf, method) == 0)))
        return m;
    }
  }
  if (s->kind == NY_S_MODULE) {
    for (size_t i = 0; i < s->as.module.body.len; ++i) {
      const stmt_t *found = ny_native_find_attached_method_in_stmt(
          s->as.module.body.data[i], owner, method);
      if (found)
        return found;
    }
  }
  if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; ++i) {
      const stmt_t *found = ny_native_find_attached_method_in_stmt(
          s->as.block.body.data[i], owner, method);
      if (found)
        return found;
    }
  }
  return NULL;
}

static const stmt_t *nyr_find_attached_owner_impl(const nyr_builder_impl_t *b,
                                                  const char *owner,
                                                  const char *method);

static const stmt_t *nyr_find_attached_owner(const nyr_builder_impl_t *b,
                                             const char *owner,
                                             const char *method) {
  ny_native_fn_index_t *index = ny_native_fn_index_for(b);
  if (!index || index->functions.prog != b->prog)
    return nyr_find_attached_owner_impl(b, owner, method);
  if (!index->attached_owners)
    index->attached_owners =
        calloc(NY_ATTACHED_OWNER_CACHE_SIZE, sizeof(*index->attached_owners));
  ny_sym_id owner_id = ny_intern_cstr(owner);
  ny_sym_id method_id = ny_intern_cstr(method);
  uint32_t hash = ny_resolve_hash(owner, method_id);
  size_t empty = NY_ATTACHED_OWNER_CACHE_SIZE;
  for (size_t probe = 0; index->attached_owners && probe < 8; ++probe) {
    size_t slot = (hash + probe) & (NY_ATTACHED_OWNER_CACHE_SIZE - 1);
    ny_native_attached_owner_entry_t *entry = &index->attached_owners[slot];
    if (!entry->resolved) {
      empty = slot;
      break;
    }
    if (entry->owner == owner_id && entry->method == method_id)
      return entry->fn;
  }
  const stmt_t *fn = nyr_find_attached_owner_impl(b, owner, method);
  if (empty < NY_ATTACHED_OWNER_CACHE_SIZE)
    index->attached_owners[empty] = (ny_native_attached_owner_entry_t){
        .owner = owner_id, .method = method_id, .fn = fn, .resolved = true};
  return fn;
}

static const stmt_t *nyr_find_attached_method_impl(const nyr_builder_impl_t *b,
                                                   const expr_t *receiver,
                                                   const char *method,
                                                   const char *receiver_type);

static uint64_t ny_native_attached_cache_hash(const program_t *prog,
                                              const expr_t *receiver,
                                              const char *receiver_type,
                                              const char *method) {
  uint64_t h = UINT64_C(1469598103934665603);
  uintptr_t p = (uintptr_t)prog;
  uintptr_t r = (uintptr_t)receiver;
  for (size_t i = 0; i < sizeof(p); ++i) {
    h = (h ^ (uint8_t)(p >> (i * 8))) * UINT64_C(1099511628211);
    h = (h ^ (uint8_t)(r >> (i * 8))) * UINT64_C(1099511628211);
  }
  for (const unsigned char *s = (const unsigned char *)receiver_type; s && *s;
       ++s)
    h = (h ^ *s) * UINT64_C(1099511628211);
  h = (h ^ 0xffu) * UINT64_C(1099511628211);
  for (const unsigned char *s = (const unsigned char *)method; *s; ++s)
    h = (h ^ *s) * UINT64_C(1099511628211);
  return h ? h : 1;
}

static bool ny_native_attached_cache_grow(nyr_builder_impl_t *owner,
                                          size_t capacity) {
  if (!owner || capacity < 16 || (capacity & (capacity - 1)) != 0 ||
      capacity > SIZE_MAX / sizeof(*owner->attached_cache))
    return false;
  ny_attached_cache_entry_t *entries = calloc(capacity, sizeof(*entries));
  if (!entries)
    return false;
  for (size_t i = 0; i < owner->attached_cache_cap; ++i) {
    ny_attached_cache_entry_t entry = owner->attached_cache[i];
    if (!entry.resolved)
      continue;
    size_t at = (size_t)entry.hash & (capacity - 1);
    while (entries[at].resolved)
      at = (at + 1) & (capacity - 1);
    entries[at] = entry;
  }
  free(owner->attached_cache);
  owner->attached_cache = entries;
  owner->attached_cache_cap = capacity;
  return true;
}

static const stmt_t *nyr_find_attached_method(const nyr_builder_impl_t *b,
                                              const expr_t *receiver,
                                              const char *method) {
  if (!b || !b->prog || !receiver || !method)
    return NULL;
  nyr_builder_impl_t *owner =
      b->cache_owner ? b->cache_owner : (nyr_builder_t *)b;
  owner->cache_owner = owner;
  const char *receiver_type = nyr_expr_type_name(b, receiver);
  if (receiver_type)
    receiver_type = nyr_builder_own_string(owner, receiver_type);
  uint64_t hash =
      ny_native_attached_cache_hash(b->prog, receiver, receiver_type, method);
  if (!owner->attached_cache_cap)
    (void)ny_native_attached_cache_grow(owner, 64);
  if (owner->attached_cache_cap) {
    size_t at = (size_t)hash & (owner->attached_cache_cap - 1);
    for (size_t n = 0; n < owner->attached_cache_cap; ++n) {
      ny_attached_cache_entry_t *entry = &owner->attached_cache[at];
      if (!entry->resolved)
        break;
      bool same_type = (!entry->receiver_type && !receiver_type) ||
                       (entry->receiver_type && receiver_type &&
                        strcmp(entry->receiver_type, receiver_type) == 0);
      if (entry->hash == hash && entry->prog == b->prog &&
          entry->receiver == receiver && same_type &&
          strcmp(entry->method, method) == 0)
        return entry->result;
      at = (at + 1) & (owner->attached_cache_cap - 1);
    }
  }
  const stmt_t *res =
      nyr_find_attached_method_impl(b, receiver, method, receiver_type);
  /*
   * Receiver type inference is contextual and may be incomplete on an
   * earlier query of the same AST node.  Cache found declarations, but let
   * unresolved lookups retry after the receiver's concrete type appears.
   */
  if (!res)
    return NULL;
  if (owner->attached_cache_cap &&
      owner->attached_cache_count >= owner->attached_cache_cap / 2 &&
      owner->attached_cache_cap <= SIZE_MAX / 2)
    (void)ny_native_attached_cache_grow(owner, owner->attached_cache_cap * 2);
  if (owner->attached_cache_cap) {
    size_t at = (size_t)hash & (owner->attached_cache_cap - 1);
    size_t n = 0;
    while (n < owner->attached_cache_cap &&
           owner->attached_cache[at].resolved) {
      at = (at + 1) & (owner->attached_cache_cap - 1);
      ++n;
    }
    if (n < owner->attached_cache_cap) {
      owner->attached_cache[at] =
          (ny_attached_cache_entry_t){.prog = b->prog,
                                      .receiver = receiver,
                                      .receiver_type = receiver_type,
                                      .method = method,
                                      .result = res,
                                      .hash = hash,
                                      .resolved = true};
      owner->attached_cache_count++;
    }
  }
  return res;
}

static const stmt_t *nyr_find_attached_method_impl(const nyr_builder_impl_t *b,
                                                   const expr_t *receiver,
                                                   const char *method,
                                                   const char *receiver_type) {
  const char *owner = receiver_type;
  while (owner && *owner == '*')
    ++owner;
  /*
   * Attached declarations are registered under the nominal base owner.
   * A literal such as [1,2,3] is inferred as list<int>, while the method
   * itself is declared on list.
   */
  char owner_base[512];
  const char *generic = owner ? strchr(owner, '<') : NULL;
  if (generic && generic > owner &&
      (size_t)(generic - owner) < sizeof(owner_base)) {
    size_t n = (size_t)(generic - owner);
    memcpy(owner_base, owner, n);
    owner_base[n] = '\0';
    owner = owner_base;
  }
  /*
   * Imported calls can lose their return annotation when the merged module
   * cache is consulted through the unqualified expression node.  Recover the
   * declaration from the call itself before treating the receiver as `any`;
   * otherwise a same-named global function (for example bytes(int)) wins over
   * the nominal attached method (bigint.bytes).
   */
  if ((!owner || strcmp(owner, "any") == 0) && receiver->kind == NY_E_CALL) {
    const stmt_t *producer = receiver->semantic.canonical_callee_stmt;
    if (!producer && receiver->semantic.canonical_callee)
      producer = nyr_find_user_function((nyr_builder_t *)b,
                                        receiver->semantic.canonical_callee);
    if (!producer && receiver->as.call.callee &&
        receiver->as.call.callee->kind == NY_E_IDENT &&
        receiver->as.call.callee->as.ident.name)
      producer = nyr_find_user_function(
          (nyr_builder_t *)b, receiver->as.call.callee->as.ident.name);
    if (producer && producer->as.fn.return_type)
      owner = producer->as.fn.return_type;
  }
  if (!owner || !b || !b->prog)
    return NULL;
  return nyr_find_attached_owner(b, owner, method);
}

static const stmt_t *nyr_find_attached_owner_impl(const nyr_builder_impl_t *b,
                                                  const char *owner,
                                                  const char *method) {
  /*
   * Merged stdlib impls carry qualified owner names while source expressions
   * inside the module retain the short type name. Resolve that short name in
   * the current module before searching the program-wide declarations, so a
   * same-named method on an unrelated type cannot capture the property.
   */
  if (owner && !strchr(owner, '.') && b->module_name && *b->module_name) {
    char qualified[512];
    int n =
        snprintf(qualified, sizeof(qualified), "%s.%s", b->module_name, owner);
    if (n > 0 && (size_t)n < sizeof(qualified)) {
      for (size_t i = 0; i < b->prog->body.len; ++i) {
        const stmt_t *found = ny_native_find_attached_method_in_stmt(
            b->prog->body.data[i], qualified, method);
        if (found)
          return found;
      }
    }
  }
  for (size_t i = 0; i < b->prog->body.len; ++i) {
    const stmt_t *found = ny_native_find_attached_method_in_stmt(
        b->prog->body.data[i], owner, method);
    if (found)
      return found;
  }
  /*
   * Impl declarations can be lowered from a merged/imported function cache
   * without remaining as direct children of the current program node.  The
   * ordinary function index still knows the fully-qualified owner.method
   * symbol, so use it as a backend-local fallback for chained members.
   */
  char qualified[512];
  int n = snprintf(qualified, sizeof(qualified), "%s.%s", owner, method);
  if (n > 0 && (size_t)n < sizeof(qualified)) {
    const stmt_t *exact =
        ny_native_fn_cache_lookup_exact_with_builder(b, qualified);
    if (exact)
      return exact;
  }
  /*
   * Imported modules qualify attached declarations in the function cache
   * (`core.math.bin.list.text`) even though the nominal owner is still just
   * `list`.  Resolve that representation-independent suffix when the short
   * owner.method spelling is absent.  Require uniqueness so unrelated
   * modules cannot win by cache order.
   */
  if (n > 0 && (size_t)n < sizeof(qualified)) {
    char suffix[512];
    int sn = snprintf(suffix, sizeof(suffix), ".%s", qualified);
    if (sn > 0 && (size_t)sn < sizeof(suffix)) {
      const stmt_t *found = NULL;
      ny_native_fn_index_t *index = ny_native_fn_index_for(b);
      /*
       * A debug-only re-lowering builder can have a program but no completed
       * function-index snapshot.  The zeroed bucket heads are not a valid
       * empty index until the `next` chains exist; walking head 0 through a
       * null chain used to segfault while formatting the diagnostic dump.
       */
      if (index && index->fn_leaf_next) {
        uint32_t slot = ny_fn_name_hash(method) & (NY_FN_BUCKET_SIZE - 1);
        const int32_t *heads[] = {index->fn_leaf_user_buckets,
                                  index->fn_leaf_stdlib_buckets};
        for (size_t h = 0; h < 2; ++h) {
          for (int32_t i = heads[h][slot]; i >= 0; i = index->fn_leaf_next[i]) {
            const stmt_t *candidate = index->functions.funcs[i];
            const char *name = candidate && candidate->as.fn.name
                                   ? candidate->as.fn.name
                                   : NULL;
            size_t len = name ? strlen(name) : 0;
            if (len < (size_t)sn ||
                strcmp(name + len - (size_t)sn, suffix) != 0)
              continue;
            if (found)
              return NULL;
            found = candidate;
          }
        }
      }
      return found;
    }
  }
  return NULL;
}

static __attribute__((unused)) bool
nyr_find_impl_type(const nyr_builder_impl_t *b, const char *type_name) {
  if (!b || !b->prog || !type_name)
    return false;
  for (size_t i = 0; i < b->prog->body.len; ++i) {
    if (ny_native_find_impl_in_stmt(b->prog->body.data[i], type_name))
      return true;
  }
  return false;
}

/*
 * Resolve source types to their runtime carrier before stamping native locals
 * and call slots. A refinement has its base type's representation; an impl
 * owner without a declared struct/layout is a runtime object handle.
 */
static const stmt_t *nyr_enum_type_in_stmt(const stmt_t *s,
                                           const char *type_name) {
  if (!s)
    return NULL;
  if (s->kind == NY_S_ENUM)
    return s->as.enu.name && strcmp(s->as.enu.name, type_name) == 0 ? s : NULL;
  if (s->kind == NY_S_MODULE)
    for (size_t i = 0; i < s->as.module.body.len; ++i) {
      const stmt_t *decl =
          nyr_enum_type_in_stmt(s->as.module.body.data[i], type_name);
      if (decl)
        return decl;
    }
  return NULL;
}

static const stmt_t *nyr_cached_enum_type_in_prog(const nyr_builder_impl_t *b,
                                                  const char *qualified_name) {
  if (!b || !b->prog || !qualified_name || !*qualified_name)
    return NULL;
  ny_native_fn_index_t *index = ny_native_fn_index_for(b);
  ny_sym_id type = index && index->functions.prog == b->prog
                       ? ny_intern_cstr(qualified_name)
                       : 0;
  size_t empty = NY_MODULE_ENUM_TYPE_CACHE_SIZE;
  ny_native_module_enum_type_entry_t *entry = NULL;
  if (type) {
    if (!index->module_enum_types)
      index->module_enum_types = calloc(NY_MODULE_ENUM_TYPE_CACHE_SIZE,
                                        sizeof(*index->module_enum_types));
    for (size_t probe = 0;
         index->module_enum_types && probe < NY_MODULE_ENUM_TYPE_CACHE_SIZE;
         ++probe) {
      size_t slot = (ny_resolve_hash(qualified_name, 0) + probe) &
                    (NY_MODULE_ENUM_TYPE_CACHE_SIZE - 1u);
      entry = &index->module_enum_types[slot];
      if (!entry->resolved) {
        empty = slot;
        entry = NULL;
        break;
      }
      if (entry->type == type)
        return entry->decl;
      entry = NULL;
    }
  }

  const stmt_t *decl = NULL;
  for (size_t i = 0; i < b->prog->body.len && !decl; ++i)
    decl = nyr_enum_type_in_stmt(b->prog->body.data[i], qualified_name);
  if (index && type && index->module_enum_types &&
      empty < NY_MODULE_ENUM_TYPE_CACHE_SIZE)
    index->module_enum_types[empty] = (ny_native_module_enum_type_entry_t){
        .type = type, .decl = decl, .resolved = true};
  return decl;
}

static ny_sem_rep_t nyr_rep_for_type_impl(const nyr_builder_impl_t *b,
                                          const char *type_name);

static ny_sem_rep_t nyr_rep_for_type(const nyr_builder_impl_t *b,
                                     const char *type_name) {
  ny_sem_rep_t rep = ny_semantic_rep_for_type(type_name);
  if (!b || !type_name || !*type_name || rep != NY_SEM_REP_AGGREGATE ||
      type_name[0] == '?' || type_name[0] == '*')
    return rep;
  if (b->module_name && *b->module_name && !strchr(type_name, '.') && b->prog) {
    char owned[1280];
    int n = snprintf(owned, sizeof(owned), "%s.%s", b->module_name, type_name);
    if (n > 0 && (size_t)n < sizeof(owned) &&
        nyr_cached_enum_type_in_prog(b, owned))
      return nyr_rep_for_type(b, owned);
  }
  ny_native_fn_index_t *index = ny_native_fn_index_for(b);
  if (!index || index->functions.prog != b->prog)
    return nyr_rep_for_type_impl(b, type_name);
  if (!index->type_reps)
    index->type_reps =
        calloc(NY_TYPE_REP_CACHE_SIZE, sizeof(*index->type_reps));
  ny_sym_id type = ny_intern_cstr(type_name);
  size_t empty = NY_TYPE_REP_CACHE_SIZE;
  for (size_t probe = 0; index->type_reps && probe < 8; ++probe) {
    size_t slot =
        (ny_resolve_hash(type_name, 0) + probe) & (NY_TYPE_REP_CACHE_SIZE - 1);
    ny_native_type_rep_entry_t *entry = &index->type_reps[slot];
    if (!entry->resolved) {
      empty = slot;
      break;
    }
    if (entry->type == type)
      return entry->rep;
  }
  rep = nyr_rep_for_type_impl(b, type_name);
  if (empty < NY_TYPE_REP_CACHE_SIZE)
    index->type_reps[empty] = (ny_native_type_rep_entry_t){
        .type = type, .rep = rep, .resolved = true};
  return rep;
}

static ny_sem_rep_t nyr_rep_for_type_impl(const nyr_builder_impl_t *b,
                                          const char *type_name) {
  ny_sem_rep_t rep = ny_semantic_rep_for_type(type_name);
  if (!b || !type_name || !*type_name || rep != NY_SEM_REP_AGGREGATE)
    return rep;
  if (type_name[0] == '?' || type_name[0] == '*')
    return rep;
  const stmt_t *refine = nyr_find_refine_in_prog(b, b->prog, type_name);
  if (refine && refine->as.refine.base_type &&
      strcmp(refine->as.refine.base_type, type_name) != 0)
    return nyr_rep_for_type(b, refine->as.refine.base_type);
  /*
   * Adding methods to an enum does not change its constructor/matcher ABI.
   * In particular mixed enums carry both payload handles and empty tags.
   */
  if (b->prog)
    for (size_t i = 0; i < b->prog->body.len; ++i) {
      const stmt_t *decl =
          nyr_enum_type_in_stmt(b->prog->body.data[i], type_name);
      if (decl) {
        for (size_t j = 0; j < decl->as.enu.items.len; ++j)
          if (decl->as.enu.items.data[j].fields.len)
            return NY_SEM_REP_TAGGED_DYNAMIC;
        return NY_SEM_REP_RAW_INT;
      }
    }
  if (nyr_find_impl_type(b, type_name) &&
      !nyr_find_layout_in_prog(b, b->prog, type_name))
    return NY_SEM_REP_OBJECT;
  /*
   * Ny `struct` values are heap handles even though their declarations carry
   * an aggregate field layout. Keep that source layout fact separate from
   * the machine carrier used by parameters and local slots.
   */
  if (nyr_find_layout_in_prog(b, b->prog, type_name))
    return NY_SEM_REP_OBJECT;
  return rep;
}

static inline int nyr_emit_const_cstr(nyr_builder_impl_t *b, const char *s) {
  if (!s)
    s = "";
  size_t slen = strlen(s);
  const char *sym = nyr_strtab_intern(s, slen, NULL, 0);
  if (!sym) {
    return nyr_fail(b, "native NYIR lower: string table full or OOM");
  }
  int addr = nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL,
                                               .dst = -1,
                                               .a = -1,
                                               .b = -1,
                                               .imm = 0,
                                               .symbol = sym});
  if (addr < 0)
    nyr_fail(b, NY_NATIVE_ALLOC_FAIL);
  return addr;
}

static bool nyr_trace_requested(const ny_options *opt) {
  /*
   * Trace instrumentation is a compiler option.  Runtime environment
   * variables control an already-emitted executable, but must not silently
   * change its generated ABI or code shape.
   */
  if (!opt)
    return false;
  return opt->trace_exec || opt->debug_everything;
}

static bool nyr_emit_trace_enter(nyr_builder_impl_t *b, const char *name,
                                 const char *source_file, int line) {
  if (!b || !b->trace_instrumented)
    return true;
  int fn = nyr_emit_const_cstr(b, name && name[0] ? name : "<anon>");
  int file = nyr_emit_const_cstr(
      b, source_file && source_file[0] ? source_file : "<unknown>");
  int line_value = nyr_emit_const(b, line > 0 ? line : 1);
  if (fn < 0 || file < 0 || line_value < 0)
    return false;
  /*
   * Trace constants are raw C pointers; trace storage expects managed
   * strings.
   */
  fn = nyr_emit_runtime_call(b, "rt_cstr_to_str", fn, -1, -1, 1, 0);
  file = nyr_emit_runtime_call(b, "rt_cstr_to_str", file, -1, -1, 1, 0);
  if (fn < 0 || file < 0)
    return false;
  return nyr_emit_runtime_call(b, "rt_trace_enter", fn, file, line_value, 3,
                               0) >= 0;
}

#include "lower/special.h"

/*
 * Attached operator lookup is implemented below with the other AST search
 * helpers, but arithmetic lowering needs its declaration first.
 */
static const stmt_t *nyr_find_operator(const nyr_builder_impl_t *b,
                                       const expr_t *e);
static const char *nyr_expr_type_name(const nyr_builder_impl_t *b,
                                      const expr_t *e);
static const stmt_t *nyr_find_attached_method(const nyr_builder_impl_t *b,
                                              const expr_t *receiver,
                                              const char *method);

/*
 * Once an expression has been lowered, the defining NYIR instruction is the
 * reliable representation fact.  AST semantic annotations can remain
 * conservative after contextual inference, but an f64 operation/result must
 * never be reinterpreted as an integer at a later ABI boundary.
 */
static nyr_scalar_class_t
nyr_value_scalar_class_impl(const nyr_builder_impl_t *b,
                            const nyr_type_map_t *types, int value,
                            unsigned depth) {
  if (!b || value < 0 || depth > 32)
    return NYIR_SCALAR_UNKNOWN;

  const nyr_inst_t *def = NULL;
  for (size_t i = b->nyir.len; i > 0; --i) {
    if (b->nyir.data[i - 1].dst == value) {
      def = &b->nyir.data[i - 1];
      break;
    }
  }
  if (!def)
    return NYIR_SCALAR_UNKNOWN;

  /*
   * Loads from reused local slots can keep an obsolete class copied from an
   * earlier occupant. Resolve only an unambiguous same-block reaching store;
   * branches and calls may change the reaching value or expose the slot.
   */
  if (def->op == NYIR_LOAD_LOCAL) {
    size_t def_at = 0;
    for (; def_at < b->nyir.len && &b->nyir.data[def_at] != def; ++def_at) {
    }
    for (size_t i = def_at; i > 0; --i) {
      const nyr_inst_t *prev = &b->nyir.data[i - 1];
      if (prev->op == NYIR_STORE_LOCAL && prev->imm == def->imm) {
        if (prev->scalar_class != NYIR_SCALAR_UNKNOWN)
          return prev->scalar_class;
        return nyr_value_scalar_class_impl(b, types, prev->a, depth + 1);
      }
      if (prev->op == NYIR_LABEL || prev->op == NYIR_BR ||
          prev->op == NYIR_BR_IF || prev->op == NYIR_CALL ||
          (prev->op == NYIR_ADDR_LOCAL && prev->imm == def->imm))
        break;
    }
  }

  /*
   * Explicit producer facts are the ABI authority for non-float values too.
   */
  if (def->scalar_class != NYIR_SCALAR_UNKNOWN)
    return def->scalar_class;

  /*
   * Copies and joins preserve a class only when every incoming value agrees.
   */
  if (def->op == NYIR_COPY)
    return nyr_value_scalar_class_impl(b, types, def->a, depth + 1);
  if (def->op == NYIR_PHI && def->phi_incoming_len) {
    nyr_scalar_class_t joined = NYIR_SCALAR_UNKNOWN;
    for (size_t i = 0; i < def->phi_incoming_len; ++i) {
      nyr_scalar_class_t incoming = nyr_value_scalar_class_impl(
          b, types, def->phi_incoming[i].value, depth + 1);
      if (incoming == NYIR_SCALAR_UNKNOWN)
        return NYIR_SCALAR_UNKNOWN;
      if (joined != NYIR_SCALAR_UNKNOWN && joined != incoming)
        return NYIR_SCALAR_UNKNOWN;
      joined = incoming;
    }
    if (joined != NYIR_SCALAR_UNKNOWN)
      return joined;
  }

  /*
   * The opcode itself is an exact producer fact for scalar floating
   * instructions, even in legacy/synthesized IR where scalar_class was not
   * stamped. Do not ask the type-map analysis to rediscover information the
   * defining instruction already states.
   */
  switch (def->op) {
  case NYIR_CONST_F32:
  case NYIR_ADD_F32:
  case NYIR_SUB_F32:
  case NYIR_MUL_F32:
  case NYIR_DIV_F32:
  case NYIR_I64_TO_F32:
    return NYIR_SCALAR_F32;
  case NYIR_CONST_F64:
  case NYIR_ADD_F64:
  case NYIR_SUB_F64:
  case NYIR_MUL_F64:
  case NYIR_DIV_F64:
  case NYIR_I64_TO_F64:
  case NYIR_F32_TO_F64:
  case NYIR_SQRT_F64:
  case NYIR_SIN_F64:
  case NYIR_COS_F64:
    return NYIR_SCALAR_F64;
  default:
    break;
  }

  /*
   * Some float opcodes predate explicit result annotations. Keep their
   * established producer analysis as the fallback, without inferring other
   * classes from an integer-shaped instruction.
   */
  nyr_scalar_class_t result = NYIR_SCALAR_UNKNOWN;
  if (types && (size_t)value < types->value_count) {
    bool f32 = types->value_f32[value];
    bool f64 = types->value_f64[value];
    if (f32 != f64)
      result = f64 ? NYIR_SCALAR_F64 : NYIR_SCALAR_F32;
  }
  return result;
}

static nyr_scalar_class_t nyr_value_scalar_class(const nyr_builder_impl_t *b,
                                                  int value) {
  if (!b || value < 0)
    return NYIR_SCALAR_UNKNOWN;
  /*
   * The common case is explicit producer metadata or an exact FP opcode.
   * Resolve those directly before constructing the whole-function type map;
   * building that map on every ABI-boundary query made large imported
   * functions spend quadratic time repeatedly rediscovering stamped classes.
   */
  nyr_scalar_class_t result = nyr_value_scalar_class_impl(b, NULL, value, 0);
  if (result != NYIR_SCALAR_UNKNOWN)
    return result;
  nyr_type_map_t types = {0};
  bool have_types =
      nyr_type_map_init(&types, &b->nyir, nyr_max_local(&b->nyir));
  result = have_types ? nyr_value_scalar_class_impl(b, &types, value, 0)
                      : NYIR_SCALAR_UNKNOWN;
  if (have_types)
    nyr_type_map_free(&types);
  return result;
}

static bool nyr_value_is_f64(const nyr_builder_impl_t *b, int value,
                             unsigned depth) {
  (void)depth;
  return nyr_value_scalar_class(b, value) == NYIR_SCALAR_F64;
}

static bool nyr_value_is_f32(const nyr_builder_impl_t *b, int value,
                             unsigned depth) {
  (void)depth;
  return nyr_value_scalar_class(b, value) == NYIR_SCALAR_F32;
}

static bool nyr_value_is_f64(const nyr_builder_impl_t *b, int value,
                             unsigned depth);
#include "lower/arith.h"

/*
 * Native dictionaries use the raw zero word for nil.  Keep integer keys in
 * the same tagged form used at dynamic boundaries so nil and integer zero
 * remain distinct without changing the scalar integer ABI elsewhere.
 */
static int nyr_lower_dict_key(nyr_builder_impl_t *b, const expr_t *e) {
  int key = nyr_lower_expr(b, e);
  bool integer_literal = e && e->kind == NY_E_LITERAL &&
                         e->as.literal.kind == NY_LIT_INT &&
                         e->tok.kind != NY_T_NIL;
  bool raw_integer = integer_literal ||
                     (e && e->semantic.rep == NY_SEM_REP_RAW_INT) ||
                     nyr_value_scalar_class(b, key) == NYIR_SCALAR_I64_RAW;
  if (key < 0 || !e || nyr_expr_is_cstr(b, e) ||
      nyr_expr_is_immutable_nil(b, e, 0) || nyr_expr_is_bool(b, e) ||
      !raw_integer)
    return key;
  int one = nyr_emit_const(b, 1);
  if (one < 0)
    return -1;
  int shifted = nyr_ir_emit(
      &b->nyir,
      (nyr_inst_t){.op = NYIR_SHL_I64, .dst = -1, .a = key, .b = one});
  if (shifted < 0) {
    return nyr_fail(b, NY_NATIVE_ALLOC_FAIL), -1;
  }
  return nyr_ir_emit(
      &b->nyir,
      (nyr_inst_t){.op = NYIR_OR_I64, .dst = -1, .a = shifted, .b = one});
}

#include "lower/call.h" /* call ABI lowering */

#include "lower/expr.h"

#include "lower/stmt.h"

static bool nyr_expr_is_range(const nyr_builder_impl_t *b, const expr_t *e) {
  if (!b || !e)
    return false;
  if (e->kind == NY_E_BINARY && e->as.binary.op &&
      strcmp(e->as.binary.op, "..") == 0)
    return true;
  if (e->kind == NY_E_CALL) {
    const char *leaf = ny_native_call_leaf(e);
    /*
     * A user function owning the `range` spelling decides first: its
     * declared return type is the fact. The stdlib spelling check only
     * applies when no user body was resolved, so a user `range` returning
     * a list/string is not misclassified as a raw native range by the ~20
     * downstream ABI consumers of this predicate.
     */
    const stmt_t *fn =
        leaf ? nyr_find_user_function((nyr_builder_t *)b, leaf) : NULL;
    if (fn && fn->as.fn.return_type)
      return strcmp(fn->as.fn.return_type, "range") == 0;
    if (leaf && (strcmp(leaf, "range") == 0 || strcmp(leaf, "range2") == 0))
      return true;
    return false;
  }
  if (e->kind == NY_E_MEMCALL) {
    /*
     * Method form: the attached-method/canonical callee return type is the
     * fact; the spelling only fires when no user method owns the name.
     */
    const stmt_t *attached =
        e->as.memcall.target && e->as.memcall.target->kind == NY_E_IDENT
            ? nyr_find_user_function((nyr_builder_t *)b, e->as.memcall.name)
            : NULL;
    if (attached && attached->as.fn.return_type)
      return strcmp(attached->as.fn.return_type, "range") == 0;
    if (e->as.memcall.name && (strcmp(e->as.memcall.name, "range") == 0 ||
                               strcmp(e->as.memcall.name, "range2") == 0))
      return true;
  }
  if (e->kind == NY_E_IDENT) {
    /*
     * The comptime interpreter can materialize a range through a top-level
     * immutable binding whose generic semantic representation is only
     * "object". Recover the concrete producer before following the ordinary
     * global initializer path, which cannot see through NY_E_COMPTIME.
     */
    if ((!b->current_fn_name || strcmp(b->current_fn_name, "rt_main") == 0 ||
         strcmp(b->current_fn_name, "_ny_top_entry") == 0) &&
        e->as.ident.name) {
      const stmt_t *decl = nyr_find_top_level_var_stmt(b, e->as.ident.name);
      if (decl && decl->kind == NY_S_VAR) {
        for (size_t i = 0; i < decl->as.var.names.len; ++i) {
          const char *name = decl->as.var.names.data[i];
          if (name && strcmp(name, e->as.ident.name) == 0 &&
              i < decl->as.var.exprs.len && decl->as.var.exprs.data[i] &&
              decl->as.var.exprs.data[i]->kind == NY_E_COMPTIME)
            return nyr_is_comptime_range_expr(b, decl->as.var.exprs.data[i]);
        }
      }
    }
    const expr_t *global = nyr_find_top_level_value(b, e->as.ident.name);
    return global && global != e && global->kind != NY_E_IDENT &&
           nyr_expr_is_range(b, global);
  }
  return false;
}
