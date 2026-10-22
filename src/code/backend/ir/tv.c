/*
 * Translation validation: after each NYIR pass, re-runs a bounded
 * set of deterministic pure-i64 programs to catch optimizer regressions.
 */
#include "base/trace.h"
#include "code/backend/ir/internal.h"
#include "code/backend/ir/opt/loop.h"
#include <nyr/ir.h>

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef NYTRIX_HAS_Z3
#include <z3.h>
#endif

#define NYIR_SMT_CFG_MAX_BACKEDGES 8u

/*
 * Real SMT translation validation for pure i64 straight-line NYIR.
 * With Z3: prove before ≡ after for all free local inputs (bitvector).
 * Without Z3 / with branches: multi-input interpreter differential.
 */

static bool nyr_has_cfg_or_phi(const nyr_func_t *f) {
  if (!f)
    return false;
  for (size_t i = 0; i < f->len; ++i) {
    nyr_op_t op = f->data[i].op;
    if (op == NYIR_BR || op == NYIR_BR_IF || op == NYIR_LABEL || op == NYIR_PHI)
      return true;
  }
  return false;
}

#ifdef NYTRIX_HAS_Z3
static Z3_ast nyr_smt_ror(Z3_context z3, Z3_ast a, Z3_ast b, unsigned bits) {
  Z3_sort word_sort = Z3_mk_bv_sort(z3, bits);
  Z3_ast word = Z3_mk_extract(z3, bits - 1, 0, a);
  Z3_ast count = Z3_mk_extract(z3, bits - 1, 0, b);
  Z3_ast mask = Z3_mk_int64(z3, (int64_t)bits - 1, word_sort);
  count = Z3_mk_bvand(z3, count, mask);
  Z3_ast width = Z3_mk_int64(z3, bits, word_sort);
  Z3_ast right = Z3_mk_bvlshr(z3, word, count);
  Z3_ast left = Z3_mk_bvshl(z3, word, Z3_mk_bvsub(z3, width, count));
  Z3_ast rotated = Z3_mk_bvor(z3, right, left);
  return bits == 32 ? Z3_mk_zero_ext(z3, 32, rotated) : rotated;
}

static Z3_ast nyr_smt_i64_shift_count(Z3_context z3, Z3_sort bv,
                                       Z3_ast count) {
  return Z3_mk_bvand(z3, count, Z3_mk_int64(z3, 63, bv));
}

/*
 * NYIR signed division and remainder trap on zero and INT64_MIN / -1.
 * SMT bit-vector division defines both cases, so track definedness separately
 * from the result to keep translation validation faithful to execution.
 */
static Z3_ast nyr_smt_div_defined(Z3_context z3, Z3_sort bv, Z3_ast a,
                                   Z3_ast b) {
  Z3_ast nonzero = Z3_mk_not(z3, Z3_mk_eq(z3, b, Z3_mk_int64(z3, 0, bv)));
  Z3_ast min_overflow =
      Z3_mk_and(z3, 2,
                (Z3_ast[]){Z3_mk_eq(z3, a, Z3_mk_int64(z3, INT64_MIN, bv)),
                           Z3_mk_eq(z3, b, Z3_mk_int64(z3, -1, bv))});
  return Z3_mk_and(z3, 2, (Z3_ast[]){nonzero, Z3_mk_not(z3, min_overflow)});
}

/*
 * NYIR_BOUNDS_CHECK traps when its byte offset is negative or reaches the
 * supplied byte length. The base is evaluated by the IR but does not affect
 * this scalar bounds predicate.
 */
static Z3_ast nyr_smt_bounds_defined(Z3_context z3, Z3_sort bv, Z3_ast offset,
                                      Z3_ast byte_len) {
  return Z3_mk_and(z3, 2,
                   (Z3_ast[]){Z3_mk_bvsge(z3, offset, Z3_mk_int64(z3, 0, bv)),
                              Z3_mk_bvslt(z3, offset, byte_len)});
}

/*
 * NYIR float-to-i64 truncates toward zero. Match the defined C conversion
 * domain so the solver cannot prove equivalence on invalid casts.
 */
static Z3_ast nyr_smt_fp_to_i64_defined(Z3_context z3, Z3_ast value) {
  Z3_sort sort = Z3_get_sort(z3, value);
  Z3_ast lower = Z3_mk_fpa_numeral_double(z3, -0x1p63, sort);
  Z3_ast upper = Z3_mk_fpa_numeral_double(z3, 0x1p63, sort);
  return Z3_mk_and(z3, 3,
                   (Z3_ast[]){Z3_mk_not(z3, Z3_mk_fpa_is_nan(z3, value)),
                              Z3_mk_fpa_geq(z3, value, lower),
                              Z3_mk_fpa_lt(z3, value, upper)});
}

static Z3_ast ny_z3_f64_from_bits(Z3_context z3, Z3_ast bits);
static Z3_ast ny_z3_f32_from_bits(Z3_context z3, Z3_ast bits);
static Z3_ast ny_z3_f64_to_bits(Z3_context z3, Z3_ast value);
static Z3_ast ny_z3_f32_to_bits64(Z3_context z3, Z3_ast value);
static Z3_ast ny_z3_fp_cmp(Z3_context z3, nyr_cmp_t cmp, Z3_ast a, Z3_ast b);

static Z3_ast nyr_smt_bswap(Z3_context z3, Z3_ast a, unsigned bits) {
  unsigned bytes = bits / 8u;
  Z3_ast out = Z3_mk_extract(z3, 7, 0, a);
  for (unsigned i = 1; i < bytes; ++i) {
    Z3_ast byte = Z3_mk_extract(z3, i * 8u + 7u, i * 8u, a);
    out = Z3_mk_concat(z3, out, byte);
  }
  return bits == 32 ? Z3_mk_zero_ext(z3, 32, out) : out;
}

static Z3_ast nyr_smt_pack_i64_lanes(Z3_context z3, Z3_ast scalar,
                                      unsigned lanes) {
  Z3_ast packed = scalar;
  for (unsigned lane = 1; lane < lanes; ++lane)
    packed = Z3_mk_concat(z3, packed, scalar);
  return packed;
}

static Z3_ast nyr_smt_shuffle_vec4_f64(Z3_context z3, Z3_ast value,
                                        int64_t mask) {
  uint64_t selectors = (uint64_t)mask;
  Z3_ast packed = NULL;
  for (unsigned lane = 4; lane-- > 0;) {
    unsigned selected = (unsigned)((selectors >> (lane * 2u)) & 3u);
    Z3_ast element =
        Z3_mk_extract(z3, selected * 64u + 63u, selected * 64u, value);
    packed = packed ? Z3_mk_concat(z3, packed, element) : element;
  }
  return packed;
}

static Z3_ast nyr_smt_reduce_i64_lanes(Z3_context z3, Z3_ast scalar,
                                        Z3_ast packed, unsigned lanes) {
  Z3_ast sum = scalar;
  for (unsigned lane = 0; lane < lanes; ++lane) {
    unsigned low = lane * 64u;
    Z3_ast value = Z3_mk_extract(z3, low + 63u, low, packed);
    sum = Z3_mk_bvadd(z3, sum, value);
  }
  return sum;
}

static Z3_ast nyr_smt_vector_i64_binop(Z3_context z3, nyr_op_t op, Z3_ast lhs,
                                        Z3_ast rhs, unsigned lanes) {
  Z3_sort lane_sort = Z3_mk_bv_sort(z3, 64);
  Z3_ast packed = NULL;
  for (unsigned lane = lanes; lane-- > 0;) {
    unsigned low = lane * 64u;
    Z3_ast a = Z3_mk_extract(z3, low + 63u, low, lhs);
    Z3_ast b = Z3_mk_extract(z3, low + 63u, low, rhs);
    Z3_ast value = NULL;
    switch (op) {
    case NYIR_VEC4_ADD_I64:
    case NYIR_VEC8_ADD_I64:
      value = Z3_mk_bvadd(z3, a, b);
      break;
    case NYIR_VEC4_SUB_I64:
    case NYIR_VEC8_SUB_I64:
      value = Z3_mk_bvsub(z3, a, b);
      break;
    case NYIR_VEC4_AND_I64:
    case NYIR_VEC8_AND_I64:
      value = Z3_mk_bvand(z3, a, b);
      break;
    case NYIR_VEC4_OR_I64:
    case NYIR_VEC8_OR_I64:
      value = Z3_mk_bvor(z3, a, b);
      break;
    case NYIR_VEC4_XOR_I64:
    case NYIR_VEC8_XOR_I64:
      value = Z3_mk_bvxor(z3, a, b);
      break;
    case NYIR_VEC4_SHL_I64:
      value = Z3_mk_bvshl(z3, a, nyr_smt_i64_shift_count(z3, lane_sort, b));
      break;
    case NYIR_VEC4_SAR_I64:
      value = Z3_mk_bvashr(z3, a, nyr_smt_i64_shift_count(z3, lane_sort, b));
      break;
    default:
      return NULL;
    }
    packed = packed ? Z3_mk_concat(z3, packed, value) : value;
  }
  return packed;
}

static Z3_ast nyr_smt_vector_f64_binop(Z3_context z3, nyr_op_t op, Z3_ast lhs,
                                        Z3_ast rhs, Z3_ast addend) {
  Z3_ast packed = NULL;
  Z3_ast rounding = Z3_mk_fpa_round_nearest_ties_to_even(z3);
  for (unsigned lane = 4; lane-- > 0;) {
    unsigned low = lane * 64u;
    Z3_ast a_bits = Z3_simplify(z3, Z3_mk_extract(z3, low + 63u, low, lhs));
    Z3_ast b_bits = Z3_simplify(z3, Z3_mk_extract(z3, low + 63u, low, rhs));
    Z3_ast a = ny_z3_f64_from_bits(z3, a_bits);
    Z3_ast b = ny_z3_f64_from_bits(z3, b_bits);
    Z3_ast value = NULL;
    switch (op) {
    case NYIR_VEC4_ADD_F64:
      value = Z3_mk_fpa_add(z3, rounding, a, b);
      break;
    case NYIR_VEC4_SUB_F64:
      value = Z3_mk_fpa_sub(z3, rounding, a, b);
      break;
    case NYIR_VEC4_MUL_F64:
      value = Z3_mk_fpa_mul(z3, rounding, a, b);
      break;
    case NYIR_VEC4_DIV_F64:
      value = Z3_mk_fpa_div(z3, rounding, a, b);
      break;
    case NYIR_VEC4_FMA_F64: {
      Z3_ast c_bits =
          Z3_simplify(z3, Z3_mk_extract(z3, low + 63u, low, addend));
      Z3_ast c = ny_z3_f64_from_bits(z3, c_bits);
      value = Z3_mk_fpa_fma(z3, rounding, a, b, c);
      break;
    }
    default:
      return NULL;
    }
    Z3_ast bits = ny_z3_f64_to_bits(z3, value);
    if (ny_trace_enabled("NY_TRACE_TV_SMT"))
      ny_trace_line("TV_SMT",
                    "vector %s lane=%u lhs=%u rhs=%u result=%u bits=%u",
                    nyr_op_name(op), lane, Z3_get_ast_id(z3, a_bits),
                    Z3_get_ast_id(z3, b_bits), Z3_get_ast_id(z3, value),
                    Z3_get_ast_id(z3, bits));
    packed = packed ? Z3_mk_concat(z3, packed, bits) : bits;
  }
  return packed;
}

static Z3_ast nyr_smt_vector_f32_binop(Z3_context z3, nyr_op_t op, Z3_ast lhs,
                                        Z3_ast rhs, Z3_ast addend) {
  Z3_ast packed = NULL;
  Z3_ast rounding = Z3_mk_fpa_round_nearest_ties_to_even(z3);
  for (unsigned lane = 4; lane-- > 0;) {
    unsigned low = lane * 32u;
    Z3_ast a = ny_z3_f32_from_bits(z3, Z3_mk_extract(z3, low + 31u, low, lhs));
    Z3_ast b = ny_z3_f32_from_bits(z3, Z3_mk_extract(z3, low + 31u, low, rhs));
    Z3_ast value = NULL;
    switch (op) {
    case NYIR_VEC8_ADD_F32:
      value = Z3_mk_fpa_add(z3, rounding, a, b);
      break;
    case NYIR_VEC8_SUB_F32:
      value = Z3_mk_fpa_sub(z3, rounding, a, b);
      break;
    case NYIR_VEC8_MUL_F32:
      value = Z3_mk_fpa_mul(z3, rounding, a, b);
      break;
    case NYIR_VEC8_DIV_F32:
      value = Z3_mk_fpa_div(z3, rounding, a, b);
      break;
    case NYIR_VEC8_FMA_F32: {
      Z3_ast c =
          ny_z3_f32_from_bits(z3, Z3_mk_extract(z3, low + 31u, low, addend));
      value = Z3_mk_fpa_fma(z3, rounding, a, b, c);
      break;
    }
    default:
      return NULL;
    }
    Z3_ast bits = Z3_mk_fpa_to_ieee_bv(z3, value);
    packed = packed ? Z3_mk_concat(z3, packed, bits) : bits;
  }
  return packed;
}

static Z3_ast nyr_smt_pack_f32_lanes(Z3_context z3, Z3_ast scalar) {
  Z3_ast bits = Z3_mk_extract(z3, 31, 0, scalar);
  Z3_ast packed = bits;
  for (unsigned lane = 1; lane < 4; ++lane)
    packed = Z3_mk_concat(z3, packed, bits);
  return packed;
}

static Z3_ast nyr_smt_shuffle_vec4_f32(Z3_context z3, Z3_ast value,
                                        int64_t mask) {
  Z3_ast packed = NULL;
  for (unsigned lane = 4; lane-- > 0;) {
    unsigned shift = lane < 2 ? lane * 2u : 4u + (lane - 2u) * 2u;
    unsigned selected = (unsigned)(((uint64_t)mask >> shift) & 3u);
    if (lane >= 2)
      selected += 2u;
    selected &= 3u;
    unsigned low = selected * 32u;
    Z3_ast element = Z3_mk_extract(z3, low + 31u, low, value);
    packed = packed ? Z3_mk_concat(z3, packed, element) : element;
  }
  return packed;
}

static Z3_ast nyr_smt_reduce_f64_lanes(Z3_context z3, Z3_ast scalar,
                                        Z3_ast packed) {
  /*
   * Materialize each rounded sum back to IEEE bits between lanes, matching
   * the scalar NYIR_ADD_F64 sequence (including its NaN representation).
   */
  Z3_ast sum_bits = scalar;
  Z3_ast rounding = Z3_mk_fpa_round_nearest_ties_to_even(z3);
  for (unsigned lane = 0; lane < 4; ++lane) {
    unsigned low = lane * 64u;
    Z3_ast bits = Z3_simplify(z3, Z3_mk_extract(z3, low + 63u, low, packed));
    Z3_ast lane_value = ny_z3_f64_from_bits(z3, bits);
    Z3_ast previous = sum_bits;
    Z3_ast sum = Z3_mk_fpa_add(z3, rounding, ny_z3_f64_from_bits(z3, sum_bits),
                               lane_value);
    sum_bits = ny_z3_f64_to_bits(z3, sum);
    if (ny_trace_enabled("NY_TRACE_TV_SMT"))
      ny_trace_line("TV_SMT",
                    "vector reduce lane=%u previous=%u lane_value=%u result=%u",
                    lane, Z3_get_ast_id(z3, previous),
                    Z3_get_ast_id(z3, lane_value), Z3_get_ast_id(z3, sum));
  }
  return sum_bits;
}

