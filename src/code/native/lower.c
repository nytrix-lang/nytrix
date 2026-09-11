/*
 * Native lowering: translates compiler IR into NYIR, selecting
 * operand widths, applying ABI constraints, and emitting NYIR ops.
 *
 * Large lowering regions are kept in focused include fragments. This preserves
 * declaration order and a single translation unit while separating special/asm,
 * arithmetic, call, expression, and statement lowering for review and testing.
 */
#include "base/common.h"
#include "base/time.h"
#include "base/trace.h"
#include "base/util.h"
#include "code/ffi/c/c.h"
#include "code/ir/internal.h"
#include "code/native/internal.h"
#include "code/parse/proof.h"
#include "code/priv.h"
#include "code/typing/pipeline.h"

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
 * Unlike the LLVM codegen layer (src/code/native/llvm/legacy/binary.c), the
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
 * ny_native_nir_index_fin_bound_elision matches a Fin-indexed access against
 * the buffer's comptime byte length (tbuf_new provenance via
 * ny_native_extract_tbuf_new_len), and drops the NYIR_BOUNDS_CHECK when the
 * bound covers the access.  load64/store64/f64buf_load/store call it.
 *     (NY_NATIVE_CAP_AST_FALLBACK / prefer_ast_fallback).
 */

static bool ny_native_nir_binop(const char *op, nyir_op_t *out) {
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
  else if (strcmp(op, "^^") == 0)
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
static bool ny_native_nir_const_i64_expr(const expr_t *e, int64_t *out,
                                         unsigned depth) {
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
    if (!ny_native_nir_const_i64_expr(e->as.unary.right, &value, depth + 1))
      return false;
    if (strcmp(e->as.unary.op, "-") == 0 && value == INT64_MIN)
      return false;
    *out = strcmp(e->as.unary.op, "-") == 0 ? -value : value;
    return true;
  }
  if (e->kind == NY_E_BINARY && e->as.binary.op &&
      strcmp(e->as.binary.op, "^") == 0) {
    int64_t base = 0, exponent = 0;
    if (!ny_native_nir_const_i64_expr(e->as.binary.left, &base, depth + 1) ||
        !ny_native_nir_const_i64_expr(e->as.binary.right, &exponent,
                                      depth + 1) ||
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

static bool ny_native_nir_fold_const_pow(const expr_t *left,
                                         const expr_t *right, int64_t *out) {
  int64_t base = 0, exponent_value = 0;
  if (!left || !right || !out ||
      !ny_native_nir_const_i64_expr(left, &base, 0) ||
      !ny_native_nir_const_i64_expr(right, &exponent_value, 0) ||
      exponent_value < 0)
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

static bool ny_native_nir_cmp(const char *op, nyir_cmp_t *out) {
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

static const char *ny_native_leaf_name(const char *name) {
  if (!name)
    return NULL;
  const char *dot = strrchr(name, '.');
  return dot ? dot + 1 : name;
}

static const char *ny_native_call_leaf(const expr_t *e) {
  if (!e || e->kind != NY_E_CALL || !e->as.call.callee)
    return NULL;
  if (e->as.call.callee->kind == NY_E_IDENT)
    return ny_native_leaf_name(e->as.call.callee->as.ident.name);
  if (e->as.call.callee->kind == NY_E_MEMBER)
    return ny_native_leaf_name(e->as.call.callee->as.member.name);
  return NULL;
}
typedef enum {
  NY_NATIVE_LEAF_NONE = 0,
  NY_NATIVE_LEAF_INTRINSIC,
  NY_NATIVE_LEAF_ASSERT,
  NY_NATIVE_LEAF_PRINT,
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
  NY_NATIVE_LEAF_STORE8,
  NY_NATIVE_LEAF_LOAD32,
  NY_NATIVE_LEAF_LOAD64,
  NY_NATIVE_LEAF_LOAD64_IDX,
  NY_NATIVE_LEAF_STORE64,
  NY_NATIVE_LEAF_STORE64_H,
  NY_NATIVE_LEAF_STORE64_IDX,
  NY_NATIVE_LEAF_IS_STR,
} ny_native_leaf_kind_t;

static ny_native_leaf_kind_t ny_native_leaf_kind(const char *leaf) {
  static const struct {
    const char *name;
    ny_native_leaf_kind_t kind;
  } names[] = {
      {"intrinsic", NY_NATIVE_LEAF_INTRINSIC},
      {"assert", NY_NATIVE_LEAF_ASSERT},
      {"print", NY_NATIVE_LEAF_PRINT},
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
      {"store8", NY_NATIVE_LEAF_STORE8},
      {"__store8_idx", NY_NATIVE_LEAF_STORE8},
      {"load32", NY_NATIVE_LEAF_LOAD32},
      {"__load32_idx", NY_NATIVE_LEAF_LOAD32},
      {"load64_i", NY_NATIVE_LEAF_LOAD64},
      {"load64_h", NY_NATIVE_LEAF_LOAD64},
      {"load64", NY_NATIVE_LEAF_LOAD64},
      {"__load64_h", NY_NATIVE_LEAF_LOAD64},
      {"__load64_idx", NY_NATIVE_LEAF_LOAD64_IDX},
      {"store64_i", NY_NATIVE_LEAF_STORE64},
      {"store64_h", NY_NATIVE_LEAF_STORE64_H},
      {"store64", NY_NATIVE_LEAF_STORE64},
      {"__store64_h", NY_NATIVE_LEAF_STORE64_H},
      {"__store64_idx", NY_NATIVE_LEAF_STORE64_IDX},
      {"is_str", NY_NATIVE_LEAF_IS_STR},
      {"__is_str_obj", NY_NATIVE_LEAF_IS_STR},
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
 * src/code/runtime/defs.h.  Native NYIR calls use raw i64 values, matching the
 * runtime ABI for these helpers; keep the mapping generated from the single
 * runtime definition table instead of maintaining a second list here.
 */
static const char *ny_native_runtime_symbol(const char *name) {
  if (!name || !*name)
    return NULL;
  if (strcmp(name, "rt_dict_get_str_raw") == 0)
    return "rt_dict_get_str_raw";
  if (strcmp(name, "__read_off") == 0)
    return "rt_read_off";
  if (strcmp(name, "__write_off") == 0)
    return "rt_write_off";
  /*
   * Direct NYIR carries unboxed scalar values.  Runtime entry points whose
   * public ABI boxes ints/floats therefore need the native bridge rather than
   * the tagged interpreter/LLVM entry point.
   */
  if (strcmp(name, "__has_tag") == 0)
    return "rt_native_has_tag";
  if (strcmp(name, "__tagof") == 0)
    return "rt_value_tag";
  if (strcmp(name, "__type_name_tagged") == 0)
    return "rt_type_name_tagged";
  if (strcmp(name, "rt_tag_or_raw_int") == 0)
    return "rt_tag_or_raw_int";
  if (strcmp(name, "__unwrap") == 0)
    return "rt_result_unwrap_raw";
  if (strcmp(name, "__result_ok") == 0)
    return "rt_result_ok";
  if (strcmp(name, "__result_err") == 0)
    return "rt_result_err";
  if (strcmp(name, "__malloc") == 0 || strcmp(name, "rt_malloc") == 0)
    return "rt_malloc";
  if (strcmp(name, "__ptr_add") == 0)
    return "rt_ptr_add_i64";
  if (strcmp(name, "__ptr_sub") == 0)
    return "rt_ptr_sub_i64";
  if (strcmp(name, "__free") == 0 || strcmp(name, "rt_free") == 0)
    return "rt_free";
  if (strcmp(name, "realloc") == 0 || strcmp(name, "__realloc") == 0)
    return "rt_realloc_raw";
  if (strcmp(name, "__str_builder_new") == 0)
    return "rt_str_builder_new";
  if (strcmp(name, "__str_builder_append") == 0)
    return "rt_str_builder_append";
  if (strcmp(name, "__str_builder_to_str") == 0)
    return "rt_str_builder_to_str";
  if (strcmp(name, "__str_builder_free") == 0)
    return "rt_str_builder_free";
  if (strcmp(name, "dict_get") == 0 || strcmp(name, "__dict_get") == 0 ||
      strcmp(name, "__dict_get_raw") == 0)
    return "rt_dict_get_raw";
  if (strcmp(name, "dict_get_i64") == 0 ||
      strcmp(name, "__dict_get_i64") == 0 ||
      strcmp(name, "__dict_get_i64_raw") == 0)
    return "rt_dict_get_i64_raw";
  if (strcmp(name, "dict_set") == 0 || strcmp(name, "__dict_set") == 0 ||
      strcmp(name, "__dict_set_raw") == 0 ||
      strcmp(name, "__dict_set_i64_raw") == 0)
    return "rt_native_dict_set_nir_i64";
  if (strcmp(name, "dict_set_str") == 0 || strcmp(name, "__dict_set_str") == 0 ||
      strcmp(name, "__dict_set_str_raw") == 0)
    return "rt_native_dict_set_str_compact";
  if (strcmp(name, "dict_set_i64") == 0 || strcmp(name, "__dict_set_i64") == 0)
    return "rt_dict_set_i64_raw";
  if (strcmp(name, "dict_exists") == 0 || strcmp(name, "__dict_exists") == 0 ||
      strcmp(name, "dict_has") == 0 || strcmp(name, "__dict_has") == 0)
    return "rt_dict_has_raw";
  if (strcmp(name, "dict_len") == 0 || strcmp(name, "__dict_len") == 0 ||
      strcmp(name, "__dict_len_raw") == 0)
    return "rt_dict_len_raw";
  if (strcmp(name, "dict_merge") == 0 || strcmp(name, "__dict_merge") == 0)
    return "rt_dict_merge_raw";
  if (strcmp(name, "dict_clear") == 0 || strcmp(name, "__dict_clear") == 0)
    return "rt_dict_clear_raw";
  if (strcmp(name, "dict_remove") == 0 || strcmp(name, "__dict_remove") == 0 ||
      strcmp(name, "dict_del") == 0 || strcmp(name, "__dict_del") == 0)
    return "rt_dict_delete_raw";
  if (strcmp(name, "dict_clone") == 0 || strcmp(name, "__dict_clone") == 0)
    return "rt_dict_clone_raw";
  if (strcmp(name, "dict_keys") == 0 || strcmp(name, "__dict_keys") == 0)
    return "rt_dict_keys_raw";
  if (strcmp(name, "dict_values") == 0 || strcmp(name, "__dict_values") == 0)
    return "rt_dict_values_raw";
  if (strcmp(name, "dict_items") == 0 || strcmp(name, "__dict_items") == 0)
    return "rt_dict_items_raw";
  if (strcmp(name, "__str_eq") == 0 || strcmp(name, "rt_str_eq") == 0)
    return "rt_cstr_eq";
  if (strcmp(name, "__cstr_to_str") == 0)
    return "rt_cstr_to_str";
  /*
   * Attached `.long` methods are semantic aliases of the runtime conversion;
   * keep their canonical names on the same raw-value ABI as `any.long`.
   */
  if (strcmp(name, "__long") == 0 || strcmp(name, "long") == 0 ||
      strcmp(name, "std.core.any.long") == 0 ||
      strcmp(name, "std.core.list.long") == 0 ||
      strcmp(name, "std.core.str.long") == 0 ||
      strcmp(name, "std.core.bytes.long") == 0)
    return "rt_long";
  if (strcmp(name, "__tag") == 0)
    return "rt_tag";
  if (strcmp(name, "__untag") == 0)
    return "rt_untag";
  if (strcmp(name, "__str_len") == 0 || strcmp(name, "rt_str_len") == 0)
    return "rt_cstr_len";
  if (strcmp(name, "__str_concat") == 0 || strcmp(name, "rt_str_concat") == 0)
    return "rt_cstr_concat";
  /*
   * Native NYIR represents f64 expressions as raw IEEE bits.  The public
   * complex accessors return boxed NyValues, so use their raw-bit siblings at
   * this ABI boundary.
   */
  if (strcmp(name, "__complex_real") == 0)
    return "rt_complex_re_bits";
  if (strcmp(name, "__complex_imag") == 0)
    return "rt_complex_im_bits";
  if (strcmp(name, "__load_item") == 0 || strcmp(name, "__load_item_fast") == 0)
    return "rt_load_item";
  if (strcmp(name, "__store_item") == 0 ||
      strcmp(name, "__store_item_fast") == 0)
    return "rt_store_item";
  if (strcmp(name, "len") == 0 || strcmp(name, "std.core.len") == 0 ||
      strcmp(name, "__len") == 0 || strcmp(name, "rt_len") == 0)
    return "rt_len";
  if (strcmp(name, "contains") == 0 || strcmp(name, "std.core.contains") == 0 ||
      strcmp(name, "__contains") == 0)
    return "rt_contains_raw";
  if (strcmp(name, "std.math.float.is_float") == 0 ||
      strcmp(name, "is_float") == 0)
    return "rt_is_float_obj";
  if (strcmp(name, "file_exists") == 0 ||
      strcmp(name, "std.os.file_exists") == 0)
    return "rt_file_exists";
  if (strcmp(name, "get_terminal_size") == 0 ||
      strcmp(name, "std.core.term.get_terminal_size") == 0)
    return "rt_terminal_size_raw";
  if (strcmp(name, "write_str") == 0 ||
      strcmp(name, "std.core.term.write_str") == 0)
    return "rt_print_cstr";
  if (strcmp(name, "color") == 0 || strcmp(name, "std.core.term.color") == 0)
    return "rt_color_raw";
  if (strcmp(name, "bytes") == 0 || strcmp(name, "std.core.bytes") == 0)
    return "rt_bytes_new_raw";
  if (strcmp(name, "bytes_get") == 0 || strcmp(name, "std.core.bytes_get") == 0)
    return "rt_bytes_get_raw";
  if (strcmp(name, "bytes_set") == 0 || strcmp(name, "std.core.bytes_set") == 0)
    return "rt_bytes_set_raw";
  if (strcmp(name, "args") == 0 || strcmp(name, "std.os.args.args") == 0)
    return "rt_args_raw";
  if (strcmp(name, "canvas") == 0 || strcmp(name, "std.core.term.canvas") == 0)
    return "rt_canvas";
  if (strcmp(name, "canvas_clear") == 0 ||
      strcmp(name, "std.core.term.canvas_clear") == 0)
    return "rt_canvas_clear";
  if (strcmp(name, "canvas_set") == 0 ||
      strcmp(name, "std.core.term.canvas_set") == 0)
    return "rt_canvas_set";
  if (strcmp(name, "canvas_refresh") == 0 ||
      strcmp(name, "std.core.term.canvas_refresh") == 0)
    return "rt_canvas_refresh";
  if (strcmp(name, "tui_canvas_loop") == 0 ||
      strcmp(name, "std.core.term.tui_canvas_loop") == 0)
    return "rt_tui_canvas_loop";
  if (strcmp(name, "tui_begin") == 0 ||
      strcmp(name, "std.core.term.tui_begin") == 0)
    return "rt_tui_begin";
  if (strcmp(name, "tui_end") == 0 ||
      strcmp(name, "std.core.term.tui_end") == 0)
    return "rt_tui_end";
  if (strcmp(name, "poll_key") == 0 ||
      strcmp(name, "std.core.term.poll_key") == 0)
    return "rt_poll_key";
  if (strcmp(name, "is_quit_key") == 0 ||
      strcmp(name, "std.core.term.is_quit_key") == 0)
    return "rt_is_quit_key";
  if (strcmp(name, "get_key") == 0 ||
      strcmp(name, "std.core.term.get_key") == 0)
    return "rt_poll_key";
  if (strcmp(name, "std.os.sound.init") == 0 || strcmp(name, "init") == 0)
    return "rt_sound_init";
  if (strcmp(name, "get_backend_name") == 0 ||
      strcmp(name, "std.os.sound.get_backend_name") == 0)
    return "rt_sound_backend_name";
  if (strcmp(name, "make_sine_source") == 0 ||
      strcmp(name, "write_synth_wav") == 0 || strcmp(name, "play") == 0 ||
      strcmp(name, "stop") == 0 || strcmp(name, "is_playing") == 0 ||
      strcmp(name, "load") == 0 || strcmp(name, "shutdown") == 0 ||
      strcmp(name, "file_remove") == 0 ||
      strcmp(name, "std.os.sound.source.synth.make_sine_source") == 0 ||
      strcmp(name, "std.os.sound.source.synth.write_synth_wav") == 0 ||
      strcmp(name, "std.os.sound.play") == 0 ||
      strcmp(name, "std.os.sound.stop") == 0 ||
      strcmp(name, "std.os.sound.is_playing") == 0 ||
      strcmp(name, "std.os.sound.load") == 0 ||
      strcmp(name, "std.os.sound.shutdown") == 0 ||
      strcmp(name, "std.os.file_remove") == 0)
    return "rt_sound_noop";
  if (strcmp(name, "list") == 0 || strcmp(name, "std.core.list") == 0)
    return "rt_list_new_raw";
  if (strcmp(name, "__list_set_len") == 0 ||
      strcmp(name, "std.core.primitives.__list_set_len") == 0)
    return "rt_list_set_len";
  if (strcmp(name, "exit") == 0 || strcmp(name, "std.os.exit") == 0)
    return "rt_exit";
  if (strcmp(name, "__runtime_tag") == 0 ||
      strcmp(name, "runtime_tag_raw") == 0)
    return "rt_runtime_tag_raw_value";
  if (strcmp(name, "rt_tbuf_swap") == 0)
    return "rt_tbuf_swap";
  if (strcmp(name, "__globals") == 0 || strcmp(name, "globals") == 0 ||
      strcmp(name, "std.core.primitives.globals") == 0 ||
      strcmp(name, "std.core.reflect.globals") == 0)
    return "rt_globals_get";
  if (strcmp(name, "__set_globals") == 0 || strcmp(name, "set_globals") == 0 ||
      strcmp(name, "std.core.primitives.set_globals") == 0)
    return "rt_globals_set";
  if (strcmp(name, "__bytes_new") == 0 || strcmp(name, "bytes_new_raw") == 0 ||
      strcmp(name, "rt_bytes_new") == 0)
    return "rt_bytes_new";
  if (strcmp(name, "mutex_new") == 0 || strcmp(name, "__mutex_new") == 0)
    return "rt_mutex_new";
  if (strcmp(name, "mutex_lock") == 0 || strcmp(name, "__mutex_lock64") == 0)
    return "rt_mutex_lock64";
  if (strcmp(name, "mutex_unlock") == 0 ||
      strcmp(name, "__mutex_unlock64") == 0)
    return "rt_mutex_unlock64";
  if (strcmp(name, "mutex_free") == 0 || strcmp(name, "__mutex_free") == 0)
    return "rt_mutex_free";
  if (strcmp(name, "thread_join") == 0 || strcmp(name, "__thread_join") == 0)
    return "rt_thread_join";
  if (strcmp(name, "thread_spawn_call") == 0 ||
      strcmp(name, "__thread_spawn_call") == 0)
    return "rt_thread_spawn_call";
  if (strcmp(name, "thread_launch_call") == 0 ||
      strcmp(name, "__thread_launch_call") == 0)
    return "rt_thread_launch_call";
  if (strcmp(name, "__is_int") == 0)
    return "rt_native_is_int";
  if (strcmp(name, "__bigint_from_int") == 0)
    return "rt_bigint_from_int";
  if (strcmp(name, "__bigint_to_str") == 0)
    return "rt_bigint_to_cstr_raw";
  if (strcmp(name, "__bigint_to_int") == 0)
    return "rt_bigint_to_int";
  if (strcmp(name, "__bigint_add") == 0)
    return "rt_bigint_add";
  if (strcmp(name, "__bigint_sub") == 0)
    return "rt_bigint_sub";
  if (strcmp(name, "__bigint_mul") == 0)
    return "rt_bigint_mul";
  if (strcmp(name, "__bigint_div") == 0)
    return "rt_bigint_div";
  if (strcmp(name, "__bigint_mod") == 0)
    return "rt_bigint_mod";
  if (strcmp(name, "__bigint_pow") == 0)
    return "rt_bigint_pow";
  if (strcmp(name, "__bigint_xor") == 0)
    return "rt_bigint_xor";
  if (strcmp(name, "__bigint_cmp") == 0)
    return "rt_bigint_cmp_raw";
  if (strcmp(name, "__bigint_bitlen") == 0)
    return "rt_bigint_bitlen_raw";
  if (strcmp(name, "__bigfloat_from_value") == 0)
    return "rt_bigfloat_from_value_raw";
  if (strcmp(name, "__bigfloat_zero") == 0)
    return "rt_bigfloat_zero";
  if (strcmp(name, "__bigfloat_one") == 0)
    return "rt_bigfloat_one";
  if (strcmp(name, "__bigfloat_add") == 0)
    return "rt_bigfloat_add";
  if (strcmp(name, "__bigfloat_sub") == 0)
    return "rt_bigfloat_sub";
  if (strcmp(name, "__bigfloat_mul") == 0)
    return "rt_bigfloat_mul";
  if (strcmp(name, "__bigfloat_div") == 0)
    return "rt_bigfloat_div";
  if (strcmp(name, "__bigfloat_neg") == 0)
    return "rt_bigfloat_neg";
  if (strcmp(name, "__bigfloat_abs") == 0)
    return "rt_bigfloat_abs";
  if (strcmp(name, "__bigfloat_sqrt") == 0)
    return "rt_bigfloat_sqrt";
  if (strcmp(name, "__bigfloat_to_f64") == 0)
    return "rt_bigfloat_to_f64_raw";
  if (strcmp(name, "__bigfloat_cmp") == 0)
    return "rt_bigfloat_cmp_raw";
  if (strcmp(name, "__bigfloat_precision") == 0)
    return "rt_bigfloat_precision_raw";
  if (strcmp(name, "__bigfloat_pow_int") == 0)
    return "rt_bigfloat_pow_int_raw";
  if (strcmp(name, "__bigfloat_to_str") == 0)
    return "rt_bigfloat_to_str";
  if (strcmp(name, "__bigfloat_is") == 0)
    return "rt_bigfloat_is";
  /*
   * System math headers are not reliably reducible to a single parsed
   * prototype on every libc (feature macros and nested includes vary).  Keep
   * the common scalar C entry points on the runtime's raw-f64 ABI.
   */
  if (strcmp(name, "sin") == 0)
    return "rt_sin_f64";
  if (strcmp(name, "cos") == 0)
    return "rt_cos_f64";
  if (strcmp(name, "sqrt") == 0)
    return "rt_sqrt_f64";
  if (strcmp(name, "os") == 0)
    return "rt_os_name";
  if (strcmp(name, "arch") == 0)
    return "rt_arch_name";
  if (strcmp(name, "__simmd_rotl32") == 0)
    return "rt_simmd_rotl32_i64";
  if (strcmp(name, "__simmd_rotr32") == 0)
    return "rt_simmd_rotr32_i64";
  if (strcmp(name, "__simmd_rotl64") == 0)
    return "rt_simmd_rotl64_i64";
  if (strcmp(name, "__simmd_rotr64") == 0)
    return "rt_simmd_rotr64_i64";
  if (strcmp(name, "__simmd_popcnt32") == 0)
    return "rt_simmd_popcnt32_i64";
  if (strcmp(name, "__simmd_popcnt64") == 0)
    return "rt_simmd_popcnt64_i64";
  if (strcmp(name, "__simmd_clz32") == 0)
    return "rt_simmd_clz32_i64";
  if (strcmp(name, "__simmd_clz64") == 0)
    return "rt_simmd_clz64_i64";
  if (strcmp(name, "__simmd_ctz32") == 0)
    return "rt_simmd_ctz32_i64";
  if (strcmp(name, "__simmd_ctz64") == 0)
    return "rt_simmd_ctz64_i64";
  if (strcmp(name, "__simmd_bswap32") == 0)
    return "rt_simmd_bswap32_i64";
  if (strcmp(name, "__simmd_bswap64") == 0)
    return "rt_simmd_bswap64_i64";
  if (strcmp(name, "__is_ptr") == 0 || strcmp(name, "is_ptr") == 0 ||
      strcmp(name, "std.core.is_ptr") == 0)
    return "rt_value_is_ptr";
  if (strcmp(name, "rt_tbuf_index_read_raw") == 0)
    return "rt_tbuf_index_read_raw";
  if (strcmp(name, "rt_tbuf_index_any") == 0)
    return "rt_tbuf_index_any";
  if (strcmp(name, "rt_tbuf_index_any_raw") == 0)
    return "rt_tbuf_index_any_raw";
  if (strcmp(name, "rt_tbuf_get_any") == 0)
    return "rt_tbuf_get_any";
  if (strcmp(name, "rt_tbuf_to_cstr") == 0)
    return "rt_tbuf_to_cstr";
  if (strcmp(name, "__shl") == 0)
    return "rt_shl_raw";
  if (strcmp(name, "__trace_func") == 0)
    return "rt_trace_func_raw";
  if (strcmp(name, "__trace_loc") == 0)
    return "rt_trace_loc_raw";
  if (strcmp(name, "__trace_enter") == 0)
    return "rt_trace_enter_raw";
  if (strcmp(name, "__print_flush") == 0)
    return "rt_print_flush_raw";
  if (strcmp(name, "__trace_ret_void") == 0)
    return "rt_trace_ret_void_raw";
  if (strcmp(name, "__trace_exit") == 0)
    return "rt_trace_exit_raw";
  if (strcmp(name, "__trace_dump") == 0)
    return "rt_trace_dump_raw";
  if (strcmp(name, "rt_result_unwrap_or_raw") == 0)
    return "rt_result_unwrap_or_raw";
  if (strcmp(name, "rt_index_key_error") == 0)
    return "rt_index_key_error";
  if (strcmp(name, "rt_bytes_index_read_raw") == 0)
    return "rt_bytes_index_read_raw";
  if (strcmp(name, "rt_range_index_read_raw") == 0)
    return "rt_range_index_read_raw";
  if (strcmp(name, "rt_cstr_index_read_raw") == 0)
    return "rt_cstr_index_read_raw";
  if (strcmp(name, "rt_value_is_ptr") == 0)
    return "rt_value_is_ptr";
  if (strcmp(name, "rt_any_mul") == 0)
    return "rt_any_mul";
  if (strcmp(name, "rt_any_div") == 0)
    return "rt_any_div";
  if (strcmp(name, "rt_any_mod") == 0)
    return "rt_any_mod";
#define RT_DEF(n, implementation, args, sig, doc)                              \
  if (strcmp(name, n) == 0)                                                    \
    return #implementation;
#define RT_GV(n, implementation, type, doc)
#include "code/runtime/defs.h"
#undef RT_GV
#undef RT_DEF
  return NULL;
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
  if (e && e->semantic.member_call_kind != NY_SEM_CALL_NONE &&
      e->semantic.canonical_callee) {
    const char *canon =
        ny_native_runtime_symbol(e->semantic.canonical_callee);
    if (canon)
      return canon;
    const char *canon_leaf =
        ny_native_leaf_name(e->semantic.canonical_callee);
    if (canon_leaf && canon_leaf != e->semantic.canonical_callee) {
      canon = ny_native_runtime_symbol(canon_leaf);
      if (canon)
        return canon;
    }
  }
  if (name) {
    const char *sym = ny_native_runtime_symbol(name);
    if (sym)
      return sym;
  }
  if (leaf) {
    const char *sym = ny_native_runtime_symbol(leaf);
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
  const char *name;
  const char *type_name; /* declared nominal owner type, when available */
  int slot;
  ny_sem_rep_t semantic_rep;
  ny_sem_ownership_t semantic_ownership;
  uint32_t alias_class;
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
  bool is_dyn_list; /* stride 24 (descriptors) vs stride 8 (scalars) */
  bool raw_dynamic_index; /* scalar payload awaiting a dynamic callback */
  const expr_t *list_literal; /* current declaration's non-escaping literal */
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

} ny_native_nir_local_t;
typedef struct {
  const expr_t *expr;
  stmt_t *fn;
  char *name;
  char *capture_names[16];
  size_t capture_count;
  ny_native_nir_local_t capture_types[16];
} ny_native_lambda_entry_t;
static ny_native_lambda_entry_t *ny_native_lambda_entries;
static size_t ny_native_lambda_count;
static size_t ny_native_lambda_cap;
static bool ny_native_type_name_is_f64(const char *name);
static bool ny_native_type_name_is_f32(const char *name);
static bool ny_native_type_name_is_int(const char *name);
static bool ny_native_type_name_is_list(const char *name);
static bool ny_native_type_name_is_str(const char *name);
static bool ny_native_type_name_is_any(const char *name);

static ny_native_lambda_entry_t *ny_native_lambda_find(const expr_t *expr) {
  for (size_t i = 0; i < ny_native_lambda_count; ++i)
    if (ny_native_lambda_entries[i].expr == expr)
      return &ny_native_lambda_entries[i];
  return NULL;
}

static ny_native_lambda_entry_t *ny_native_lambda_find_name(const char *name) {
  if (!name)
    return NULL;
  for (size_t i = 0; i < ny_native_lambda_count; ++i)
    if (ny_native_lambda_entries[i].name &&
        strcmp(ny_native_lambda_entries[i].name, name) == 0)
      return &ny_native_lambda_entries[i];
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

static stmt_t *ny_native_lambda_create(const expr_t *e) {
  if (!e || (e->kind != NY_E_FN && e->kind != NY_E_LAMBDA) ||
      !e->as.lambda.body)
    return NULL;
  ny_native_lambda_entry_t *existing = ny_native_lambda_find(e);
  if (existing)
    return existing->fn;
  if (ny_native_lambda_count == ny_native_lambda_cap) {
    size_t cap = ny_native_lambda_cap ? ny_native_lambda_cap * 2 : 64;
    ny_native_lambda_entry_t *grown =
        realloc(ny_native_lambda_entries, cap * sizeof(*grown));
    if (!grown)
      return NULL;
    ny_native_lambda_entries = grown;
    ny_native_lambda_cap = cap;
  }
  stmt_t *fn = calloc(1, sizeof(*fn));
  if (!fn)
    return NULL;
  char name[96];
  int n = snprintf(name, sizeof(name), "__ny_lambda_%d_%d", e->tok.line,
                   e->tok.col);
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
    if (ny_native_type_name_is_f64(fn->as.fn.return_type))
      fn->as.fn.return_semantic.rep = NY_SEM_REP_F64;
    else if (ny_native_type_name_is_f32(fn->as.fn.return_type))
      fn->as.fn.return_semantic.rep = NY_SEM_REP_F32;
    else if (ny_native_type_name_is_any(fn->as.fn.return_type))
      fn->as.fn.return_semantic.rep = NY_SEM_REP_TAGGED_DYNAMIC;
    else if (ny_native_type_name_is_str(fn->as.fn.return_type))
      fn->as.fn.return_semantic.rep = NY_SEM_REP_STRING;
    else if (ny_native_type_name_is_list(fn->as.fn.return_type))
      fn->as.fn.return_semantic.rep = NY_SEM_REP_TYPED_BUFFER;
    else
      fn->as.fn.return_semantic.rep = NY_SEM_REP_RAW_INT;
  }
  fn->as.fn.body = e->as.lambda.body;
  if (e->as.lambda.params.len == 0) {
    fn->as.fn.params.data = calloc(1, sizeof(param_t));
    if (!fn->as.fn.params.data || !fn->as.fn.name) {
      free((void *)fn->as.fn.name);
      free((void *)fn->as.fn.params.data);
      free(fn);
      return NULL;
    }
    fn->as.fn.params.len = 1;
    fn->as.fn.params.cap = 1;
    fn->as.fn.params.data[0].name = "__thread_arg";
    fn->as.fn.params.data[0].type = "int";
  } else {
    fn->as.fn.params = e->as.lambda.params;
  }
  ny_native_lambda_entries[ny_native_lambda_count++] =
      (ny_native_lambda_entry_t){
          .expr = e, .fn = fn, .name = (char *)fn->as.fn.name};
  return fn;
}

static void ny_native_lambda_clear(void) {
  for (size_t i = 0; i < ny_native_lambda_count; ++i) {
    for (size_t j = 0; j < ny_native_lambda_entries[i].capture_count; ++j)
      free(ny_native_lambda_entries[i].capture_names[j]);
    free(ny_native_lambda_entries[i].name);
    free(ny_native_lambda_entries[i].fn);
  }
  free(ny_native_lambda_entries);
  ny_native_lambda_entries = NULL;
  ny_native_lambda_count = 0;
  ny_native_lambda_cap = 0;
}

typedef struct {
  int head_label;
  int continue_label;
  int end_label;
  size_t defer_mark;
} ny_native_nir_loop_frame_t;

typedef struct {
  const char *name;
  int label;
  bool emitted;
} ny_native_nir_label_t;

typedef enum {
  NY_NATIVE_NIR_FACT_ALLOC = 1,
  NY_NATIVE_NIR_FACT_FIN = 2,
  NY_NATIVE_NIR_FACT_LIST_LEN = 3,
  NY_NATIVE_NIR_FACT_DYN_STR_LEN = 4,
  NY_NATIVE_NIR_FACT_DYN_TAG = 5,
} ny_native_nir_fact_kind_t;

typedef struct {
  int value;
  int64_t payload;
  ny_native_nir_fact_kind_t kind;
} ny_native_nir_fact_t;

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

#define NY_NATIVE_C_DEFINE_MAX 1024
typedef struct {
  ny_native_c_define_t entries[NY_NATIVE_C_DEFINE_MAX];
  size_t count;
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

static bool ny_native_c_define_add(ny_native_c_define_table_t *table,
                                   const char *name, int64_t value) {
  if (!table || !name || !*name)
    return true;
  for (size_t i = 0; i < table->count; ++i) {
    if (strcmp(table->entries[i].name, name) == 0) {
      table->entries[i].value = value;
      return true;
    }
  }
  if (table->count >= NY_NATIVE_C_DEFINE_MAX)
    return false;
  char *copy = ny_strdup(name);
  if (!copy)
    return false;
  table->entries[table->count++] = (ny_native_c_define_t){copy, value};
  return true;
}

static bool ny_native_c_define_lookup(const ny_native_c_define_table_t *table,
                                      const char *name, int64_t *value) {
  if (!table || !name)
    return false;
  for (size_t i = table->count; i > 0; --i) {
    const ny_native_c_define_t *entry = &table->entries[i - 1];
    if (strcmp(entry->name, name) == 0) {
      if (value)
        *value = entry->value;
      return true;
    }
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
                                bool ret_f32, const bool *param_f64,
                                const bool *param_f32) {
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
  nyir_func_t nyir;
  ny_native_nir_local_t *locals;
  size_t local_count;
  size_t local_cap;
  int next_local_slot;
  int next_label;
  int last_value;
  size_t scope_depth;
  int resolve_depth;
  ny_native_nir_fact_t *facts;
  size_t fact_count;
  size_t fact_cap;
  ny_native_nir_loop_frame_t *loop_frames;
  size_t loop_depth;
  size_t loop_cap;
  ny_native_nir_label_t *labels;
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
  unsigned return_flags;
  /*
   * Any-return functions use a tagged incoming ABI but expose raw scalar
   * payloads to native NYIR callers.
   */
  bool return_any;
  /* Set while lowering a `case` expression whose result feeds a dynamic
   * consumer: raw scalar arm values must be boxed once at the join. */
  bool match_result_any;
  const char *return_type;
  const ny_extern_table_t *externs;
  const program_t *prog;
  const ny_options *options;
  const char *profile_name;
  const char *current_fn_name;
  const char *source_file;
  const char *module_name;
  const ny_native_lambda_entry_t *lambda_entry;
  int closure_env_slot;
  /*
   * C aggregate layouts discovered while building the native extern table.
   */
  const struct ny_native_c_layout_table_t *c_layouts;
  int64_t current_list_elem_size;
  bool force_dynamic_list;
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
   * Bounded self-tail recursion lowering.  These fields are enabled only for
   * scalar-parameter functions; aggregate ABI values remain ordinary calls.
   */
  int tail_loop_label;
  int *tail_param_slots;
  size_t tail_param_count;
  const stmt_t *tail_body;
  bool tail_recur_enabled;
  int opt_level;
  uint32_t current_syntax_ctx;
  char *err;
  size_t err_len;
} ny_native_nir_builder_t;

static bool ny_native_nir_fn_has_thread_attr(const stmt_t *fn) {
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
 * Handles "Ctor<42>" (literal); symbolic bounds are resolved via ny_native_resolve_indexed_bound.
 */
static int64_t ny_native_parse_indexed_bound(const char *type_name, char *out_ctor, size_t ctor_cap) {
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
  if (!s || !name || !out_value || !out_chain || !out_poisoned ||
      depth > 16 || *out_poisoned)
    return;
  if (s->kind == NY_S_VAR) {
    for (size_t n = 0;
         n < s->as.var.names.len && n < s->as.var.exprs.len; ++n) {
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
      if (init->kind == NY_E_LITERAL &&
          init->as.literal.kind == NY_LIT_INT &&
          init->tok.kind != NY_T_NIL && init->as.literal.as.i > 0) {
        if (*out_value < 0)
          *out_value = init->as.literal.as.i;
        else if (*out_value != init->as.literal.as.i)
          *out_poisoned = true;
        return;
      }
      if (init->kind == NY_E_IDENT && init->as.ident.name) {
        size_t link_len = strlen(init->as.ident.name);
        if (out_chain[0] != '\0' || link_len == 0 ||
            link_len >= chain_cap) {
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
static int64_t ny_native_resolve_fin_sym_depth(
    const ny_native_nir_builder_t *b, const char *sym, unsigned depth) {
  if (!b || !b->prog || !sym || !*sym || depth > 8)
    return -1;
  size_t name_len = strlen(sym);
  if (name_len == 0 || name_len >= 256)
    return -1;
  int64_t value = -1;
  char chain[256] = {0};
  bool poisoned = false;
  for (size_t i = 0; i < b->prog->body.len && !poisoned; ++i)
    ny_native_scan_fin_def(b->prog->body.data[i], sym, name_len, &value,
                           chain, sizeof(chain), &poisoned, 0);
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
 * `Ctor<42>` resolves directly; `Ctor<NAME>` resolves through an immutable top-level
 * `def NAME = <positive int>`. Returns -1 when the bound is not compile-time known.
 */
static int64_t ny_native_resolve_indexed_bound(
    const ny_native_nir_builder_t *b, const char *type_name, char *out_ctor, size_t ctor_cap) {
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

static int64_t ny_native_resolve_fin_bound(
    const ny_native_nir_builder_t *b, const char *type_name) {
  int64_t lit = ny_native_parse_fin_bound(type_name);
  if (lit >= 0)
    return lit;
  char ctor[64] = {0};
  int64_t bound = ny_native_resolve_indexed_bound(b, type_name, ctor, sizeof(ctor));
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
static ny_native_nir_local_t *
ny_native_nir_find_local(ny_native_nir_builder_t *b, const char *name);

static int64_t
ny_native_nir_fin_bound_for_name(const ny_native_nir_builder_t *b,
                                 const char *name, unsigned depth) {
  if (!b || !name || !name[0] || depth > 16)
    return 0;
  ny_native_nir_local_t *local =
      ny_native_nir_find_local((ny_native_nir_builder_t *)b, name);
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
          int64_t bound = ny_native_resolve_fin_bound(b, s->as.var.types.data[vi]);
          if (bound > 0)
            return bound;
        }
        if (vi < s->as.var.exprs.len && s->as.var.exprs.data[vi] &&
            s->as.var.exprs.data[vi]->kind == NY_E_IDENT)
          return ny_native_nir_fin_bound_for_name(
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
        return ny_native_resolve_fin_bound(b,
                                           s->as.fn.params.data[pi].type);
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
          int64_t bound = ny_native_resolve_fin_bound(b, vs->as.var.types.data[vi]);
          if (bound > 0)
            return bound;
        }
        if (vi < vs->as.var.exprs.len && vs->as.var.exprs.data[vi] &&
            vs->as.var.exprs.data[vi]->kind == NY_E_IDENT)
          return ny_native_nir_fin_bound_for_name(
              b, vs->as.var.exprs.data[vi]->as.ident.name, depth + 1);
      }
    }
  }
  return 0;
}

static bool
ny_native_nir_index_fin_bound_elision(const ny_native_nir_builder_t *b,
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
  int64_t fin_bound = ny_native_nir_fin_bound_for_name(b, idx_name, 0);
  return fin_bound > 0 && fin_bound <= INT64_MAX / scale &&
         fin_bound * scale <= buffer_byte_len;
}

/*
 * Resolve the comptime buffer byte length from the first argument of a
 * tbuf intrinsic call (__load64_idx, f64buf_load, etc.).  Returns 0 if
 * the pointer argument isn't a named local with known buffer_byte_len.
 */
static int64_t
ny_native_nir_resolve_buf_byte_len(const ny_native_nir_builder_t *b,
                                   const expr_t *e) {
  if (!b || !e || e->as.call.args.len == 0)
    return 0;
  const expr_t *ptr_arg = e->as.call.args.data[0].val;
  if (!ptr_arg || ptr_arg->kind != NY_E_IDENT)
    return 0;
  ny_native_nir_local_t *bl = ny_native_nir_find_local(
      (ny_native_nir_builder_t *)b, ptr_arg->as.ident.name);
  return (bl && bl->buffer_byte_len > 0) ? bl->buffer_byte_len : 0;
}

static bool ny_native_nir_fail(ny_native_nir_builder_t *b, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void ny_native_nir_builder_dispose(ny_native_nir_builder_t *b) {
  if (!b)
    return;
  free(b->locals);
  free(b->loop_frames);
  free(b->labels);
  free(b->facts);
  free(b->defers);
  free(b->tail_param_slots);
  b->locals = NULL;
  b->loop_frames = NULL;
  b->labels = NULL;
  b->facts = NULL;
  b->defers = NULL;
  b->tail_param_slots = NULL;
  b->local_count = 0;
  b->local_cap = 0;
  b->loop_depth = 0;
  b->loop_cap = 0;
  b->fact_count = 0;
  b->fact_cap = 0;
  b->defer_count = 0;
  b->defer_cap = 0;
  b->tail_param_count = 0;
}

static bool ny_native_nir_push_loop(ny_native_nir_builder_t *b, int head_label,
                                    int continue_label, int end_label) {
  if (!b)
    return false;
  if (b->loop_depth == b->loop_cap) {
    size_t cap = b->loop_cap ? b->loop_cap * 2 : 16;
    if (cap < b->loop_cap || cap > SIZE_MAX / sizeof(*b->loop_frames))
      return ny_native_nir_fail(b,
                                "native NYIR lower: loop table is too large");
    ny_native_nir_loop_frame_t *frames =
        realloc(b->loop_frames, cap * sizeof(*frames));
    if (!frames)
      return ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
    b->loop_frames = frames;
    b->loop_cap = cap;
  }
  b->loop_frames[b->loop_depth++] = (ny_native_nir_loop_frame_t){
      .head_label = head_label,
      .continue_label = continue_label,
      .end_label = end_label,
      .defer_mark = b->defer_count,
  };
  return true;
}

static int ny_native_nir_temp_slot(ny_native_nir_builder_t *b) {
  return b ? b->next_local_slot++ : -1;
}

static size_t ny_native_nir_scope_mark(ny_native_nir_builder_t *b) {
  return b ? b->local_count : 0;
}

static void ny_native_nir_scope_restore(ny_native_nir_builder_t *b,
                                        size_t mark) {
  if (b && mark <= b->local_count)
    b->local_count = mark;
}

/*
 * Forward declarations (the statement lowerer is defined later in this file).
 */
static bool ny_native_nir_lower_stmt(ny_native_nir_builder_t *b,
                                     const stmt_t *s);

/*
 * Push a deferred body so it runs LIFO at the enclosing scope's exit.
 */
static bool ny_native_nir_push_defer(ny_native_nir_builder_t *b, stmt_t *body) {
  if (!b || !body)
    return true;
  if (b->defer_count == b->defer_cap) {
    size_t cap = b->defer_cap ? b->defer_cap * 2 : 16;
    if (cap < b->defer_cap || cap > SIZE_MAX / sizeof(*b->defers))
      return ny_native_nir_fail(b,
                                "native NYIR lower: defer table is too large");
    stmt_t **defers = realloc(b->defers, cap * sizeof(*defers));
    if (!defers)
      return ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
    b->defers = defers;
    b->defer_cap = cap;
  }
  b->defers[b->defer_count++] = body;
  return true;
}

/*
 * Run deferred bodies registered since mark, LIFO, then drop them.
 */
static bool ny_native_nir_emit_defers(ny_native_nir_builder_t *b, size_t mark) {
  if (!b || b->defer_count <= mark)
    return true;
  size_t saved_return = b->emitted_return;
  while (b->defer_count > mark) {
    stmt_t *body = b->defers[--b->defer_count];
    if (body && !ny_native_nir_lower_stmt(b, body)) {
      b->emitted_return = saved_return;
      return false;
    }
    if (b->emitted_return) /* a defer body returned; stop unwinding */
      break;
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
static bool ny_native_nir_lower_scoped_body(ny_native_nir_builder_t *b,
                                            const stmt_t *body) {
  size_t defer_mark = b->defer_count;
  if (!ny_native_nir_lower_stmt(b, body))
    return false;
  if (b->emitted_return)
    return true; /* return/break/continue already ran the defers */
  int saved_last = b->last_value;
  if (!ny_native_nir_emit_defers(b, defer_mark))
    return false;
  b->last_value = saved_last;
  return true;
}

static bool ny_native_nir_fail(ny_native_nir_builder_t *b, const char *fmt,
                               ...) {
  if (!b || !b->err || b->err_len == 0)
    return false;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b->err, b->err_len, fmt, ap);
  va_end(ap);
  return false;
}

static bool ny_native_nir_ignored_stmt(const stmt_t *s) {
  return !s || s->kind == NY_S_USE || s->kind == NY_S_LINK ||
         s->kind == NY_S_INCLUDE || s->kind == NY_S_DEFINE ||
         s->kind == NY_S_EXPORT || s->kind == NY_S_MODULE ||
         s->kind == NY_S_EXTERN || s->kind == NY_S_LAYOUT ||
         s->kind == NY_S_STRUCT || s->kind == NY_S_ENUM ||
         s->kind == NY_S_MACRO || s->kind == NY_S_OPERATOR ||
         s->kind == NY_S_IMPL;
}

static ny_native_nir_local_t *
ny_native_nir_find_local_ctx(ny_native_nir_builder_t *b, const char *name,
                             uint32_t syntax_ctx) {
  if (!b || !name)
    return NULL;
  for (size_t i = b->local_count; i > 0; --i) {
    ny_native_nir_local_t *l = &b->locals[i - 1];
    if (l->name && strcmp(l->name, name) == 0) {
      if (l->syntax_ctx == syntax_ctx)
        return l;
    }
  }
  return NULL;
}

static ny_native_nir_local_t *
ny_native_nir_find_local(ny_native_nir_builder_t *b, const char *name) {
  return ny_native_nir_find_local_ctx(b, name, b ? b->current_syntax_ctx : 0);
}

/*
 * Forward declarations needed by ny_native_nir_expr_is_dyn_list which is
 * defined here but calls helpers defined later in the file.
 */
static bool ny_native_type_name_is_list(const char *name);
static bool ny_native_nir_expr_is_list(const ny_native_nir_builder_t *b,
                                       const expr_t *e);
static bool ny_native_nir_expr_is_dict(const ny_native_nir_builder_t *b,
                                       const expr_t *e);
static bool ny_native_nir_expr_is_f64(ny_native_nir_builder_t *b,
                                      const expr_t *e);
static bool ny_native_nir_expr_is_bigint(ny_native_nir_builder_t *b,
                                         const expr_t *e);
static bool ny_native_nir_expr_is_bigfloat(ny_native_nir_builder_t *b,
                                           const expr_t *e);
static bool ny_native_nir_fold_top_level_int(const program_t *prog,
                                             const expr_t *e, int64_t *out,
                                             unsigned depth);
static const stmt_t *
ny_native_nir_find_imported_function(const ny_native_nir_builder_t *b,
                                     const char *name);
static bool ny_native_nir_expr_is_cstr(ny_native_nir_builder_t *b,
                                       const expr_t *e);
static bool ny_native_nir_expr_is_bytes(const ny_native_nir_builder_t *b,
                                        const expr_t *e);
static bool ny_native_nir_expr_is_bool(ny_native_nir_builder_t *b,
                                       const expr_t *e);
static int ny_native_nir_box_bool(ny_native_nir_builder_t *b, int reg);
static bool ny_native_nir_expr_is_any(ny_native_nir_builder_t *b,
                                      const expr_t *e);
static const stmt_t *
ny_native_nir_find_attached_method(const ny_native_nir_builder_t *b,
                                   const expr_t *receiver, const char *method);
static bool ny_native_nir_expr_is_raw_dynamic_read(ny_native_nir_builder_t *b,
                                                   const expr_t *e);
static bool ny_native_nir_stmt_uses_ident(const stmt_t *s, const char *name);
static bool ny_native_nir_expr_is_range(const ny_native_nir_builder_t *b,
                                        const expr_t *e);
static const expr_t *
ny_native_nir_find_top_level_value(const ny_native_nir_builder_t *b,
                                   const char *name);
static const expr_t *
ny_native_nir_find_imported_value(const ny_native_nir_builder_t *b,
                                  const char *name);
static const expr_t *
ny_native_nir_resolve_member_expr(const ny_native_nir_builder_t *b,
                                  const expr_t *e);
static const stmt_t *
ny_native_nir_find_user_function(ny_native_nir_builder_t *b, const char *name);
static const char *ny_native_call_leaf(const expr_t *e);
static bool ny_native_nir_record_dyn_fact(ny_native_nir_builder_t *b, int value,
                                          ny_native_nir_fact_kind_t kind,
                                          int reg);

static bool ny_native_type_name_is_dyn_list(const char *name) {
  if (!name || !ny_native_type_name_is_list(name))
    return false;
  if (strcmp(name, "list") == 0 || strcmp(name, "list[]") == 0)
    return true;
  if (strstr(name, "<int>") || strstr(name, "<i64>") || strstr(name, "<i32>") ||
      strstr(name, "<i16>") || strstr(name, "<i8>") || strstr(name, "<u64>") ||
      strstr(name, "<u32>") || strstr(name, "<u16>") || strstr(name, "<u8>") ||
      strstr(name, "<f64>") || strstr(name, "<f32>") ||
      strstr(name, "<float>") || strstr(name, "<double>") ||
      strstr(name, "<bool>") || strstr(name, "int[]") ||
      strstr(name, "i64[]") || strstr(name, "i32[]") || strstr(name, "f64[]") ||
      strstr(name, "float[]") || strstr(name, "bool[]"))
    return false;
  return true;
}

static bool ny_native_nir_function_param_is_dyn_list(const program_t *prog,
                                                     const stmt_t *fn,
                                                     size_t param_index);

static bool ny_native_nir_expr_is_dyn_list(ny_native_nir_builder_t *b,
                                           const expr_t *e) {
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
        if (!ny_native_nir_expr_is_f64(b, e->as.list_like.data[i])) {
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
    const ny_native_nir_local_t *local =
        ny_native_nir_find_local(b, e->as.ident.name);
    if (local && local->is_list && !local->is_any)
      return false;
  }
  if (ny_native_nir_expr_is_list(b, e) && ny_native_nir_expr_is_any(b, e))
    return true;
  if (e->kind == NY_E_MEMCALL) {
    if (e->as.memcall.name && strcmp(e->as.memcall.name, "get") == 0 &&
        e->as.memcall.args.len == 2)
      return ny_native_nir_expr_is_dyn_list(b, e->as.memcall.args.data[1].val);
    const stmt_t *fn =
        e->as.memcall.name
            ? ny_native_nir_find_user_function((ny_native_nir_builder_t *)b,
                                               e->as.memcall.name)
            : NULL;
    return fn && ny_native_type_name_is_dyn_list(fn->as.fn.return_type);
  }
  if (e->kind == NY_E_CALL) {
    const char *leaf = ny_native_call_leaf(e);
    if (leaf && (strcmp(leaf, "list") == 0 || strcmp(leaf, "color") == 0 ||
                 strcmp(leaf, "get_terminal_size") == 0))
      return true;
    const stmt_t *fn = leaf ? ny_native_nir_find_user_function(
                                  (ny_native_nir_builder_t *)b, leaf)
                            : NULL;
    if (!fn && e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT)
      fn = ny_native_nir_find_user_function((ny_native_nir_builder_t *)b,
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
      if (!scalar || scalar->kind != NY_E_LITERAL)
        return true;
      if (ny_native_nir_expr_is_cstr(b, el) || ny_native_nir_expr_is_any(b, el))
        return true;
      if (ny_native_nir_expr_is_f64(b, el))
        has_f64 = true;
      else
        has_int = true;
    }
    return has_f64 && has_int;
  }
  if (e->kind == NY_E_BINARY && e->as.binary.op &&
      strcmp(e->as.binary.op, "*") == 0) {
    bool left_list = ny_native_nir_expr_is_list(b, e->as.binary.left);
    bool right_list = ny_native_nir_expr_is_list(b, e->as.binary.right);
    if (left_list != right_list)
      return ny_native_nir_expr_is_dyn_list(b, left_list ? e->as.binary.left
                                                         : e->as.binary.right);
  }
  if (e->kind == NY_E_IDENT) {
    ny_native_nir_local_t *l = ny_native_nir_find_local(b, e->as.ident.name);
    if (l)
      return l->is_dyn_list || (l->is_list && l->is_any);
    const expr_t *g = ny_native_nir_find_top_level_value(b, e->as.ident.name);
    if (g && g != e)
      return ny_native_nir_expr_is_dyn_list(b, g);
  }
  if (e->kind == NY_E_MEMBER) {
    const expr_t *v = ny_native_nir_resolve_member_expr(b, e);
    if (v && v != e)
      return ny_native_nir_expr_is_dyn_list(b, v);
  }
  return false;
}
static bool ny_native_nir_stmt_iterates_param(const stmt_t *s, const char *name,
                                              unsigned depth) {
  if (!s || !name || depth > 64)
    return false;
  if (s->kind == NY_S_FOR) {
    if (s->as.fr.iterable && s->as.fr.iterable->kind == NY_E_IDENT &&
        s->as.fr.iterable->as.ident.name &&
        strcmp(s->as.fr.iterable->as.ident.name, name) == 0)
      return true;
    return ny_native_nir_stmt_iterates_param(s->as.fr.body, name, depth + 1);
  }
  if (s->kind == NY_S_BLOCK || s->kind == NY_S_MODULE) {
    const ny_stmt_list *body =
        s->kind == NY_S_BLOCK ? &s->as.block.body : &s->as.module.body;
    for (size_t i = 0; i < body->len; ++i)
      if (ny_native_nir_stmt_iterates_param(body->data[i], name, depth + 1))
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
static bool ny_native_nir_expr_uses_list_surface(const expr_t *e,
                                                 const char *name,
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
    if (ny_native_nir_expr_uses_list_surface(e->as.memcall.target, name,
                                             depth + 1))
      return true;
    for (size_t i = 0; i < e->as.memcall.args.len; ++i)
      if (ny_native_nir_expr_uses_list_surface(e->as.memcall.args.data[i].val,
                                               name, depth + 1))
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
    if (ny_native_nir_expr_uses_list_surface(e->as.call.callee, name,
                                             depth + 1))
      return true;
    for (size_t i = 0; i < e->as.call.args.len; ++i)
      if (ny_native_nir_expr_uses_list_surface(e->as.call.args.data[i].val,
                                               name, depth + 1))
        return true;
    return false;
  case NY_E_BINARY:
    return ny_native_nir_expr_uses_list_surface(e->as.binary.left, name,
                                                depth + 1) ||
           ny_native_nir_expr_uses_list_surface(e->as.binary.right, name,
                                                depth + 1);
  case NY_E_UNARY:
    return ny_native_nir_expr_uses_list_surface(e->as.unary.right, name,
                                                depth + 1);
  case NY_E_INDEX:
    if (e->as.index.target && e->as.index.target->kind == NY_E_IDENT &&
        e->as.index.target->as.ident.name &&
        strcmp(e->as.index.target->as.ident.name, name) == 0)
      return true;
    return ny_native_nir_expr_uses_list_surface(e->as.index.target, name,
                                                depth + 1) ||
           ny_native_nir_expr_uses_list_surface(e->as.index.start, name,
                                                depth + 1) ||
           ny_native_nir_expr_uses_list_surface(e->as.index.stop, name,
                                                depth + 1) ||
           ny_native_nir_expr_uses_list_surface(e->as.index.step, name,
                                                depth + 1);
  case NY_E_MEMBER:
    return ny_native_nir_expr_uses_list_surface(e->as.member.target, name,
                                                depth + 1);
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
    for (size_t i = 0; i < e->as.list_like.len; ++i)
      if (ny_native_nir_expr_uses_list_surface(e->as.list_like.data[i], name,
                                               depth + 1))
        return true;
    return false;
  default:
    return false;
  }
}

static bool ny_native_nir_stmt_uses_list_surface(const stmt_t *s,
                                                 const char *name,
                                                 unsigned depth) {
  if (!s || !name || depth > 64)
    return false;
  switch (s->kind) {
  case NY_S_EXPR:
    return ny_native_nir_expr_uses_list_surface(s->as.expr.expr, name,
                                                depth + 1);
  case NY_S_RETURN:
    return ny_native_nir_expr_uses_list_surface(s->as.ret.value, name,
                                                depth + 1);
  case NY_S_VAR:
    for (size_t i = 0; i < s->as.var.exprs.len; ++i)
      if (ny_native_nir_expr_uses_list_surface(s->as.var.exprs.data[i], name,
                                               depth + 1))
        return true;
    return false;
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      if (ny_native_nir_stmt_uses_list_surface(s->as.block.body.data[i], name,
                                               depth + 1))
        return true;
    return false;
  default:
    return false;
  }
}

static bool ny_native_nir_param_is_inferred_list(const stmt_t *fn,
                                                 size_t index) {
  if (!fn || fn->kind != NY_S_FUNC || index >= fn->as.fn.params.len)
    return false;
  const char *name = fn->as.fn.params.data[index].name;
  return name && !fn->as.fn.params.data[index].type &&
         (ny_native_nir_stmt_iterates_param(fn->as.fn.body, name, 0) ||
          ny_native_nir_stmt_uses_list_surface(fn->as.fn.body, name, 0));
}

static bool ny_native_type_name_is_f64(const char *name) {
  return name && (strcmp(name, "f64") == 0 || strcmp(name, "float") == 0 ||
                  strcmp(name, "double") == 0);
}

static bool ny_native_type_name_is_f32(const char *name) {
  return name && (strcmp(name, "f32") == 0 || strcmp(name, "float32") == 0);
}

static bool ny_native_type_name_is_int(const char *name) {
  if (!name)
    return false;
  const char *leaf = strrchr(name, '.');
  leaf = leaf ? leaf + 1 : name;
  return strcmp(leaf, "int") == 0 || strcmp(leaf, "i64") == 0 ||
         strcmp(leaf, "i32") == 0 || strcmp(leaf, "i16") == 0 ||
         strcmp(leaf, "i8") == 0 || strcmp(leaf, "u64") == 0 ||
         strcmp(leaf, "u32") == 0 || strcmp(leaf, "u16") == 0 ||
         strcmp(leaf, "u8") == 0 || strcmp(leaf, "usize") == 0 ||
         strcmp(leaf, "isize") == 0 || strcmp(leaf, "integer") == 0;
}

static bool ny_native_type_name_is_list(const char *name) {
  if (!name)
    return false;
  const char *leaf = strrchr(name, '.');
  leaf = leaf ? leaf + 1 : name;
  return strncmp(leaf, "list", 4) == 0 &&
         (leaf[4] == '\0' || leaf[4] == '<' || leaf[4] == '[' ||
          isspace((unsigned char)leaf[4]));
}

static bool ny_native_type_name_is_str(const char *name) {
  return name && (strcmp(name, "str") == 0 || strcmp(name, "string") == 0);
}

static bool ny_native_type_name_is_bytes(const char *name) {
  if (!name)
    return false;
  const char *leaf = strrchr(name, '.');
  leaf = leaf ? leaf + 1 : name;
  return strcmp(leaf, "bytes") == 0;
}

static bool ny_native_type_name_is_any(const char *name) {
  if (!name)
    return false;
  return strcmp(name, "any") == 0 || strcmp(name, "sequence") == 0 ||
         strcmp(name, "seq") == 0 || strcmp(name, "iterable") == 0 ||
         strcmp(name, "indexable") == 0 || strcmp(name, "collection") == 0 ||
         strcmp(name, "container") == 0;
}

static bool ny_native_nir_set_param_types(ny_native_nir_builder_t *b,
                                          const stmt_t *fn) {
  if (!b || !fn || fn->kind != NY_S_FUNC)
    return false;
  size_t source_count = fn->as.fn.params.len;
  const ny_native_lambda_entry_t *lambda =
      ny_native_lambda_find_name(fn->as.fn.name);
  bool hidden_env = lambda && lambda->capture_count > 0;
  size_t count = source_count + (hidden_env ? 1 : 0);
  for (size_t i = 0; i < source_count; ++i) {
    const param_t *param = &fn->as.fn.params.data[i];
    const char *type = fn->as.fn.params.data[i].type;
    bool inferred_list = ny_native_nir_param_is_inferred_list(fn, i);
    ny_sem_rep_t rep =
        param->semantic.resolved ? param->semantic.rep : NY_SEM_REP_UNKNOWN;
    bool is_list = ny_native_type_name_is_list(type) || inferred_list ||
                   (!type && rep == NY_SEM_REP_TYPED_BUFFER);
    bool is_str =
        ny_native_type_name_is_str(type) || (!type && rep == NY_SEM_REP_STRING);
    bool is_any = ny_native_type_name_is_any(type) ||
                  (!type && (rep == NY_SEM_REP_TAGGED_DYNAMIC ||
                             rep == NY_SEM_REP_UNKNOWN));
    if (is_list)
      count++;
    else if (is_str || is_any)
      count += 2;
  }
  if (!count)
    return true;
  b->nyir.param_types = calloc(count, sizeof(*b->nyir.param_types));
  if (!b->nyir.param_types)
    return ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
  b->nyir.param_count = count;
  size_t abi_index = 0;
  if (hidden_env)
    b->nyir.param_types[abi_index++] = NYIR_PARAM_I64;
  for (size_t i = 0; i < source_count; ++i) {
    const param_t *param = &fn->as.fn.params.data[i];
    const char *type = fn->as.fn.params.data[i].type;
    bool inferred_list = ny_native_nir_param_is_inferred_list(fn, i);
    ny_sem_rep_t rep =
        param->semantic.resolved ? param->semantic.rep : NY_SEM_REP_UNKNOWN;
    bool is_list = ny_native_type_name_is_list(type) || inferred_list ||
                   (!type && rep == NY_SEM_REP_TYPED_BUFFER);
    bool is_str =
        ny_native_type_name_is_str(type) || (!type && rep == NY_SEM_REP_STRING);
    bool is_any = ny_native_type_name_is_any(type) ||
                  (!type && (rep == NY_SEM_REP_TAGGED_DYNAMIC ||
                             rep == NY_SEM_REP_UNKNOWN));
    b->nyir.param_types[abi_index++] =
        (ny_native_type_name_is_f64(type) || (!type && rep == NY_SEM_REP_F64))
            ? NYIR_PARAM_F64
        : (ny_native_type_name_is_f32(type) || (!type && rep == NY_SEM_REP_F32))
            ? NYIR_PARAM_F32
            : NYIR_PARAM_I64;
    if (is_list)
      b->nyir.param_types[abi_index++] = NYIR_PARAM_I64;
    else if (is_str || is_any) {
      b->nyir.param_types[abi_index++] = NYIR_PARAM_I64;
      b->nyir.param_types[abi_index++] = NYIR_PARAM_I64;
    }
  }
  return true;
}

static int64_t ny_native_f64_bits(double v) { return nyir_f64_to_bits(v); }

static int64_t ny_native_f32_bits(float v) {
  uint32_t bits = 0;
  memcpy(&bits, &v, sizeof(bits));
  return (int64_t)bits;
}

static ny_native_nir_local_t *
ny_native_nir_bind_local_typed(ny_native_nir_builder_t *b, const char *name,
                               bool is_f64, bool is_f32, bool is_cstr) {
  if (!name || !name[0] || strcmp(name, "_") == 0)
    return NULL;
  if (b->local_count == b->local_cap) {
    size_t cap = b->local_cap ? b->local_cap * 2 : 64;
    if (cap < b->local_cap || cap > SIZE_MAX / sizeof(*b->locals)) {
      ny_native_nir_fail(b, "native NYIR lower: local table is too large");
      return NULL;
    }
    ny_native_nir_local_t *locals = realloc(b->locals, cap * sizeof(*locals));
    if (!locals) {
      ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
      return NULL;
    }
    b->locals = locals;
    b->local_cap = cap;
  }
  ny_native_nir_local_t *l = &b->locals[b->local_count];
  *l = (ny_native_nir_local_t){
      .name = name,
      .slot = b->next_local_slot++,
      .semantic_rep = is_f64 ? NY_SEM_REP_F64
                             : (is_f32 ? NY_SEM_REP_F32
                                       : (is_cstr ? NY_SEM_REP_STRING
                                                  : NY_SEM_REP_UNKNOWN)),
      .is_f64 = is_f64,
      .is_f32 = is_f32,
      .is_cstr = is_cstr,
      .is_bytes = false,
      .callable_expr = NULL,
      .callable_name = NULL,
      .sb_slot = -1,
      .list_literal = NULL,
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

static ny_native_nir_local_t *
ny_native_nir_bind_local(ny_native_nir_builder_t *b, const char *name) {
  return ny_native_nir_bind_local_typed(b, name, false, false, false);
}

static ny_native_nir_local_t *
ny_native_nir_add_local(ny_native_nir_builder_t *b, const char *name) {
  if (!name || !name[0] || strcmp(name, "_") == 0)
    return NULL;
  ny_native_nir_local_t *old = ny_native_nir_find_local(b, name);
  return old ? old : ny_native_nir_bind_local(b, name);
}

/*
 * Compact NYIR emit helpers: one push path for control/value forms.
 */
static bool ny_native_nir_push_ctrl(ny_native_nir_builder_t *b, nyir_op_t op,
                                    int a, int64_t imm) {
  size_t before = b->nyir.len;
  nyir_emit(&b->nyir,
            (nyir_inst_t){.op = op, .dst = -1, .a = a, .b = -1, .imm = imm});
  return b->nyir.len != before || ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
}

static int ny_native_nir_push_val(ny_native_nir_builder_t *b, nyir_op_t op,
                                  int a, int b_arg, int64_t imm,
                                  const char *symbol) {
  int v = nyir_emit(&b->nyir, (nyir_inst_t){.op = op,
                                            .dst = -1,
                                            .a = a,
                                            .b = b_arg,
                                            .imm = imm,
                                            .symbol = symbol});
  if (v < 0)
    ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
  return v;
}

static bool ny_native_nir_emit_label(ny_native_nir_builder_t *b, int label) {
  return ny_native_nir_push_ctrl(b, NYIR_LABEL, -1, label);
}
static bool ny_native_nir_emit_br(ny_native_nir_builder_t *b, int label) {
  return ny_native_nir_push_ctrl(b, NYIR_BR, -1, label);
}
static int ny_native_nir_emit_const(ny_native_nir_builder_t *b, int64_t value);
static int ny_native_nir_load_local_value(ny_native_nir_builder_t *b, int slot);
static inline int ny_native_nir_emit_binop(ny_native_nir_builder_t *b,
                                           nyir_op_t op, int a, int rhs);
static inline int ny_native_nir_emit_cmp_i64(ny_native_nir_builder_t *b,
                                             nyir_cmp_t cmp, int a, int rhs) {
  int v = nyir_emit(
      &b->nyir,
      (nyir_inst_t){
          .op = NYIR_CMP_I64, .dst = -1, .a = a, .b = rhs, .cmp = cmp});
  if (v < 0)
    ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
  return v;
}

static int ny_native_nir_box_bool(ny_native_nir_builder_t *b, int reg) {
  if (!b || reg < 0)
    return -1;
  for (size_t i = b->nyir.len; i > 0; --i) {
    const nyir_inst_t *in = &b->nyir.data[i - 1];
    if (in->dst == reg) {
      if (in->op == NYIR_CONST_I64) {
        bool truthy = in->imm != 0 && in->imm != NY_IMM_FALSE;
        return ny_native_nir_emit_const(b, truthy ? NY_IMM_TRUE : NY_IMM_FALSE);
      }
      break;
    }
  }
  int zero = ny_native_nir_emit_const(b, 0);
  int false_imm = ny_native_nir_emit_const(b, NY_IMM_FALSE);
  int six = ny_native_nir_emit_const(b, 6);
  if (zero < 0 || false_imm < 0 || six < 0)
    return -1;
  int got = ny_native_nir_emit_cmp_i64(b, NYIR_CMP_NE, reg, zero);
  int not_false = ny_native_nir_emit_cmp_i64(b, NYIR_CMP_NE, reg, false_imm);
  int truthy = ny_native_nir_emit_binop(b, NYIR_AND_I64, got, not_false);
  int mul = ny_native_nir_emit_binop(b, NYIR_MUL_I64, truthy, six);
  return ny_native_nir_emit_binop(b, NYIR_ADD_I64, mul, false_imm);
}

static bool ny_native_nir_value_is_dynamic(const ny_native_nir_builder_t *b,
                                           int value) {
  if (!b || value < 0)
    return false;
  for (size_t i = b->nyir.len; i > 0; --i) {
    const nyir_inst_t *in = &b->nyir.data[i - 1];
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

static bool ny_native_nir_emit_br_if(ny_native_nir_builder_t *b, int value,
                                     int label) {
  /*
   * Runtime predicates (rt_is_nil, rt_has_tag, ...), dynamic locals, and
   * boxed bools return/hold the boxed boolean immediates NY_IMM_TRUE=8 /
   * NY_IMM_FALSE=2, whereas NYIR result values from comparisons are 0/1.
   * A raw != 0 test would treat the false boxed bool (2) as truthy.
   * Normalize dynamic/call/boxed conditions to a real 0/1 truthiness so
   * branch semantics agree for both encodings.
   */
  if (ny_native_nir_value_is_dynamic(b, value)) {
    int got = ny_native_nir_emit_cmp_i64(b, NYIR_CMP_NE, value,
                                         ny_native_nir_emit_const(b, 0));
    if (got < 0)
      return false;
    int not_false = ny_native_nir_emit_cmp_i64(
        b, NYIR_CMP_NE, value, ny_native_nir_emit_const(b, NY_IMM_FALSE));
    if (not_false < 0)
      return false;
    int truthy = ny_native_nir_emit_binop(b, NYIR_AND_I64, got, not_false);
    if (truthy < 0)
      return false;
    value = truthy;
  }
  return ny_native_nir_push_ctrl(b, NYIR_BR_IF, value, label);
}

static int ny_native_nir_emit_runtime_call(ny_native_nir_builder_t *b,
                                           const char *symbol, int a, int b_arg,
                                           int c, int argc, unsigned flags);

static bool ny_native_nir_emit_ret(ny_native_nir_builder_t *b, int value) {
  if (!b)
    return false;
  if (b && b->trace_instrumented &&
      ny_native_nir_emit_runtime_call(b, "rt_trace_exit", -1, -1, -1, 0, 0) < 0)
    return false;
  size_t before = b->nyir.len;
  nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_RET,
                                    .dst = -1,
                                    .a = value,
                                    .b = -1,
                                    .flags = b ? b->return_flags : 0});
  if (b->nyir.len == before)
    return ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
  b->emitted_return = true;
  b->last_value = value;
  return true;
}

static bool ny_native_nir_record_dyn_fact(ny_native_nir_builder_t *b, int value,
                                          ny_native_nir_fact_kind_t kind,
                                          int reg);
static bool ny_native_nir_record_list_len_fact(ny_native_nir_builder_t *b,
                                               int value, int length_value);
static int ny_native_nir_emit_add_i64(ny_native_nir_builder_t *b, int a,
                                      int rhs);
static bool ny_native_nir_emit_store_i64(ny_native_nir_builder_t *b, int addr,
                                         int value);
static int ny_native_nir_emit_runtime_call(ny_native_nir_builder_t *b,
                                           const char *symbol, int a, int b_arg,
                                           int c, int argc, unsigned flags);
static int ny_native_nir_emit_const(ny_native_nir_builder_t *b, int64_t value) {
  return ny_native_nir_push_val(b, NYIR_CONST_I64, -1, -1, value, NULL);
}
static int ny_native_nir_emit_cstr_const(ny_native_nir_builder_t *b,
                                         const char *s) {
  size_t len = s ? strlen(s) : 0;
  const char *sym = ny_native_strtab_intern(s ? s : "", len, NULL, 0);
  if (!sym) {
    ny_native_nir_fail(b, "native NYIR lower: string table full or OOM");
    return -1;
  }
  int addr = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_ADDR_SYMBOL,
                                               .dst = -1,
                                               .a = -1,
                                               .b = -1,
                                               .imm = 0,
                                               .symbol = sym});
  if (addr < 0) {
    ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
    return -1;
  }
  int length = ny_native_nir_emit_const(b, (int64_t)len);
  int tag = ny_native_nir_emit_const(b, TAG_STR_CONST);
  if (length >= 0 && tag >= 0) {
    ny_native_nir_record_dyn_fact(b, addr, NY_NATIVE_NIR_FACT_DYN_STR_LEN,
                                  length);
    ny_native_nir_record_dyn_fact(b, addr, NY_NATIVE_NIR_FACT_DYN_TAG, tag);
  }
  return addr;
}
static int ny_native_nir_emit_pair_list_i64(ny_native_nir_builder_t *b,
                                            int first, int second) {
  int count = ny_native_nir_emit_const(b, 2);
  int width = ny_native_nir_emit_const(b, 24);
  int base = count < 0 || width < 0
                 ? -1
                 : ny_native_nir_emit_runtime_call(b, "rt_tbuf_new_raw",
                                                   count, width, -1, 2, 0);
  int zero = ny_native_nir_emit_const(b, 0);
  int int_tag = ny_native_nir_emit_const(b, 3);
  int stride = ny_native_nir_emit_const(b, 24);
  if (base < 0 || first < 0 || second < 0 || zero < 0 || int_tag < 0 ||
      stride < 0)
    return -1;
  for (int i = 0; i < 2; ++i) {
    int slot = i == 0 ? base : ny_native_nir_emit_add_i64(b, base, stride);
    int len_slot = slot < 0 ? -1
                            : ny_native_nir_emit_add_i64(
                                  b, slot, ny_native_nir_emit_const(b, 8));
    int tag_slot = slot < 0 ? -1
                            : ny_native_nir_emit_add_i64(
                                  b, slot, ny_native_nir_emit_const(b, 16));
    if (slot < 0 || len_slot < 0 || tag_slot < 0 ||
        !ny_native_nir_emit_store_i64(b, slot, i == 0 ? first : second) ||
        !ny_native_nir_emit_store_i64(b, len_slot, zero) ||
        !ny_native_nir_emit_store_i64(b, tag_slot, int_tag))
      return -1;
  }
  int len = ny_native_nir_emit_const(b, 2);
  if (len < 0 || !ny_native_nir_record_list_len_fact(b, base, len))
    return -1;
  return base;
}
static int ny_native_nir_emit_const_f64(ny_native_nir_builder_t *b,
                                        double value) {
  return ny_native_nir_push_val(b, NYIR_CONST_F64, -1, -1,
                                ny_native_f64_bits(value), NULL);
}
static int ny_native_nir_emit_const_f32(ny_native_nir_builder_t *b,
                                        double value) {
  return ny_native_nir_push_val(b, NYIR_CONST_F32, -1, -1,
                                ny_native_f32_bits((float)value), NULL);
}
static int ny_native_nir_emit_i64_to_f64(ny_native_nir_builder_t *b,
                                         int value) {
  return ny_native_nir_push_val(b, NYIR_I64_TO_F64, value, -1, 0, NULL);
}
static int ny_native_nir_emit_f64_to_i64(ny_native_nir_builder_t *b,
                                         int value) {
  return ny_native_nir_emit_runtime_call(b, "rt_f64_to_i64", value, -1, -1, 1,
                                         0);
}
static __attribute__((unused)) int
ny_native_nir_emit_f32_to_i64(ny_native_nir_builder_t *b, int value) {
  int widened = ny_native_nir_push_val(b, NYIR_F32_TO_F64, value, -1, 0, NULL);
  if (widened < 0)
    return -1;
  return ny_native_nir_emit_f64_to_i64(b, widened);
}
static int ny_native_nir_emit_i64_to_f32(ny_native_nir_builder_t *b,
                                         int value) {
  return ny_native_nir_push_val(b, NYIR_I64_TO_F32, value, -1, 0, NULL);
}
static int ny_native_nir_emit_f32_to_f64(ny_native_nir_builder_t *b,
                                         int value) {
  return ny_native_nir_push_val(b, NYIR_F32_TO_F64, value, -1, 0, NULL);
}
static int ny_native_nir_emit_f64_to_f32(ny_native_nir_builder_t *b,
                                         int value) {
  return ny_native_nir_push_val(b, NYIR_F64_TO_F32, value, -1, 0, NULL);
}
static int ny_native_nir_emit_add_i64(ny_native_nir_builder_t *b, int a,
                                      int rhs) {
  return ny_native_nir_push_val(b, NYIR_ADD_I64, a, rhs, 0, NULL);
}
static int ny_native_nir_emit_load_i64(ny_native_nir_builder_t *b, int addr) {
  return ny_native_nir_push_val(b, NYIR_LOAD_I64, addr, -1, 0, NULL);
}
static int64_t ny_native_nir_static_dyn_tag(const expr_t *e) {
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
static void ny_native_nir_record_global_dyn_tag(ny_native_nir_builder_t *b,
                                                const char *name, int value) {
  if (!b || !name || value < 0)
    return;
  const expr_t *init = ny_native_nir_find_top_level_value(b, name);
  int64_t raw = ny_native_nir_static_dyn_tag(init);
  if (raw < 0)
    return;
  int tag = ny_native_nir_emit_const(b, raw);
  if (tag >= 0)
    (void)ny_native_nir_record_dyn_fact(b, value, NY_NATIVE_NIR_FACT_DYN_TAG,
                                        tag);
}
static int ny_native_nir_emit_load8(ny_native_nir_builder_t *b, int addr) {
  int value = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_LOAD_I64,
                                                .dst = -1,
                                                .a = addr,
                                                .b = -1,
                                                .flags = NYIR_INST_F_MEM_BYTE});
  if (value < 0)
    ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
  return value;
}

static int ny_native_nir_emit_load_f64(ny_native_nir_builder_t *b, int addr) {
  int value = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_LOAD_I64,
                                                .dst = -1,
                                                .a = addr,
                                                .b = -1,
                                                .flags = NYIR_INST_F_MEM_F64});
  if (value < 0)
    ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
  return value;
}
static int ny_native_nir_emit_addr_local(ny_native_nir_builder_t *b, int slot,
                                         const char *symbol) {
  return ny_native_nir_push_val(b, NYIR_ADDR_LOCAL, -1, -1, slot, symbol);
}
static bool ny_native_nir_emit_store_i64(ny_native_nir_builder_t *b, int addr,
                                         int value) {
  size_t before = b->nyir.len;
  nyir_emit(
      &b->nyir,
      (nyir_inst_t){
          .op = NYIR_STORE_I64, .dst = -1, .a = addr, .b = -1, .c = value});
  return b->nyir.len != before || ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
}
static bool ny_native_nir_emit_store8(ny_native_nir_builder_t *b, int addr,
                                      int value) {
  size_t before = b->nyir.len;
  nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_STORE_I64,
                                    .dst = -1,
                                    .a = addr,
                                    .b = -1,
                                    .c = value,
                                    .flags = NYIR_INST_F_MEM_BYTE});
  return b->nyir.len != before || ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
}

static bool ny_native_nir_emit_store_f64(ny_native_nir_builder_t *b, int addr,
                                         int value) {
  size_t before = b->nyir.len;
  nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_STORE_I64,
                                    .dst = -1,
                                    .a = addr,
                                    .b = -1,
                                    .c = value,
                                    .flags = NYIR_INST_F_MEM_F64});
  return b->nyir.len != before || ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
}

/*
 * Emit a bounds check: verifies (base + offset) is within [base,
 * base+byte_len). Elided at lowering time for Fin-typed indices.
 */
static bool ny_native_nir_emit_bounds_check_value(ny_native_nir_builder_t *b,
                                                  int base, int offset,
                                                  int byte_len_value,
                                                  int64_t byte_len) {
  /*
   * Raw pointers without retained provenance have neither a dynamic nor a
   * static capacity.  A zero static bound is the sentinel for that case.
   */
  if (byte_len_value < 0 && byte_len <= 0)
    return true;
  size_t before = b->nyir.len;
  nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_BOUNDS_CHECK,
                                    .dst = -1,
                                    .a = base,
                                    .b = offset,
                                    .c = byte_len_value,
                                    .imm = byte_len});
  return b->nyir.len != before || ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
}

static bool ny_native_nir_emit_bounds_check(ny_native_nir_builder_t *b,
                                            int base, int offset,
                                            int64_t byte_len) {
  return ny_native_nir_emit_bounds_check_value(b, base, offset, -1, byte_len);
}

static int ny_native_nir_emit_runtime_call(ny_native_nir_builder_t *b,
                                           const char *symbol, int a, int b_arg,
                                           int c, int argc, unsigned flags) {
  const char *canonical = ny_native_runtime_symbol(symbol);
  if (canonical)
    symbol = canonical;
  int value =
      nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_CALL,
                                        .dst = -1,
                                        .a = a,
                                        .b = b_arg,
                                        .c = c,
                                        .imm = argc,
                                        .flags = NYIR_INST_F_EXTERN | flags,
                                        .symbol = symbol});
  if (value < 0)
    ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
  return value;
}

/*
 * Forward declaration — defined below with module/block recursion.
 */
static const stmt_t *
ny_native_nir_find_user_function(ny_native_nir_builder_t *b, const char *name);
static const stmt_t *
ny_native_nir_find_user_function_exact(const stmt_t *s, const char *name,
                                       const ny_options *opt, unsigned depth);
static const stmt_t *
ny_native_nir_find_extern_decl_in_stmt(const stmt_t *s, const char *name,
                                       const ny_options *opt, unsigned depth);
bool ny_native_target_eval_bool(const ny_options *opt, const expr_t *e,
                                bool *out);
/*
 * Defined below with the top-level value helpers; used by expr_is_f64 so a
 * member access (gfx.WHITE) or a const list literal types as f64 for the
 * native index/load lowering.
 */
static const expr_t *
ny_native_nir_find_top_level_value(const ny_native_nir_builder_t *b,
                                   const char *name);
static const expr_t *
ny_native_nir_find_top_level_value_in_source(const ny_native_nir_builder_t *b,
                                             const char *name,
                                             const char *source_file);
static const expr_t *
ny_native_nir_resolve_member_expr(const ny_native_nir_builder_t *b,
                                  const expr_t *e);
static const expr_t *
ny_native_nir_resolve_list_literal(const ny_native_nir_builder_t *b,
                                   const expr_t *e, unsigned depth);
static bool ny_native_nir_expr_is_f32(ny_native_nir_builder_t *b,
                                      const expr_t *e);
static const stmt_t *
ny_native_nir_find_imported_function(const ny_native_nir_builder_t *b,
                                     const char *name);

static bool ny_native_nir_expr_is_f64(ny_native_nir_builder_t *b,
                                      const expr_t *e) {
  if (!e)
    return false;
  if (e->semantic.resolved && e->semantic.rep == NY_SEM_REP_F64)
    return true;
  /*
   * A C math declaration may carry a conservative dynamic semantic
   * representation.  Its native leaf is nevertheless an exact raw-f64
   * result, and this must be decided before consulting that stale annotation
   * so the value remains f64 when stored in a local.
   */
  if (e->kind == NY_E_CALL) {
    const char *leaf = ny_native_call_leaf(e);
    if (leaf && (strcmp(leaf, "__flt_sin") == 0 ||
                 strcmp(leaf, "__flt_cos") == 0 || strcmp(leaf, "sin") == 0 ||
                 strcmp(leaf, "cos") == 0 || strcmp(leaf, "sqrt") == 0))
      return true;
  }
  /*
   * A bound parameter/local is authoritative even when the semantic pass
   * attached a stale generic float representation to its identifier node.
   */
  if (e->kind == NY_E_IDENT) {
    ny_native_nir_local_t *l = ny_native_nir_find_local(b, e->as.ident.name);
    if (l)
      return l->semantic_rep == NY_SEM_REP_F64 || l->is_f64;
    const expr_t *g =
        e->tok.filename
            ? ny_native_nir_find_top_level_value_in_source(b, e->as.ident.name,
                                                           e->tok.filename)
            : ny_native_nir_find_top_level_value(b, e->as.ident.name);
    if (!g)
      g = ny_native_nir_find_top_level_value(b, e->as.ident.name);
    if (g && g != e && g->kind == NY_E_CALL) {
      const char *leaf = ny_native_call_leaf(g);
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
    if (e->as.binary.op && strcmp(e->as.binary.op, "^") == 0) {
      int64_t folded = 0;
      return !ny_native_nir_fold_const_pow(e->as.binary.left,
                                           e->as.binary.right, &folded);
    }
    bool f32 = ny_native_nir_expr_is_f32(b, e->as.binary.left) ||
               ny_native_nir_expr_is_f32(b, e->as.binary.right);
    bool f64 = ny_native_nir_expr_is_f64(b, e->as.binary.left) ||
               ny_native_nir_expr_is_f64(b, e->as.binary.right);
    if (f32 && !f64)
      return false;
  }
  if (e->kind == NY_E_LITERAL && e->as.literal.kind == NY_LIT_FLOAT)
    return true;
  if (e->semantic.resolved && e->semantic.rep == NY_SEM_REP_F64)
    return true;
  switch (e->kind) {
  case NY_E_IDENT: {
    ny_native_nir_local_t *l = ny_native_nir_find_local(b, e->as.ident.name);
    if (l)
      return l->semantic_rep == NY_SEM_REP_F64 || l->is_f64;
    const expr_t *g = ny_native_nir_find_top_level_value(b, e->as.ident.name);
    return g && g != e && g->kind != NY_E_IDENT &&
           ny_native_nir_expr_is_f64(b, g);
  }
  case NY_E_BINARY: {
    nyir_cmp_t ignored;
    if (ny_native_nir_cmp(e->as.binary.op, &ignored))
      return false;
    if (e->as.binary.op && strcmp(e->as.binary.op, "*") == 0) {
      if (ny_native_nir_expr_is_dict(b, e->as.binary.left) &&
          ny_native_nir_expr_is_dict(b, e->as.binary.right))
        return true;
    }
    return ny_native_nir_expr_is_f64(b, e->as.binary.left) ||
           ny_native_nir_expr_is_f64(b, e->as.binary.right);
  }
  case NY_E_UNARY:
    return ny_native_nir_expr_is_f64(b, e->as.unary.right);
  case NY_E_CALL: {
    const char *leaf = ny_native_call_leaf(e);
    if (leaf && strcmp(leaf, "get") == 0 && e->as.call.args.len >= 3) {
      if (ny_native_nir_expr_is_f64(b, e->as.call.args.data[2].val))
        return true;
    }
    ny_native_leaf_kind_t kind = ny_native_leaf_kind(leaf);
    if (kind == NY_NATIVE_LEAF_FLOAT || kind == NY_NATIVE_LEAF_F64BUF_LOAD ||
        kind == NY_NATIVE_LEAF_FLT_SQRT ||
        (leaf &&
         (strcmp(leaf, "__flt_unbox_val") == 0 || strcmp(leaf, "atof") == 0 ||
          strcmp(leaf, "strtod") == 0 || strcmp(leaf, "sin") == 0 ||
          strcmp(leaf, "cos") == 0 || strcmp(leaf, "sqrt") == 0)))
      return true;
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
        if (ny_native_nir_expr_is_f64(b, e->as.call.args.data[i].val))
          return true;
    }
    if (e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT) {
      const stmt_t *fn =
          ny_native_nir_find_user_function(b, e->as.call.callee->as.ident.name);
      if (!fn)
        fn = ny_native_nir_find_imported_function(
            b, e->as.call.callee->as.ident.name);
      if (fn)
        return (fn->as.fn.return_semantic.resolved &&
                fn->as.fn.return_semantic.rep == NY_SEM_REP_F64) ||
               ny_native_type_name_is_f64(fn->as.fn.return_type);
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
        if (ny_native_nir_expr_is_f64(b, e->as.memcall.args.data[i].val))
          return true;
    }
    if (e->as.memcall.name && strcmp(e->as.memcall.name, "get") == 0 &&
        e->as.memcall.args.len >= 2) {
      if (ny_native_nir_expr_is_f64(b, e->as.memcall.args.data[1].val))
        return true;
    }
    return false;
  case NY_E_LIST:
    if (e->as.list_like.len == 0)
      return false;
    for (size_t i = 0; i < e->as.list_like.len; ++i)
      if (!ny_native_nir_expr_is_f64(b, e->as.list_like.data[i]))
        return false;
    return true;
  case NY_E_INDEX:
    return ny_native_nir_expr_is_f64(b, e->as.index.target);
  case NY_E_MEMBER: {
    const expr_t *v = ny_native_nir_resolve_member_expr(b, e);
    if (v && v != e)
      return ny_native_nir_expr_is_f64(b, v);
    return false;
  }
  default:
    return false;
  }
}

static bool ny_native_nir_expr_is_f32(ny_native_nir_builder_t *b,
                                      const expr_t *e) {
  if (!e)
    return false;
  /*
   * Keep explicit local/parameter types ahead of stale semantic annotations.
   */
  if (e->kind == NY_E_IDENT) {
    ny_native_nir_local_t *l = ny_native_nir_find_local(b, e->as.ident.name);
    if (l)
      return l->semantic_rep == NY_SEM_REP_F32 || l->is_f32;
  }
  if (e->kind == NY_E_BINARY) {
    bool f32 = ny_native_nir_expr_is_f32(b, e->as.binary.left) ||
               ny_native_nir_expr_is_f32(b, e->as.binary.right);
    bool f64 = ny_native_nir_expr_is_f64(b, e->as.binary.left) ||
               ny_native_nir_expr_is_f64(b, e->as.binary.right);
    if (f32 && !f64)
      return true;
  }
  if (e->semantic.resolved)
    return e->semantic.rep == NY_SEM_REP_F32;
  switch (e->kind) {
  case NY_E_IDENT: {
    ny_native_nir_local_t *l = ny_native_nir_find_local(b, e->as.ident.name);
    if (l)
      return l->semantic_rep == NY_SEM_REP_F32 || l->is_f32;
    const expr_t *g = ny_native_nir_find_top_level_value(b, e->as.ident.name);
    return g && g != e && g->kind != NY_E_IDENT &&
           ny_native_nir_expr_is_f32(b, g);
  }
  case NY_E_BINARY: {
    nyir_cmp_t ignored;
    if (ny_native_nir_cmp(e->as.binary.op, &ignored))
      return false;
    return ny_native_nir_expr_is_f32(b, e->as.binary.left) ||
           ny_native_nir_expr_is_f32(b, e->as.binary.right);
  }
  case NY_E_UNARY:
    return ny_native_nir_expr_is_f32(b, e->as.unary.right);
  case NY_E_CALL:
    if (e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT) {
      const stmt_t *fn =
          ny_native_nir_find_user_function(b, e->as.call.callee->as.ident.name);
      if (!fn)
        fn = ny_native_nir_find_imported_function(
            b, e->as.call.callee->as.ident.name);
      if (fn)
        return (fn->as.fn.return_semantic.resolved &&
                fn->as.fn.return_semantic.rep == NY_SEM_REP_F32) ||
               ny_native_type_name_is_f32(fn->as.fn.return_type);
    }
    return false;
  default:
    return false;
  }
}

/*
 * Bigints are represented by runtime handles, not by the raw i64 arithmetic
 * domain.  Keep unary negation in that domain when the source expression is
 * statically known to produce a bigint (large literals, aliases, or bigint
 * returning calls).
 */
static bool ny_native_nir_expr_is_bigint(ny_native_nir_builder_t *b,
                                         const expr_t *e) {
  if (!b || !e)
    return false;
  if (e->kind == NY_E_LITERAL && e->as.literal.kind == NY_LIT_INT &&
      e->tok.kind != NY_T_NIL)
    return e->as.literal.as.i >= (INT64_C(1) << 62) ||
           e->as.literal.as.i <= -(INT64_C(1) << 62);
  if (e->kind == NY_E_IDENT && e->as.ident.name) {
    ny_native_nir_local_t *l = ny_native_nir_find_local(b, e->as.ident.name);
    if (l)
      return l->is_bigint;
    const expr_t *value =
        e->tok.filename
            ? ny_native_nir_find_top_level_value_in_source(b, e->as.ident.name,
                                                           e->tok.filename)
            : ny_native_nir_find_top_level_value(b, e->as.ident.name);
    if (!value)
      value = ny_native_nir_find_top_level_value(b, e->as.ident.name);
    return value && value != e && value->kind != NY_E_IDENT &&
           ny_native_nir_expr_is_bigint(b, value);
  }
  if (e->kind == NY_E_UNARY && e->as.unary.right) {
    return ny_native_nir_expr_is_bigint(b, e->as.unary.right);
  }
  if (e->kind == NY_E_BINARY) {
    if (e->as.binary.op &&
        (strcmp(e->as.binary.op, "+") == 0 ||
         strcmp(e->as.binary.op, "-") == 0 ||
         strcmp(e->as.binary.op, "*") == 0 ||
         strcmp(e->as.binary.op, "/") == 0 ||
         strcmp(e->as.binary.op, "%") == 0 ||
         strcmp(e->as.binary.op, "^") == 0 ||
         strcmp(e->as.binary.op, "^^") == 0) &&
        (ny_native_nir_expr_is_bigint(b, e->as.binary.left) ||
         ny_native_nir_expr_is_bigint(b, e->as.binary.right)))
      return true;
    int64_t value = 0;
    /*
     * A folded arithmetic result can cross the tagged-small-int boundary even
     * when neither source operand is large (e.g. MAX_SMALL + 1).
     */
    if (ny_native_nir_fold_top_level_int(b->prog, e, &value, 0))
      return value >= (INT64_C(1) << 62) || value <= -(INT64_C(1) << 62);
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
    const stmt_t *fn = ny_native_nir_find_user_function(b, name);
    if (!fn)
      fn = ny_native_nir_find_imported_function(b, name);
    return fn && fn->kind == NY_S_FUNC && fn->as.fn.return_type &&
           strcmp(fn->as.fn.return_type, "bigint") == 0;
  }
  if (e->kind == NY_E_CALL && e->as.call.callee &&
      e->as.call.callee->kind == NY_E_MEMBER &&
      e->as.call.callee->as.member.name) {
    const char *mname = e->as.call.callee->as.member.name;
    if (strcmp(mname, "long") == 0 || strcmp(mname, "__long") == 0)
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
static bool ny_native_nir_expr_is_bigfloat(ny_native_nir_builder_t *b,
                                           const expr_t *e) {
  if (!b || !e)
    return false;
  if (e->kind == NY_E_IDENT && e->as.ident.name) {
    ny_native_nir_local_t *local = ny_native_nir_find_local(b, e->as.ident.name);
    if (local && local->type_name && strcmp(local->type_name, "bigfloat") == 0)
      return true;
    const expr_t *value = ny_native_nir_find_top_level_value(b, e->as.ident.name);
    return value && value != e && ny_native_nir_expr_is_bigfloat(b, value);
  }
  if (e->kind == NY_E_CALL && e->as.call.callee &&
      e->as.call.callee->kind == NY_E_IDENT) {
    const char *name = e->as.call.callee->as.ident.name;
    if (name && (strstr(name, "bigfloat") || strncmp(name, "bf_", 3) == 0))
      return strcmp(name, "bf_to_float") != 0 &&
             strcmp(name, "bf_to_str") != 0 &&
             strcmp(name, "bf_precision") != 0;
    const stmt_t *fn = name ? ny_native_nir_find_user_function(b, name) : NULL;
    return fn && fn->kind == NY_S_FUNC && fn->as.fn.return_type &&
           strcmp(fn->as.fn.return_type, "bigfloat") == 0;
  }
  return false;
}

static bool ny_native_nir_expr_is_cstr(ny_native_nir_builder_t *b,
                                       const expr_t *e) {
  if (!e)
    return false;
  if (ny_native_nir_expr_is_bytes((const ny_native_nir_builder_t *)b, e))
    return false;
  /*
   * Literal strings are C-string values regardless of a conservative
   * unresolved semantic annotation attached by the front end.
   */
  if (e->kind == NY_E_LITERAL)
    return e->as.literal.kind == NY_LIT_STR;
  if (e->kind == NY_E_INDEX)
    return e->as.index.target &&
           !ny_native_nir_expr_is_bytes(b, e->as.index.target) &&
           ny_native_nir_expr_is_cstr(b, e->as.index.target);
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
   * `list.get(index, default)` has the type of its default at this dynamic
   * boundary.  Semantic resolution often records the generic list result as
   * `any`, which used to return early below and route a raw string pointer to
   * integer formatting.  Keep this deliberately limited to list receivers:
   * dictionary get has different key/default semantics.
   */
  if (e->kind == NY_E_MEMCALL && e->as.memcall.name &&
      strcmp(e->as.memcall.name, "get") == 0 && e->as.memcall.args.len == 2 &&
      ny_native_nir_expr_is_list(b, e->as.memcall.target))
    return ny_native_nir_expr_is_cstr(b, e->as.memcall.args.data[1].val);
  if (e->semantic.resolved)
    return e->semantic.rep == NY_SEM_REP_STRING;
  switch (e->kind) {
  case NY_E_LITERAL:
    return e->as.literal.kind == NY_LIT_STR;
  case NY_E_FSTRING:
    return true; /* lowered to a runtime C string */
  case NY_E_IDENT: {
    ny_native_nir_local_t *l = ny_native_nir_find_local(b, e->as.ident.name);
    if (l) {
      if (l->is_bytes)
        return false;
      return l->semantic_rep == NY_SEM_REP_STRING || l->is_cstr;
    }
    /*
     * Module-level global: classify by its initializer.  Do not chase
     * ident->ident global chains — that keeps this bounded and cycle-free.
     */
    const expr_t *g = ny_native_nir_find_top_level_value(b, e->as.ident.name);
    if (g && g != e && g->kind != NY_E_IDENT)
      return ny_native_nir_expr_is_cstr(b, g);
    return false;
  }
  case NY_E_BINARY:
    return e->as.binary.op && strcmp(e->as.binary.op, "+") == 0 &&
           (ny_native_nir_expr_is_cstr(b, e->as.binary.left) ||
            ny_native_nir_expr_is_cstr(b, e->as.binary.right));
  case NY_E_TERNARY:
    /*
     * A ternary yields a string only when both arms do.
     */
    return ny_native_nir_expr_is_cstr(b, e->as.ternary.true_expr) &&
           ny_native_nir_expr_is_cstr(b, e->as.ternary.false_expr);
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
    if (leaf && strcmp(leaf, "type") == 0)
      return true;
    const stmt_t *fn = leaf ? ny_native_nir_find_user_function(b, leaf) : NULL;
    return fn && fn->as.fn.return_type &&
           strcmp(fn->as.fn.return_type, "str") == 0;
  }
  case NY_E_MEMCALL: {
    const stmt_t *fn =
        e->as.memcall.name
            ? ny_native_nir_find_user_function(b, e->as.memcall.name)
            : NULL;
    return fn && fn->as.fn.return_type &&
           strcmp(fn->as.fn.return_type, "str") == 0;
  }
  default:
    return false;
  }
}

/*
 * True when the expression produces a boolean value (a comparison, logical
 * operation, boolean literal, or boolean unary).  These lower to a raw 0/1
 * i64 in NYIR, so string formatting must route them through
 * rt_bool_to_cstr rather than the i64 formatter, which would print
 * the raw 0/1 bits.
 */
static bool ny_native_nir_expr_is_bool(ny_native_nir_builder_t *b,
                                       const expr_t *e) {
  if (!e)
    return false;
  switch (e->kind) {
  case NY_E_LITERAL:
    return e->as.literal.kind == NY_LIT_BOOL;
  case NY_E_BINARY:
    return ny_native_nir_cmp(e->as.binary.op, &(nyir_cmp_t){0});
  case NY_E_LOGICAL:
    return true;
  case NY_E_UNARY:
    return e->as.unary.op && strcmp(e->as.unary.op, "!") == 0;
  case NY_E_TERNARY:
    return ny_native_nir_expr_is_bool(b, e->as.ternary.true_expr) &&
           ny_native_nir_expr_is_bool(b, e->as.ternary.false_expr);
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
    if (e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT &&
        e->as.call.callee->as.ident.name) {
      const stmt_t *fn =
          ny_native_nir_find_user_function(b, e->as.call.callee->as.ident.name);
      return fn && fn->as.fn.return_type &&
             strcmp(fn->as.fn.return_type, "bool") == 0;
    }
    return false;
  }
  case NY_E_IDENT: {
    /*
     * A binding initialized from a boolean expression keeps the raw 0/1
     * value, so follow its initializer for classification.  Do not chase
     * ident->ident chains (bounded, cycle-free).
     */
    ny_native_nir_local_t *l = ny_native_nir_find_local(b, e->as.ident.name);
    if (l)
      return l->is_bool;
    const expr_t *g = ny_native_nir_find_top_level_value(b, e->as.ident.name);
    if (g && g != e && g->kind != NY_E_IDENT)
      return ny_native_nir_expr_is_bool(b, g);
    return false;
  }
  default:
    return false;
  }
}

static const stmt_t *
ny_native_nir_find_imported_function(const ny_native_nir_builder_t *b,
                                     const char *name);
static const stmt_t *
ny_native_nir_find_extern_decl_in_stmt(const stmt_t *s, const char *name,
                                       const ny_options *opt, unsigned depth);

/*
 * True when the expression is any-typed: the value is tagged and requires
 * runtime type dispatch, so it cannot be treated as a raw i64 or pointer.
 */
static bool ny_native_nir_expr_is_any(ny_native_nir_builder_t *b,
                                      const expr_t *e) {
  if (!e)
    return false;
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
   * Prefer the resolved ABI fact; retain the intrinsic spelling only when
   * semantic resolution has no declaration to consult.
   */
  if ((e->kind == NY_E_CALL || e->kind == NY_E_MEMCALL) &&
      e->semantic.canonical_callee_stmt &&
      e->semantic.canonical_callee_stmt->kind == NY_S_FUNC) {
    const stmt_t *fn = e->semantic.canonical_callee_stmt;
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
      const char *leaf = ny_native_leaf_name(fn->as.fn.name);
      if (leaf && strcmp(leaf, "load64") == 0)
        return false;
    }
  } else if (e->kind == NY_E_CALL && e->as.call.callee &&
             e->as.call.callee->kind == NY_E_IDENT &&
             e->as.call.callee->as.ident.name &&
             strcmp(e->as.call.callee->as.ident.name,
                    "__list_sum_int_range") == 0) {
    return false;
  }
  /*
   * Module values lower through their initializer, including re-exports.
   * Use the same representation here: a conservative dynamic annotation on
   * the member must not turn its raw integer constant into a tagged value.
   */
  if (e->kind == NY_E_MEMBER && e->as.member.target &&
      e->as.member.target->kind == NY_E_IDENT &&
      !ny_native_nir_find_local(b, e->as.member.target->as.ident.name)) {
    const expr_t *value = ny_native_nir_resolve_member_expr(b, e);
    if (value && value != e)
      return ny_native_nir_expr_is_any(b, value);
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
        (ny_native_nir_expr_is_bytes(b, receiver) ||
         ny_native_nir_expr_is_range(b, receiver)))
      return false;
    const stmt_t *fn = ny_native_nir_find_attached_method(b, receiver, method);
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
  }
  /*
   * A resolved floating representation is a raw IEEE value in NYIR.  Do not
   * let a conservative dynamic annotation override it: treating an f64 as
   * `any` routes it through integer unboxing before a later comparison.
   */
  if (ny_native_nir_expr_is_f64(b, e) || ny_native_nir_expr_is_f32(b, e))
    return false;
  /*
   * Dictionary indexing returns a dynamic slot value.  Keep the tag through
   * callers such as print/equality; treating it as a raw scalar causes a
   * second tag operation at the consumer boundary.
   */
  if (e->kind == NY_E_INDEX && e->as.index.target &&
      ny_native_nir_expr_is_dict(b, e->as.index.target))
    return true;
  if (e->kind == NY_E_INDEX && e->as.index.target &&
      !ny_native_nir_expr_is_any(b, e->as.index.target) &&
      (ny_native_nir_expr_is_list(b, e->as.index.target) ||
       ny_native_nir_expr_is_range(b, e->as.index.target) ||
       ny_native_nir_expr_is_bytes(b, e->as.index.target)))
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
      ny_native_nir_expr_is_any(b, e->as.index.target))
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
     * The canonical sequence accessor deliberately yields the dynamic
     * value/len/tag word.  Its untyped builtin declaration otherwise lets a
     * conservative RAW_INT semantic win, seeding every def-bound result with
     * a zero length slot (builder_append then measured slen=0 and
     * _char_list_to_str returned empty strings).
     */
    {
      const char *canon_sym = ny_native_runtime_symbol_for_expr(cname, NULL, e);
      if (canon_sym && strcmp(canon_sym, "rt_tbuf_index_any_raw") == 0)
        return true;
    }
    /*
     * Runtime bridges with a raw-machine-word result (lengths, counts,
     * opaque handles) are not tagged dynamics.  Their untyped declarations
     * otherwise classify every call as `any`, and the next dynamic consumer
     * halves an odd raw word (rt_cstr_len("hello world") = 11 printed as 5).
     */
    {
      const char *sym = ny_native_runtime_symbol_for_expr(cname, NULL, e);
      if (sym &&
          (strcmp(sym, "rt_cstr_len") == 0 || strcmp(sym, "rt_len") == 0 ||
           strcmp(sym, "rt_dict_len_raw") == 0 ||
           strcmp(sym, "rt_tbuf_len_raw") == 0 ||
           strcmp(sym, "rt_str_builder_new") == 0 ||
           strcmp(sym, "rt_str_builder_append") == 0 ||
           strcmp(sym, "rt_str_builder_free") == 0 ||
           strcmp(sym, "rt_cstr_builder_new") == 0 ||
           strcmp(sym, "rt_bytes_len_raw") == 0))
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
            ? ny_native_leaf_name(e->semantic.canonical_callee)
            : ny_native_call_leaf(e);
    if (store_leaf &&
        (strcmp(store_leaf, "__store_item") == 0 ||
         strcmp(store_leaf, "__store_item_fast") == 0))
      return false;
    const stmt_t *fn = ny_native_nir_find_user_function(b, cname);
    if (!fn)
      fn = ny_native_nir_find_imported_function(b, cname);
    if (fn && fn->as.fn.return_type) {
      /*
       * Same raw-load bridge as the canonical-callee check above: the
       * stdlib `any` annotation must not classify the native machine-word
       * result of load64 as a tagged dynamic.
       */
      if (strcmp(fn->as.fn.return_type, "any") == 0) {
        const char *leaf = ny_native_leaf_name(fn->as.fn.name);
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
    ny_native_nir_local_t *local = ny_native_nir_find_local(b, cname);
    if (local)
      return true;
    /*
     * Extern signatures are already indexed before lowering starts.  Avoid a
     * repeated recursive AST scan here; this predicate is queried for nearly
     * every call argument in large stdlib-expanded programs.
     */
  }
  if (e->kind == NY_E_IDENT) {
    ny_native_nir_local_t *local =
        ny_native_nir_find_local(b, e->as.ident.name);
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
    const expr_t *init = e->tok.filename
                             ? ny_native_nir_find_top_level_value_in_source(
                                   b, e->as.ident.name, e->tok.filename)
                             : NULL;
    if (!init)
      init = ny_native_nir_find_top_level_value(b, e->as.ident.name);
    if (init && init->kind == NY_E_LITERAL &&
        init->as.literal.kind == NY_LIT_INT && init->tok.kind != NY_T_NIL)
      return false;
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
      const stmt_t *gfn = ny_native_nir_find_user_function(
          b, init->as.call.callee->as.ident.name);
      if (!gfn)
        gfn = ny_native_nir_find_imported_function(
            b, init->as.call.callee->as.ident.name);
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
      (ny_native_nir_expr_is_list(b, e->as.call.callee->as.member.target) ||
       ny_native_nir_expr_is_dyn_list(b, e->as.call.callee->as.member.target) ||
       ny_native_nir_expr_is_any(b, e->as.call.callee->as.member.target) ||
       ny_native_nir_expr_is_bytes(b, e->as.call.callee->as.member.target) ||
       ny_native_nir_expr_is_range(b, e->as.call.callee->as.member.target)))
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
      (ny_native_nir_expr_is_bytes(b, e->as.memcall.target) ||
       ny_native_nir_expr_is_range(b, e->as.memcall.target)))
    return false;
  if (e->kind == NY_E_MEMCALL && e->as.memcall.name &&
      (strcmp(e->as.memcall.name, "get") == 0 ||
       strcmp(e->as.memcall.name, "pop") == 0) &&
      e->as.memcall.target &&
      (ny_native_nir_expr_is_list(b, e->as.memcall.target) ||
       ny_native_nir_expr_is_dyn_list(b, e->as.memcall.target) ||
       ny_native_nir_expr_is_dict(b, e->as.memcall.target) ||
       ny_native_nir_expr_is_any(b, e->as.memcall.target)))
    return true;
  if (e->kind == NY_E_BINARY &&
      !ny_native_nir_expr_is_any(b, e->as.binary.left) &&
      !ny_native_nir_expr_is_any(b, e->as.binary.right))
    return false;
  if (e->semantic.resolved)
    return e->semantic.rep == NY_SEM_REP_TAGGED_DYNAMIC;
  switch (e->kind) {
  case NY_E_IDENT: {
    ny_native_nir_local_t *l = ny_native_nir_find_local(b, e->as.ident.name);
    if (l) {
      if (l->semantic_rep == NY_SEM_REP_RAW_INT)
        return false;
      return l->semantic_rep == NY_SEM_REP_TAGGED_DYNAMIC || l->is_any;
    }
    const expr_t *g = ny_native_nir_find_top_level_value(b, e->as.ident.name);
    if (g && g != e) {
      if (ny_expr_is_nil_literal(g))
        return true;
      return ny_native_nir_expr_is_any(b, g);
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
    return e->as.binary.op &&
           (ny_native_nir_expr_is_any(b, e->as.binary.left) ||
            ny_native_nir_expr_is_any(b, e->as.binary.right));
  case NY_E_UNARY:
    return ny_native_nir_expr_is_any(b, e->as.unary.right);
  case NY_E_MEMBER: {
    const expr_t *v = ny_native_nir_resolve_member_expr(b, e);
    return v && v != e && ny_native_nir_expr_is_any(b, v);
  }
  case NY_E_CALL:
    /*
     * The native load64 intrinsics return raw machine i64 values. The
     * language-level `any` return annotations describe the tagged VM path,
     * not the shared NYIR representation.
     */
    if (ny_native_call_leaf(e) &&
        (ny_native_leaf_kind(ny_native_call_leaf(e)) == NY_NATIVE_LEAF_LOAD8 ||
         ny_native_leaf_kind(ny_native_call_leaf(e)) == NY_NATIVE_LEAF_STORE8 ||
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
                 (ny_native_nir_expr_is_list(
                      b, e->as.call.callee->as.member.target) ||
                  ny_native_nir_expr_is_bytes(
                      b, e->as.call.callee->as.member.target) ||
                  ny_native_nir_expr_is_range(
                      b, e->as.call.callee->as.member.target)));
    }
    if (e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT &&
        e->as.call.callee->as.ident.name) {
      const char *cname = e->as.call.callee->as.ident.name;
      const stmt_t *fn = ny_native_nir_find_user_function(b, cname);
      if (!fn)
        fn = ny_native_nir_find_imported_function(b, cname);
      if (fn)
        return !fn->as.fn.return_type ||
               strcmp(fn->as.fn.return_type, "any") == 0;
      ny_native_nir_local_t *local = ny_native_nir_find_local(b, cname);
      if (local)
        return true;
    }
    break;
  case NY_E_MEMCALL:
    if (e->as.memcall.name && (strcmp(e->as.memcall.name, "get") == 0 ||
                               strcmp(e->as.memcall.name, "pop") == 0 ||
                               strcmp(e->as.memcall.name, "slice") == 0))
      return !(e->as.memcall.target &&
               (ny_native_nir_expr_is_list(b, e->as.memcall.target) ||
                ny_native_nir_expr_is_bytes(b, e->as.memcall.target) ||
                ny_native_nir_expr_is_range(b, e->as.memcall.target)));
    break;
  default:
    break;
  }
  return false;
}

/*
 * Native dictionary/list accessors return the payload stored in the native
 * slot.  That payload is raw even when the source expression is `any`, unlike
 * an any-valued function parameter which enters the ABI tagged.  Keep this
 * provenance fact local to lowering so arithmetic does not guess from the
 * low bit of an ambiguous integer (raw 1 and tagged integer 0 are identical
 * to the old runtime heuristic).
 */
static bool ny_native_nir_expr_is_raw_dynamic_read(ny_native_nir_builder_t *b,
                                                   const expr_t *e) {
  if (!e)
    return false;
  if (e->kind == NY_E_CALL && ny_native_call_leaf(e)) {
    ny_native_leaf_kind_t kind = ny_native_leaf_kind(ny_native_call_leaf(e));
    if (kind == NY_NATIVE_LEAF_LOAD8 || kind == NY_NATIVE_LEAF_LOAD64 ||
        kind == NY_NATIVE_LEAF_LOAD64_IDX)
      return true;
  }
  /*
   * Indexing an `any` local is lowered through rt_tbuf_get.  Like a
   * dynamic get/pop, that accessor returns the native slot payload (raw i64
   * for scalar typed buffers), not the tagged any-call ABI.
   */
  if (e->kind == NY_E_INDEX)
    return true;
  /*
   * Preserve raw-slot provenance through arithmetic trees.  A compound
   * accumulator commonly arrives as `total += 3 - list[i]`; the outer `+`
   * must not dispatch to the boxed any helper merely because its RHS is an
   * expression whose leaf is an indexed native buffer value.
   */
  if (e->kind == NY_E_BINARY)
    return ny_native_nir_expr_is_raw_dynamic_read(b, e->as.binary.left) ||
           ny_native_nir_expr_is_raw_dynamic_read(b, e->as.binary.right);
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
  if (ny_native_nir_expr_is_bytes(b, target) ||
      ny_native_nir_expr_is_range(b, target))
    return false;
  if (ny_native_nir_expr_is_list(b, target) &&
      !ny_native_nir_expr_is_any(b, target) &&
      !ny_native_nir_expr_is_dyn_list(b, target) &&
      !ny_native_nir_expr_is_dict(b, target))
    return true;
  if (target->kind == NY_E_IDENT && target->as.ident.name) {
    const ny_native_nir_local_t *local =
        ny_native_nir_find_local(b, target->as.ident.name);
    if (local && local->is_list && !local->is_any && !local->is_dict)
      return true;
  }
  return false;
}

static const char *ny_native_nir_dict_get_symbol(ny_native_nir_builder_t *b,
                                                 const expr_t *key) {
  if (ny_native_nir_expr_is_cstr(b, key))
    return "rt_dict_get_str_raw";
  /* Mirror the set-side rule: a str-typed call result (to_str(i)) is a
   * string key by type.  Probing with the i64 variant hashed the pointer
   * word instead of the bytes, so half the lookups missed. */
  if (key && key->kind == NY_E_CALL) {
    const stmt_t *fn = NULL;
    if (key->semantic.canonical_callee_stmt &&
        key->semantic.canonical_callee_stmt->kind == NY_S_FUNC)
      fn = key->semantic.canonical_callee_stmt;
    const char *leaf = ny_native_call_leaf(key);
    if (!fn && leaf)
      fn = ny_native_nir_find_user_function(b, leaf);
    if (!fn && leaf)
      fn = ny_native_nir_find_imported_function(b, leaf);
    if (fn && fn->as.fn.return_type &&
        strcmp(fn->as.fn.return_type, "str") == 0)
      return "rt_dict_get_str_raw";
  }
  if (key && key->semantic.resolved &&
      key->semantic.rep != NY_SEM_REP_RAW_INT)
    return "rt_dict_get_str_raw";
  return "rt_dict_get_raw";
}

static const char *ny_native_nir_dict_set_symbol(ny_native_nir_builder_t *b,
                                                 const expr_t *key) {
  if (ny_native_nir_expr_is_cstr(b, key))
    return "rt_native_dict_set_str_compact";
  /* A str-typed call result (to_str(i)) is a string key by type even when
   * the value is a managed handle rather than a raw literal.  Choosing the
   * i64 variant stored to_str's reused buffer word, so half the inserted
   * keys aliased later numbers (dict MISMATCH checksum 90674). */
  if (key && key->kind == NY_E_CALL) {
    const stmt_t *fn = NULL;
    if (key->semantic.canonical_callee_stmt &&
        key->semantic.canonical_callee_stmt->kind == NY_S_FUNC)
      fn = key->semantic.canonical_callee_stmt;
    const char *leaf = ny_native_call_leaf(key);
    if (!fn && leaf)
      fn = ny_native_nir_find_user_function(b, leaf);
    if (!fn && leaf)
      fn = ny_native_nir_find_imported_function(b, leaf);
    if (fn && fn->as.fn.return_type &&
        strcmp(fn->as.fn.return_type, "str") == 0)
      return "rt_native_dict_set_str_compact";
  }
  if (key && key->semantic.resolved &&
      key->semantic.rep != NY_SEM_REP_RAW_INT)
    return "rt_native_dict_set_str_compact";
  return "rt_native_dict_set_nir_i64";
}

static const stmt_t *ny_native_nir_find_user_function_in_stmt(const stmt_t *s,
                                                              const char *name,
                                                              unsigned depth);
static const stmt_t *
ny_native_nir_find_user_function_target(const stmt_t *s, const char *name,
                                        const ny_options *opt, unsigned depth);
static const stmt_t *
ny_native_nir_find_imported_function(const ny_native_nir_builder_t *b,
                                     const char *name);
static const stmt_t *
ny_native_nir_find_reexported_function(const ny_native_nir_builder_t *b,
                                       const stmt_t *mod, const char *name);
static const expr_t *
ny_native_nir_find_top_level_value_in_stmt(const stmt_t *s, const char *name,
                                           unsigned depth);
static const char *
ny_native_nir_resolve_use_alias(const ny_native_nir_builder_t *b,
                                const char *alias);
static const char *
ny_native_nir_use_module_canonical(const ny_native_nir_builder_t *b,
                                   const char *raw);
static const char *ny_native_nir_module_name_for_use(const stmt_t *s,
                                                     const char *basename,
                                                     const char *rel,
                                                     unsigned depth);
static bool ny_native_nir_same_source_file(const char *a, const char *b);

static const char *ny_native_nir_platform_string(const ny_options *opt,
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
  if (strcmp(fn_name, lookup) == 0)
    return true;
  const char *fn_leaf = ny_native_leaf_name(fn_name);
  if (fn_leaf && fn_leaf != fn_name && strcmp(fn_leaf, lookup) == 0)
    return true;
  const char *lookup_leaf = ny_native_leaf_name(lookup);
  if (lookup_leaf && lookup_leaf != lookup && strcmp(fn_name, lookup_leaf) == 0)
    return true;
  size_t fn_len = strlen(fn_name);
  size_t lookup_len = strlen(lookup);
  if (fn_len > lookup_len && fn_name[fn_len - lookup_len - 1] == '.' &&
      strcmp(fn_name + fn_len - lookup_len, lookup) == 0)
    return true;
  if (lookup_len > fn_len && lookup[lookup_len - fn_len - 1] == '.' &&
      strcmp(lookup + lookup_len - fn_len, fn_name) == 0)
    return true;
  return false;
}

typedef struct {
  const program_t *prog;
  const stmt_t **funcs;
  size_t count;
  size_t cap;
} ny_native_fn_cache_t;

static ny_native_fn_cache_t ny_native_fn_cache;
typedef struct {
  const char *source_file;
  char name[512];
  const stmt_t *fn;
  bool resolved;
} ny_native_fn_resolve_cache_entry_t;
static ny_native_fn_resolve_cache_entry_t *ny_native_fn_resolve_cache;
static size_t ny_native_fn_resolve_cache_len;
static size_t ny_native_fn_resolve_cache_cap;

static const stmt_t *ny_native_fn_resolve_cache_get(const char *name,
                                                    const char *source_file,
                                                    bool *known) {
  if (known)
    *known = false;
  for (size_t i = 0; i < ny_native_fn_resolve_cache_len; ++i) {
    ny_native_fn_resolve_cache_entry_t *entry = &ny_native_fn_resolve_cache[i];
    bool same_file = entry->source_file == source_file ||
                     (!entry->source_file && !source_file) ||
                     (entry->source_file && source_file &&
                      strcmp(entry->source_file, source_file) == 0);
    if (same_file && strcmp(entry->name, name) == 0) {
      if (known)
        *known = entry->resolved;
      return entry->fn;
    }
  }
  return NULL;
}

static void ny_native_fn_resolve_cache_put(const char *name,
                                           const char *source_file,
                                           const stmt_t *fn) {
  if (!name || strlen(name) >= sizeof(ny_native_fn_resolve_cache[0].name))
    return;
  for (size_t i = 0; i < ny_native_fn_resolve_cache_len; ++i) {
    ny_native_fn_resolve_cache_entry_t *entry = &ny_native_fn_resolve_cache[i];
    bool same_file = entry->source_file == source_file ||
                     (!entry->source_file && !source_file) ||
                     (entry->source_file && source_file &&
                      strcmp(entry->source_file, source_file) == 0);
    if (same_file && strcmp(entry->name, name) == 0) {
      entry->fn = fn;
      entry->resolved = true;
      return;
    }
  }
  if (ny_native_fn_resolve_cache_len == ny_native_fn_resolve_cache_cap) {
    size_t cap = ny_native_fn_resolve_cache_cap
                     ? ny_native_fn_resolve_cache_cap * 2
                     : 256;
    ny_native_fn_resolve_cache_entry_t *grown =
        realloc(ny_native_fn_resolve_cache, cap * sizeof(*grown));
    if (!grown)
      return;
    ny_native_fn_resolve_cache = grown;
    ny_native_fn_resolve_cache_cap = cap;
  }
  ny_native_fn_resolve_cache_entry_t *entry =
      &ny_native_fn_resolve_cache[ny_native_fn_resolve_cache_len++];
  entry->source_file = source_file;
  snprintf(entry->name, sizeof(entry->name), "%s", name);
  entry->fn = fn;
  entry->resolved = true;
}

static const stmt_t *
ny_native_fn_resolve_cache_put_both(const ny_native_nir_builder_t *b,
                                    const char *name, const char *lookup_name,
                                    const stmt_t *fn) {
  if (b) {
    if (lookup_name)
      ny_native_fn_resolve_cache_put(lookup_name, b->source_file, fn);
    if (name && (!lookup_name || strcmp(name, lookup_name) != 0))
      ny_native_fn_resolve_cache_put(name, b->source_file, fn);
  }
  return fn;
}

static const stmt_t *ny_native_fn_cache_lookup_exact(const char *name) {
  if (!name)
    return NULL;
  for (size_t i = 0; i < ny_native_fn_cache.count; ++i) {
    const stmt_t *fn = ny_native_fn_cache.funcs[i];
    if (fn && fn->as.fn.name && strcmp(fn->as.fn.name, name) == 0 &&
        !ny_is_stdlib_tok(fn->tok))
      return fn;
  }
  for (size_t i = 0; i < ny_native_fn_cache.count; ++i) {
    const stmt_t *fn = ny_native_fn_cache.funcs[i];
    if (fn && fn->as.fn.name && strcmp(fn->as.fn.name, name) == 0)
      return fn;
  }
  return NULL;
}

static const stmt_t *ny_native_fn_cache_lookup(const char *name) {
  if (!name)
    return NULL;
  for (size_t i = 0; i < ny_native_fn_cache.count; ++i) {
    const stmt_t *fn = ny_native_fn_cache.funcs[i];
    if (!fn || !fn->as.fn.name)
      continue;
    if (strcmp(fn->as.fn.name, name) == 0 && !ny_is_stdlib_tok(fn->tok))
      return fn;
  }
  for (size_t i = 0; i < ny_native_fn_cache.count; ++i) {
    const stmt_t *fn = ny_native_fn_cache.funcs[i];
    if (!fn || !fn->as.fn.name)
      continue;
    if (strcmp(fn->as.fn.name, name) == 0)
      return fn;
  }
  for (size_t i = 0; i < ny_native_fn_cache.count; ++i) {
    const stmt_t *fn = ny_native_fn_cache.funcs[i];
    if (!fn || !fn->as.fn.name)
      continue;
    if (ny_native_name_matches(fn->as.fn.name, name) &&
        !ny_is_stdlib_tok(fn->tok))
      return fn;
  }
  for (size_t i = 0; i < ny_native_fn_cache.count; ++i) {
    const stmt_t *fn = ny_native_fn_cache.funcs[i];
    if (!fn || !fn->as.fn.name)
      continue;
    if (ny_native_name_matches(fn->as.fn.name, name))
      return fn;
  }
  return NULL;
}

static void ny_native_fn_cache_add(const stmt_t *s) {
  if (!s || s->kind != NY_S_FUNC || !s->as.fn.name)
    return;
  if (ny_native_fn_cache.count == ny_native_fn_cache.cap) {
    size_t next = ny_native_fn_cache.cap ? ny_native_fn_cache.cap * 2 : 256;
    const stmt_t **grown = realloc(ny_native_fn_cache.funcs,
                                   next * sizeof(*ny_native_fn_cache.funcs));
    if (!grown)
      return;
    ny_native_fn_cache.funcs = grown;
    ny_native_fn_cache.cap = next;
  }
  ny_native_fn_cache.funcs[ny_native_fn_cache.count++] = s;
}

static void ny_native_fn_cache_collect(const stmt_t *s, unsigned depth) {
  if (!s || depth > 64)
    return;
  if (s->kind == NY_S_FUNC) {
    /*
     * Expanded imports can expose the same function node through several
     * re-export/package surfaces.  Avoid recursively walking its body again;
     * without this identity guard a broad import graph turns cache building
     * into exponential work before reachable-function collection starts.
     */
    for (size_t i = 0; i < ny_native_fn_cache.count; ++i)
      if (ny_native_fn_cache.funcs[i] == s)
        return;
    ny_native_fn_cache_add(s);
    ny_native_fn_cache_collect(s->as.fn.body, depth + 1);
    return;
  }
  if (s->kind == NY_S_MODULE) {
    for (size_t i = 0; i < s->as.module.body.len; ++i)
      ny_native_fn_cache_collect(s->as.module.body.data[i], depth + 1);
    return;
  }
  if (s->kind == NY_S_IMPL) {
    for (size_t i = 0; i < s->as.impl.methods.len; ++i)
      ny_native_fn_cache_collect(s->as.impl.methods.data[i], depth + 1);
    return;
  }
  if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      ny_native_fn_cache_collect(s->as.block.body.data[i], depth + 1);
    return;
  }
  if (s->kind == NY_S_IF) {
    ny_native_fn_cache_collect(s->as.iff.conseq, depth + 1);
    ny_native_fn_cache_collect(s->as.iff.alt, depth + 1);
    return;
  }
  if (s->kind == NY_S_WHILE) {
    ny_native_fn_cache_collect(s->as.whl.body, depth + 1);
    return;
  }
  if (s->kind == NY_S_FOR) {
    ny_native_fn_cache_collect(s->as.fr.body, depth + 1);
    return;
  }
  if (s->kind == NY_S_MATCH) {
    for (size_t i = 0; i < s->as.match.arms.len; ++i)
      ny_native_fn_cache_collect(s->as.match.arms.data[i].conseq, depth + 1);
    ny_native_fn_cache_collect(s->as.match.default_conseq, depth + 1);
    return;
  }
  if (s->kind == NY_S_TRY) {
    ny_native_fn_cache_collect(s->as.tr.body, depth + 1);
    ny_native_fn_cache_collect(s->as.tr.handler, depth + 1);
    return;
  }
  if (s->kind == NY_S_DEFER)
    ny_native_fn_cache_collect(s->as.de.body, depth + 1);
}
static bool ny_native_nir_stmt_has_defer(const stmt_t *s, unsigned depth) {
  if (!s || depth > 64)
    return false;
  switch (s->kind) {
  case NY_S_DEFER:
    return true;
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      if (ny_native_nir_stmt_has_defer(s->as.block.body.data[i], depth + 1))
        return true;
    return false;
  case NY_S_IF:
    return ny_native_nir_stmt_has_defer(s->as.iff.conseq, depth + 1) ||
           ny_native_nir_stmt_has_defer(s->as.iff.alt, depth + 1);
  case NY_S_WHILE:
    return ny_native_nir_stmt_has_defer(s->as.whl.body, depth + 1);
  case NY_S_FOR:
    return ny_native_nir_stmt_has_defer(s->as.fr.body, depth + 1);
  case NY_S_MATCH:
    for (size_t i = 0; i < s->as.match.arms.len; ++i)
      if (ny_native_nir_stmt_has_defer(s->as.match.arms.data[i].conseq,
                                       depth + 1))
        return true;
    return ny_native_nir_stmt_has_defer(s->as.match.default_conseq, depth + 1);
  case NY_S_TRY:
    return ny_native_nir_stmt_has_defer(s->as.tr.body, depth + 1) ||
           ny_native_nir_stmt_has_defer(s->as.tr.handler, depth + 1);
  default:
    return false;
  }
}

static void ny_native_fn_cache_build(const program_t *prog) {
  if (ny_native_fn_cache.prog == prog)
    return;
  free(ny_native_fn_resolve_cache);
  ny_native_fn_resolve_cache = NULL;
  ny_native_fn_resolve_cache_len = 0;
  ny_native_fn_resolve_cache_cap = 0;
  ny_native_fn_cache.prog = prog;
  ny_native_fn_cache.count = 0;
  if (!prog)
    return;
  for (size_t i = 0; i < prog->body.len; ++i)
    ny_native_fn_cache_collect(prog->body.data[i], 0);
}

static const stmt_t *
ny_native_nir_find_module(const stmt_t *s, const char *name, unsigned depth);
static const stmt_t *ny_native_nir_find_nested_module(const stmt_t *s,
                                                      const char *prefix,
                                                      const char *leaf,
                                                      unsigned depth);
/*
 * A package module's exports can surface its child modules' public functions
 * one level up without a call-site spelling change (`module implicitpkg`
 * exporting `child` exposes `implicitpkg.child.child_value` as the valid
 * reference `implicitpkg.child_value`).  Collect the unique function with the
 * requested leaf from modules whose declared names nest under `prefix.`.
 */
static void ny_native_nir_find_package_surface_fn(
    const stmt_t *s, const char *prefix, size_t prefix_len, const char *leaf,
    const stmt_t **unique, bool *ambiguous, unsigned depth) {
  if (!s || depth > 64 || *ambiguous)
    return;
  if (s->kind == NY_S_MODULE) {
    if (s->as.module.name && *s->as.module.name &&
        strncmp(s->as.module.name, prefix, prefix_len) == 0 &&
        s->as.module.name[prefix_len] == '.') {
      const stmt_t *found =
          ny_native_nir_find_user_function_in_stmt(s, leaf, 0);
      if (found) {
        if (*unique && *unique != found) {
          *ambiguous = true;
          return;
        }
        *unique = found;
      }
    }
    for (size_t i = 0; i < s->as.module.body.len; ++i) {
      ny_native_nir_find_package_surface_fn(s->as.module.body.data[i], prefix,
                                            prefix_len, leaf, unique, ambiguous,
                                            depth + 1);
      if (*ambiguous)
        return;
    }
    return;
  }
  if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; ++i) {
      ny_native_nir_find_package_surface_fn(s->as.block.body.data[i], prefix,
                                            prefix_len, leaf, unique, ambiguous,
                                            depth + 1);
      if (*ambiguous)
        return;
    }
  }
}

static const stmt_t *
ny_native_nir_find_user_function(ny_native_nir_builder_t *b, const char *name) {
  if (!name)
    return NULL;
  if (strncmp(name, "__ny_lambda_", 12) == 0) {
    for (size_t i = 0; i < ny_native_lambda_count; ++i) {
      if (ny_native_lambda_entries[i].name &&
          strcmp(ny_native_lambda_entries[i].name, name) == 0)
        return ny_native_lambda_entries[i].fn;
    }
  }
  if (!b || !b->prog)
    return NULL;
  if (b->externs && ny_extern_table_lookup(b->externs, name))
    return NULL;
  /*
   * Reachability discovery asks the same resolver about a name from many
   * expressions.  Consult both positive and negative results before any AST
   * walk; the expanded standard library is large enough that otherwise this
   * becomes quadratic.
   */
  if (ny_native_fn_cache.prog == b->prog) {
    bool known = false;
    const stmt_t *cached =
        ny_native_fn_resolve_cache_get(name, b->source_file, &known);
    if (known)
      return cached;
  }
  /*
   * The function index is built once per expanded program.  Use it before
   * any recursive AST walk; reachable-call scanning asks this question for
   * the same names repeatedly.
   */
  if (ny_native_fn_cache.prog == b->prog && !strchr(name, '.')) {
    for (size_t i = 0; i < ny_native_fn_cache.count; ++i) {
      const stmt_t *indexed = ny_native_fn_cache.funcs[i];
      if (indexed && indexed->as.fn.name &&
          strcmp(indexed->as.fn.name, name) == 0 &&
          (!ny_is_stdlib_tok(indexed->tok) ||
           !ny_native_runtime_symbol(name))) {
        ny_native_fn_resolve_cache_put(name, b->source_file, indexed);
        return indexed;
      }
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
  if (ny_native_fn_cache.prog != b->prog && !strchr(name, '.')) {
    for (size_t i = 0; i < b->prog->body.len; ++i) {
      const stmt_t *found = ny_native_nir_find_user_function_exact(
          b->prog->body.data[i], name, b->options, 0);
      if (found && !ny_is_stdlib_tok(found->tok)) {
        ny_native_fn_resolve_cache_put(name, b->source_file, found);
        return found;
      }
    }
  }
  if (strcmp(name, "std.os.platform.init_hint") == 0)
    lookup_name = "std.os.ui.window.platform.init_hint";
  const char *imported_name = ny_native_nir_resolve_use_alias(b, name);
  if (imported_name && strchr(imported_name, '.'))
    lookup_name = imported_name;
  const char *dot = strchr(name, '.');
  if (dot && dot != name) {
    size_t alias_len = (size_t)(dot - name);
    if (alias_len < 256) {
      char alias[256];
      memcpy(alias, name, alias_len);
      alias[alias_len] = '\0';
      const char *module = ny_native_nir_resolve_use_alias(b, alias);
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
  if (ny_native_fn_cache.prog == b->prog) {
    const stmt_t *unique = NULL;
    bool ambiguous = false;
    for (size_t i = 0; i < ny_native_fn_cache.count; ++i) {
      const stmt_t *candidate = ny_native_fn_cache.funcs[i];
      if (!candidate || !candidate->as.fn.name ||
          strcmp(candidate->as.fn.name, lookup_name) != 0)
        continue;
      if (unique) {
        ambiguous = true;
        break;
      }
      unique = candidate;
    }
    if (unique && !ambiguous) {
      return ny_native_fn_resolve_cache_put_both(b, name, lookup_name, unique);
    }
  }
  if (!strchr(name, '.')) {
    const stmt_t *imported_fn = ny_native_nir_find_imported_function(b, name);
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
  if (!strchr(name, '.') && ny_native_fn_cache.prog == b->prog) {
    const stmt_t *unique = NULL;
    bool ambiguous = false;
    for (size_t i = 0; i < ny_native_fn_cache.count; ++i) {
      const stmt_t *candidate = ny_native_fn_cache.funcs[i];
      if (!candidate || !candidate->as.fn.name ||
          !ny_native_name_matches(candidate->as.fn.name, name))
        continue;
      if (unique) {
        ambiguous = true;
        break;
      }
      unique = candidate;
    }
    if (!ambiguous) {
      return ny_native_fn_resolve_cache_put_both(b, name, lookup_name, unique);
    }
  }
  /*
   * Prefer the branch selected for the current target. Platform modules
   * intentionally provide extern implementations in one branch and local
   * fallbacks in the other; an all-tree cache can otherwise select the
   * inactive extern declaration first.
   */
  for (size_t i = 0; i < b->prog->body.len; ++i) {
    const stmt_t *found = ny_native_nir_find_user_function_exact(
        b->prog->body.data[i], lookup_name, b->options, 0);
    if (found) {
      return ny_native_fn_resolve_cache_put_both(b, name, lookup_name, found);
    }
  }
  for (size_t i = 0; i < b->prog->body.len; ++i) {
    const stmt_t *found = ny_native_nir_find_user_function_target(
        b->prog->body.data[i], lookup_name, b->options, 0);
    if (found) {
      return ny_native_fn_resolve_cache_put_both(b, name, lookup_name, found);
    }
  }
  bool resolve_cache_known = false;
  const stmt_t *cached_resolved = ny_native_fn_resolve_cache_get(
      lookup_name, b->source_file, &resolve_cache_known);
  if (resolve_cache_known)
    return cached_resolved;
  if (!resolve_cache_known && name && strcmp(name, lookup_name) != 0) {
    cached_resolved = ny_native_fn_resolve_cache_get(name, b->source_file,
                                                     &resolve_cache_known);
    if (resolve_cache_known)
      return cached_resolved;
  }
  if (ny_native_fn_cache.prog == b->prog) {
    const stmt_t *found = ny_native_fn_cache_lookup(lookup_name);
    if (found) {
      return ny_native_fn_resolve_cache_put_both(b, name, lookup_name, found);
    }
  }
  for (size_t i = 0; i < b->prog->body.len; ++i) {
    const stmt_t *found = ny_native_nir_find_user_function_in_stmt(
        b->prog->body.data[i], lookup_name, 0);
    if (found) {
      return ny_native_fn_resolve_cache_put_both(b, name, lookup_name, found);
    }
  }
  if (strchr(lookup_name, '.')) {
    const char *leaf = strrchr(lookup_name, '.') + 1;
    const char *last = strrchr(lookup_name, '.');
    size_t module_len = (size_t)(last - lookup_name);
    /*
     * Copy the module prefix into a dedicated buffer.  `lookup_name` can
     * alias the canonical buffer, so truncating it here would silently
     * strip the member suffix from every later dot-based fallback.
     */
    if (module_len > 0 && module_len < sizeof(canonical)) {
      char module_prefix[256];
      memcpy(module_prefix, lookup_name, module_len);
      module_prefix[module_len] = '\0';
      for (size_t i = 0; i < b->prog->body.len; ++i) {
        const stmt_t *module =
            ny_native_nir_find_module(b->prog->body.data[i], module_prefix, 0);
        if (!module)
          continue;
        const stmt_t *found =
            ny_native_nir_find_user_function_in_stmt(module, leaf, 0);
        if (!found)
          found = ny_native_nir_find_reexported_function(b, module, leaf);
        if (found) {
          return ny_native_fn_resolve_cache_put_both(b, name, lookup_name,
                                                     found);
        }
      }
    }
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
      const stmt_t *unique = NULL;
      bool ambiguous = false;
      for (size_t i = 0; i < b->prog->body.len && !ambiguous; ++i)
        ny_native_nir_find_package_surface_fn(b->prog->body.data[i],
                                              lookup_name, module_len, leaf,
                                              &unique, &ambiguous, 0);
      if (!ambiguous && unique) {
        return ny_native_fn_resolve_cache_put_both(b, name, lookup_name,
                                                   unique);
      }
    }
  }
  if (strchr(lookup_name, '.')) {
    const char *leaf = strrchr(lookup_name, '.') + 1;
    const stmt_t *unique = NULL;
    for (size_t i = 0; i < ny_native_fn_cache.count; ++i) {
      const stmt_t *candidate = ny_native_fn_cache.funcs[i];
      if (!candidate || !candidate->as.fn.name ||
          strcmp(candidate->as.fn.name, leaf) != 0)
        continue;
      if (unique)
        return NULL;
      unique = candidate;
    }
    if (unique) {
      return ny_native_fn_resolve_cache_put_both(b, name, lookup_name, unique);
    }
  }
  ny_native_fn_resolve_cache_put_both(b, name, lookup_name, NULL);
  return NULL;
}

/*
 * Recurse into NY_S_MODULE/NY_S_BLOCK/control-flow bodies to find
 * nested function declarations (e.g. stdlib functions inside modules).
 * depth caps at 64 for safety, matching the rest of the codebase.
 */
static const stmt_t *ny_native_nir_find_user_function_in_stmt(const stmt_t *s,
                                                              const char *name,
                                                              unsigned depth) {
  if (!s || !name || depth > 64)
    return NULL;
  if (s->kind == NY_S_FUNC && s->as.fn.name &&
      ny_native_name_matches(s->as.fn.name, name))
    return s;
  if (s->kind == NY_S_MODULE) {
    for (size_t i = 0; i < s->as.module.body.len; ++i) {
      const stmt_t *found = ny_native_nir_find_user_function_in_stmt(
          s->as.module.body.data[i], name, depth + 1);
      if (found)
        return found;
    }
    return NULL;
  }
  if (s->kind == NY_S_IMPL) {
    for (size_t i = 0; i < s->as.impl.methods.len; ++i) {
      const stmt_t *found = ny_native_nir_find_user_function_in_stmt(
          s->as.impl.methods.data[i], name, depth + 1);
      if (found)
        return found;
    }
    return NULL;
  }
  if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; ++i) {
      const stmt_t *found = ny_native_nir_find_user_function_in_stmt(
          s->as.block.body.data[i], name, depth + 1);
      if (found)
        return found;
    }
    return NULL;
  }
  if (s->kind == NY_S_IF) {
    if (s->as.iff.conseq) {
      const stmt_t *found = ny_native_nir_find_user_function_in_stmt(
          s->as.iff.conseq, name, depth + 1);
      if (found)
        return found;
    }
    if (s->as.iff.alt) {
      const stmt_t *found = ny_native_nir_find_user_function_in_stmt(
          s->as.iff.alt, name, depth + 1);
      if (found)
        return found;
    }
    return NULL;
  }
  if (s->kind == NY_S_WHILE && s->as.whl.body)
    return ny_native_nir_find_user_function_in_stmt(s->as.whl.body, name,
                                                    depth + 1);
  if (s->kind == NY_S_FOR && s->as.fr.body)
    return ny_native_nir_find_user_function_in_stmt(s->as.fr.body, name,
                                                    depth + 1);
  if (s->kind == NY_S_MATCH) {
    for (size_t i = 0; i < s->as.match.arms.len; ++i) {
      const stmt_t *found = ny_native_nir_find_user_function_in_stmt(
          s->as.match.arms.data[i].conseq, name, depth + 1);
      if (found)
        return found;
    }
    if (s->as.match.default_conseq)
      return ny_native_nir_find_user_function_in_stmt(
          s->as.match.default_conseq, name, depth + 1);
    return NULL;
  }
  return NULL;
}

/*
 * Follow a module's re-export chain (`use ... (name as alias)`) down to the
 * underlying definition. A re-exporting module declares no body of its own
 * for the name, so a chase through its named-import use statements is the
 * only way to locate the source function. Bounded by depth so inter-module
 * import cycles stay deterministic. Named-import items only to prevent
 * combinatorial fanout across import-all facades.
 */
static const stmt_t *ny_native_nir_find_reexported_function_rec(
    const ny_native_nir_builder_t *b, const stmt_t *mod, const char *name,
    unsigned depth, const stmt_t **visited, size_t *visited_len,
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
          ny_native_nir_use_module_canonical(b, use->as.use.module);
      const stmt_t *target_mod = NULL;
      for (size_t r = 0; r < b->prog->body.len; ++r) {
        target_mod = ny_native_nir_find_module(b->prog->body.data[r],
                                               target_module_name, 0);
        if (target_mod)
          break;
      }
      if (!target_mod)
        continue;
      const stmt_t *found =
          ny_native_nir_find_user_function_in_stmt(target_mod, item->name, 0);
      if (!found && target_mod->as.module.name &&
          !strchr(target_mod->as.module.name, '.')) {
        size_t plen = strlen(target_mod->as.module.name);
        const stmt_t *unique = NULL;
        bool ambiguous = false;
        for (size_t r = 0; r < b->prog->body.len && !ambiguous; ++r)
          ny_native_nir_find_package_surface_fn(
              b->prog->body.data[r], target_mod->as.module.name, plen,
              item->name, &unique, &ambiguous, 0);
        if (!ambiguous && unique)
          found = unique;
      }
      if (!found)
        found = ny_native_nir_find_reexported_function_rec(
            b, target_mod, item->name, depth + 1, visited, visited_len,
            visited_cap);
      if (found)
        return found;
    }
  }
  return NULL;
}

static const stmt_t *
ny_native_nir_find_reexported_function(const ny_native_nir_builder_t *b,
                                       const stmt_t *mod, const char *name) {
  const stmt_t *visited[32];
  size_t visited_len = 0;
  return ny_native_nir_find_reexported_function_rec(b, mod, name, 0, visited,
                                                    &visited_len, 32);
}

static const stmt_t *
ny_native_nir_find_user_function_exact(const stmt_t *s, const char *name,
                                       const ny_options *opt, unsigned depth) {
  if (!s || !name || depth > 64)
    return NULL;
  if (s->kind == NY_S_FUNC && s->as.fn.name && strcmp(s->as.fn.name, name) == 0)
    return s;
  if (s->kind == NY_S_IF) {
    bool selected = false;
    if (s->as.iff.test && s->as.iff.test->kind == NY_E_COMPTIME &&
        ny_native_target_eval_bool(opt, s->as.iff.test, &selected))
      return ny_native_nir_find_user_function_exact(
          selected ? s->as.iff.conseq : s->as.iff.alt, name, opt, depth + 1);
    const stmt_t *found = ny_native_nir_find_user_function_exact(
        s->as.iff.conseq, name, opt, depth + 1);
    return found ? found
                 : ny_native_nir_find_user_function_exact(s->as.iff.alt, name,
                                                          opt, depth + 1);
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
      const stmt_t *found = ny_native_nir_find_user_function_exact(
          body->data[i], name, opt, depth + 1);
      if (found)
        return found;
    }
  }
  if (s->kind == NY_S_WHILE)
    return ny_native_nir_find_user_function_exact(s->as.whl.body, name, opt,
                                                  depth + 1);
  if (s->kind == NY_S_FOR)
    return ny_native_nir_find_user_function_exact(s->as.fr.body, name, opt,
                                                  depth + 1);
  return NULL;
}

static const stmt_t *
ny_native_nir_find_user_function_target(const stmt_t *s, const char *name,
                                        const ny_options *opt, unsigned depth) {
  if (!s || !name || depth > 64)
    return NULL;
  /*
   * Most target-fallback queries are for a unique indexed helper.  Resolve
   * those in O(n) over the already-built function index instead of walking
   * every expanded stdlib statement for each call site.  Keep ambiguous names
   * on the target-aware tree walk so platform branches remain correct.
   */
  if (depth == 0 && ny_native_fn_cache.count > 0) {
    const stmt_t *unique = NULL;
    for (size_t i = 0; i < ny_native_fn_cache.count; ++i) {
      const stmt_t *candidate = ny_native_fn_cache.funcs[i];
      if (!candidate || !candidate->as.fn.name ||
          strcmp(candidate->as.fn.name, name) != 0)
        continue;
      if (unique)
        unique = NULL;
      else
        unique = candidate;
      if (!unique)
        break;
    }
    if (unique)
      return unique;
  }
  if (s->kind == NY_S_FUNC && s->as.fn.name &&
      ny_native_name_matches(s->as.fn.name, name))
    return s;
  if (s->kind == NY_S_IF) {
    bool selected = false;
    if (s->as.iff.test && s->as.iff.test->kind == NY_E_COMPTIME &&
        ny_native_target_eval_bool(opt, s->as.iff.test, &selected))
      return ny_native_nir_find_user_function_target(
          selected ? s->as.iff.conseq : s->as.iff.alt, name, opt, depth + 1);
    const stmt_t *found = ny_native_nir_find_user_function_target(
        s->as.iff.conseq, name, opt, depth + 1);
    return found ? found
                 : ny_native_nir_find_user_function_target(s->as.iff.alt, name,
                                                           opt, depth + 1);
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
      const stmt_t *found = ny_native_nir_find_user_function_target(
          body->data[i], name, opt, depth + 1);
      if (found)
        return found;
    }
  }
  if (s->kind == NY_S_WHILE)
    return ny_native_nir_find_user_function_target(s->as.whl.body, name, opt,
                                                   depth + 1);
  if (s->kind == NY_S_FOR)
    return ny_native_nir_find_user_function_target(s->as.fr.body, name, opt,
                                                   depth + 1);
  return NULL;
}
static const stmt_t *
ny_native_nir_find_imported_function(const ny_native_nir_builder_t *b,
                                     const char *name) {
  if (!b || !b->prog || !name || strchr(name, '.'))
    return NULL;
  const stmt_t *owner = NULL;
  if (b->module_name && *b->module_name) {
    for (size_t i = 0; i < b->prog->body.len && !owner; ++i)
      owner =
          ny_native_nir_find_module(b->prog->body.data[i], b->module_name, 0);
  }
  if (owner && owner->kind == NY_S_MODULE) {
    const stmt_t *local =
        ny_native_nir_find_user_function_in_stmt(owner, name, 0);
    if (local)
      return local;
    for (size_t i = 0; i < owner->as.module.body.len; ++i) {
      const stmt_t *use = owner->as.module.body.data[i];
      if (!use || use->kind != NY_S_USE || !use->as.use.module)
        continue;
      const stmt_t *imported = NULL;
      /*
       * A local `use "./file.ny"` stores its path in the use node, while
       * the imported module is indexed by its declared name.  Resolve the
       * path to that declared name (declared module blocks may not match the
       * file basename) so selective imports and renamed functions retain
       * their source-level meaning.
       */
      const char *module_name =
          ny_native_nir_use_module_canonical(b, use->as.use.module);
      for (size_t j = 0; j < b->prog->body.len && !imported; ++j)
        imported =
            ny_native_nir_find_module(b->prog->body.data[j], module_name, 0);
      if (!imported)
        continue;
      if (use->as.use.imports.len == 0) {
        const stmt_t *found =
            ny_native_nir_find_user_function_in_stmt(imported, name, 0);
        if (!found && imported->as.module.name &&
            !strchr(imported->as.module.name, '.')) {
          size_t plen = strlen(imported->as.module.name);
          const stmt_t *unique = NULL;
          bool ambiguous = false;
          for (size_t r = 0; r < b->prog->body.len && !ambiguous; ++r)
            ny_native_nir_find_package_surface_fn(
                b->prog->body.data[r], imported->as.module.name, plen, name,
                &unique, &ambiguous, 0);
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
            ny_native_nir_find_user_function_in_stmt(imported, item->name, 0);
        if (!found && imported->as.module.name &&
            !strchr(imported->as.module.name, '.')) {
          size_t plen = strlen(imported->as.module.name);
          const stmt_t *unique = NULL;
          bool ambiguous = false;
          for (size_t r = 0; r < b->prog->body.len && !ambiguous; ++r)
            ny_native_nir_find_package_surface_fn(
                b->prog->body.data[r], imported->as.module.name, plen,
                item->name, &unique, &ambiguous, 0);
          if (!ambiguous && unique)
            found = unique;
        }
        if (!found)
          found =
              ny_native_nir_find_reexported_function(b, imported, item->name);
        if (found)
          return found;
      }
    }
    return NULL;
  }
  for (size_t i = 0; i < b->prog->body.len; ++i) {
    const stmt_t *use = b->prog->body.data[i];
    if (!use || use->kind != NY_S_USE || !use->as.use.module)
      continue;
    if (b->source_file && use->tok.filename &&
        !ny_native_nir_same_source_file(use->tok.filename, b->source_file))
      continue;
    const stmt_t *imported = NULL;
    const char *module_name =
        ny_native_nir_use_module_canonical(b, use->as.use.module);
    for (size_t j = 0; j < b->prog->body.len && !imported; ++j)
      imported =
          ny_native_nir_find_module(b->prog->body.data[j], module_name, 0);
    if (!imported)
      continue;
    if (use->as.use.imports.len == 0) {
      const stmt_t *found =
          ny_native_nir_find_user_function_in_stmt(imported, name, 0);
      if (!found && imported->as.module.name &&
          !strchr(imported->as.module.name, '.')) {
        size_t plen = strlen(imported->as.module.name);
        const stmt_t *unique = NULL;
        bool ambiguous = false;
        for (size_t r = 0; r < b->prog->body.len && !ambiguous; ++r)
          ny_native_nir_find_package_surface_fn(b->prog->body.data[r],
                                                imported->as.module.name, plen,
                                                name, &unique, &ambiguous, 0);
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
          ny_native_nir_find_user_function_in_stmt(imported, item->name, 0);
      if (!found && imported->as.module.name &&
          !strchr(imported->as.module.name, '.')) {
        size_t plen = strlen(imported->as.module.name);
        const stmt_t *unique = NULL;
        bool ambiguous = false;
        for (size_t r = 0; r < b->prog->body.len && !ambiguous; ++r)
          ny_native_nir_find_package_surface_fn(
              b->prog->body.data[r], imported->as.module.name, plen, item->name,
              &unique, &ambiguous, 0);
        if (!ambiguous && unique)
          found = unique;
      }
      if (!found)
        found = ny_native_nir_find_reexported_function(b, imported, item->name);
      if (found)
        return found;
    }
  }
  return NULL;
}

static const stmt_t *
ny_native_nir_find_extern_decl_in_stmt(const stmt_t *s, const char *name,
                                       const ny_options *opt, unsigned depth) {
  if (!s || !name || depth > 64)
    return NULL;
  if (s->kind == NY_S_EXTERN && s->as.ext.name &&
      ny_native_name_matches(s->as.ext.name, name))
    return s;
  if (s->kind == NY_S_MODULE || s->kind == NY_S_BLOCK) {
    const ny_stmt_list *body =
        s->kind == NY_S_MODULE ? &s->as.module.body : &s->as.block.body;
    for (size_t i = 0; i < body->len; ++i) {
      const stmt_t *found = ny_native_nir_find_extern_decl_in_stmt(
          body->data[i], name, opt, depth + 1);
      if (found)
        return found;
    }
    return NULL;
  }
  if (s->kind == NY_S_IF) {
    bool selected = false;
    if (s->as.iff.test && s->as.iff.test->kind == NY_E_COMPTIME &&
        ny_native_target_eval_bool(opt, s->as.iff.test, &selected)) {
      return ny_native_nir_find_extern_decl_in_stmt(
          selected ? s->as.iff.conseq : s->as.iff.alt, name, opt, depth + 1);
    }
    const stmt_t *found = ny_native_nir_find_extern_decl_in_stmt(
        s->as.iff.conseq, name, opt, depth + 1);
    return found ? found
                 : ny_native_nir_find_extern_decl_in_stmt(s->as.iff.alt, name,
                                                          opt, depth + 1);
  }
  if (s->kind == NY_S_WHILE) {
    const stmt_t *found = ny_native_nir_find_extern_decl_in_stmt(
        s->as.whl.init, name, opt, depth + 1);
    if (!found)
      found = ny_native_nir_find_extern_decl_in_stmt(s->as.whl.body, name, opt,
                                                     depth + 1);
    return found ? found
                 : ny_native_nir_find_extern_decl_in_stmt(s->as.whl.update,
                                                          name, opt, depth + 1);
  }
  if (s->kind == NY_S_FOR) {
    const stmt_t *found = ny_native_nir_find_extern_decl_in_stmt(
        s->as.fr.init, name, opt, depth + 1);
    if (!found)
      found = ny_native_nir_find_extern_decl_in_stmt(s->as.fr.body, name, opt,
                                                     depth + 1);
    return found ? found
                 : ny_native_nir_find_extern_decl_in_stmt(s->as.fr.update, name,
                                                          opt, depth + 1);
  }
  if (s->kind == NY_S_TRY) {
    const stmt_t *found = ny_native_nir_find_extern_decl_in_stmt(
        s->as.tr.body, name, opt, depth + 1);
    return found ? found
                 : ny_native_nir_find_extern_decl_in_stmt(s->as.tr.handler,
                                                          name, opt, depth + 1);
  }
  if (s->kind == NY_S_DEFER)
    return ny_native_nir_find_extern_decl_in_stmt(s->as.de.body, name, opt,
                                                  depth + 1);
  if (s->kind == NY_S_MATCH) {
    for (size_t i = 0; i < s->as.match.arms.len; ++i) {
      const stmt_t *found = ny_native_nir_find_extern_decl_in_stmt(
          s->as.match.arms.data[i].conseq, name, opt, depth + 1);
      if (found)
        return found;
    }
    return ny_native_nir_find_extern_decl_in_stmt(s->as.match.default_conseq,
                                                  name, opt, depth + 1);
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
typedef struct {
  const program_t *prog;
  char name[128];
  bool result;
} ny_native_user_fn_cache_entry_t;

#define NY_USER_FN_CACHE_SIZE 512
static ny_native_user_fn_cache_entry_t ny_user_fn_cache[NY_USER_FN_CACHE_SIZE];

static bool ny_native_nir_user_defined_fn_uncached(ny_native_nir_builder_t *b,
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
  if (ny_native_fn_cache.prog == b->prog && surface && *surface) {
    const stmt_t *indexed = ny_native_fn_cache_lookup_exact(surface);
    if (indexed && !indexed->as.fn.is_extern && !indexed->as.fn.link_name &&
        !ny_is_stdlib_tok(indexed->tok)) {
      return true;
    }
  }
  const stmt_t *fn = ny_native_nir_find_user_function(b, name);
  if (!fn) {
    const char *leaf = strrchr(name, '.');
    if (leaf && leaf[1])
      fn = ny_native_nir_find_user_function(b, leaf + 1);
  }
  if (fn && !fn->as.fn.is_extern && !fn->as.fn.link_name &&
      !ny_is_stdlib_tok(fn->tok))
    return true;

  /*
   * Semantic resolution can canonicalize an unqualified builtin spelling to
   * its stdlib name before native lowering sees it.  That is valid only when
   * no source function shadows the surface spelling.  Search the indexed
   * program for a non-stdlib leaf so a user `len`, `set`, or `int` cannot be
   * silently replaced by a runtime intrinsic.
   */
  if (!surface || !*surface || ny_native_fn_cache.prog != b->prog)
    return false;
  for (size_t i = 0; i < ny_native_fn_cache.count; ++i) {
    const stmt_t *candidate = ny_native_fn_cache.funcs[i];
    if (candidate && candidate->as.fn.name &&
        strcmp(candidate->as.fn.name, surface) == 0 &&
        !candidate->as.fn.is_extern && !candidate->as.fn.link_name &&
        !ny_is_stdlib_tok(candidate->tok))
      return true;
  }
  return false;
}

static bool ny_native_nir_user_defined_fn(ny_native_nir_builder_t *b,
                                          const char *name) {
  if (!b || !b->prog || !name)
    return false;
  uint32_t h = 2166136261u;
  for (const char *p = name; *p; ++p)
    h = (h ^ (uint8_t)*p) * 16777619u;
  uint32_t idx = h % NY_USER_FN_CACHE_SIZE;
  if (ny_user_fn_cache[idx].prog == b->prog &&
      strcmp(ny_user_fn_cache[idx].name, name) == 0)
    return ny_user_fn_cache[idx].result;

  bool res = ny_native_nir_user_defined_fn_uncached(b, name);
  ny_user_fn_cache[idx].prog = b->prog;
  snprintf(ny_user_fn_cache[idx].name, sizeof(ny_user_fn_cache[idx].name), "%s",
           name);
  ny_user_fn_cache[idx].result = res;
  return res;
}

static const expr_t *
ny_native_nir_find_top_level_value(const ny_native_nir_builder_t *b,
                                   const char *name) {
  if (!b || !b->prog || !name)
    return NULL;
  const expr_t *fallback = NULL;
  for (size_t i = 0; i < b->prog->body.len; ++i) {
    const expr_t *found = ny_native_nir_find_top_level_value_in_stmt(
        b->prog->body.data[i], name, 0);
    if (found) {
      /*
       * Imported modules can contain same-named #main/self-test bindings.
       * Prefer the declaration from the source currently being lowered so a
       * user binding such as `c` cannot resolve to std.core.collections.c.
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
      for (size_t i = 0; i < b->prog->body.len; ++i) {
        const expr_t *found = ny_native_nir_find_top_level_value_in_stmt(
            b->prog->body.data[i], qualified, 0);
        if (found)
          return found;
      }
    }
  }
  if (fallback)
    return fallback;
  return ny_native_nir_find_imported_value(b, name);
}

static const expr_t *
ny_native_nir_find_top_level_value_in_source(const ny_native_nir_builder_t *b,
                                             const char *name,
                                             const char *source_file) {
  if (!b || !b->prog || !name || !source_file)
    return NULL;
  for (size_t i = 0; i < b->prog->body.len; ++i) {
    const expr_t *found = ny_native_nir_find_top_level_value_in_stmt(
        b->prog->body.data[i], name, 0);
    if (found && found->tok.filename &&
        strcmp(found->tok.filename, source_file) == 0)
      return found;
  }
  return NULL;
}

/*
 * Global data symbols use raw 8-byte storage. Preserve an f64 value's
 * representation at that symbol boundary so machine lowering selects XMM
 * loads/stores instead of treating the bits as an integer.
 */
static bool ny_native_nir_global_is_f64(const ny_native_nir_builder_t *b,
                                        const char *name,
                                        const expr_t *reference) {
  if (!b || !name)
    return false;
  const expr_t *value = NULL;
  if (reference && reference->tok.filename)
    value = ny_native_nir_find_top_level_value_in_source(
        b, name, reference->tok.filename);
  if (!value)
    value = ny_native_nir_find_top_level_value(b, name);
  if (!value) {
    const char *leaf = strrchr(name, '.');
    if (leaf && leaf[1]) {
      if (reference && reference->tok.filename)
        value = ny_native_nir_find_top_level_value_in_source(
            b, leaf + 1, reference->tok.filename);
      if (!value)
        value = ny_native_nir_find_top_level_value(b, leaf + 1);
    }
  }
  return value &&
         ny_native_nir_expr_is_f64((ny_native_nir_builder_t *)b, value);
}

/*
 * Recurse into container statements to find top-level variable values
 * nested inside modules/blocks (e.g. stdlib constants).
 */
static const expr_t *
ny_native_nir_find_top_level_value_in_stmt(const stmt_t *s, const char *name,
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
      const expr_t *found = ny_native_nir_find_top_level_value_in_stmt(
          s->as.module.body.data[i], name, depth + 1);
      if (found)
        return found;
    }
    return NULL;
  }
  if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; ++i) {
      const expr_t *found = ny_native_nir_find_top_level_value_in_stmt(
          s->as.block.body.data[i], name, depth + 1);
      if (found)
        return found;
    }
    return NULL;
  }
  if (s->kind == NY_S_IF) {
    bool selected = false;
    if (s->as.iff.test && s->as.iff.test->kind == NY_E_COMPTIME &&
        ny_native_target_eval_bool(NULL, s->as.iff.test, &selected)) {
      return ny_native_nir_find_top_level_value_in_stmt(
          selected ? s->as.iff.conseq : s->as.iff.alt, name, depth + 1);
    }
    const expr_t *found = ny_native_nir_find_top_level_value_in_stmt(
        s->as.iff.conseq, name, depth + 1);
    return found ? found
                 : ny_native_nir_find_top_level_value_in_stmt(s->as.iff.alt,
                                                              name, depth + 1);
  }
  return NULL;
}

/*
 * Resolve a `use module as alias` declaration (recursively, so aliases
 * declared inside modules resolve too) to the module path.  Returns NULL
 * when no use statement declares the alias.
 */
static bool ny_native_nir_same_source_file(const char *a, const char *b) {
  if (!a || !b)
    return true;
  if (strcmp(a, b) == 0)
    return true;
  const char *base_a = strrchr(a, '/');
  base_a = base_a ? base_a + 1 : a;
  const char *base_b = strrchr(b, '/');
  base_b = base_b ? base_b + 1 : b;
  return strcmp(base_a, base_b) == 0;
}

/*
 * True when `filename` ends with the relative import `want` at a path
 * boundary (`.../modules/use/declared.ny` ends with `use/declared.ny`).
 */
static bool ny_native_nir_path_ends_with(const char *filename,
                                         const char *want) {
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
static const char *ny_native_nir_module_name_for_use(const stmt_t *s,
                                                     const char *basename,
                                                     const char *rel,
                                                     unsigned depth) {
  if (!s || depth > 64)
    return NULL;
  if (s->kind == NY_S_MODULE) {
    if (s->as.module.name) {
      if (basename && strcmp(s->as.module.name, basename) == 0)
        return s->as.module.name;
      if (rel && *rel && s->tok.filename &&
          ny_native_nir_path_ends_with(s->tok.filename, rel))
        return s->as.module.name;
    }
    for (size_t i = 0; i < s->as.module.body.len; ++i) {
      const char *found = ny_native_nir_module_name_for_use(
          s->as.module.body.data[i], basename, rel, depth + 1);
      if (found)
        return found;
    }
    return NULL;
  }
  if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; ++i) {
      const char *found = ny_native_nir_module_name_for_use(
          s->as.block.body.data[i], basename, rel, depth + 1);
      if (found)
        return found;
    }
  }
  return NULL;
}

/*
 * Canonicalize the module prefix of a local file import to the imported
 * module's declared name.  Raw dotted package names (`std.math.float`) are
 * already canonical and pass through untouched.  A trailing member suffix
 * (`./sub.ny.member`, produced for a renamed/selective import item) is kept
 * after canonicalizing the path part.  Returns an AST- or buffer-owned string
 * valid until the next call.
 */
static const char *
ny_native_nir_use_module_canonical(const ny_native_nir_builder_t *b,
                                   const char *raw) {
  if (!b || !b->prog || !raw || !*raw || !strchr(raw, '/'))
    return raw;
  char *normalized = normalize_module_name(raw);
  const char *rel = raw;
  while (rel[0] == '.' && rel[1] == '/')
    rel += 2;
  const char *name = NULL;
  for (size_t i = 0; i < b->prog->body.len && !name; ++i)
    name = ny_native_nir_module_name_for_use(b->prog->body.data[i], normalized,
                                             rel, 0);
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
      char prefix[512];
      memcpy(prefix, raw, prefix_len);
      prefix[prefix_len] = '\0';
      normalized = normalize_module_name(prefix);
      rel = prefix;
      while (rel[0] == '.' && rel[1] == '/')
        rel += 2;
      const char *mod_name = NULL;
      for (size_t i = 0; i < b->prog->body.len && !mod_name; ++i)
        mod_name = ny_native_nir_module_name_for_use(b->prog->body.data[i],
                                                     normalized, rel, 0);
      free(normalized);
      if (mod_name) {
        static char combined[8][640];
        static size_t next_combined = 0;
        char *out = combined[next_combined++ % 8];
        int n = snprintf(out, sizeof(combined[0]), "%s.%s", mod_name, dot + 1);
        if (n > 0 && (size_t)n < sizeof(combined[0]))
          return out;
      }
    }
  }
  return raw;
}

static const char *ny_native_nir_use_alias_in_stmt(const stmt_t *s,
                                                   const char *alias,
                                                   unsigned depth,
                                                   const char *source_file) {
  if (!s || !alias || depth > 64)
    return NULL;
  if (s->kind == NY_S_USE && source_file && s->tok.filename &&
      !ny_native_nir_same_source_file(source_file, s->tok.filename))
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
      static char imported[512];
      int n = snprintf(imported, sizeof(imported), "%s.%s", s->as.use.module,
                       item->name);
      return n > 0 && (size_t)n < sizeof(imported) ? imported : NULL;
    }
    return NULL;
  }
  if (s->kind == NY_S_MODULE) {
    for (size_t i = 0; i < s->as.module.body.len; ++i) {
      const char *found = ny_native_nir_use_alias_in_stmt(
          s->as.module.body.data[i], alias, depth + 1, source_file);
      if (found)
        return found;
    }
  }
  if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; ++i) {
      const char *found = ny_native_nir_use_alias_in_stmt(
          s->as.block.body.data[i], alias, depth + 1, source_file);
      if (found)
        return found;
    }
  }
  return NULL;
}

