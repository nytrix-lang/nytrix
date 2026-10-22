/*
 * Compact scalar loop transforms; large pipeline and vector passes live in
 * loop_pipeline.c and loop_vectorize.c. Shared analysis is declared by loop.h.
 */

#include "code/backend/ir/opt/loop.h"
#include "base/common.h"
#include "base/compat.h"
#include "base/debug.h"
#include "code/backend/ir/internal.h"
#include "code/backend/ir/opt/util.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Narrow counted-loop idioms that are exact under NyIR wrapping i64 semantics.
 */

static int idiom_root_copy(const nyr_func_t *f, const int *defs, int value) {
  while (value >= 0 && value < f->next_value && defs[value] >= 0 &&
         f->data[defs[value]].op == NYIR_COPY)
    value = f->data[defs[value]].a;
  return value;
}

static bool const_i64_val(const nyr_func_t *f, const int *defs, int value,
                          int64_t *out) {
  value = idiom_root_copy(f, defs, value);
  if (value < 0 || value >= f->next_value || defs[value] < 0)
    return false;
  const nyr_inst_t *def = &f->data[defs[value]];
  if (def->op != NYIR_CONST_I64)
    return false;
  if (out)
    *out = def->imm;
  return true;
}

typedef struct {
  size_t pos;
  int src;
  int shift;
} nir_loop_idiom_mul_t;

/*
 * Canonicalize `iv * C` (C a power of 2 >= 2) to `iv << log2(C)` inside
 * a counted positive-step natural loop.  Exact under Nytrix's wrapping i64
 * multiply for any shift in [1, 63): (iv * 2^s) mod 2^64 == (iv << s).
 *
 *   iv * 2  -> iv << 1
 *   iv * 4  -> iv << 2
 *   ... up to iv * (1<<62) -> iv << 62
 *
 * For non-power-of-2 multipliers, leave them for general strength-reduce.
 *
 * Candidates are collected first and applied in descending instruction
 * position, so inserting a CONST before a MUL never invalidates the
 * position of a MUL that has not been rewritten yet (the old approach of
 * bumping cfg.block_end per insertion left the block ranges of every later
 * block stale and silently skipped MULs near block ends).
 */
bool nyr_loop_idiom(nyr_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;
  nyr_cfg_t cfg = {0};
  nyr_scev_info_t info = {0};
  int *defs = nyr_build_defs(f);
  if (!defs || !nyr_cfg_build(f, &cfg) || !nyr_scev_analyze(f, &info)) {
    free(defs);
    nyr_cfg_free(&cfg);
    nyr_scev_free(&info);
    return false;
  }
  bool *in_loop = calloc(cfg.block_count, sizeof(*in_loop));
  if (!in_loop) {
    free(defs);
    nyr_cfg_free(&cfg);
    nyr_scev_free(&info);
    return false;
  }
  nir_loop_idiom_mul_t *cands = NULL;
  size_t cand_count = 0, cand_cap = 0;
  for (size_t li = 0; li < info.count; ++li) {
    const nyr_scev_loop_t *loop = &info.loops[li];
    if (loop->step <= 0 || !loop->limit_is_const ||
        !nyr_cfg_natural_loop_blocks(&cfg, loop->latch_block,
                                     loop->header_block, in_loop,
                                     cfg.block_count))
      continue;
    for (size_t block = 0; block < cfg.block_count; ++block) {
      if (!in_loop[block])
        continue;
      for (size_t i = cfg.block_start[block]; i < cfg.block_end[block]; ++i) {
        const nyr_inst_t *in = &f->data[i];
        if (in->op != NYIR_MUL_I64)
          continue;
        /*
         * Identify the iv operand and the constant multiplier.
         */
        int iv_operand = -1;
        int64_t multiplier = 0;
        if (idiom_root_copy(f, defs, in->a) == loop->iv &&
            const_i64_val(f, defs, in->b, &multiplier)) {
          iv_operand = in->a;
        } else if (idiom_root_copy(f, defs, in->b) == loop->iv &&
                   const_i64_val(f, defs, in->a, &multiplier)) {
          iv_operand = in->b;
        }
        if (iv_operand < 0 || multiplier <= 1)
          continue;
        /*
         * Only handle exact powers of two.
         */
        if (multiplier <= 0 || (multiplier & (multiplier - 1)) != 0)
          continue;
        int shift = 0;
        int64_t v = multiplier;
        while (v > 1) {
          v >>= 1;
          shift++;
        }
        if (shift < 1 || shift > 62)
          continue;
        if (cand_count == cand_cap) {
          size_t new_cap = cand_cap ? cand_cap * 2 : 16;
          nir_loop_idiom_mul_t *grown =
              realloc(cands, new_cap * sizeof(*grown));
          if (!grown) {
            free(cands);
            free(in_loop);
            free(defs);
            nyr_cfg_free(&cfg);
            nyr_scev_free(&info);
            return false;
          }
          cands = grown;
          cand_cap = new_cap;
        }
        cands[cand_count++] =
            (nir_loop_idiom_mul_t){.pos = i, .src = iv_operand, .shift = shift};
      }
    }
  }
  for (size_t ci = cand_count; ci > 0; --ci) {
    const nir_loop_idiom_mul_t *c = &cands[ci - 1];
    size_t i = c->pos;
    /*
     * Another candidate may already have rewritten this position (nested
     * loops can match the same MUL twice); only rewrite a live MUL.
     */
    if (i >= f->len || f->data[i].op != NYIR_MUL_I64)
      continue;
    int shift_dst = f->next_value++;
    if (!nir_ensure_inst_space(f, 1)) {
      free(cands);
      free(in_loop);
      free(defs);
      nyr_cfg_free(&cfg);
      nyr_scev_free(&info);
      return false;
    }
    memmove(&f->data[i + 1], &f->data[i], (f->len - i) * sizeof(*f->data));
    f->len++;
    f->data[i] = (nyr_inst_t){.op = NYIR_CONST_I64,
                              .dst = shift_dst,
                              .a = -1,
                              .b = -1,
                              .c = -1,
                              .d = -1,
                              .e = -1,
                              .f = -1,
                              .imm = (int64_t)c->shift};
    nyr_inst_t *mul = &f->data[i + 1];
    mul->op = NYIR_SHL_I64;
    mul->a = c->src;
    mul->b = shift_dst;
    mul->flags = 0;
    mul->effects = nyr_inst_effects(mul);
    /*
     * MUL is not an alias-producing operation in the alias contract.
     */
    mul->alias_class = 0;
  }
  free(cands);
  free(in_loop);
  free(defs);
  nyr_cfg_free(&cfg);
  nyr_scev_free(&info);
  /*
   * The DNA-complement rewrite (3 - x → x ^ 3) was removed: the
   * identity holds only for 2-bit bases (x in [0,3]) and only for the
   * const-on-the-left orientation, none of which is provable from
   * local pattern shape. Applied to an arbitrary loaded value it
   * changed results outright.
   */
  return true;
}

/*
 * Conservative loop interchange for rectangular nests with no loop-carried
 * state and only effects proven independent of the reordered iteration space.
 */

static int interchange_root_copy(const nyr_func_t *f, const int *defs,
                                 int value) {
  for (int depth = 0; value >= 0 && value < f->next_value && defs[value] >= 0 &&
                      f->data[defs[value]].op == NYIR_COPY && depth < 32;
       ++depth)
    value = f->data[defs[value]].a;
  return value;
}

static bool is_iv_control_use(const nyr_inst_t *in, int iv, int next) {
  if (in->op == NYIR_PHI || in->op == NYIR_COPY)
    return true;
  if (in->op == NYIR_CMP_I64 && in->a == iv)
    return true;
  if ((in->op == NYIR_ADD_I64 || in->op == NYIR_SUB_I64) && in->dst == next)
    return true;
  return false;
}

static bool interchange_iv_use_allowed(const nyr_cfg_t *cfg,
                                       const bool *inner_blocks,
                                       const nyr_inst_t *in, size_t index,
                                       const bool *safe_stores, int iv,
                                       int next) {
  if (is_iv_control_use(in, iv, next) || (safe_stores && safe_stores[index]))
    return true;
  size_t block = cfg->inst_block[index];
  if (block >= cfg->block_count || !inner_blocks[block])
    return false;
  switch (in->op) {
  case NYIR_ADD_I64:
  case NYIR_SUB_I64:
  case NYIR_MUL_I64:
  case NYIR_SHL_I64:
    return true;
  default:
    return false;
  }
}

static bool iv_has_only_control_uses(const nyr_func_t *f, const int *defs,
                                     const nyr_cfg_t *cfg,
                                     const bool *inner_blocks,
                                     const bool *safe_stores,
                                     const nyr_scev_loop_t *outer,
                                     const nyr_scev_loop_t *inner) {
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op == NYIR_PHI) {
      /*
       * The induction PHIs define the two loop variables. Other PHIs can
       * carry either variable out of the loop; that is an observable use,
       * even though the PHI itself is a control-flow merge.
       */
      if (in->dst == outer->iv || in->dst == inner->iv)
        continue;
      for (size_t k = 0; k < in->phi_incoming_len; ++k) {
        int incoming =
            interchange_root_copy(f, defs, in->phi_incoming[k].value);
        if (incoming == outer->iv || incoming == inner->iv)
          return false;
      }
      continue;
    }
    nyr_inst_t canonical = *in;
    canonical.a = interchange_root_copy(f, defs, canonical.a);
    canonical.b = interchange_root_copy(f, defs, canonical.b);
    canonical.c = interchange_root_copy(f, defs, canonical.c);
    canonical.d = interchange_root_copy(f, defs, canonical.d);
    canonical.e = interchange_root_copy(f, defs, canonical.e);
    canonical.f = interchange_root_copy(f, defs, canonical.f);
    bool uses_outer = canonical.a == outer->iv || canonical.b == outer->iv ||
                      canonical.c == outer->iv || canonical.d == outer->iv ||
                      canonical.e == outer->iv || canonical.f == outer->iv;
    bool uses_inner = canonical.a == inner->iv || canonical.b == inner->iv ||
                      canonical.c == inner->iv || canonical.d == inner->iv ||
                      canonical.e == inner->iv || canonical.f == inner->iv;
    for (size_t k = 0; k < canonical.extra_args_len; ++k) {
      int arg = interchange_root_copy(f, defs, canonical.extra_args[k]);
      uses_outer |= arg == outer->iv;
      uses_inner |= arg == inner->iv;
    }
    if (uses_outer &&
        !interchange_iv_use_allowed(cfg, inner_blocks, &canonical, i,
                                    safe_stores, outer->iv, outer->next_value))
      return false;
    if (uses_inner &&
        !interchange_iv_use_allowed(cfg, inner_blocks, &canonical, i,
                                    safe_stores, inner->iv, inner->next_value))
      return false;
  }
  return true;
}

static nyr_inst_t *loop_cmp(nyr_func_t *f, const nyr_cfg_t *cfg,
                            const int *defs, const nyr_scev_loop_t *l) {
  if (!f || !cfg || !l || l->header_block >= cfg->block_count)
    return NULL;
  for (size_t i = cfg->block_start[l->header_block];
       i < cfg->block_end[l->header_block]; ++i)
    if (f->data[i].op == NYIR_CMP_I64 &&
        interchange_root_copy(f, defs, f->data[i].a) == l->iv &&
        interchange_root_copy(f, defs, f->data[i].b) ==
            interchange_root_copy(f, defs, l->limit_value))
      return &f->data[i];
  return NULL;
}

static bool interchange_inst_uses_value(const nyr_inst_t *in, int value) {
  if (in->a == value || in->b == value || in->c == value || in->d == value ||
      in->e == value || in->f == value)
    return true;
  for (size_t i = 0; i < in->extra_args_len; ++i)
    if (in->extra_args[i] == value)
      return true;
  for (size_t i = 0; i < in->phi_incoming_len; ++i)
    if (in->phi_incoming[i].value == value)
      return true;
  return false;
}

static bool interchange_inst_uses_only_address(const nyr_inst_t *in,
                                               int address) {
  return in->a == address && in->b != address && in->c != address &&
         in->d != address && in->e != address && in->f != address &&
         in->extra_args_len == 0 && in->phi_incoming_len == 0;
}

typedef struct {
  int root;
  nyr_scev_affine_t offset;
} interchange_address_t;

static bool interchange_scalar_address_offset(
    const nyr_func_t *f, const nyr_scev_info_t *info, const int *defs,
    const nyr_cfg_t *cfg, const nyr_scev_loop_t *outer,
    const nyr_scev_loop_t *inner, int value, size_t at, unsigned depth,
    nyr_scev_affine_t *out);

static bool interchange_address_from_root(const nyr_func_t *f,
                                          const nyr_scev_info_t *info,
                                          const int *defs, const nyr_cfg_t *cfg,
                                          const nyr_scev_loop_t *outer,
                                          const nyr_scev_loop_t *inner,
                                          int value, size_t at, unsigned depth,
                                          interchange_address_t *out) {
  if (!f || !info || !defs || !cfg || !outer || !inner || !out || depth > 16)
    return false;
  value = interchange_root_copy(f, defs, value);
  if (value < 0 || value >= f->next_value || defs[value] < 0)
    return false;
  const nyr_inst_t *def = &f->data[(size_t)defs[value]];
  /*
   * A pointer-producing value is a provenance root.  ALLOCA roots carry a
   * statically known extent; external roots do not, so their individual
   * accesses must retain the frontend's in-bounds proof marker.
   */
  if (def->scalar_class == NYIR_SCALAR_POINTER && def->op != NYIR_ADD_I64) {
    out->root = value;
    out->offset = (nyr_scev_affine_t){
        .iv = {-1, -1},
        .range = {.has_min = true, .has_max = true, .min = 0, .max = 0}};
    return true;
  }
  if (def->op != NYIR_ADD_I64)
    return false;

  int operands[2] = {def->a, def->b};
  for (size_t base_index = 0; base_index < 2; ++base_index) {
    interchange_address_t base = {0};
    if (!interchange_address_from_root(f, info, defs, cfg, outer, inner,
                                       operands[base_index], at, depth + 1,
                                       &base))
      continue;
    nyr_scev_affine_t offset = {0};
    if (!interchange_scalar_address_offset(f, info, defs, cfg, outer, inner,
                                           operands[1 - base_index], at, 0,
                                           &offset))
      continue;
    __int128 constant = (__int128)base.offset.constant + offset.constant;
    __int128 minimum = (__int128)base.offset.range.min + offset.range.min;
    __int128 maximum = (__int128)base.offset.range.max + offset.range.max;
    if (constant < INT64_MIN || constant > INT64_MAX || minimum < INT64_MIN ||
        minimum > INT64_MAX || maximum < INT64_MIN || maximum > INT64_MAX)
      return false;
    out->root = base.root;
    out->offset = offset;
    out->offset.constant = (int64_t)constant;
    out->offset.range.min = (int64_t)minimum;
    out->offset.range.max = (int64_t)maximum;
    return true;
  }
  return false;
}

static bool interchange_scalar_address_offset(
    const nyr_func_t *f, const nyr_scev_info_t *info, const int *defs,
    const nyr_cfg_t *cfg, const nyr_scev_loop_t *outer,
    const nyr_scev_loop_t *inner, int value, size_t at, unsigned depth,
    nyr_scev_affine_t *out) {
  if (!f || !info || !defs || !cfg || !outer || !inner || !out || depth > 32 ||
      !nyr_scev_affine_at_cfg(f, info, defs, cfg, value, at, out) ||
      !out->range.has_min || !out->range.has_max)
    return false;
  for (size_t i = 0; i < 2; ++i)
    if (out->iv[i] >= 0 && out->iv[i] != outer->iv && out->iv[i] != inner->iv)
      return false;

  int resolved = interchange_root_copy(f, defs, value);
  if (resolved < 0 || resolved >= f->next_value || defs[resolved] < 0)
    return false;
  const nyr_inst_t *def = &f->data[(size_t)defs[resolved]];
  if (def->op == NYIR_CONST_I64 || resolved == outer->iv ||
      resolved == inner->iv)
    return true;
  if (def->op != NYIR_ADD_I64 && def->op != NYIR_SUB_I64 &&
      def->op != NYIR_MUL_I64 && def->op != NYIR_SHL_I64)
    return false;
  nyr_scev_affine_t checked = {0};
  if (!nyr_scev_affine_at_cfg(f, info, defs, cfg, resolved, at, &checked) ||
      !checked.range.has_min || !checked.range.has_max)
    return false;
  if (!interchange_scalar_address_offset(f, info, defs, cfg, outer, inner,
                                         def->a, at, depth + 1, &checked))
    return false;
  if (def->op != NYIR_SHL_I64 &&
      !interchange_scalar_address_offset(f, info, defs, cfg, outer, inner,
                                         def->b, at, depth + 1, &checked))
    return false;
  return true;
}

static int64_t interchange_affine_coefficient(const nyr_scev_affine_t *a,
                                              int iv) {
  if (!a || iv < 0)
    return 0;
  for (size_t i = 0; i < 2; ++i)
    if (a->iv[i] == iv)
      return a->coefficient[i];
  return 0;
}

static bool interchange_store_is_injective(const nyr_scev_affine_t *address,
                                           const nyr_scev_loop_t *outer,
                                           const nyr_scev_loop_t *inner) {
  if (!address || !outer || !inner || !outer->trip_count_known ||
      !inner->trip_count_known || outer->trip_count == 0 ||
      inner->trip_count == 0)
    return false;
  __int128 outer_delta =
      (__int128)interchange_affine_coefficient(address, outer->iv) *
      outer->step;
  __int128 inner_delta =
      (__int128)interchange_affine_coefficient(address, inner->iv) *
      inner->step;
  if (outer_delta < 0)
    outer_delta = -outer_delta;
  if (inner_delta < 0)
    inner_delta = -inner_delta;
  if ((outer->trip_count > 1 && outer_delta == 0) ||
      (inner->trip_count > 1 && inner_delta == 0))
    return false;
  if (outer->trip_count <= 1 || inner->trip_count <= 1)
    return true;
  __int128 outer_span = outer_delta * (outer->trip_count - 1);
  __int128 inner_span = inner_delta * (inner->trip_count - 1);
  return outer_delta > inner_span || inner_delta > outer_span;
}

static bool interchange_same_affine_store_map(const nyr_scev_affine_t *a,
                                              const nyr_scev_affine_t *b) {
  if (!a || !b || a->constant != b->constant)
    return false;
  for (size_t i = 0; i < 2; ++i)
    if (a->iv[i] != b->iv[i] || a->coefficient[i] != b->coefficient[i])
      return false;
  return true;
}

static bool interchange_store_ranges_disjoint(const nyr_scev_affine_t *a,
                                              const nyr_scev_affine_t *b) {
  if (!a || !b || !a->range.has_min || !a->range.has_max || !b->range.has_min ||
      !b->range.has_max)
    return false;
  return (__int128)a->range.max + 8 <= b->range.min ||
         (__int128)b->range.max + 8 <= a->range.min;
}

static bool interchange_allocation_uses_confined(
    const nyr_func_t *f, const nyr_scev_info_t *info, const int *defs,
    const nyr_cfg_t *cfg, const nyr_scev_loop_t *outer,
    const nyr_scev_loop_t *inner, int root) {
  if (!f || !info || !defs || !cfg || root < 0 || root >= f->next_value)
    return false;
  size_t value_count = (size_t)f->next_value;
  uint8_t *address_values = calloc(value_count ? value_count : 1, 1);
  if (!address_values)
    return false;
  address_values[root] = 1;
  for (size_t v = 0; v < value_count; ++v) {
    if (defs[v] < 0 || (size_t)defs[v] >= f->len)
      continue;
    interchange_address_t address = {0};
    if (interchange_address_from_root(f, info, defs, cfg, outer, inner, (int)v,
                                      (size_t)defs[v], 0, &address) &&
        address.root == root)
      address_values[v] = 1;
  }

  bool confined = true;
  for (size_t i = 0; i < f->len && confined; ++i) {
    const nyr_inst_t *in = &f->data[i];
    int operands[6] = {in->a, in->b, in->c, in->d, in->e, in->f};
    for (size_t k = 0; k < 6; ++k) {
      int value = operands[k];
      if (value < 0 || (size_t)value >= value_count || !address_values[value])
        continue;
      bool derived =
          in->dst >= 0 && (size_t)in->dst < value_count &&
          address_values[in->dst] &&
          ((in->op == NYIR_ADD_I64 && (k == 0 || k == 1) &&
            (operands[1 - k] < 0 || (size_t)operands[1 - k] >= value_count ||
             !address_values[operands[1 - k]])) ||
           (in->op == NYIR_COPY && k == 0));
      bool memory_use =
          (in->op == NYIR_LOAD_I64 || in->op == NYIR_STORE_I64) && k == 0;
      if (!derived && !memory_use) {
        confined = false;
        break;
      }
    }
    for (size_t k = 0; k < in->extra_args_len && confined; ++k) {
      int value = in->extra_args[k];
      if (value >= 0 && (size_t)value < value_count && address_values[value])
        confined = false;
    }
    for (size_t k = 0; k < in->phi_incoming_len && confined; ++k) {
      int value = in->phi_incoming[k].value;
      if (value >= 0 && (size_t)value < value_count && address_values[value])
        confined = false;
    }
  }
  free(address_values);
  return confined;
}

static bool interchange_affine_store_candidate(
    const nyr_func_t *f, const nyr_scev_info_t *info, const int *defs,
    const nyr_cfg_t *cfg, const bool *inner_blocks,
    const nyr_scev_loop_t *outer, const nyr_scev_loop_t *inner,
    bool *safe_stores, size_t *store_count) {
  if (!f || !info || !defs || !cfg || !inner_blocks || !outer || !inner ||
      !safe_stores || !store_count)
    return false;
  *store_count = 0;
  for (size_t b = 0; b < cfg->block_count; ++b) {
    if (!inner_blocks[b])
      continue;
    for (size_t i = cfg->block_start[b]; i < cfg->block_end[b]; ++i) {
      const nyr_inst_t *in = &f->data[i];
      if (in->op != NYIR_STORE_I64)
        continue;
      if (in->imm != 0 || (in->flags & NYIR_INST_F_MEM_BYTE) ||
          in->scalar_class != NYIR_SCALAR_I64_RAW ||
          nyr_effective_effects(in) !=
              (NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP))
        return false;
      interchange_address_t address = {0};
      if (!interchange_address_from_root(f, info, defs, cfg, outer, inner,
                                         in->a, i, 0, &address) ||
          address.root < 0 || address.root >= f->next_value ||
          defs[address.root] < 0)
        return false;
      const nyr_inst_t *root = &f->data[(size_t)defs[address.root]];
      bool private_stack = root->op == NYIR_ALLOCA;
      bool address_proven =
          defs[in->a] >= 0 && (f->data[(size_t)defs[in->a]].flags &
                               NYIR_INST_F_IN_BOUNDS_ADDR) != 0;
      if (!address.offset.range.has_min || !address.offset.range.has_max ||
          (private_stack &&
           (root->imm < 8 || address.offset.range.min < 0 ||
            address.offset.range.max > root->imm - 8 ||
            !interchange_allocation_uses_confined(f, info, defs, cfg, outer,
                                                  inner, address.root))) ||
          (!private_stack && !address_proven) ||
          !interchange_store_is_injective(&address.offset, outer, inner))
        return false;
      /*
       * Per-store injectivity does not rule out a dependence between two
       * affine stores into the same allocation. Their iteration order can
       * reverse when the loops are exchanged. Different maps are safe only
       * when their complete byte ranges are disjoint; otherwise require the
       * same element map, whose instruction order is unchanged.
       */
      for (size_t previous = 0; previous < i; ++previous) {
        if (!safe_stores[previous])
          continue;
        interchange_address_t previous_address = {0};
        if (!interchange_address_from_root(f, info, defs, cfg, outer, inner,
                                           f->data[previous].a, previous, 0,
                                           &previous_address))
          return false;
        const nyr_inst_t *previous_root =
            &f->data[(size_t)defs[previous_address.root]];
        /*
         * Two external pointer values may alias even when their SSA ids do
         * not.  Without a no-alias proof, only permit stores rooted at the
         * exact same pointer value.  Private allocas remain distinct.
         */
        if (previous_address.root != address.root &&
            (root->op != NYIR_ALLOCA || previous_root->op != NYIR_ALLOCA))
          return false;
        if (previous_address.root == address.root &&
            !interchange_same_affine_store_map(&previous_address.offset,
                                               &address.offset) &&
            !interchange_store_ranges_disjoint(&previous_address.offset,
                                               &address.offset))
          return false;
      }
      safe_stores[i] = true;
      ++*store_count;
    }
  }
  return true;
}

static bool interchange_inner_defs_do_not_escape(const nyr_func_t *f,
                                                 const nyr_cfg_t *cfg,
                                                 const bool *inner_blocks) {
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *def = &f->data[i];
    if (def->dst < 0 || cfg->inst_block[i] >= cfg->block_count ||
        !inner_blocks[cfg->inst_block[i]])
      continue;
    for (size_t j = 0; j < f->len; ++j) {
      if (i == j || !interchange_inst_uses_value(&f->data[j], def->dst))
        continue;
      size_t user_block = cfg->inst_block[j];
      if (user_block >= cfg->block_count || !inner_blocks[user_block])
        return false;
    }
  }
  return true;
}

static bool interchange_iv_copies_do_not_escape(const nyr_func_t *f,
                                                const int *defs,
                                                const nyr_cfg_t *cfg,
                                                const bool *inner_blocks,
                                                const nyr_scev_loop_t *outer,
                                                const nyr_scev_loop_t *inner) {
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *copy = &f->data[i];
    if (copy->op != NYIR_COPY || copy->dst < 0)
      continue;
    int source = interchange_root_copy(f, defs, copy->a);
    if (source != outer->iv && source != inner->iv)
      continue;
    for (size_t j = 0; j < f->len; ++j) {
      if (i == j || !interchange_inst_uses_value(&f->data[j], copy->dst))
        continue;
      size_t user_block = cfg->inst_block[j];
      if (user_block >= cfg->block_count || !inner_blocks[user_block])
        return false;
    }
  }
  return true;
}

static bool interchange_inst_dominates(const nyr_cfg_t *cfg, size_t dominator,
                                       size_t use) {
  size_t dominator_block = cfg->inst_block[dominator];
  size_t use_block = cfg->inst_block[use];
  if (dominator_block == use_block)
    return dominator < use;
  return nyr_cfg_dominates(cfg, dominator_block, use_block);
}

/*
 * Permit a memory read only when it is a direct read from an 8-byte stack
 * allocation whose final initialization store dominates every loop read and
 * has no other pointer uses. Earlier direct stores are harmless because the
 * dominating store overwrites them on every path to a read. This excludes
 * escaped/derived aliases and proves both that the address is valid and that
 * the loaded value is invariant across the reordered loop nest.
 */