/*
 * Z3 has no exact theory for libm's transcendental functions.  Model each
 * pure unary operation as one shared function over the complete input and
 * output bit patterns.  This over-approximates every deterministic runtime
 * implementation: it can prove congruence-preserving rewrites, but cannot
 * prove numeric identities such as sin(x) == x.  Keeping the function
 * bit-level also avoids assuming anything about NaN payloads.
 */
static Z3_ast nyr_smt_uninterpreted_unary_f64(Z3_context z3, Z3_sort bv,
                                               const char *name, Z3_ast arg) {
  Z3_sort domain[1] = {bv};
  Z3_func_decl function =
      Z3_mk_func_decl(z3, Z3_mk_string_symbol(z3, name), 1, domain, bv);
  Z3_ast args[1] = {arg};
  return Z3_mk_app(z3, function, 1, args);
}

typedef struct {
  Z3_ast initial_bytes;
  Z3_func_decl valid_byte;
  struct ny_smt_call_decl *calls;
  size_t call_count;
} ny_smt_memory_t;

typedef struct ny_smt_call_decl {
  const char *symbol;
  size_t arity;
  Z3_func_decl function;
  Z3_func_decl capture_function;
  Z3_func_decl valid_function;
  bool reads_memory;
  bool writes_memory;
  bool may_trap;
  Z3_func_decl memory_function;
} ny_smt_call_decl_t;

static ny_smt_memory_t nyr_smt_memory_model(Z3_context z3, Z3_sort bv) {
  Z3_sort domain[1] = {bv};
  Z3_sort byte = Z3_mk_bv_sort(z3, 8);
  Z3_sort array = Z3_mk_array_sort(z3, bv, byte);
  return (ny_smt_memory_t){
      .initial_bytes = Z3_mk_const(
          z3, Z3_mk_string_symbol(z3, "nyir.tv.memory.bytes"), array),
      .valid_byte = Z3_mk_func_decl(
          z3, Z3_mk_string_symbol(z3, "nyir.tv.memory.valid_byte"), 1, domain,
          Z3_mk_bool_sort(z3)),
  };
}

static void nyr_smt_memory_free(ny_smt_memory_t *memory) {
  if (!memory)
    return;
  free(memory->calls);
  memory->calls = NULL;
  memory->call_count = 0;
}

static bool nyr_smt_call_supported(const nyr_func_t *f,
                                    const nyr_inst_t *in) {
  unsigned effects = in ? in->effects : 0;
  bool writes_memory = effects == (NYIR_EFFECT_CALL | NYIR_EFFECT_WRITE_MEMORY);
  bool reads_and_writes_memory =
      effects ==
      (NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_WRITE_MEMORY);
  bool reads_memory_may_trap =
      effects ==
      (NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP);
  bool writes_memory_may_trap =
      effects ==
      (NYIR_EFFECT_CALL | NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP);
  bool reads_and_writes_memory_may_trap =
      effects == (NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                  NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP);
  bool may_trap = effects == (NYIR_EFFECT_CALL | NYIR_EFFECT_MAY_TRAP);
  bool known_effects =
      effects == NYIR_EFFECT_CALL ||
      effects == (NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY) ||
      writes_memory || reads_and_writes_memory || may_trap ||
      reads_memory_may_trap || writes_memory_may_trap ||
      reads_and_writes_memory_may_trap;
  if (!f || !in || in->op != NYIR_CALL || in->dst < 0 ||
      in->dst >= f->next_value || !in->symbol || !in->symbol[0] ||
      in->imm < 0 || in->imm > NYIR_CALL_MAX_ARGS ||
      !(in->flags & NYIR_INST_F_EFFECTS_KNOWN) ||
      (in->flags & NYIR_INST_F_SRET) || !known_effects ||
      in->ret_aggregate_size != 0)
    return false;

  int args[NYIR_CALL_MAX_ARGS];
  int argc = 0;
  if (!nyr_call_args(in, f->next_value, args, NYIR_CALL_MAX_ARGS, &argc, NULL,
                      0) ||
      argc != in->imm ||
      in->extra_args_len != (argc > 6 ? (size_t)(argc - 6) : 0))
    return false;
  for (int i = 0; in->arg_sizes && i < argc; ++i)
    if (in->arg_sizes[i] != 0)
      return false;
  return true;
}

static bool nyr_smt_call_table_add(Z3_context z3, Z3_sort bv,
                                    ny_smt_memory_t *memory,
                                    const nyr_inst_t *in) {
  size_t arity = (size_t)in->imm;
  bool reads_memory = (in->effects & NYIR_EFFECT_READ_MEMORY) != 0;
  bool writes_memory = (in->effects & NYIR_EFFECT_WRITE_MEMORY) != 0;
  bool may_trap = (in->effects & NYIR_EFFECT_MAY_TRAP) != 0;
  for (size_t i = 0; i < memory->call_count; ++i) {
    const ny_smt_call_decl_t *decl = &memory->calls[i];
    if (decl->arity == arity && strcmp(decl->symbol, in->symbol) == 0)
      return decl->reads_memory == reads_memory &&
             decl->writes_memory == writes_memory && decl->may_trap == may_trap;
  }
  Z3_sort domain[NYIR_CALL_MAX_ARGS + 1];
  for (size_t i = 0; i < arity; ++i)
    domain[i] = bv;
  if (reads_memory)
    domain[arity] = Z3_get_sort(z3, memory->initial_bytes);
  unsigned call_arity = (unsigned)arity + (reads_memory ? 1u : 0u);
  Z3_func_decl function =
      Z3_mk_func_decl(z3, Z3_mk_string_symbol(z3, in->symbol), call_arity,
                      call_arity ? domain : NULL, bv);
  if (memory->call_count > (size_t)INT_MAX - 1)
    return false;
  Z3_sort capture_domain[NYIR_CALL_MAX_ARGS + 2];
  capture_domain[0] = bv;
  for (size_t i = 0; i < arity; ++i)
    capture_domain[i + 1] = bv;
  if (reads_memory)
    capture_domain[arity + 1] = Z3_get_sort(z3, memory->initial_bytes);
  unsigned capture_arity = (unsigned)arity + 1u + (reads_memory ? 1u : 0u);
  Z3_func_decl capture_function =
      Z3_mk_func_decl(z3, Z3_mk_int_symbol(z3, (int)memory->call_count),
                      capture_arity, capture_domain, bv);
  Z3_func_decl valid_function = NULL;
  if (may_trap) {
    Z3_sort valid_domain[NYIR_CALL_MAX_ARGS + 1];
    for (size_t i = 0; i < arity; ++i)
      valid_domain[i] = bv;
    unsigned valid_arity = (unsigned)arity;
    if (reads_memory)
      valid_domain[valid_arity++] = Z3_get_sort(z3, memory->initial_bytes);
    char valid_name[48];
    snprintf(valid_name, sizeof(valid_name), "nyir.tv.call.valid.%zu",
             memory->call_count);
    valid_function =
        Z3_mk_func_decl(z3, Z3_mk_string_symbol(z3, valid_name), valid_arity,
                        valid_arity ? valid_domain : NULL, Z3_mk_bool_sort(z3));
  }
  Z3_func_decl memory_function = NULL;
  if (writes_memory) {
    Z3_sort memory_domain[NYIR_CALL_MAX_ARGS + 1];
    for (size_t i = 0; i < arity; ++i)
      memory_domain[i] = bv;
    memory_domain[arity] = Z3_get_sort(z3, memory->initial_bytes);
    memory_function = Z3_mk_func_decl(z3, Z3_mk_string_symbol(z3, in->symbol),
                                      (unsigned)arity + 1, memory_domain,
                                      Z3_get_sort(z3, memory->initial_bytes));
  }
  memory->calls[memory->call_count++] =
      (ny_smt_call_decl_t){.symbol = in->symbol,
                           .arity = arity,
                           .function = function,
                           .capture_function = capture_function,
                           .reads_memory = reads_memory,
                           .writes_memory = writes_memory,
                           .may_trap = may_trap,
                           .valid_function = valid_function,
                           .memory_function = memory_function};
  return function != NULL && capture_function != NULL &&
         (!may_trap || valid_function != NULL) &&
         (!writes_memory || memory_function != NULL);
}

static bool nyr_smt_call_table_build(Z3_context z3, Z3_sort bv,
                                      ny_smt_memory_t *memory,
                                      const nyr_func_t *before,
                                      const nyr_func_t *after) {
  size_t capacity = 0;
  for (size_t i = 0; i < before->len; ++i)
    capacity += before->data[i].op == NYIR_CALL;
  for (size_t i = 0; i < after->len; ++i)
    capacity += after->data[i].op == NYIR_CALL;
  if (!capacity)
    return true;
  memory->calls = calloc(capacity, sizeof(*memory->calls));
  if (!memory->calls)
    return false;
  const nyr_func_t *functions[] = {before, after};
  for (size_t f = 0; f < sizeof(functions) / sizeof(functions[0]); ++f) {
    for (size_t i = 0; i < functions[f]->len; ++i) {
      const nyr_inst_t *in = &functions[f]->data[i];
      if (in->op == NYIR_CALL && !nyr_smt_call_table_add(z3, bv, memory, in))
        return false;
    }
  }
  return true;
}

static const ny_smt_call_decl_t *
nyr_smt_call_find(const ny_smt_memory_t *memory, const nyr_inst_t *in) {
  if (!memory || !in || !in->symbol || in->imm < 0)
    return NULL;
  for (size_t i = 0; i < memory->call_count; ++i) {
    const ny_smt_call_decl_t *decl = &memory->calls[i];
    if (decl->arity == (size_t)in->imm && strcmp(decl->symbol, in->symbol) == 0)
      return decl;
  }
  return NULL;
}

static const nyr_inst_t *nyr_smt_capture_call(const nyr_func_t *f,
                                                size_t capture_index) {
  if (!f || capture_index >= f->len)
    return NULL;
  for (size_t i = capture_index; i > 0; --i) {
    const nyr_inst_t *in = &f->data[i - 1];
    if (in->op == NYIR_CALL)
      return in;
    if (in->op != NYIR_CAPTURE_RET)
      return NULL;
  }
  return NULL;
}

static bool nyr_smt_call_result(Z3_context z3, const nyr_func_t *f,
                                 const nyr_inst_t *in,
                                 const ny_smt_memory_t *memory, Z3_ast bytes,
                                 Z3_ast *values, Z3_ast *out, Z3_ast *out_bytes,
                                 Z3_ast *out_valid) {
  if (!out || !out_bytes || !out_valid || !nyr_smt_call_supported(f, in))
    return false;
  int arg_ids[NYIR_CALL_MAX_ARGS];
  int argc = 0;
  if (!nyr_call_args(in, f->next_value, arg_ids, NYIR_CALL_MAX_ARGS, &argc,
                      NULL, 0))
    return false;
  Z3_ast args[NYIR_CALL_MAX_ARGS + 1];
  for (int i = 0; i < argc; ++i) {
    if (!values || !values[arg_ids[i]])
      return false;
    args[i] = values[arg_ids[i]];
  }
  const ny_smt_call_decl_t *decl = nyr_smt_call_find(memory, in);
  if (!decl)
    return false;
  unsigned call_arity = (unsigned)argc;
  if (decl->reads_memory) {
    if (!bytes)
      return false;
    args[call_arity++] = bytes;
  }
  *out = Z3_mk_app(z3, decl->function, call_arity, call_arity ? args : NULL);
  *out_bytes = bytes;
  *out_valid = Z3_mk_true(z3);
  if (decl->may_trap)
    *out_valid = Z3_mk_app(z3, decl->valid_function, call_arity,
                           call_arity ? args : NULL);
  if (decl->writes_memory) {
    if (!bytes)
      return false;
    Z3_ast memory_args[NYIR_CALL_MAX_ARGS + 1];
    for (int i = 0; i < argc; ++i)
      memory_args[i] = args[i];
    memory_args[argc] = bytes;
    *out_bytes = Z3_mk_app(z3, decl->memory_function, argc + 1, memory_args);
  }
  return *out != NULL && *out_bytes != NULL && *out_valid != NULL;
}

static bool nyr_smt_capture_result(Z3_context z3, Z3_sort bv,
                                    const nyr_func_t *f,
                                    const nyr_inst_t *capture,
                                    size_t capture_index,
                                    const ny_smt_memory_t *memory, Z3_ast bytes,
                                    Z3_ast *values, Z3_ast *out) {
  const nyr_inst_t *call = nyr_smt_capture_call(f, capture_index);
  if (!out || !capture || capture->imm < 0 || !call ||
      (call->effects & NYIR_EFFECT_WRITE_MEMORY) ||
      !nyr_smt_call_supported(f, call))
    return false;
  int arg_ids[NYIR_CALL_MAX_ARGS];
  int argc = 0;
  if (!nyr_call_args(call, f->next_value, arg_ids, NYIR_CALL_MAX_ARGS, &argc,
                      NULL, 0))
    return false;
  Z3_ast args[NYIR_CALL_MAX_ARGS + 2];
  args[0] = Z3_mk_int64(z3, capture->imm, bv);
  for (int i = 0; i < argc; ++i) {
    if (!values || !values[arg_ids[i]])
      return false;
    args[i + 1] = values[arg_ids[i]];
  }
  const ny_smt_call_decl_t *decl = nyr_smt_call_find(memory, call);
  if (!decl)
    return false;
  unsigned capture_arity = (unsigned)argc + 1;
  if (decl->reads_memory) {
    if (!bytes)
      return false;
    args[capture_arity++] = bytes;
  }
  *out = Z3_mk_app(z3, decl->capture_function, capture_arity, args);
  return *out != NULL;
}

static Z3_ast nyr_smt_memory_valid_byte(Z3_context z3,
                                         const ny_smt_memory_t *memory,
                                         Z3_ast address) {
  Z3_ast args[1] = {address};
  return Z3_mk_app(z3, memory->valid_byte, 1, args);
}

static Z3_ast nyr_smt_address_add_const(Z3_context z3, Z3_sort bv,
                                         Z3_ast address, int64_t offset) {
  /*
   * Normalize offset-zero scalar pointers against vector lane addresses.
   */
  if (offset == 0)
    return Z3_simplify(z3, address);
  return Z3_simplify(z3, Z3_mk_bvadd(z3, address, Z3_mk_int64(z3, offset, bv)));
}

typedef struct {
  int alloca_inst;
  int64_t offset;
  bool unknown;
} ny_smt_alloca_ptr_t;

static ny_smt_alloca_ptr_t ny_smt_no_alloca_ptr(void) {
  return (ny_smt_alloca_ptr_t){.alloca_inst = -1};
}

static ny_smt_alloca_ptr_t ny_smt_unknown_alloca_ptr(void) {
  return (ny_smt_alloca_ptr_t){.alloca_inst = -1, .unknown = true};
}

static const nyr_inst_t *nyr_smt_value_def(const nyr_func_t *f, int value) {
  if (!f || value < 0 || value >= f->next_value)
    return NULL;
  for (size_t i = 0; i < f->len; ++i)
    if (f->data[i].dst == value)
      return &f->data[i];
  return NULL;
}

static bool nyr_smt_const_i64(const nyr_func_t *f, int value, int64_t *out,
                               unsigned depth) {
  if (!out || depth >= 64)
    return false;
  const nyr_inst_t *def = nyr_smt_value_def(f, value);
  if (!def)
    return false;
  if (def->op == NYIR_CONST_I64) {
    *out = def->imm;
    return true;
  }
  return def->op == NYIR_COPY && nyr_smt_const_i64(f, def->a, out, depth + 1);
}

static ny_smt_alloca_ptr_t nyr_smt_alloca_ptr(const nyr_func_t *f, int value,
                                               unsigned depth);