static const char *
ny_native_nir_resolve_use_alias(const ny_native_nir_builder_t *b,
                                const char *alias) {
  if (!b || !b->prog || !alias)
    return NULL;
  /*
   * A parameter/local shadows every imported module basename.  The expanded
   * stdlib can contain an unrelated `use ... as f` while a method in another
   * module legitimately uses `f` as its receiver; treating that local as a
   * module alias rewrites `f.evaluate(...)` into a non-existent qualified
   * symbol such as `std.math.float.evaluate`.
   */
  if (ny_native_nir_find_local((ny_native_nir_builder_t *)b, alias))
    return NULL;
  if (b->module_name && *b->module_name) {
    const stmt_t *owner = NULL;
    for (size_t i = 0; i < b->prog->body.len && !owner; ++i)
      owner =
          ny_native_nir_find_module(b->prog->body.data[i], b->module_name, 0);
    if (owner && owner->kind == NY_S_MODULE) {
      for (size_t i = 0; i < owner->as.module.body.len; ++i) {
        const stmt_t *use = owner->as.module.body.data[i];
        if (!use || use->kind != NY_S_USE ||
            (!use->as.use.alias && use->as.use.imports.len == 0))
          continue;
        const char *found =
            ny_native_nir_use_alias_in_stmt(use, alias, 0, NULL);
        if (found)
          return ny_native_nir_use_module_canonical(b, found);
      }
      for (size_t i = 0; i < owner->as.module.body.len; ++i) {
        const stmt_t *use = owner->as.module.body.data[i];
        if (!use || use->kind != NY_S_USE || use->as.use.alias ||
            use->as.use.imports.len != 0)
          continue;
        const char *found =
            ny_native_nir_use_alias_in_stmt(use, alias, 0, NULL);
        if (found)
          return ny_native_nir_use_module_canonical(b, found);
        if (use->as.use.module) {
          char prefix[512];
          int pn = snprintf(prefix, sizeof(prefix), "%s.", use->as.use.module);
          if (pn > 0 && (size_t)pn < sizeof(prefix)) {
            for (size_t j = 0; j < b->prog->body.len; ++j) {
              const stmt_t *m = ny_native_nir_find_nested_module(
                  b->prog->body.data[j], prefix, alias, 0);
              if (m && m->as.module.name) {
                static char out[8][512];
                static size_t next_out = 0;
                char *res = out[next_out++ % 8];
                snprintf(res, sizeof(out[0]), "%s", m->as.module.name);
                return res;
              }
            }
          }
        }
      }
    }
  }
  for (size_t i = 0; i < b->prog->body.len; ++i) {
    const stmt_t *stmt = b->prog->body.data[i];
    if (!stmt || stmt->kind != NY_S_USE)
      continue;
    const char *found =
        ny_native_nir_use_alias_in_stmt(stmt, alias, 0, b->source_file);
    if (found)
      return ny_native_nir_use_module_canonical(b, found);
  }
  for (size_t i = 0; i < b->prog->body.len; ++i) {
    const char *found = ny_native_nir_use_alias_in_stmt(
        b->prog->body.data[i], alias, 0, b->source_file);
    if (found)
      return ny_native_nir_use_module_canonical(b, found);
  }
  for (size_t i = 0; i < b->prog->body.len; ++i) {
    const stmt_t *stmt = b->prog->body.data[i];
    if (!stmt || stmt->kind != NY_S_USE || !stmt->as.use.module)
      continue;
    if (stmt->as.use.alias || stmt->as.use.imports.len != 0)
      continue;
    if (b->source_file && stmt->tok.filename &&
        !ny_native_nir_same_source_file(b->source_file, stmt->tok.filename))
      continue;
    char candidate[512];
    int n = snprintf(candidate, sizeof(candidate), "%s.%s", stmt->as.use.module,
                     alias);
    if (n > 0 && (size_t)n < sizeof(candidate)) {
      for (size_t j = 0; j < b->prog->body.len; ++j) {
        const stmt_t *m =
            ny_native_nir_find_module(b->prog->body.data[j], candidate, 0);
        if (m) {
          static char out[8][512];
          static size_t next_out = 0;
          char *res = out[next_out++ % 8];
          snprintf(res, sizeof(out[0]), "%s", candidate);
          return res;
        }
      }
    }
    char prefix[512];
    int pn = snprintf(prefix, sizeof(prefix), "%s.", stmt->as.use.module);
    if (pn > 0 && (size_t)pn < sizeof(prefix)) {
      for (size_t j = 0; j < b->prog->body.len; ++j) {
        const stmt_t *m = ny_native_nir_find_nested_module(
            b->prog->body.data[j], prefix, alias, 0);
        if (m && m->as.module.name) {
          static char out[8][512];
          static size_t next_out = 0;
          char *res = out[next_out++ % 8];
          snprintf(res, sizeof(out[0]), "%s", m->as.module.name);
          return res;
        }
      }
    }
  }
  return NULL;
}