static bool interchange_stack_read_proven(const nyr_func_t *f,
                                          const nyr_cfg_t *cfg, const int *defs,
                                          const bool *outer_blocks,
                                          const bool *inner_blocks,
                                          size_t read_index) {
  const nyr_inst_t *read = &f->data[read_index];
  int address = read->a;
  if (address < 0 || address >= f->next_value)
    return false;
  int alloca_index = defs[address];
  if (alloca_index < 0 || (size_t)alloca_index >= f->len)
    return false;
  const nyr_inst_t *alloca = &f->data[alloca_index];
  if (alloca->op != NYIR_ALLOCA || alloca->imm < 8 ||
      alloca->scalar_class != NYIR_SCALAR_POINTER)
    return false;
  size_t alloca_block = cfg->inst_block[alloca_index];
  if (outer_blocks[alloca_block] || inner_blocks[alloca_block] ||
      !cfg->reachable[alloca_block])
    return false;

  size_t store_index = SIZE_MAX;
  size_t read_count = 0;
  bool found_read = false;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (!interchange_inst_uses_value(in, address))
      continue;
    if (in->op == NYIR_LOAD_I64 &&
        interchange_inst_uses_only_address(in, address) &&
        !(in->flags & NYIR_INST_F_MEM_BYTE) &&
        !(in->effects & NYIR_EFFECT_VOLATILE) &&
        !(nyr_effective_effects(in) & NYIR_EFFECT_VOLATILE) &&
        nyr_effective_effects(in) ==
            (NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP)) {
      size_t block = cfg->inst_block[i];
      if ((!outer_blocks[block] && !inner_blocks[block]) ||
          !cfg->reachable[block])
        return false;
      ++read_count;
      found_read |= i == read_index;
      continue;
    }
    if (in->op == NYIR_STORE_I64 &&
        interchange_inst_uses_only_address(in, address) &&
        !(in->flags & NYIR_INST_F_MEM_BYTE) && in->imm == 0 &&
        nyr_effective_effects(in) ==
            (NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP)) {
      size_t block = cfg->inst_block[i];
      if ((outer_blocks[block] || inner_blocks[block]) ||
          !cfg->reachable[block])
        return false;
      /*
       * Keep the last store in instruction order; the dominance checks below
       * establish that this is the value observed on every loop path.
       */
      store_index = i;
      continue;
    }
    /*
     * A copy, derived pointer, call argument, or other use is an escape.
     */
    return false;
  }
  if (!found_read || read_count == 0 || store_index == SIZE_MAX ||
      !interchange_inst_dominates(cfg, (size_t)alloca_index, store_index))
    return false;

  size_t store_block = cfg->inst_block[store_index];
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op != NYIR_LOAD_I64 || in->a != address)
      continue;
    size_t block = cfg->inst_block[i];
    if ((!outer_blocks[block] && !inner_blocks[block]) ||
        !cfg->reachable[block] ||
        (store_block == block ? store_index >= i
                              : !nyr_cfg_dominates(cfg, store_block, block)))
      return false;
  }
  return true;
}

static bool
interchange_stack_read_is_invariant(const nyr_func_t *f, const nyr_cfg_t *cfg,
                                    const int *defs, const bool *outer_blocks,
                                    const bool *inner_blocks,
                                    uint8_t *read_state, size_t read_index) {
  int address = f->data[read_index].a;
  if (address < 0 || address >= f->next_value)
    return false;
  if (read_state[address] != 0)
    return read_state[address] == 1;
  bool proven = interchange_stack_read_proven(f, cfg, defs, outer_blocks,
                                              inner_blocks, read_index);
  read_state[address] = proven ? 1 : 2;
  return proven;
}

/*
 * Interchange reorders the iteration schedule. Pure bodies and proven
 * invariant stack reads are safe; private, in-bounds, injective affine stores
 * are safe when cross-store mappings cannot change the final write order.
 * Other memory accesses, local writes, calls, traps, volatile operations, and
 * unknown effects reject the candidate. Also reject any loop-carried PHI
 * beyond the two induction variables (coupled recurrences reorder under swap).
 */
static bool bodies_stateless(const nyr_func_t *f, const nyr_cfg_t *cfg,
                             const int *defs, const bool *outer_blocks,
                             const bool *inner_blocks, uint8_t *read_state,
                             int outer_iv, int inner_iv,
                             const bool *safe_stores) {
  const nyr_effect_t reorder_barriers =
      NYIR_EFFECT_CALL | NYIR_EFFECT_WRITE_LOCAL | NYIR_EFFECT_WRITE_MEMORY |
      NYIR_EFFECT_MAY_TRAP | NYIR_EFFECT_VOLATILE | NYIR_EFFECT_IO |
      NYIR_EFFECT_THREAD | NYIR_EFFECT_FFI | NYIR_EFFECT_ALLOCATION |
      NYIR_EFFECT_UNKNOWN_SIDE_EFFECT;
  for (size_t b = 0; b < cfg->block_count; ++b) {
    if (!outer_blocks[b] && !inner_blocks[b])
      continue;
    for (size_t i = cfg->block_start[b]; i < cfg->block_end[b]; ++i) {
      const nyr_inst_t *in = &f->data[i];
      nyr_effect_t effects = nyr_effective_effects(in);
      bool invariant_stack_read =
          in->op == NYIR_LOAD_I64 && (effects & NYIR_EFFECT_READ_MEMORY) &&
          interchange_stack_read_is_invariant(f, cfg, defs, outer_blocks,
                                              inner_blocks, read_state, i);
      nyr_effect_t forbidden = effects & reorder_barriers;
      if (safe_stores && safe_stores[i])
        forbidden &= ~(NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP);
      if (invariant_stack_read)
        forbidden &= ~NYIR_EFFECT_MAY_TRAP;
      if (((effects & NYIR_EFFECT_READ_MEMORY) && !invariant_stack_read) ||
          forbidden ||
          ((effects & NYIR_EFFECT_READ_LOCAL) &&
           (in->op != NYIR_LOAD_LOCAL || effects != NYIR_EFFECT_READ_LOCAL))) {
        if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
          ny_dbg_heuristic_event(
              "LOOP_INTERCHANGE",
              "reject body instruction=%zu op=%s effects=0x%x", i,
              nyr_op_name(in->op), effects);
        return false;
      }
      switch (in->op) {
      case NYIR_CALL:
      case NYIR_STORE_LOCAL:
      case NYIR_COPY_STRUCT:
      case NYIR_VEC4_STORE_F64:
      case NYIR_VEC8_STORE_F32:
      case NYIR_VEC4_STORE_I64:
        return false;
      case NYIR_STORE_I64:
        if (!safe_stores || !safe_stores[i])
          return false;
        break;
      case NYIR_PHI:
        if (in->dst != outer_iv && in->dst != inner_iv)
          return false;
        break;
      default:
        break;
      }
    }
  }
  return true;
}

static int interchange_swap_iv(const nyr_func_t *f, const int *defs, int value,
                               int outer_iv, int inner_iv) {
  int root = interchange_root_copy(f, defs, value);
  if (root == outer_iv)
    return inner_iv;
  if (root == inner_iv)
    return outer_iv;
  return value;
}

static void interchange_swap_body_ivs(nyr_func_t *f, const int *defs,
                                      const nyr_cfg_t *cfg,
                                      const bool *inner_blocks,
                                      const nyr_scev_loop_t *outer,
                                      const nyr_scev_loop_t *inner,
                                      size_t outer_cmp_index,
                                      size_t inner_cmp_index) {
  for (size_t i = 0; i < f->len; ++i) {
    if (i == outer_cmp_index || i == inner_cmp_index ||
        (f->data[i].dst >= 0 && (f->data[i].dst == outer->next_value ||
                                 f->data[i].dst == inner->next_value)))
      continue;
    size_t block = cfg->inst_block[i];
    if (block >= cfg->block_count || !inner_blocks[block])
      continue;
    nyr_inst_t *in = &f->data[i];
    if (in->op == NYIR_LABEL || in->op == NYIR_PHI || in->op == NYIR_BR ||
        in->op == NYIR_RET)
      continue;
    in->a = interchange_swap_iv(f, defs, in->a, outer->iv, inner->iv);
    in->b = interchange_swap_iv(f, defs, in->b, outer->iv, inner->iv);
    in->c = interchange_swap_iv(f, defs, in->c, outer->iv, inner->iv);
    in->d = interchange_swap_iv(f, defs, in->d, outer->iv, inner->iv);
    in->e = interchange_swap_iv(f, defs, in->e, outer->iv, inner->iv);
    in->f = interchange_swap_iv(f, defs, in->f, outer->iv, inner->iv);
    for (size_t k = 0; k < in->extra_args_len; ++k)
      in->extra_args[k] =
          interchange_swap_iv(f, defs, in->extra_args[k], outer->iv, inner->iv);
  }
}

/*
 * Interchange requires equal starts/steps/predicates and invariant bounds,
 * rejects unsupported loop-carried state, and proves each memory access safe
 * under the changed iteration schedule. Multiple nonescaping, in-bounds,
 * injective affine stores are supported only when same-allocation stores use
 * the same affine map (or their allocations are distinct).
 */
bool nyr_loop_interchange(nyr_func_t *f) {
  if (!f)
    return true;
  nyr_scev_info_t info = {0};
  nyr_cfg_t cfg = {0};
  int *defs = nyr_build_defs(f);
  if (!defs || !nyr_scev_analyze(f, &info) || !nyr_cfg_build(f, &cfg)) {
    free(defs);
    nyr_scev_free(&info);
    nyr_cfg_free(&cfg);
    return false;
  }
  bool *outer_blocks = calloc(cfg.block_count, sizeof(*outer_blocks));
  bool *inner_blocks = calloc(cfg.block_count, sizeof(*inner_blocks));
  bool *safe_stores = calloc(f->len ? f->len : 1, sizeof(*safe_stores));
  bool has_memory_load = false;
  for (size_t i = 0; i < f->len; ++i) {
    if (f->data[i].op == NYIR_LOAD_I64 &&
        (nyr_effective_effects(&f->data[i]) & NYIR_EFFECT_READ_MEMORY)) {
      has_memory_load = true;
      break;
    }
  }
  size_t value_count = f->next_value > 0 ? (size_t)f->next_value : 1;
  uint8_t *read_state =
      has_memory_load ? calloc(value_count, sizeof(*read_state)) : NULL;
  if (!outer_blocks || !inner_blocks || !safe_stores ||
      (has_memory_load && !read_state)) {
    free(outer_blocks);
    free(inner_blocks);
    free(safe_stores);
    free(read_state);
    free(defs);
    nyr_scev_free(&info);
    nyr_cfg_free(&cfg);
    return false;
  }
  size_t pairs = 0;
  size_t rejected_nesting = 0, rejected_bounds = 0;
  size_t rejected_control_uses = 0, rejected_compare = 0;
  size_t rejected_constant = 0;
  size_t rejected_effects = 0;
  for (size_t oi = 0; oi < info.count; ++oi) {
    nyr_scev_loop_t *outer = &info.loops[oi];
    if (!nyr_cfg_natural_loop_blocks(&cfg, outer->latch_block,
                                     outer->header_block, outer_blocks,
                                     cfg.block_count))
      continue;
    for (size_t ii = 0; ii < info.count; ++ii) {
      nyr_scev_loop_t *inner = &info.loops[ii];
      if (outer == inner || !nyr_cfg_natural_loop_blocks(
                                &cfg, inner->latch_block, inner->header_block,
                                inner_blocks, cfg.block_count))
        continue;
      ++pairs;
      bool nested = inner->header_block != outer->header_block;
      for (size_t block = 0; block < cfg.block_count && nested; ++block)
        if (inner_blocks[block] && !outer_blocks[block])
          nested = false;
      if (!nested) {
        ++rejected_nesting;
        continue;
      }
      if (!outer->trip_count_known || !inner->trip_count_known ||
          !outer->init_is_const || !inner->init_is_const ||
          outer->init != inner->init || outer->step != inner->step ||
          outer->predicate != inner->predicate || !outer->limit_is_const ||
          !inner->limit_is_const || outer->limit == inner->limit) {
        ++rejected_bounds;
        continue;
      }
      nyr_inst_t *outer_cmp = loop_cmp(f, &cfg, defs, outer);
      nyr_inst_t *inner_cmp = loop_cmp(f, &cfg, defs, inner);
      if (!outer_cmp || !inner_cmp) {
        ++rejected_compare;
        continue;
      }
      int outer_limit = interchange_root_copy(f, defs, outer_cmp->b);
      int inner_limit = interchange_root_copy(f, defs, inner_cmp->b);
      int outer_def = outer_limit >= 0 ? defs[outer_limit] : -1;
      int inner_def = inner_limit >= 0 ? defs[inner_limit] : -1;
      if (outer_def < 0 || inner_def < 0 ||
          f->data[outer_def].op != NYIR_CONST_I64 ||
          f->data[inner_def].op != NYIR_CONST_I64) {
        ++rejected_constant;
        continue;
      }
      memset(safe_stores, 0, f->len * sizeof(*safe_stores));
      size_t safe_store_count = 0;
      if (!interchange_affine_store_candidate(f, &info, defs, &cfg,
                                              inner_blocks, outer, inner,
                                              safe_stores, &safe_store_count) ||
          (safe_store_count != 0 &&
           (!interchange_inner_defs_do_not_escape(f, &cfg, inner_blocks) ||
            !interchange_iv_copies_do_not_escape(f, defs, &cfg, inner_blocks,
                                                 outer, inner)))) {
        ++rejected_effects;
        continue;
      }
      if (!iv_has_only_control_uses(f, defs, &cfg, inner_blocks, safe_stores,
                                    outer, inner)) {
        ++rejected_control_uses;
        continue;
      }
      if (read_state && f->next_value > 0)
        memset(read_state, 0, (size_t)f->next_value);
      if (!bodies_stateless(f, &cfg, defs, outer_blocks, inner_blocks,
                            read_state, outer->iv, inner->iv, safe_stores)) {
        ++rejected_effects;
        continue;
      }
      size_t outer_cmp_index = (size_t)(outer_cmp - f->data);
      size_t inner_cmp_index = (size_t)(inner_cmp - f->data);
      if (safe_store_count != 0)
        interchange_swap_body_ivs(f, defs, &cfg, inner_blocks, outer, inner,
                                  outer_cmp_index, inner_cmp_index);
      /*
       * Mutating shared constant definitions would alter unrelated uses and
       * leave their range metadata stale.
       */
      outer_cmp->b = inner_limit;
      inner_cmp->b = outer_limit;
      if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
        ny_dbg_heuristic_event(
            "LOOP_INTERCHANGE",
            "accepted pair=%zu proof=rectangular-same-init-step-predicate; "
            "distinct-constant-bounds; induction-remap=%s; "
            "private-affine-store=%s; stateless-bodies",
            pairs, safe_store_count == 0 ? "none" : "inner-body",
            safe_store_count == 0 ? "no" : "yes");
      free(outer_blocks);
      free(inner_blocks);
      free(safe_stores);
      free(read_state);
      free(defs);
      nyr_scev_free(&info);
      nyr_cfg_free(&cfg);
      return true;
    }
  }
  free(outer_blocks);
  free(inner_blocks);
  free(safe_stores);
  free(read_state);
  free(defs);
  nyr_scev_free(&info);
  nyr_cfg_free(&cfg);
  if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
    ny_dbg_heuristic_event(
        "LOOP_INTERCHANGE",
        "no rewrite pairs=%zu rejected={nesting:%zu,bounds:%zu,iv-uses:%zu,"
        "compare:%zu,constant-limits:%zu,effects:%zu}",
        pairs, rejected_nesting, rejected_bounds, rejected_control_uses,
        rejected_compare, rejected_constant, rejected_effects);
  return true;
}

/*
 * Unroll-and-jam for canonical two-block counted loops.
 *
 * A canonical loop {header, latch} with a pure-scalar body and a known, even
 * trip count is unrolled by a factor of two inside the loop: each trip runs
 * the first body at `iv` and a fetched clone of the body at `iv + 1`, then
 * advances the induction by two.  The header guard (iv < T) is unchanged; the
 * step-2 induction halves the trip naturally and runs exactly the original T
 * body executions in order.
 *
 *   Before (step 1, T even):
 *     H: iv=phi(0, back); c=cmp(iv,T); br_if c -> L
 *     L: body(iv,...); next=phiBack(body outputs); iv'=iv+1; br -> H
 *
 *   After (step 2):
 *     H: iv=phi(0, back); c=cmp(iv,T); br_if c -> L
 *     L: body(iv,...); body(iv+1, carriedNext,...); next2=...; br -> H
 *
 * Correctness: the second body's uses of each header PHI value map to the
 * first body's carried output, and its own step writes iv+2; the guard still
 * runs while iv < T, i.e. T/2 trips of two bodies each.  Loop-carried values
 * flow through the first body into the second, preserving order and data
 * dependencies.
 *
 * Constraints: pure-scalar payload-free latch body (no memory/calls/traps/
 * control), SCEV-proven even trip count, canonical LT guard on the SCEV
 * induction with constant trip bound, step +1, exactly two natural-loop
 * blocks.
 *
 * Transactional: operates on a clone and commits only after nyr_ir_verify.
 */

static bool jam_body_pure(const nyr_inst_t *in) {
  if (!nyr_inst_payload_free(in))
    return false;
  if (nyr_inst_effects(in) != NYIR_EFFECT_NONE)
    return false;
  switch (in->op) {
  case NYIR_ADD_I64:
  case NYIR_SUB_I64:
  case NYIR_MUL_I64:
  case NYIR_DIV_I64:
  case NYIR_MOD_I64:
  case NYIR_AND_I64:
  case NYIR_OR_I64:
  case NYIR_XOR_I64:
  case NYIR_SHL_I64:
  case NYIR_SAR_I64:
  case NYIR_COPY:
  case NYIR_CONST_I64:
  case NYIR_SELECT_I64:
    return true;
  default:
    return false;
  }
}

static int jam_map_value(const int *map, size_t map_len, int value) {
  if (value < 0)
    return value;
  if ((size_t)value < map_len && map[value] >= 0)
    return map[value];
  return value;
}

static bool jam_try_one(nyr_func_t *work, const nyr_scev_loop_t *loop) {
  if (!work || !loop || !loop->trip_count_known || loop->trip_count < 2 ||
      (loop->trip_count & 1) != 0 || loop->step != 1)
    return false;

  nyr_cfg_t cfg = {0};
  bool *in_loop = NULL;
  int *map = NULL;
  bool changed = false;
  if (!nyr_cfg_build(work, &cfg))
    goto out;
  if (loop->header_block >= cfg.block_count ||
      loop->latch_block >= cfg.block_count ||
      loop->preheader_block >= cfg.block_count ||
      loop->header_block == loop->latch_block)
    goto out;

  in_loop = calloc(cfg.block_count, sizeof(*in_loop));
  if (!in_loop ||
      !nyr_cfg_natural_loop_blocks(&cfg, loop->latch_block, loop->header_block,
                                   in_loop, cfg.block_count))
    goto out;
  size_t loop_blocks = 0;
  for (size_t b = 0; b < cfg.block_count; ++b)
    loop_blocks += in_loop[b] ? 1u : 0u;
  if (loop_blocks != 2 || !in_loop[loop->header_block] ||
      !in_loop[loop->latch_block])
    goto out;

  const size_t h = loop->header_block, l = loop->latch_block;
  const size_t p = loop->preheader_block;
  int64_t hlabel = cfg.block_label[h];
  int64_t llabel = cfg.block_label[l];
  if (hlabel < 0 || llabel < 0)
    goto out;

  size_t pend = nyr_block_last_non_nop(work, &cfg, p);
  size_t hend = nyr_block_last_non_nop(work, &cfg, h);
  size_t lend = nyr_block_last_non_nop(work, &cfg, l);
  if (pend <= cfg.block_start[p] || hend <= cfg.block_start[h] ||
      lend <= cfg.block_start[l])
    goto out;
  nyr_inst_t *pterm = &work->data[pend - 1];
  const nyr_inst_t *hterm = &work->data[hend - 1];
  const nyr_inst_t *lterm = &work->data[lend - 1];
  if (pterm->op != NYIR_BR || pterm->imm != hlabel || hterm->op != NYIR_BR_IF ||
      lterm->op != NYIR_BR || lterm->imm != hlabel)
    goto out;
  size_t taken = SIZE_MAX;
  for (size_t b = 0; b < cfg.block_count; ++b)
    if (cfg.block_label[b] == hterm->imm) {
      taken = b;
      break;
    }
  if (taken != l)
    goto out;

  /*
   * Header scan: PHIs, then one LT cmp(iv, const), then the br_if at hend-1.
   */
  size_t phi_begin = cfg.block_start[h];
  if (phi_begin < cfg.block_end[h] && work->data[phi_begin].op == NYIR_LABEL)
    ++phi_begin;
  while (phi_begin < cfg.block_end[h] && work->data[phi_begin].op == NYIR_NOP)
    ++phi_begin;

  size_t map_len = (size_t)(work->next_value > 0 ? work->next_value : 1);
  map = malloc(map_len * sizeof(*map));
  if (!map)
    goto out;
  for (size_t i = 0; i < map_len; ++i)
    map[i] = -1;

  int phi_dst[32], phi_back[32];
  int phi_count = 0;
  bool saw_phi = false;
  int guard_iv = -1, guard_trip = -1;
  bool have_guard = false;
  for (size_t i = phi_begin; i < hend - 1; ++i) {
    nyr_inst_t *in = &work->data[i];
    if (in->op == NYIR_NOP)
      continue;
    if (in->op == NYIR_PHI) {
      saw_phi = true;
      if (in->dst < 0 || (size_t)in->dst >= map_len ||
          in->phi_incoming_len != 2 || phi_count >= 32)
        goto out;
      int back = -1;
      bool have_latch = false;
      for (size_t k = 0; k < in->phi_incoming_len; ++k) {
        const nyr_phi_incoming_t *inc = &in->phi_incoming[k];
        if (inc->predecessor_label == llabel) {
          back = inc->value;
          have_latch = true;
        }
      }
      if (!have_latch || back < 0)
        goto out;
      phi_dst[phi_count] = in->dst;
      phi_back[phi_count] = back;
      ++phi_count;
      continue;
    }
    if (in->op == NYIR_CMP_I64) {
      if (have_guard || in->cmp != NYIR_CMP_LT || in->a < 0 || in->b < 0)
        goto out;
      guard_iv = in->a;
      guard_trip = in->b;
      have_guard = true;
      continue;
    }
    goto out; /* unexpected instruction before the terminator */
  }
  if (!saw_phi || !have_guard)
    goto out;
  if (guard_iv != loop->iv)
    goto out;
  int64_t trip_imm = 0;
  bool trip_is_const = false;
  for (size_t i = 0; i <= pend; ++i) {
    if (work->data[i].op == NYIR_CONST_I64 && work->data[i].dst == guard_trip) {
      trip_imm = work->data[i].imm;
      trip_is_const = true;
      break;
    }
  }
  if (!trip_is_const || trip_imm != (int64_t)loop->trip_count)
    goto out;

  /*
   * Latch body boundaries.
   */
  size_t body_begin = cfg.block_start[l];
  if (body_begin < cfg.block_end[l] && work->data[body_begin].op == NYIR_LABEL)
    ++body_begin;
  while (body_begin < lend - 1 && work->data[body_begin].op == NYIR_NOP)
    ++body_begin;
  if (body_begin >= lend - 1)
    goto out;
  for (size_t i = body_begin; i + 1 < lend; ++i) {
    const nyr_inst_t *in = &work->data[i];
    if (in->op == NYIR_NOP)
      continue;
    if (!jam_body_pure(in))
      goto out;
  }

  /*
   * Each carried value must be defined exactly once inside the latch body.
   */
  for (int q = 0; q < phi_count; ++q) {
    int count = 0;
    for (size_t i = body_begin; i + 1 < lend; ++i)
      if (work->data[i].dst == phi_back[q])
        ++count;
    if (count != 1)
      goto out;
  }

  /*
   * Jamming halves the executions of body definitions that are NOT
   * carried through a header PHI: after the rewrite, only the cloned
   * (odd-iteration) copy produces them on the final iteration, so any
   * use outside the latch body would read the second-to-last
   * iteration's value. Require every non-carried body def to be dead
   * outside its own body region (same discipline as softpipe's
   * stage-A liveness check).
   */
  {
    bool carried[32];
    for (int q = 0; q < phi_count; ++q)
      carried[q] = true;
    for (size_t i = body_begin; i + 1 < lend; ++i) {
      const nyr_inst_t *in = &work->data[i];
      int d = in->dst;
      if (d < 0)
        continue;
      bool is_carried = false;
      for (int q = 0; q < phi_count; ++q)
        if (d == phi_back[q])
          is_carried = true;
      if (is_carried)
        continue;
      for (size_t j = 0; j < work->len; ++j) {
        if (j >= body_begin && j + 1 < lend)
          continue;
        const nyr_inst_t *user = &work->data[j];
        if (user->op == NYIR_NOP)
          continue;
        const int ops[6] = {user->a, user->b, user->c,
                            user->d, user->e, user->f};
        for (int o = 0; o < 6; ++o)
          if (ops[o] == d)
            goto out;
        for (size_t k = 0; k < user->extra_args_len; ++k)
          if (user->extra_args[k] == d)
            goto out;
        if (user->op == NYIR_PHI && user->phi_incoming) {
          for (size_t k = 0; k < user->phi_incoming_len; ++k)
            if (user->phi_incoming[k].value == d)
              goto out;
        }
      }
    }
  }

  /*
   * Mapping for the fetched second body: a use of a header PHI value (the
   * value at the start of this iteration) becomes the first body's carried
   * output for that PHI, i.e. the value at the start of the next iteration.
   */
  for (int q = 0; q < phi_count; ++q)
    map[phi_dst[q]] = phi_back[q];

  int new_back[32];
  for (int q = 0; q < phi_count; ++q)
    new_back[q] = -1;

  /*
   * Insert the fetched clone before the latch terminator.
   */
  size_t insert_at = lend; /* position of the trailing br; insert before it */
  for (size_t i = body_begin; i + 1 < lend; ++i) {
    const nyr_inst_t *src = &work->data[i];
    if (src->op == NYIR_NOP)
      continue;
    nyr_inst_t in = *src;
    int old_dst = in.dst;
    if (in.dst >= 0) {
      if ((size_t)in.dst >= map_len || work->next_value == INT_MAX)
        goto out;
      int fresh = work->next_value++;
      map[in.dst] = fresh;
      in.dst = fresh;
    }
    in.a = jam_map_value(map, map_len, in.a);
    in.b = jam_map_value(map, map_len, in.b);
    in.c = jam_map_value(map, map_len, in.c);
    in.d = jam_map_value(map, map_len, in.d);
    in.e = jam_map_value(map, map_len, in.e);
    in.f = jam_map_value(map, map_len, in.f);
    if (!nir_ensure_inst_space(work, 1))
      goto out;
    memmove(&work->data[insert_at + 1], &work->data[insert_at],
            (work->len - insert_at) * sizeof(*work->data));
    work->data[insert_at] = in;
    work->data[insert_at].effects = nyr_inst_effects(&work->data[insert_at]);
    work->len += 1;
    ++insert_at;
    if (old_dst >= 0)
      for (int q = 0; q < phi_count; ++q)
        if (old_dst == phi_back[q])
          new_back[q] = (int)map[phi_back[q]];
  }

  /*
   * Wire the header PHIs to the fetched body's outputs.
   */
  for (int q = 0; q < phi_count; ++q) {
    if (new_back[q] < 0)
      goto out;
    if ((size_t)new_back[q] >= map_len) {
      /*
       * Allocate a fresh id from map[phi_back] only when mapped.
       */
      continue;
    }
    for (size_t i = phi_begin; i < hend - 1; ++i) {
      nyr_inst_t *phi = &work->data[i];
      if (phi->op != NYIR_PHI || phi->dst != phi_dst[q])
        continue;
      for (size_t k = 0; k < phi->phi_incoming_len; ++k) {
        nyr_phi_incoming_t *inc = &phi->phi_incoming[k];
        if (inc->predecessor_label == llabel) {
          inc->value = new_back[q];
        }
      }
    }
  }

  char err[256] = {0};
  changed = nyr_ir_verify(work, err, sizeof(err));

out:
  free(map);
  free(in_loop);
  nyr_cfg_free(&cfg);
  return changed;
}