static ny_smt_alloca_ptr_t
nyr_smt_merge_alloca_ptr(ny_smt_alloca_ptr_t left, ny_smt_alloca_ptr_t right) {
  if (left.unknown || right.unknown)
    return ny_smt_unknown_alloca_ptr();
  if (left.alloca_inst < 0 && right.alloca_inst < 0)
    return ny_smt_no_alloca_ptr();
  if (left.alloca_inst < 0 || right.alloca_inst < 0 ||
      left.alloca_inst != right.alloca_inst || left.offset != right.offset)
    return ny_smt_unknown_alloca_ptr();
  return left;
}

static ny_smt_alloca_ptr_t nyr_smt_alloca_ptr(const nyr_func_t *f, int value,
                                               unsigned depth) {
  if (!f || depth >= 64)
    return ny_smt_unknown_alloca_ptr();
  const nyr_inst_t *def = nyr_smt_value_def(f, value);
  if (!def)
    return ny_smt_unknown_alloca_ptr();
  if (def->op == NYIR_ALLOCA) {
    for (size_t i = 0; i < f->len; ++i)
      if (&f->data[i] == def)
        return (ny_smt_alloca_ptr_t){.alloca_inst = (int)i};
    return ny_smt_unknown_alloca_ptr();
  }
  if (def->op == NYIR_COPY)
    return nyr_smt_alloca_ptr(f, def->a, depth + 1);
  if (def->op == NYIR_ADD_I64 || def->op == NYIR_SUB_I64) {
    ny_smt_alloca_ptr_t left = nyr_smt_alloca_ptr(f, def->a, depth + 1);
    ny_smt_alloca_ptr_t right = nyr_smt_alloca_ptr(f, def->b, depth + 1);
    int64_t constant = 0;
    bool right_constant = nyr_smt_const_i64(f, def->b, &constant, depth + 1);
    if (left.alloca_inst >= 0 && !left.unknown && right_constant &&
        right.alloca_inst < 0 && !right.unknown) {
      if ((constant > 0 && left.offset > INT64_MAX - constant) ||
          (constant < 0 && left.offset < INT64_MIN - constant))
        return ny_smt_unknown_alloca_ptr();
      left.offset += constant;
      return left;
    }
    if (def->op == NYIR_ADD_I64) {
      int64_t left_constant = 0;
      bool is_left_constant =
          nyr_smt_const_i64(f, def->a, &left_constant, depth + 1);
      if (right.alloca_inst >= 0 && !right.unknown && is_left_constant &&
          left.alloca_inst < 0 && !left.unknown) {
        if ((left_constant > 0 && right.offset > INT64_MAX - left_constant) ||
            (left_constant < 0 && right.offset < INT64_MIN - left_constant))
          return ny_smt_unknown_alloca_ptr();
        right.offset += left_constant;
        return right;
      }
    } else if (left.alloca_inst >= 0 && !left.unknown &&
               right.alloca_inst < 0 && !right.unknown &&
               nyr_smt_const_i64(f, def->b, &constant, depth + 1)) {
      if ((constant > 0 && left.offset < INT64_MIN + constant) ||
          (constant < 0 && left.offset > INT64_MAX + constant))
        return ny_smt_unknown_alloca_ptr();
      left.offset -= constant;
      return left;
    }
    if (left.unknown || right.unknown || left.alloca_inst >= 0 ||
        right.alloca_inst >= 0)
      return ny_smt_unknown_alloca_ptr();
    return ny_smt_no_alloca_ptr();
  }
  if (def->op == NYIR_PHI) {
    ny_smt_alloca_ptr_t merged = ny_smt_no_alloca_ptr();
    bool has_incoming = false;
    for (size_t i = 0; i < def->phi_incoming_len; ++i)
      if (!has_incoming) {
        merged = nyr_smt_alloca_ptr(f, def->phi_incoming[i].value, depth + 1);
        has_incoming = true;
      } else
        merged = nyr_smt_merge_alloca_ptr(
            merged,
            nyr_smt_alloca_ptr(f, def->phi_incoming[i].value, depth + 1));
    return merged;
  }
  if (def->op == NYIR_SELECT_I64)
    return nyr_smt_merge_alloca_ptr(nyr_smt_alloca_ptr(f, def->b, depth + 1),
                                     nyr_smt_alloca_ptr(f, def->c, depth + 1));
  if (def->op == NYIR_LOAD_LOCAL) {
    ny_smt_alloca_ptr_t merged = ny_smt_no_alloca_ptr();
    bool has_store = false;
    size_t load_index = f->len;
    for (size_t i = 0; i < f->len; ++i)
      if (&f->data[i] == def) {
        load_index = i;
        break;
      }
    if (load_index == f->len)
      return ny_smt_unknown_alloca_ptr();
    for (size_t i = 0; i < load_index; ++i)
      if (f->data[i].op == NYIR_STORE_LOCAL && f->data[i].imm == def->imm) {
        ny_smt_alloca_ptr_t stored =
            nyr_smt_alloca_ptr(f, f->data[i].a, depth + 1);
        merged = has_store ? nyr_smt_merge_alloca_ptr(merged, stored) : stored;
        has_store = true;
      }
    return merged;
  }
  int operands[] = {def->a, def->b, def->c, def->d, def->e, def->f};
  for (size_t i = 0; i < sizeof(operands) / sizeof(operands[0]); ++i) {
    if (operands[i] < 0)
      continue;
    ny_smt_alloca_ptr_t operand =
        nyr_smt_alloca_ptr(f, operands[i], depth + 1);
    if (operand.unknown || operand.alloca_inst >= 0)
      return ny_smt_unknown_alloca_ptr();
  }
  return ny_smt_no_alloca_ptr();
}

typedef struct {
  int slot;
  int64_t offset;
  bool unknown;
} ny_smt_local_ptr_t;

static ny_smt_local_ptr_t ny_smt_no_local_ptr(void) {
  return (ny_smt_local_ptr_t){.slot = -1};
}

static ny_smt_local_ptr_t ny_smt_unknown_local_ptr(void) {
  return (ny_smt_local_ptr_t){.slot = -1, .unknown = true};
}

static ny_smt_local_ptr_t nyr_smt_local_ptr(const nyr_func_t *f, int value,
                                             unsigned depth);

static ny_smt_local_ptr_t nyr_smt_merge_local_ptr(ny_smt_local_ptr_t left,
                                                   ny_smt_local_ptr_t right) {
  if (left.unknown || right.unknown)
    return ny_smt_unknown_local_ptr();
  if (left.slot < 0 && right.slot < 0)
    return ny_smt_no_local_ptr();
  if (left.slot < 0 || right.slot < 0 || left.slot != right.slot ||
      left.offset != right.offset)
    return ny_smt_unknown_local_ptr();
  return left;
}

static ny_smt_local_ptr_t nyr_smt_local_ptr(const nyr_func_t *f, int value,
                                             unsigned depth) {
  if (!f || depth >= 64)
    return ny_smt_unknown_local_ptr();
  const nyr_inst_t *def = nyr_smt_value_def(f, value);
  if (!def)
    return ny_smt_unknown_local_ptr();
  if (def->op == NYIR_ADDR_LOCAL)
    return def->imm >= 0 && def->imm <= INT_MAX
               ? (ny_smt_local_ptr_t){.slot = (int)def->imm}
               : ny_smt_unknown_local_ptr();
  if (def->op == NYIR_COPY)
    return nyr_smt_local_ptr(f, def->a, depth + 1);
  if (def->op == NYIR_ADD_I64 || def->op == NYIR_SUB_I64) {
    ny_smt_local_ptr_t left = nyr_smt_local_ptr(f, def->a, depth + 1);
    ny_smt_local_ptr_t right = nyr_smt_local_ptr(f, def->b, depth + 1);
    int64_t constant = 0;
    bool right_constant = nyr_smt_const_i64(f, def->b, &constant, depth + 1);
    if (left.slot >= 0 && !left.unknown && right_constant && right.slot < 0 &&
        !right.unknown) {
      if (def->op == NYIR_ADD_I64) {
        if ((constant > 0 && left.offset > INT64_MAX - constant) ||
            (constant < 0 && left.offset < INT64_MIN - constant))
          return ny_smt_unknown_local_ptr();
        left.offset += constant;
      } else {
        if ((constant > 0 && left.offset < INT64_MIN + constant) ||
            (constant < 0 && left.offset > INT64_MAX + constant))
          return ny_smt_unknown_local_ptr();
        left.offset -= constant;
      }
      return left;
    }
    if (def->op == NYIR_ADD_I64) {
      int64_t left_constant = 0;
      if (right.slot >= 0 && !right.unknown && left.slot < 0 && !left.unknown &&
          nyr_smt_const_i64(f, def->a, &left_constant, depth + 1)) {
        if ((left_constant > 0 && right.offset > INT64_MAX - left_constant) ||
            (left_constant < 0 && right.offset < INT64_MIN - left_constant))
          return ny_smt_unknown_local_ptr();
        right.offset += left_constant;
        return right;
      }
    }
    if (left.unknown || right.unknown || left.slot >= 0 || right.slot >= 0)
      return ny_smt_unknown_local_ptr();
    return ny_smt_no_local_ptr();
  }
  if (def->op == NYIR_PHI) {
    ny_smt_local_ptr_t merged = ny_smt_no_local_ptr();
    bool has_incoming = false;
    for (size_t i = 0; i < def->phi_incoming_len; ++i) {
      ny_smt_local_ptr_t incoming =
          nyr_smt_local_ptr(f, def->phi_incoming[i].value, depth + 1);
      merged =
          has_incoming ? nyr_smt_merge_local_ptr(merged, incoming) : incoming;
      has_incoming = true;
    }
    return has_incoming ? merged : ny_smt_unknown_local_ptr();
  }
  if (def->op == NYIR_SELECT_I64)
    return nyr_smt_merge_local_ptr(nyr_smt_local_ptr(f, def->b, depth + 1),
                                    nyr_smt_local_ptr(f, def->c, depth + 1));
  if (def->op == NYIR_LOAD_LOCAL) {
    ny_smt_local_ptr_t merged = ny_smt_no_local_ptr();
    bool has_store = false;
    size_t load_index = f->len;
    for (size_t i = 0; i < f->len; ++i)
      if (&f->data[i] == def) {
        load_index = i;
        break;
      }
    if (load_index == f->len)
      return ny_smt_unknown_local_ptr();
    for (size_t i = 0; i < load_index; ++i)
      if (f->data[i].op == NYIR_STORE_LOCAL && f->data[i].imm == def->imm) {
        ny_smt_local_ptr_t stored =
            nyr_smt_local_ptr(f, f->data[i].a, depth + 1);
        merged = has_store ? nyr_smt_merge_local_ptr(merged, stored) : stored;
        has_store = true;
      }
    return merged;
  }
  int operands[] = {def->a, def->b, def->c, def->d, def->e, def->f};
  for (size_t i = 0; i < sizeof(operands) / sizeof(operands[0]); ++i) {
    if (operands[i] < 0)
      continue;
    ny_smt_local_ptr_t operand = nyr_smt_local_ptr(f, operands[i], depth + 1);
    if (operand.unknown || operand.slot >= 0)
      return ny_smt_unknown_local_ptr();
  }
  return ny_smt_no_local_ptr();
}

static Z3_ast nyr_smt_alloca_base(Z3_context z3, Z3_sort bv,
                                   size_t inst_index) {
  char name[64];
  snprintf(name, sizeof(name), "nyir.tv.alloca.%zu", inst_index);
  return Z3_mk_const(z3, Z3_mk_string_symbol(z3, name), bv);
}

static Z3_ast nyr_smt_local_base(Z3_context z3, Z3_sort bv, size_t slot) {
  char name[48];
  snprintf(name, sizeof(name), "nyir.tv.local.%zu", slot);
  return Z3_mk_const(z3, Z3_mk_string_symbol(z3, name), bv);
}

static Z3_ast nyr_smt_symbol_address(Z3_context z3, Z3_sort bv,
                                      const char *symbol) {
  static const char prefix[] = "nyir.tv.symbol.";
  if (!symbol)
    return NULL;
  size_t prefix_len = sizeof(prefix) - 1;
  size_t symbol_len = strlen(symbol);
  if (symbol_len > SIZE_MAX - prefix_len - 1)
    return NULL;
  char *name = malloc(prefix_len + symbol_len + 1);
  if (!name)
    return NULL;
  memcpy(name, prefix, prefix_len);
  memcpy(name + prefix_len, symbol, symbol_len + 1);
  Z3_ast address = Z3_mk_const(z3, Z3_mk_string_symbol(z3, name), bv);
  free(name);
  return address;
}

static bool nyr_smt_has_alloca(const nyr_func_t *f) {
  for (size_t i = 0; f && i < f->len; ++i)
    if (f->data[i].op == NYIR_ALLOCA)
      return true;
  return false;
}

static bool nyr_smt_alloca_cfg_supported(const nyr_func_t *f) {
  if (!nyr_smt_has_alloca(f))
    return true;

  nyr_cfg_t cfg = {0};
  nyr_scev_info_t scev = {0};
  bool *members = NULL;
  bool supported = false;
  if (!nyr_cfg_build(f, &cfg) || !nyr_scev_analyze(f, &scev))
    goto done;
  members = calloc(cfg.block_count ? cfg.block_count : 1, sizeof(*members));
  if (!members)
    goto done;

  /*
   * NYIR_ALLOCA denotes one function-frame slot per instruction, reused when
   * control reaches the instruction again. Admit only statically bounded
   * allocation loops that fit the CFG walker's per-path backedge budget.
   */
  for (size_t latch = 0; latch < cfg.block_count; ++latch) {
    for (size_t e = cfg.succ_offsets[latch]; e < cfg.succ_offsets[latch + 1];
         ++e) {
      size_t header = cfg.succ_blocks[e];
      if (!nyr_cfg_is_backedge(&cfg, latch, header))
        continue;
      if (!nyr_cfg_natural_loop_blocks(&cfg, latch, header, members,
                                        cfg.block_count))
        goto done;
      bool has_alloca = false;
      for (size_t block = 0; block < cfg.block_count && !has_alloca; ++block) {
        if (!members[block])
          continue;
        for (size_t i = cfg.block_start[block]; i < cfg.block_end[block]; ++i)
          has_alloca |= f->data[i].op == NYIR_ALLOCA;
      }
      if (!has_alloca)
        continue;
      bool within_backedge_limit = false;
      for (size_t i = 0; i < scev.count; ++i) {
        const nyr_scev_loop_t *loop = &scev.loops[i];
        if (loop->header_block == header && loop->latch_block == latch &&
            loop->trip_count_known &&
            loop->trip_count <= NYIR_SMT_CFG_MAX_BACKEDGES) {
          within_backedge_limit = true;
          break;
        }
      }
      if (!within_backedge_limit)
        goto done;
    }
  }

  /*
   * Also reject allocation-bearing backward edges that are not recognized as
   * natural loops. This keeps irreducible or malformed cycles fail-closed.
   */
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *branch = &f->data[i];
    if (branch->op != NYIR_BR && branch->op != NYIR_BR_IF)
      continue;
    size_t target = SIZE_MAX;
    for (size_t j = 0; j < f->len; ++j)
      if (f->data[j].op == NYIR_LABEL && f->data[j].imm == branch->imm) {
        target = j;
        break;
      }
    if (target == SIZE_MAX)
      goto done;
    if (target > i)
      continue;
    bool has_alloca = false;
    for (size_t k = target; k <= i; ++k)
      has_alloca |= f->data[k].op == NYIR_ALLOCA;
    if (!has_alloca)
      continue;
    size_t header = cfg.inst_block[target];
    size_t latch = cfg.inst_block[i];
    if (!nyr_cfg_is_backedge(&cfg, latch, header))
      goto done;
    bool within_backedge_limit = false;
    for (size_t k = 0; k < scev.count; ++k) {
      const nyr_scev_loop_t *loop = &scev.loops[k];
      if (loop->header_block == header && loop->latch_block == latch &&
          loop->trip_count_known &&
          loop->trip_count <= NYIR_SMT_CFG_MAX_BACKEDGES) {
        within_backedge_limit = true;
        break;
      }
    }
    if (!within_backedge_limit)
      goto done;
  }
  supported = true;