static const stmt_t *
ny_native_nir_find_module(const stmt_t *s, const char *name, unsigned depth) {
  if (!s || !name || depth > 64)
    return NULL;
  if (s->kind == NY_S_MODULE) {
    if (s->as.module.name && ny_native_name_matches(s->as.module.name, name))
      return s;
    for (size_t i = 0; i < s->as.module.body.len; ++i) {
      const stmt_t *found =
          ny_native_nir_find_module(s->as.module.body.data[i], name, depth + 1);
      if (found)
        return found;
    }
  } else if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; ++i) {
      const stmt_t *found =
          ny_native_nir_find_module(s->as.block.body.data[i], name, depth + 1);
      if (found)
        return found;
    }
  }
  return NULL;
}

static const stmt_t *ny_native_nir_find_nested_module(const stmt_t *s,
                                                      const char *prefix,
                                                      const char *leaf,
                                                      unsigned depth) {
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
      const stmt_t *found = ny_native_nir_find_nested_module(
          s->as.module.body.data[i], prefix, leaf, depth + 1);
      if (found)
        return found;
    }
  } else if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; ++i) {
      const stmt_t *found = ny_native_nir_find_nested_module(
          s->as.block.body.data[i], prefix, leaf, depth + 1);
      if (found)
        return found;
    }
  }
  return NULL;
}