bool nyr_unroll_jam(nyr_func_t *f) {
  if (!f || f->len == 0)
    return true;

  nyr_scev_info_t info = {0};
  if (!nyr_scev_analyze(f, &info))
    return false;

  size_t rejected_trip_proof = 0, rejected_trip_shape = 0;
  size_t rejected_legality = 0, rejected_verifier = 0;

  for (size_t i = 0; i < info.count; ++i) {
    const nyr_scev_loop_t *loop = &info.loops[i];
    if (!loop->trip_count_known) {
      ++rejected_trip_proof;
      continue;
    }
    if (loop->trip_count < 2 || (loop->trip_count & 1) != 0 ||
        loop->limit_value < 0 || loop->step != 1) {
      ++rejected_trip_shape;
      continue;
    }

    nyr_func_t candidate = {0};
    if (!nyr_ir_func_clone(f, &candidate)) {
      nyr_scev_free(&info);
      return false;
    }
    if (jam_try_one(&candidate, loop)) {
      char err[256] = {0};
      if (nyr_ir_verify(&candidate, err, sizeof(err))) {
        nyr_func_replace(f, &candidate);
        if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
          ny_dbg_heuristic_event(
              "UNROLL_JAM",
              "accepted loop=%zu trip=%lld factor=2 proof=even-constant-trip; "
              "canonical-two-block-pure-scalar-body; verifier=pass",
              i, (long long)loop->trip_count);
        nyr_scev_free(&info);
        return true;
      }
      ++rejected_verifier;
      if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
        ny_dbg_heuristic_event("UNROLL_JAM",
                               "rejected loop=%zu gate=verifier reason=%s", i,
                               err[0] ? err : "unknown verifier failure");
    } else {
      ++rejected_legality;
    }
    nyr_ir_func_free(&candidate);
  }

  if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
    ny_dbg_heuristic_event(
        "UNROLL_JAM",
        "no rewrite loops=%zu rejected={trip-proof:%zu,trip-shape:%zu,"
        "body-legality:%zu,verifier:%zu}",
        info.count, rejected_trip_proof, rejected_trip_shape, rejected_legality,
        rejected_verifier);
  nyr_scev_free(&info);
  return true;
}
/*
 * First-iteration loop peeling for canonical two-block counted loops.
 *
 * The transform is intentionally transactional: operate on a deep clone and
 * commit only after the NYIR verifier accepts the rewritten CFG/SSA graph.
 * It accepts a loop only when SCEV proves at least one iteration, the natural
 * loop is exactly {header,latch}, the preheader/latch both branch directly to
 * the header, and the latch body contains no nested control flow or owned
 * instruction payloads.  Memory effects are allowed: a positive trip count
 * proves the original guard executes the first body iteration as well.
 *
 * Before:
 *   P -> H(phi init from P, backedge from L)
 *   H --true--> L --back--> H
 *
 * After:
 *   P -> PEEL(init-substituted clone of L) -> H
 *   H(phi first value from PEEL, backedge from L)
 */

/*
 * Peeling moves the latch body AHEAD of the header prefix. When BOTH
 * sides touch observable state (raw memory, local slots, calls), the
 * move reorders a store/load pair across the boundary and aliasing
 * programs observe a different final memory state. Rejecting only when
 * both sides touch state keeps every pure-guard peel legal.
 */
static bool peel_touches_state(const nyr_inst_t *in) {
  unsigned e = nyr_inst_effects(in);
  return (e & (NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_WRITE_MEMORY |
               NYIR_EFFECT_READ_LOCAL | NYIR_EFFECT_WRITE_LOCAL |
               NYIR_EFFECT_UNKNOWN_SIDE_EFFECT | NYIR_EFFECT_VOLATILE)) != 0;
}

static int peel_map_value(const int *map, size_t map_len, int value) {
  if (value < 0)
    return value;
  if ((size_t)value < map_len && map[value] >= 0)
    return map[value];
  return value;
}

static bool peel_try_one(nyr_func_t *work, const nyr_scev_loop_t *loop) {
  if (!work || !loop || !loop->trip_count_known || loop->trip_count == 0)
    return false;

  nyr_cfg_t cfg = {0};
  bool *in_loop = NULL;
  int *map = NULL;
  nyr_inst_t *clone = NULL;
  bool changed = false;
  if (!nyr_cfg_build(work, &cfg))
    goto out;
  if (loop->header_block >= cfg.block_count ||
      loop->latch_block >= cfg.block_count ||
      loop->preheader_block >= cfg.block_count ||
      loop->header_block == loop->latch_block)
    goto out;

  in_loop = calloc(cfg.block_count, sizeof(*in_loop));
  if (!in_loop ||
      !nyr_cfg_natural_loop_blocks(&cfg, loop->latch_block, loop->header_block,
                                   in_loop, cfg.block_count))
    goto out;
  size_t loop_blocks = 0;
  for (size_t b = 0; b < cfg.block_count; ++b)
    loop_blocks += in_loop[b] ? 1u : 0u;
  if (loop_blocks != 2 || !in_loop[loop->header_block] ||
      !in_loop[loop->latch_block])
    goto out;

  const size_t h = loop->header_block, l = loop->latch_block;
  const size_t p = loop->preheader_block;
  int64_t hlabel = cfg.block_label[h];
  int64_t plabel = cfg.block_label[p];
  int64_t llabel = cfg.block_label[l];
  if (hlabel < 0 || llabel < 0)
    goto out;

  size_t pend = nyr_block_last_non_nop(work, &cfg, p);
  size_t hend = nyr_block_last_non_nop(work, &cfg, h);
  size_t lend = nyr_block_last_non_nop(work, &cfg, l);
  if (pend <= cfg.block_start[p] || hend <= cfg.block_start[h] ||
      lend <= cfg.block_start[l])
    goto out;
  nyr_inst_t *pterm = &work->data[pend - 1];
  const nyr_inst_t *hterm = &work->data[hend - 1];
  const nyr_inst_t *lterm = &work->data[lend - 1];
  if (pterm->op != NYIR_BR || pterm->imm != hlabel || hterm->op != NYIR_BR_IF ||
      lterm->op != NYIR_BR || lterm->imm != hlabel)
    goto out;
  size_t taken = SIZE_MAX;
  for (size_t b = 0; b < cfg.block_count; ++b)
    if (cfg.block_label[b] == hterm->imm) {
      taken = b;
      break;
    }
  if (taken != l)
    goto out;

  /*
   * Header must contain only PHIs/NOPs before ordinary guard computation.
   */
  size_t phi_begin = cfg.block_start[h];
  if (phi_begin < cfg.block_end[h] && work->data[phi_begin].op == NYIR_LABEL)
    ++phi_begin;
  while (phi_begin < cfg.block_end[h] && work->data[phi_begin].op == NYIR_NOP)
    ++phi_begin;

  size_t map_len = (size_t)(work->next_value > 0 ? work->next_value : 1);
  map = malloc(map_len * sizeof(*map));
  if (!map)
    goto out;
  for (size_t i = 0; i < map_len; ++i)
    map[i] = -1;

  bool saw_phi = false;
  for (size_t i = phi_begin; i < hend - 1; ++i) {
    nyr_inst_t *phi = &work->data[i];
    if (phi->op == NYIR_NOP)
      continue;
    if (phi->op != NYIR_PHI)
      break;
    saw_phi = true;
    if (phi->dst < 0 || (size_t)phi->dst >= map_len ||
        phi->phi_incoming_len != 2)
      goto out;
    int init = -1;
    bool have_pre = false, have_latch = false;
    for (size_t k = 0; k < phi->phi_incoming_len; ++k) {
      const nyr_phi_incoming_t *inc = &phi->phi_incoming[k];
      if (inc->predecessor_label == plabel) {
        init = inc->value;
        have_pre = true;
      } else if (inc->predecessor_label == llabel) {
        have_latch = true;
      }
    }
    if (!have_pre || !have_latch || init < 0)
      goto out;
    map[phi->dst] = init;
  }
  if (!saw_phi)
    goto out;

  /*
   * Clone the latch body with first-iteration PHI substitutions.
   */
  size_t body_begin = cfg.block_start[l];
  if (body_begin < cfg.block_end[l] && work->data[body_begin].op == NYIR_LABEL)
    ++body_begin;
  while (body_begin < lend - 1 && work->data[body_begin].op == NYIR_NOP)
    ++body_begin;
  size_t body_cap = lend > body_begin ? lend - body_begin - 1 : 0;
  if (body_cap == 0)
    goto out;
  clone = calloc(body_cap, sizeof(*clone));
  if (!clone)
    goto out;
  size_t clone_len = 0;
  bool body_touches_state = false;
  for (size_t i = body_begin; i < lend - 1; ++i) {
    const nyr_inst_t *src = &work->data[i];
    if (src->op == NYIR_NOP)
      continue;
    if (src->op == NYIR_LABEL || src->op == NYIR_PHI || src->op == NYIR_BR ||
        src->op == NYIR_BR_IF || src->op == NYIR_RET ||
        !nyr_inst_payload_free(src))
      goto out;
    if (peel_touches_state(src))
      body_touches_state = true;
    nyr_inst_t in = *src;
    if (in.dst >= 0) {
      if ((size_t)in.dst >= map_len || work->next_value == INT_MAX)
        goto out;
      int fresh = work->next_value++;
      map[in.dst] = fresh;
      in.dst = fresh;
    }
    in.a = peel_map_value(map, map_len, in.a);
    in.b = peel_map_value(map, map_len, in.b);
    in.c = peel_map_value(map, map_len, in.c);
    in.d = peel_map_value(map, map_len, in.d);
    in.e = peel_map_value(map, map_len, in.e);
    in.f = peel_map_value(map, map_len, in.f);
    clone[clone_len++] = in;
  }
  if (clone_len == 0)
    goto out;

  /*
   * Header-prefix state check (see peel_touches_state): if the cloned
   * body AND the header prefix both touch observable state, the peel
   * would reorder them.
   */
  {
    bool prefix_touches_state = false;
    for (size_t i = phi_begin; i < hend - 1; ++i) {
      const nyr_inst_t *in = &work->data[i];
      if (in->op == NYIR_NOP || in->op == NYIR_PHI)
        continue;
      if (peel_touches_state(in))
        prefix_touches_state = true;
    }
    if (body_touches_state && prefix_touches_state)
      goto out;
  }

  int64_t peeled_label = nyr_fresh_label(work);
  if (peeled_label < 0)
    goto out;

  /*
   * Rewire each header PHI to consume the value produced by the peel.
   */
  for (size_t i = phi_begin; i < hend - 1; ++i) {
    nyr_inst_t *phi = &work->data[i];
    if (phi->op == NYIR_NOP)
      continue;
    if (phi->op != NYIR_PHI)
      break;
    int back_value = -1;
    for (size_t k = 0; k < phi->phi_incoming_len; ++k)
      if (phi->phi_incoming[k].predecessor_label == llabel)
        back_value = phi->phi_incoming[k].value;
    if (back_value < 0)
      goto out;
    int first_result = peel_map_value(map, map_len, back_value);
    for (size_t k = 0; k < phi->phi_incoming_len; ++k) {
      nyr_phi_incoming_t *inc = &phi->phi_incoming[k];
      if (inc->predecessor_label == plabel) {
        inc->predecessor_label = peeled_label;
        inc->value = first_result;
      }
    }
  }
  pterm->imm = peeled_label;

  /*
   * Insert the peeled block immediately before the old header.
   */
  size_t insert = cfg.block_start[h];
  size_t add = clone_len + 2;
  if (!nir_ensure_inst_space(work, add))
    goto out;
  memmove(&work->data[insert + add], &work->data[insert],
          (work->len - insert) * sizeof(*work->data));
  work->len += add;
  work->data[insert] = (nyr_inst_t){.op = NYIR_LABEL,
                                    .dst = -1,
                                    .a = -1,
                                    .b = -1,
                                    .c = -1,
                                    .d = -1,
                                    .e = -1,
                                    .f = -1,
                                    .imm = peeled_label};
  memcpy(&work->data[insert + 1], clone, clone_len * sizeof(*clone));
  work->data[insert + 1 + clone_len] =
      (nyr_inst_t){.op = NYIR_BR,
                   .dst = -1,
                   .a = -1,
                   .b = -1,
                   .c = -1,
                   .d = -1,
                   .e = -1,
                   .f = -1,
                   .imm = hlabel,
                   .effects = NYIR_EFFECT_CONTROL};

  char err[256] = {0};
  changed = nyr_ir_verify(work, err, sizeof(err));

out:
  free(clone);
  free(map);
  free(in_loop);
  nyr_cfg_free(&cfg);
  return changed;
}

bool nyr_loop_peel(nyr_func_t *f) {
  if (!f || f->len == 0)
    return true;

  /*
   * Analyze the immutable source once.  Each candidate gets its own clone so
   * a rejected peel can never leak fresh SSA ids or partially rewritten PHIs
   * into the next candidate.
   */
  nyr_scev_info_t info = {0};
  if (!nyr_scev_analyze(f, &info))
    return false;

  for (size_t i = 0; i < info.count; ++i) {
    const nyr_scev_loop_t *loop = &info.loops[i];
    if (!loop->trip_count_known || loop->trip_count == 0 ||
        loop->limit_value < 0)
      continue;

    nyr_func_t candidate = {0};
    if (!nyr_ir_func_clone(f, &candidate)) {
      nyr_scev_free(&info);
      return false;
    }
    if (peel_try_one(&candidate, loop)) {
      char err[256] = {0};
      if (nyr_ir_verify(&candidate, err, sizeof(err))) {
        nyr_func_replace(f, &candidate);
        nyr_scev_free(&info);
        return true;
      }
    }
    nyr_ir_func_free(&candidate);
  }

  nyr_scev_free(&info);
  return true;
}

/*
 * Conservative loop predication for empty integer diamonds.
 *
 * Convert a loop-local control-only diamond selecting two already-computed
 * i64 values into wrapping arithmetic:
 *
 *   result = false_value + condition * (true_value - false_value)
 *
 * The transform is deliberately general (it is not a vector-tail guard), but
 * only fires when both arms contain no executable instruction other than their
 * branch to the same merge, both selected values dominate the condition, and
 * the condition is verifier-proven boolean.  Nothing is speculated and NyIR's
 * wrapping integer semantics make the select identity exact.
 */

typedef struct {
  int64_t target;
  int64_t local;
  int value;
} predicated_arm_t;

/*
 * Frontend conditionals commonly materialize their result through one local
 * even after mem2reg.  Accept exactly COPY*; STORE_LOCAL; BR and resolve the
 * copied value back to a definition that dominates the diamond.
 */
static bool parse_store_arm(const nyr_func_t *f, const nyr_cfg_t *cfg,
                            const int *defs, size_t block,
                            predicated_arm_t *arm) {
  if (!f || !cfg || !defs || !arm || block >= cfg->block_count)
    return false;
  *arm = (predicated_arm_t){.target = -1, .local = -1, .value = -1};
  bool saw_store = false, saw_branch = false;
  for (size_t i = cfg->block_start[block]; i < cfg->block_end[block]; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op == NYIR_LABEL || in->op == NYIR_NOP || in->op == NYIR_COPY)
      continue;
    if (in->op == NYIR_STORE_LOCAL && !saw_store && !saw_branch) {
      saw_store = true;
      arm->local = in->imm;
      arm->value = in->a;
      continue;
    }
    if (in->op == NYIR_BR && saw_store && !saw_branch) {
      saw_branch = true;
      arm->target = in->imm;
      continue;
    }
    return false;
  }
  while (arm->value >= 0 && arm->value < f->next_value &&
         defs[arm->value] >= 0 && f->data[defs[arm->value]].op == NYIR_COPY)
    arm->value = f->data[defs[arm->value]].a;
  if (saw_store && !saw_branch && block + 1 < cfg->block_count) {
    arm->target = cfg->block_label[block + 1];
    saw_branch = true;
  }
  return saw_store && saw_branch;
}

static size_t skip_branch_trampoline(const nyr_func_t *f, const nyr_cfg_t *cfg,
                                     size_t block) {
  if (!f || !cfg || block >= cfg->block_count)
    return block;
  int64_t target = -1;
  for (size_t i = cfg->block_start[block]; i < cfg->block_end[block]; ++i) {
    if (f->data[i].op == NYIR_LABEL || f->data[i].op == NYIR_NOP)
      continue;
    if (f->data[i].op == NYIR_BR && target < 0) {
      target = f->data[i].imm;
      continue;
    }
    return block;
  }
  if (target < 0)
    return block;
  for (size_t b = 0; b < cfg->block_count; ++b)
    if (cfg->block_label[b] == target)
      return b;
  return block;
}

static bool bool_condition(const nyr_func_t *f, const int *defs, int value) {
  if (!f || !defs || value < 0 || value >= f->next_value || defs[value] < 0)
    return false;
  const nyr_inst_t *def = &f->data[defs[value]];
  return def->op == NYIR_CMP_I64 ||
         (def->range.has_min && def->range.has_max && def->range.min >= 0 &&
          def->range.max <= 1);
}

static bool value_dominates_block(const nyr_cfg_t *cfg, const int *defs,
                                  int value, size_t block) {
  return value >= 0 && defs[value] >= 0 &&
         nyr_cfg_dominates(cfg, cfg->inst_block[(size_t)defs[value]], block);
}

static bool replace_with_five(nyr_func_t *f, size_t at,
                              const nyr_inst_t replacement[5]) {
  if (!nir_ensure_inst_space(f, 4))
    return false;
  memmove(&f->data[at + 5], &f->data[at + 1],
          (f->len - at - 1) * sizeof(*f->data));
  memcpy(&f->data[at], replacement, 5 * sizeof(*replacement));
  f->len += 4;
  return true;
}

bool nyr_loop_predication(nyr_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;
  nyr_cfg_t cfg = {0};
  int *defs = nyr_build_defs(f);
  if (!defs || !nyr_cfg_build(f, &cfg)) {
    free(defs);
    nyr_cfg_free(&cfg);
    return false;
  }

  bool *loop_member = calloc(cfg.block_count, sizeof(*loop_member));
  bool *natural = calloc(cfg.block_count, sizeof(*natural));
  if (!loop_member || !natural) {
    free(loop_member);
    free(natural);
    nyr_cfg_free(&cfg);
    free(defs);
    return false;
  }
  for (size_t latch = 0; latch < cfg.block_count; ++latch) {
    for (size_t edge = cfg.succ_offsets[latch];
         edge < cfg.succ_offsets[latch + 1]; ++edge) {
      size_t header = cfg.succ_blocks[edge];
      if (!nyr_cfg_is_backedge(&cfg, latch, header))
        continue;
      if (!nyr_cfg_natural_loop_blocks(&cfg, latch, header, natural,
                                       cfg.block_count)) {
        free(loop_member);
        free(natural);
        nyr_cfg_free(&cfg);
        free(defs);
        return false;
      }
      for (size_t block = 0; block < cfg.block_count; ++block)
        loop_member[block] = loop_member[block] || natural[block];
    }
  }

  for (size_t head = 0; head < cfg.block_count; ++head) {
    if (!loop_member[head])
      continue;
    size_t end = cfg.block_end[head];
    while (end > cfg.block_start[head] && f->data[end - 1].op == NYIR_NOP)
      --end;
    if (end == cfg.block_start[head] || f->data[end - 1].op != NYIR_BR_IF)
      continue;
    size_t br_i = end - 1;
    nyr_inst_t *br = &f->data[br_i];
    if (!bool_condition(f, defs, br->a))
      continue;

    size_t true_block = SIZE_MAX;
    for (size_t b = 0; b < cfg.block_count; ++b)
      if (cfg.block_label[b] == br->imm)
        true_block = b;
    size_t false_block = head + 1;
    if (true_block == SIZE_MAX || false_block >= cfg.block_count)
      continue;
    false_block = skip_branch_trampoline(f, &cfg, false_block);
    if (true_block == false_block)
      continue;
    predicated_arm_t true_arm, false_arm;
    if (!parse_store_arm(f, &cfg, defs, true_block, &true_arm) ||
        !parse_store_arm(f, &cfg, defs, false_block, &false_arm) ||
        true_arm.target != false_arm.target ||
        true_arm.local != false_arm.local || true_arm.local < 0 ||
        !value_dominates_block(&cfg, defs, true_arm.value, head) ||
        !value_dominates_block(&cfg, defs, false_arm.value, head))
      continue;

    int sub = f->next_value++;
    int mul = f->next_value++;
    int result = f->next_value++;
    nyr_debug_loc_t debug = br->debug;
    nyr_inst_t replacement[5] = {{.op = NYIR_SUB_I64,
                                  .dst = sub,
                                  .a = true_arm.value,
                                  .b = false_arm.value,
                                  .c = -1,
                                  .d = -1,
                                  .e = -1,
                                  .f = -1,
                                  .debug = debug},
                                 {.op = NYIR_MUL_I64,
                                  .dst = mul,
                                  .a = br->a,
                                  .b = sub,
                                  .c = -1,
                                  .d = -1,
                                  .e = -1,
                                  .f = -1,
                                  .debug = debug},
                                 {.op = NYIR_ADD_I64,
                                  .dst = result,
                                  .a = false_arm.value,
                                  .b = mul,
                                  .c = -1,
                                  .d = -1,
                                  .e = -1,
                                  .f = -1,
                                  .debug = debug},
                                 {.op = NYIR_STORE_LOCAL,
                                  .dst = -1,
                                  .a = result,
                                  .b = -1,
                                  .c = -1,
                                  .d = -1,
                                  .e = -1,
                                  .f = -1,
                                  .imm = true_arm.local,
                                  .debug = debug},
                                 {.op = NYIR_BR,
                                  .dst = -1,
                                  .a = -1,
                                  .b = -1,
                                  .c = -1,
                                  .d = -1,
                                  .e = -1,
                                  .f = -1,
                                  .imm = true_arm.target,
                                  .debug = debug}};
    if (!replace_with_five(f, br_i, replacement)) {
      free(loop_member);
      free(natural);
      nyr_cfg_free(&cfg);
      free(defs);
      return false;
    }
    nyr_refresh_metadata(f);
    free(loop_member);
    free(natural);
    nyr_cfg_free(&cfg);
    free(defs);
    return true;
  }
  free(loop_member);
  free(natural);
  nyr_cfg_free(&cfg);
  free(defs);
  return true;
}

/*
 * The current NyIR loop representation is already rotated at its latch.
 * Removing a branch whose target is the following label is locally
 * equivalent, but it changes block boundaries in a way that has exposed an
 * O3 miscompile in later loop transforms. Keep this pass conservative until
 * those transforms can consume an explicitly rebuilt CFG here.
 */
bool nyr_loop_rotate(nyr_func_t *f) {
  (void)f;
  return true;
}

/*
 * A last-iteration reduction is valid only for a pure loop whose carried
 * values are overwritten from the induction variable and loop-invariant
 * inputs on every iteration. Replacing the induction variable's preheader
 * value with its statically proven final iteration value then executes the
 * same final body state once, while preserving the original exit value.
 *
 * This is intentionally a narrow integer transform. Unknown effects,
 * trapping operations, memory, local writes, early exits, and dependencies on
 * prior carried values all leave the loop untouched.
 */
static bool last_overwrite_pure_opcode(nyr_op_t op) {
  switch (op) {
  case NYIR_NOP:
  case NYIR_CONST_I64:
  case NYIR_COPY:
  case NYIR_PHI:
  case NYIR_ADD_I64:
  case NYIR_SUB_I64:
  case NYIR_MUL_I64:
  case NYIR_AND_I64:
  case NYIR_OR_I64:
  case NYIR_XOR_I64:
  case NYIR_SHL_I64:
  case NYIR_SAR_I64:
  case NYIR_ROR_I64:
  case NYIR_CMP_I64:
  case NYIR_LABEL:
  case NYIR_LOAD_LOCAL:
  case NYIR_BR:
  case NYIR_BR_IF:
  case NYIR_SELECT_I64:
  case NYIR_ROR32_I64:
  case NYIR_BSWAP_I64:
  case NYIR_BSWAP32_I64:
    return true;
  default:
    return false;
  }
}

static bool last_overwrite_value_depends_only_on_iv(const nyr_func_t *f,
                                                    const int *defs,
                                                    const nyr_cfg_t *cfg,
                                                    const bool *in_loop,
                                                    const bool *carried, int iv,
                                                    int value, uint8_t *state) {
  if (value < 0 || value >= f->next_value)
    return false;
  if (value == iv)
    return true;
  if (carried[value])
    return false;
  if (defs[value] < 0)
    return false;
  size_t def_index = (size_t)defs[value];
  if (def_index >= f->len || cfg->inst_block[def_index] >= cfg->block_count)
    return false;
  if (!in_loop[cfg->inst_block[def_index]])
    return true;
  if (state[value] == 1)
    return false;
  if (state[value] == 2)
    return true;
  if (state[value] == 3)
    return false;

  const nyr_inst_t *in = &f->data[def_index];
  if (in->op == NYIR_LOAD_LOCAL)
    return true;
  if (!last_overwrite_pure_opcode(in->op)) {
    state[value] = 3;
    return false;
  }
  state[value] = 1;
  bool ok = true;
  if (in->op == NYIR_PHI) {
    for (size_t k = 0; k < in->phi_incoming_len && ok; ++k)
      ok = last_overwrite_value_depends_only_on_iv(
          f, defs, cfg, in_loop, carried, iv, in->phi_incoming[k].value, state);
  } else {
    const int operands[] = {in->a, in->b, in->c, in->d, in->e, in->f};
    for (size_t k = 0; k < sizeof(operands) / sizeof(operands[0]) && ok; ++k) {
      if (operands[k] >= 0)
        ok = last_overwrite_value_depends_only_on_iv(
            f, defs, cfg, in_loop, carried, iv, operands[k], state);
    }
  }
  state[value] = ok ? 2 : 3;
  return ok;
}

static bool last_overwrite_phi_input(const nyr_inst_t *phi, int64_t label,
                                     int *value) {
  if (!phi || phi->op != NYIR_PHI || !value)
    return false;
  bool found = false;
  for (size_t k = 0; k < phi->phi_incoming_len; ++k) {
    if (phi->phi_incoming[k].predecessor_label != label)
      continue;
    if (found)
      return false;
    *value = phi->phi_incoming[k].value;
    found = true;
  }
  return found;
}

static void last_overwrite_replace_outside_uses(nyr_func_t *f,
                                                const nyr_cfg_t *cfg,
                                                const bool *in_loop, int from,
                                                int to) {
  for (size_t i = 0; i < f->len; ++i) {
    size_t block = cfg->inst_block[i];
    if (block >= cfg->block_count || in_loop[block])
      continue;
    nyr_inst_t *in = &f->data[i];
    int *operands[] = {&in->a, &in->b, &in->c, &in->d, &in->e, &in->f};
    for (size_t k = 0; k < sizeof(operands) / sizeof(operands[0]); ++k)
      if (*operands[k] == from)
        *operands[k] = to;
    for (size_t k = 0; k < in->phi_incoming_len; ++k)
      if (in->phi_incoming[k].value == from)
        in->phi_incoming[k].value = to;
  }
}