done:
  free(members);
  nyr_scev_free(&scev);
  nyr_cfg_free(&cfg);
  return supported;
}

static bool nyr_smt_alloca_shapes_match(const nyr_func_t *before,
                                         const nyr_func_t *after) {
  if (!before || !after)
    return false;
  bool before_has_alloca = nyr_smt_has_alloca(before);
  bool after_has_alloca = nyr_smt_has_alloca(after);
  if (!before_has_alloca && !after_has_alloca)
    return true;
  if (!before_has_alloca || !after_has_alloca)
    return false;
  size_t len = before->len > after->len ? before->len : after->len;
  for (size_t i = 0; i < len; ++i) {
    const nyr_inst_t *a = i < before->len ? &before->data[i] : NULL;
    const nyr_inst_t *b = i < after->len ? &after->data[i] : NULL;
    bool a_alloca = a && a->op == NYIR_ALLOCA;
    bool b_alloca = b && b->op == NYIR_ALLOCA;
    if (a_alloca != b_alloca)
      return false;
    if (a_alloca && (a->imm <= 0 || a->imm != b->imm))
      return false;
  }
  return true;
}

static void nyr_smt_constrain_allocas(Z3_context z3, Z3_sort bv,
                                       Z3_solver solver, const nyr_func_t *f) {
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *alloca = &f->data[i];
    if (alloca->op != NYIR_ALLOCA)
      continue;
    Z3_ast base = nyr_smt_alloca_base(z3, bv, i);
    Z3_ast size = Z3_mk_int64(z3, alloca->imm, bv);
    Z3_ast end = Z3_mk_bvadd(z3, base, size);
    Z3_solver_assert(z3, solver,
                     Z3_mk_bvuge(z3, base, Z3_mk_int64(z3, 4096, bv)));
    Z3_solver_assert(z3, solver, Z3_mk_bvuge(z3, end, base));
    for (size_t j = 0; j < i; ++j) {
      const nyr_inst_t *other = &f->data[j];
      if (other->op != NYIR_ALLOCA)
        continue;
      Z3_ast other_base = nyr_smt_alloca_base(z3, bv, j);
      Z3_ast other_end =
          Z3_mk_bvadd(z3, other_base, Z3_mk_int64(z3, other->imm, bv));
      Z3_ast disjoint = Z3_mk_or(z3, 2,
                                 (Z3_ast[]){Z3_mk_bvule(z3, end, other_base),
                                            Z3_mk_bvule(z3, other_end, base)});
      Z3_solver_assert(z3, solver, disjoint);
    }
  }
}

static void nyr_smt_constrain_locals(Z3_context z3, Z3_sort bv,
                                      Z3_solver solver, const nyr_func_t *f,
                                      size_t nloc) {
  for (size_t i = 0; i < nloc; ++i) {
    Z3_ast base = nyr_smt_local_base(z3, bv, i);
    Z3_ast end = Z3_mk_bvadd(z3, base, Z3_mk_int64(z3, 8, bv));
    Z3_solver_assert(z3, solver,
                     Z3_mk_bvuge(z3, base, Z3_mk_int64(z3, 4096, bv)));
    Z3_solver_assert(z3, solver, Z3_mk_bvuge(z3, end, base));
    for (size_t j = 0; j < i; ++j) {
      Z3_ast other_base = nyr_smt_local_base(z3, bv, j);
      Z3_ast other_end = Z3_mk_bvadd(z3, other_base, Z3_mk_int64(z3, 8, bv));
      Z3_solver_assert(z3, solver,
                       Z3_mk_or(z3, 2,
                                (Z3_ast[]){Z3_mk_bvule(z3, end, other_base),
                                           Z3_mk_bvule(z3, other_end, base)}));
    }
    for (size_t j = 0; j < f->len; ++j) {
      if (f->data[j].op != NYIR_ALLOCA)
        continue;
      Z3_ast other_base = nyr_smt_alloca_base(z3, bv, j);
      Z3_ast other_end =
          Z3_mk_bvadd(z3, other_base, Z3_mk_int64(z3, f->data[j].imm, bv));
      Z3_solver_assert(z3, solver,
                       Z3_mk_or(z3, 2,
                                (Z3_ast[]){Z3_mk_bvule(z3, end, other_base),
                                           Z3_mk_bvule(z3, other_end, base)}));
    }
  }
}

static Z3_ast nyr_smt_memory_valid_for_value(Z3_context z3, Z3_sort bv,
                                              const ny_smt_memory_t *memory,
                                              const nyr_func_t *f,
                                              bool has_alloca,
                                              int address_value, Z3_ast address,
                                              unsigned width) {
  ny_smt_alloca_ptr_t ptr = has_alloca
                                ? nyr_smt_alloca_ptr(f, address_value, 0)
                                : ny_smt_no_alloca_ptr();
  if (ptr.unknown)
    return NULL;
  if (ptr.alloca_inst >= 0) {
    if ((size_t)ptr.alloca_inst >= f->len || ptr.offset < 0)
      return Z3_mk_false(z3);
    int64_t size = f->data[ptr.alloca_inst].imm;
    uint64_t offset = (uint64_t)ptr.offset;
    return width <= (uint64_t)size && offset <= (uint64_t)size - width
               ? Z3_mk_true(z3)
               : Z3_mk_false(z3);
  }
  ny_smt_local_ptr_t local = nyr_smt_local_ptr(f, address_value, 0);
  if (local.unknown)
    return NULL;
  if (local.slot >= 0) {
    if (local.offset < 0)
      return Z3_mk_false(z3);
    uint64_t offset = (uint64_t)local.offset;
    return width <= sizeof(int64_t) && offset <= sizeof(int64_t) - width
               ? Z3_mk_true(z3)
               : Z3_mk_false(z3);
  }
  Z3_ast valid = Z3_mk_true(z3);
  for (unsigned chunk_start = 0; chunk_start < width; chunk_start += 8) {
    unsigned chunk_width = width - chunk_start;
    if (chunk_width > 8)
      chunk_width = 8;
    Z3_ast chunk_valid = Z3_mk_true(z3);
    for (unsigned i = 0; i < chunk_width; ++i) {
      Z3_ast at = nyr_smt_address_add_const(z3, bv, address,
                                             (int64_t)chunk_start + (int64_t)i);
      chunk_valid = Z3_simplify(
          z3, Z3_mk_and(z3, 2,
                        (Z3_ast[]){chunk_valid, nyr_smt_memory_valid_byte(
                                                    z3, memory, at)}));
    }
    valid = Z3_simplify(z3, Z3_mk_and(z3, 2, (Z3_ast[]){valid, chunk_valid}));
  }
  return valid;
}

static Z3_ast nyr_smt_copy_struct_valid(Z3_context z3, Z3_sort bv,
                                         const ny_smt_memory_t *memory,
                                         const nyr_func_t *f, bool has_alloca,
                                         int dst_value, Z3_ast dst,
                                         int src_value, Z3_ast src,
                                         unsigned size) {
  Z3_ast dst_valid = nyr_smt_memory_valid_for_value(
      z3, bv, memory, f, has_alloca, dst_value, dst, size);
  Z3_ast src_valid = nyr_smt_memory_valid_for_value(
      z3, bv, memory, f, has_alloca, src_value, src, size);
  if (!dst_valid || !src_valid)
    return NULL;

  Z3_ast extent = Z3_mk_int64(z3, size, bv);
  Z3_ast dst_end = Z3_mk_bvadd(z3, dst, extent);
  Z3_ast src_end = Z3_mk_bvadd(z3, src, extent);
  Z3_ast no_wrap = Z3_mk_and(
      z3, 2,
      (Z3_ast[]){Z3_mk_bvuge(z3, dst_end, dst), Z3_mk_bvuge(z3, src_end, src)});
  Z3_ast disjoint = Z3_mk_or(
      z3, 2,
      (Z3_ast[]){Z3_mk_bvule(z3, dst_end, src), Z3_mk_bvule(z3, src_end, dst)});
  return Z3_mk_and(z3, 3,
                   (Z3_ast[]){dst_valid, src_valid,
                              Z3_mk_and(z3, 2, (Z3_ast[]){no_wrap, disjoint})});
}

static Z3_ast nyr_smt_memory_copy_struct(Z3_context z3, Z3_sort bv,
                                          Z3_ast bytes, Z3_ast dst, Z3_ast src,
                                          unsigned size) {
  for (unsigned i = 0; i < size; ++i) {
    Z3_ast offset = Z3_mk_int64(z3, i, bv);
    Z3_ast source = Z3_mk_bvadd(z3, src, offset);
    Z3_ast destination = Z3_mk_bvadd(z3, dst, offset);
    Z3_ast value = Z3_mk_select(z3, bytes, source);
    bytes = Z3_mk_store(z3, bytes, destination, value);
  }
  return bytes;
}

static Z3_ast nyr_smt_memory_load32(Z3_context z3, Z3_sort bv, Z3_ast bytes,
                                     Z3_ast address) {
  Z3_ast value = NULL;
  for (unsigned i = 0; i < 4; ++i) {
    Z3_ast at = nyr_smt_address_add_const(z3, bv, address, i);
    Z3_ast byte = Z3_mk_select(z3, bytes, at);
    value = value ? Z3_mk_concat(z3, byte, value) : byte;
  }
  return value;
}

static Z3_ast nyr_smt_memory_load64(Z3_context z3, Z3_sort bv, Z3_ast bytes,
                                     Z3_ast address) {
  Z3_ast value = NULL;
  for (unsigned i = 0; i < 8; ++i) {
    Z3_ast at = nyr_smt_address_add_const(z3, bv, address, i);
    Z3_ast byte = Z3_mk_select(z3, bytes, at);
    value = value ? Z3_mk_concat(z3, byte, value) : byte;
  }
  value = Z3_simplify(z3, value);
  if (ny_trace_enabled("NY_TRACE_TV_SMT"))
    ny_trace_line("TV_SMT", "load64 address=%u value=%u",
                  Z3_get_ast_id(z3, address), Z3_get_ast_id(z3, value));
  return value;
}

static Z3_ast nyr_smt_memory_store64(Z3_context z3, Z3_sort bv, Z3_ast bytes,
                                      Z3_ast address, Z3_ast value,
                                      unsigned width) {
  for (unsigned i = 0; i < width; ++i) {
    Z3_ast at = nyr_smt_address_add_const(z3, bv, address, i);
    Z3_ast byte = Z3_mk_extract(z3, i * 8 + 7, i * 8, value);
    bytes = Z3_mk_store(z3, bytes, at, byte);
  }
  return bytes;
}

static Z3_ast nyr_smt_memory_init_locals(Z3_context z3, Z3_sort bv,
                                          Z3_ast bytes, Z3_ast *locals,
                                          size_t nloc) {
  if (nloc && !locals)
    return NULL;
  for (size_t i = 0; i < nloc; ++i)
    bytes = nyr_smt_memory_store64(
        z3, bv, bytes, nyr_smt_local_base(z3, bv, i), locals[i], 8);
  return bytes;
}

static Z3_ast nyr_smt_memory_store_vec_i64(Z3_context z3, Z3_sort bv,
                                            Z3_ast bytes, Z3_ast address,
                                            Z3_ast packed, unsigned lanes) {
  for (unsigned lane = 0; lane < lanes; ++lane) {
    unsigned low = lane * 64u;
    Z3_ast value = Z3_mk_extract(z3, low + 63u, low, packed);
    Z3_ast lane_address =
        nyr_smt_address_add_const(z3, bv, address, (int64_t)low / 8);
    bytes = nyr_smt_memory_store64(z3, bv, bytes, lane_address, value, 8);
  }
  return bytes;
}

static Z3_ast nyr_smt_memory_load_vec2_i64(Z3_context z3, Z3_sort bv,
                                            Z3_ast bytes, Z3_ast address) {
  Z3_ast second_address = nyr_smt_address_add_const(z3, bv, address, 8);
  Z3_ast first = nyr_smt_memory_load64(z3, bv, bytes, address);
  Z3_ast second = nyr_smt_memory_load64(z3, bv, bytes, second_address);
  return Z3_mk_concat(z3, second, first);
}

static Z3_ast nyr_smt_memory_load_vec4_i64(Z3_context z3, Z3_sort bv,
                                            Z3_ast bytes, Z3_ast address) {
  Z3_ast packed = nyr_smt_memory_load64(z3, bv, bytes, address);
  for (unsigned lane = 1; lane < 4; ++lane) {
    Z3_ast lane_address =
        nyr_smt_address_add_const(z3, bv, address, (int64_t)lane * 8);
    packed = Z3_mk_concat(
        z3, nyr_smt_memory_load64(z3, bv, bytes, lane_address), packed);
  }
  return packed;
}

static Z3_ast nyr_smt_memory_load_vec4_f32(Z3_context z3, Z3_sort bv,
                                            Z3_ast bytes, Z3_ast address) {
  Z3_ast packed = nyr_smt_memory_load32(z3, bv, bytes, address);
  for (unsigned lane = 1; lane < 4; ++lane) {
    Z3_ast lane_address =
        nyr_smt_address_add_const(z3, bv, address, (int64_t)lane * 4);
    packed = Z3_mk_concat(
        z3, nyr_smt_memory_load32(z3, bv, bytes, lane_address), packed);
  }
  return packed;
}

static Z3_ast nyr_smt_memory_store_vec_f32(Z3_context z3, Z3_sort bv,
                                            Z3_ast bytes, Z3_ast address,
                                            Z3_ast packed) {
  for (unsigned lane = 0; lane < 4; ++lane) {
    unsigned low = lane * 32u;
    Z3_ast value = Z3_mk_extract(z3, low + 31u, low, packed);
    Z3_ast lane_address =
        nyr_smt_address_add_const(z3, bv, address, lane * 4u);
    bytes = nyr_smt_memory_store64(z3, bv, bytes, lane_address, value, 4);
  }
  return bytes;
}