static const expr_t *ny_native_nir_find_module_member_in(const stmt_t *mod,
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
      const expr_t *found = ny_native_nir_find_module_member_in(
          mod->as.module.body.data[i], member, depth + 1);
      if (found)
        return found;
    }
    return NULL;
  }
  if (mod->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < mod->as.block.body.len; ++i) {
      const expr_t *found = ny_native_nir_find_module_member_in(
          mod->as.block.body.data[i], member, depth + 1);
      if (found)
        return found;
    }
  }
  return NULL;
}

/*
 * Resolve a constant re-exported through a module's use declarations.
 */
static const expr_t *ny_native_nir_find_imported_module_member_rec(
    const ny_native_nir_builder_t *b, const stmt_t *mod, const char *member,
    unsigned depth, const stmt_t **visited, size_t *visited_len,
    size_t visited_cap) {
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
    const stmt_t *imported_mod = NULL;
    for (size_t r = 0; r < b->prog->body.len; ++r) {
      imported_mod =
          ny_native_nir_find_module(b->prog->body.data[r], imported_name, 0);
      if (imported_mod)
        break;
    }
    const expr_t *direct =
        ny_native_nir_find_module_member_in(imported_mod, target_member, 0);
    if (direct)
      return direct;
    char qualified[1024];
    int qn = snprintf(qualified, sizeof(qualified), "%s.%s", imported_name,
                      target_member);
    if (qn > 0 && (size_t)qn < sizeof(qualified)) {
      const expr_t *v = ny_native_nir_find_top_level_value(b, qualified);
      if (v)
        return v;
    }
    /*
     * Transitive: the imported module itself re-exports the constant.
     */
    for (size_t r = 0; r < b->prog->body.len; ++r) {
      const stmt_t *imported =
          ny_native_nir_find_module(b->prog->body.data[r], imported_name, 0);
      if (!imported || imported == mod)
        continue;
      const expr_t *value = ny_native_nir_find_imported_module_member_rec(
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
ny_native_nir_find_imported_module_member(const ny_native_nir_builder_t *b,
                                          const stmt_t *mod, const char *member,
                                          unsigned depth) {
  const stmt_t *visited[256];
  size_t visited_len = 0;
  return ny_native_nir_find_imported_module_member_rec(
      b, mod, member, depth, visited, &visited_len, 256);
}

static const expr_t *
ny_native_nir_find_imported_value(const ny_native_nir_builder_t *b,
                                  const char *name) {
  if (!b || !b->prog || !name)
    return NULL;

  if (b->module_name && b->module_name[0]) {
    const stmt_t *mod = NULL;
    for (size_t i = 0; i < b->prog->body.len && !mod; ++i)
      mod = ny_native_nir_find_module(b->prog->body.data[i], b->module_name, 0);
    if (mod && mod->kind == NY_S_MODULE) {
      for (size_t i = 0; i < mod->as.module.body.len; ++i) {
        const stmt_t *use = mod->as.module.body.data[i];
        if (!use || use->kind != NY_S_USE || !use->as.use.module)
          continue;
        const char *mod_name =
            ny_native_nir_use_module_canonical(b, use->as.use.module);
        const stmt_t *target_mod = NULL;
        for (size_t j = 0; j < b->prog->body.len && !target_mod; ++j)
          target_mod =
              ny_native_nir_find_module(b->prog->body.data[j], mod_name, 0);
        if (!target_mod)
          continue;
        for (size_t j = 0; j < use->as.use.imports.len; ++j) {
          const use_item_t *item = &use->as.use.imports.data[j];
          const char *visible = item->alias ? item->alias : item->name;
          if (!visible || strcmp(visible, name) != 0 || !item->name)
            continue;
          const expr_t *val =
              ny_native_nir_find_module_member_in(target_mod, item->name, 0);
          if (!val)
            val = ny_native_nir_find_imported_module_member(b, target_mod,
                                                            item->name, 0);
          if (val)
            return val;
        }
        if (use->as.use.import_all) {
          const expr_t *val =
              ny_native_nir_find_module_member_in(target_mod, name, 0);
          if (!val)
            val = ny_native_nir_find_imported_module_member(b, target_mod, name,
                                                            0);
          if (val)
            return val;
        }
      }
    }
  }

  /*
   * Root programs keep top-level `use std.math` declarations directly in the
   * program body (rather than inside a module block).  Resolve imported
   * constants from those declarations as well as module-local uses.
   */
  for (size_t i = 0; i < b->prog->body.len; ++i) {
    const stmt_t *use = b->prog->body.data[i];
    if (!use || use->kind != NY_S_USE || !use->as.use.module)
      continue;
    const char *mod_name =
        ny_native_nir_use_module_canonical(b, use->as.use.module);
    const stmt_t *target_mod = NULL;
    for (size_t j = 0; j < b->prog->body.len && !target_mod; ++j)
      target_mod =
          ny_native_nir_find_module(b->prog->body.data[j], mod_name, 0);
    if (!target_mod)
      continue;
    if (use->as.use.imports.len == 0 && !use->as.use.alias &&
        !use->as.use.import_all) {
      const expr_t *val =
          ny_native_nir_find_module_member_in(target_mod, name, 0);
      if (val)
        return val;
    }
    for (size_t j = 0; j < use->as.use.imports.len; ++j) {
      const use_item_t *item = &use->as.use.imports.data[j];
      const char *visible = item->alias ? item->alias : item->name;
      if (visible && item->name && strcmp(visible, name) == 0) {
        const expr_t *val =
            ny_native_nir_find_module_member_in(target_mod, item->name, 0);
        if (!val)
          val = ny_native_nir_find_imported_module_member(b, target_mod,
                                                          item->name, 0);
        if (val)
          return val;
      }
    }
    if (use->as.use.import_all) {
      const expr_t *val =
          ny_native_nir_find_module_member_in(target_mod, name, 0);
      if (!val)
        val = ny_native_nir_find_imported_module_member(b, target_mod, name, 0);
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
            !ny_native_nir_same_source_file(use->tok.filename, b->source_file))
          continue;
        const char *mod_name =
            ny_native_nir_use_module_canonical(b, use->as.use.module);
        const stmt_t *target_mod = NULL;
        for (size_t j = 0; j < b->prog->body.len && !target_mod; ++j)
          target_mod =
              ny_native_nir_find_module(b->prog->body.data[j], mod_name, 0);
        if (!target_mod)
          continue;
        for (size_t j = 0; j < use->as.use.imports.len; ++j) {
          const use_item_t *item = &use->as.use.imports.data[j];
          const char *visible = item->alias ? item->alias : item->name;
          if (!visible || strcmp(visible, name) != 0 || !item->name)
            continue;
          const expr_t *val =
              ny_native_nir_find_module_member_in(target_mod, item->name, 0);
          if (!val)
            val = ny_native_nir_find_imported_module_member(b, target_mod,
                                                            item->name, 0);
          if (val)
            return val;
        }
        if (use->as.use.import_all) {
          const expr_t *val =
              ny_native_nir_find_module_member_in(target_mod, name, 0);
          if (!val)
            val = ny_native_nir_find_imported_module_member(b, target_mod, name,
                                                            0);
          if (val)
            return val;
        }
      }
    } else if (s->kind == NY_S_USE && s->as.use.module) {
      const stmt_t *use = s;
      if (b->source_file && use->tok.filename &&
          !ny_native_nir_same_source_file(use->tok.filename, b->source_file))
        continue;
      const char *mod_name =
          ny_native_nir_use_module_canonical(b, use->as.use.module);
      const stmt_t *target_mod = NULL;
      for (size_t j = 0; j < b->prog->body.len && !target_mod; ++j)
        target_mod =
            ny_native_nir_find_module(b->prog->body.data[j], mod_name, 0);
      if (!target_mod)
        continue;
      for (size_t j = 0; j < use->as.use.imports.len; ++j) {
        const use_item_t *item = &use->as.use.imports.data[j];
        const char *visible = item->alias ? item->alias : item->name;
        if (!visible || strcmp(visible, name) != 0 || !item->name)
          continue;
        const expr_t *val =
            ny_native_nir_find_module_member_in(target_mod, item->name, 0);
        if (!val)
          val = ny_native_nir_find_imported_module_member(b, target_mod,
                                                          item->name, 0);
        if (val)
          return val;
      }
      if (use->as.use.import_all) {
        const expr_t *val =
            ny_native_nir_find_module_member_in(target_mod, name, 0);
        if (!val)
          val =
              ny_native_nir_find_imported_module_member(b, target_mod, name, 0);
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
static const expr_t *
ny_native_nir_resolve_member_expr(const ny_native_nir_builder_t *b,
                                  const expr_t *e) {
  if (!b || !e || e->kind != NY_E_MEMBER || !e->as.member.target ||
      !e->as.member.name)
    return NULL;

  /*
   * Flatten a (possibly nested) dotted member chain such as
   * `std.math.big._TAG_LIST` into a root identifier plus its path
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
  const char *resolved = ny_native_nir_resolve_use_alias(b, root);
  if (!resolved) {
    ny_native_nir_builder_t probe = *b;
    probe.source_file = NULL;
    resolved = ny_native_nir_resolve_use_alias(&probe, root);
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
        ny_native_nir_find_module(b->prog->body.data[i], module, 0);
    const expr_t *v = ny_native_nir_find_module_member_in(mod, path[0], 0);
    if (!v)
      v = ny_native_nir_find_imported_module_member(b, mod, path[0], 0);
    if (v)
      return v;
  }

  char qualified[1024];
  int n = snprintf(qualified, sizeof(qualified), "%s.%s", module, path[0]);
  if (n > 0 && (size_t)n < sizeof(qualified)) {
    const expr_t *v = ny_native_nir_find_top_level_value(b, qualified);
    if (v)
      return v;
  }
  return ny_native_nir_find_top_level_value(b, path[0]);
}

/*
 * Resolve an expression to a constant list literal when it is one directly,
 * via a top-level def, or via a member access.  Used for .len folding and
 * index element-type detection.
 */
static bool ny_native_nir_expr_is_list(const ny_native_nir_builder_t *b,
                                       const expr_t *e) {
  if (!b || !e)
    return false;
  /*
   * Indexing yields an element, never the container's representation.  The
   * semantic pass may leave the container's typed-buffer rep on the index
   * node; trusting it makes `xs[i] + ys[i]` look like list concatenation.
   */
  if (e->kind == NY_E_INDEX)
    return false;
  if (e->kind == NY_E_CALL) {
    const char *leaf = ny_native_call_leaf(e);
    if (leaf && strcmp(leaf, "get") == 0)
      return false;
  }
  if (e->semantic.resolved && e->semantic.rep == NY_SEM_REP_TYPED_BUFFER)
    return true;
  if (e->kind == NY_E_LIST)
    return true;
  if (e->kind == NY_E_BINARY && e->as.binary.op &&
      strcmp(e->as.binary.op, "*") == 0) {
    bool left_list = ny_native_nir_expr_is_list(b, e->as.binary.left);
    bool right_list = ny_native_nir_expr_is_list(b, e->as.binary.right);
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
      return ny_native_nir_expr_is_list(b, e->as.call.args.data[0].val);
    if (leaf &&
        (strcmp(leaf, "sort") == 0 || strcmp(leaf, "sorted") == 0 ||
         strcmp(leaf, "reverse") == 0 || strcmp(leaf, "clone") == 0) &&
        e->as.call.args.len >= 1)
      return ny_native_nir_expr_is_list(b, e->as.call.args.data[0].val);
    const char *call_name =
        e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT
            ? e->as.call.callee->as.ident.name
            : leaf;
    const stmt_t *fn = call_name ? ny_native_nir_find_user_function(
                                       (ny_native_nir_builder_t *)b, call_name)
                                 : NULL;
    if (!fn && leaf)
      fn = ny_native_nir_find_user_function((ny_native_nir_builder_t *)b, leaf);
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
                               strcmp(e->as.memcall.name, "clear") == 0 ||
                               strcmp(e->as.memcall.name, "clone") == 0))
      return true;
    if (e->as.memcall.name && strcmp(e->as.memcall.name, "get") == 0 &&
        e->as.memcall.args.len == 2)
      return ny_native_nir_expr_is_list(b, e->as.memcall.args.data[1].val);
    const stmt_t *fn =
        e->as.memcall.name
            ? ny_native_nir_find_user_function((ny_native_nir_builder_t *)b,
                                               e->as.memcall.name)
            : NULL;
    return fn && ny_native_type_name_is_list(fn->as.fn.return_type);
  }
  if (e->kind == NY_E_MEMBER) {
    const expr_t *global = ny_native_nir_resolve_member_expr(b, e);
    return global && global != e && ny_native_nir_expr_is_list(b, global);
  }
  if (e->kind == NY_E_IDENT) {
    ny_native_nir_local_t *local = ny_native_nir_find_local(
        (ny_native_nir_builder_t *)b, e->as.ident.name);
    if (local)
      return local->semantic_rep == NY_SEM_REP_TYPED_BUFFER || local->is_list;
    const expr_t *global =
        ny_native_nir_find_top_level_value(b, e->as.ident.name);
    if (global && global != e && global->kind != NY_E_IDENT)
      return ny_native_nir_expr_is_list(b, global);
    return false;
  }
  return false;
}

/*
 * Bytes share the string semantic representation, but their methods have a
 * different native ABI. Keep the distinction explicit so `b.get`/`b.set`
 * cannot fall through to the generic dictionary dispatcher.
 */
static bool ny_native_nir_expr_is_bytes(const ny_native_nir_builder_t *b,
                                        const expr_t *e) {
  if (!b || !e)
    return false;
  if (e->kind == NY_E_CALL) {
    const char *leaf = ny_native_call_leaf(e);
    if (leaf &&
        (strcmp(leaf, "bytes") == 0 || strcmp(leaf, "__bytes_new") == 0 ||
         strcmp(leaf, "bytes_new_raw") == 0))
      return true;
    const stmt_t *fn = leaf ? ny_native_nir_find_user_function(
                                  (ny_native_nir_builder_t *)b, leaf)
                            : NULL;
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
             ny_native_nir_expr_is_bytes(b, e->as.memcall.target);
    const stmt_t *fn =
        e->as.memcall.name
            ? ny_native_nir_find_user_function((ny_native_nir_builder_t *)b,
                                               e->as.memcall.name)
            : NULL;
    return fn && ny_native_type_name_is_bytes(fn->as.fn.return_type);
  }
  if (e->kind == NY_E_IDENT) {
    ny_native_nir_local_t *local = ny_native_nir_find_local(
        (ny_native_nir_builder_t *)b, e->as.ident.name);
    if (local)
      return local->is_bytes;
    const expr_t *global =
        ny_native_nir_find_top_level_value(b, e->as.ident.name);
    return global && global != e && global->kind != NY_E_IDENT &&
           ny_native_nir_expr_is_bytes(b, global);
  }
  if (e->kind == NY_E_MEMBER) {
    const expr_t *global = ny_native_nir_resolve_member_expr(b, e);
    return global && global != e && ny_native_nir_expr_is_bytes(b, global);
  }
  return false;
}

static bool ny_native_nir_expr_is_dict(const ny_native_nir_builder_t *b,
                                       const expr_t *e) {
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
  if (e->semantic.resolved && e->semantic.rep == NY_SEM_REP_OBJECT)
    return true;
  if (e->kind == NY_E_DICT)
    return true;
  if (e->kind == NY_E_CALL) {
    const char *leaf = ny_native_call_leaf(e);
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
      return ny_native_nir_expr_is_dict(b, e->as.call.args.data[0].val);
    const stmt_t *fn = leaf ? ny_native_nir_find_user_function(
                                  (ny_native_nir_builder_t *)b, leaf)
                            : NULL;
    return fn && fn->as.fn.return_type &&
           strcmp(fn->as.fn.return_type, "dict") == 0;
  }
  if (e->kind == NY_E_MEMCALL) {
    const stmt_t *fn =
        e->as.memcall.name
            ? ny_native_nir_find_user_function((ny_native_nir_builder_t *)b,
                                               e->as.memcall.name)
            : NULL;
    if (fn && fn->as.fn.return_type &&
        strcmp(fn->as.fn.return_type, "dict") == 0)
      return true;
    const expr_t *global = ny_native_nir_resolve_member_expr(b, e);
    return global && global != e && ny_native_nir_expr_is_dict(b, global);
  }
  if (e->kind == NY_E_IDENT) {
    ny_native_nir_local_t *local = ny_native_nir_find_local(
        (ny_native_nir_builder_t *)b, e->as.ident.name);
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
    const expr_t *global =
        ny_native_nir_find_top_level_value(b, e->as.ident.name);
    if (global && global != e && global->kind != NY_E_IDENT)
      return ny_native_nir_expr_is_dict(b, global);
  }
  return false;
}

static const expr_t *
ny_native_nir_resolve_list_literal(const ny_native_nir_builder_t *b,
                                   const expr_t *e, unsigned depth) {
  if (!b || !e || depth > 64)
    return NULL;
  if (e->kind == NY_E_LIST)
    return e;
  if (e->kind == NY_E_IDENT) {
    ny_native_nir_local_t *local = ny_native_nir_find_local(
        (ny_native_nir_builder_t *)b, e->as.ident.name);
    if (local && !local->semantic_mutable && local->list_literal)
      return local->list_literal;
    const expr_t *v = ny_native_nir_find_top_level_value(b, e->as.ident.name);
    if (v && v != e)
      return ny_native_nir_resolve_list_literal(b, v, depth + 1);
    return NULL;
  }
  if (e->kind == NY_E_MEMBER) {
    const expr_t *v = ny_native_nir_resolve_member_expr(b, e);
    if (v && v != e)
      return ny_native_nir_resolve_list_literal(b, v, depth + 1);
    return NULL;
  }
  return NULL;
}

/*
 * Fold a top-level def initializer to a raw i64 when it is a compile-time
 * constant (literals, references to other top-level defs, integer unary and
 * binary ops).  Used to register the def's value in the consttab so object
 * emission writes an 8-byte .data definition for the symbol.
 */
static bool ny_native_nir_fold_top_level_int(const program_t *prog,
                                             const expr_t *e, int64_t *out,
                                             unsigned depth) {
  if (!e || !out || depth > 64)
    return false;
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
    if (!prog || !e->as.ident.name)
      return false;
    for (size_t i = 0; i < prog->body.len; ++i) {
      const expr_t *val = ny_native_nir_find_top_level_value_in_stmt(
          prog->body.data[i], e->as.ident.name, 0);
      if (val && val != e)
        return ny_native_nir_fold_top_level_int(prog, val, out, depth + 1);
    }
    return false;
  }
  case NY_E_UNARY: {
    if (!e->as.unary.op || !e->as.unary.right)
      return false;
    int64_t v = 0;
    if (!ny_native_nir_fold_top_level_int(prog, e->as.unary.right, &v,
                                          depth + 1))
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
    if (strcmp(e->as.binary.op, "^") == 0)
      return ny_native_nir_fold_const_pow(e->as.binary.left, e->as.binary.right,
                                          out);
    int64_t l = 0, r = 0;
    if (!ny_native_nir_fold_top_level_int(prog, e->as.binary.left, &l,
                                          depth + 1) ||
        !ny_native_nir_fold_top_level_int(prog, e->as.binary.right, &r,
                                          depth + 1))
      return false;
    nyir_op_t op;
    if (!ny_native_nir_binop(e->as.binary.op, &op))
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
  default:
    return false;
  }
}

/*
 * Fold a call to __runtime_tag("name") / runtime_tag_raw("name") with a
 * string-literal argument to its tag constant.  Shared by the call-site
 * lowering and top-level consttab registration.
 */
static bool ny_native_nir_fold_runtime_tag(const expr_t *call, int64_t *out) {
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
 * path. It intentionally accepts only side-effect-free integer/boolean
 * expressions and immutable top-level values; unknown expressions remain
 * unknown instead of being guessed true.
 */
static bool ny_native_nir_eval_proof_i64(const ny_native_nir_builder_t *b,
                                         const expr_t *e, unsigned depth,
                                         int64_t *out) {
  if (!b || !e || !out || depth > 64)
    return false;
  if (e->kind == NY_E_LITERAL) {
    if (e->as.literal.kind == NY_LIT_INT) {
      *out = e->as.literal.as.i;
      return true;
    }
    if (e->as.literal.kind == NY_LIT_BOOL) {
      *out = e->as.literal.as.b ? 1 : 0;
      return true;
    }
    return false;
  }
  if (e->kind == NY_E_IDENT && e->as.ident.name) {
    const expr_t *value =
        ny_native_nir_find_top_level_value(b, e->as.ident.name);
    return value && value != e &&
           ny_native_nir_eval_proof_i64(b, value, depth + 1, out);
  }
  if (e->kind == NY_E_UNARY && e->as.unary.op) {
    int64_t value = 0;
    if (!ny_native_nir_eval_proof_i64(b, e->as.unary.right, depth + 1, &value))
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
  int64_t a = 0, c = 0;
  if (!ny_native_nir_eval_proof_i64(b, left, depth + 1, &a) ||
      !ny_native_nir_eval_proof_i64(b, right, depth + 1, &c))
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

/*
 * Register immutable constants and mutable module storage before lowering.
 */
static void ny_native_register_const_defs(const program_t *prog,
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
            ny_native_consttab_add(qualified, (int64_t)parsed);
        } else {
          ny_native_consttab_add(s->as.def.name, (int64_t)parsed);
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
          ny_native_globaltab_add(name);
        } else {
          size_t module_len = strlen(module_name);
          if (strncmp(name, module_name, module_len) == 0 &&
              name[module_len] == '.')
            ny_native_globaltab_add(name);
          else {
            char qualified[512];
            int n = snprintf(qualified, sizeof(qualified), "%s.%s", module_name,
                             name);
            if (n > 0 && (size_t)n < sizeof(qualified))
              ny_native_globaltab_add(qualified);
          }
        }
      }
      if (!init)
        continue;
      int64_t value = 0;
      bool folded = ny_native_nir_fold_top_level_int(prog, init, &value, 0);
      bool is_tag = false;
      if (!folded && init->kind == NY_E_LITERAL &&
          init->as.literal.kind == NY_LIT_FLOAT) {
        value = ny_native_f64_bits(init->as.literal.as.f);
        folded = true;
      }
      if (!folded && ny_native_nir_fold_runtime_tag(init, &value)) {
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
              ny_native_consttab_add(qualified, value);
          } else {
            ny_native_consttab_add(name, value);
          }
        }
        if (is_tag) {
          const char *leaf = strrchr(name, '.');
          if (leaf && leaf[1])
            ny_native_consttab_add(leaf + 1, value);
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
      ny_native_register_const_defs(prog, s->as.module.body.data[i],
                                    child_module, opt);
    return;
  }
  if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      ny_native_register_const_defs(prog, s->as.block.body.data[i], module_name,
                                    opt);
    return;
  }
  if (s->kind == NY_S_IF) {
    bool selected = false;
    if (s->as.iff.test && s->as.iff.test->kind == NY_E_COMPTIME &&
        ny_native_target_eval_bool(opt, s->as.iff.test, &selected)) {
      ny_native_register_const_defs(
          prog, selected ? s->as.iff.conseq : s->as.iff.alt, module_name, opt);
      return;
    }
    ny_native_register_const_defs(prog, s->as.iff.conseq, module_name, opt);
    ny_native_register_const_defs(prog, s->as.iff.alt, module_name, opt);
  }
}

static ny_native_nir_local_t *
ny_native_nir_find_local_slot(ny_native_nir_builder_t *b, int slot) {
  if (!b)
    return NULL;
  for (size_t i = b->local_count; i > 0; --i)
    if (b->locals[i - 1].slot == slot)
      return &b->locals[i - 1];
  return NULL;
}

static bool ny_native_nir_record_fact(ny_native_nir_builder_t *b, int value,
                                      ny_native_nir_fact_kind_t kind,
                                      int64_t payload) {
  if (!b || value < 0 || payload <= 0)
    return true;
  if (b->fact_count == b->fact_cap) {
    size_t cap = b->fact_cap ? b->fact_cap * 2 : 16;
    if (cap < b->fact_cap || cap > SIZE_MAX / sizeof(*b->facts))
      return ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
    ny_native_nir_fact_t *facts = realloc(b->facts, cap * sizeof(*facts));
    if (!facts)
      return ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
    b->facts = facts;
    b->fact_cap = cap;
  }
  b->facts[b->fact_count++] =
      (ny_native_nir_fact_t){.value = value, .payload = payload, .kind = kind};
  return true;
}

static int64_t ny_native_nir_peek_fact(const ny_native_nir_builder_t *b,
                                       int value,
                                       ny_native_nir_fact_kind_t kind) {
  if (!b || value < 0)
    return 0;
  for (size_t i = b->fact_count; i > 0; --i) {
    size_t index = i - 1;
    if (b->facts[index].value == value && b->facts[index].kind == kind)
      return b->facts[index].payload;
  }
  return 0;
}

static int64_t ny_native_nir_take_fact(ny_native_nir_builder_t *b, int value,
                                       ny_native_nir_fact_kind_t kind) {
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

static bool ny_native_nir_record_alloc_fact(ny_native_nir_builder_t *b,
                                            int value, int64_t byte_len) {
  return ny_native_nir_record_fact(b, value, NY_NATIVE_NIR_FACT_ALLOC,
                                   byte_len);
}

static int64_t ny_native_nir_peek_alloc_fact(const ny_native_nir_builder_t *b,
                                             int value) {
  return ny_native_nir_peek_fact(b, value, NY_NATIVE_NIR_FACT_ALLOC);
}

static int64_t ny_native_nir_take_alloc_fact(ny_native_nir_builder_t *b,
                                             int value) {
  return ny_native_nir_take_fact(b, value, NY_NATIVE_NIR_FACT_ALLOC);
}

static bool ny_native_nir_record_fin_fact(ny_native_nir_builder_t *b, int value,
                                          int64_t bound) {
  return ny_native_nir_record_fact(b, value, NY_NATIVE_NIR_FACT_FIN, bound);
}

static int64_t ny_native_nir_take_fin_fact(ny_native_nir_builder_t *b,
                                           int value) {
  return ny_native_nir_take_fact(b, value, NY_NATIVE_NIR_FACT_FIN);
}

static bool ny_native_nir_record_list_len_fact(ny_native_nir_builder_t *b,
                                               int value, int length_value) {
  return length_value < 0 || length_value == INT32_MAX
             ? false
             : ny_native_nir_record_fact(b, value, NY_NATIVE_NIR_FACT_LIST_LEN,
                                         (int64_t)length_value + 1);
}

static int ny_native_nir_peek_list_len_fact(const ny_native_nir_builder_t *b,
                                            int value) {
  int64_t encoded =
      ny_native_nir_peek_fact(b, value, NY_NATIVE_NIR_FACT_LIST_LEN);
  return encoded > 0 ? (int)(encoded - 1) : -1;
}

static int ny_native_nir_take_list_len_fact(ny_native_nir_builder_t *b,
                                            int value) {
  int64_t encoded =
      ny_native_nir_take_fact(b, value, NY_NATIVE_NIR_FACT_LIST_LEN);
  return encoded > 0 ? (int)(encoded - 1) : -1;
}

static bool ny_native_nir_record_dyn_fact(ny_native_nir_builder_t *b, int value,
                                          ny_native_nir_fact_kind_t kind,
                                          int reg) {
  return reg >= 0 &&
         ny_native_nir_record_fact(b, value, kind, (int64_t)reg + 1);
}

static int ny_native_nir_peek_dyn_fact(const ny_native_nir_builder_t *b,
                                       int value,
                                       ny_native_nir_fact_kind_t kind) {
  int64_t encoded = ny_native_nir_peek_fact(b, value, kind);
  return encoded > 0 ? (int)(encoded - 1) : -1;
}

static int ny_native_nir_take_dyn_fact(ny_native_nir_builder_t *b, int value,
                                       ny_native_nir_fact_kind_t kind) {
  int64_t encoded = ny_native_nir_take_fact(b, value, kind);
  return encoded > 0 ? (int)(encoded - 1) : -1;
}

static int ny_native_nir_emit_known_list_append_len(ny_native_nir_builder_t *b,
                                                    int list, int out) {
  int length = -1;
  int base_len = ny_native_nir_take_list_len_fact(b, list);
  if (base_len >= 0) {
    int one = ny_native_nir_emit_const(b, 1);
    length = one < 0 ? -1 : ny_native_nir_emit_add_i64(b, base_len, one);
  }
  if (length < 0 && out >= 0)
    length = ny_native_nir_emit_runtime_call(b, "rt_tbuf_len_raw", out, -1,
                                             -1, 1, 0);
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
static int ny_native_nir_emit_known_list_pop_len(ny_native_nir_builder_t *b,
                                                 int list) {
  int base_len = ny_native_nir_take_list_len_fact(b, list);
  if (base_len < 0)
    return -1;
  int zero = ny_native_nir_emit_const(b, 0);
  if (zero < 0)
    return -1;
  int nonempty = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_CMP_I64,
                                                   .dst = -1,
                                                   .a = base_len,
                                                   .b = zero,
                                                   .cmp = NYIR_CMP_GT});
  if (nonempty < 0) {
    ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
    return -1;
  }
  return ny_native_nir_push_val(b, NYIR_SUB_I64, base_len, nonempty, 0, NULL);
}

static int ny_native_nir_emit_known_cstr_concat_len(ny_native_nir_builder_t *b,
                                                    int lhs, int rhs, int out) {
  int lhs_len =
      ny_native_nir_peek_dyn_fact(b, lhs, NY_NATIVE_NIR_FACT_DYN_STR_LEN);
  int rhs_len =
      ny_native_nir_peek_dyn_fact(b, rhs, NY_NATIVE_NIR_FACT_DYN_STR_LEN);
  if (lhs_len >= 0 && rhs_len >= 0)
    return ny_native_nir_emit_add_i64(b, lhs_len, rhs_len);
  return out < 0 ? -1
                 : ny_native_nir_emit_runtime_call(b, "rt_cstr_len", out, -1,
                                                   -1, 1, 0);
}

static int64_t ny_native_nir_literal_allocation_size(const char *leaf,
                                                     const expr_t *call) {
  if (!leaf || !call || call->kind != NY_E_CALL)
    return 0;
  ny_builtin_alloc_kind_t kind = ny_builtin_alloc_kind(leaf);
  if (kind == NY_BUILTIN_ALLOC_NONE || kind == NY_BUILTIN_ALLOC_FREE)
    return 0;
  size_t size_arg = kind == NY_BUILTIN_ALLOC_REALLOC ? 1 : 0;
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

static bool ny_native_nir_store_local_value(ny_native_nir_builder_t *b,
                                            int slot, int value) {
  ny_native_nir_local_t *capture = ny_native_nir_find_local_slot(b, slot);
  if (capture && capture->is_capture && b->closure_env_slot >= 0) {
    int env = ny_native_nir_load_local_value(b, b->closure_env_slot);
    int off =
        env < 0
            ? -1
            : ny_native_nir_emit_const(b, (int64_t)capture->capture_index * 8);
    int addr =
        env < 0 || off < 0 ? -1 : ny_native_nir_emit_add_i64(b, env, off);
    return addr >= 0 && ny_native_nir_emit_store_i64(b, addr, value);
  }
  size_t before = b->nyir.len;
  nyir_emit(
      &b->nyir,
      (nyir_inst_t){
          .op = NYIR_STORE_LOCAL, .dst = -1, .a = value, .b = -1, .imm = slot});
  if (b->nyir.len == before)
    return ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
  ny_native_nir_local_t *local = ny_native_nir_find_local_slot(b, slot);
  if (local && b->nyir.len > 0) {
    nyir_inst_t *in = &b->nyir.data[b->nyir.len - 1];
    if (in->op == NYIR_STORE_LOCAL && in->imm == slot) {
      if (local->semantic_rep)
        in->semantic_rep = (uint8_t)local->semantic_rep;
      if (local->semantic_ownership)
        in->semantic_ownership = (uint8_t)local->semantic_ownership;
      if (local->alias_class)
        in->alias_class = local->alias_class;
      in->semantic_mutable = local->semantic_mutable;
    }
  }
  int64_t byte_len = ny_native_nir_take_alloc_fact(b, value);
  int64_t fin_bound = ny_native_nir_take_fin_fact(b, value);
  int list_len = ny_native_nir_take_list_len_fact(b, value);
  int dyn_str_len =
      ny_native_nir_take_dyn_fact(b, value, NY_NATIVE_NIR_FACT_DYN_STR_LEN);
  int dyn_tag =
      ny_native_nir_take_dyn_fact(b, value, NY_NATIVE_NIR_FACT_DYN_TAG);
  if (local) {
    local->buffer_byte_len = byte_len;
    local->fin_bound = fin_bound;
    if (dyn_str_len >= 0) {
      if (local->dyn_str_len_slot < 0)
        local->dyn_str_len_slot = b->next_local_slot++;
      if (!ny_native_nir_store_local_value(b, local->dyn_str_len_slot,
                                           dyn_str_len))
        return false;
    }
    if (dyn_tag >= 0) {
      if (local->dyn_tag_slot < 0)
        local->dyn_tag_slot = b->next_local_slot++;
      if (!ny_native_nir_store_local_value(b, local->dyn_tag_slot, dyn_tag))
        return false;
    }
    if (local->is_list) {
      if (list_len < 0) {
        list_len = ny_native_nir_emit_runtime_call(b, "rt_tbuf_len_raw",
                                                   value, -1, -1, 1, 0);
        if (list_len < 0)
          return ny_native_nir_fail(
              b, "native NYIR lower: list assignment length query failed in %s",
              b->current_fn_name ? b->current_fn_name : "<unknown>");
      }
      if (local->list_len_slot < 0)
        local->list_len_slot = b->next_local_slot++;
      size_t length_before = b->nyir.len;
      nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_STORE_LOCAL,
                                        .dst = -1,
                                        .a = list_len,
                                        .b = -1,
                                        .imm = local->list_len_slot});
      if (b->nyir.len == length_before)
        return ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
    }
  }
  return true;
}

static int ny_native_nir_load_local_value(ny_native_nir_builder_t *b,
                                          int slot) {
  ny_native_nir_local_t *capture = ny_native_nir_find_local_slot(b, slot);
  if (capture && capture->is_capture && b->closure_env_slot >= 0) {
    int env = ny_native_nir_load_local_value(b, b->closure_env_slot);
    int off =
        env < 0
            ? -1
            : ny_native_nir_emit_const(b, (int64_t)capture->capture_index * 8);
    int addr =
        env < 0 || off < 0 ? -1 : ny_native_nir_emit_add_i64(b, env, off);
    return addr < 0 ? -1 : ny_native_nir_emit_load_i64(b, addr);
  }
  int v = nyir_emit(
      &b->nyir,
      (nyir_inst_t){
          .op = NYIR_LOAD_LOCAL, .dst = -1, .a = -1, .b = -1, .imm = slot});
  if (v < 0) {
    ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
    return v;
  }
  ny_native_nir_local_t *local = ny_native_nir_find_local_slot(b, slot);
  if (local && b->nyir.len > 0) {
    nyir_inst_t *in = &b->nyir.data[b->nyir.len - 1];
    if (in->op == NYIR_LOAD_LOCAL && in->imm == slot) {
      if (local->semantic_rep)
        in->semantic_rep = (uint8_t)local->semantic_rep;
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
  }
  if (local && local->buffer_byte_len > 0 &&
      !ny_native_nir_record_alloc_fact(b, v, local->buffer_byte_len))
    return -1;
  if (local && local->fin_bound > 0 &&
      !ny_native_nir_record_fin_fact(b, v, local->fin_bound))
    return -1;
  if (local && local->dyn_str_len_slot >= 0) {
    int length = ny_native_nir_load_local_value(b, local->dyn_str_len_slot);
    if (length < 0 || !ny_native_nir_record_dyn_fact(
                          b, v, NY_NATIVE_NIR_FACT_DYN_STR_LEN, length))
      return -1;
  }
  if (local && local->dyn_tag_slot >= 0) {
    int tag = ny_native_nir_load_local_value(b, local->dyn_tag_slot);
    if (tag < 0 ||
        !ny_native_nir_record_dyn_fact(b, v, NY_NATIVE_NIR_FACT_DYN_TAG, tag))
      return -1;
  }
  if (local && local->is_list) {
    int length =
        nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_LOAD_LOCAL,
                                          .dst = -1,
                                          .a = -1,
                                          .b = -1,
                                          .imm = local->list_len_slot});
    if (length < 0 || !ny_native_nir_record_list_len_fact(b, v, length)) {
      ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
      return -1;
    }
  }
  return v;
}