static bool last_overwrite_try_loop(nyr_func_t *f, const nyr_cfg_t *cfg,
                                    const int *defs,
                                    const nyr_scev_loop_t *loop,
                                    bool *in_loop) {
#define LAST_OVERWRITE_REJECT(reason)                                          \
  do {                                                                         \
    ny_dbg_heuristic_event("LOOP_LAST_OVERWRITE",                              \
                           "decision=reject header=L%lld reason=%s",           \
                           (long long)loop->header_label, reason);             \
    return false;                                                              \
  } while (0)
  ny_dbg_heuristic_event(
      "LOOP_LAST_OVERWRITE",
      "candidate header=L%lld trip-known=%d trips=%llu step=%lld",
      (long long)loop->header_label, loop->trip_count_known,
      (unsigned long long)loop->trip_count, (long long)loop->step);
  if (!loop->init_is_const || !loop->trip_count_known || loop->trip_count < 2 ||
      loop->step == 0 || loop->header_block >= cfg->block_count ||
      loop->latch_block >= cfg->block_count ||
      loop->preheader_block >= cfg->block_count || loop->iv < 0 ||
      loop->iv >= f->next_value ||
      !nyr_cfg_natural_loop_blocks(cfg, loop->latch_block, loop->header_block,
                                   in_loop, cfg->block_count))
    LAST_OVERWRITE_REJECT("unsupported-or-unknown-trip-count");

  /*
   * A known SCEV trip count is useful only when the final iteration and the
   * post-loop IV are representable in the language's wrapping i64 domain.
   */
  __int128 last_wide =
      (__int128)loop->init + (__int128)(loop->trip_count - 1u) * loop->step;
  __int128 exit_wide = last_wide + loop->step;
  if (last_wide < INT64_MIN || last_wide > INT64_MAX || exit_wide < INT64_MIN ||
      exit_wide > INT64_MAX)
    LAST_OVERWRITE_REJECT("final-induction-overflow");
  int64_t last = (int64_t)last_wide;

  /*
   * The SCEV loop must have one preheader and one backedge, and no early
   * exits. Otherwise the final body execution is not necessarily observable.
   */
  size_t header_backedges = 0;
  size_t header_exits = 0;
  for (size_t block = 0; block < cfg->block_count; ++block) {
    if (!in_loop[block])
      continue;
    for (size_t e = cfg->succ_offsets[block]; e < cfg->succ_offsets[block + 1];
         ++e) {
      size_t succ = cfg->succ_blocks[e];
      if (succ == loop->header_block) {
        if (block != loop->latch_block)
          LAST_OVERWRITE_REJECT("multiple-or-noncanonical-loop-edge");
        ++header_backedges;
      } else if (!in_loop[succ] && block != loop->header_block) {
        LAST_OVERWRITE_REJECT("early-exit-from-loop-body");
      } else if (block == loop->header_block && !in_loop[succ]) {
        ++header_exits;
      }
    }
    for (size_t i = cfg->block_start[block]; i < cfg->block_end[block]; ++i) {
      const nyr_inst_t *in = &f->data[i];
      if (!last_overwrite_pure_opcode(in->op))
        LAST_OVERWRITE_REJECT("body-contains-unsupported-opcode");
      unsigned effects = nyr_effective_effects(in);
      if (effects & ~(NYIR_EFFECT_READ_LOCAL | NYIR_EFFECT_CONTROL))
        LAST_OVERWRITE_REJECT("body-has-unmodeled-effects");
    }
  }
  if (header_backedges != 1 || header_exits != 1)
    LAST_OVERWRITE_REJECT("loop-needs-one-backedge-and-one-exit");

  /*
   * Collapse the one remaining trip as well as setting the final IV. The
   * canonical header has an explicit body branch and an explicit exit branch;
   * the loop exit must have no alternate predecessors or PHIs to repair.
   */
  size_t header_body_branch = SIZE_MAX;
  size_t header_exit_branch = SIZE_MAX;
  size_t header_exit_bridge = SIZE_MAX;
  size_t bridge_exit_branch = SIZE_MAX;
  size_t exit_block = SIZE_MAX;
  int64_t body_label = -1;
  int64_t exit_label = -1;
  for (size_t e = cfg->succ_offsets[loop->header_block];
       e < cfg->succ_offsets[loop->header_block + 1]; ++e) {
    size_t succ = cfg->succ_blocks[e];
    if (in_loop[succ]) {
      body_label = cfg->block_label[succ];
    } else if (cfg->block_label[succ] >= 0) {
      exit_label = cfg->block_label[succ];
      exit_block = succ;
    } else {
      header_exit_bridge = succ;
    }
  }
  if (exit_label < 0 && header_exit_bridge < cfg->block_count) {
    bool bridge_ok = true;
    for (size_t i = cfg->block_start[header_exit_bridge];
         i < cfg->block_end[header_exit_bridge]; ++i) {
      const nyr_inst_t *in = &f->data[i];
      if (in->op == NYIR_NOP)
        continue;
      if (in->op == NYIR_BR && bridge_exit_branch == SIZE_MAX) {
        bridge_exit_branch = i;
        exit_label = in->imm;
        continue;
      }
      bridge_ok = false;
      break;
    }
    if (!bridge_ok || bridge_exit_branch == SIZE_MAX ||
        cfg->succ_offsets[header_exit_bridge + 1] -
                cfg->succ_offsets[header_exit_bridge] !=
            1)
      LAST_OVERWRITE_REJECT("fallthrough-exit-bridge-not-trivial");
    for (size_t b = 0; b < cfg->block_count; ++b)
      if (cfg->block_label[b] == exit_label) {
        exit_block = b;
        break;
      }
  }
  if (body_label < 0 || exit_label < 0 || exit_block >= cfg->block_count) {
    ny_dbg_heuristic_event(
        "LOOP_LAST_OVERWRITE",
        "decision=reject header=L%lld reason=unlabeled-header-successor "
        "body=L%lld exit=L%lld",
        (long long)loop->header_label, (long long)body_label,
        (long long)exit_label);
    return false;
  }
  for (size_t i = cfg->block_start[loop->header_block];
       i < cfg->block_end[loop->header_block]; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op == NYIR_BR_IF && in->imm == body_label)
      header_body_branch = i;
    else if (in->op == NYIR_BR && in->imm == exit_label)
      header_exit_branch = i;
  }
  if (header_body_branch == SIZE_MAX ||
      (header_exit_bridge >= cfg->block_count &&
       header_exit_branch == SIZE_MAX)) {
    ny_dbg_heuristic_event(
        "LOOP_LAST_OVERWRITE",
        "decision=reject header=L%lld reason=header-branch-target-mismatch "
        "body=L%lld exit=L%lld body-branch=%zu exit-branch=%zu",
        (long long)loop->header_label, (long long)body_label,
        (long long)exit_label, header_body_branch, header_exit_branch);
    return false;
  }
  if (exit_block == SIZE_MAX)
    LAST_OVERWRITE_REJECT("header-exit-block-missing");
  size_t expected_exit_pred = header_exit_bridge < cfg->block_count
                                  ? header_exit_bridge
                                  : loop->header_block;
  if (cfg->pred_offsets[exit_block + 1] - cfg->pred_offsets[exit_block] != 1 ||
      cfg->pred_blocks[cfg->pred_offsets[exit_block]] != expected_exit_pred)
    LAST_OVERWRITE_REJECT("header-exit-has-alternate-predecessor");
  if (header_exit_bridge < cfg->block_count &&
      (cfg->pred_offsets[header_exit_bridge + 1] -
               cfg->pred_offsets[header_exit_bridge] !=
           1 ||
       cfg->pred_blocks[cfg->pred_offsets[header_exit_bridge]] !=
           loop->header_block))
    LAST_OVERWRITE_REJECT("fallthrough-exit-bridge-has-alternate-predecessor");
  for (size_t i = cfg->block_start[exit_block]; i < cfg->block_end[exit_block];
       ++i)
    if (f->data[i].op == NYIR_PHI)
      LAST_OVERWRITE_REJECT("exit-phi-needs-edge-repair");

  size_t latch_branch = SIZE_MAX;
  for (size_t i = cfg->block_start[loop->latch_block];
       i < cfg->block_end[loop->latch_block]; ++i)
    if (cfg->inst_block[i] == loop->latch_block && f->data[i].op == NYIR_BR &&
        f->data[i].imm == loop->header_label)
      latch_branch = i;
  if (latch_branch == SIZE_MAX)
    LAST_OVERWRITE_REJECT("latch-needs-direct-header-branch");

  int64_t preheader_label = cfg->block_label[loop->preheader_block];
  int64_t latch_label = cfg->block_label[loop->latch_block];
  bool *carried = calloc((size_t)f->next_value, sizeof(*carried));
  uint8_t *state = calloc((size_t)f->next_value, sizeof(*state));
  if (!carried || !state) {
    free(carried);
    free(state);
    ny_dbg_heuristic_event(
        "LOOP_LAST_OVERWRITE",
        "decision=reject header=L%lld reason=analysis-allocation-failed",
        (long long)loop->header_label);
    return false;
  }
  size_t iv_phi_index = SIZE_MAX;
  size_t iv_init_incoming_index = SIZE_MAX;
  size_t iv_backedge_incoming_index = SIZE_MAX;
  int iv_next = -1;
  int iv_init = -1;
  size_t non_iv_phis = 0;
  bool valid = true;
  for (size_t i = cfg->block_start[loop->header_block];
       i < cfg->block_end[loop->header_block]; ++i) {
    const nyr_inst_t *phi = &f->data[i];
    if (phi->op != NYIR_PHI)
      continue;
    int init = -1, next = -1;
    if (phi->dst < 0 || phi->dst >= f->next_value ||
        phi->phi_incoming_len != 2 ||
        !last_overwrite_phi_input(phi, preheader_label, &init) ||
        !last_overwrite_phi_input(phi, latch_label, &next)) {
      valid = false;
      break;
    }
    if (phi->dst == loop->iv) {
      iv_phi_index = i;
      iv_init = init;
      iv_next = next;
      for (size_t k = 0; k < phi->phi_incoming_len; ++k) {
        if (phi->phi_incoming[k].predecessor_label == preheader_label)
          iv_init_incoming_index = k;
        if (phi->phi_incoming[k].predecessor_label == latch_label)
          iv_backedge_incoming_index = k;
      }
      continue;
    }
    carried[phi->dst] = true;
    ++non_iv_phis;
  }
  if (!valid || iv_phi_index == SIZE_MAX ||
      iv_init_incoming_index == SIZE_MAX ||
      iv_backedge_incoming_index == SIZE_MAX || non_iv_phis == 0 ||
      iv_init < 0 || iv_next < 0) {
    free(carried);
    free(state);
    LAST_OVERWRITE_REJECT("header-phi-contract");
  }
  /*
   * Every carried result must be fully overwritten from the final IV and
   * invariant inputs. A reduction such as acc += iv fails this dependency
   * check and retains its original iteration count.
   */
  for (size_t i = cfg->block_start[loop->header_block];
       i < cfg->block_end[loop->header_block] && valid; ++i) {
    const nyr_inst_t *phi = &f->data[i];
    if (phi->op != NYIR_PHI || phi->dst == loop->iv)
      continue;
    int next = -1;
    valid = last_overwrite_phi_input(phi, latch_label, &next);
    memset(state, 0, (size_t)f->next_value * sizeof(*state));
    if (valid)
      valid = last_overwrite_value_depends_only_on_iv(
          f, defs, cfg, in_loop, carried, loop->iv, next, state);
  }
  /*
   * Branch choices may depend on the IV and invariant values, but not on a
   * previous iteration's carried state. The header's own trip guard is the
   * one exception.
   */
  for (size_t block = 0; block < cfg->block_count && valid; ++block) {
    if (!in_loop[block] || block == loop->header_block)
      continue;
    for (size_t i = cfg->block_start[block]; i < cfg->block_end[block]; ++i) {
      const nyr_inst_t *in = &f->data[i];
      if (in->op != NYIR_BR_IF)
        continue;
      memset(state, 0, (size_t)f->next_value * sizeof(*state));
      if (!last_overwrite_value_depends_only_on_iv(
              f, defs, cfg, in_loop, carried, loop->iv, in->a, state)) {
        valid = false;
        break;
      }
    }
  }
  /*
   * Validate every edge we will edit before changing any instruction. Keep
   * the rewrite atomic: a malformed or unexpectedly changed PHI must reject
   * the candidate while the original function is still intact.
   */
  for (size_t i = cfg->block_start[loop->header_block];
       i < cfg->block_end[loop->header_block] && valid; ++i) {
    const nyr_inst_t *phi = &f->data[i];
    if (phi->op != NYIR_PHI)
      continue;
    size_t init_index = SIZE_MAX, backedge_index = SIZE_MAX;
    int next_value = -1;
    for (size_t k = 0; k < phi->phi_incoming_len; ++k) {
      if (phi->phi_incoming[k].predecessor_label == preheader_label) {
        init_index = k;
      } else if (phi->phi_incoming[k].predecessor_label == latch_label) {
        backedge_index = k;
        next_value = phi->phi_incoming[k].value;
      }
    }
    valid = init_index != SIZE_MAX && backedge_index != SIZE_MAX &&
            next_value >= 0 && next_value < f->next_value;
  }
  size_t dead_header_compare = SIZE_MAX;
  nyr_use_def_t uses = {0};
  if (valid && nyr_build_use_def(f, &uses)) {
    int condition = f->data[header_body_branch].a;
    if (condition >= 0 && (size_t)condition < uses.value_count &&
        defs[condition] >= 0) {
      size_t begin = uses.offsets[condition];
      size_t end = uses.offsets[condition + 1];
      const nyr_inst_t *cmp = &f->data[defs[condition]];
      if (end - begin == 1 && uses.users[begin] == header_body_branch &&
          (cmp->op == NYIR_CMP_I64 || cmp->op == NYIR_CMP_F64 ||
           cmp->op == NYIR_CMP_F32) &&
          nyr_inst_effects(cmp) == NYIR_EFFECT_NONE)
        dead_header_compare = (size_t)defs[condition];
    }
  }
  nyr_use_def_free(&uses);
  free(carried);
  free(state);
  if (!valid)
    LAST_OVERWRITE_REJECT("carried-value-or-branch-depends-on-prior-state");

  /*
   * Insert a fresh final-IV constant immediately before the preheader branch
   * so it dominates the header and does not mutate a potentially shared
   * initializer constant.
   */
  size_t insertion = SIZE_MAX;
  for (size_t i = 0; i < f->len; ++i) {
    size_t block = cfg->inst_block[i];
    if (block < cfg->block_count && block != loop->latch_block &&
        f->data[i].op == NYIR_BR && f->data[i].imm == loop->header_label) {
      insertion = i;
      break;
    }
  }
  if (insertion == SIZE_MAX) {
    /*
     * A preheader may reach the header by fallthrough. Put the constant
     * before the header label so it executes on that edge and dominates the
     * header's PHIs.
     */
    for (size_t i = 0; i < f->len; ++i) {
      if (f->data[i].op == NYIR_LABEL && f->data[i].imm == loop->header_label) {
        insertion = i;
        break;
      }
    }
  }
  if (insertion == SIZE_MAX || !nir_ensure_inst_space(f, 1))
    LAST_OVERWRITE_REJECT("preheader-not-rewritable");

  const nyr_inst_t *iv_phi = &f->data[iv_phi_index];
  int fresh = f->next_value++;
  nyr_inst_t init = {
      .op = NYIR_CONST_I64,
      .dst = fresh,
      .a = -1,
      .b = -1,
      .c = -1,
      .d = -1,
      .e = -1,
      .f = -1,
      .imm = last,
      .effects = NYIR_EFFECT_NONE,
      .scalar_class = iv_phi->scalar_class,
      .semantic_ownership = iv_phi->semantic_ownership,
      .semantic_mutable = iv_phi->semantic_mutable,
      .range = {.has_min = true, .has_max = true, .min = last, .max = last}};
  if (defs[iv_init] >= 0)
    init.debug = f->data[defs[iv_init]].debug;

  /*
   * Replace the header test with a direct entry into the body, route the
   * single final body execution to the original exit, and remove the now-dead
   * backedge from every header PHI. Outside users observe the exact values
   * produced by that final execution (including the post-loop IV).
   */
  nyr_inst_t *body_branch = &f->data[header_body_branch];
  body_branch->op = NYIR_BR;
  body_branch->a = body_branch->b = body_branch->c = -1;
  body_branch->d = body_branch->e = body_branch->f = -1;
  body_branch->imm = body_label;
  if (dead_header_compare < f->len)
    nyr_inst_discard(&f->data[dead_header_compare]);
  if (header_exit_bridge < cfg->block_count) {
    /*
     * The conditional's false edge is a compiler-generated fallthrough
     * bridge. The header now jumps directly to the body, so keep the dead
     * bridge from contributing another static edge into the loop exit.
     */
    f->data[bridge_exit_branch].imm = body_label;
  } else {
    nyr_inst_discard(&f->data[header_exit_branch]);
  }
  f->data[latch_branch].imm = exit_label;
  for (size_t i = cfg->block_start[loop->header_block];
       i < cfg->block_end[loop->header_block]; ++i) {
    nyr_inst_t *phi = &f->data[i];
    if (phi->op != NYIR_PHI)
      continue;
    size_t init_index = SIZE_MAX, backedge_index = SIZE_MAX;
    int next_value = -1;
    for (size_t k = 0; k < phi->phi_incoming_len; ++k) {
      if (phi->phi_incoming[k].predecessor_label == preheader_label) {
        init_index = k;
      } else if (phi->phi_incoming[k].predecessor_label == latch_label) {
        backedge_index = k;
        next_value = phi->phi_incoming[k].value;
      }
    }
    /*
     * All edges were checked before mutation, so these indices and values
     * remain valid until this loop finishes.
     */
    assert(init_index != SIZE_MAX && backedge_index != SIZE_MAX &&
           next_value >= 0 && next_value < f->next_value);
    if (phi->dst == loop->iv)
      phi->phi_incoming[init_index].value = fresh;
    else
      last_overwrite_replace_outside_uses(f, cfg, in_loop, phi->dst,
                                          next_value);
    memmove(&phi->phi_incoming[backedge_index],
            &phi->phi_incoming[backedge_index + 1],
            (phi->phi_incoming_len - backedge_index - 1) *
                sizeof(*phi->phi_incoming));
    --phi->phi_incoming_len;
    if (phi->dst == loop->iv)
      last_overwrite_replace_outside_uses(f, cfg, in_loop, phi->dst, iv_next);
  }
  memmove(&f->data[insertion + 1], &f->data[insertion],
          (f->len - insertion) * sizeof(*f->data));
  f->data[insertion] = init;
  ++f->len;
  ny_dbg_heuristic_event("LOOP_LAST_OVERWRITE",
                         "decision=apply header=L%lld final-iv=%lld "
                         "carried-phis=%zu body=%lld exit=%lld",
                         (long long)loop->header_label, (long long)last,
                         non_iv_phis, (long long)body_label,
                         (long long)exit_label);
#undef LAST_OVERWRITE_REJECT
  return true;
}

bool nyr_loop_last_overwrite(nyr_func_t *f) {
  if (!f || f->len < 8 || f->next_value <= 0)
    return true;
  nyr_cfg_t cfg = {0};
  nyr_scev_info_t scev = {0};
  int *defs = nyr_build_defs(f);
  if (!defs || !nyr_cfg_build(f, &cfg) || !nyr_scev_analyze(f, &scev)) {
    free(defs);
    nyr_cfg_free(&cfg);
    nyr_scev_free(&scev);
    return false;
  }
  bool *in_loop = calloc(cfg.block_count, sizeof(*in_loop));
  if (!in_loop) {
    free(defs);
    nyr_cfg_free(&cfg);
    nyr_scev_free(&scev);
    return false;
  }
  bool ok = true;
  for (size_t i = 0; i < scev.count; ++i) {
    if (last_overwrite_try_loop(f, &cfg, defs, &scev.loops[i], in_loop))
      break; /* CFG and definition indices are stale after insertion. */
  }
  free(in_loop);
  free(defs);
  nyr_cfg_free(&cfg);
  nyr_scev_free(&scev);
  return ok;
}

/*
 * Loop unswitching: duplicates loop bodies when a loop-invariant
 * conditional branch can be evaluated once outside the loop.
 */

/*
 * Clone owned per-instruction buffers; symbol and source-file strings remain
 * owned by the containing function and can be shared by its instructions.
 */
static bool unswitch_copy_inst(const nyr_inst_t *src, nyr_inst_t *dst) {
  *dst = *src;
  dst->extra_args = NULL;
  dst->arg_sizes = NULL;
  dst->phi_incoming = NULL;
  if (src->extra_args_len) {
    if (!src->extra_args ||
        src->extra_args_len > SIZE_MAX / sizeof(*dst->extra_args))
      return false;
    dst->extra_args = malloc(src->extra_args_len * sizeof(*dst->extra_args));
    if (!dst->extra_args)
      return false;
    memcpy(dst->extra_args, src->extra_args,
           src->extra_args_len * sizeof(*dst->extra_args));
  }
  if (src->arg_sizes && src->imm > 0) {
    if ((uint64_t)src->imm > SIZE_MAX / sizeof(*dst->arg_sizes))
      goto fail;
    dst->arg_sizes = malloc((size_t)src->imm * sizeof(*dst->arg_sizes));
    if (!dst->arg_sizes)
      goto fail;
    memcpy(dst->arg_sizes, src->arg_sizes,
           (size_t)src->imm * sizeof(*dst->arg_sizes));
  }
  if (src->phi_incoming_len) {
    if (!src->phi_incoming ||
        src->phi_incoming_len > SIZE_MAX / sizeof(*dst->phi_incoming))
      goto fail;
    dst->phi_incoming =
        malloc(src->phi_incoming_len * sizeof(*dst->phi_incoming));
    if (!dst->phi_incoming)
      goto fail;
    memcpy(dst->phi_incoming, src->phi_incoming,
           src->phi_incoming_len * sizeof(*dst->phi_incoming));
  }
  return true;

fail:
  nyr_inst_discard(dst);
  return false;
}

static size_t unswitch_block_for_label(const nyr_cfg_t *cfg, int64_t label) {
  for (size_t b = 0; b < cfg->block_count; ++b)
    if (cfg->block_label[b] == label)
      return b;
  return SIZE_MAX;
}

static size_t unswitch_resolve_exit(const nyr_func_t *f, const nyr_cfg_t *cfg,
                                    size_t block) {
  for (size_t hops = 0; hops < cfg->block_count; ++hops) {
    if (block >= cfg->block_count)
      return SIZE_MAX;
    if (cfg->block_label[block] >= 0)
      return block;
    size_t branch = SIZE_MAX;
    for (size_t i = cfg->block_start[block]; i < cfg->block_end[block]; ++i) {
      if (f->data[i].op == NYIR_NOP)
        continue;
      if (f->data[i].op != NYIR_BR || branch != SIZE_MAX)
        return SIZE_MAX;
      branch = i;
    }
    if (branch == SIZE_MAX)
      return SIZE_MAX;
    block = unswitch_block_for_label(cfg, f->data[branch].imm);
  }
  return SIZE_MAX;
}

static bool unswitch_control_inst_is_pure(const nyr_inst_t *in) {
  if (!in ||
      (nyr_effective_effects(in) &
       (NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_CALL |
        NYIR_EFFECT_READ_LOCAL | NYIR_EFFECT_WRITE_LOCAL |
        NYIR_EFFECT_MAY_TRAP | NYIR_EFFECT_ALLOCATION | NYIR_EFFECT_VOLATILE |
        NYIR_EFFECT_UNKNOWN_SIDE_EFFECT | NYIR_EFFECT_IO | NYIR_EFFECT_THREAD |
        NYIR_EFFECT_FFI | NYIR_EFFECT_FENV)) != 0)
    return false;
  switch (in->op) {
  case NYIR_NOP:
  case NYIR_LABEL:
  case NYIR_PHI:
  case NYIR_CONST_I64:
  case NYIR_CONST_F64:
  case NYIR_COPY:
  case NYIR_ADD_I64:
  case NYIR_SUB_I64:
  case NYIR_CMP_I64:
  case NYIR_BR:
  case NYIR_BR_IF:
    return true;
  default:
    return false;
  }
}

static bool unswitch_header_is_pure_control(const nyr_func_t *f,
                                            const nyr_cfg_t *cfg,
                                            size_t header) {
  for (size_t i = cfg->block_start[header]; i < cfg->block_end[header]; ++i)
    if (!unswitch_control_inst_is_pure(&f->data[i]))
      return false;
  return true;
}

static int unswitch_value(const int *map, size_t count, int value) {
  return value >= 0 && (size_t)value < count && map[value] >= 0 ? map[value]
                                                                : value;
}

static bool unswitch_iv_update_matches(const nyr_func_t *f, const int *defs,
                                       int iv, int value, int64_t step) {
  value = interchange_root_copy(f, defs, value);
  if (value < 0 || value >= f->next_value || defs[value] < 0)
    return false;
  const nyr_inst_t *update = &f->data[(size_t)defs[value]];
  int lhs = interchange_root_copy(f, defs, update->a);
  int rhs = interchange_root_copy(f, defs, update->b);
  int step_value = -1;
  bool negate_step = false;
  if (update->op == NYIR_ADD_I64) {
    if (lhs == iv)
      step_value = rhs;
    else if (rhs == iv)
      step_value = lhs;
  } else if (update->op == NYIR_SUB_I64 && lhs == iv) {
    step_value = rhs;
    negate_step = true;
  } else {
    return false;
  }
  if (step_value < 0 || step_value >= f->next_value || defs[step_value] < 0)
    return false;
  const nyr_inst_t *constant = &f->data[(size_t)defs[step_value]];
  if (constant->op != NYIR_CONST_I64 ||
      (negate_step && constant->imm == INT64_MIN))
    return false;
  int64_t actual_step = negate_step ? -constant->imm : constant->imm;
  return actual_step == step;
}

/*
 * Clone a natural loop with one preheader and direct latches sharing the
 * identified induction step. The preheader dispatches to the
 * true-specialized original or a false-specialized copy. Each exit retains
 * its own continuation and remapped exit-PHI edges.
 */