static bool nyr_smt_eval_straightline(Z3_context z3, Z3_sort bv,
                                       const nyr_func_t *f,
                                       const ny_smt_memory_t *memory,
                                       Z3_ast *locals, size_t nloc,
                                       Z3_ast *out_ret, Z3_ast *out_valid) {
  Z3_ast *vals = NULL;
  Z3_ast *loc = NULL;
  if (f->next_value > 0) {
    vals = (Z3_ast *)calloc((size_t)f->next_value, sizeof(Z3_ast));
    if (!vals)
      return false;
  }
  if (nloc) {
    loc = (Z3_ast *)calloc(nloc, sizeof(Z3_ast));
    if (!loc) {
      free(vals);
      return false;
    }
    for (size_t i = 0; i < nloc; ++i)
      loc[i] = locals[i];
  }
  *out_ret = Z3_mk_int64(z3, 0, bv);
  *out_valid = Z3_mk_true(z3);
  Z3_ast bytes =
      nyr_smt_memory_init_locals(z3, bv, memory->initial_bytes, locals, nloc);
  if (!bytes)
    goto unsupported;
  bool has_alloca = nyr_smt_has_alloca(f);
#define NYIR_SMT_ADD_MEMORY_VALID(width)                                       \
  do {                                                                         \
    Z3_ast valid = nyr_smt_memory_valid_for_value(                             \
        z3, bv, memory, f, has_alloca, in->a, a, (width));                     \
    if (!valid)                                                                \
      goto unsupported;                                                        \
    *out_valid = Z3_mk_and(z3, 2, (Z3_ast[]){*out_valid, valid});              \
  } while (0)
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    Z3_ast a = Z3_mk_int64(z3, 0, bv);
    Z3_ast b = Z3_mk_int64(z3, 0, bv);
    Z3_ast select_false = Z3_mk_int64(z3, 0, bv);
    if (in->a >= 0 && vals)
      a = vals[in->a];
    if (in->b >= 0 && vals)
      b = vals[in->b];
    if (in->c >= 0 && vals)
      select_false = vals[in->c];
    Z3_ast r = a;
    switch (in->op) {
    case NYIR_CONST_I64:
      r = Z3_mk_int64(z3, in->imm, bv);
      break;
    case NYIR_CONST_F64:
      r = Z3_mk_int64(z3, in->imm, bv);
      break;
    case NYIR_CONST_F32:
      r = Z3_mk_zero_ext(
          z3, 32,
          Z3_mk_int64(z3, (int64_t)(uint32_t)in->imm, Z3_mk_bv_sort(z3, 32)));
      break;
    case NYIR_ADDR_SYMBOL:
      r = nyr_smt_symbol_address(z3, bv, in->symbol);
      if (!r)
        goto unsupported;
      break;
    case NYIR_ADDR_LOCAL:
      if (in->imm < 0 || (uint64_t)in->imm >= nloc)
        goto unsupported;
      r = nyr_smt_local_base(z3, bv, (size_t)in->imm);
      break;
    case NYIR_ALLOCA:
      r = nyr_smt_alloca_base(z3, bv, i);
      break;
    case NYIR_COPY:
      r = a;
      break;
    case NYIR_LOAD_I64:
      NYIR_SMT_ADD_MEMORY_VALID(8);
      r = nyr_smt_memory_load64(z3, bv, bytes, a);
      break;
    case NYIR_STORE_I64:
      NYIR_SMT_ADD_MEMORY_VALID((in->flags & NYIR_INST_F_MEM_BYTE) ? 1 : 8);
      bytes =
          nyr_smt_memory_store64(z3, bv, bytes, a, select_false,
                                  (in->flags & NYIR_INST_F_MEM_BYTE) ? 1u : 8u);
      break;
    case NYIR_COPY_STRUCT: {
      if (in->imm <= 0 || (uint64_t)in->imm > UINT_MAX)
        goto unsupported;
      unsigned size = (unsigned)in->imm;
      Z3_ast valid = nyr_smt_copy_struct_valid(z3, bv, memory, f, has_alloca,
                                                in->a, a, in->b, b, size);
      if (!valid)
        goto unsupported;
      *out_valid = Z3_mk_and(z3, 2, (Z3_ast[]){*out_valid, valid});
      bytes = nyr_smt_memory_copy_struct(z3, bv, bytes, a, b, size);
      break;
    }
    case NYIR_VEC4_STORE_I64:
    case NYIR_VEC8_STORE_I64: {
      unsigned lanes = in->op == NYIR_VEC4_STORE_I64 ? 2u : 4u;
      Z3_ast packed = in->b >= 0 ? b : select_false;
      NYIR_SMT_ADD_MEMORY_VALID(lanes * 8);
      bytes = nyr_smt_memory_store_vec_i64(z3, bv, bytes, a, packed, lanes);
      break;
    }
    case NYIR_VEC4_LOAD_I64:
      NYIR_SMT_ADD_MEMORY_VALID(16);
      r = nyr_smt_memory_load_vec2_i64(z3, bv, bytes, a);
      break;
    case NYIR_VEC8_LOAD_I64:
      NYIR_SMT_ADD_MEMORY_VALID(32);
      r = nyr_smt_memory_load_vec4_i64(z3, bv, bytes, a);
      break;
    case NYIR_VEC4_SET1_I64:
      r = nyr_smt_pack_i64_lanes(z3, a, 2);
      break;
    case NYIR_VEC4_REDUCE_ADD_I64:
      r = nyr_smt_reduce_i64_lanes(z3, a, b, 2);
      break;
    case NYIR_VEC8_REDUCE_ADD_I64:
      r = nyr_smt_reduce_i64_lanes(z3, a, b, 4);
      break;
    case NYIR_VEC4_ADD_I64:
    case NYIR_VEC4_SUB_I64:
    case NYIR_VEC4_AND_I64:
    case NYIR_VEC4_OR_I64:
    case NYIR_VEC4_XOR_I64:
    case NYIR_VEC4_SHL_I64:
    case NYIR_VEC4_SAR_I64:
      r = nyr_smt_vector_i64_binop(z3, in->op, a, b, 2);
      break;
    case NYIR_VEC8_ADD_I64:
    case NYIR_VEC8_SUB_I64:
    case NYIR_VEC8_AND_I64:
    case NYIR_VEC8_OR_I64:
    case NYIR_VEC8_XOR_I64:
      r = nyr_smt_vector_i64_binop(z3, in->op, a, b, 4);
      break;
    case NYIR_VEC4_LOAD_F64:
      NYIR_SMT_ADD_MEMORY_VALID(32);
      r = nyr_smt_memory_load_vec4_i64(z3, bv, bytes, a);
      break;
    case NYIR_VEC4_STORE_F64: {
      Z3_ast packed = in->b >= 0 ? b : select_false;
      NYIR_SMT_ADD_MEMORY_VALID(32);
      bytes = nyr_smt_memory_store_vec_i64(z3, bv, bytes, a, packed, 4);
      break;
    }
    case NYIR_VEC4_SET1_F64:
      r = nyr_smt_pack_i64_lanes(z3, a, 4);
      break;
    case NYIR_VEC4_SHUFFLE_F64:
      r = nyr_smt_shuffle_vec4_f64(z3, a, in->imm);
      break;
    case NYIR_VEC4_REDUCE_ADD_F64:
      r = nyr_smt_reduce_f64_lanes(z3, a, b);
      break;
    case NYIR_VEC4_ADD_F64:
    case NYIR_VEC4_SUB_F64:
    case NYIR_VEC4_MUL_F64:
    case NYIR_VEC4_DIV_F64:
    case NYIR_VEC4_FMA_F64:
      r = nyr_smt_vector_f64_binop(z3, in->op, a, b, select_false);
      break;
    case NYIR_VEC8_LOAD_F32:
      NYIR_SMT_ADD_MEMORY_VALID(16);
      r = nyr_smt_memory_load_vec4_f32(z3, bv, bytes, a);
      break;
    case NYIR_VEC8_STORE_F32: {
      Z3_ast packed = in->b >= 0 ? b : select_false;
      NYIR_SMT_ADD_MEMORY_VALID(16);
      bytes = nyr_smt_memory_store_vec_f32(z3, bv, bytes, a, packed);
      break;
    }
    case NYIR_VEC8_SET1_F32:
      r = nyr_smt_pack_f32_lanes(z3, a);
      break;
    case NYIR_VEC8_SHUFFLE_F32:
      r = nyr_smt_shuffle_vec4_f32(z3, a, in->imm);
      break;
    case NYIR_VEC8_ADD_F32:
    case NYIR_VEC8_SUB_F32:
    case NYIR_VEC8_MUL_F32:
    case NYIR_VEC8_DIV_F32:
    case NYIR_VEC8_FMA_F32:
      r = nyr_smt_vector_f32_binop(z3, in->op, a, b, select_false);
      break;
    case NYIR_SELECT_I64:
      r = Z3_mk_ite(z3, Z3_mk_not(z3, Z3_mk_eq(z3, a, Z3_mk_int64(z3, 0, bv))),
                    b, select_false);
      break;
    case NYIR_CALL: {
      Z3_ast next_bytes = NULL;
      Z3_ast call_valid = NULL;
      if (!nyr_smt_call_result(z3, f, in, memory, bytes, vals, &r, &next_bytes,
                                &call_valid))
        goto unsupported;
      bytes = next_bytes;
      *out_valid = Z3_mk_and(z3, 2, (Z3_ast[]){*out_valid, call_valid});
      break;
    }
    case NYIR_CAPTURE_RET:
      if (in->imm < 0 || in->imm > 13 ||
          !nyr_smt_capture_result(z3, bv, f, in, i, memory, bytes, vals, &r))
        goto unsupported;
      break;
    case NYIR_ADD_I64:
      r = Z3_mk_bvadd(z3, a, b);
      break;
    case NYIR_SUB_I64:
      r = Z3_mk_bvsub(z3, a, b);
      break;
    case NYIR_MUL_I64:
      r = Z3_mk_bvmul(z3, a, b);
      break;
    case NYIR_DIV_I64:
      *out_valid = Z3_mk_and(
          z3, 2, (Z3_ast[]){*out_valid, nyr_smt_div_defined(z3, bv, a, b)});
      r = Z3_mk_bvsdiv(z3, a, b);
      break;
    case NYIR_MOD_I64:
      *out_valid = Z3_mk_and(
          z3, 2, (Z3_ast[]){*out_valid, nyr_smt_div_defined(z3, bv, a, b)});
      r = Z3_mk_bvsrem(z3, a, b);
      break;
    case NYIR_AND_I64:
      r = Z3_mk_bvand(z3, a, b);
      break;
    case NYIR_OR_I64:
      r = Z3_mk_bvor(z3, a, b);
      break;
    case NYIR_XOR_I64:
      r = Z3_mk_bvxor(z3, a, b);
      break;
    case NYIR_SHL_I64:
      r = Z3_mk_bvshl(z3, a, nyr_smt_i64_shift_count(z3, bv, b));
      break;
    case NYIR_SAR_I64:
      r = Z3_mk_bvashr(z3, a, nyr_smt_i64_shift_count(z3, bv, b));
      break;
    case NYIR_ROR32_I64:
      r = nyr_smt_ror(z3, a, b, 32);
      break;
    case NYIR_ROR_I64:
      r = nyr_smt_ror(z3, a, b, 64);
      break;
    case NYIR_BSWAP_I64:
      r = nyr_smt_bswap(z3, a, 64);
      break;
    case NYIR_BSWAP32_I64:
      r = nyr_smt_bswap(z3, a, 32);
      break;
    case NYIR_F64_TO_I64:
    case NYIR_F32_TO_I64: {
      Z3_ast value = in->op == NYIR_F64_TO_I64 ? ny_z3_f64_from_bits(z3, a)
                                               : ny_z3_f32_from_bits(z3, a);
      *out_valid = Z3_mk_and(
          z3, 2, (Z3_ast[]){*out_valid, nyr_smt_fp_to_i64_defined(z3, value)});
      r = Z3_mk_fpa_to_sbv(z3, Z3_mk_fpa_round_toward_zero(z3), value, 64);
      break;
    }
    /*
     * Float arithmetic and conversions mirror the CFG encoding exactly:
     * IEEE round-nearest-ties-to-even via the fpa theory, bit patterns as
     * the carrier.  These cases let straightline i64-returning rewrites with
     * float intermediates (scaling-then-truncate idioms) reach the solver
     * instead of the bounded interpreter.
     */
    case NYIR_ADD_F64:
    case NYIR_SUB_F64:
    case NYIR_MUL_F64:
    case NYIR_DIV_F64: {
      Z3_ast fa = ny_z3_f64_from_bits(z3, a);
      Z3_ast fb = ny_z3_f64_from_bits(z3, b);
      Z3_ast rm = Z3_mk_fpa_round_nearest_ties_to_even(z3);
      Z3_ast value = in->op == NYIR_ADD_F64   ? Z3_mk_fpa_add(z3, rm, fa, fb)
                     : in->op == NYIR_SUB_F64 ? Z3_mk_fpa_sub(z3, rm, fa, fb)
                     : in->op == NYIR_MUL_F64 ? Z3_mk_fpa_mul(z3, rm, fa, fb)
                                              : Z3_mk_fpa_div(z3, rm, fa, fb);
      r = ny_z3_f64_to_bits(z3, value);
      if (ny_trace_enabled("NY_TRACE_TV_SMT"))
        ny_trace_line("TV_SMT", "scalar %s lhs=%u rhs=%u result=%u bits=%u",
                      nyr_op_name(in->op), Z3_get_ast_id(z3, a),
                      Z3_get_ast_id(z3, b), Z3_get_ast_id(z3, value),
                      Z3_get_ast_id(z3, r));
      break;
    }
    case NYIR_ADD_F32:
    case NYIR_SUB_F32:
    case NYIR_MUL_F32:
    case NYIR_DIV_F32: {
      Z3_ast fa = ny_z3_f32_from_bits(z3, a);
      Z3_ast fb = ny_z3_f32_from_bits(z3, b);
      Z3_ast rm = Z3_mk_fpa_round_nearest_ties_to_even(z3);
      Z3_ast value = in->op == NYIR_ADD_F32   ? Z3_mk_fpa_add(z3, rm, fa, fb)
                     : in->op == NYIR_SUB_F32 ? Z3_mk_fpa_sub(z3, rm, fa, fb)
                     : in->op == NYIR_MUL_F32 ? Z3_mk_fpa_mul(z3, rm, fa, fb)
                                              : Z3_mk_fpa_div(z3, rm, fa, fb);
      r = ny_z3_f32_to_bits64(z3, value);
      break;
    }
    case NYIR_SQRT_F64: {
      Z3_ast value =
          Z3_mk_fpa_sqrt(z3, Z3_mk_fpa_round_nearest_ties_to_even(z3),
                         ny_z3_f64_from_bits(z3, a));
      r = ny_z3_f64_to_bits(z3, value);
      break;
    }
    case NYIR_SIN_F64:
      r = nyr_smt_uninterpreted_unary_f64(z3, bv, "nyir.sin.f64.bits", a);
      break;
    case NYIR_COS_F64:
      r = nyr_smt_uninterpreted_unary_f64(z3, bv, "nyir.cos.f64.bits", a);
      break;
    case NYIR_CMP_F64:
    case NYIR_CMP_F32: {
      Z3_ast fa = in->op == NYIR_CMP_F64 ? ny_z3_f64_from_bits(z3, a)
                                         : ny_z3_f32_from_bits(z3, a);
      Z3_ast fb = in->op == NYIR_CMP_F64 ? ny_z3_f64_from_bits(z3, b)
                                         : ny_z3_f32_from_bits(z3, b);
      Z3_ast condition = ny_z3_fp_cmp(z3, in->cmp, fa, fb);
      if (!condition) {
        free(vals);
        free(loc);
        return false;
      }
      r = Z3_mk_ite(z3, condition, Z3_mk_int64(z3, 1, bv),
                    Z3_mk_int64(z3, 0, bv));
      break;
    }
    case NYIR_I64_TO_F64: {
      Z3_ast value =
          Z3_mk_fpa_to_fp_signed(z3, Z3_mk_fpa_round_nearest_ties_to_even(z3),
                                 a, Z3_mk_fpa_sort_64(z3));
      r = ny_z3_f64_to_bits(z3, value);
      break;
    }
    case NYIR_I64_TO_F32: {
      Z3_ast value =
          Z3_mk_fpa_to_fp_signed(z3, Z3_mk_fpa_round_nearest_ties_to_even(z3),
                                 a, Z3_mk_fpa_sort_32(z3));
      r = ny_z3_f32_to_bits64(z3, value);
      break;
    }
    case NYIR_F64_TO_F32: {
      Z3_ast value = Z3_mk_fpa_to_fp_float(
          z3, Z3_mk_fpa_round_nearest_ties_to_even(z3),
          ny_z3_f64_from_bits(z3, a), Z3_mk_fpa_sort_32(z3));
      r = ny_z3_f32_to_bits64(z3, value);
      break;
    }
    case NYIR_F32_TO_F64: {
      Z3_ast value = Z3_mk_fpa_to_fp_float(
          z3, Z3_mk_fpa_round_nearest_ties_to_even(z3),
          ny_z3_f32_from_bits(z3, a), Z3_mk_fpa_sort_64(z3));
      r = ny_z3_f64_to_bits(z3, value);
      break;
    }
    case NYIR_CMP_I64: {
      Z3_ast c = Z3_mk_true(z3);
      switch (in->cmp) {
      case NYIR_CMP_EQ:
        c = Z3_mk_eq(z3, a, b);
        break;
      case NYIR_CMP_NE:
        c = Z3_mk_not(z3, Z3_mk_eq(z3, a, b));
        break;
      case NYIR_CMP_LT:
        c = Z3_mk_bvslt(z3, a, b);
        break;
      case NYIR_CMP_LE:
        c = Z3_mk_bvsle(z3, a, b);
        break;
      case NYIR_CMP_GT:
        c = Z3_mk_bvsgt(z3, a, b);
        break;
      case NYIR_CMP_GE:
        c = Z3_mk_bvsge(z3, a, b);
        break;
      }
      r = Z3_mk_ite(z3, c, Z3_mk_int64(z3, 1, bv), Z3_mk_int64(z3, 0, bv));
      break;
    }
    case NYIR_LOAD_LOCAL:
      if (in->imm < 0 || (uint64_t)in->imm >= nloc)
        goto unsupported;
      r = nyr_smt_memory_load64(z3, bv, bytes,
                                 nyr_smt_local_base(z3, bv, (size_t)in->imm));
      break;
    case NYIR_STORE_LOCAL:
      if (in->imm < 0 || (uint64_t)in->imm >= nloc || !loc || in->a < 0 ||
          !vals || !vals[in->a])
        goto unsupported;
      loc[in->imm] = vals[in->a];
      bytes = nyr_smt_memory_store64(
          z3, bv, bytes, nyr_smt_local_base(z3, bv, (size_t)in->imm),
          vals[in->a], 8);
      continue;
    case NYIR_BOUNDS_CHECK: {
      Z3_ast byte_len =
          in->c >= 0 && vals ? vals[in->c] : Z3_mk_int64(z3, in->imm, bv);
      *out_valid = Z3_mk_and(
          z3, 2,
          (Z3_ast[]){*out_valid, nyr_smt_bounds_defined(z3, bv, b, byte_len)});
      continue;
    }
    case NYIR_RET:
      if (in->a >= 0 && vals)
        *out_ret = vals[in->a];
      *out_ret = Z3_simplify(z3, *out_ret);
      *out_valid = Z3_simplify(z3, *out_valid);
      free(vals);
      free(loc);
      return true;
    case NYIR_NOP:
      continue;
    default:
      free(vals);
      free(loc);
      return false;
    }
    if (in->dst >= 0 && vals)
      vals[in->dst] = r;
  }
  free(vals);
  free(loc);
  return true;