static int ny_native_nir_emit_is_zero(ny_native_nir_builder_t *b, int value) {
  int zero = ny_native_nir_emit_const(b, 0);
  if (zero < 0)
    return -1;
  int v = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_CMP_I64,
                                            .dst = -1,
                                            .a = value,
                                            .b = zero,
                                            .cmp = NYIR_CMP_EQ});
  if (v < 0)
    ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
  return v;
}

static int ny_native_nir_lower_logical(ny_native_nir_builder_t *b,
                                       const expr_t *left, const expr_t *right,
                                       bool is_or);
static int ny_native_nir_lower_ternary(ny_native_nir_builder_t *b,
                                       const expr_t *cond,
                                       const expr_t *true_expr,
                                       const expr_t *false_expr);
static bool ny_native_nir_lower_match(ny_native_nir_builder_t *b,
                                      const stmt_t *s);

static int ny_native_nir_lower_expr(ny_native_nir_builder_t *b,
                                    const expr_t *e);
static int ny_native_nir_lower_binary(ny_native_nir_builder_t *b,
                                      const expr_t *e);
static int ny_native_nir_lower_call(ny_native_nir_builder_t *b,
                                    const expr_t *e);

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

static const stmt_t *ny_native_nir_find_layout_stmt(const stmt_t *s,
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
      const stmt_t *found = ny_native_nir_find_layout_stmt(body->data[i], name);
      if (found)
        return found;
    }
  if (s->kind == NY_S_IF) {
    const stmt_t *found =
        ny_native_nir_find_layout_stmt(s->as.iff.conseq, name);
    if (found)
      return found;
    return ny_native_nir_find_layout_stmt(s->as.iff.alt, name);
  }
  return NULL;
}