static bool unswitch_try_one(nyr_func_t *f, const nyr_cfg_t *cfg,
                             const nyr_scev_loop_t *loop) {
  if (!f || !cfg || !loop || loop->iv < 0 ||
      loop->header_block >= cfg->block_count ||
      loop->latch_block >= cfg->block_count ||
      loop->preheader_block >= cfg->block_count)
    return false;

  bool changed = false;
  const char *stage = "natural-loop";
  size_t rejected_block = SIZE_MAX;
  bool *members = calloc(cfg->block_count, sizeof(*members));
  bool *latches = calloc(cfg->block_count, sizeof(*latches));
  bool *latch_members = calloc(cfg->block_count, sizeof(*latch_members));
  bool *fallthrough_exits =
      calloc(cfg->block_count, sizeof(*fallthrough_exits));
  bool *exit_blocks = calloc(cfg->block_count, sizeof(*exit_blocks));
  size_t *fallthrough_target =
      calloc(cfg->block_count, sizeof(*fallthrough_target));
  int *defs = nyr_build_defs(f);
  int *value_map = NULL;
  int64_t *label_map = NULL;
  int64_t *exit_label_map = NULL;
  size_t *cloned_at = NULL;
  nyr_inst_t *clones = NULL;
  nyr_inst_t *rebuilt = NULL;
  size_t clone_count = 0, clone_built = 0;
  size_t extra_exit_inst_count = 0;
  size_t branch_index = SIZE_MAX, false_branch_index = SIZE_MAX;
  size_t preheader_term = SIZE_MAX;
  if (!members || !latches || !latch_members || !fallthrough_exits ||
      !exit_blocks || !fallthrough_target || !defs)
    goto done;

  stage = "header-shape";
  const size_t header = loop->header_block;
  const size_t preheader = loop->preheader_block;
  const int64_t header_label = cfg->block_label[header];
  if (header_label < 0 || members[preheader] ||
      cfg->succ_offsets[preheader + 1] - cfg->succ_offsets[preheader] != 1 ||
      cfg->succ_blocks[cfg->succ_offsets[preheader]] != header)
    goto done;

  bool have_preheader_pred = false;
  size_t latch_count = 0;
  for (size_t e = cfg->pred_offsets[header]; e < cfg->pred_offsets[header + 1];
       ++e) {
    size_t pred = cfg->pred_blocks[e];
    if (pred == preheader) {
      have_preheader_pred = true;
      continue;
    }
    memset(latch_members, 0, cfg->block_count * sizeof(*latch_members));
    if (!nyr_cfg_is_backedge(cfg, pred, header) ||
        !nyr_cfg_natural_loop_blocks(cfg, pred, header, latch_members,
                                     cfg->block_count))
      goto done;
    latches[pred] = true;
    ++latch_count;
    for (size_t b = 0; b < cfg->block_count; ++b)
      members[b] |= latch_members[b];
  }
  if (!have_preheader_pred || latch_count == 0 ||
      latch_count + 1 !=
          cfg->pred_offsets[header + 1] - cfg->pred_offsets[header] ||
      !latches[loop->latch_block] || !members[header])
    goto done;

  bool found_iv_phi = false;
  const nyr_inst_t *iv_phi = NULL;
  for (size_t i = cfg->block_start[header]; i < cfg->block_end[header]; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op != NYIR_PHI)
      continue;
    if (in->dst == loop->iv) {
      if (found_iv_phi)
        goto done;
      found_iv_phi = true;
      iv_phi = in;
    }
    if (in->dst < 0)
      goto done;
  }
  if (!found_iv_phi || !iv_phi || iv_phi->phi_incoming_len != latch_count + 1)
    goto done;
  bool found_init = false;
  size_t found_latches = 0;
  for (size_t k = 0; k < iv_phi->phi_incoming_len; ++k) {
    int value = iv_phi->phi_incoming[k].value;
    size_t pred = unswitch_block_for_label(
        cfg, iv_phi->phi_incoming[k].predecessor_label);
    if (pred == preheader) {
      if (found_init || interchange_root_copy(f, defs, value) !=
                            interchange_root_copy(f, defs, loop->init_value))
        goto done;
      found_init = true;
    } else if (pred < cfg->block_count && latches[pred]) {
      if (!unswitch_iv_update_matches(f, defs, loop->iv, value, loop->step))
        goto done;
      ++found_latches;
    } else {
      goto done;
    }
  }
  if (!found_init || found_latches != latch_count)
    goto done;
  for (size_t latch = 0; latch < cfg->block_count; ++latch) {
    if (!latches[latch])
      continue;
    if (cfg->block_label[latch] < 0 || !members[latch])
      goto done;
    size_t end = cfg->block_end[latch];
    while (end > cfg->block_start[latch] && f->data[end - 1].op == NYIR_NOP)
      --end;
    if (end == cfg->block_start[latch] || f->data[end - 1].op != NYIR_BR ||
        f->data[end - 1].imm != header_label)
      goto done;
  }

  /*
   * The preheader must end in its direct edge to the original header.
   */
  stage = "preheader";
  for (size_t i = cfg->block_start[preheader]; i < cfg->block_end[preheader];
       ++i) {
    if (f->data[i].op == NYIR_BR && f->data[i].imm == header_label)
      preheader_term = i;
  }
  if (preheader_term == SIZE_MAX)
    goto done;
  size_t last_preheader_inst = cfg->block_end[preheader];
  while (last_preheader_inst > cfg->block_start[preheader] &&
         f->data[last_preheader_inst - 1].op == NYIR_NOP)
    --last_preheader_inst;
  if (last_preheader_inst == 0 || last_preheader_inst - 1 != preheader_term)
    goto done;

  stage = "loop-exits";
  size_t loop_inst_count = 0;
  for (size_t b = 0; b < cfg->block_count; ++b) {
    if (!members[b])
      continue;
    stage = "loop-block-label";
    rejected_block = b;
    if (((b == header || latches[b]) && cfg->block_label[b] < 0) ||
        cfg->block_end[b] - cfg->block_start[b] > SIZE_MAX - loop_inst_count)
      goto done;
    loop_inst_count += cfg->block_end[b] - cfg->block_start[b];
    for (size_t e = cfg->succ_offsets[b]; e < cfg->succ_offsets[b + 1]; ++e) {
      size_t succ = cfg->succ_blocks[e];
      if (members[succ])
        continue;
      stage = "exit-target";
      size_t resolved = unswitch_resolve_exit(f, cfg, succ);
      if (resolved >= cfg->block_count || members[resolved])
        goto done;
      exit_blocks[resolved] = true;
    }
  }
  stage = "exit-validation";
  if (loop_inst_count == 0) {
    stage = "empty-loop";
    goto done;
  }
  if (loop_inst_count > f->len) {
    stage = "loop-size";
    goto done;
  }
  bool have_exit = false;
  for (size_t b = 0; b < cfg->block_count; ++b) {
    if (!exit_blocks[b])
      continue;
    have_exit = true;
    if (cfg->block_label[b] < 0) {
      stage = "unlabeled-exit";
      rejected_block = b;
      goto done;
    }
    for (size_t i = cfg->block_start[b]; i < cfg->block_end[b]; ++i) {
      const nyr_inst_t *in = &f->data[i];
      if (in->op != NYIR_PHI)
        continue;
      for (size_t k = 0; k < in->phi_incoming_len; ++k) {
        size_t pred = unswitch_block_for_label(
            cfg, in->phi_incoming[k].predecessor_label);
        if (pred >= cfg->block_count)
          goto done;
      }
    }
  }
  if (!have_exit) {
    stage = "missing-exit";
    goto done;
  }

  /*
   * Internal fallthrough is preserved by source order. Redirect external
   * fallthrough to its resolved exit after cloning; the original may use an
   * unlabeled branch trampoline that is not part of the loop.
   */
  stage = "fallthrough";
  for (size_t b = 0; b < cfg->block_count; ++b) {
    if (!members[b])
      continue;
    size_t end = cfg->block_end[b];
    while (end > cfg->block_start[b] && f->data[end - 1].op == NYIR_NOP)
      --end;
    if (end == cfg->block_start[b])
      goto done;
    nyr_op_t terminal = f->data[end - 1].op;
    if (terminal == NYIR_RET)
      goto done;
    if (terminal == NYIR_BR)
      continue;
    if (b + 1 < cfg->block_count && members[b + 1])
      continue;
    if (b + 1 >= cfg->block_count)
      goto done;
    size_t resolved_exit = unswitch_resolve_exit(f, cfg, b + 1);
    if (resolved_exit >= cfg->block_count || !exit_blocks[resolved_exit])
      goto done;
    fallthrough_exits[b] = true;
    fallthrough_target[b] = resolved_exit;
  }
  size_t extra_exit_branches = 0;
  for (size_t b = 0; b < cfg->block_count; ++b)
    if (fallthrough_exits[b])
      ++extra_exit_branches;
  if (extra_exit_branches > SIZE_MAX - loop_inst_count)
    goto done;
  if (extra_exit_branches > SIZE_MAX / 2u)
    goto done;
  extra_exit_inst_count = extra_exit_branches * 2u;
  if (extra_exit_inst_count > SIZE_MAX - loop_inst_count)
    goto done;

  if ((size_t)f->next_value > SIZE_MAX / sizeof(*value_map))
    goto done;
  if (cfg->block_count > SIZE_MAX / sizeof(*label_map) ||
      f->len > SIZE_MAX / sizeof(*cloned_at))
    goto done;
  size_t value_count = (size_t)f->next_value;
  value_map = malloc(value_count * sizeof(*value_map));
  label_map = malloc(cfg->block_count * sizeof(*label_map));
  exit_label_map = malloc(cfg->block_count * sizeof(*exit_label_map));
  cloned_at = malloc(f->len * sizeof(*cloned_at));
  if (!value_map || !label_map || !exit_label_map || !cloned_at)
    goto done;
  for (size_t v = 0; v < value_count; ++v)
    value_map[v] = -1;
  for (size_t b = 0; b < cfg->block_count; ++b)
    label_map[b] = -1;
  for (size_t b = 0; b < cfg->block_count; ++b)
    exit_label_map[b] = -1;
  for (size_t i = 0; i < f->len; ++i)
    cloned_at[i] = SIZE_MAX;

  stage = "clone-maps";
  int64_t max_label = -1;
  for (size_t b = 0; b < cfg->block_count; ++b)
    if (cfg->block_label[b] > max_label)
      max_label = cfg->block_label[b];
  size_t value_defs = 0;
  for (size_t b = 0; b < cfg->block_count; ++b) {
    if (!members[b])
      continue;
    if (cfg->block_label[b] >= 0) {
      if (max_label > INT64_MAX - 1)
        goto done;
      label_map[b] = ++max_label;
    }
    for (size_t i = cfg->block_start[b]; i < cfg->block_end[b]; ++i) {
      int dst = f->data[i].dst;
      if (dst < 0)
        continue;
      if ((size_t)dst >= value_count || value_map[dst] >= 0 ||
          value_defs == SIZE_MAX)
        goto done;
      ++value_defs;
    }
  }
  for (size_t b = 0; b < cfg->block_count; ++b) {
    if (!fallthrough_exits[b])
      continue;
    if (max_label > INT64_MAX - 1)
      goto done;
    exit_label_map[b] = ++max_label;
  }
  if (value_defs > (size_t)(INT_MAX - f->next_value))
    goto done;
  int next_value = f->next_value;
  for (size_t b = 0; b < cfg->block_count; ++b) {
    if (!members[b])
      continue;
    for (size_t i = cfg->block_start[b]; i < cfg->block_end[b]; ++i)
      if (f->data[i].dst >= 0)
        value_map[f->data[i].dst] = next_value++;
  }

  /*
   * Loop-defined values may escape only through incoming values on an exit
   * PHI; those edges are duplicated below with cloned SSA values.
   */
  stage = "escaping-values";
  for (size_t b = 0; b < cfg->block_count; ++b) {
    if (members[b])
      continue;
    for (size_t i = cfg->block_start[b]; i < cfg->block_end[b]; ++i) {
      const nyr_inst_t *in = &f->data[i];
      int uses[] = {in->a, in->b, in->c, in->d, in->e, in->f};
      for (size_t k = 0; k < sizeof(uses) / sizeof(uses[0]); ++k)
        if (uses[k] >= 0 && (size_t)uses[k] < value_count &&
            value_map[uses[k]] >= 0)
          goto done;
      for (size_t k = 0; k < in->extra_args_len; ++k)
        if (in->extra_args[k] >= 0 && (size_t)in->extra_args[k] < value_count &&
            value_map[in->extra_args[k]] >= 0)
          goto done;
      for (size_t k = 0; k < in->phi_incoming_len; ++k)
        if (in->phi_incoming[k].value >= 0 &&
            (size_t)in->phi_incoming[k].value < value_count &&
            value_map[in->phi_incoming[k].value] >= 0) {
          size_t pred = unswitch_block_for_label(
              cfg, in->phi_incoming[k].predecessor_label);
          if (!exit_blocks[b] || in->op != NYIR_PHI ||
              pred >= cfg->block_count || !members[pred])
            goto done;
        }
    }
  }

  /*
   * Prefer invariant conditionals whose two edges stay in the loop. The
   * bounded one-sided case below additionally specializes a proven-positive,
   * pure-control loop header when the opposite edge resolves to an exit.
   */
  stage = "invariant-branch";
  size_t branch_block = SIZE_MAX, false_block = SIZE_MAX;
  size_t one_sided_exit = SIZE_MAX, one_sided_internal = SIZE_MAX;
  bool one_sided_exit_on_true = false;
  int condition = -1;
  for (size_t b = 0; b < cfg->block_count && branch_index == SIZE_MAX; ++b) {
    if (!members[b])
      continue;
    for (size_t i = cfg->block_start[b]; i < cfg->block_end[b]; ++i) {
      const nyr_inst_t *in = &f->data[i];
      if (in->op != NYIR_BR_IF || in->a < 0 || (size_t)in->a >= value_count ||
          value_map[in->a] >= 0)
        continue;
      size_t true_block = unswitch_block_for_label(cfg, in->imm);
      if (true_block >= cfg->block_count)
        continue;
      size_t other = SIZE_MAX;
      for (size_t e = cfg->succ_offsets[b]; e < cfg->succ_offsets[b + 1]; ++e) {
        size_t succ = cfg->succ_blocks[e];
        if (succ != true_block) {
          if (other != SIZE_MAX)
            other = SIZE_MAX;
          else
            other = succ;
        }
      }
      if (other >= cfg->block_count || other == true_block)
        continue;
      size_t next = i + 1;
      while (next < cfg->block_end[b] && f->data[next].op == NYIR_NOP)
        ++next;
      size_t candidate_false = SIZE_MAX;
      size_t candidate_false_branch = SIZE_MAX;
      if (next < cfg->block_end[b]) {
        if (f->data[next].op != NYIR_BR)
          continue;
        candidate_false = unswitch_block_for_label(cfg, f->data[next].imm);
        if (candidate_false != other)
          continue;
        candidate_false_branch = next;
      } else {
        if (b + 1 != other)
          continue;
      }
      bool true_inside = members[true_block];
      bool false_inside = members[other];
      if (true_inside != false_inside) {
        size_t exit_target = true_inside
                                 ? unswitch_resolve_exit(f, cfg, other)
                                 : unswitch_resolve_exit(f, cfg, true_block);
        size_t internal_target = true_inside ? true_block : other;
        if (exit_target >= cfg->block_count || !exit_blocks[exit_target])
          continue;
        branch_index = i;
        branch_block = b;
        false_block = candidate_false;
        false_branch_index = candidate_false_branch;
        condition = in->a;
        one_sided_exit = exit_target;
        one_sided_internal = internal_target;
        one_sided_exit_on_true = !true_inside;
        break;
      }
      if (!true_inside)
        continue;
      false_block = candidate_false;
      false_branch_index = candidate_false_branch;
      branch_index = i;
      branch_block = b;
      condition = in->a;
      break;
    }
  }
  if (branch_index == SIZE_MAX || condition < 0 || branch_block == SIZE_MAX)
    goto done;

  if (one_sided_exit < cfg->block_count) {
    /*
     * Moving an early exit to the preheader skips the loop header. Require
     * SCEV to prove at least one iteration and the header to be pure control;
     * the non-exiting dispatch still targets the header so its PHIs execute.
     */
    bool header_body_edge = false;
    for (size_t e = cfg->succ_offsets[header];
         e < cfg->succ_offsets[header + 1]; ++e)
      if (cfg->succ_blocks[e] == branch_block)
        header_body_edge = true;
    int64_t exit_label = cfg->block_label[one_sided_exit];
    int64_t internal_label = cfg->block_label[one_sided_internal];
    int64_t preheader_label = cfg->block_label[header];
    bool pure_prefix = true;
    for (size_t i = cfg->block_start[branch_block]; i < branch_index; ++i)
      if (!unswitch_control_inst_is_pure(&f->data[i]) ||
          f->data[i].op == NYIR_BR || f->data[i].op == NYIR_BR_IF)
        pure_prefix = false;
    if (!loop->trip_count_known || loop->trip_count == 0 || !header_body_edge ||
        !unswitch_header_is_pure_control(f, cfg, header) || !pure_prefix ||
        exit_label < 0 || internal_label < 0 || preheader_label < 0 ||
        preheader_term >= f->len || false_branch_index == preheader_term)
      goto done;
    stage = "one-sided-rebuild";
    if (f->len == SIZE_MAX || f->len + 1 > SIZE_MAX / sizeof(*rebuilt))
      goto done;
    size_t new_len = f->len + 1;
    rebuilt = calloc(new_len, sizeof(*rebuilt));
    if (!rebuilt)
      goto done;
    size_t out = 0;
    for (size_t i = 0; i < f->len; ++i) {
      nyr_inst_t moved = f->data[i];
      if (i == preheader_term) {
        nyr_inst_t dispatch = moved;
        dispatch.op = NYIR_BR_IF;
        dispatch.dst = dispatch.b = dispatch.c = dispatch.d = dispatch.e =
            dispatch.f = -1;
        dispatch.a = condition;
        dispatch.imm = one_sided_exit_on_true ? exit_label : preheader_label;
        rebuilt[out++] = dispatch;
        rebuilt[out++] = (nyr_inst_t){
            .op = NYIR_BR,
            .dst = -1,
            .a = -1,
            .b = -1,
            .c = -1,
            .d = -1,
            .e = -1,
            .f = -1,
            .effects = NYIR_EFFECT_CONTROL,
            .imm = one_sided_exit_on_true ? preheader_label : exit_label};
        continue;
      }
      if (i == branch_index) {
        moved.op = NYIR_BR;
        moved.dst = moved.a = moved.b = moved.c = moved.d = moved.e = moved.f =
            -1;
        moved.imm = internal_label;
      } else if (i == false_branch_index) {
        nyr_inst_discard(&moved);
      }
      rebuilt[out++] = moved;
    }
    if (out != new_len)
      goto done;
    free(f->data);
    f->data = rebuilt;
    f->len = new_len;
    f->cap = new_len;
    rebuilt = NULL;
    changed = true;
    goto done;
  }
  if (false_block < cfg->block_count && exit_blocks[false_block])
    goto done;

  stage = "clone-instructions";
  clones = calloc(loop_inst_count + extra_exit_inst_count, sizeof(*clones));
  if (!clones)
    goto done;
  for (size_t i = 0; i < f->len; ++i) {
    size_t b = cfg->inst_block[i];
    if (!members[b])
      continue;
    nyr_inst_t *copy = &clones[clone_built];
    if (!unswitch_copy_inst(&f->data[i], copy))
      goto done;
    cloned_at[i] = clone_built++;
    if (copy->dst >= 0)
      copy->dst = value_map[copy->dst];
    int *ops[] = {&copy->a, &copy->b, &copy->c, &copy->d, &copy->e, &copy->f};
    for (size_t k = 0; k < sizeof(ops) / sizeof(ops[0]); ++k)
      *ops[k] = unswitch_value(value_map, value_count, *ops[k]);
    for (size_t k = 0; k < copy->extra_args_len; ++k)
      copy->extra_args[k] =
          unswitch_value(value_map, value_count, copy->extra_args[k]);
    if (copy->op == NYIR_LABEL) {
      if (label_map[b] < 0)
        goto done;
      copy->imm = label_map[b];
    } else if (copy->op == NYIR_BR || copy->op == NYIR_BR_IF) {
      size_t target = unswitch_block_for_label(cfg, copy->imm);
      size_t resolved_target = target < cfg->block_count
                                   ? unswitch_resolve_exit(f, cfg, target)
                                   : SIZE_MAX;
      if (target >= cfg->block_count ||
          (!members[target] && (resolved_target >= cfg->block_count ||
                                !exit_blocks[resolved_target])))
        goto done;
      if (members[target]) {
        if (label_map[target] < 0)
          goto done;
        copy->imm = label_map[target];
      }
    }
    for (size_t k = 0; k < copy->phi_incoming_len; ++k) {
      nyr_phi_incoming_t *inc = &copy->phi_incoming[k];
      size_t pred = unswitch_block_for_label(cfg, inc->predecessor_label);
      if (pred >= cfg->block_count || (!members[pred] && pred != preheader))
        goto done;
      if (members[pred]) {
        if (label_map[pred] < 0)
          goto done;
        inc->predecessor_label = label_map[pred];
      }
      inc->value = unswitch_value(value_map, value_count, inc->value);
    }
    if (fallthrough_exits[b] && i + 1 == cfg->block_end[b]) {
      clones[clone_built++] = (nyr_inst_t){.op = NYIR_LABEL,
                                           .dst = -1,
                                           .a = -1,
                                           .b = -1,
                                           .c = -1,
                                           .d = -1,
                                           .e = -1,
                                           .f = -1,
                                           .imm = exit_label_map[b]};
      clones[clone_built++] =
          (nyr_inst_t){.op = NYIR_BR,
                       .dst = -1,
                       .a = -1,
                       .b = -1,
                       .c = -1,
                       .d = -1,
                       .e = -1,
                       .f = -1,
                       .effects = NYIR_EFFECT_CONTROL,
                       .imm = cfg->block_label[fallthrough_target[b]]};
    }
  }
  clone_count = clone_built;
  if (clone_count != loop_inst_count + extra_exit_inst_count ||
      cloned_at[branch_index] == SIZE_MAX)
    goto done;

  /*
   * The original specializes to the true edge. The clone keeps the false
   * edge by replacing only its invariant conditional with a NOP.
   */
  nyr_inst_t *original_branch = &f->data[branch_index];
  if (original_branch->op != NYIR_BR_IF)
    goto done;
  original_branch = &clones[cloned_at[branch_index]];
  nyr_inst_discard(original_branch);

  /*
   * A cloned exit adds one predecessor per original loop-exit edge. Duplicate
   * those exit-PHI inputs with the cloned predecessor label and cloned SSA
   * value; external predecessor inputs remain unchanged.
   */
  stage = "exit-phi-remap";
  for (size_t b = 0; b < cfg->block_count; ++b) {
    if (!exit_blocks[b])
      continue;
    for (size_t i = cfg->block_start[b]; i < cfg->block_end[b]; ++i) {
      nyr_inst_t *phi = &f->data[i];
      if (phi->op != NYIR_PHI)
        continue;
      size_t extra = 0;
      for (size_t k = 0; k < phi->phi_incoming_len; ++k) {
        size_t pred = unswitch_block_for_label(
            cfg, phi->phi_incoming[k].predecessor_label);
        if (pred < cfg->block_count && members[pred])
          ++extra;
      }
      if (extra == 0)
        continue;
      if (extra > SIZE_MAX - phi->phi_incoming_len ||
          phi->phi_incoming_len + extra > SIZE_MAX / sizeof(*phi->phi_incoming))
        goto done;
      size_t new_len = phi->phi_incoming_len + extra;
      nyr_phi_incoming_t *incoming =
          realloc(phi->phi_incoming, new_len * sizeof(*incoming));
      if (!incoming)
        goto done;
      phi->phi_incoming = incoming;
      size_t out = phi->phi_incoming_len;
      for (size_t k = 0; k < phi->phi_incoming_len; ++k) {
        size_t pred = unswitch_block_for_label(
            cfg, phi->phi_incoming[k].predecessor_label);
        if (pred >= cfg->block_count || !members[pred])
          continue;
        int64_t clone_pred =
            fallthrough_exits[pred] ? exit_label_map[pred] : label_map[pred];
        if (clone_pred < 0)
          goto done;
        phi->phi_incoming[out++] = (nyr_phi_incoming_t){
            .value = unswitch_value(value_map, value_count,
                                    phi->phi_incoming[k].value),
            .predecessor_label = clone_pred};
      }
      phi->phi_incoming_len = new_len;
    }
  }

  stage = "rebuild";
  if (clone_count == SIZE_MAX)
    goto done;
  size_t insert_count = clone_count + 1;
  if (insert_count > SIZE_MAX - f->len)
    goto done;
  size_t new_len = f->len + insert_count;
  rebuilt = calloc(new_len, sizeof(*rebuilt));
  if (!rebuilt)
    goto done;
  size_t out = 0;
  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t moved = f->data[i];
    if (i == preheader_term) {
      nyr_inst_t dispatch = f->data[preheader_term];
      dispatch.op = NYIR_BR_IF;
      dispatch.dst = dispatch.b = dispatch.c = dispatch.d = dispatch.e =
          dispatch.f = -1;
      dispatch.a = condition;
      rebuilt[out++] = dispatch;
      rebuilt[out++] = (nyr_inst_t){.op = NYIR_BR,
                                    .dst = -1,
                                    .a = -1,
                                    .b = -1,
                                    .c = -1,
                                    .d = -1,
                                    .e = -1,
                                    .f = -1,
                                    .effects = NYIR_EFFECT_CONTROL,
                                    .imm = label_map[header]};
      continue;
    }
    if (i == branch_index) {
      moved.op = NYIR_BR;
      moved.dst = moved.a = moved.b = moved.c = moved.d = moved.e = moved.f =
          -1;
    } else if (i == false_branch_index) {
      nyr_inst_discard(&moved);
    }
    rebuilt[out++] = moved;
  }
  if (out + clone_count != new_len)
    goto done;
  memcpy(&rebuilt[out], clones, clone_count * sizeof(*clones));
  out += clone_count;

  free(f->data);
  f->data = rebuilt;
  f->len = new_len;
  f->cap = new_len;
  f->next_value = next_value;
  rebuilt = NULL;
  free(clones);
  clones = NULL;
  clone_count = 0;
  changed = true;

done:
  if (!changed && ny_dbg_is_enabled(NY_DBG_HEURISTIC))
    ny_dbg_heuristic_event("LOOP_UNSWITCH",
                           "decision=reject header=L%lld stage=%s block=%zu "
                           "label=L%lld",
                           (long long)loop->header_label, stage, rejected_block,
                           (long long)(rejected_block < cfg->block_count
                                           ? cfg->block_label[rejected_block]
                                           : -1));
  if (clones) {
    for (size_t i = 0; i < clone_built; ++i)
      nyr_inst_discard(&clones[i]);
  }
  free(clones);
  free(rebuilt);
  free(cloned_at);
  free(exit_label_map);
  free(label_map);
  free(value_map);
  free(fallthrough_target);
  free(exit_blocks);
  free(fallthrough_exits);
  free(latch_members);
  free(latches);
  free(members);
  free(defs);
  return changed;
}

/*
 * Loop unswitching is restricted to a verified natural loop with a single
 * preheader and direct latches sharing one induction step
 * with an identified induction PHI and any additional header recurrences
 * cloned by SSA remapping. Loop-defined values may escape only through
 * remapped exit PHIs; unsupported exits and noncanonical branch edges fail
 * closed.
 */
bool nyr_loop_unswitch(nyr_func_t *f) {
  if (!f || f->len < 8 || f->next_value <= 0)
    return true;

  nyr_scev_info_t info = {0};
  nyr_cfg_t cfg = {0};
  if (!nyr_scev_analyze(f, &info) || !nyr_cfg_build(f, &cfg)) {
    nyr_scev_free(&info);
    nyr_cfg_free(&cfg);
    return false;
  }
  for (size_t li = 0; li < info.count; ++li) {
    const nyr_scev_loop_t *loop = &info.loops[li];
    nyr_func_t candidate = {0};
    if (!nyr_ir_func_clone(f, &candidate)) {
      nyr_scev_free(&info);
      nyr_cfg_free(&cfg);
      return false;
    }
    nyr_cfg_t candidate_cfg = {0};
    if (!nyr_cfg_build(&candidate, &candidate_cfg)) {
      nyr_ir_func_free(&candidate);
      nyr_cfg_free(&candidate_cfg);
      continue;
    }
    bool changed = unswitch_try_one(&candidate, &candidate_cfg, loop);
    nyr_cfg_free(&candidate_cfg);
    char err[256] = {0};
    if (changed && nyr_ir_verify(&candidate, err, sizeof(err))) {
      nyr_func_replace(f, &candidate);
      nyr_ir_func_free(&candidate);
      nyr_scev_free(&info);
      nyr_cfg_free(&cfg);
      return true;
    }
    if (changed && ny_dbg_is_enabled(NY_DBG_HEURISTIC))
      ny_dbg_heuristic_event("LOOP_UNSWITCH",
                             "decision=reject stage=verify error=%s", err);
    nyr_ir_func_free(&candidate);
  }
  nyr_scev_free(&info);
  nyr_cfg_free(&cfg);
  return true;
}

/*
 * Fully unroll a small canonical SSA counted loop. This complements the
 * local-slot unroller in advanced.c: header PHIs are substituted with the
 * proven initial/backedge values, while the single-block body is cloned in
 * source order. The strict shape keeps CFG remapping local and makes this
 * useful before vectorization exposes constants in the unrolled body.
 */
static int lu_remap_value(const int *map, size_t count, int value) {
  if (value < 0 || (size_t)value >= count || map[value] < 0)
    return value;
  return map[value];
}