unsupported:
  free(vals);
  free(loc);
  return false;
#undef NYIR_SMT_ADD_MEMORY_VALID
}
#endif

#ifdef NYTRIX_HAS_Z3
typedef struct {
  Z3_ast *values;
  Z3_ast *locals;
  ny_smt_memory_t memory;
  Z3_ast bytes;
  Z3_ast path;
  size_t pc;
  size_t steps;
  size_t nloc;
  unsigned backedges;
  int64_t current_label;
  int64_t predecessor_label;
} ny_smt_path_t;

typedef struct {
  Z3_ast valid;
  Z3_ast value;
  size_t paths;
  bool complete;
} ny_smt_result_t;

static Z3_ast ny_z3_and2(Z3_context z3, Z3_ast a, Z3_ast b) {
  Z3_ast xs[2] = {a, b};
  return Z3_mk_and(z3, 2, xs);
}

static Z3_ast ny_z3_or2(Z3_context z3, Z3_ast a, Z3_ast b) {
  Z3_ast xs[2] = {a, b};
  return Z3_mk_or(z3, 2, xs);
}

static Z3_ast ny_z3_nonzero(Z3_context z3, Z3_sort bv, Z3_ast v) {
  return Z3_mk_not(z3, Z3_mk_eq(z3, v, Z3_mk_int64(z3, 0, bv)));
}

static Z3_ast ny_z3_f64_from_bits(Z3_context z3, Z3_ast bits) {
  return Z3_mk_fpa_to_fp_bv(z3, bits, Z3_mk_fpa_sort_64(z3));
}

static Z3_ast ny_z3_f32_from_bits(Z3_context z3, Z3_ast bits) {
  Z3_ast low = Z3_mk_extract(z3, 31, 0, bits);
  return Z3_mk_fpa_to_fp_bv(z3, low, Z3_mk_fpa_sort_32(z3));
}

static Z3_ast ny_z3_f64_to_bits(Z3_context z3, Z3_ast value) {
  return Z3_mk_fpa_to_ieee_bv(z3, value);
}

static Z3_ast ny_z3_f32_to_bits64(Z3_context z3, Z3_ast value) {
  Z3_ast bits = Z3_mk_fpa_to_ieee_bv(z3, value);
  return Z3_mk_zero_ext(z3, 32, bits);
}

static Z3_ast ny_z3_fp_cmp(Z3_context z3, nyr_cmp_t cmp, Z3_ast a, Z3_ast b) {
  Z3_ast condition = NULL;
  switch (cmp) {
  case NYIR_CMP_EQ:
    condition = Z3_mk_fpa_eq(z3, a, b);
    break;
  case NYIR_CMP_NE:
    condition = Z3_mk_not(z3, Z3_mk_fpa_eq(z3, a, b));
    break;
  case NYIR_CMP_LT:
    condition = Z3_mk_fpa_lt(z3, a, b);
    break;
  case NYIR_CMP_LE:
    condition = Z3_mk_fpa_leq(z3, a, b);
    break;
  case NYIR_CMP_GT:
    condition = Z3_mk_fpa_gt(z3, a, b);
    break;
  case NYIR_CMP_GE:
    condition = Z3_mk_fpa_geq(z3, a, b);
    break;
  }
  return condition;
}

static bool ny_smt_clone_path(const nyr_func_t *f, size_t nloc,
                              const ny_smt_path_t *src, ny_smt_path_t *dst) {
  *dst = *src;
  if (f->next_value > 0) {
    dst->values = malloc((size_t)f->next_value * sizeof(*dst->values));
    if (!dst->values)
      return false;
    memcpy(dst->values, src->values,
           (size_t)f->next_value * sizeof(*dst->values));
  }
  if (nloc) {
    dst->locals = malloc(nloc * sizeof(*dst->locals));
    if (!dst->locals) {
      free(dst->values);
      dst->values = NULL;
      return false;
    }
    memcpy(dst->locals, src->locals, nloc * sizeof(*dst->locals));
  }
  return true;
}

static void ny_smt_free_path(ny_smt_path_t *path) {
  if (!path)
    return;
  free(path->values);
  free(path->locals);
  path->values = NULL;
  path->locals = NULL;
}

static size_t ny_smt_label_pc(const nyr_func_t *f, int64_t label) {
  for (size_t i = 0; i < f->len; ++i)
    if (f->data[i].op == NYIR_LABEL && f->data[i].imm == label)
      return i;
  return SIZE_MAX;
}

static bool ny_smt_record_return(Z3_context z3, ny_smt_result_t *out,
                                 Z3_ast path, Z3_ast value) {
  if (!out || out->paths >= 4096)
    return false;
  out->value = Z3_mk_ite(z3, path, value, out->value);
  out->valid = ny_z3_or2(z3, out->valid, path);
  out->paths++;
  return true;
}