static bool ny_native_nir_primitive_layout(const char *type, size_t *size,
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
  else if (!strcmp(type, "i64") || !strcmp(type, "u64") ||
           !strcmp(type, "int") || !strcmp(type, "float") ||
           !strcmp(type, "f64") || !strcmp(type, "ptr") ||
           !strcmp(type, "str") || !strcmp(type, "string") ||
           !strcmp(type, "bytes") || !strcmp(type, "any") || type[0] == '*' ||
           type[0] == '?' || !strncmp(type, "list", 4) ||
           !strncmp(type, "dict", 4) || !strncmp(type, "set", 3))
    *size = *align = 8;
  else
    return false;
  return true;
}

static size_t ny_native_nir_align_up(size_t n, size_t a) {
  return a > 1 ? (n + a - 1) / a * a : n;
}

static bool ny_native_nir_ast_layout_query(const ny_native_nir_builder_t *b,
                                           const char *requested,
                                           const char *field_name,
                                           size_t *size_out, size_t *align_out,
                                           size_t *offset_out) {
  if (!b || !b->prog || !requested)
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
  bool has_ast_layout = false;
  for (size_t i = 0; i < b->prog->body.len && !has_ast_layout; ++i)
    has_ast_layout =
        ny_native_nir_find_layout_stmt(b->prog->body.data[i], base) != NULL;
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
  const stmt_t *decl = NULL;
  for (size_t i = 0; i < prog->body.len && !decl; ++i)
    decl = ny_native_nir_find_layout_stmt(prog->body.data[i], base);
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
    if (!ny_native_nir_primitive_layout(f->type_name, &elem_size, &elem_align))
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
    offset = ny_native_nir_align_up(offset, fa);
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
    *size_out = ny_native_nir_align_up(offset, aggregate_align);
  if (align_out)
    *align_out = aggregate_align;
  return true;
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

static bool ny_native_nir_find_enum_member(const ny_native_nir_builder_t *b,
                                           const char *name,
                                           const stmt_t **out_enum,
                                           const stmt_enum_item_t **out_item,
                                           int64_t *out_tag) {
  if (!b || !b->prog || !name || !*name)
    return false;
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
  for (size_t i = 0; i < b->prog->body.len; ++i) {
    if (ny_native_find_enum_item_in_stmt(b->prog->body.data[i], enum_name,
                                         member_name, &counter, out_enum,
                                         out_item, out_tag))
      return true;
  }
  return false;
}

static inline int ny_native_nir_emit_binop(ny_native_nir_builder_t *b,
                                           nyir_op_t op, int a, int rhs) {
  int v =
      nyir_emit(&b->nyir, (nyir_inst_t){.op = op, .dst = -1, .a = a, .b = rhs});
  if (v < 0)
    ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
  return v;
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
static const stmt_t *
ny_native_find_impl_operator_in_stmt(const stmt_t *s, const char *owner,
                                     const char *op, const char *right_type) {
  if (!s || !op)
    return NULL;
  if (s->kind == NY_S_IMPL && s->as.impl.type_name &&
      (!owner || strcmp(s->as.impl.type_name, owner) == 0)) {
    for (size_t i = 0; i < s->as.impl.methods.len; ++i) {
      const stmt_t *m = s->as.impl.methods.data[i];
      if (!m || m->kind != NY_S_OPERATOR || !m->as.oper.op ||
          strcmp(m->as.oper.op, op) != 0)
        continue;
      if (!right_type || !m->as.oper.right_type ||
          strcmp(m->as.oper.right_type, right_type) != 0)
        continue;
      return m;
    }
  }
  if (s->kind == NY_S_MODULE) {
    for (size_t i = 0; i < s->as.module.body.len; ++i) {
      const stmt_t *found = ny_native_find_impl_operator_in_stmt(
          s->as.module.body.data[i], owner, op, right_type);
      if (found)
        return found;
    }
  }
  return NULL;
}

static const char *
ny_native_nir_expr_type_name(const ny_native_nir_builder_t *b,
                             const expr_t *e) {
  if (!b || !e)
    return NULL;
  if (e->kind == NY_E_IDENT && e->as.ident.name) {
    const ny_native_nir_local_t *local = ny_native_nir_find_local(
        (ny_native_nir_builder_t *)b, e->as.ident.name);
    return local ? local->type_name : NULL;
  }
  if (e->kind == NY_E_CALL && e->as.call.callee &&
      e->as.call.callee->kind == NY_E_IDENT) {
    const char *name = e->as.call.callee->as.ident.name;
    const stmt_t *fn = ny_native_fn_cache_lookup_exact(name);
    if (fn && fn->as.fn.return_type)
      return fn->as.fn.return_type;
    if (b->prog) {
      for (size_t i = 0; i < b->prog->body.len; ++i)
        if (ny_native_find_impl_in_stmt(b->prog->body.data[i], name))
          return name;
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
    const stmt_t *fn = ny_native_nir_find_attached_method(b, receiver, method);
    if (fn && fn->as.fn.return_type)
      return strcmp(fn->as.fn.return_type, "self") == 0
                 ? ny_native_nir_expr_type_name(b, receiver)
                 : fn->as.fn.return_type;
  }
  return NULL;
}

static const stmt_t *
ny_native_nir_find_operator(const ny_native_nir_builder_t *b, const expr_t *e) {
  if (!b || !b->prog || !e || e->kind != NY_E_BINARY || !e->as.binary.op)
    return NULL;
  const char *owner = ny_native_nir_expr_type_name(b, e->as.binary.left);
  const char *right = ny_native_nir_expr_type_name(b, e->as.binary.right);
  if (!owner)
    return NULL;
  for (size_t i = 0; i < b->prog->body.len; ++i) {
    const stmt_t *found = ny_native_find_impl_operator_in_stmt(
        b->prog->body.data[i], owner, e->as.binary.op, right);
    if (found)
      return found;
  }
  return NULL;
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
      if (m && m->kind == NY_S_FUNC && m->as.fn.name &&
          strcmp(m->as.fn.name, method) == 0)
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

static const stmt_t *
ny_native_nir_find_attached_method(const ny_native_nir_builder_t *b,
                                   const expr_t *receiver, const char *method) {
  const char *owner = ny_native_nir_expr_type_name(b, receiver);
  if (!owner || !b || !b->prog)
    return NULL;
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
  if (n > 0 && (size_t)n < sizeof(qualified))
    return ny_native_fn_cache_lookup_exact(qualified);
  return NULL;
}

static __attribute__((unused)) bool
ny_native_nir_find_impl_type(const ny_native_nir_builder_t *b,
                             const char *type_name) {
  if (!b || !b->prog || !type_name)
    return false;
  for (size_t i = 0; i < b->prog->body.len; ++i) {
    if (ny_native_find_impl_in_stmt(b->prog->body.data[i], type_name))
      return true;
  }
  return false;
}

static inline int ny_native_nir_emit_const_cstr(ny_native_nir_builder_t *b,
                                                const char *s) {
  if (!s)
    s = "";
  size_t slen = strlen(s);
  const char *sym = ny_native_strtab_intern(s, slen, NULL, 0);
  if (!sym) {
    ny_native_nir_fail(b, "native NYIR lower: string table full or OOM");
    return -1;
  }
  int addr = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_ADDR_SYMBOL,
                                               .dst = -1,
                                               .a = -1,
                                               .b = -1,
                                               .imm = 0,
                                               .symbol = sym});
  if (addr < 0)
    ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
  return addr;
}

static bool ny_native_nir_trace_requested(const ny_options *opt) {
  /*
   * Object emission is consumed by standalone linkers and may not carry the
   * runtime support library.  Keep runtime frame calls in the live native
   * executable path; object debug metadata is a separate DWARF concern.
   */
  if (!opt || !opt->native_only)
    return false;
  if (getenv("NYTRIX_NO_TRACE") && strcmp(getenv("NYTRIX_NO_TRACE"), "0") != 0)
    return false;
  if (opt->trace_exec || opt->debug_everything)
    return true;
  const char *trace = getenv("NYTRIX_TRACE");
  return trace && trace[0] && strcmp(trace, "0") != 0;
}

static bool ny_native_nir_emit_trace_enter(ny_native_nir_builder_t *b,
                                           const char *name,
                                           const char *source_file, int line) {
  if (!b || !b->trace_instrumented)
    return true;
  int fn = ny_native_nir_emit_const_cstr(b, name && name[0] ? name : "<anon>");
  int file = ny_native_nir_emit_const_cstr(
      b, source_file && source_file[0] ? source_file : "<unknown>");
  int line_value = ny_native_nir_emit_const(b, line > 0 ? line : 1);
  if (fn < 0 || file < 0 || line_value < 0)
    return false;
  return ny_native_nir_emit_runtime_call(b, "rt_trace_enter", fn, file,
                                         line_value, 3, 0) >= 0;
}

#include "lower/lower_special.h"

/*
 * Attached operator lookup is implemented below with the other AST search
 * helpers, but arithmetic lowering needs its declaration first.
 */
static const stmt_t *
ny_native_nir_find_operator(const ny_native_nir_builder_t *b, const expr_t *e);
static const char *
ny_native_nir_expr_type_name(const ny_native_nir_builder_t *b, const expr_t *e);
static const stmt_t *
ny_native_nir_find_attached_method(const ny_native_nir_builder_t *b,
                                   const expr_t *receiver, const char *method);

#include "lower/lower_arith.h"

/*
 * Native dictionaries use the raw zero word for nil.  Keep integer keys in
 * the same tagged form used at dynamic boundaries so nil and integer zero
 * remain distinct without changing the scalar integer ABI elsewhere.
 */
static int ny_native_nir_lower_dict_key(ny_native_nir_builder_t *b,
                                        const expr_t *e) {
  int key = ny_native_nir_lower_expr(b, e);
  bool integer_literal = e && e->kind == NY_E_LITERAL &&
                         e->as.literal.kind == NY_LIT_INT &&
                         e->tok.kind != NY_T_NIL;
  bool raw_integer =
      integer_literal || (e && e->semantic.rep == NY_SEM_REP_RAW_INT);
  if (key < 0 || !e || ny_native_nir_expr_is_cstr(b, e) ||
      ny_native_nir_expr_is_immutable_nil(b, e, 0) ||
      ny_native_nir_expr_is_any(b, e) || !raw_integer)
    return key;
  int one = ny_native_nir_emit_const(b, 1);
  if (one < 0)
    return -1;
  int shifted = nyir_emit(
      &b->nyir,
      (nyir_inst_t){.op = NYIR_SHL_I64, .dst = -1, .a = key, .b = one});
  if (shifted < 0) {
    ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
    return -1;
  }
  return nyir_emit(
      &b->nyir,
      (nyir_inst_t){.op = NYIR_OR_I64, .dst = -1, .a = shifted, .b = one});
}

#include "lower/lower_call.h"

#include "lower/lower_expr.h"

#include "lower/lower_stmt.h"

static bool ny_native_nir_expr_is_range(const ny_native_nir_builder_t *b,
                                        const expr_t *e) {
  if (!b || !e)
    return false;
  if (e->kind == NY_E_CALL) {
    const char *leaf = ny_native_call_leaf(e);
    if (leaf && (strcmp(leaf, "range") == 0 || strcmp(leaf, "range2") == 0))
      return true;
    const stmt_t *fn = leaf ? ny_native_nir_find_user_function(
                                  (ny_native_nir_builder_t *)b, leaf)
                            : NULL;
    return fn && fn->as.fn.return_type &&
           strcmp(fn->as.fn.return_type, "range") == 0;
  }
  if (e->kind == NY_E_MEMCALL) {
    if (e->as.memcall.name && (strcmp(e->as.memcall.name, "range") == 0 ||
                               strcmp(e->as.memcall.name, "range2") == 0))
      return true;
  }
  if (e->kind == NY_E_IDENT) {
    const expr_t *global =
        ny_native_nir_find_top_level_value(b, e->as.ident.name);
    return global && global != e && global->kind != NY_E_IDENT &&
           ny_native_nir_expr_is_range(b, global);
  }
  return false;
}