static bool lu_scalar_pure_op(const nyr_inst_t *in) {
  if (!in || in->symbol || in->extra_args || in->arg_sizes ||
      in->phi_incoming || nyr_effective_effects(in) != NYIR_EFFECT_NONE)
    return false;
  switch (in->op) {
  case NYIR_NOP:
  case NYIR_CONST_I64:
  case NYIR_CONST_F64:
  case NYIR_CONST_F32:
  case NYIR_COPY:
  case NYIR_ADD_I64:
  case NYIR_SUB_I64:
  case NYIR_MUL_I64:
  case NYIR_AND_I64:
  case NYIR_OR_I64:
  case NYIR_XOR_I64:
  case NYIR_SHL_I64:
  case NYIR_SAR_I64:
  case NYIR_ROR_I64:
  case NYIR_CMP_I64:
  case NYIR_ADD_F64:
  case NYIR_SUB_F64:
  case NYIR_MUL_F64:
  case NYIR_CMP_F64:
  case NYIR_ADD_F32:
  case NYIR_SUB_F32:
  case NYIR_MUL_F32:
  case NYIR_CMP_F32:
  case NYIR_SELECT_I64:
    return true;
  default:
    return false;
  }
}

/*
 * Division may trap, so preserve its original block and iteration order.
 */
static bool lu_clone_ordered_division(const nyr_inst_t *in) {
  return in && in->op == NYIR_DIV_I64 && !in->symbol && !in->extra_args &&
         !in->arg_sizes && !in->phi_incoming &&
         nyr_effective_effects(in) == NYIR_EFFECT_MAY_TRAP;
}

static bool lu_clone_reject(const nyr_scev_loop_t *loop, const char *reason) {
  if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
    ny_dbg_heuristic_event("LOOP_UNROLL_SSA",
                           "multi-block clone rejected header=L%lld reason=%s",
                           (long long)(loop ? loop->header_label : -1), reason);
  return false;
}

/*
 * Clone a small counted loop whose body contains nested control flow. The
 * single-block unroller below remains the cheaper path; this path accepts
 * scalar operations and ordered calls, with one header guard, one latch, and
 * one exit. Calls are copied in iteration order, never hoisted or reordered.
 * It is useful when a short outer loop carries constants into a nested loop:
 * after cloning, ordinary propagation and unrolling can specialize the inner
 * loops independently.
 */
#define LU_CFG_CLONE_MAX_EXPANDED_INSTS 256u

static bool lu_try_clone_cfg(nyr_func_t *work, const nyr_scev_loop_t *loop,
                             const nyr_cfg_t *cfg, const bool *members) {
  if (!work || !loop || !cfg || !members || !loop->trip_count_known ||
      loop->trip_count < 2 || loop->trip_count > 16 || loop->step <= 0 ||
      loop->iv < 0 || loop->iv >= work->next_value)
    return lu_clone_reject(loop, "loop-shape");

  const size_t trips = (size_t)loop->trip_count;
  const size_t h = loop->header_block, l = loop->latch_block;
  const size_t p = loop->preheader_block, nb = cfg->block_count;
  if (h >= nb || l >= nb || p >= nb || h == l || !members[h] || !members[l])
    return lu_clone_reject(loop, "block-index");

  size_t first_block = nb, last_block = 0, member_count = 0;
  size_t loop_inst_count = 0;
  int64_t next_label = 0;
  for (size_t b = 0; b < nb; ++b) {
    if (cfg->block_label[b] == INT64_MAX)
      return lu_clone_reject(loop, "label-overflow");
    if (cfg->block_label[b] >= next_label)
      next_label = cfg->block_label[b] + 1;
    if (!members[b])
      continue;
    if (cfg->block_start[b] >= cfg->block_end[b]) {
      if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
        ny_dbg_heuristic_event(
            "LOOP_UNROLL_SSA",
            "multi-block clone invalid block header=L%lld block=%zu label=%lld "
            "start=%zu end=%zu",
            (long long)loop->header_label, b, (long long)cfg->block_label[b],
            cfg->block_start[b], cfg->block_end[b]);
      return lu_clone_reject(loop, "invalid-loop-block");
    }
    if (b < first_block)
      first_block = b;
    if (b > last_block)
      last_block = b;
    ++member_count;
    loop_inst_count += cfg->block_end[b] - cfg->block_start[b];
  }
  if (!member_count || next_label <= 0 ||
      loop_inst_count > LU_CFG_CLONE_MAX_EXPANDED_INSTS ||
      loop_inst_count > SIZE_MAX / trips ||
      loop_inst_count * trips > LU_CFG_CLONE_MAX_EXPANDED_INSTS ||
      member_count > (SIZE_MAX - loop_inst_count) / trips) {
    return lu_clone_reject(loop, "code-size");
  }

  /*
   * Outside control may enter only through the unique loop preheader.
   */
  size_t preheader_edges = 0;
  for (size_t b = 0; b < nb; ++b) {
    for (size_t e = cfg->succ_offsets[b]; e < cfg->succ_offsets[b + 1]; ++e) {
      size_t to = cfg->succ_blocks[e];
      if (!members[to] || members[b])
        continue;
      if (b != p || to != h)
        return lu_clone_reject(loop, "external-loop-entry");
      ++preheader_edges;
    }
  }
  if (preheader_edges != 1)
    return lu_clone_reject(loop, "preheader-edge-count");

  /*
   * The header has one counted-loop guard and one false edge out.
   */
  size_t guard = SIZE_MAX, guard_true = SIZE_MAX, exit_block = SIZE_MAX;
  size_t header_exit_branch = SIZE_MAX;
  bool synthesize_exit_label = false;
  int64_t exit_label = -1;
  size_t header_external_preds = 0;
  for (size_t e = cfg->pred_offsets[h]; e < cfg->pred_offsets[h + 1]; ++e) {
    size_t pred = cfg->pred_blocks[e];
    if (pred == p)
      ++header_external_preds;
    else if (pred != l)
      return lu_clone_reject(loop, "header-predecessor");
  }
  if (header_external_preds != 1 ||
      cfg->succ_offsets[h + 1] - cfg->succ_offsets[h] != 2)
    return lu_clone_reject(loop, "header-successor-count");
  for (size_t i = cfg->block_start[h]; i < cfg->block_end[h]; ++i) {
    const nyr_inst_t *in = &work->data[i];
    if (in->op == NYIR_BR_IF) {
      if (guard != SIZE_MAX)
        return lu_clone_reject(loop, "multiple-header-guards");
      guard = i;
    }
    if (in->op == NYIR_CALL || in->op == NYIR_RET)
      return lu_clone_reject(loop, "call-or-return-in-header");
  }
  if (guard == SIZE_MAX)
    return lu_clone_reject(loop, "missing-header-guard");
  int64_t guard_target_label = work->data[guard].imm;
  size_t guard_target = SIZE_MAX;
  for (size_t b = 0; b < nb; ++b)
    if (cfg->block_label[b] == guard_target_label) {
      guard_target = b;
      break;
    }
  if (guard_target >= nb || !members[guard_target])
    return lu_clone_reject(loop, "guard-target");
  for (size_t e = cfg->succ_offsets[h]; e < cfg->succ_offsets[h + 1]; ++e) {
    size_t to = cfg->succ_blocks[e];
    if (to == guard_target)
      guard_true = to;
    else if (!members[to])
      exit_block = to;
    else
      return lu_clone_reject(loop, "header-edge-shape");
  }
  if (guard_true == SIZE_MAX || exit_block == SIZE_MAX)
    return lu_clone_reject(loop, "header-exit-shape");
  exit_label = cfg->block_label[exit_block];
  if (exit_label < 0) {
    size_t pred_count =
        cfg->pred_offsets[exit_block + 1] - cfg->pred_offsets[exit_block];
    if (pred_count != 1 || cfg->pred_blocks[cfg->pred_offsets[exit_block]] != h)
      return lu_clone_reject(loop, "unlabeled-exit-predecessor");
    for (size_t i = cfg->block_start[exit_block];
         i < cfg->block_end[exit_block]; ++i)
      if (work->data[i].op == NYIR_PHI)
        return lu_clone_reject(loop, "unlabeled-exit-phi");
    if (next_label == INT64_MAX)
      return lu_clone_reject(loop, "label-overflow");
    exit_label = next_label++;
    synthesize_exit_label = true;
  }
  for (size_t i = cfg->block_start[h]; i < cfg->block_end[h]; ++i) {
    const nyr_inst_t *in = &work->data[i];
    if (in->op != NYIR_BR)
      continue;
    if (cfg->block_label[exit_block] < 0 || in->imm != exit_label ||
        header_exit_branch != SIZE_MAX)
      return lu_clone_reject(loop, "header-exit-branch");
    header_exit_branch = i;
  }
  size_t phi_count = 0;
  int64_t header_label = cfg->block_label[h];
  int64_t latch_label = cfg->block_label[l];
  int64_t preheader_label = cfg->block_label[p];
  size_t preheader_term = SIZE_MAX;
  for (size_t i = cfg->block_start[p]; i < cfg->block_end[p]; ++i)
    if (work->data[i].op == NYIR_BR && work->data[i].imm == header_label)
      preheader_term = i;
  bool synthesize_preheader_branch = preheader_term == SIZE_MAX;
  if (synthesize_preheader_branch) {
    size_t succ_count = cfg->succ_offsets[p + 1] - cfg->succ_offsets[p];
    size_t end = cfg->block_end[p];
    while (end > cfg->block_start[p] && work->data[end - 1].op == NYIR_NOP)
      --end;
    if (succ_count != 1 || cfg->succ_blocks[cfg->succ_offsets[p]] != h ||
        end == cfg->block_start[p] || end >= work->len ||
        work->data[end - 1].op == NYIR_BR ||
        work->data[end - 1].op == NYIR_BR_IF ||
        work->data[end - 1].op == NYIR_RET)
      return lu_clone_reject(loop, "preheader-fallthrough");
  }

  for (size_t b = first_block; b <= last_block; ++b) {
    if (!members[b])
      continue;
    bool has_terminator = false, saw_conditional = false;
    bool saw_unconditional = false;
    for (size_t i = cfg->block_start[b]; i < cfg->block_end[b]; ++i) {
      const nyr_inst_t *in = &work->data[i];
      if (in->op == NYIR_PHI) {
        if (b == h)
          ++phi_count;
        else if (in->dst < 0 || in->phi_incoming_len == 0)
          return lu_clone_reject(loop, "nested-phi-shape");
      } else if (in->op == NYIR_BR_IF) {
        if (saw_conditional || saw_unconditional)
          return lu_clone_reject(loop, "multiple-conditional-branches");
        saw_conditional = true;
        has_terminator = true;
        if (b == h && i != guard)
          return lu_clone_reject(loop, "extra-header-branch");
      } else if (in->op == NYIR_BR) {
        if (saw_unconditional || (has_terminator && !saw_conditional))
          return lu_clone_reject(loop, "branch-order");
        saw_unconditional = true;
        has_terminator = true;
      } else if (in->op == NYIR_RET) {
        if (has_terminator)
          return lu_clone_reject(loop, "terminator-order");
        has_terminator = true;
      } else if (in->op != NYIR_LABEL && in->op != NYIR_NOP &&
                 in->op != NYIR_CALL && !lu_scalar_pure_op(in) &&
                 !lu_clone_ordered_division(in)) {
        if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
          ny_dbg_heuristic_event(
              "LOOP_UNROLL_SSA",
              "unsupported loop instruction pc=%zu op=%s block=%zu label=L%lld",
              i, nyr_op_name(in->op), b, (long long)cfg->block_label[b]);
        return lu_clone_reject(loop, "unsupported-loop-op");
      }
      if (in->op == NYIR_RET)
        return lu_clone_reject(loop, "return-in-loop");
      if (in->op != NYIR_CALL &&
          (in->extra_args || in->arg_sizes || in->symbol))
        return lu_clone_reject(loop, "non-call-metadata");
    }
    if (cfg->succ_offsets[b + 1] == cfg->succ_offsets[b])
      return lu_clone_reject(loop, "block-without-successor");
    for (size_t e = cfg->succ_offsets[b]; e < cfg->succ_offsets[b + 1]; ++e) {
      size_t to = cfg->succ_blocks[e];
      if (b == h && to == exit_block)
        continue;
      if (b == l && to == h)
        continue;
      if (!members[to])
        return lu_clone_reject(loop, "edge-leaves-loop");
    }
  }
  if (!phi_count)
    return lu_clone_reject(loop, "missing-header-phi");

  const size_t value_count =
      (size_t)(work->next_value > 0 ? work->next_value : 1);
  if ((size_t)loop->iv >= value_count || value_count > SIZE_MAX / trips ||
      nb > SIZE_MAX / trips || trips * value_count > SIZE_MAX / sizeof(int) ||
      trips * nb > SIZE_MAX / sizeof(int64_t))
    return lu_clone_reject(loop, "allocation-size-overflow");
  int *maps = malloc(trips * value_count * sizeof(*maps));
  int *phi_init = malloc(value_count * sizeof(*phi_init));
  int *phi_next = malloc(value_count * sizeof(*phi_next));
  int *exit_values = malloc(value_count * sizeof(*exit_values));
  int64_t *labels = malloc(trips * nb * sizeof(*labels));
  bool *inside_defs = calloc(value_count, sizeof(*inside_defs));
  nyr_inst_t *clones = NULL, *rebuilt = NULL;
  size_t clone_len = 0, clone_cap = 0;
  bool changed = false;
  const char *reject_stage = "allocation";
  if (!maps || !phi_init || !phi_next || !exit_values || !labels ||
      !inside_defs)
    goto done;
  for (size_t v = 0; v < value_count; ++v)
    phi_init[v] = phi_next[v] = exit_values[v] = -1;
  for (size_t i = 0; i < trips * value_count; ++i)
    maps[i] = -1;
  for (size_t i = 0; i < trips * nb; ++i)
    labels[i] = -1;
  /*
   * Identify the outer recurrences; nested-loop PHIs remain ordinary PHIs.
   */
  reject_stage = "header PHI setup";
  for (size_t i = cfg->block_start[h]; i < cfg->block_end[h]; ++i) {
    const nyr_inst_t *phi = &work->data[i];
    if (phi->op != NYIR_PHI)
      continue;
    if (phi->dst < 0 || (size_t)phi->dst >= value_count ||
        phi->phi_incoming_len != 2)
      goto done;
    for (size_t k = 0; k < phi->phi_incoming_len; ++k) {
      const nyr_phi_incoming_t *inc = &phi->phi_incoming[k];
      if (inc->predecessor_label == preheader_label)
        phi_init[phi->dst] = inc->value;
      else if (inc->predecessor_label == latch_label)
        phi_next[phi->dst] = inc->value;
    }
    if (phi_init[phi->dst] < 0 || phi_next[phi->dst] < 0)
      goto done;
  }
  if (phi_init[loop->iv] < 0 || phi_next[loop->iv] < 0)
    goto done;

  /*
   * Values defined inside the loop may escape only through a header PHI.
   */
  reject_stage = "loop-local value escape";
  for (size_t b = first_block; b <= last_block; ++b) {
    if (!members[b])
      continue;
    for (size_t i = cfg->block_start[b]; i < cfg->block_end[b]; ++i) {
      const nyr_inst_t *in = &work->data[i];
      if (in->dst >= 0 && (size_t)in->dst < value_count) {
        inside_defs[in->dst] = true;
        if (b == h && in->op == NYIR_PHI)
          inside_defs[in->dst] = false;
      }
    }
  }
  for (size_t i = 0; i < work->len; ++i) {
    size_t b = cfg->inst_block[i];
    if (b < nb && members[b])
      continue;
    const nyr_inst_t *in = &work->data[i];
    if (in->extra_args_len && !in->extra_args)
      goto done;
    const int ops[] = {in->a, in->b, in->c, in->d, in->e, in->f};
    for (size_t k = 0; k < sizeof(ops) / sizeof(ops[0]); ++k)
      if (ops[k] >= 0 && (size_t)ops[k] < value_count && inside_defs[ops[k]])
        goto done;
    for (size_t k = 0; k < in->extra_args_len; ++k) {
      int v = in->extra_args[k];
      if (v >= 0 && (size_t)v < value_count && inside_defs[v])
        goto done;
    }
    for (size_t k = 0; k < in->phi_incoming_len; ++k) {
      int v = in->phi_incoming[k].value;
      if (v >= 0 && (size_t)v < value_count && inside_defs[v])
        goto done;
    }
  }

  /*
   * Preserve source labels where possible. Fallthrough blocks have no source
   * label; give them one so cloned branches can target them explicitly.
   */
  reject_stage = "clone labels";
  for (size_t k = 0; k < trips; ++k) {
    for (size_t b = first_block; b <= last_block; ++b) {
      if (!members[b])
        continue;
      if (k == 0 && cfg->block_label[b] >= 0)
        labels[k * nb + b] = cfg->block_label[b];
      else {
        if (next_label == INT64_MAX)
          goto done;
        labels[k * nb + b] = next_label++;
      }
    }
  }

  /*
   * Make a fresh SSA mapping for every clone, including nested-loop PHIs.
   */
  reject_stage = "SSA mapping";
  int next_value = work->next_value;
  for (size_t k = 0; k < trips; ++k) {
    int *map = &maps[k * value_count];
    for (size_t i = cfg->block_start[h]; i < cfg->block_end[h]; ++i) {
      const nyr_inst_t *phi = &work->data[i];
      if (phi->op != NYIR_PHI)
        continue;
      int carry = phi_init[phi->dst];
      if (k > 0) {
        int prior = phi_next[phi->dst];
        if (prior < 0 || (size_t)prior >= value_count)
          goto done;
        int mapped = maps[(k - 1) * value_count + (size_t)prior];
        carry = mapped >= 0 ? mapped : prior;
      }
      if (phi->dst == loop->iv) {
        __int128 iv = (__int128)loop->init + (__int128)k * loop->step;
        if (iv < INT64_MIN || iv > INT64_MAX || next_value == INT_MAX)
          goto done;
        map[phi->dst] = next_value++;
      } else {
        map[phi->dst] = carry;
      }
    }
    for (size_t b = first_block; b <= last_block; ++b) {
      if (!members[b])
        continue;
      for (size_t i = cfg->block_start[b]; i < cfg->block_end[b]; ++i) {
        const nyr_inst_t *in = &work->data[i];
        if (in->dst < 0 || (b == h && in->op == NYIR_PHI))
          continue;
        if ((size_t)in->dst >= value_count || next_value == INT_MAX)
          goto done;
        map[in->dst] = next_value++;
      }
    }
  }

  size_t max_clone_count = (loop_inst_count + member_count + 1) * trips;
  if (max_clone_count > 4096 || max_clone_count > SIZE_MAX / sizeof(*clones))
    goto done;
  clone_cap = max_clone_count;
  clones = calloc(clone_cap, sizeof(*clones));
  if (!clones)
    goto done;

  reject_stage = "block and edge cloning";
  for (size_t k = 0; k < trips; ++k) {
    int *map = &maps[k * value_count];
    for (size_t b = first_block; b <= last_block; ++b) {
      if (!members[b])
        continue;
      if (clone_len >= clone_cap)
        goto done;
      clones[clone_len++] = (nyr_inst_t){.op = NYIR_LABEL,
                                         .dst = -1,
                                         .a = -1,
                                         .b = -1,
                                         .c = -1,
                                         .d = -1,
                                         .e = -1,
                                         .f = -1,
                                         .imm = labels[k * nb + b]};
      if (b == h) {
        int iv = map[loop->iv];
        __int128 value = (__int128)loop->init + (__int128)k * loop->step;
        if (iv < 0 || value < INT64_MIN || value > INT64_MAX ||
            clone_len >= clone_cap)
          goto done;
        clones[clone_len++] = (nyr_inst_t){.op = NYIR_CONST_I64,
                                           .dst = iv,
                                           .a = -1,
                                           .b = -1,
                                           .c = -1,
                                           .d = -1,
                                           .e = -1,
                                           .f = -1,
                                           .imm = (int64_t)value,
                                           .scalar_class = NYIR_SCALAR_I64_RAW};
      }
      size_t succ_count = cfg->succ_offsets[b + 1] - cfg->succ_offsets[b];
      size_t branch_if = SIZE_MAX;
      size_t explicit_branch = SIZE_MAX;
      for (size_t i = cfg->block_start[b]; i < cfg->block_end[b]; ++i) {
        const nyr_inst_t *src = &work->data[i];
        if (b == h && src->op == NYIR_PHI)
          continue;
        if (src->op == NYIR_LABEL)
          continue;
        if (b == h && i == guard) {
          nyr_inst_t br = {.op = NYIR_BR,
                           .dst = -1,
                           .a = -1,
                           .b = -1,
                           .c = -1,
                           .d = -1,
                           .e = -1,
                           .f = -1,
                           .imm = labels[k * nb + guard_true],
                           .effects = NYIR_EFFECT_CONTROL};
          if (clone_len >= clone_cap)
            goto done;
          clones[clone_len++] = br;
          continue;
        }
        if (b == h && i == header_exit_branch)
          continue;
        if (b == l && src->op == NYIR_BR && src->imm == header_label) {
          int64_t target =
              k + 1 < trips ? labels[(k + 1) * nb + h] : exit_label;
          nyr_inst_t br = {.op = NYIR_BR,
                           .dst = -1,
                           .a = -1,
                           .b = -1,
                           .c = -1,
                           .d = -1,
                           .e = -1,
                           .f = -1,
                           .imm = target,
                           .effects = NYIR_EFFECT_CONTROL};
          if (clone_len >= clone_cap)
            goto done;
          clones[clone_len++] = br;
          explicit_branch = i;
          continue;
        }
        if (src->op == NYIR_BR_IF)
          branch_if = i;
        if (src->op == NYIR_BR)
          explicit_branch = i;
        if (clone_len >= clone_cap)
          goto done;
        nyr_inst_t copy;
        if (!unswitch_copy_inst(src, &copy))
          goto done;
        if (src->phi_incoming_len) {
          for (size_t q = 0; q < copy.phi_incoming_len; ++q) {
            if (copy.phi_incoming[q].predecessor_label < 0) {
              nyr_inst_discard(&copy);
              goto done;
            }
            int pred = -1;
            for (size_t pb = first_block; pb <= last_block; ++pb)
              if (members[pb] && cfg->block_label[pb] ==
                                     copy.phi_incoming[q].predecessor_label) {
                pred = (int)pb;
                break;
              }
            if (pred < 0 || !members[(size_t)pred]) {
              nyr_inst_discard(&copy);
              goto done;
            }
            copy.phi_incoming[q].predecessor_label =
                labels[k * nb + (size_t)pred];
            int v = copy.phi_incoming[q].value;
            if (v >= 0 && (size_t)v < value_count && map[v] >= 0)
              copy.phi_incoming[q].value = map[v];
          }
        }
        if (src->op == NYIR_BR) {
          size_t target = SIZE_MAX;
          for (size_t tb = 0; tb < nb; ++tb)
            if (cfg->block_label[tb] == src->imm) {
              target = tb;
              break;
            }
          if (target >= nb || !members[target]) {
            nyr_inst_discard(&copy);
            goto done;
          }
          copy.imm = labels[k * nb + target];
        } else if (src->op == NYIR_BR_IF) {
          size_t target = SIZE_MAX;
          for (size_t tb = 0; tb < nb; ++tb)
            if (cfg->block_label[tb] == src->imm) {
              target = tb;
              break;
            }
          if (target >= nb || !members[target]) {
            nyr_inst_discard(&copy);
            goto done;
          }
          copy.imm = labels[k * nb + target];
        }
        if (copy.dst >= 0 && (size_t)copy.dst < value_count &&
            map[copy.dst] >= 0)
          copy.dst = map[copy.dst];
        int *ops[] = {&copy.a, &copy.b, &copy.c, &copy.d, &copy.e, &copy.f};
        for (size_t q = 0; q < sizeof(ops) / sizeof(*ops); ++q) {
          int v = *ops[q];
          if (v >= 0 && (size_t)v < value_count && map[v] >= 0)
            *ops[q] = map[v];
        }
        for (size_t q = 0; q < copy.extra_args_len; ++q) {
          int v = copy.extra_args[q];
          if (v >= 0 && (size_t)v < value_count && map[v] >= 0)
            copy.extra_args[q] = map[v];
        }
        clones[clone_len++] = copy;
      }

      /*
       * Convert implicit fallthroughs into explicit clone-local branches.
       */
      if (b == h)
        continue; /* the counted guard was replaced by an unconditional edge */
      if (branch_if != SIZE_MAX) {
        int64_t true_label = work->data[branch_if].imm;
        size_t true_target = SIZE_MAX, false_target = SIZE_MAX;
        for (size_t tb = 0; tb < nb; ++tb)
          if (cfg->block_label[tb] == true_label) {
            true_target = tb;
            break;
          }
        for (size_t e = cfg->succ_offsets[b]; e < cfg->succ_offsets[b + 1];
             ++e) {
          size_t to = cfg->succ_blocks[e];
          if (to != true_target)
            false_target = to;
        }
        if (true_target >= nb || false_target >= nb || !members[false_target])
          goto done;
        if (explicit_branch == SIZE_MAX) {
          nyr_inst_t br = {.op = NYIR_BR,
                           .dst = -1,
                           .a = -1,
                           .b = -1,
                           .c = -1,
                           .d = -1,
                           .e = -1,
                           .f = -1,
                           .imm = labels[k * nb + false_target],
                           .effects = NYIR_EFFECT_CONTROL};
          if (clone_len >= clone_cap)
            goto done;
          clones[clone_len++] = br;
        }
      } else if (explicit_branch == SIZE_MAX) {
        if (succ_count != 1)
          goto done;
        size_t to = cfg->succ_blocks[cfg->succ_offsets[b]];
        if (b == l && to == h)
          to = k + 1 < trips ? h : exit_block;
        if ((b == l && to == exit_block) || members[to]) {
          int64_t target_label =
              members[to] ? labels[k * nb + to] : cfg->block_label[to];
          nyr_inst_t br = {.op = NYIR_BR,
                           .dst = -1,
                           .a = -1,
                           .b = -1,
                           .c = -1,
                           .d = -1,
                           .e = -1,
                           .f = -1,
                           .imm = target_label,
                           .effects = NYIR_EFFECT_CONTROL};
          if (clone_len >= clone_cap)
            goto done;
          clones[clone_len++] = br;
        } else {
          goto done;
        }
      }
    }
  }

  /*
   * Rewire exit PHIs to the final cloned latch.
   */
  reject_stage = "exit PHI remapping";
  for (size_t i = cfg->block_start[exit_block]; i < cfg->block_end[exit_block];
       ++i) {
    nyr_inst_t *phi = &work->data[i];
    if (phi->op != NYIR_PHI)
      continue;
    for (size_t q = 0; q < phi->phi_incoming_len; ++q)
      if (phi->phi_incoming[q].predecessor_label == header_label) {
        phi->phi_incoming[q].predecessor_label = labels[(trips - 1) * nb + l];
      }
  }
  for (size_t i = cfg->block_start[h]; i < cfg->block_end[h]; ++i) {
    const nyr_inst_t *phi = &work->data[i];
    if (phi->op != NYIR_PHI)
      continue;
    int v = phi_next[phi->dst];
    if (v < 0 || (size_t)v >= value_count)
      goto done;
    int mapped = maps[(trips - 1) * value_count + (size_t)v];
    exit_values[phi->dst] = mapped >= 0 ? mapped : v;
  }

  size_t extra_labels = synthesize_exit_label ? 1 : 0;
  size_t extra_preheader_branches = synthesize_preheader_branch ? 1 : 0;
  if (clone_len > SIZE_MAX - extra_labels ||
      clone_len + extra_labels > SIZE_MAX - extra_preheader_branches ||
      work->len >
          SIZE_MAX - (clone_len + extra_labels + extra_preheader_branches))
    goto done;
  size_t rebuilt_cap =
      work->len + clone_len + extra_labels + extra_preheader_branches;
  rebuilt = calloc(rebuilt_cap, sizeof(*rebuilt));
  if (!rebuilt)
    goto done;
  reject_stage = "function rebuild";
  size_t out = 0;
  bool emitted = false;
  for (size_t i = 0; i < work->len; ++i) {
    if (synthesize_preheader_branch && i == cfg->block_end[p]) {
      rebuilt[out++] = (nyr_inst_t){.op = NYIR_BR,
                                    .dst = -1,
                                    .a = -1,
                                    .b = -1,
                                    .c = -1,
                                    .d = -1,
                                    .e = -1,
                                    .f = -1,
                                    .imm = header_label,
                                    .effects = NYIR_EFFECT_CONTROL};
    }
    size_t b = cfg->inst_block[i];
    if (b >= first_block && b <= last_block && members[b]) {
      if (!emitted && i == cfg->block_start[first_block]) {
        memcpy(&rebuilt[out], clones, clone_len * sizeof(*clones));
        out += clone_len;
        emitted = true;
      }
      nyr_inst_discard(&work->data[i]);
      continue;
    }
    if (synthesize_exit_label && b == exit_block &&
        i == cfg->block_start[exit_block]) {
      rebuilt[out++] = (nyr_inst_t){.op = NYIR_LABEL,
                                    .dst = -1,
                                    .a = -1,
                                    .b = -1,
                                    .c = -1,
                                    .d = -1,
                                    .e = -1,
                                    .f = -1,
                                    .imm = exit_label};
    }
    nyr_inst_t moved = work->data[i];
    int *ops[] = {&moved.a, &moved.b, &moved.c, &moved.d, &moved.e, &moved.f};
    for (size_t q = 0; q < sizeof(ops) / sizeof(*ops); ++q) {
      int v = *ops[q];
      if (v >= 0 && (size_t)v < value_count && exit_values[v] >= 0)
        *ops[q] = exit_values[v];
    }
    for (size_t q = 0; q < moved.phi_incoming_len; ++q) {
      int v = moved.phi_incoming[q].value;
      if (v >= 0 && (size_t)v < value_count && exit_values[v] >= 0)
        moved.phi_incoming[q].value = exit_values[v];
      if (moved.phi_incoming[q].predecessor_label == header_label)
        moved.phi_incoming[q].predecessor_label = labels[(trips - 1) * nb + l];
    }
    rebuilt[out++] = moved;
  }
  if (!emitted || out > rebuilt_cap)
    goto done;
  free(work->data);
  work->data = rebuilt;
  work->len = out;
  work->cap = work->len;
  work->next_value = next_value;
  rebuilt = NULL;
  changed = true;

done:
  if (clones && !changed)
    for (size_t i = 0; i < clone_len; ++i)
      nyr_inst_discard(&clones[i]);
  free(clones);
  free(rebuilt);
  free(maps);
  free(phi_init);
  free(phi_next);
  free(exit_values);
  free(labels);
  free(inside_defs);
  if (!changed && ny_dbg_is_enabled(NY_DBG_HEURISTIC))
    ny_dbg_heuristic_event("LOOP_UNROLL_SSA",
                           "multi-block clone rejected header=L%lld stage=%s",
                           (long long)loop->header_label, reject_stage);
  return changed;
}