static bool nyr_smt_walk_cfg(Z3_context z3, Z3_sort bv, const nyr_func_t *f,
                              bool has_alloca, size_t nloc,
                              const bool *memory_locals, ny_smt_path_t *st,
                              ny_smt_result_t *out) {
  const size_t step_limit = f->len * 12u + 64u;
#define NYIR_SMT_CFG_MEMORY_VALID(width)                                       \
  do {                                                                         \
    Z3_ast valid = nyr_smt_memory_valid_for_value(                             \
        z3, bv, &st->memory, f, has_alloca, in->a, a, (width));                \
    if (!valid)                                                                \
      return false;                                                            \
    st->path = ny_z3_and2(z3, st->path, valid);                                \
  } while (0)
  while (st->pc < f->len) {
    if (++st->steps > step_limit) {
      out->complete = false;
      return true;
    }
    size_t inst_index = st->pc;
    const nyr_inst_t *in = &f->data[st->pc++];
    Z3_ast zero = Z3_mk_int64(z3, 0, bv);
    Z3_ast one = Z3_mk_int64(z3, 1, bv);
    Z3_ast a = zero, b = zero, select_false = zero, r = zero;
    if (in->a >= 0) {
      if (!st->values || !st->values[in->a])
        return false;
      a = st->values[in->a];
    }
    if (in->b >= 0) {
      if (!st->values || !st->values[in->b])
        return false;
      b = st->values[in->b];
    }
    if (in->c >= 0) {
      if (!st->values || !st->values[in->c])
        return false;
      select_false = st->values[in->c];
    }
    switch (in->op) {
    case NYIR_NOP:
      continue;
    case NYIR_BOUNDS_CHECK: {
      Z3_ast byte_len =
          in->c >= 0 ? select_false : Z3_mk_int64(z3, in->imm, bv);
      st->path = ny_z3_and2(z3, st->path,
                            nyr_smt_bounds_defined(z3, bv, b, byte_len));
      continue;
    }
    case NYIR_LABEL:
      st->predecessor_label = st->current_label;
      st->current_label = in->imm;
      continue;
    case NYIR_PHI: {
      bool found = false;
      for (size_t k = 0; k < in->phi_incoming_len; ++k) {
        if (in->phi_incoming[k].predecessor_label != st->predecessor_label)
          continue;
        int v = in->phi_incoming[k].value;
        if (v < 0 || !st->values || !st->values[v])
          return false;
        r = st->values[v];
        found = true;
        break;
      }
      if (!found)
        return false;
      break;
    }
    case NYIR_CONST_I64:
      r = Z3_mk_int64(z3, in->imm, bv);
      break;
    case NYIR_CONST_F64:
      r = Z3_mk_int64(z3, in->imm, bv);
      break;
    case NYIR_CONST_F32:
      r = Z3_mk_zero_ext(
          z3, 32,
          Z3_mk_int64(z3, (int64_t)(uint32_t)in->imm, Z3_mk_bv_sort(z3, 32)));
      break;
    case NYIR_ADDR_SYMBOL:
      r = nyr_smt_symbol_address(z3, bv, in->symbol);
      if (!r)
        return false;
      break;
    case NYIR_ADDR_LOCAL:
      if (in->imm < 0 || (uint64_t)in->imm >= nloc)
        return false;
      r = nyr_smt_local_base(z3, bv, (size_t)in->imm);
      break;
    case NYIR_ALLOCA:
      if (inst_index >= f->len)
        return false;
      r = nyr_smt_alloca_base(z3, bv, inst_index);
      break;
    case NYIR_COPY:
      r = a;
      break;
    case NYIR_LOAD_I64:
      NYIR_SMT_CFG_MEMORY_VALID(8);
      r = nyr_smt_memory_load64(z3, bv, st->bytes, a);
      break;
    case NYIR_STORE_I64:
      NYIR_SMT_CFG_MEMORY_VALID((in->flags & NYIR_INST_F_MEM_BYTE) ? 1 : 8);
      st->bytes =
          nyr_smt_memory_store64(z3, bv, st->bytes, a, select_false,
                                  (in->flags & NYIR_INST_F_MEM_BYTE) ? 1u : 8u);
      break;
    case NYIR_COPY_STRUCT: {
      if (in->imm <= 0 || (uint64_t)in->imm > UINT_MAX)
        return false;
      unsigned size = (unsigned)in->imm;
      Z3_ast valid = nyr_smt_copy_struct_valid(
          z3, bv, &st->memory, f, has_alloca, in->a, a, in->b, b, size);
      if (!valid)
        return false;
      st->path = ny_z3_and2(z3, st->path, valid);
      st->bytes = nyr_smt_memory_copy_struct(z3, bv, st->bytes, a, b, size);
      break;
    }
    case NYIR_VEC4_STORE_I64:
    case NYIR_VEC8_STORE_I64: {
      unsigned lanes = in->op == NYIR_VEC4_STORE_I64 ? 2u : 4u;
      Z3_ast packed = in->b >= 0 ? b : select_false;
      NYIR_SMT_CFG_MEMORY_VALID(lanes * 8);
      st->bytes =
          nyr_smt_memory_store_vec_i64(z3, bv, st->bytes, a, packed, lanes);
      break;
    }
    case NYIR_VEC4_LOAD_I64:
      NYIR_SMT_CFG_MEMORY_VALID(16);
      r = nyr_smt_memory_load_vec2_i64(z3, bv, st->bytes, a);
      break;
    case NYIR_VEC8_LOAD_I64:
      NYIR_SMT_CFG_MEMORY_VALID(32);
      r = nyr_smt_memory_load_vec4_i64(z3, bv, st->bytes, a);
      break;
    case NYIR_VEC4_SET1_I64:
      r = nyr_smt_pack_i64_lanes(z3, a, 2);
      break;
    case NYIR_VEC4_REDUCE_ADD_I64:
      r = nyr_smt_reduce_i64_lanes(z3, a, b, 2);
      break;
    case NYIR_VEC8_REDUCE_ADD_I64:
      r = nyr_smt_reduce_i64_lanes(z3, a, b, 4);
      break;
    case NYIR_VEC4_ADD_I64:
    case NYIR_VEC4_SUB_I64:
    case NYIR_VEC4_AND_I64:
    case NYIR_VEC4_OR_I64:
    case NYIR_VEC4_XOR_I64:
    case NYIR_VEC4_SHL_I64:
    case NYIR_VEC4_SAR_I64:
      r = nyr_smt_vector_i64_binop(z3, in->op, a, b, 2);
      break;
    case NYIR_VEC8_ADD_I64:
    case NYIR_VEC8_SUB_I64:
    case NYIR_VEC8_AND_I64:
    case NYIR_VEC8_OR_I64:
    case NYIR_VEC8_XOR_I64:
      r = nyr_smt_vector_i64_binop(z3, in->op, a, b, 4);
      break;
    case NYIR_VEC4_LOAD_F64:
      NYIR_SMT_CFG_MEMORY_VALID(32);
      r = nyr_smt_memory_load_vec4_i64(z3, bv, st->bytes, a);
      break;
    case NYIR_VEC4_STORE_F64: {
      Z3_ast packed = in->b >= 0 ? b : select_false;
      NYIR_SMT_CFG_MEMORY_VALID(32);
      st->bytes =
          nyr_smt_memory_store_vec_i64(z3, bv, st->bytes, a, packed, 4);
      continue;
    }
    case NYIR_VEC4_SET1_F64:
      r = nyr_smt_pack_i64_lanes(z3, a, 4);
      break;
    case NYIR_VEC4_SHUFFLE_F64:
      r = nyr_smt_shuffle_vec4_f64(z3, a, in->imm);
      break;
    case NYIR_VEC4_REDUCE_ADD_F64:
      r = nyr_smt_reduce_f64_lanes(z3, a, b);
      break;
    case NYIR_VEC4_ADD_F64:
    case NYIR_VEC4_SUB_F64:
    case NYIR_VEC4_MUL_F64:
    case NYIR_VEC4_DIV_F64:
    case NYIR_VEC4_FMA_F64:
      r = nyr_smt_vector_f64_binop(z3, in->op, a, b, select_false);
      break;
    case NYIR_VEC8_LOAD_F32:
      NYIR_SMT_CFG_MEMORY_VALID(16);
      r = nyr_smt_memory_load_vec4_f32(z3, bv, st->bytes, a);
      break;
    case NYIR_VEC8_STORE_F32: {
      Z3_ast packed = in->b >= 0 ? b : select_false;
      NYIR_SMT_CFG_MEMORY_VALID(16);
      st->bytes = nyr_smt_memory_store_vec_f32(z3, bv, st->bytes, a, packed);
      break;
    }
    case NYIR_VEC8_SET1_F32:
      r = nyr_smt_pack_f32_lanes(z3, a);
      break;
    case NYIR_VEC8_SHUFFLE_F32:
      r = nyr_smt_shuffle_vec4_f32(z3, a, in->imm);
      break;
    case NYIR_VEC8_ADD_F32:
    case NYIR_VEC8_SUB_F32:
    case NYIR_VEC8_MUL_F32:
    case NYIR_VEC8_DIV_F32:
    case NYIR_VEC8_FMA_F32:
      r = nyr_smt_vector_f32_binop(z3, in->op, a, b, select_false);
      break;
    case NYIR_SELECT_I64:
      r = Z3_mk_ite(z3, ny_z3_nonzero(z3, bv, a), b, select_false);
      break;
    case NYIR_CALL: {
      Z3_ast call_valid = NULL;
      if (!nyr_smt_call_result(z3, f, in, &st->memory, st->bytes, st->values,
                                &r, &st->bytes, &call_valid))
        return false;
      st->path = ny_z3_and2(z3, st->path, call_valid);
      break;
    }
    case NYIR_CAPTURE_RET:
      if (in->imm < 0 || in->imm > 13 ||
          !nyr_smt_capture_result(z3, bv, f, in, inst_index, &st->memory,
                                   st->bytes, st->values, &r))
        return false;
      break;
    case NYIR_ADD_I64:
      r = Z3_mk_bvadd(z3, a, b);
      break;
    case NYIR_SUB_I64:
      r = Z3_mk_bvsub(z3, a, b);
      break;
    case NYIR_MUL_I64:
      r = Z3_mk_bvmul(z3, a, b);
      break;
    case NYIR_DIV_I64:
    case NYIR_MOD_I64:
      st->path = ny_z3_and2(z3, st->path, nyr_smt_div_defined(z3, bv, a, b));
      r = in->op == NYIR_DIV_I64 ? Z3_mk_bvsdiv(z3, a, b)
                                 : Z3_mk_bvsrem(z3, a, b);
      break;
    case NYIR_AND_I64:
      r = Z3_mk_bvand(z3, a, b);
      break;
    case NYIR_OR_I64:
      r = Z3_mk_bvor(z3, a, b);
      break;
    case NYIR_XOR_I64:
      r = Z3_mk_bvxor(z3, a, b);
      break;
    case NYIR_SHL_I64:
      r = Z3_mk_bvshl(z3, a, nyr_smt_i64_shift_count(z3, bv, b));
      break;
    case NYIR_SAR_I64:
      r = Z3_mk_bvashr(z3, a, nyr_smt_i64_shift_count(z3, bv, b));
      break;
    case NYIR_ROR32_I64:
      r = nyr_smt_ror(z3, a, b, 32);
      break;
    case NYIR_ROR_I64:
      r = nyr_smt_ror(z3, a, b, 64);
      break;
    case NYIR_BSWAP_I64:
      r = nyr_smt_bswap(z3, a, 64);
      break;
    case NYIR_BSWAP32_I64:
      r = nyr_smt_bswap(z3, a, 32);
      break;
    case NYIR_CMP_I64: {
      Z3_ast c = Z3_mk_true(z3);
      switch (in->cmp) {
      case NYIR_CMP_EQ:
        c = Z3_mk_eq(z3, a, b);
        break;
      case NYIR_CMP_NE:
        c = Z3_mk_not(z3, Z3_mk_eq(z3, a, b));
        break;
      case NYIR_CMP_LT:
        c = Z3_mk_bvslt(z3, a, b);
        break;
      case NYIR_CMP_LE:
        c = Z3_mk_bvsle(z3, a, b);
        break;
      case NYIR_CMP_GT:
        c = Z3_mk_bvsgt(z3, a, b);
        break;
      case NYIR_CMP_GE:
        c = Z3_mk_bvsge(z3, a, b);
        break;
      }
      r = Z3_mk_ite(z3, c, one, zero);
      break;
    }
    case NYIR_ADD_F64:
    case NYIR_SUB_F64:
    case NYIR_MUL_F64:
    case NYIR_DIV_F64: {
      Z3_ast fa = ny_z3_f64_from_bits(z3, a);
      Z3_ast fb = ny_z3_f64_from_bits(z3, b);
      Z3_ast rm = Z3_mk_fpa_round_nearest_ties_to_even(z3);
      Z3_ast value = in->op == NYIR_ADD_F64   ? Z3_mk_fpa_add(z3, rm, fa, fb)
                     : in->op == NYIR_SUB_F64 ? Z3_mk_fpa_sub(z3, rm, fa, fb)
                     : in->op == NYIR_MUL_F64 ? Z3_mk_fpa_mul(z3, rm, fa, fb)
                                              : Z3_mk_fpa_div(z3, rm, fa, fb);
      r = ny_z3_f64_to_bits(z3, value);
      break;
    }
    case NYIR_ADD_F32:
    case NYIR_SUB_F32:
    case NYIR_MUL_F32:
    case NYIR_DIV_F32: {
      Z3_ast fa = ny_z3_f32_from_bits(z3, a);
      Z3_ast fb = ny_z3_f32_from_bits(z3, b);
      Z3_ast rm = Z3_mk_fpa_round_nearest_ties_to_even(z3);
      Z3_ast value = in->op == NYIR_ADD_F32   ? Z3_mk_fpa_add(z3, rm, fa, fb)
                     : in->op == NYIR_SUB_F32 ? Z3_mk_fpa_sub(z3, rm, fa, fb)
                     : in->op == NYIR_MUL_F32 ? Z3_mk_fpa_mul(z3, rm, fa, fb)
                                              : Z3_mk_fpa_div(z3, rm, fa, fb);
      r = ny_z3_f32_to_bits64(z3, value);
      break;
    }
    case NYIR_SQRT_F64: {
      Z3_ast value =
          Z3_mk_fpa_sqrt(z3, Z3_mk_fpa_round_nearest_ties_to_even(z3),
                         ny_z3_f64_from_bits(z3, a));
      r = ny_z3_f64_to_bits(z3, value);
      break;
    }
    case NYIR_SIN_F64:
      r = nyr_smt_uninterpreted_unary_f64(z3, bv, "nyir.sin.f64.bits", a);
      break;
    case NYIR_COS_F64:
      r = nyr_smt_uninterpreted_unary_f64(z3, bv, "nyir.cos.f64.bits", a);
      break;
    case NYIR_CMP_F64:
    case NYIR_CMP_F32: {
      Z3_ast fa = in->op == NYIR_CMP_F64 ? ny_z3_f64_from_bits(z3, a)
                                         : ny_z3_f32_from_bits(z3, a);
      Z3_ast fb = in->op == NYIR_CMP_F64 ? ny_z3_f64_from_bits(z3, b)
                                         : ny_z3_f32_from_bits(z3, b);
      Z3_ast condition = ny_z3_fp_cmp(z3, in->cmp, fa, fb);
      if (!condition)
        return false;
      r = Z3_mk_ite(z3, condition, one, zero);
      break;
    }
    case NYIR_I64_TO_F64: {
      Z3_ast value =
          Z3_mk_fpa_to_fp_signed(z3, Z3_mk_fpa_round_nearest_ties_to_even(z3),
                                 a, Z3_mk_fpa_sort_64(z3));
      r = ny_z3_f64_to_bits(z3, value);
      break;
    }
    case NYIR_I64_TO_F32: {
      Z3_ast value =
          Z3_mk_fpa_to_fp_signed(z3, Z3_mk_fpa_round_nearest_ties_to_even(z3),
                                 a, Z3_mk_fpa_sort_32(z3));
      r = ny_z3_f32_to_bits64(z3, value);
      break;
    }
    case NYIR_F64_TO_F32: {
      Z3_ast value = Z3_mk_fpa_to_fp_float(
          z3, Z3_mk_fpa_round_nearest_ties_to_even(z3),
          ny_z3_f64_from_bits(z3, a), Z3_mk_fpa_sort_32(z3));
      r = ny_z3_f32_to_bits64(z3, value);
      break;
    }
    case NYIR_F32_TO_F64: {
      Z3_ast value = Z3_mk_fpa_to_fp_float(
          z3, Z3_mk_fpa_round_nearest_ties_to_even(z3),
          ny_z3_f32_from_bits(z3, a), Z3_mk_fpa_sort_64(z3));
      r = ny_z3_f64_to_bits(z3, value);
      break;
    }
    case NYIR_F64_TO_I64:
    case NYIR_F32_TO_I64: {
      Z3_ast value = in->op == NYIR_F64_TO_I64 ? ny_z3_f64_from_bits(z3, a)
                                               : ny_z3_f32_from_bits(z3, a);
      st->path =
          ny_z3_and2(z3, st->path, nyr_smt_fp_to_i64_defined(z3, value));
      r = Z3_mk_fpa_to_sbv(z3, Z3_mk_fpa_round_toward_zero(z3), value, 64);
      break;
    }
    case NYIR_LOAD_LOCAL:
      if (in->imm < 0 || (size_t)in->imm >= nloc || !st->locals)
        return false;
      if (!memory_locals || !memory_locals[in->imm])
        r = st->locals[in->imm];
      else
        r = nyr_smt_memory_load64(
            z3, bv, st->bytes,
            nyr_smt_local_base(z3, bv, (size_t)in->imm));
      break;
    case NYIR_STORE_LOCAL:
      if (in->imm < 0 || (size_t)in->imm >= nloc || !st->locals)
        return false;
      st->locals[in->imm] = a;
      st->bytes = nyr_smt_memory_store64(
          z3, bv, st->bytes, nyr_smt_local_base(z3, bv, (size_t)in->imm), a,
          8);
      continue;
    case NYIR_BR: {
      size_t target = ny_smt_label_pc(f, in->imm);
      if (target == SIZE_MAX)
        return false;
      if (target < st->pc && ++st->backedges > NYIR_SMT_CFG_MAX_BACKEDGES) {
        out->complete = false;
        return true;
      }
      st->pc = target;
      continue;
    }
    case NYIR_BR_IF: {
      size_t target = ny_smt_label_pc(f, in->imm);
      if (target == SIZE_MAX)
        return false;
      Z3_ast cond = Z3_simplify(z3, ny_z3_nonzero(z3, bv, a));
      Z3_lbool known = Z3_get_bool_value(z3, cond);
      if (known == Z3_L_FALSE)
        continue;
      if (known == Z3_L_TRUE) {
        if (target < st->pc && ++st->backedges > NYIR_SMT_CFG_MAX_BACKEDGES) {
          out->complete = false;
          return true;
        }
        st->pc = target;
        continue;
      }
      ny_smt_path_t taken = {0};
      if (!ny_smt_clone_path(f, nloc, st, &taken))
        return false;
      taken.path = ny_z3_and2(z3, taken.path, cond);
      if (target < st->pc && ++taken.backedges > NYIR_SMT_CFG_MAX_BACKEDGES)
        out->complete = false;
      else {
        taken.pc = target;
        if (!nyr_smt_walk_cfg(z3, bv, f, has_alloca, nloc, memory_locals,
                               &taken, out)) {
          ny_smt_free_path(&taken);
          return false;
        }
      }
      ny_smt_free_path(&taken);
      st->path = Z3_simplify(z3, ny_z3_and2(z3, st->path, Z3_mk_not(z3, cond)));
      continue;
    }
    case NYIR_RET:
      return ny_smt_record_return(z3, out, st->path, in->a >= 0 ? a : zero);
    default:
      return false;
    }
    r = Z3_simplify(z3, r);
    if (in->dst >= 0) {
      if (!st->values)
        return false;
      st->values[in->dst] = r;
    }
  }
#undef NYIR_SMT_CFG_MEMORY_VALID
  return true;
}

static bool nyr_smt_eval_cfg(Z3_context z3, Z3_sort bv, const nyr_func_t *f,
                              const ny_smt_memory_t *memory, Z3_ast *locals,
                              size_t nloc, ny_smt_result_t *out) {
  *out = (ny_smt_result_t){.valid = Z3_mk_false(z3),
                           .value = Z3_mk_int64(z3, 0, bv),
                           .complete = true};
  ny_smt_path_t st = {.memory = *memory,
                      .bytes = memory->initial_bytes,
                      .path = Z3_mk_true(z3),
                      .nloc = nloc,
                      .current_label = -1,
                      .predecessor_label = -1};
  if (f->next_value > 0) {
    st.values = calloc((size_t)f->next_value, sizeof(*st.values));
    if (!st.values)
      return false;
  }
  if (nloc) {
    st.locals = malloc(nloc * sizeof(*st.locals));
    if (!st.locals) {
      ny_smt_free_path(&st);
      return false;
    }
    memcpy(st.locals, locals, nloc * sizeof(*st.locals));
  }
  bool *memory_locals = nloc ? calloc(nloc, sizeof(*memory_locals)) : NULL;
  if (nloc && !memory_locals) {
    ny_smt_free_path(&st);
    return false;
  }
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op == NYIR_ADDR_LOCAL && in->imm >= 0 &&
        (size_t)in->imm < nloc)
      memory_locals[in->imm] = true;
  }
  st.bytes = nyr_smt_memory_init_locals(z3, bv, st.bytes, st.locals, nloc);
  if (!st.bytes) {
    free(memory_locals);
    ny_smt_free_path(&st);
    return false;
  }
  bool has_alloca = nyr_smt_has_alloca(f);
  bool ok = nyr_smt_walk_cfg(z3, bv, f, has_alloca, nloc, memory_locals, &st,
                              out);
  free(memory_locals);
  ny_smt_free_path(&st);
  return ok && out->paths > 0;
}

static bool nyr_smt_fp_opcode(nyr_op_t op) {
  switch (op) {
  case NYIR_CONST_F64:
  case NYIR_ADD_F64:
  case NYIR_SUB_F64:
  case NYIR_MUL_F64:
  case NYIR_DIV_F64:
  case NYIR_I64_TO_F64:
  case NYIR_F64_TO_F32:
  case NYIR_F32_TO_F64:
  case NYIR_CMP_F64:
  case NYIR_SQRT_F64:
  case NYIR_CONST_F32:
  case NYIR_ADD_F32:
  case NYIR_SUB_F32:
  case NYIR_MUL_F32:
  case NYIR_DIV_F32:
  case NYIR_VEC4_ADD_F64:
  case NYIR_VEC4_SUB_F64:
  case NYIR_VEC4_MUL_F64:
  case NYIR_VEC4_DIV_F64:
  case NYIR_VEC4_FMA_F64:
  case NYIR_VEC4_REDUCE_ADD_F64:
  case NYIR_VEC8_ADD_F32:
  case NYIR_VEC8_SUB_F32:
  case NYIR_VEC8_MUL_F32:
  case NYIR_VEC8_DIV_F32:
  case NYIR_VEC8_FMA_F32:
  case NYIR_VEC8_SET1_F32:
  case NYIR_VEC8_SHUFFLE_F32:
  case NYIR_I64_TO_F32:
  case NYIR_F64_TO_I64:
  case NYIR_F32_TO_I64:
  case NYIR_CMP_F32:
    return true;
  default:
    return false;
  }
}

static bool nyr_smt_returns_fp(const nyr_func_t *f) {
  bool has_fp = false;
  for (size_t i = 0; i < f->len; ++i)
    has_fp |= nyr_smt_fp_opcode(f->data[i].op);
  if (!has_fp)
    return false;
  size_t value_count = f->next_value > 0 ? (size_t)f->next_value : 0;
  if (!value_count)
    return true;
  const nyr_inst_t **defs = calloc(value_count, sizeof(*defs));
  int *work = malloc(value_count * sizeof(*work));
  bool *seen = calloc(value_count, sizeof(*seen));
  if (!defs || !work || !seen) {
    free(defs);
    free(work);
    free(seen);
    return true;
  }
  for (size_t i = 0; i < f->len; ++i)
    if (f->data[i].dst >= 0 && (size_t)f->data[i].dst < value_count)
      defs[f->data[i].dst] = &f->data[i];

  bool returns_fp = false;
#define NY_SMT_PUSH_VALUE(value_)                                              \
  do {                                                                         \
    if (top < value_count)                                                     \
      work[top++] = (value_);                                                  \
    else                                                                       \
      returns_fp = true;                                                       \
  } while (0)
  for (size_t i = 0; i < f->len && !returns_fp; ++i) {
    const nyr_inst_t *ret = &f->data[i];
    if (ret->op != NYIR_RET || ret->a < 0)
      continue;
    if ((size_t)ret->a >= value_count) {
      returns_fp = true;
      break;
    }
    size_t top = 0;
    work[top++] = ret->a;
    while (top && !returns_fp) {
      int value = work[--top];
      if (value < 0 || (size_t)value >= value_count) {
        returns_fp = true;
        break;
      }
      if (seen[value])
        continue;
      seen[value] = true;
      const nyr_inst_t *def = defs[value];
      if (!def) {
        returns_fp = true;
        break;
      }
      bool fp_to_i64 = def->op == NYIR_F64_TO_I64 || def->op == NYIR_F32_TO_I64;
      if (def->op != NYIR_CMP_F32 && def->op != NYIR_CMP_F64 && !fp_to_i64 &&
          (def->scalar_class == NYIR_SCALAR_F32 ||
           def->scalar_class == NYIR_SCALAR_F64 ||
           nyr_smt_fp_opcode(def->op))) {
        returns_fp = true;
        break;
      }
      if (def->op == NYIR_COPY) {
        NY_SMT_PUSH_VALUE(def->a);
      } else if (def->op == NYIR_SELECT_I64) {
        NY_SMT_PUSH_VALUE(def->b);
        NY_SMT_PUSH_VALUE(def->c);
      } else if (def->op == NYIR_PHI) {
        for (size_t k = 0; k < def->phi_incoming_len; ++k)
          NY_SMT_PUSH_VALUE(def->phi_incoming[k].value);
      } else if (def->op == NYIR_LOAD_LOCAL) {
        bool has_store = false;
        for (size_t k = 0; k < f->len; ++k)
          if (f->data[k].op == NYIR_STORE_LOCAL && f->data[k].imm == def->imm) {
            has_store = true;
            NY_SMT_PUSH_VALUE(f->data[k].a);
          }
        if (!has_store && has_fp)
          returns_fp = true;
      }
    }
  }
#undef NY_SMT_PUSH_VALUE
  free(defs);
  free(work);
  free(seen);
  return returns_fp;
}

static bool nyr_smt_function_supported(const nyr_func_t *f) {
  if (!f || !f->len)
    return false;
  if (nyr_has_cfg_or_phi(f) && !nyr_smt_alloca_cfg_supported(f))
    return false;
  nyr_inst_t *instructions = malloc(f->len * sizeof(*instructions));
  if (!instructions)
    return false;
  memcpy(instructions, f->data, f->len * sizeof(*instructions));
  for (size_t i = 0; i < f->len; ++i) {
    if (instructions[i].op == NYIR_CALL) {
      if (!nyr_smt_call_supported(f, &instructions[i])) {
        free(instructions);
        return false;
      }
      instructions[i].op = NYIR_NOP;
      continue;
    }
    if (instructions[i].op == NYIR_CAPTURE_RET) {
      const nyr_inst_t *call = nyr_smt_capture_call(f, i);
      if (instructions[i].imm < 0 || instructions[i].imm > 13 || !call ||
          !nyr_smt_call_supported(f, call)) {
        free(instructions);
        return false;
      }
      instructions[i].op = NYIR_NOP;
      continue;
    }
    /*
     * Memory operations and vector float operations are modeled in the SMT
     * evaluator but are not implemented by the bounded interpreter.
     */
    if (instructions[i].op == NYIR_ADDR_LOCAL ||
        instructions[i].op == NYIR_ADDR_SYMBOL ||
        instructions[i].op == NYIR_LOAD_I64 ||
        instructions[i].op == NYIR_VEC4_LOAD_I64 ||
        instructions[i].op == NYIR_VEC8_LOAD_I64 ||
        instructions[i].op == NYIR_VEC4_LOAD_F64 ||
        instructions[i].op == NYIR_VEC8_LOAD_F32 ||
        instructions[i].op == NYIR_STORE_I64 ||
        instructions[i].op == NYIR_VEC4_STORE_F64 ||
        instructions[i].op == NYIR_VEC8_STORE_F32 ||
        instructions[i].op == NYIR_VEC4_STORE_I64 ||
        instructions[i].op == NYIR_VEC8_STORE_I64 ||
        instructions[i].op == NYIR_COPY_STRUCT ||
        instructions[i].op == NYIR_VEC4_FMA_F64 ||
        instructions[i].op == NYIR_VEC8_ADD_F32 ||
        instructions[i].op == NYIR_VEC8_SUB_F32 ||
        instructions[i].op == NYIR_VEC8_MUL_F32 ||
        instructions[i].op == NYIR_VEC8_DIV_F32 ||
        instructions[i].op == NYIR_VEC8_FMA_F32 ||
        instructions[i].op == NYIR_VEC8_SET1_F32 ||
        instructions[i].op == NYIR_VEC8_SHUFFLE_F32 ||
        instructions[i].op == NYIR_ALLOCA)
      instructions[i].op = NYIR_NOP;
  }
  nyr_func_t supported = *f;
  supported.data = instructions;
  bool ok = nyr_is_pure_i64_straightline(&supported);
  free(instructions);
  return ok;
}

static bool nyr_smt_results_equivalent(Z3_context z3, Z3_solver solver,
                                        Z3_ast valid_before,
                                        Z3_ast value_before, Z3_ast valid_after,
                                        Z3_ast value_after, bool cfg, char *err,
                                        size_t err_len) {
  bool valid_same = Z3_is_eq_ast(z3, valid_before, valid_after);
  bool value_same = Z3_is_eq_ast(z3, value_before, value_after);
  if (valid_same && value_same)
    return true;

  if (!valid_same) {
    if (ny_trace_enabled("NY_TRACE_TV_SMT"))
      ny_trace_line("TV_SMT", "checking definedness equivalence");
    Z3_solver_push(z3, solver);
    Z3_solver_assert(z3, solver, Z3_mk_xor(z3, valid_before, valid_after));
    Z3_lbool status = Z3_solver_check(z3, solver);
    Z3_solver_pop(z3, solver, 1);
    if (ny_trace_enabled("NY_TRACE_TV_SMT"))
      ny_trace_line("TV_SMT", "definedness query status=%d", (int)status);
    if (status != Z3_L_FALSE) {
      if (status == Z3_L_TRUE)
        nyr_err(err, err_len, "%s: definedness counterexample",
                 cfg ? "SMT CFG TV" : "SMT TV");
      else
        nyr_err(err, err_len, "%s: solver unknown",
                 cfg ? "SMT CFG TV" : "SMT TV");
      return false;
    }
  }

  if (value_same)
    return true;

  Z3_ast both_valid = ny_z3_and2(z3, valid_before, valid_after);
  Z3_ast value_mismatch =
      Z3_mk_not(z3, Z3_mk_eq(z3, value_before, value_after));
  if (ny_trace_enabled("NY_TRACE_TV_SMT"))
    ny_trace_line("TV_SMT", "checking value equivalence on defined inputs");
  Z3_solver_push(z3, solver);
  Z3_solver_assert(z3, solver, ny_z3_and2(z3, both_valid, value_mismatch));
  Z3_lbool status = Z3_solver_check(z3, solver);
  Z3_solver_pop(z3, solver, 1);
  if (ny_trace_enabled("NY_TRACE_TV_SMT"))
    ny_trace_line("TV_SMT", "value query status=%d", (int)status);
  if (status == Z3_L_FALSE)
    return true;
  if (status == Z3_L_TRUE)
    nyr_err(err, err_len, "%s: bounded counterexample",
             cfg ? "SMT CFG TV" : "SMT TV");
  else
    nyr_err(err, err_len, "%s: solver unknown", cfg ? "SMT CFG TV" : "SMT TV");
  return false;
}
#endif

bool nyr_tv_smt_equiv(const nyr_func_t *before, const nyr_func_t *after,
                       char *err, size_t err_len) {
  if (err && err_len)
    err[0] = '\0';
  if (!before || !after)
    return nyr_err(err, err_len, "SMT TV: missing function");
#ifndef NYTRIX_HAS_Z3
  if (!nyr_is_pure_i64_straightline(before) ||
      !nyr_is_pure_i64_straightline(after))
#else
  if (!nyr_smt_function_supported(before) ||
      !nyr_smt_function_supported(after))
#endif
    return false;
#ifdef NYTRIX_HAS_Z3
  if (!nyr_smt_alloca_shapes_match(before, after))
    return false;
#endif

  if (nyr_has_cfg_or_phi(before) || nyr_has_cfg_or_phi(after)) {
#ifndef NYTRIX_HAS_Z3
    return nyr_tv_equiv_straightline(before, after, 2048, err, err_len);
#else
    /*
     * `to_ieee_bv` leaves a NaN's payload unspecified. Until return-type and
     * NaN-payload equivalence are represented explicitly, do not use that
     * encoding to claim bitwise equivalence for a returned float.
     */
    if (nyr_smt_returns_fp(before) || nyr_smt_returns_fp(after))
      return nyr_tv_equiv_straightline(before, after, 2048, err, err_len);
    size_t n_before = nyr_max_local(before);
    size_t n_after = nyr_max_local(after);
    size_t nloc = n_before > n_after ? n_before : n_after;
    Z3_config cfg = Z3_mk_config();
    Z3_context z3 = Z3_mk_context(cfg);
    Z3_del_config(cfg);
    Z3_solver solver = Z3_mk_solver(z3);
    Z3_solver_inc_ref(z3, solver);
    Z3_sort bv = Z3_mk_bv_sort(z3, 64);
    nyr_smt_constrain_allocas(z3, bv, solver, before);
    nyr_smt_constrain_locals(z3, bv, solver, before, nloc);
    ny_smt_memory_t memory = nyr_smt_memory_model(z3, bv);
    if (!nyr_smt_call_table_build(z3, bv, &memory, before, after)) {
      nyr_smt_memory_free(&memory);
      Z3_solver_dec_ref(z3, solver);
      Z3_del_context(z3);
      return nyr_err(
          err, err_len,
          "SMT CFG TV: call model allocation or signature conflict");
    }
    Z3_ast *locals = nloc ? calloc(nloc, sizeof(*locals)) : NULL;
    if (nloc && !locals) {
      nyr_smt_memory_free(&memory);
      Z3_solver_dec_ref(z3, solver);
      Z3_del_context(z3);
      return nyr_err(err, err_len, "SMT CFG TV: OOM");
    }
    for (size_t i = 0; i < nloc; ++i) {
      char name[32];
      snprintf(name, sizeof(name), "L%zu", i);
      locals[i] = Z3_mk_const(z3, Z3_mk_string_symbol(z3, name), bv);
    }
    ny_smt_result_t rb = {0}, ra = {0};
    bool eb = nyr_smt_eval_cfg(z3, bv, before, &memory, locals, nloc, &rb);
    bool ea = nyr_smt_eval_cfg(z3, bv, after, &memory, locals, nloc, &ra);
    if (!eb || !ea) {
      free(locals);
      nyr_smt_memory_free(&memory);
      Z3_solver_dec_ref(z3, solver);
      Z3_del_context(z3);
      return nyr_tv_equiv_straightline(before, after, 2048, err, err_len);
    }
    if (!rb.complete || !ra.complete) {
      free(locals);
      nyr_smt_memory_free(&memory);
      Z3_solver_dec_ref(z3, solver);
      Z3_del_context(z3);
      return nyr_err(err, err_len,
                      "SMT CFG TV: bounded exploration incomplete");
    }
    bool ok = nyr_smt_results_equivalent(
        z3, solver, rb.valid, rb.value, ra.valid, ra.value, true, err, err_len);
    free(locals);
    nyr_smt_memory_free(&memory);
    Z3_solver_dec_ref(z3, solver);
    Z3_del_context(z3);
    return ok;
#endif
  }

#ifndef NYTRIX_HAS_Z3
  return nyr_tv_equiv_straightline(before, after, 64, err, err_len);
#else
  /*
   * Same NaN-payload discipline as the CFG path: `to_ieee_bv` leaves a NaN's
   * payload unspecified, so a returned float must not be claimed bitwise
   * equivalent here.  Straightline functions returning i64 with float
   * intermediates still take the solver below.
   */
  if (nyr_smt_returns_fp(before) || nyr_smt_returns_fp(after))
    return nyr_tv_equiv_straightline(before, after, 64, err, err_len);
  size_t n_before = nyr_max_local(before);
  size_t n_after = nyr_max_local(after);
  size_t nloc = n_before > n_after ? n_before : n_after;

  Z3_config cfg = Z3_mk_config();
  Z3_context z3 = Z3_mk_context(cfg);
  Z3_del_config(cfg);
  Z3_solver solver = Z3_mk_solver(z3);
  Z3_solver_inc_ref(z3, solver);
  Z3_sort bv = Z3_mk_bv_sort(z3, 64);
  nyr_smt_constrain_allocas(z3, bv, solver, before);
  nyr_smt_constrain_locals(z3, bv, solver, before, nloc);
  ny_smt_memory_t memory = nyr_smt_memory_model(z3, bv);
  if (!nyr_smt_call_table_build(z3, bv, &memory, before, after)) {
    nyr_smt_memory_free(&memory);
    Z3_solver_dec_ref(z3, solver);
    Z3_del_context(z3);
    return nyr_err(err, err_len,
                    "SMT TV: call model allocation or signature conflict");
  }

  Z3_ast *locals = NULL;
  if (nloc) {
    locals = (Z3_ast *)calloc(nloc, sizeof(Z3_ast));
    if (!locals) {
      nyr_smt_memory_free(&memory);
      Z3_solver_dec_ref(z3, solver);
      Z3_del_context(z3);
      return nyr_err(err, err_len, "SMT TV: OOM");
    }
    for (size_t i = 0; i < nloc; ++i) {
      char name[32];
      snprintf(name, sizeof(name), "L%zu", i);
      locals[i] = Z3_mk_const(z3, Z3_mk_string_symbol(z3, name), bv);
    }
  }

  Z3_ast ret_b = Z3_mk_int64(z3, 0, bv);
  Z3_ast ret_a = Z3_mk_int64(z3, 0, bv);
  Z3_ast valid_b = Z3_mk_true(z3);
  Z3_ast valid_a = Z3_mk_true(z3);
  bool ok_b = nyr_smt_eval_straightline(z3, bv, before, &memory, locals, nloc,
                                         &ret_b, &valid_b);
  bool ok_a = nyr_smt_eval_straightline(z3, bv, after, &memory, locals, nloc,
                                         &ret_a, &valid_a);
  if (!ok_b || !ok_a) {
    free(locals);
    nyr_smt_memory_free(&memory);
    Z3_solver_dec_ref(z3, solver);
    Z3_del_context(z3);
    return nyr_tv_equiv_straightline(before, after, 64, err, err_len);
  }
  bool ok = nyr_smt_results_equivalent(z3, solver, valid_b, ret_b, valid_a,
                                        ret_a, false, err, err_len);

  free(locals);
  nyr_smt_memory_free(&memory);
  Z3_solver_dec_ref(z3, solver);
  Z3_del_context(z3);
  return ok;
#endif
}