static bool lu_try_one(nyr_func_t *work, const nyr_scev_loop_t *loop) {
  const char *reject_stage = "loop-shape";
  if (!work || !loop || !loop->init_is_const || !loop->trip_count_known ||
      loop->trip_count < 2 || loop->trip_count > 16 || loop->step <= 0 ||
      loop->iv < 0)
    return false;

  nyr_cfg_t cfg = {0};
  size_t *phi_index = NULL, *body_index = NULL;
  int *phi_init = NULL, *phi_next = NULL;
  int **maps = NULL;
  int *exit_values = NULL;
  nyr_inst_t *clones = NULL, *rebuilt = NULL;
  bool *members = NULL;
  bool changed = false;
  reject_stage = "cfg-build";
  if (!nyr_cfg_build(work, &cfg))
    goto done;
  reject_stage = "block-shape";
  if (loop->header_block >= cfg.block_count ||
      loop->latch_block >= cfg.block_count ||
      loop->preheader_block >= cfg.block_count ||
      loop->header_block == loop->latch_block)
    goto done;

  const size_t h = loop->header_block;
  const size_t l = loop->latch_block;
  const size_t p = loop->preheader_block;
  members = calloc(cfg.block_count, sizeof(*members));
  if (!members ||
      !nyr_cfg_natural_loop_blocks(&cfg, l, h, members, cfg.block_count))
    goto done;
  size_t interior_blocks = 0;
  int64_t first_interior_label = -1;
  for (size_t b = 0; b < cfg.block_count; ++b) {
    if (!members[b] || b == h || b == l)
      continue;
    if (first_interior_label < 0)
      first_interior_label = cfg.block_label[b];
    ++interior_blocks;
  }
  if (interior_blocks) {
    changed = lu_try_clone_cfg(work, loop, &cfg, members);
    if (!changed && ny_dbg_is_enabled(NY_DBG_HEURISTIC))
      ny_dbg_heuristic_event(
          "LOOP_UNROLL_SSA",
          "decision=reject header=L%lld trips=%llu "
          "reason=multi-block-clone-legality interior_blocks=%zu first=L%lld",
          (long long)loop->header_label, (unsigned long long)loop->trip_count,
          interior_blocks, (long long)first_interior_label);
    goto done;
  }
  reject_stage = "entry-shape";
  if (!members[h] || !members[l] || cfg.block_label[h] < 0 ||
      cfg.block_label[l] < 0 || cfg.block_start[h] >= cfg.block_end[h] ||
      cfg.block_start[l] >= cfg.block_end[l])
    goto done;

  const int64_t hl = cfg.block_label[h];
  const int64_t ll = cfg.block_label[l];
  const int64_t pl = cfg.block_label[p];
  /*
   * The header may only be entered from its preheader and latch.
   */
  reject_stage = "preheader-edges";
  size_t header_external_preds = 0;
  for (size_t e = cfg.pred_offsets[h]; e < cfg.pred_offsets[h + 1]; ++e) {
    size_t pred = cfg.pred_blocks[e];
    if (pred != p && pred != l)
      goto done;
    if (pred == p)
      ++header_external_preds;
  }
  if (header_external_preds != 1)
    goto done;

  /*
   * The only loop exit must be the false edge of the header guard.
   */
  reject_stage = "exit-shape";
  size_t exit_block = SIZE_MAX;
  for (size_t b = 0; b < cfg.block_count; ++b) {
    if (!members[b])
      continue;
    for (size_t e = cfg.succ_offsets[b]; e < cfg.succ_offsets[b + 1]; ++e) {
      size_t succ = cfg.succ_blocks[e];
      if (members[succ])
        continue;
      if (b != h || exit_block != SIZE_MAX)
        goto done;
      exit_block = succ;
    }
  }
  if (exit_block == SIZE_MAX ||
      cfg.succ_offsets[h + 1] - cfg.succ_offsets[h] != 2)
    goto done;
  bool emit_exit_branch = cfg.block_label[exit_block] >= 0;
  if (!emit_exit_branch && exit_block != h + 1)
    goto done; /* preserve lexical fallthrough through an unlabeled trampoline
                */

  const size_t hs = cfg.block_start[h], he = cfg.block_end[h];
  const size_t ls = cfg.block_start[l], le = cfg.block_end[l];
  reject_stage = "header-and-latch-shape";
  size_t guard = SIZE_MAX, preheader_term = SIZE_MAX, latch_term = SIZE_MAX;
  size_t phi_count = 0, body_count = 0;
  for (size_t i = hs; i < he; ++i) {
    const nyr_inst_t *in = &work->data[i];
    if (in->op == NYIR_BR_IF)
      guard = i;
    else if (in->op == NYIR_PHI)
      ++phi_count;
    else if (in->op != NYIR_LABEL && in->op != NYIR_NOP && in->op != NYIR_BR &&
             !lu_scalar_pure_op(in))
      goto done;
  }
  for (size_t i = cfg.block_start[p]; i < cfg.block_end[p]; ++i)
    if (work->data[i].op == NYIR_BR && work->data[i].imm == hl)
      preheader_term = i;
  for (size_t i = ls; i < le; ++i) {
    const nyr_inst_t *in = &work->data[i];
    if (in->op == NYIR_BR && in->imm == hl) {
      latch_term = i;
      continue;
    }
    if (in->op == NYIR_LABEL || in->op == NYIR_NOP)
      continue;
    if (in->op == NYIR_PHI || in->op == NYIR_BR || in->op == NYIR_BR_IF ||
        in->op == NYIR_RET || !lu_scalar_pure_op(in))
      goto done;
    ++body_count;
  }
  reject_stage = "loop-size";
  if (guard == SIZE_MAX || preheader_term == SIZE_MAX ||
      latch_term == SIZE_MAX || !phi_count || !body_count || body_count > 128 ||
      body_count * (size_t)loop->trip_count > 1024)
    goto done;

  /*
   * Validate that the header's taken edge is the single latch block.
   */
  reject_stage = "successor-shape";
  size_t body_succ = SIZE_MAX, false_succ = SIZE_MAX;
  for (size_t e = cfg.succ_offsets[h]; e < cfg.succ_offsets[h + 1]; ++e) {
    size_t succ = cfg.succ_blocks[e];
    if (succ == l)
      body_succ = succ;
    else if (!members[succ])
      false_succ = succ;
    else
      goto done;
  }
  if (body_succ != l || false_succ != exit_block)
    goto done;

  reject_stage = "allocation";
  const size_t value_count =
      (size_t)(work->next_value > 0 ? work->next_value : 1);
  phi_index = malloc(phi_count * sizeof(*phi_index));
  body_index = malloc(body_count * sizeof(*body_index));
  phi_init = malloc(value_count * sizeof(*phi_init));
  phi_next = malloc(value_count * sizeof(*phi_next));
  maps = calloc((size_t)loop->trip_count, sizeof(*maps));
  exit_values = malloc(value_count * sizeof(*exit_values));
  clones = calloc(body_count * (size_t)loop->trip_count, sizeof(*clones));
  if (!phi_index || !body_index || !phi_init || !phi_next || !maps ||
      !exit_values || !clones)
    goto done;
  for (size_t v = 0; v < value_count; ++v)
    phi_init[v] = phi_next[v] = exit_values[v] = -1;

  size_t pi = 0, bi = 0;
  reject_stage = "phi-inputs";
  for (size_t i = hs; i < he; ++i) {
    const nyr_inst_t *in = &work->data[i];
    if (in->op != NYIR_PHI)
      continue;
    if (in->dst < 0 || (size_t)in->dst >= value_count ||
        in->phi_incoming_len != 2)
      goto done;
    phi_index[pi++] = i;
    for (size_t k = 0; k < in->phi_incoming_len; ++k) {
      const nyr_phi_incoming_t *inc = &in->phi_incoming[k];
      if (inc->predecessor_label == pl)
        phi_init[in->dst] = inc->value;
      else if (inc->predecessor_label == ll)
        phi_next[in->dst] = inc->value;
    }
    if (phi_init[in->dst] < 0 || phi_next[in->dst] < 0)
      goto done;
  }
  if (pi != phi_count || phi_init[loop->iv] < 0 || phi_next[loop->iv] < 0)
    goto done;
  reject_stage = "body-index";
  for (size_t i = ls; i < le; ++i) {
    if (i == latch_term || work->data[i].op == NYIR_LABEL ||
        work->data[i].op == NYIR_NOP)
      continue;
    body_index[bi++] = i;
  }
  if (bi != body_count)
    goto done;

  /*
   * No outside PHI may name the removed latch as a predecessor.
   */
  reject_stage = "outside-phi";
  for (size_t b = 0; b < cfg.block_count; ++b) {
    if (members[b])
      continue;
    for (size_t i = cfg.block_start[b]; i < cfg.block_end[b]; ++i) {
      const nyr_inst_t *in = &work->data[i];
      if (in->op != NYIR_PHI)
        continue;
      for (size_t k = 0; k < in->phi_incoming_len; ++k)
        if (in->phi_incoming[k].predecessor_label == ll)
          goto done;
    }
  }

  reject_stage = "phi-maps";
  for (size_t k = 0; k < (size_t)loop->trip_count; ++k) {
    maps[k] = malloc(value_count * sizeof(**maps));
    if (!maps[k])
      goto done;
    for (size_t v = 0; v < value_count; ++v)
      maps[k][v] = -1;
    for (size_t q = 0; q < phi_count; ++q) {
      const nyr_inst_t *phi = &work->data[phi_index[q]];
      int carry = phi_init[phi->dst];
      if (k > 0) {
        int prior = phi_next[phi->dst];
        if (prior < 0 || (size_t)prior >= value_count || maps[k - 1][prior] < 0)
          goto done;
        carry = maps[k - 1][prior];
      }
      if (phi->dst == loop->iv) {
        __int128 iv = (__int128)loop->init + (__int128)k * loop->step;
        if (iv < INT64_MIN || iv > INT64_MAX || work->next_value == INT_MAX)
          goto done;
        int constant = work->next_value++;
        maps[k][phi->dst] = constant;
      } else {
        maps[k][phi->dst] = carry;
      }
    }
    for (size_t q = 0; q < body_count; ++q) {
      const nyr_inst_t *in = &work->data[body_index[q]];
      if (in->dst >= 0) {
        if ((size_t)in->dst >= value_count || work->next_value == INT_MAX)
          goto done;
        maps[k][in->dst] = work->next_value++;
      }
    }
  }

  /*
   * Build a compact straight-line clone stream; constants are separate.
   */
  reject_stage = "clone";
  size_t const_count = (size_t)loop->trip_count;
  size_t clone_count = body_count * (size_t)loop->trip_count;
  size_t extra_const_at = 0;
  nyr_inst_t *iv_consts = calloc(const_count, sizeof(*iv_consts));
  if (!iv_consts)
    goto done;
  for (size_t k = 0; k < const_count; ++k) {
    int iv_id = maps[k][loop->iv];
    __int128 iv = (__int128)loop->init + (__int128)k * loop->step;
    iv_consts[extra_const_at++] =
        (nyr_inst_t){.op = NYIR_CONST_I64,
                     .dst = iv_id,
                     .a = -1,
                     .b = -1,
                     .c = -1,
                     .d = -1,
                     .e = -1,
                     .f = -1,
                     .imm = (int64_t)iv,
                     .effects = NYIR_EFFECT_NONE,
                     .scalar_class = NYIR_SCALAR_I64_RAW};
  }
  for (size_t q = 0; q < phi_count; ++q) {
    const nyr_inst_t *phi = &work->data[phi_index[q]];
    int next = phi_next[phi->dst];
    if (next < 0 || (size_t)next >= value_count)
      goto done;
    exit_values[phi->dst] =
        maps[const_count - 1][next] >= 0 ? maps[const_count - 1][next] : next;
  }

  for (size_t k = 0; k < (size_t)loop->trip_count; ++k) {
    for (size_t q = 0; q < body_count; ++q) {
      const nyr_inst_t *src = &work->data[body_index[q]];
      nyr_inst_t *dst = &clones[k * body_count + q];
      *dst = *src;
      if (src->dst >= 0)
        dst->dst = maps[k][src->dst];
      int *ops[] = {&dst->a, &dst->b, &dst->c, &dst->d, &dst->e, &dst->f};
      for (size_t oi = 0; oi < sizeof(ops) / sizeof(ops[0]); ++oi)
        *ops[oi] = lu_remap_value(maps[k], value_count, *ops[oi]);
    }
  }

  reject_stage = "rebuilt";
  size_t add_count = clone_count + const_count + (emit_exit_branch ? 1u : 0u);
  if (add_count > SIZE_MAX - work->len)
    goto done;
  rebuilt = calloc(work->len + add_count, sizeof(*rebuilt));
  if (!rebuilt) {
    free(iv_consts);
    goto done;
  }
  size_t out = 0;
  bool emitted = false;
  for (size_t i = 0; i < work->len; ++i) {
    size_t b = cfg.inst_block[i];
    if (b == h) {
      if (!emitted && i == hs) {
        rebuilt[out++] = work->data[hs]; /* keep the original header label */
        for (size_t k = 0; k < const_count; ++k)
          rebuilt[out++] = iv_consts[k];
        for (size_t k = 0; k < clone_count; ++k)
          rebuilt[out++] = clones[k];
        if (emit_exit_branch)
          rebuilt[out++] = (nyr_inst_t){.op = NYIR_BR,
                                        .dst = -1,
                                        .a = -1,
                                        .b = -1,
                                        .c = -1,
                                        .d = -1,
                                        .e = -1,
                                        .f = -1,
                                        .imm = cfg.block_label[exit_block],
                                        .effects = NYIR_EFFECT_CONTROL};
        emitted = true;
      }
      if (i != hs)
        nyr_inst_discard(&work->data[i]);
      continue;
    }
    if (b == l) {
      nyr_inst_discard(&work->data[i]);
      continue;
    }
    nyr_inst_t moved = work->data[i];
    if (!members[b]) {
      int *ops[] = {&moved.a, &moved.b, &moved.c, &moved.d, &moved.e, &moved.f};
      for (size_t oi = 0; oi < sizeof(ops) / sizeof(ops[0]); ++oi) {
        int v = *ops[oi];
        if (v >= 0 && (size_t)v < value_count && exit_values[v] >= 0)
          *ops[oi] = exit_values[v];
      }
      for (size_t q = 0; q < moved.phi_incoming_len; ++q) {
        int v = moved.phi_incoming[q].value;
        if (v >= 0 && (size_t)v < value_count && exit_values[v] >= 0)
          moved.phi_incoming[q].value = exit_values[v];
      }
    }
    rebuilt[out++] = moved;
  }
  free(iv_consts);
  if (!emitted || out > work->len + add_count)
    goto done;

  free(work->data);
  work->data = rebuilt;
  work->len = out;
  work->cap = out;
  rebuilt = NULL;
  changed = true;

done:
  if (!changed && ny_dbg_is_enabled(NY_DBG_HEURISTIC))
    ny_dbg_heuristic_event(
        "LOOP_UNROLL_SSA", "single-block clone rejected header=L%lld stage=%s",
        (long long)(loop ? loop->header_label : -1), reject_stage);
  if (!changed && rebuilt)
    free(rebuilt);
  if (maps) {
    for (size_t k = 0; k < (size_t)loop->trip_count; ++k)
      free(maps[k]);
  }
  free(maps);
  free(exit_values);
  free(clones);
  free(phi_index);
  free(body_index);
  free(phi_init);
  free(phi_next);
  free(members);
  nyr_cfg_free(&cfg);
  return changed;
}

/*
 * Fully unroll a small counted loop whose body contains nested control flow.
 * Clone each block in the natural loop with fresh labels and SSA values,
 * substitute the header recurrences at each iteration, and connect the
 * cloned latch directly to the next header. This remains deliberately
 * bounded: code size grows by at most 8x and no loop may have an exit other
 * than its header guard.
 */

static bool lu_divisor_proven_positive(const nyr_func_t *f, const int *defs,
                                       const nyr_scev_loop_t *loop, int value) {
  int64_t constant = 0;
  if (const_i64_val(f, defs, value, &constant))
    return constant > 0;
  if (idiom_root_copy(f, defs, value) != loop->iv)
    return false;
  return loop->init_is_const && loop->init > 0 && loop->step > 0 &&
         loop->trip_count_known &&
         (loop->predicate == NYIR_CMP_LT || loop->predicate == NYIR_CMP_LE);
}

bool nyr_loop_unroll_ssa(nyr_func_t *f) {
  if (!f || f->len < 8 || f->next_value <= 0)
    return true;
  for (size_t round = 0; round < 8; ++round) {
    nyr_scev_info_t info = {0};
    nyr_cfg_t cfg = {0};
    if (!nyr_scev_analyze(f, &info) || !nyr_cfg_build(f, &cfg)) {
      nyr_scev_free(&info);
      nyr_cfg_free(&cfg);
      return false;
    }
    bool applied = false;
    int *defs = NULL;
    bool defs_attempted = false;
    for (size_t i = 0; i < info.count; ++i) {
      const nyr_scev_loop_t *loop = &info.loops[i];
      if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
        ny_dbg_heuristic_event(
            "LOOP_UNROLL_SSA",
            "candidate header=L%lld latch=L%lld preheader=L%lld trip-known=%d "
            "trips=%llu step=%lld predicate=%d",
            (long long)loop->header_label,
            (long long)(loop->latch_block < cfg.block_count
                            ? cfg.block_label[loop->latch_block]
                            : -1),
            (long long)(loop->preheader_block < cfg.block_count
                            ? cfg.block_label[loop->preheader_block]
                            : -1),
            loop->trip_count_known, (unsigned long long)loop->trip_count,
            (long long)loop->step, (int)loop->predicate);
      if (!loop->trip_count_known || loop->trip_count < 2 ||
          loop->trip_count > 16 ||
          (loop->predicate != NYIR_CMP_LT && loop->predicate != NYIR_CMP_LE) ||
          loop->step <= 0)
        continue;
      /*
       * Unrolling can expose a zero divisor in a branch that is statically
       * unreachable for that iteration. Keep unknown and modulo divisors
       * rolled. Positive constants and positive, non-wrapping induction
       * values make division non-trapping; the cloner preserves source order.
       */
      size_t loop_begin = loop->header_index;
      size_t loop_end = loop->latch_index;
      if (loop_begin >= f->len || loop_end >= f->len || loop_begin > loop_end)
        continue;
      bool unsafe_division = false;
      for (size_t k = loop_begin; k <= loop_end; ++k) {
        const nyr_inst_t *in = &f->data[k];
        if (in->op == NYIR_MOD_I64) {
          unsafe_division = true;
          break;
        }
        if (in->op != NYIR_DIV_I64)
          continue;
        if (!defs_attempted) {
          defs = nyr_build_defs(f);
          defs_attempted = true;
        }
        if (!defs || !lu_divisor_proven_positive(f, defs, loop, in->b)) {
          unsafe_division = true;
          break;
        }
      }
      if (unsafe_division)
        continue;
      nyr_func_t candidate = {0};
      if (!nyr_ir_func_clone(f, &candidate)) {
        free(defs);
        nyr_scev_free(&info);
        nyr_cfg_free(&cfg);
        return false;
      }
      bool changed = lu_try_one(&candidate, loop);

      char err[256] = {0};
      if (changed && nyr_ir_verify(&candidate, err, sizeof(err))) {
        if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
          ny_dbg_heuristic_event(
              "LOOP_UNROLL_SSA",
              "decision=apply header=L%lld trips=%llu insts=%zu",
              (long long)loop->header_label,
              (unsigned long long)loop->trip_count, candidate.len - f->len);
        nyr_func_replace(f, &candidate);
        applied = true;
        break;
      }
      if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
        ny_dbg_heuristic_event(
            "LOOP_UNROLL_SSA", "decision=reject header=L%lld reason=%s",
            (long long)loop->header_label,
            changed ? (err[0] ? err : "verification") : "shape-or-legality");
      nyr_ir_func_free(&candidate);
    }
    free(defs);
    nyr_scev_free(&info);
    nyr_cfg_free(&cfg);
    if (!applied)
      break;
  }
  return true;
}
/*
 * Canonical single-version form for loop-invariant integer predicates.
 */

static int versioning_root_copy(const nyr_func_t *f, const int *defs,
                                int value) {
  while (value >= 0 && value < f->next_value && defs[value] >= 0 &&
         f->data[defs[value]].op == NYIR_COPY)
    value = f->data[defs[value]].a;
  return value;
}

/*
 * Put a loop-invariant relational guard in constant-left canonical form.  It
 * is a concrete, verifier-safe specialization of the existing loop version;
 * guards involving the induction variable are deliberately left unchanged.
 */
bool nyr_loop_versioning(nyr_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;
  nyr_cfg_t cfg = {0};
  int *defs = nyr_build_defs(f);
  if (!defs || !nyr_cfg_build(f, &cfg)) {
    free(defs);
    nyr_cfg_free(&cfg);
    return false;
  }
  bool *in_loop = calloc(cfg.block_count, sizeof(*in_loop));
  if (!in_loop) {
    free(defs);
    nyr_cfg_free(&cfg);
    return false;
  }
  for (size_t latch = 0; latch < cfg.block_count; ++latch) {
    for (size_t e = cfg.succ_offsets[latch]; e < cfg.succ_offsets[latch + 1];
         ++e) {
      size_t header = cfg.succ_blocks[e];
      if (!nyr_cfg_is_backedge(&cfg, latch, header) ||
          !nyr_cfg_natural_loop_blocks(&cfg, latch, header, in_loop,
                                       cfg.block_count))
        continue;
      for (size_t block = 0; block < cfg.block_count; ++block) {
        if (!in_loop[block])
          continue;
        for (size_t i = cfg.block_start[block]; i < cfg.block_end[block]; ++i) {
          nyr_inst_t *in = &f->data[i];
          if (in->op != NYIR_CMP_I64 || in->cmp == NYIR_CMP_EQ ||
              in->cmp == NYIR_CMP_NE)
            continue;
          int left = versioning_root_copy(f, defs, in->a);
          int right = versioning_root_copy(f, defs, in->b);
          if (left < 0 || right < 0 || defs[left] < 0 || defs[right] < 0 ||
              in_loop[cfg.inst_block[(size_t)defs[left]]] ||
              in_loop[cfg.inst_block[(size_t)defs[right]]] ||
              f->data[defs[right]].op != NYIR_CONST_I64)
            continue;
          int tmp = in->a;
          in->a = in->b;
          in->b = tmp;
          in->cmp = nyr_cmp_swap_operands(in->cmp);
          free(in_loop);
          free(defs);
          nyr_cfg_free(&cfg);
          return true;
        }
      }
    }
  }
  free(in_loop);
  free(defs);
  nyr_cfg_free(&cfg);
  return true;
}

/*
 * Counted zero-store loop -> memset.
 *
 * LLVM reference: lib/Transforms/Scalar/LoopIdiomRecognize.cpp (the memset
 * form). Reimplemented for NYIR per the opt/pipeline.c reuse rule: cite
 * upstream, reimplement, keep verify/rollback/oracle green.
 *
 * Bench kernels and top-level scripts keep their induction variable in a
 * named slot (`mut i` lowered to address-taken memory with per-iteration
 * loads and stores), which SCEV-lite does not model, so this pass matches
 * the memory-induction shape directly:
 *
 *   preheader: ... st slot, 0                      (last slot store)
 *   header:    iv = ld slot; cmp = iv < LIMIT; br.if cmp -> body
 *   body:      *(base + iv*ELEM) = 0 ... ; st slot, iv + 1 ; br header
 *
 * and replaces the whole loop with rt_memset(base, 0, LIMIT*ELEM) in the
 * preheader. The rewrite is fail-closed: a second exit, an extra loop
 * predecessor, any other store to the slot, any load of the slot outside
 * the loop (deleting the loop changes its final value), a loop-defined
 * value escaping the loop, or any unrecognized instruction cancels the
 * candidate. The loop's per-iteration bounds check is preserved as one
 * check of the last accessed offset, so an out-of-range loop still traps
 * exactly when the original loop's final iteration would have.
 */
typedef struct {
  size_t latch_block;  /* natural-loop latch for block-set rebuild */
  size_t header_block; /* natural-loop header */
  size_t br_pos;       /* preheader terminator index (retargeted) */
  int base_value;      /* loop-invariant base pointer */
  int64_t limit;       /* const trip count; stores span [0, limit) */
  int64_t elem;        /* store width in bytes: 1 or 8 */
  int64_t exit_label;  /* not-taken header successor */
  bool explicit_br;    /* preheader ends in BR (vs falling through) */
} nir_loop_memset_cand_t;

/*
 * Two address values name the same slot when their roots carry the same
 * symbol (ADDR_SYMBOL) or the same local frame slot (ADDR_LOCAL).
 */
static bool idiom_memset_same_slot(const nyr_func_t *f, const int *defs, int a,
                                   int b) {
  a = idiom_root_copy(f, defs, a);
  b = idiom_root_copy(f, defs, b);
  if (a < 0 || b < 0 || a >= f->next_value || b >= f->next_value)
    return false;
  if (a == b)
    return true;
  if (defs[a] < 0 || defs[b] < 0)
    return false;
  const nyr_inst_t *da = &f->data[defs[a]];
  const nyr_inst_t *db = &f->data[defs[b]];
  if (da->op != db->op)
    return false;
  if (da->op == NYIR_ADDR_SYMBOL)
    return da->symbol && db->symbol && strcmp(da->symbol, db->symbol) == 0;
  if (da->op == NYIR_ADDR_LOCAL)
    return da->imm == db->imm;
  return false;
}

/*
 * `value` must be a load of the induction slot named by `slot`.
 */
static bool idiom_memset_is_slot_load(const nyr_func_t *f, const int *defs,
                                      int value, int slot) {
  value = idiom_root_copy(f, defs, value);
  if (value < 0 || value >= f->next_value || defs[value] < 0)
    return false;
  const nyr_inst_t *ld = &f->data[defs[value]];
  if (ld->op != NYIR_LOAD_I64)
    return false;
  return idiom_memset_same_slot(f, defs, ld->a, slot);
}

/*
 * Match `off` against exactly `iv * elem` (or `iv` itself when elem == 1),
 * where `iv` is any load of the induction slot.
 */
static bool idiom_memset_offset_scale(const nyr_func_t *f, const int *defs,
                                      int value, int slot, int64_t elem) {
  int root = idiom_root_copy(f, defs, value);
  if (root < 0 || root >= f->next_value || defs[root] < 0)
    return false;
  const nyr_inst_t *def = &f->data[defs[root]];
  if (def->op == NYIR_LOAD_I64)
    return elem == 1 && idiom_memset_same_slot(f, defs, def->a, slot);
  int64_t factor = 0;
  if (def->op == NYIR_MUL_I64) {
    if (idiom_memset_is_slot_load(f, defs, def->a, slot) &&
        const_i64_val(f, defs, def->b, &factor))
      return factor == elem;
    if (idiom_memset_is_slot_load(f, defs, def->b, slot) &&
        const_i64_val(f, defs, def->a, &factor))
      return factor == elem;
    return false;
  }
  if (def->op == NYIR_SHL_I64) {
    if (!idiom_memset_is_slot_load(f, defs, def->a, slot))
      return false;
    if (!const_i64_val(f, defs, def->b, &factor) || factor < 0 || factor > 62)
      return false;
    return (INT64_C(1) << factor) == elem;
  }
  return false;
}

/*
 * The store at `store_pos` must be `*(base + iv*elem) = 0` with a
 * loop-invariant base. Returns false when the shape does not match exactly.
 */
static bool idiom_memset_match_store(const nyr_func_t *f, const int *defs,
                                     const nyr_cfg_t *cfg, const bool *in_loop,
                                     int slot, int64_t limit, size_t store_pos,
                                     int *base_out, int64_t *elem_out,
                                     int *off_out) {
  const nyr_inst_t *st = &f->data[store_pos];
  int64_t value = 0;
  if (!const_i64_val(f, defs, st->c, &value) || value != 0)
    return false;
  int64_t elem = (st->flags & NYIR_INST_F_MEM_BYTE) ? 1 : 8;

  int addr = idiom_root_copy(f, defs, st->a);
  if (addr < 0 || addr >= f->next_value || defs[addr] < 0)
    return false;
  const nyr_inst_t *add = &f->data[defs[addr]];
  if (add->op != NYIR_ADD_I64)
    return false;
  /*
   * One operand is the base pointer, the other the `iv * elem` offset.
   * The base is whichever side does not derive from the induction slot.
   */
  int off_value = -1, base_value = -1;
  bool a_is_offset = idiom_memset_offset_scale(f, defs, add->a, slot, elem);
  bool b_is_offset = idiom_memset_offset_scale(f, defs, add->b, slot, elem);
  if (a_is_offset == b_is_offset)
    return false;
  if (b_is_offset) {
    off_value = add->b;
    base_value = add->a;
  } else {
    off_value = add->a;
    base_value = add->b;
  }
  int base = idiom_root_copy(f, defs, base_value);
  if (base < 0 || base >= f->next_value || defs[base] < 0)
    return false;
  /*
   * Require a fresh, sufficiently large allocation to rule out aliases.
   */
  const nyr_inst_t *alloca = &f->data[defs[base]];
  if (alloca->op != NYIR_ALLOCA || in_loop[cfg->inst_block[defs[base]]] ||
      alloca->imm < 0 ||
      (uint64_t)alloca->imm < (uint64_t)limit * (uint64_t)elem)
    return false;
  *base_out = base;
  *elem_out = elem;
  *off_out = off_value;
  return true;
}

/*
 * The deleted loop leaves the induction slot at `limit`. It is safe to
 * remove only if the slot address is used exclusively by its initializer,
 * induction loads, and increment store.
 */
static bool idiom_memset_slot_private(const nyr_func_t *f, const nyr_cfg_t *cfg,
                                      const bool *in_loop, const int *defs,
                                      int slot, size_t preheader,
                                      size_t incr_pos) {
  size_t init_stores = 0;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    bool loop = in_loop[cfg->inst_block[i]];
    bool slot_load =
        in->op == NYIR_LOAD_I64 && idiom_memset_same_slot(f, defs, in->a, slot);
    bool slot_store = in->op == NYIR_STORE_I64 &&
                      idiom_memset_same_slot(f, defs, in->a, slot);
    if (slot_load) {
      if (!loop)
        return false;
    } else if (slot_store) {
      if (i == incr_pos && loop) {
        /*
         * expected induction update
         */
      } else if (!loop && cfg->inst_block[i] == preheader) {
        int64_t value = 0;
        if (!const_i64_val(f, defs, in->c, &value) || value != 0 ||
            ++init_stores != 1)
          return false;
      } else {
        return false;
      }
    }

    int operands[6] = {in->a, in->b, in->c, in->d, in->e, in->f};
    size_t operand_count = sizeof(operands) / sizeof(operands[0]);
    for (size_t k = 0; k < operand_count; ++k) {
      if (k == 0 && (slot_load || slot_store))
        continue;
      int value = operands[k];
      if (value >= 0 && value < f->next_value &&
          idiom_memset_same_slot(f, defs, value, slot))
        return false;
    }
    for (size_t k = 0; k < in->extra_args_len; ++k)
      if (idiom_memset_same_slot(f, defs, in->extra_args[k], slot))
        return false;
    for (size_t k = 0; k < in->phi_incoming_len; ++k)
      if (idiom_memset_same_slot(f, defs, in->phi_incoming[k].value, slot))
        return false;
  }
  return init_stores == 1;
}

/*
 * No instruction outside the loop may reference a value defined inside it:
 * the entire loop is removed, so every such use would go stale.
 */
static bool idiom_memset_no_escape(const nyr_func_t *f, const nyr_cfg_t *cfg,
                                   const bool *in_loop, const int *defs) {
  for (size_t i = 0; i < f->len; ++i) {
    if (in_loop[cfg->inst_block[i]])
      continue;
    const nyr_inst_t *in = &f->data[i];
    int operands[6] = {in->a, in->b, in->c, in->d, in->e, in->f};
    for (size_t k = 0; k < 6; ++k) {
      int v = operands[k];
      if (v >= 0 && v < f->next_value && defs[v] >= 0 &&
          in_loop[cfg->inst_block[defs[v]]])
        return false;
    }
    for (size_t k = 0; k < in->extra_args_len; ++k) {
      int v = in->extra_args[k];
      if (v >= 0 && v < f->next_value && defs[v] >= 0 &&
          in_loop[cfg->inst_block[defs[v]]])
        return false;
    }
    for (size_t k = 0; k < in->phi_incoming_len; ++k) {
      int v = in->phi_incoming[k].value;
      if (v >= 0 && v < f->next_value && defs[v] >= 0 &&
          in_loop[cfg->inst_block[defs[v]]])
        return false;
    }
  }
  return true;
}

/*
 * Match the memory-induction counted zero-store loop formed by the natural
 * loop of backedge latch -> header. Everything is matched against the input
 * function; a failed match leaves the IR untouched.
 */
static bool idiom_memset_trace_enabled(void) {
  static int cached = -1;
  if (cached < 0)
    cached = getenv("NY_LOOP_MEMSET_DEBUG") != NULL;
  return cached;
}

static int64_t idiom_memset_exit_label(const nyr_func_t *f,
                                       const nyr_cfg_t *cfg, size_t block) {
  if (!f || !cfg || block >= cfg->block_count)
    return -1;
  if (cfg->block_label[block] >= 0)
    return cfg->block_label[block];
  int64_t target = -1;
  for (size_t i = cfg->block_start[block]; i < cfg->block_end[block]; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op == NYIR_LABEL || in->op == NYIR_NOP)
      continue;
    if (in->op != NYIR_BR || target >= 0)
      return -1;
    target = in->imm;
  }
  return target;
}

static bool idiom_memset_candidate(const nyr_func_t *f, const int *defs,
                                   const nyr_cfg_t *cfg, size_t latch,
                                   size_t header, nir_loop_memset_cand_t *out) {
  bool *in_loop = calloc(cfg->block_count, sizeof(*in_loop));
  if (!in_loop)
    return false;
  bool ok = true;
  const char *why = "ok";
  int slot = -1;      /* an address value naming the induction slot */
  int iv_load = -1;   /* the header's load of the slot */
  int cmp_value = -1; /* the header compare result feeding br.if */
  int base = -1;
  int off_value = -1;
  int64_t elem = 0, limit = 0;
  size_t store_pos = SIZE_MAX, store_block = SIZE_MAX;
  size_t incr_pos = SIZE_MAX, incr_block = SIZE_MAX;
  size_t br_if_count = 0;
  size_t outside_br_count = 0;
  int64_t exit_label = -1;

  do {
    if (!nyr_cfg_natural_loop_blocks(cfg, latch, header, in_loop,
                                     cfg->block_count)) {
      ok = false;
      why = "line 221";
      break;
    }
    if (idiom_memset_trace_enabled()) {
      for (size_t b = 0; b < cfg->block_count; ++b) {
        fprintf(stderr,
                "[loop-memset] B%zu label=%lld range=%zu..%zu loop=%d succ=", b,
                (long long)cfg->block_label[b], cfg->block_start[b],
                cfg->block_end[b], in_loop[b]);
        for (size_t e = cfg->succ_offsets[b]; e < cfg->succ_offsets[b + 1]; ++e)
          fprintf(stderr, "%s%zu", e == cfg->succ_offsets[b] ? "" : ",",
                  cfg->succ_blocks[e]);
        fputc('\n', stderr);
      }
    }

    /*
     * Exactly one outside predecessor, and it ends in a branch to header.
     */
    size_t outside_preds = 0;
    size_t the_pred = SIZE_MAX;
    for (size_t e = cfg->pred_offsets[header];
         e < cfg->pred_offsets[header + 1]; ++e) {
      size_t p = cfg->pred_blocks[e];
      if (in_loop[p])
        continue;
      ++outside_preds;
      the_pred = p;
    }
    if (outside_preds != 1 || the_pred == SIZE_MAX) {
      ok = false;
      why = "line 237";
      break;
    }
    if (cfg->block_end[the_pred] == cfg->block_start[the_pred]) {
      ok = false;
      why = "line 241";
      break;
    }
    size_t br_pos = cfg->block_end[the_pred] - 1;
    const nyr_inst_t *term = &f->data[br_pos];
    bool explicit_br;
    if (term->op == NYIR_BR && term->imm == cfg->block_label[header]) {
      explicit_br = true;
    } else if (term->op != NYIR_BR && term->op != NYIR_BR_IF &&
               cfg->block_start[header] == cfg->block_end[the_pred]) {
      /*
       * Fallthrough shape: the preheader drops into the header with no
       * terminator. The rewrite appends an exit branch after the memset
       * call instead of retargeting one.
       */
      explicit_br = false;
      br_pos = cfg->block_end[the_pred];
    } else {
      ok = false;
      why = "preheader terminator";
      break;
    }

    /*
     * Walk the whole loop. Allowed: pure address/IV arithmetic, the header's
     * slot load and compare, one zero store, its bounds check, and the
     * increment store. Everything else cancels the candidate.
     */
    for (size_t b = 0; b < cfg->block_count && ok; ++b) {
      if (!in_loop[b])
        continue;
      for (size_t i = cfg->block_start[b]; i < cfg->block_end[b] && ok; ++i) {
        const nyr_inst_t *in = &f->data[i];
        switch (in->op) {
        case NYIR_LABEL:
        case NYIR_PHI:
        case NYIR_CONST_I64:
        case NYIR_COPY:
        case NYIR_ADD_I64:
        case NYIR_SUB_I64:
        case NYIR_MUL_I64:
        case NYIR_SHL_I64:
          break;
        case NYIR_LOAD_I64:
          if (slot < 0 && b == header && in->dst >= 0) {
            /*
             * Adopt the first header load as the induction slot.
             */
            slot = idiom_root_copy(f, defs, in->a);
            iv_load = in->dst;
            if (slot < 0)
              ok = false;
          } else if (slot >= 0 && in->dst >= 0 &&
                     idiom_memset_is_slot_load(f, defs, in->dst, slot)) {
            /*
             * Additional pre-increment loads of the slot are fine; the
             * position rules below pin the ones that matter.
             */
          } else {
            ok = false;
          }
          break;
        case NYIR_CMP_I64:
          if (b == header && cmp_value < 0 && in->dst >= 0 &&
              in->cmp == NYIR_CMP_LT &&
              idiom_memset_is_slot_load(f, defs, in->a, slot) &&
              const_i64_val(f, defs, in->b, &limit) && limit > 0 &&
              limit <= INT64_MAX / 8) {
            cmp_value = in->dst;
          } else {
            ok = false;
          }
          break;
        case NYIR_BR_IF:
          if (b != header || ++br_if_count > 1 ||
              idiom_root_copy(f, defs, in->a) != cmp_value) {
            ok = false;
            why = "line 300";
            break;
          }
          {
            bool inside = false;
            for (size_t t = 0; t < cfg->block_count; ++t) {
              if (in_loop[t] && cfg->block_label[t] == in->imm) {
                inside = true;
                break;
              }
            }
            if (!inside)
              ok = false;
          }
          break;
        case NYIR_BR: {
          bool inside = false;
          for (size_t t = 0; t < cfg->block_count; ++t) {
            if (in_loop[t] && cfg->block_label[t] == in->imm) {
              inside = true;
              break;
            }
          }
          if (inside)
            break;
          if (b != header || ++outside_br_count > 1) {
            ok = false;
            why = "line 326";
            break;
          }
          exit_label = in->imm;
          break;
        }
        case NYIR_BOUNDS_CHECK:
          /*
           * Replacing per-iteration stores with a prevalidated bulk write
           * changes partial-write behavior if a later iteration traps.
           */
          ok = false;
          why = "bounds-checked loop is not eligible";
          break;
        case NYIR_STORE_I64:
          /*
           * Exactly two stores: the slot increment and the zero store. The
           * increment stores to the slot itself; any other store is the
           * zero store.
           */
          if (idiom_memset_same_slot(f, defs, in->a, slot)) {
            if (incr_pos != SIZE_MAX) {
              ok = false;
              why = "line 346";
              break;
            }
            incr_pos = i;
            incr_block = b;
          } else if (store_pos != SIZE_MAX) {
            ok = false;
          } else {
            store_pos = i;
            store_block = b;
          }
          break;
        default:
          ok = false;
          why = "line 359";
          break;
        }
      }
    }
    if (!ok)
      break;
    if (store_pos == SIZE_MAX || incr_pos == SIZE_MAX || cmp_value < 0 ||
        iv_load < 0 || outside_br_count > 1) {
      ok = false;
      why = "incomplete counted-loop shape";
      if (idiom_memset_trace_enabled())
        fprintf(stderr,
                "[loop-memset] shape store=%zu increment=%zu compare=%d "
                "iv-load=%d exit-branches=%zu exit-label=%lld\n",
                store_pos, incr_pos, cmp_value, iv_load, outside_br_count,
                (long long)exit_label);
      break;
    }
    if (outside_br_count == 0) {
      size_t outside_succ = SIZE_MAX;
      for (size_t e = cfg->succ_offsets[header];
           e < cfg->succ_offsets[header + 1]; ++e) {
        size_t succ = cfg->succ_blocks[e];
        if (in_loop[succ])
          continue;
        if (outside_succ != SIZE_MAX) {
          ok = false;
          why = "header has multiple loop exits";
          break;
        }
        outside_succ = succ;
      }
      if (!ok)
        break;
      if (outside_succ == SIZE_MAX ||
          (exit_label = idiom_memset_exit_label(f, cfg, outside_succ)) < 0) {
        ok = false;
        why = "cannot resolve header exit";
        break;
      }
    } else if (exit_label < 0) {
      ok = false;
      why = "missing direct loop exit";
      break;
    }
    /*
     * Ordering: the zero store and the increment share one block and the
     * zero store comes first, so every stored offset is the pre-increment
     * iteration value.
     */
    if (store_block != incr_block || store_pos > incr_pos) {
      ok = false;
      why = "line 377";
      break;
    }
    /*
     * The increment must be `st slot, slot_load + 1` with step exactly 1,
     * consuming a load that runs before it (the header load, or a body load
     * above the increment in the same block).
     */
    {
      int inc_src = idiom_root_copy(f, defs, f->data[incr_pos].c);
      if (inc_src < 0 || inc_src >= f->next_value || defs[inc_src] < 0) {
        ok = false;
        why = "line 388";
        break;
      }
      const nyr_inst_t *add = &f->data[defs[inc_src]];
      if (add->op != NYIR_ADD_I64) {
        ok = false;
        why = "line 393";
        break;
      }
      int64_t step = 0;
      int iv_side = -1;
      if (const_i64_val(f, defs, add->b, &step) && step == 1)
        iv_side = add->a;
      else if (const_i64_val(f, defs, add->a, &step) && step == 1)
        iv_side = add->b;
      else {
        ok = false;
        why = "line 403";
        break;
      }
      if (!idiom_memset_is_slot_load(f, defs, iv_side, slot)) {
        ok = false;
        why = "line 407";
        break;
      }
      int load_def = defs[idiom_root_copy(f, defs, iv_side)];
      size_t load_block = cfg->inst_block[load_def];
      if (load_block != header &&
          (load_block != incr_block || (size_t)load_def > incr_pos)) {
        ok = false;
        why = "line 414";
        break;
      }
    }
    if (!idiom_memset_match_store(f, defs, cfg, in_loop, slot, limit, store_pos,
                                  &base, &elem, &off_value)) {
      ok = false;
      why = "line 420";
      break;
    }
    /*
     * The induction slot must be initialized to zero in the preheader: the
     * preheader is straight-line, so its last slot store dominates the loop
     * and feeds the first iteration's offset.
     */
    {
      int init_pos = -1;
      bool saw_branch = false;
      for (size_t i = cfg->block_start[the_pred]; i < cfg->block_end[the_pred];
           ++i) {
        const nyr_inst_t *in = &f->data[i];
        if (in->op == NYIR_BR_IF) {
          saw_branch = true;
          break;
        }
        if (in->op == NYIR_STORE_I64 &&
            idiom_memset_same_slot(f, defs, in->a, slot))
          init_pos = (int)i;
      }
      int64_t init_value = 0;
      if (saw_branch || init_pos < 0 ||
          !const_i64_val(f, defs, f->data[init_pos].c, &init_value) ||
          init_value != 0) {
        ok = false;
        why = "no dominating zero init in preheader";
        break;
      }
    }
    if (!idiom_memset_slot_private(f, cfg, in_loop, defs, slot, the_pred,
                                   incr_pos)) {
      ok = false;
      why = "induction slot address escapes";
      break;
    }
    /*
     * The zero store's offset load must also read the pre-increment value:
     * it is either the header load or a body load above the zero store.
     */
    int off_root = -1;
    {
      off_root = idiom_root_copy(f, defs, off_value);
      if (off_root < 0 || off_root >= f->next_value || defs[off_root] < 0) {
        ok = false;
        why = "line 432";
        break;
      }
      const nyr_inst_t *off_def = &f->data[defs[off_root]];
      int load_value = -1;
      if (off_def->op == NYIR_LOAD_I64)
        load_value = off_root;
      else if (off_def->op == NYIR_MUL_I64 &&
               idiom_memset_is_slot_load(f, defs, off_def->a, slot))
        load_value = off_def->a;
      else if (off_def->op == NYIR_MUL_I64 &&
               idiom_memset_is_slot_load(f, defs, off_def->b, slot))
        load_value = off_def->b;
      else if (off_def->op == NYIR_SHL_I64 &&
               idiom_memset_is_slot_load(f, defs, off_def->a, slot))
        load_value = off_def->a;
      if (load_value < 0) {
        ok = false;
        why = "line 449";
        break;
      }
      int load_def = defs[idiom_root_copy(f, defs, load_value)];
      if (load_def < 0) {
        ok = false;
        why = "line 454";
        break;
      }
      size_t load_block = cfg->inst_block[load_def];
      if (load_block != header &&
          (load_block != store_block || (size_t)load_def > store_pos)) {
        ok = false;
        why = "line 460";
        break;
      }
    }
    if (!idiom_memset_no_escape(f, cfg, in_loop, defs)) {
      ok = false;
      why = "line 481";
      break;
    }
    /*
     * Deleting the loop changes the slot's final value from `limit` back to
     * the pre-loop 0. Any load of the slot outside the loop could observe
     * that difference, so none may exist.
     */
    for (size_t i = 0; i < f->len && ok; ++i) {
      if (in_loop[cfg->inst_block[i]])
        continue;
      const nyr_inst_t *in = &f->data[i];
      if (in->op == NYIR_LOAD_I64 &&
          idiom_memset_same_slot(f, defs, in->a, slot))
        ok = false;
    }
    if (!ok)
      break;

    *out = (nir_loop_memset_cand_t){.latch_block = latch,
                                    .header_block = header,
                                    .br_pos = br_pos,
                                    .base_value = base,
                                    .limit = limit,
                                    .elem = elem,
                                    .exit_label = exit_label,
                                    .explicit_br = explicit_br};
  } while (false);
  if (!ok && idiom_memset_trace_enabled())
    fprintf(stderr, "[loop-memset] reject h=%zu latch=%zu: %s\n", header, latch,
            why);
  free(in_loop);
  return ok;
}

static bool idiom_memset_apply(nyr_func_t *f,
                               const nir_loop_memset_cand_t *cand,
                               const nyr_cfg_t *cfg) {
  bool *in_loop = calloc(cfg->block_count, sizeof(*in_loop));
  if (!in_loop)
    return false;
  if (!nyr_cfg_natural_loop_blocks(cfg, cand->latch_block, cand->header_block,
                                   in_loop, cfg->block_count)) {
    free(in_loop);
    return false;
  }
  size_t count = 3;
  if (!cand->explicit_br)
    count += 1; /* appended exit branch */
  if (f->next_value > INT_MAX - 4 || !nir_ensure_inst_space(f, count)) {
    free(in_loop);
    return false;
  }
  int zero_v = f->next_value++;
  int bytes_v = f->next_value++;

  size_t at = cand->br_pos;
  memmove(&f->data[at + count], &f->data[at], (f->len - at) * sizeof(*f->data));
  f->len += (size_t)count;

  size_t w = at;
  f->data[w++] = (nyr_inst_t){.op = NYIR_CONST_I64,
                              .dst = zero_v,
                              .a = -1,
                              .b = -1,
                              .c = -1,
                              .d = -1,
                              .e = -1,
                              .f = -1,
                              .imm = 0};
  f->data[w++] = (nyr_inst_t){.op = NYIR_CONST_I64,
                              .dst = bytes_v,
                              .a = -1,
                              .b = -1,
                              .c = -1,
                              .d = -1,
                              .e = -1,
                              .f = -1,
                              .imm = cand->limit * cand->elem};
  f->data[w++] = (nyr_inst_t){.op = NYIR_CALL,
                              .dst = -1,
                              .a = cand->base_value,
                              .b = zero_v,
                              .c = bytes_v,
                              .d = -1,
                              .e = -1,
                              .f = -1,
                              .imm = 3,
                              .flags = NYIR_INST_F_EXTERN,
                              .symbol = "rt_memset_raw"};
  f->has_symbol_calls = true;
  for (size_t k = at; k < at + count; ++k)
    f->data[k].effects = nyr_inst_effects(&f->data[k]);

  if (cand->explicit_br) {
    /*
     * Retarget the preheader terminator past the deleted loop.
     */
    f->data[at + count].imm = cand->exit_label;
  } else {
    /*
     * Fallthrough shape: the deleted loop's label follows the inserted
     * sequence, so terminate the preheader with an explicit branch to the
     * exit block instead.
     */
    f->data[at + count] = (nyr_inst_t){.op = NYIR_BR,
                                       .dst = -1,
                                       .a = -1,
                                       .b = -1,
                                       .c = -1,
                                       .d = -1,
                                       .e = -1,
                                       .f = -1,
                                       .imm = cand->exit_label,
                                       .effects = NYIR_EFFECT_CONTROL};
  }

  /*
   * Tombstone the loop; a later compaction reclaims the NOPs.
   */
  for (size_t b = 0; b < cfg->block_count; ++b) {
    if (!in_loop[b])
      continue;
    for (size_t i = cfg->block_start[b]; i < cfg->block_end[b]; ++i)
      nyr_erase_instruction(f, i + count);
  }
  free(in_loop);
  return true;
}

bool nyr_loop_memset(nyr_func_t *f) {
  if (!f || f->len == 0 || f->next_value <= 0)
    return true;
  nyr_cfg_t cfg = {0};
  int *defs = nyr_build_defs(f);
  if (!defs || !nyr_cfg_build(f, &cfg)) {
    free(defs);
    nyr_cfg_free(&cfg);
    return false;
  }
  nir_loop_memset_cand_t *cands = NULL;
  size_t cand_count = 0, cand_cap = 0;
  bool failed = false;
  /*
   * Candidates are matched against the input CFG, then applied in descending
   * preheader position so earlier insertions never shift a pending
   * candidate's instruction indices. Candidate regions are disjoint: a loop
   * containing another zero-store loop has two non-slot stores and never
   * matches twice.
   */
  for (size_t h = 0; h < cfg.block_count && !failed; ++h) {
    for (size_t pi = cfg.pred_offsets[h];
         pi < cfg.pred_offsets[h + 1] && !failed; ++pi) {
      size_t p = cfg.pred_blocks[pi];
      if (!nyr_cfg_is_backedge(&cfg, p, h))
        continue;
      nir_loop_memset_cand_t cand;
      if (!idiom_memset_candidate(f, defs, &cfg, p, h, &cand))
        continue;
      if (cand_count == cand_cap) {
        size_t new_cap = cand_cap ? cand_cap * 2 : 8;
        nir_loop_memset_cand_t *grown =
            realloc(cands, new_cap * sizeof(*grown));
        if (!grown) {
          failed = true;
          break;
        }
        cands = grown;
        cand_cap = new_cap;
      }
      cands[cand_count++] = cand;
    }
  }
  for (size_t ci = cand_count; ci > 0 && !failed; --ci) {
    if (!idiom_memset_apply(f, &cands[ci - 1], &cfg))
      failed = true;
  }
  free(cands);
  free(defs);
  nyr_cfg_free(&cfg);
  return !failed;
}
