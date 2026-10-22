/*
 * x86-64 native encoder: NYIR and machine-form lowering to object-code.
 * Source AST fallback emission lives in the language pipeline.
 */
#include "base/debug.h"
#include "base/trace.h"
#include "code/backend/internal.h"
#include "code/backend/ir/opt/util.h"
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

/*
 * NYIR -> x86-64 instruction selection
 *
 * The NYIR is already optimized (constant-folded, copy-propagated,
 * DCE'd).  We map each live NYIR value to a dedicated stack slot so
 * that multi-use values survive across instruction boundaries.  The
 * optimizer has already removed most redundant slots.
 *
 * When one operand of a binop is CONST_I64 with a 32-bit immediate,
 * we emit the compact immediate form (e.g. addq $imm, %rax) instead
 * of the full load-load-operate-store sequence.
 */

#define NY_X64_NIR_MAX_SLOTS 16384
#define NY_X64_LOOP_REG_COUNT 5
#define NY_X64_VEC_CACHE_REGS 4

typedef struct {
  nyr_writer_t *w;
  const ny_native_target_info_t *target;
  const nyr_func_t *nyir;
  int slot_offset[NY_X64_NIR_MAX_SLOTS];
  int value_home_index[NY_X64_NIR_MAX_SLOTS];
  int value_home_root[NY_X64_NIR_MAX_SLOTS];
  /*
   * Selected scalar homes stay resident in callee-saved GPRs.
   */
  int home_reg[NY_X64_NIR_MAX_SLOTS];
  int home_reg_count;
  /*
   * Number of machine words reserved for value homes.
   */
  int value_home_count;
  /*
   * RAX may retain a value whose stack home is still current.
   */
  int rax_cached_slot;
  int rax_store_slot;
  /*
   * Block-local AVX vector values cached in caller-saved YMM registers.
   */
  int vec_cached_value[NY_X64_VEC_CACHE_REGS];
  bool vec_cache_dirty[NY_X64_VEC_CACHE_REGS];
  uint64_t vec_cache_age[NY_X64_VEC_CACHE_REGS];
  uint64_t vec_cache_clock;
  int frame_slots;
  int frame_bytes;
  int max_local_slot;
  bool value_f64[NY_X64_NIR_MAX_SLOTS];
  bool value_f32[NY_X64_NIR_MAX_SLOTS];
  bool value_i64v2[NY_X64_NIR_MAX_SLOTS];
  bool local_f64[NY_X64_NIR_MAX_SLOTS];
  bool local_f32[NY_X64_NIR_MAX_SLOTS];
  /*
   * valmap[i] = index of the NYIR instruction that defines value i,
   * or -1 if not defined.  Used for immediate-operand detection.
   */
  int def_index[NY_X64_NIR_MAX_SLOTS];
  int def_count[NY_X64_NIR_MAX_SLOTS];
  /*
   * State 1 means every definition of this post-PHI value is the same
   * literal CONST.  State 2 means at least one definition is not.
   */
  unsigned char uniform_const_state[NY_X64_NIR_MAX_SLOTS];
  int64_t uniform_const_value[NY_X64_NIR_MAX_SLOTS];
  int use_count[NY_X64_NIR_MAX_SLOTS];
  /*
   * Folded `base + (index << 3)` addresses for typed 64-bit buffer accesses.
   */
  int indexed_base[NY_X64_NIR_MAX_SLOTS];
  int indexed_index[NY_X64_NIR_MAX_SLOTS];
  bool folded_addr_temp[NY_X64_NIR_MAX_SLOTS];
  bool fused_cmp[NY_X64_NIR_MAX_SLOTS];
  bool fused_select_cmp[NY_X64_NIR_MAX_SLOTS];
  bool fused_sign_bias[NY_X64_NIR_MAX_SLOTS];
  bool elided_sign_shift[NY_X64_NIR_MAX_SLOTS];
  bool elided_mask_test[NY_X64_NIR_MAX_SLOTS];
  int fused_mask_test_src[NY_X64_NIR_MAX_SLOTS];
  bool fused_const[NY_X64_NIR_MAX_SLOTS];
  int64_t fused_imm[NY_X64_NIR_MAX_SLOTS];
  bool use_avx_vector;
  bool is_leaf;
  const char *label_prefix;
  char epilogue_label[128];
  char *err;
  size_t err_len;
} ny_x64_nir_ctx_t;

static int ny_x64_nir_slot(ny_x64_nir_ctx_t *c, int value_id) {
  if (value_id < 0 || value_id >= c->nyir->next_value ||
      value_id >= NY_X64_NIR_MAX_SLOTS)
    return -1;
  int home = c->value_home_index[value_id];
  if (home < 0)
    return -1;
  int s = c->frame_slots + home;
  bool vector_home = c->value_f64[value_id] || c->value_f32[value_id] ||
                     c->value_i64v2[value_id];
  if (s < 0 || s + (vector_home ? 3 : 0) >= NY_X64_NIR_MAX_SLOTS)
    return -1;
  /*
   * Keep vector lanes in contiguous words in increasing address order.
   */
  for (int lane = 0; lane < (vector_home ? 4 : 1); ++lane)
    c->slot_offset[s + lane] = (s + 4 - lane) * 8;
  return s;
}

static bool ny_x64_nir_inst_uses_value(const nyr_inst_t *in, int value_id) {
  const int args[] = {in->a, in->b, in->c, in->d, in->e, in->f};
  for (size_t i = 0; i < sizeof(args) / sizeof(args[0]); ++i) {
    if (args[i] == value_id)
      return true;
  }
  if (in->op == NYIR_CALL) {
    for (size_t i = 0; i < in->extra_args_len; ++i) {
      if (in->extra_args[i] == value_id)
        return true;
    }
  }
  return false;
}

static bool ny_x64_nir_try_const_i32(ny_x64_nir_ctx_t *c, int value_id,
                                     size_t use_idx, int64_t *out);

static void ny_x64_nir_compute_frame(ny_x64_nir_ctx_t *c) {
  int max_val = c->nyir->next_value;
  int max_local = c->nyir->param_count > 0 ? (int)c->nyir->param_count - 1 : -1;
  for (int v = 0; v < max_val && v < NY_X64_NIR_MAX_SLOTS; ++v) {
    c->def_index[v] = -1;
    c->def_count[v] = 0;
    c->uniform_const_state[v] = 0;
    c->uniform_const_value[v] = 0;
    c->use_count[v] = 0;
    c->indexed_base[v] = -1;
    c->indexed_index[v] = -1;
    c->folded_addr_temp[v] = false;
    c->fused_sign_bias[v] = false;
    c->elided_sign_shift[v] = false;
    c->elided_mask_test[v] = false;
    c->fused_mask_test_src[v] = -1;
    c->value_home_index[v] = -1;
    c->value_home_root[v] = -1;
  }
  for (size_t i = 0; i < c->nyir->len; ++i) {
    const nyr_inst_t *in = &c->nyir->data[i];
    bool i64_vector_result =
        in->op == NYIR_VEC4_LOAD_I64 || in->op == NYIR_VEC4_SET1_I64 ||
        in->op == NYIR_VEC4_ADD_I64 || in->op == NYIR_VEC4_SUB_I64 ||
        in->op == NYIR_VEC4_AND_I64 || in->op == NYIR_VEC4_OR_I64 ||
        in->op == NYIR_VEC4_XOR_I64 || in->op == NYIR_VEC4_SHL_I64 ||
        in->op == NYIR_VEC4_SAR_I64;
    if (i64_vector_result && in->dst >= 0 && in->dst < NY_X64_NIR_MAX_SLOTS)
      c->value_i64v2[in->dst] = true;
    if ((in->op == NYIR_VEC4_REDUCE_ADD_I64 ||
         in->op == NYIR_VEC8_REDUCE_ADD_I64) &&
        in->b >= 0 && in->b < NY_X64_NIR_MAX_SLOTS)
      c->value_i64v2[in->b] = true;
    if ((in->op == NYIR_LOAD_LOCAL || in->op == NYIR_STORE_LOCAL) &&
        (int)in->imm > max_local)
      max_local = (int)in->imm;
    if (in->dst >= 0 && in->dst < NY_X64_NIR_MAX_SLOTS) {
      int dst = in->dst;
      if (c->def_count[dst] == 0) {
        if (in->op == NYIR_CONST_I64) {
          c->uniform_const_state[dst] = 1;
          c->uniform_const_value[dst] = in->imm;
        } else {
          c->uniform_const_state[dst] = 2;
        }
      } else if (c->uniform_const_state[dst] != 1 || in->op != NYIR_CONST_I64 ||
                 c->uniform_const_value[dst] != in->imm) {
        c->uniform_const_state[dst] = 2;
      }
      c->def_index[in->dst] = (int)i;
      c->def_count[in->dst]++;
    }
    /*
     * Count uses of input values for next-use fusion.
     */
    const int args[] = {in->a, in->b, in->c, in->d, in->e, in->f};
    for (size_t j = 0; j < sizeof(args) / sizeof(args[0]); ++j) {
      if (args[j] >= 0 && args[j] < NY_X64_NIR_MAX_SLOTS)
        c->use_count[args[j]]++;
    }
    if (in->op == NYIR_CALL && in->extra_args && in->extra_args_len > 0) {
      for (size_t j = 0; j < in->extra_args_len; ++j) {
        int a = in->extra_args[j];
        if (a >= 0 && a < NY_X64_NIR_MAX_SLOTS)
          c->use_count[a]++;
      }
    }
  }
  /*
   * Signed division by two is lowered to (x >> 63) & 1 followed by an
   * add/shift pair.  Fuse the single-use sign extraction so the emitter can
   * use SHR directly, avoiding a spill/reload between SAR and AND.
   */
  for (size_t i = 0; i < c->nyir->len; ++i) {
    const nyr_inst_t *and = &c->nyir->data[i];
    if (and->op != NYIR_AND_I64 || and->dst < 0 ||
        and->dst >= NY_X64_NIR_MAX_SLOTS)
      continue;
    int bias = -1;
    int64_t mask = 0;
    if (ny_x64_nir_try_const_i32(c, and->b, i, &mask) && mask == 1)
      bias = and->a;
    else if (ny_x64_nir_try_const_i32(c, and->a, i, &mask) && mask == 1)
      bias = and->b;
    if (bias < 0 || bias >= NY_X64_NIR_MAX_SLOTS || c->def_count[bias] != 1 ||
        c->use_count[bias] != 1)
      continue;
    int shift_idx = c->def_index[bias];
    if (shift_idx < 0 || (size_t)shift_idx >= i)
      continue;
    const nyr_inst_t *shift = &c->nyir->data[shift_idx];
    int64_t amount = 0;
    if (shift->op != NYIR_SAR_I64 || shift->dst != bias ||
        !ny_x64_nir_try_const_i32(c, shift->b, (size_t)shift_idx, &amount) ||
        amount != 63 || shift->a < 0 || shift->a >= NY_X64_NIR_MAX_SLOTS)
      continue;
    bool phi_use = false;
    for (size_t user = 0; user < c->nyir->len && !phi_use; ++user) {
      const nyr_inst_t *in = &c->nyir->data[user];
      for (size_t incoming = 0; incoming < in->phi_incoming_len; ++incoming)
        if (in->phi_incoming[incoming].value == bias) {
          phi_use = true;
          break;
        }
    }
    if (!phi_use) {
      c->fused_sign_bias[and->dst] = true;
      c->elided_sign_shift[bias] = true;
    }
  }
  /*
   * Mark branch-only integer comparisons before emission.  BR_IF can emit the
   * original compare and conditional jump directly; discovering that use
   * while visiting BR_IF is too late because the earlier CMP has already
   * emitted setcc and stored its boolean result.
   */
  for (size_t i = 0; i < c->nyir->len; ++i) {
    const nyr_inst_t *branch = &c->nyir->data[i];
    if (branch->op != NYIR_BR_IF || branch->a < 0 ||
        branch->a >= NY_X64_NIR_MAX_SLOTS)
      continue;
    int cond = branch->a;
    if (c->def_count[cond] != 1 || c->use_count[cond] != 1)
      continue;
    int def = c->def_index[cond];
    if (def < 0 || (size_t)def >= c->nyir->len || (size_t)def + 1 != i ||
        c->nyir->data[def].op != NYIR_CMP_I64 || c->nyir->data[def].dst != cond)
      continue;
    bool has_phi_use = false;
    for (size_t user = 0; user < c->nyir->len && !has_phi_use; ++user) {
      const nyr_inst_t *in = &c->nyir->data[user];
      for (size_t incoming = 0; incoming < in->phi_incoming_len; ++incoming)
        if (in->phi_incoming[incoming].value == cond) {
          has_phi_use = true;
          break;
        }
    }
    if (!has_phi_use)
      c->fused_cmp[cond] = true;
  }
  /*
   * A compare used only as an integer SELECT condition can be re-emitted
   * immediately before the select. This avoids materializing and reloading
   * its 0/1 result across the speculative arms of an if-converted diamond.
   */
  for (size_t i = 0; i < c->nyir->len; ++i) {
    const nyr_inst_t *select = &c->nyir->data[i];
    if (select->op != NYIR_SELECT_I64 || select->a < 0 ||
        select->a >= NY_X64_NIR_MAX_SLOTS)
      continue;
    int cond = select->a;
    if (c->def_count[cond] != 1 || c->use_count[cond] != 1)
      continue;
    int def = c->def_index[cond];
    if (def < 0 || (size_t)def >= i || (size_t)def >= c->nyir->len ||
        (size_t)def + 1 != i || c->nyir->data[def].op != NYIR_CMP_I64 ||
        c->nyir->data[def].dst != cond)
      continue;
    bool has_phi_use = false;
    for (size_t user = 0; user < c->nyir->len && !has_phi_use; ++user) {
      const nyr_inst_t *in = &c->nyir->data[user];
      for (size_t incoming = 0; incoming < in->phi_incoming_len; ++incoming)
        if (in->phi_incoming[incoming].value == cond) {
          has_phi_use = true;
          break;
        }
    }
    if (!has_phi_use)
      c->fused_select_cmp[cond] = true;
  }
  /*
   * Fold `(x & 1) ==/!= 0` into TEST when the compare feeds a SELECT.
   */
  for (size_t i = 0; i < c->nyir->len; ++i) {
    const nyr_inst_t *select = &c->nyir->data[i];
    if (select->op != NYIR_SELECT_I64 || select->a < 0 ||
        select->a >= NY_X64_NIR_MAX_SLOTS || !c->fused_select_cmp[select->a])
      continue;
    const nyr_inst_t *cmp = &c->nyir->data[c->def_index[select->a]];
    if (cmp->cmp != NYIR_CMP_EQ && cmp->cmp != NYIR_CMP_NE)
      continue;
    int masked = -1;
    int64_t zero = 1, mask = 0;
    if (ny_x64_nir_try_const_i32(c, cmp->b, i, &zero) && zero == 0)
      masked = cmp->a;
    else if (ny_x64_nir_try_const_i32(c, cmp->a, i, &zero) && zero == 0)
      masked = cmp->b;
    if (masked < 0 || masked >= NY_X64_NIR_MAX_SLOTS ||
        c->def_count[masked] != 1 || c->use_count[masked] != 1)
      continue;
    int and_idx = c->def_index[masked];
    if (and_idx < 0 || (size_t)and_idx >= i)
      continue;
    const nyr_inst_t *and = &c->nyir->data[and_idx];
    int source = -1;
    if (and->op != NYIR_AND_I64 || and->dst != masked)
      continue;
    if (ny_x64_nir_try_const_i32(c, and->b, (size_t)and_idx, &mask) &&
        mask == 1)
      source = and->a;
    else if (ny_x64_nir_try_const_i32(c, and->a, (size_t)and_idx, &mask) &&
             mask == 1)
      source = and->b;
    if (source < 0 || source >= NY_X64_NIR_MAX_SLOTS)
      continue;
    bool phi_use = false;
    for (size_t user = 0; user < c->nyir->len && !phi_use; ++user) {
      const nyr_inst_t *in = &c->nyir->data[user];
      for (size_t incoming = 0; incoming < in->phi_incoming_len; ++incoming)
        if (in->phi_incoming[incoming].value == masked) {
          phi_use = true;
          break;
        }
    }
    if (phi_use)
      continue;
    c->elided_mask_test[masked] = true;
    c->fused_mask_test_src[select->a] = source;
  }
  /*
   * Fold a unique, adjacent address chain into x86's scaled-index memory
   * operand. Typed-buffer lowering emits `base + (index << 3)` immediately
   * before its load/store. When both temporaries have a single use, skipping
   * their stack homes is safe and removes two scalar operations per access.
   * Keep this pattern deliberately exact; unknown shapes use the normal path.
   */
  for (size_t i = 0; i < c->nyir->len; ++i) {
    const nyr_inst_t *access = &c->nyir->data[i];
    if ((access->op != NYIR_LOAD_I64 && access->op != NYIR_STORE_I64 &&
         access->op != NYIR_VEC4_LOAD_F64 &&
         access->op != NYIR_VEC4_STORE_F64 &&
         access->op != NYIR_VEC4_LOAD_I64 &&
         access->op != NYIR_VEC4_STORE_I64) ||
        access->a < 0 || access->a >= NY_X64_NIR_MAX_SLOTS ||
        c->use_count[access->a] != 1 || c->def_count[access->a] != 1)
      continue;
    int add_idx = c->def_index[access->a];
    if (add_idx < 0 || (size_t)add_idx >= i)
      continue;
    const nyr_inst_t *add = &c->nyir->data[add_idx];
    if (add->op != NYIR_ADD_I64 || add->dst != access->a)
      continue;
    bool safe_gap = true;
    for (size_t gap = (size_t)add_idx + 1; gap < i; ++gap) {
      nyr_op_t op = c->nyir->data[gap].op;
      if (op != NYIR_CONST_I64 && op != NYIR_LOAD_LOCAL && op != NYIR_COPY) {
        safe_gap = false;
        break;
      }
    }
    if (!safe_gap)
      continue;
    int base = -1, scaled = -1, shift_idx = -1;
    int candidates[2] = {add->a, add->b};
    for (size_t k = 0; k < 2; ++k) {
      int candidate = candidates[k];
      if (candidate < 0 || candidate >= NY_X64_NIR_MAX_SLOTS ||
          c->def_count[candidate] != 1 || c->use_count[candidate] != 1)
        continue;
      int di = c->def_index[candidate];
      if (di < 0 || di + 1 != add_idx)
        continue;
      const nyr_inst_t *shift = &c->nyir->data[di];
      if (shift->op != NYIR_SHL_I64 || shift->dst != candidate)
        continue;
      if (shift->b < 0 || shift->b >= NY_X64_NIR_MAX_SLOTS ||
          c->def_count[shift->b] != 1)
        continue;
      const nyr_inst_t *scale_def = &c->nyir->data[c->def_index[shift->b]];
      if (scale_def->op != NYIR_CONST_I64 || scale_def->imm != 3)
        continue;
      scaled = candidate;
      base = candidates[1 - k];
      shift_idx = di;
      break;
    }
    if (base < 0 || scaled < 0 || base >= NY_X64_NIR_MAX_SLOTS ||
        base == access->a || base == scaled)
      continue;
    c->indexed_base[access->a] = base;
    c->indexed_index[access->a] = c->nyir->data[shift_idx].a;
    c->folded_addr_temp[access->a] = true;
    c->folded_addr_temp[scaled] = true;
  }
  /*
   * SSA COPY values are bit-identical aliases of their producer. Give each
   * copy chain one canonical stack home instead of reserving and shuffling a
   * fresh pair of frame words for every move. PHIs and all other producers
   * retain distinct homes; their values can vary by control-flow edge.
   *
   * The sharing is only sound for single-definition chains. After PHI
   * elimination a phi result carries one COPY per incoming edge in different
   * blocks; one of those copies can then be elided as a "self-copy" while a
   * sibling edge overwrites the shared home with a different value (a nested
   * loop's backedge), so the elided copy no longer re-establishes the value.
   * Any multi-definition value keeps a private home and terminates the walk.
   */
  for (int v = 0; v < max_val && v < NY_X64_NIR_MAX_SLOTS; ++v) {
    int root = v;
    for (int depth = 0; depth < max_val; ++depth) {
      if (c->def_count[root] != 1)
        break;
      int di = c->def_index[root];
      if (di < 0 || (size_t)di >= c->nyir->len)
        break;
      const nyr_inst_t *def = &c->nyir->data[di];
      if (def->op != NYIR_COPY || def->a < 0 || def->a >= max_val ||
          def->a >= NY_X64_NIR_MAX_SLOTS)
        break;
      root = def->a;
      if (depth + 1 == max_val)
        root = v; /* malformed cyclic COPY input: preserve a private home */
    }
    if (root < 0 || root >= max_val || c->def_count[root] != 1)
      root = v;
    c->value_home_root[v] = root;
  }
  /*
   * Reuse scalar stack homes when their conservative linear live ranges do
   * not overlap. A value with multiple definitions, no unique definition, or
   * a use textually before its definition is pinned for the whole function;
   * those cases include PHI-eliminated loop-carried values. The range of a
   * COPY chain is the union of every member's definitions and uses because
   * the chain intentionally shares one home.
   */
  size_t *home_first_def = malloc((size_t)max_val * sizeof(size_t));
  size_t *home_last_def = calloc((size_t)max_val, sizeof(size_t));
  size_t *home_first_use = malloc((size_t)max_val * sizeof(size_t));
  size_t *home_last_use = calloc((size_t)max_val, sizeof(size_t));
  size_t *home_block = malloc((size_t)max_val * sizeof(size_t));
  size_t *root_first = malloc((size_t)max_val * sizeof(size_t));
  size_t *root_last = calloc((size_t)max_val, sizeof(size_t));
  size_t *root_block = malloc((size_t)max_val * sizeof(size_t));
  size_t *root_order = malloc((size_t)max_val * sizeof(size_t));
  size_t *color_last = malloc((size_t)max_val * sizeof(size_t));
  bool *root_pinned = calloc((size_t)max_val, sizeof(bool));
  bool home_reuse_ready = home_first_def && home_last_def && home_first_use &&
                          home_last_use && home_block && root_first &&
                          root_last && root_block && root_order && color_last &&
                          root_pinned;
  if (home_reuse_ready) {
    int tracked_val_count =
        max_val < NY_X64_NIR_MAX_SLOTS ? max_val : NY_X64_NIR_MAX_SLOTS;
    for (int v = 0; v < tracked_val_count; ++v) {
      home_first_def[v] = SIZE_MAX;
      home_first_use[v] = SIZE_MAX;
      home_block[v] = SIZE_MAX;
      root_first[v] = SIZE_MAX;
      root_block[v] = SIZE_MAX;
    }
    /*
     * A linear interval is unsafe for values that cross basic-block edges:
     * loops can make a later textual use happen in a different iteration.
     * Values whose definitions and uses stay within one block are safe to
     * color by instruction position, including inside loop bodies. Mark phi
     * inputs mixed because their uses belong to predecessor edges.
     */
    size_t current_block = 0;
    for (size_t i = 0; i < c->nyir->len; ++i) {
      const nyr_inst_t *in = &c->nyir->data[i];
      if (in->op == NYIR_LABEL)
        ++current_block;
      if (in->dst >= 0 && in->dst < tracked_val_count) {
        size_t v = (size_t)in->dst;
        if (i < home_first_def[v])
          home_first_def[v] = i;
        if (i > home_last_def[v])
          home_last_def[v] = i;
        if (home_block[v] == SIZE_MAX)
          home_block[v] = current_block;
        else if (home_block[v] != current_block)
          home_block[v] = SIZE_MAX - 1;
      }
      const int args[] = {in->a, in->b, in->c, in->d, in->e, in->f};
      for (size_t a = 0; a < sizeof(args) / sizeof(args[0]); ++a) {
        if (args[a] >= 0 && args[a] < tracked_val_count) {
          size_t v = (size_t)args[a];
          if (i < home_first_use[v])
            home_first_use[v] = i;
          if (i > home_last_use[v])
            home_last_use[v] = i;
          if (home_block[v] == SIZE_MAX)
            home_block[v] = current_block;
          else if (home_block[v] != current_block)
            home_block[v] = SIZE_MAX - 1;
        }
      }
      if (in->op == NYIR_CALL && in->extra_args) {
        for (size_t a = 0; a < in->extra_args_len; ++a) {
          int arg = in->extra_args[a];
          if (arg >= 0 && arg < tracked_val_count) {
            size_t v = (size_t)arg;
            if (i < home_first_use[v])
              home_first_use[v] = i;
            if (i > home_last_use[v])
              home_last_use[v] = i;
            if (home_block[v] == SIZE_MAX)
              home_block[v] = current_block;
            else if (home_block[v] != current_block)
              home_block[v] = SIZE_MAX - 1;
          }
        }
      }
      for (size_t a = 0; a < in->phi_incoming_len; ++a) {
        int value = in->phi_incoming[a].value;
        if (value >= 0 && value < tracked_val_count)
          home_block[value] = SIZE_MAX - 1;
      }
      if (in->op == NYIR_BR || in->op == NYIR_BR_IF || in->op == NYIR_RET)
        ++current_block;
    }
    for (int v = 0; v < tracked_val_count; ++v) {
      int root = c->value_home_root[v];
      if (root < 0 || root >= max_val)
        continue;
      size_t r = (size_t)root;
      if (home_block[v] == SIZE_MAX || home_block[v] == SIZE_MAX - 1)
        root_pinned[r] = true;
      else if (root_block[r] == SIZE_MAX)
        root_block[r] = home_block[v];
      else if (root_block[r] != home_block[v])
        root_pinned[r] = true;
      if (c->def_count[v] != 1 || home_first_def[v] == SIZE_MAX ||
          (home_first_use[v] != SIZE_MAX &&
           home_first_use[v] < home_first_def[v]))
        root_pinned[r] = true;
      size_t first = home_first_def[v];
      if (first == SIZE_MAX)
        first = 0;
      if (first < root_first[r])
        root_first[r] = first;
      size_t last = home_last_use[v];
      if (home_last_def[v] > last)
        last = home_last_def[v];
      if (last > root_last[r])
        root_last[r] = last;
    }
    size_t root_count = 0;
    for (int root = 0; root < tracked_val_count; ++root) {
      if (c->value_home_root[root] != root)
        continue;
      if (root_first[root] == SIZE_MAX)
        root_first[root] = 0;
      if (root_pinned[root]) {
        root_first[root] = 0;
        root_last[root] = c->nyir->len;
      }
      size_t pos = root_count;
      while (pos > 0 && root_first[root_order[pos - 1]] > root_first[root]) {
        root_order[pos] = root_order[pos - 1];
        --pos;
      }
      root_order[pos] = (size_t)root;
      ++root_count;
    }
    size_t scalar_color_count = 0;
    size_t vector_color_count = 0;
    /*
     * Scalar values need one stack word. Values marked for vector spilling
     * retain a four-word home. Color the two widths independently, then pack
     * the vector homes after the scalar region so no scalar home overlaps a
     * vector lane.
     */
    for (int width_pass = 0; width_pass < 2; ++width_pass) {
      bool vector_home_pass = width_pass != 0;
      size_t *color_count =
          vector_home_pass ? &vector_color_count : &scalar_color_count;
      for (size_t i = 0; i < root_count; ++i) {
        size_t root = root_order[i];
        bool vector_home =
            c->value_f64[root] || c->value_f32[root] || c->value_i64v2[root];
        if (vector_home != vector_home_pass)
          continue;
        size_t color_base = vector_home_pass ? scalar_color_count : 0;
        size_t color = 0;
        while (color < *color_count &&
               color_last[color_base + color] >= root_first[root])
          ++color;
        if (color == *color_count)
          ++*color_count;
        color_last[color_base + color] = root_last[root];
        /*
         * ny_x64_nir_slot maps a vector base to the four words ending at
         * base+4. Leave three index words between scalar and vector colors
         * so their physical rbp-relative ranges cannot overlap.
         */
        size_t word_offset =
            vector_home_pass ? scalar_color_count + 3 + color * 4 : color;
        c->value_home_index[root] = (int)word_offset;
      }
    }
    size_t total_words = scalar_color_count + (vector_color_count ? 3 : 0) +
                         vector_color_count * 4;
    c->value_home_count = (int)total_words;
    for (int v = 0; v < tracked_val_count; ++v) {
      int root = c->value_home_root[v];
      if (root >= 0 && root < max_val)
        c->value_home_index[v] = c->value_home_index[root];
    }
  }
  free(home_first_def);
  free(home_last_def);
  free(home_first_use);
  free(home_last_use);
  free(home_block);
  free(root_first);
  free(root_last);
  free(root_block);
  free(root_order);
  free(color_last);
  free(root_pinned);
  if (!home_reuse_ready) {
    for (int v = 0; v < max_val && v < NY_X64_NIR_MAX_SLOTS; ++v) {
      int root = c->value_home_root[v];
      if (root < 0 || root >= max_val)
        continue;
      if (c->value_home_index[root] < 0) {
        int width =
            c->value_f64[root] || c->value_f32[root] || c->value_i64v2[root]
                ? 4
                : 1;
        c->value_home_index[root] = c->value_home_count;
        c->value_home_count += width;
      }
    }
    for (int v = 0; v < max_val && v < NY_X64_NIR_MAX_SLOTS; ++v) {
      int root = c->value_home_root[v];
      if (root >= 0 && root < max_val)
        c->value_home_index[v] = c->value_home_index[root];
    }
  }
  c->is_leaf = true;
  for (size_t i = 0; i < c->nyir->len; ++i) {
    nyr_op_t op = c->nyir->data[i].op;
    if (op == NYIR_CALL) {
      c->is_leaf = false;
      break;
    }
    /*
     * Vector ops and x87 trig hardcode -N(%rbp) slot addressing; force a
     * rbp frame so those references are valid even for otherwise-leaf
     * functions.
     */
    if (op == NYIR_VEC4_LOAD_F64 || op == NYIR_VEC4_STORE_F64 ||
        op == NYIR_VEC4_LOAD_I64 || op == NYIR_VEC4_STORE_I64 ||
        op == NYIR_VEC4_ADD_I64 || op == NYIR_VEC4_SUB_I64 ||
        op == NYIR_VEC4_AND_I64 || op == NYIR_VEC4_OR_I64 ||
        op == NYIR_VEC4_XOR_I64 || op == NYIR_VEC4_SHL_I64 ||
        op == NYIR_VEC4_REDUCE_ADD_I64 || op == NYIR_VEC4_ADD_F64 ||
        op == NYIR_VEC4_SUB_F64 || op == NYIR_VEC4_MUL_F64 ||
        op == NYIR_VEC4_DIV_F64 || op == NYIR_VEC4_FMA_F64 ||
        op == NYIR_VEC4_SET1_F64 || op == NYIR_VEC4_SHUFFLE_F64 ||
        op == NYIR_VEC8_LOAD_I64 || op == NYIR_VEC8_STORE_I64 ||
        op == NYIR_VEC8_LOAD_F32 || op == NYIR_VEC8_STORE_F32 ||
        op == NYIR_VEC8_ADD_F32 || op == NYIR_VEC8_SUB_F32 ||
        op == NYIR_VEC8_MUL_F32 || op == NYIR_VEC8_DIV_F32 ||
        op == NYIR_VEC8_FMA_F32 || op == NYIR_VEC8_SET1_F32) {
      c->is_leaf = false;
      break;
    }
  }
  c->max_local_slot = max_local + 1;
  c->frame_slots = c->max_local_slot;
  /*
   * Value homes are packed by the width selected during interval coloring.
   */
  for (int v = 0; v < max_val &&
                  c->frame_slots + c->value_home_count < NY_X64_NIR_MAX_SLOTS;
       ++v)
    ny_x64_nir_slot(c, v);
  int total = c->frame_slots + c->value_home_count;
  int raw = total * 8;
  /*
   * Keep the frame size 16-byte aligned for nested calls.
   */
  c->frame_bytes = (raw + 15) & ~15;
}

static void ny_x64_nir_slot_str(ny_x64_nir_ctx_t *c, int slot, char *buf,
                                size_t len) {
  if (slot < 0 || slot >= NY_X64_NIR_MAX_SLOTS) {
    buf[0] = '\0';
    return;
  }
  if (c->home_reg[slot] >= 0) {
    static const char *const regs[] = {"%rbx", "%r12", "%r13", "%r14", "%r15"};
    int reg = c->home_reg[slot];
    if (reg < (int)(sizeof(regs) / sizeof(regs[0])))
      snprintf(buf, len, "%s", regs[reg]);
    else
      buf[0] = '\0';
    return;
  }
  if (c->is_leaf) {
    snprintf(buf, len, "%d(%%rsp)", c->frame_bytes - (slot + 2) * 8);
  } else {
    int off = slot < c->frame_slots ? (slot + 1) * 8 : c->slot_offset[slot];
    snprintf(buf, len, "-%d(%%rbp)", off);
  }
}

/*
 * Keep the most frequently used scalar loop homes in callee-saved registers.
 * Stack homes remain allocated for the conservative fallback path; selected
 * homes are written and read through the register throughout the function.
 */
static void ny_x64_nir_assign_loop_regs(ny_x64_nir_ctx_t *c) {
  if (!c || !c->nyir || c->max_local_slot <= 0)
    return;
  int *labels = malloc(NY_X64_NIR_MAX_SLOTS * sizeof(*labels));
  uint32_t *score = calloc((size_t)c->max_local_slot, sizeof(*score));
  bool *address_taken =
      calloc((size_t)c->max_local_slot, sizeof(*address_taken));
  if (!labels || !score || !address_taken) {
    free(labels);
    free(score);
    free(address_taken);
    return;
  }
  for (size_t i = 0; i < NY_X64_NIR_MAX_SLOTS; ++i)
    labels[i] = -1;
  for (int i = 0; i < NY_X64_NIR_MAX_SLOTS; ++i)
    c->home_reg[i] = -1;
  for (size_t i = 0; i < c->nyir->len; ++i) {
    const nyr_inst_t *in = &c->nyir->data[i];
    if (in->op == NYIR_LABEL && in->imm >= 0 && in->imm < NY_X64_NIR_MAX_SLOTS)
      labels[in->imm] = (int)i;
    if (in->op == NYIR_ADDR_LOCAL && in->imm >= 0 &&
        in->imm < c->max_local_slot)
      address_taken[in->imm] = true;
  }

  for (size_t end = 0; end < c->nyir->len; ++end) {
    const nyr_inst_t *branch = &c->nyir->data[end];
    if ((branch->op != NYIR_BR && branch->op != NYIR_BR_IF) ||
        branch->imm < 0 || branch->imm >= NY_X64_NIR_MAX_SLOTS)
      continue;
    int start = labels[branch->imm];
    if (start < 0 || (size_t)start >= end)
      continue;
    for (size_t i = (size_t)start + 1; i < end; ++i) {
      const nyr_inst_t *in = &c->nyir->data[i];
      if ((in->op == NYIR_LOAD_LOCAL || in->op == NYIR_STORE_LOCAL) &&
          in->imm >= 0 && in->imm < c->max_local_slot &&
          !address_taken[in->imm] && !c->local_f64[in->imm] &&
          !c->local_f32[in->imm] && score[in->imm] < UINT32_MAX)
        score[in->imm]++;
    }
  }
  for (int reg = 0; reg < NY_X64_LOOP_REG_COUNT; ++reg) {
    if ((size_t)reg < (size_t)c->home_reg_count)
      continue;
    int best_slot = -1;
    uint32_t best_score = 0;
    for (int slot = 0; slot < c->max_local_slot; ++slot) {
      if (c->home_reg[slot] < 0 && score[slot] > best_score) {
        best_slot = slot;
        best_score = score[slot];
      }
    }
    if (best_slot < 0)
      break;
    c->home_reg[best_slot] = reg;
    c->home_reg_count = reg + 1;
    if (ny_dbg_is_enabled(NY_DBG_DEEP)) {
      ny_dbg_enter_context(c->label_prefix ? c->label_prefix : "<anonymous>");
      ny_dbg_heuristic_event("X64_LOOP_REG",
                             "function=%s home=%d register=%d loop_accesses=%u",
                             c->label_prefix ? c->label_prefix : "<anonymous>",
                             best_slot, reg, best_score);
      ny_dbg_exit_context(c->label_prefix ? c->label_prefix : "<anonymous>");
    }
  }
  if (c->home_reg_count & 1)
    c->frame_bytes += 8;
  /*
   * The leaf prologue uses RSP-relative homes and intentionally omits the
   * callee-saved register frame. If a leaf loop gets register homes, use the
   * regular frame path so those registers are preserved and local homes keep
   * their RBP-relative offsets. The loop itself remains call-free.
   */
  if (c->home_reg_count > 0)
    c->is_leaf = false;
  /*
   * Keep SSA value homes separate from local homes.  A local can be
   * overwritten while an earlier SSA value remains live, so sharing the
   * local's callee-saved register requires CFG-aware liveness.
   */
  free(labels);
  free(score);
  free(address_taken);
}

/*
 * Coalesce a scalar value home with a loop local's register when the value
 * dies at its STORE_LOCAL. The value remains in that register for all of its
 * in-block uses, and the local store becomes a no-op. Reject homes shared by
 * distinct SSA roots, PHI-edge values, and any interval that reads or writes
 * the local before the store; those cases require full CFG liveness.
 */

/*
 * A rematerialized immediate must not cross an instruction that consumes a
 * fixed register.  The value's stack home is the conservative handoff across
 * those boundaries.
 */
static bool ny_x64_nir_is_fixed_clobber(ny_x64_nir_ctx_t *c,
                                        const nyr_inst_t *in, size_t idx);

/*
 * Resolve a value through explicit COPY definitions to its CONST_I64 root.
 * The complete chain must lie in one fixed-register-safe region.
 */
static bool ny_x64_nir_try_remat_i64(ny_x64_nir_ctx_t *c, int value_id,
                                     size_t use_idx, int64_t *out) {
  if (value_id < 0 || value_id >= NY_X64_NIR_MAX_SLOTS ||
      use_idx > c->nyir->len)
    return false;
  int current = value_id;
  size_t before = use_idx;
  size_t root = 0;
  int64_t imm = 0;
  for (size_t hops = 0; hops < NY_X64_NIR_MAX_SLOTS; ++hops) {
    int di = c->def_index[current];
    if (di < 0 || (size_t)di >= before)
      return false;
    /*
     * Multi-definition values (PHI-elimination copy pairs) have no single
     * rematerializable constant across incoming edges.
     */
    if (current < NY_X64_NIR_MAX_SLOTS && c->def_count[current] != 1)
      return false;
    const nyr_inst_t *def = &c->nyir->data[di];
    if (def->dst != current)
      return false;
    if (def->op == NYIR_CONST_I64) {
      root = (size_t)di;
      imm = def->imm;
      break;
    }
    if (def->op != NYIR_COPY || def->a < 0 || def->a >= NY_X64_NIR_MAX_SLOTS)
      return false;
    current = def->a;
    before = (size_t)di;
    if (hops + 1 == NY_X64_NIR_MAX_SLOTS)
      return false;
  }
  for (size_t i = root + 1; i < use_idx; ++i) {
    if (ny_x64_nir_is_fixed_clobber(c, &c->nyir->data[i], i))
      return false;
  }
  *out = imm;
  return true;
}

static bool ny_x64_nir_is_fixed_clobber(ny_x64_nir_ctx_t *c,
                                        const nyr_inst_t *in, size_t idx) {
  switch (in->op) {
  case NYIR_CALL:
  case NYIR_DIV_I64:
  case NYIR_MOD_I64:
    return true;
  case NYIR_SHL_I64:
  case NYIR_SAR_I64: {
    int64_t count = 0;
    return !ny_x64_nir_try_remat_i64(c, in->b, idx, &count) || count < 1 ||
           count > 63;
  }
  case NYIR_ROR32_I64: {
    int64_t count = 0;
    return !ny_x64_nir_try_remat_i64(c, in->b, idx, &count) || count < 0 ||
           count >= 32;
  }
  default:
    return false;
  }
}

static bool ny_x64_nir_find_const_val(ny_x64_nir_ctx_t *c, int value_id,
                                      int64_t *out) {
  if (!c || !c->nyir || value_id < 0 || value_id >= (int)c->nyir->next_value)
    return false;
  int current = value_id;
  for (size_t hops = 0; hops < 32; ++hops) {
    if (current < 0 || current >= (int)c->nyir->next_value)
      return false;
    /*
     * A multi-definition value (a PHI-elimination copy pair) has no single
     * constant: each incoming edge carries a different value.
     */
    if (current < NY_X64_NIR_MAX_SLOTS && c->def_count[current] != 1) {
      if (c->uniform_const_state[current] != 1)
        return false;
      if (out)
        *out = c->uniform_const_value[current];
      return true;
    }
    int di = c->def_index[current];
    if (di < 0 || (size_t)di >= c->nyir->len)
      return false;
    const nyr_inst_t *def = &c->nyir->data[di];
    if (def->dst != current)
      return false;
    if (def->op == NYIR_CONST_I64) {
      if (out)
        *out = def->imm;
      return true;
    }
    if (def->op == NYIR_COPY && def->a >= 0) {
      current = def->a;
      continue;
    }
    return false;
  }
  return false;
}

/*
 * Immediate operands do not depend on machine-register state. A uniquely
 * defined CONST/COPY chain remains a literal across calls, divisions, and
 * CFG labels, even when its stack home is still needed by another use.
 */
static bool ny_x64_nir_try_const_i32(ny_x64_nir_ctx_t *c, int value_id,
                                     size_t use_idx, int64_t *out) {
  (void)use_idx;
  int64_t v = 0;
  if (!ny_x64_nir_find_const_val(c, value_id, &v) || v < INT32_MIN ||
      v > INT32_MAX)
    return false;
  *out = v;
  return true;
}

/*
 * Check whether value_id is a constant/COPY shift count.
 * x86-64 immediate shifts use counts 1 through 63.
 */
static bool ny_x64_nir_try_shift_imm(ny_x64_nir_ctx_t *c, int value_id,
                                     size_t use_idx, int *out) {
  (void)use_idx;
  int64_t v = 0;
  if (!ny_x64_nir_find_const_val(c, value_id, &v) || v < 1 || v > 63)
    return false;
  *out = (int)v;
  return true;
}

/*
 * Return the shift count only for a positive power-of-two divisor. Shift
 * operands accept any count; division and modulo must not confuse a literal
 * divisor such as 5 with a shift count of 5.
 */
static bool ny_x64_nir_try_pow2_divisor(ny_x64_nir_ctx_t *c, int value_id,
                                        size_t use_idx, int *out) {
  int64_t divisor = 0;
  if (!ny_x64_nir_try_const_i32(c, value_id, use_idx, &divisor) ||
      divisor <= 1 || (divisor & (divisor - 1)) != 0)
    return false;
  int shift = 0;
  while (divisor > 1) {
    divisor >>= 1;
    shift++;
  }
  *out = shift;
  return true;
}

static bool ny_x64_nir_remat_consumer(const nyr_inst_t *in, int value_id) {
  if (!ny_x64_nir_inst_uses_value(in, value_id))
    return false;
  switch (in->op) {
  case NYIR_ADD_I64:
  case NYIR_SUB_I64:
  case NYIR_MUL_I64:
  case NYIR_AND_I64:
  case NYIR_OR_I64:
  case NYIR_XOR_I64:
  case NYIR_SHL_I64:
  case NYIR_SAR_I64:
  case NYIR_ROR32_I64:
  case NYIR_CALL:
    return true;
  default:
    return false;
  }
}

/*
 * A home may be skipped only when this value has one COPY-only path to a
 * consumer in the same safe region.  Any unknown segment keeps its home.
 */
static bool ny_x64_nir_can_fuse_const(ny_x64_nir_ctx_t *c, int value_id,
                                      size_t def_idx, int64_t *out) {
  int current = value_id;
  size_t current_def = def_idx;
  for (size_t hops = 0; hops < NY_X64_NIR_MAX_SLOTS; ++hops) {
    if (current < 0 || current >= NY_X64_NIR_MAX_SLOTS ||
        c->use_count[current] != 1)
      return false;
    size_t use_idx = current_def + 1;
    while (use_idx < c->nyir->len &&
           !ny_x64_nir_inst_uses_value(&c->nyir->data[use_idx], current))
      use_idx++;
    if (use_idx == c->nyir->len)
      return false;
    const nyr_inst_t *use = &c->nyir->data[use_idx];
    if (use->op == NYIR_COPY && use->a == current && use->dst >= 0 &&
        use->dst < NY_X64_NIR_MAX_SLOTS) {
      current = use->dst;
      current_def = use_idx;
      continue;
    }
    if (!ny_x64_nir_remat_consumer(use, current))
      return false;
    return ny_x64_nir_try_remat_i64(c, value_id, use_idx, out);
  }
  return false;
}

static bool ny_x64_nir_load(ny_x64_nir_ctx_t *c, int slot) {
  if (slot < 0 || slot >= NY_X64_NIR_MAX_SLOTS)
    return false;
  if (c->rax_cached_slot == slot)
    return true;
  if (c->home_reg[slot] >= 0) {
    static const char *const regs[] = {"%rbx", "%r12", "%r13", "%r14", "%r15"};
    int reg = c->home_reg[slot];
    if (reg >= (int)(sizeof(regs) / sizeof(regs[0])) ||
        !nyr_printf(c->w, "\tmovq\t%s, %%rax\n", regs[reg]))
      return false;
    c->rax_cached_slot = slot;
    return true;
  }
  if (c->is_leaf) {
    int off = c->frame_bytes - (slot + 2) * 8;
    if (!nyr_printf(c->w, "\tmovq\t%d(%%rsp), %%rax\n", off))
      return false;
    c->rax_cached_slot = slot;
    return true;
  }
  if (c->slot_offset[slot] <= 0)
    return false;
  if (!nyr_printf(c->w, "\tmovq\t-%d(%%rbp), %%rax\n", c->slot_offset[slot]))
    return false;
  c->rax_cached_slot = slot;
  return true;
}

static bool ny_x64_nir_store(ny_x64_nir_ctx_t *c, int slot) {
  if (slot < 0 || slot >= NY_X64_NIR_MAX_SLOTS)
    return false;
  if (c->home_reg[slot] >= 0) {
    static const char *const regs[] = {"%rbx", "%r12", "%r13", "%r14", "%r15"};
    int reg = c->home_reg[slot];
    if (reg >= (int)(sizeof(regs) / sizeof(regs[0])) ||
        !nyr_printf(c->w, "\tmovq\t%%rax, %s\n", regs[reg]))
      return false;
    c->rax_cached_slot = slot;
    c->rax_store_slot = slot;
    return true;
  }
  if (c->is_leaf) {
    int off = c->frame_bytes - (slot + 2) * 8;
    if (!nyr_printf(c->w, "\tmovq\t%%rax, %d(%%rsp)\n", off))
      return false;
    c->rax_cached_slot = slot;
    c->rax_store_slot = slot;
    return true;
  }
  if (c->slot_offset[slot] <= 0)
    return false;
  if (!nyr_printf(c->w, "\tmovq\t%%rax, -%d(%%rbp)\n", c->slot_offset[slot]))
    return false;
  c->rax_cached_slot = slot;
  c->rax_store_slot = slot;
  return true;
}

static bool ny_x64_nir_materialize_i64(ny_x64_nir_ctx_t *c, int64_t imm) {
  c->rax_cached_slot = -1;
  if (imm >= INT32_MIN && imm <= INT32_MAX)
    return nyr_printf(c->w, "\tmovq\t$%" PRId64 ", %%rax\n", imm);
  return nyr_printf(c->w, "\tmovabsq\t$%" PRId64 ", %%rax\n", imm);
}

/*
 * Load a value that may be a next-use-fused CONST_I64 whose home store was
 * skipped (see can_fuse_const).  Fused constants are rematerialized with the
 * signed form that preserves their full i64 value.
 */
static bool ny_x64_nir_load_value(ny_x64_nir_ctx_t *c, int value_id) {
  if (value_id >= 0 && value_id < NY_X64_NIR_MAX_SLOTS &&
      c->fused_const[value_id]) {
    return ny_x64_nir_materialize_i64(c, c->fused_imm[value_id]);
  }
  return ny_x64_nir_load(c, ny_x64_nir_slot(c, value_id));
}

/*
 * Use x86's memory-source form when the right operand has a canonical home.
 * Fused constants have no home, so retain the scratch-register sequence for
 * those values.
 */
static bool ny_x64_nir_emit_i64_binop(ny_x64_nir_ctx_t *c, const nyr_inst_t *in,
                                      const char *mnemonic) {
  if (!c || !in || !mnemonic)
    return false;
  int bslot = ny_x64_nir_slot(c, in->b);
  if (in->b >= 0 && in->b < NY_X64_NIR_MAX_SLOTS && !c->fused_const[in->b] &&
      bslot >= 0) {
    char rhs[64];
    ny_x64_nir_slot_str(c, bslot, rhs, sizeof(rhs));
    if (rhs[0]) {
      return ny_x64_nir_load_value(c, in->a) &&
             nyr_printf(c->w, "\t%s\t%s, %%rax\n", mnemonic, rhs) &&
             ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    }
  }
  return ny_x64_nir_load_value(c, in->b) &&
         nyr_put(c->w, "\tmovq\t%rax, %r10\n") &&
         ny_x64_nir_load_value(c, in->a) &&
         nyr_printf(c->w, "\t%s\t%%r10, %%rax\n", mnemonic) &&
         ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
}

static bool ny_x64_nir_emit_i64_varshift(ny_x64_nir_ctx_t *c,
                                         const nyr_inst_t *in,
                                         const char *mnemonic) {
  if (!c || !in || !mnemonic)
    return false;
  int bslot = ny_x64_nir_slot(c, in->b);
  if (in->b >= 0 && in->b < NY_X64_NIR_MAX_SLOTS && !c->fused_const[in->b] &&
      bslot >= 0) {
    char rhs[64];
    ny_x64_nir_slot_str(c, bslot, rhs, sizeof(rhs));
    if (rhs[0]) {
      return nyr_printf(c->w, "\tmovb\t%s, %%cl\n", rhs) &&
             ny_x64_nir_load_value(c, in->a) &&
             nyr_printf(c->w, "\t%s\t%%cl, %%rax\n", mnemonic) &&
             ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    }
  }
  return ny_x64_nir_load_value(c, in->b) &&
         nyr_put(c->w, "\tmovq\t%rax, %r10\n") &&
         ny_x64_nir_load_value(c, in->a) &&
         nyr_printf(c->w, "\tmovb\t%%r10b, %%cl\n\t%s\t%%cl, %%rax\n",
                    mnemonic) &&
         ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
}

static bool ny_x64_nir_load_xmm(ny_x64_nir_ctx_t *c, int slot, int xmm) {
  if (slot < 0 || slot >= NY_X64_NIR_MAX_SLOTS || xmm < 0 || xmm > 15)
    return false;
  if (c->is_leaf) {
    int off = c->frame_bytes - (slot + 2) * 8;
    return nyr_printf(c->w, "\tmovsd\t%d(%%rsp), %%xmm%d\n", off, xmm);
  }
  if (c->slot_offset[slot] <= 0)
    return false;
  return nyr_printf(c->w, "\tmovsd\t-%d(%%rbp), %%xmm%d\n",
                    c->slot_offset[slot], xmm);
}

static bool ny_x64_nir_store_xmm(ny_x64_nir_ctx_t *c, int slot, int xmm) {
  if (slot < 0 || slot >= NY_X64_NIR_MAX_SLOTS || xmm < 0 || xmm > 15)
    return false;
  if (c->is_leaf) {
    int off = c->frame_bytes - (slot + 2) * 8;
    return nyr_printf(c->w, "\tmovsd\t%%xmm%d, %d(%%rsp)\n", xmm, off);
  }
  if (c->slot_offset[slot] <= 0)
    return false;
  return nyr_printf(c->w, "\tmovsd\t%%xmm%d, -%d(%%rbp)\n", xmm,
                    c->slot_offset[slot]);
}

/* VEC4 i64 values contain two lanes. Their homes are four scalar slots wide
 * in the shared allocator; the pair beginning at slot is contiguous. */
static bool ny_x64_nir_load_i64v2(ny_x64_nir_ctx_t *c, int slot, int xmm) {
  if (!c || slot < 0 || slot + 1 >= NY_X64_NIR_MAX_SLOTS || xmm < 0 ||
      xmm > 15 || c->slot_offset[slot] <= 0)
    return false;
  return nyr_printf(c->w, "\tmovdqu\t-%d(%%rbp), %%xmm%d\n",
                    c->slot_offset[slot], xmm);
}

static bool ny_x64_nir_store_i64v2(ny_x64_nir_ctx_t *c, int slot, int xmm) {
  if (!c || slot < 0 || slot + 1 >= NY_X64_NIR_MAX_SLOTS || xmm < 0 ||
      xmm > 15 || c->slot_offset[slot] <= 0)
    return false;
  return nyr_printf(c->w, "\tmovdqu\t%%xmm%d, -%d(%%rbp)\n", xmm,
                    c->slot_offset[slot]);
}

static bool ny_x64_nir_load_f64_lane(ny_x64_nir_ctx_t *c, int slot,
                                     unsigned lane, int xmm) {
  return lane < 4 && slot >= 0 && slot + (int)lane < NY_X64_NIR_MAX_SLOTS &&
         ny_x64_nir_load_xmm(c, slot + (int)lane, xmm);
}

static bool ny_x64_nir_store_f64_lane(ny_x64_nir_ctx_t *c, int slot,
                                      unsigned lane, int xmm) {
  return lane < 4 && slot >= 0 && slot + (int)lane < NY_X64_NIR_MAX_SLOTS &&
         ny_x64_nir_store_xmm(c, slot + (int)lane, xmm);
}

/*
 * Vector values occupy four consecutive f64 homes. Load/store two lanes at
 * once; the lane-zero home is the lowest address in each pair.
 */
static bool ny_x64_nir_load_f64_pair(ny_x64_nir_ctx_t *c, int slot,
                                     unsigned first_lane, int xmm) {
  if (!c || slot < 0 || first_lane > 2 ||
      slot + (int)first_lane + 1 >= NY_X64_NIR_MAX_SLOTS || xmm < 0 || xmm > 15)
    return false;
  int pair_slot = slot + (int)first_lane;
  if (c->is_leaf) {
    int off = c->frame_bytes - (pair_slot + 2) * 8;
    return nyr_printf(c->w, "\tmovupd\t%d(%%rsp), %%xmm%d\n", off, xmm);
  }
  if (c->slot_offset[pair_slot] <= 0)
    return false;
  return nyr_printf(c->w, "\tmovupd\t-%d(%%rbp), %%xmm%d\n",
                    c->slot_offset[pair_slot], xmm);
}

static bool ny_x64_nir_store_f64_pair(ny_x64_nir_ctx_t *c, int slot,
                                      unsigned first_lane, int xmm) {
  if (!c || slot < 0 || first_lane > 2 ||
      slot + (int)first_lane + 1 >= NY_X64_NIR_MAX_SLOTS || xmm < 0 || xmm > 15)
    return false;
  int pair_slot = slot + (int)first_lane;
  if (c->is_leaf) {
    int off = c->frame_bytes - (pair_slot + 2) * 8;
    return nyr_printf(c->w, "\tmovupd\t%%xmm%d, %d(%%rsp)\n", xmm, off);
  }
  if (c->slot_offset[pair_slot] <= 0)
    return false;
  return nyr_printf(c->w, "\tmovupd\t%%xmm%d, -%d(%%rbp)\n", xmm,
                    c->slot_offset[pair_slot]);
}

static int ny_x64_nir_vec_cached_reg(const ny_x64_nir_ctx_t *c, int value) {
  if (!c || value < 0)
    return -1;
  for (int reg = 0; reg < NY_X64_VEC_CACHE_REGS; ++reg)
    if (c->vec_cached_value[reg] == value)
      return reg;
  return -1;
}

static bool ny_x64_nir_vec_flush_reg(ny_x64_nir_ctx_t *c, int reg) {
  if (!c || reg < 0 || reg >= NY_X64_VEC_CACHE_REGS)
    return false;
  if (!c->vec_cache_dirty[reg])
    return true;
  int value = c->vec_cached_value[reg];
  int slot = ny_x64_nir_slot(c, value);
  int off = slot < 0 ? 0
                     : (c->is_leaf ? c->frame_bytes - (slot + 2) * 8
                                   : c->slot_offset[slot]);
  if (off <= 0 ||
      !(c->is_leaf
            ? nyr_printf(c->w, "\tvmovupd\t%%ymm%d, %d(%%rsp)\n", reg, off)
            : nyr_printf(c->w, "\tvmovupd\t%%ymm%d, -%d(%%rbp)\n", reg, off)))
    return false;
  c->vec_cache_dirty[reg] = false;
  return true;
}

static int ny_x64_nir_vec_alloc_reg(ny_x64_nir_ctx_t *c, int avoid_reg) {
  if (!c)
    return -1;
  for (int reg = 0; reg < NY_X64_VEC_CACHE_REGS; ++reg)
    if (reg != avoid_reg && c->vec_cached_value[reg] < 0)
      return reg;
  int oldest = -1;
  for (int reg = 0; reg < NY_X64_VEC_CACHE_REGS; ++reg)
    if (reg != avoid_reg &&
        (oldest < 0 || c->vec_cache_age[reg] < c->vec_cache_age[oldest]))
      oldest = reg;
  if (oldest >= 0 && !ny_x64_nir_vec_flush_reg(c, oldest))
    return -1;
  return oldest;
}

static void ny_x64_nir_vec_cache_set(ny_x64_nir_ctx_t *c, int reg, int value) {
  if (!c || reg < 0 || reg >= NY_X64_VEC_CACHE_REGS)
    return;
  c->vec_cached_value[reg] = value;
  c->vec_cache_dirty[reg] = false;
  c->vec_cache_age[reg] = ++c->vec_cache_clock;
}

static bool ny_x64_nir_vec4_f64_load(ny_x64_nir_ctx_t *c, int value,
                                     int avoid_reg, int *out_reg) {
  if (!c || !out_reg || value < 0)
    return false;
  int reg = ny_x64_nir_vec_cached_reg(c, value);
  if (reg >= 0) {
    c->vec_cache_age[reg] = ++c->vec_cache_clock;
    *out_reg = reg;
    return true;
  }
  reg = ny_x64_nir_vec_alloc_reg(c, avoid_reg);
  int slot = ny_x64_nir_slot(c, value);
  int off = slot < 0 ? 0
                     : (c->is_leaf ? c->frame_bytes - (slot + 2) * 8
                                   : c->slot_offset[slot]);
  if (reg < 0 || off <= 0 ||
      !(c->is_leaf
            ? nyr_printf(c->w, "\tvmovupd\t%d(%%rsp), %%ymm%d\n", off, reg)
            : nyr_printf(c->w, "\tvmovupd\t-%d(%%rbp), %%ymm%d\n", off, reg)))
    return false;
  ny_x64_nir_vec_cache_set(c, reg, value);
  *out_reg = reg;
  return true;
}

static bool ny_x64_nir_vec4_f64_store(ny_x64_nir_ctx_t *c, int value, int reg) {
  if (!c || value < 0 || reg < 0 || reg >= NY_X64_VEC_CACHE_REGS)
    return false;
  int slot = ny_x64_nir_slot(c, value);
  if (slot < 0)
    return false;
  /*
   * A newly defined vector takes ownership of its colored stack home.
   */
  for (int other = 0; other < NY_X64_VEC_CACHE_REGS; ++other) {
    if (other == reg || c->vec_cached_value[other] < 0 ||
        !c->vec_cache_dirty[other])
      continue;
    if (ny_x64_nir_slot(c, c->vec_cached_value[other]) == slot &&
        !ny_x64_nir_vec_flush_reg(c, other))
      return false;
  }
  c->vec_cached_value[reg] = value;
  c->vec_cache_dirty[reg] = true;
  c->vec_cache_age[reg] = ++c->vec_cache_clock;
  return true;
}

static void ny_x64_nir_vec_release_single_use(ny_x64_nir_ctx_t *c, int value) {
  if (!c || value < 0 || value >= NY_X64_NIR_MAX_SLOTS ||
      c->use_count[value] != 1)
    return;
  int reg = ny_x64_nir_vec_cached_reg(c, value);
  if (reg >= 0) {
    c->vec_cached_value[reg] = -1;
    c->vec_cache_dirty[reg] = false;
  }
}

static bool ny_x64_nir_vec_cache_clear(ny_x64_nir_ctx_t *c) {
  if (!c)
    return false;
  for (int reg = 0; reg < NY_X64_VEC_CACHE_REGS; ++reg) {
    if (!ny_x64_nir_vec_flush_reg(c, reg))
      return false;
    c->vec_cached_value[reg] = -1;
    c->vec_cache_dirty[reg] = false;
  }
  return true;
}

static bool ny_x64_nir_emit_vec4_f64_binop(ny_x64_nir_ctx_t *c,
                                           const nyr_inst_t *in,
                                           const char *mnemonic) {
  if (!c || !in || !mnemonic)
    return false;
  int aslot = ny_x64_nir_slot(c, in->a);
  int bslot = ny_x64_nir_slot(c, in->b);
  int dslot = ny_x64_nir_slot(c, in->dst);
  if (c->target && (c->target->caps & NY_NATIVE_CAP_MACH_AVX)) {
    if (aslot < 0 || bslot < 0 || dslot < 0)
      return false;
    int bo =
        c->is_leaf ? c->frame_bytes - (bslot + 2) * 8 : c->slot_offset[bslot];
    if (bo <= 0)
      return false;
    int a_reg = -1;
    if (!ny_x64_nir_vec4_f64_load(c, in->a, -1, &a_reg))
      return false;
    /*
     * The operation overwrites its left operand's register. Preserve the
     * prior value only when another use still needs its stack home.
     */
    if (c->vec_cached_value[a_reg] == in->a && c->vec_cache_dirty[a_reg] &&
        in->a >= 0 && in->a < NY_X64_NIR_MAX_SLOTS &&
        c->use_count[in->a] != 1 && !ny_x64_nir_vec_flush_reg(c, a_reg))
      return false;
    int b_reg = ny_x64_nir_vec_cached_reg(c, in->b);
    bool op_ok;
    if (b_reg >= 0) {
      c->vec_cache_age[b_reg] = ++c->vec_cache_clock;
      op_ok = nyr_printf(c->w, "\tv%s\t%%ymm%d, %%ymm%d, %%ymm%d\n", mnemonic,
                         b_reg, a_reg, a_reg);
    } else {
      op_ok = c->is_leaf
                  ? nyr_printf(c->w, "\tv%s\t%d(%%rsp), %%ymm%d, %%ymm%d\n",
                               mnemonic, bo, a_reg, a_reg)
                  : nyr_printf(c->w, "\tv%s\t-%d(%%rbp), %%ymm%d, %%ymm%d\n",
                               mnemonic, bo, a_reg, a_reg);
    }
    if (!op_ok)
      return false;
    ny_x64_nir_vec_cache_set(c, a_reg, in->dst);
    if (!ny_x64_nir_vec4_f64_store(c, in->dst, a_reg))
      return false;
    if (b_reg >= 0 && b_reg != a_reg)
      ny_x64_nir_vec_release_single_use(c, in->b);
    return true;
  }
  for (unsigned pair = 0; pair < 2; ++pair) {
    unsigned lane = pair * 2;
    if (!ny_x64_nir_load_f64_pair(c, aslot, lane, 0))
      return false;
    int bpair = bslot + (int)lane;
    if (bpair < 0 || bpair + 1 >= NY_X64_NIR_MAX_SLOTS ||
        c->slot_offset[bpair] <= 0)
      return false;
    bool op_ok = c->is_leaf
                     ? nyr_printf(c->w, "\t%s\t%d(%%rsp), %%xmm0\n", mnemonic,
                                  c->frame_bytes - (bpair + 2) * 8)
                     : nyr_printf(c->w, "\t%s\t-%d(%%rbp), %%xmm0\n", mnemonic,
                                  c->slot_offset[bpair]);
    if (!op_ok || !ny_x64_nir_store_f64_pair(c, dslot, lane, 0))
      return false;
  }
  return true;
}

static bool ny_x64_nir_load_xmm_f32(ny_x64_nir_ctx_t *c, int slot, int xmm) {
  if (slot < 0 || slot >= NY_X64_NIR_MAX_SLOTS || xmm < 0 || xmm > 15)
    return false;
  if (c->is_leaf) {
    int off = c->frame_bytes - (slot + 2) * 8;
    return nyr_printf(c->w, "\tmovss\t%d(%%rsp), %%xmm%d\n", off, xmm);
  }
  if (c->slot_offset[slot] <= 0)
    return false;
  return nyr_printf(c->w, "\tmovss\t-%d(%%rbp), %%xmm%d\n",
                    c->slot_offset[slot], xmm);
}

static bool ny_x64_nir_store_xmm_f32(ny_x64_nir_ctx_t *c, int slot, int xmm) {
  if (slot < 0 || slot >= NY_X64_NIR_MAX_SLOTS || xmm < 0 || xmm > 15)
    return false;
  if (c->is_leaf) {
    int off = c->frame_bytes - (slot + 2) * 8;
    return nyr_printf(c->w, "\tmovss\t%%xmm%d, %d(%%rsp)\n", xmm, off);
  }
  if (c->slot_offset[slot] <= 0)
    return false;
  return nyr_printf(c->w, "\tmovss\t%%xmm%d, -%d(%%rbp)\n", xmm,
                    c->slot_offset[slot]);
}

static void ny_x64_nir_classify_values(ny_x64_nir_ctx_t *c) {
  if (!c || !c->nyir)
    return;
  size_t param_count = c->nyir->param_count < NY_X64_NIR_MAX_SLOTS
                           ? c->nyir->param_count
                           : NY_X64_NIR_MAX_SLOTS;
  for (size_t i = 0; i < param_count; ++i) {
    c->local_f64[i] = c->nyir->param_types[i] == NYIR_PARAM_F64;
    c->local_f32[i] = c->nyir->param_types[i] == NYIR_PARAM_F32;
  }
  for (size_t i = 0; i < c->nyir->len; ++i) {
    const nyr_inst_t *in = &c->nyir->data[i];
    if (c->target && (c->target->caps & NY_NATIVE_CAP_MACH_AVX) &&
        (in->op == NYIR_VEC4_LOAD_F64 || in->op == NYIR_VEC4_STORE_F64 ||
         in->op == NYIR_VEC4_ADD_F64 || in->op == NYIR_VEC4_SUB_F64 ||
         in->op == NYIR_VEC4_MUL_F64 || in->op == NYIR_VEC4_DIV_F64 ||
         in->op == NYIR_VEC4_SET1_F64))
      c->use_avx_vector = true;
    if (in->dst >= 0 && in->dst < NY_X64_NIR_MAX_SLOTS &&
        (nyr_op_is_f64(in->op) || in->op == NYIR_VEC4_LOAD_F64 ||
         in->op == NYIR_VEC4_ADD_F64 || in->op == NYIR_VEC4_SUB_F64 ||
         in->op == NYIR_VEC4_MUL_F64 || in->op == NYIR_VEC4_DIV_F64 ||
         in->op == NYIR_VEC4_FMA_F64 || in->op == NYIR_VEC4_SET1_F64 ||
         in->op == NYIR_VEC4_SHUFFLE_F64 ||
         (in->scalar_class == NYIR_SCALAR_F64 &&
          (in->op == NYIR_CALL || in->op == NYIR_LOAD_I64))))
      c->value_f64[in->dst] = true;
    if (in->dst >= 0 && in->dst < NY_X64_NIR_MAX_SLOTS &&
        (nyr_op_is_f32(in->op) ||
         (in->scalar_class == NYIR_SCALAR_F32 && in->op == NYIR_CALL)))
      c->value_f32[in->dst] = true;
    if ((in->op == NYIR_LOAD_LOCAL || in->op == NYIR_STORE_LOCAL) &&
        in->imm >= 0 && in->imm < NY_X64_NIR_MAX_SLOTS) {
      if (in->scalar_class == NYIR_SCALAR_F64)
        c->local_f64[in->imm] = true;
      else if (in->scalar_class == NYIR_SCALAR_F32)
        c->local_f32[in->imm] = true;
    }
  }
  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t i = 0; i < c->nyir->len; ++i) {
      const nyr_inst_t *in = &c->nyir->data[i];
      if (in->op == NYIR_COPY && in->dst >= 0 && in->a >= 0 &&
          in->dst < NY_X64_NIR_MAX_SLOTS && in->a < NY_X64_NIR_MAX_SLOTS &&
          c->value_f64[in->a] && !c->value_f64[in->dst]) {
        c->value_f64[in->dst] = true;
        changed = true;
      } else if (in->op == NYIR_COPY && in->dst >= 0 && in->a >= 0 &&
                 in->dst < NY_X64_NIR_MAX_SLOTS &&
                 in->a < NY_X64_NIR_MAX_SLOTS && c->value_f32[in->a] &&
                 !c->value_f32[in->dst]) {
        c->value_f32[in->dst] = true;
        changed = true;
      } else if (in->op == NYIR_COPY && in->dst >= 0 && in->a >= 0 &&
                 in->dst < NY_X64_NIR_MAX_SLOTS &&
                 in->a < NY_X64_NIR_MAX_SLOTS && c->value_i64v2[in->a] &&
                 !c->value_i64v2[in->dst]) {
        c->value_i64v2[in->dst] = true;
        changed = true;
      } else if (in->op == NYIR_PHI && in->dst >= 0 &&
                 in->dst < NY_X64_NIR_MAX_SLOTS && !c->value_i64v2[in->dst]) {
        for (size_t p = 0; p < in->phi_incoming_len; ++p) {
          int value = in->phi_incoming[p].value;
          if (value >= 0 && value < NY_X64_NIR_MAX_SLOTS &&
              c->value_i64v2[value]) {
            c->value_i64v2[in->dst] = true;
            changed = true;
            break;
          }
        }
      } else if (in->op == NYIR_LOAD_LOCAL && in->dst >= 0 && in->imm >= 0 &&
                 in->dst < NY_X64_NIR_MAX_SLOTS &&
                 in->imm < NY_X64_NIR_MAX_SLOTS && c->local_f64[in->imm] &&
                 !c->value_f64[in->dst]) {
        c->value_f64[in->dst] = true;
        changed = true;
      } else if (in->op == NYIR_LOAD_LOCAL && in->dst >= 0 && in->imm >= 0 &&
                 in->dst < NY_X64_NIR_MAX_SLOTS &&
                 in->imm < NY_X64_NIR_MAX_SLOTS && c->local_f32[in->imm] &&
                 !c->value_f32[in->dst]) {
        c->value_f32[in->dst] = true;
        changed = true;
      } else if (in->op == NYIR_LOAD_LOCAL && in->dst >= 0 && in->imm >= 0 &&
                 in->dst < NY_X64_NIR_MAX_SLOTS &&
                 in->imm < NY_X64_NIR_MAX_SLOTS && c->value_f64[in->dst] &&
                 !c->local_f64[in->imm]) {
        c->local_f64[in->imm] = true;
        changed = true;
      } else if (in->op == NYIR_LOAD_LOCAL && in->dst >= 0 && in->imm >= 0 &&
                 in->dst < NY_X64_NIR_MAX_SLOTS &&
                 in->imm < NY_X64_NIR_MAX_SLOTS && c->value_f32[in->dst] &&
                 !c->local_f32[in->imm]) {
        c->local_f32[in->imm] = true;
        changed = true;
      } else if (in->op == NYIR_STORE_LOCAL && in->a >= 0 && in->imm >= 0 &&
                 in->a < NY_X64_NIR_MAX_SLOTS &&
                 in->imm < NY_X64_NIR_MAX_SLOTS && c->value_f64[in->a] &&
                 !c->local_f64[in->imm]) {
        c->local_f64[in->imm] = true;
        changed = true;
      } else if (in->op == NYIR_STORE_LOCAL && in->a >= 0 && in->imm >= 0 &&
                 in->a < NY_X64_NIR_MAX_SLOTS &&
                 in->imm < NY_X64_NIR_MAX_SLOTS && c->value_f32[in->a] &&
                 !c->local_f32[in->imm]) {
        c->local_f32[in->imm] = true;
        changed = true;
      }
      if ((in->op == NYIR_ADD_F64 || in->op == NYIR_SUB_F64 ||
           in->op == NYIR_MUL_F64 || in->op == NYIR_DIV_F64 ||
           in->op == NYIR_SQRT_F64 || in->op == NYIR_SIN_F64 ||
           in->op == NYIR_COS_F64 || in->op == NYIR_CMP_F64)) {
        if (in->a >= 0 && in->a < NY_X64_NIR_MAX_SLOTS &&
            !c->value_f64[in->a]) {
          c->value_f64[in->a] = true;
          changed = true;
        }
        if (in->b >= 0 && in->b < NY_X64_NIR_MAX_SLOTS &&
            !c->value_f64[in->b]) {
          c->value_f64[in->b] = true;
          changed = true;
        }
      }
      if ((in->op == NYIR_ADD_F32 || in->op == NYIR_SUB_F32 ||
           in->op == NYIR_MUL_F32 || in->op == NYIR_DIV_F32 ||
           in->op == NYIR_CMP_F32)) {
        if (in->a >= 0 && in->a < NY_X64_NIR_MAX_SLOTS &&
            !c->value_f32[in->a]) {
          c->value_f32[in->a] = true;
          changed = true;
        }
        if (in->b >= 0 && in->b < NY_X64_NIR_MAX_SLOTS &&
            !c->value_f32[in->b]) {
          c->value_f32[in->b] = true;
          changed = true;
        }
      }
    }
  }
}

static const char *ny_x64_nir_setcc(nyr_cmp_t cmp) {
  switch (cmp) {
  case NYIR_CMP_EQ:
    return "sete";
  case NYIR_CMP_NE:
    return "setne";
  case NYIR_CMP_LT:
    return "setl";
  case NYIR_CMP_LE:
    return "setle";
  case NYIR_CMP_GT:
    return "setg";
  case NYIR_CMP_GE:
    return "setge";
  }
  return "sete";
}

static const char *ny_x64_nir_jcc(nyr_cmp_t cmp) {
  switch (cmp) {
  case NYIR_CMP_EQ:
    return "je";
  case NYIR_CMP_NE:
    return "jne";
  case NYIR_CMP_LT:
    return "jl";
  case NYIR_CMP_LE:
    return "jle";
  case NYIR_CMP_GT:
    return "jg";
  case NYIR_CMP_GE:
    return "jge";
  }
  return "jne";
}

static const char *ny_x64_nir_cmov(nyr_cmp_t cmp) {
  switch (cmp) {
  case NYIR_CMP_EQ:
    return "cmove";
  case NYIR_CMP_NE:
    return "cmovne";
  case NYIR_CMP_LT:
    return "cmovl";
  case NYIR_CMP_LE:
    return "cmovle";
  case NYIR_CMP_GT:
    return "cmovg";
  case NYIR_CMP_GE:
    return "cmovge";
  }
  return "cmovne";
}

static const char *ny_x64_nir_f64_setcc(nyr_cmp_t cmp) {
  switch (cmp) {
  case NYIR_CMP_EQ:
    return "sete";
  case NYIR_CMP_NE:
    return "setne";
  case NYIR_CMP_LT:
    return "setb";
  case NYIR_CMP_LE:
    return "setbe";
  case NYIR_CMP_GT:
    return "seta";
  case NYIR_CMP_GE:
    return "setae";
  }
  return "sete";
}

/*
 * Check whether value_id has a full-width constant/COPY definition in this
 * use's safe region.
 */
static bool ny_x64_nir_try_const_i64(ny_x64_nir_ctx_t *c, int value_id,
                                     size_t use_idx, int64_t *out) {
  (void)use_idx;
  return ny_x64_nir_find_const_val(c, value_id, out);
}

/*
 * Emit the magic-reciprocal sequence for x / divisor (or x % divisor) where
 * divisor is a known non-power-of-2 constant and in->a holds the i64 dividend.
 * Leaves the result in %rax and stores it to in->dst.
 */
static bool ny_x64_nir_emit_sdiv_magic(ny_x64_nir_ctx_t *c,
                                       const nyr_inst_t *in, int64_t divisor,
                                       bool want_rem) {
  uint64_t ad =
      divisor >= 0 ? (uint64_t)divisor : (uint64_t)(-(divisor + 1)) + 1;
  int64_t magic = 0;
  unsigned s = 0;
  nyr_sdiv_magic_i64(ad, &magic, &s);
  bool negate = divisor < 0;
  if (!ny_x64_nir_load_value(c, in->a) ||
      !nyr_put(c->w, "\tmovq\t%rax, %r9\n") ||
      !nyr_printf(c->w, "\tmovabsq\t$%" PRId64 ", %%r10\n", magic) ||
      !nyr_put(c->w, "\timulq\t%r10\n\tmovq\t%rdx, %rax\n"))
    return false;
  if (magic < 0 && !nyr_put(c->w, "\taddq\t%r9, %rax\n"))
    return false;
  if (s > 0 && !nyr_printf(c->w, "\tsarq\t$%u, %%rax\n", s))
    return false;
  if (!nyr_put(c->w,
               "\tmovq\t%r9, %rcx\n\tsarq\t$63, %rcx\n\tsubq\t%rcx, %rax\n"))
    return false;
  if (negate && !nyr_put(c->w, "\tnegq\t%rax\n"))
    return false;
  if (want_rem) {
    if (!nyr_put(c->w, "\tmovq\t%rax, %r10\n\tmovq\t%r9, %rax\n"))
      return false;
    if ((int64_t)(int32_t)divisor == divisor) {
      if (!nyr_printf(c->w, "\timulq\t$%" PRId64 ", %%r10, %%r10\n", divisor))
        return false;
    } else if (!nyr_printf(c->w,
                           "\tmovabsq\t$%" PRId64
                           ", %%r11\n\timulq\t%%r11, %%r10\n",
                           divisor)) {
      return false;
    }
    if (!nyr_put(c->w, "\tsubq\t%r10, %rax\n"))
      return false;
  }
  return ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
}

static bool ny_x64_nir_emit_inst_raw(ny_x64_nir_ctx_t *c, const nyr_inst_t *in,
                                     size_t idx) {
  if (in->dst >= 0 && in->dst < NY_X64_NIR_MAX_SLOTS &&
      c->folded_addr_temp[in->dst])
    return true;
  switch (in->op) {
  case NYIR_NOP:
    return true;
  case NYIR_CONST_I64: {
    int64_t imm = 0;
    if (in->dst < 0)
      return true;
    ny_x64_nir_slot(c, in->dst);
    if (ny_x64_nir_can_fuse_const(c, in->dst, idx, &imm)) {
      c->fused_const[in->dst] = true;
      c->fused_imm[in->dst] = imm;
      return true;
    }
    return ny_x64_nir_materialize_i64(c, in->imm) &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  }
  case NYIR_CONST_F64:
  case NYIR_CONST_F32:
    if (in->dst < 0)
      return true;
    ny_x64_nir_slot(c, in->dst);
    return nyr_printf(c->w, "\tmovabsq\t$%" PRId64 ", %%rax\n", in->imm) &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  case NYIR_COPY: {
    int64_t imm = 0;
    if (in->dst < 0)
      return true;
    if (ny_x64_nir_can_fuse_const(c, in->dst, idx, &imm)) {
      c->fused_const[in->dst] = true;
      c->fused_imm[in->dst] = imm;
      return true;
    }
    if (ny_x64_nir_slot(c, in->dst) == ny_x64_nir_slot(c, in->a))
      return true;
    return ny_x64_nir_load_value(c, in->a) &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  }
  case NYIR_LOAD_LOCAL: {
    if (in->dst < 0)
      return true;
    /*
     * Load from a local (param/mut) slot into a value slot.
     */
    if (in->imm < 0 || (int)in->imm >= c->max_local_slot) {
      nyr_set_error(c->err, c->err_len,
                    "nyir x86-64: load.local invalid slot %" PRId64, in->imm);
      return false;
    }
    {
      int dst_slot = ny_x64_nir_slot(c, in->dst);
      int local_reg = c->home_reg[in->imm];
      if (local_reg >= 0 && dst_slot >= 0 && c->home_reg[dst_slot] == local_reg)
        return true;
    }
    c->slot_offset[in->imm] = (in->imm + 1) * 8;
    if (getenv("NYTRIX_NATIVE_TRACE_LOCALS"))
      ny_trace_line(
          "native-local", "load slot=%" PRId64 " f64=%d f32=%d dst=%d", in->imm,
          in->imm < NY_X64_NIR_MAX_SLOTS ? c->local_f64[in->imm] : 0,
          in->imm < NY_X64_NIR_MAX_SLOTS ? c->local_f32[in->imm] : 0, in->dst);
    if (in->imm < NY_X64_NIR_MAX_SLOTS && c->local_f64[in->imm])
      return ny_x64_nir_load_xmm(c, (int)in->imm, 0) &&
             ny_x64_nir_store_xmm(c, ny_x64_nir_slot(c, in->dst), 0);
    if (in->imm < NY_X64_NIR_MAX_SLOTS && c->local_f32[in->imm])
      return ny_x64_nir_load_xmm_f32(c, (int)in->imm, 0) &&
             ny_x64_nir_store_xmm_f32(c, ny_x64_nir_slot(c, in->dst), 0);
    return ny_x64_nir_load(c, (int)in->imm) &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  }
  case NYIR_ADDR_LOCAL: {
    if (in->dst < 0)
      return true;
    if (in->imm < 0 || (int)in->imm >= c->max_local_slot) {
      nyr_set_error(c->err, c->err_len,
                    "nyir x86-64: addr.local invalid slot %" PRId64, in->imm);
      return false;
    }
    c->slot_offset[in->imm] = (in->imm + 1) * 8;
    /*
     * Leaf functions do not establish %rbp.  Use the same slot spelling as
     * loads/stores so addr_of(local) aliases the actual O0 frame slot.
     */
    char addr[64];
    ny_x64_nir_slot_str(c, (int)in->imm, addr, sizeof(addr));
    return nyr_printf(c->w, "\tleaq\t%s, %%rax\n", addr) &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  }
  case NYIR_ADDR_SYMBOL:
    if (in->dst < 0)
      return true;
    if (!in->symbol || !in->symbol[0]) {
      nyr_set_error(c->err, c->err_len,
                    "nyir x86-64: addr.symbol missing symbol name");
      return false;
    }
    return nyr_printf(c->w, "\tleaq\t%s(%%rip), %%rax\n", in->symbol) &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  case NYIR_ALLOCA:
    if (in->dst < 0)
      return true;
    return nyr_printf(c->w,
                      "\tsubq\t$%" PRId64
                      ", %%rsp\n\tandq\t$-16, %%rsp\n\tmovq\t%%rsp, %%rax\n",
                      in->imm) &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  case NYIR_COPY_STRUCT:
    if (in->imm <= 0)
      return true;
    return ny_x64_nir_load_value(c, in->b) &&
           nyr_put(c->w, "\tmovq\t%rax, %rsi\n") &&
           ny_x64_nir_load_value(c, in->a) &&
           nyr_put(c->w, "\tmovq\t%rax, %rdi\n") &&
           nyr_printf(c->w, "\tmovq\t$%" PRId64 ", %%rcx\n\trep movsb\n",
                      in->imm);
  case NYIR_CAPTURE_RET:
    if (in->dst < 0)
      return true;
    switch (in->imm) {
    case 0:
      if (!nyr_put(c->w, "\tmovq\t%rdx, %rax\n"))
        return false;
      break;
    case 1:
      break;
    case 2:
      if (!nyr_put(c->w, "\tmovq\t%xmm0, %rax\n"))
        return false;
      break;
    case 3:
      if (!nyr_put(c->w, "\tmovq\t%xmm1, %rax\n"))
        return false;
      break;
    default:
      nyr_set_error(c->err, c->err_len,
                    "nyir x86-64: invalid capture.ret selector");
      return false;
    }
    return ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  case NYIR_VEC4_ADD_F64: {
    return ny_x64_nir_emit_vec4_f64_binop(c, in, "addpd");
  }
  case NYIR_VEC4_SUB_F64: {
    return ny_x64_nir_emit_vec4_f64_binop(c, in, "subpd");
  }
  case NYIR_VEC4_MUL_F64: {
    return ny_x64_nir_emit_vec4_f64_binop(c, in, "mulpd");
  }
  case NYIR_VEC4_DIV_F64: {
    return ny_x64_nir_emit_vec4_f64_binop(c, in, "divpd");
  }
  case NYIR_VEC4_FMA_F64: {
    int aslot = ny_x64_nir_slot(c, in->a);
    int bslot = ny_x64_nir_slot(c, in->b);
    int cslot = ny_x64_nir_slot(c, in->c);
    int dslot = ny_x64_nir_slot(c, in->dst);
    for (unsigned lane = 0; lane < 4; ++lane) {
      if (!ny_x64_nir_load_f64_lane(c, aslot, lane, 0) ||
          !ny_x64_nir_load_f64_lane(c, bslot, lane, 1) ||
          !ny_x64_nir_load_f64_lane(c, cslot, lane, 2) ||
          !nyr_printf(c->w, "\tcall\t%sfma\n", c->target->symbol_prefix) ||
          !ny_x64_nir_store_f64_lane(c, dslot, lane, 0))
        return false;
    }
    return true;
  }
  case NYIR_VEC4_SET1_F64: {
    int scalar = ny_x64_nir_slot(c, in->a);
    int dst = ny_x64_nir_slot(c, in->dst);
    if (c->target && (c->target->caps & NY_NATIVE_CAP_MACH_AVX)) {
      if (scalar < 0 || dst < 0)
        return false;
      int src = c->is_leaf ? c->frame_bytes - (scalar + 2) * 8
                           : c->slot_offset[scalar];
      int reg = ny_x64_nir_vec_alloc_reg(c, -1);
      if (src <= 0 || reg < 0 ||
          !(c->is_leaf
                ? nyr_printf(c->w, "\tvbroadcastsd\t%d(%%rsp), %%ymm%d\n", src,
                             reg)
                : nyr_printf(c->w, "\tvbroadcastsd\t-%d(%%rbp), %%ymm%d\n", src,
                             reg)))
        return false;
      ny_x64_nir_vec_cache_set(c, reg, in->dst);
      return ny_x64_nir_vec4_f64_store(c, in->dst, reg);
    }
    if (!ny_x64_nir_load_xmm(c, scalar, 0))
      return false;
    for (unsigned lane = 0; lane < 4; ++lane)
      if (!ny_x64_nir_store_f64_lane(c, dst, lane, 0))
        return false;
    return true;
  }
  case NYIR_VEC4_LOAD_F64: {
    int dst = ny_x64_nir_slot(c, in->dst);
    bool indexed = in->a >= 0 && in->a < NY_X64_NIR_MAX_SLOTS &&
                   c->indexed_base[in->a] >= 0 && c->indexed_index[in->a] >= 0;
    if (indexed && (!ny_x64_nir_load_value(c, c->indexed_base[in->a]) ||
                    !nyr_put(c->w, "\tmovq\t%rax, %rcx\n") ||
                    !ny_x64_nir_load_value(c, c->indexed_index[in->a])))
      return false;
    if (indexed) {
      if (c->target && (c->target->caps & NY_NATIVE_CAP_MACH_AVX)) {
        if (dst < 0)
          return false;
        int reg = ny_x64_nir_vec_alloc_reg(c, -1);
        if (reg < 0 ||
            !nyr_printf(c->w, "\tvmovupd\t(%%rcx,%%rax,8), %%ymm%d\n", reg))
          return false;
        ny_x64_nir_vec_cache_set(c, reg, in->dst);
        return ny_x64_nir_vec4_f64_store(c, in->dst, reg);
      }
      for (unsigned pair = 0; pair < 2; ++pair) {
        if (!nyr_printf(c->w, "\tmovupd\t%u(%%rcx,%%rax,8), %%xmm0\n",
                        pair * 16) ||
            !ny_x64_nir_store_f64_pair(c, dst, pair * 2, 0))
          return false;
      }
      return true;
    }
    if (!ny_x64_nir_load_value(c, in->a))
      return false;
    if (c->target && (c->target->caps & NY_NATIVE_CAP_MACH_AVX)) {
      if (dst < 0)
        return false;
      int reg = ny_x64_nir_vec_alloc_reg(c, -1);
      if (reg < 0 || !nyr_printf(c->w, "\tvmovupd\t(%%rax), %%ymm%d\n", reg))
        return false;
      ny_x64_nir_vec_cache_set(c, reg, in->dst);
      return ny_x64_nir_vec4_f64_store(c, in->dst, reg);
    }
    for (unsigned pair = 0; pair < 2; ++pair)
      if (!nyr_printf(c->w, "\tmovupd\t%u(%%rax), %%xmm0\n", pair * 16) ||
          !ny_x64_nir_store_f64_pair(c, dst, pair * 2, 0))
        return false;
    return true;
  }
  case NYIR_VEC4_STORE_F64: {
    int src = ny_x64_nir_slot(c, in->b);
    bool indexed = in->a >= 0 && in->a < NY_X64_NIR_MAX_SLOTS &&
                   c->indexed_base[in->a] >= 0 && c->indexed_index[in->a] >= 0;
    if (indexed && (!ny_x64_nir_load_value(c, c->indexed_base[in->a]) ||
                    !nyr_put(c->w, "\tmovq\t%rax, %rcx\n") ||
                    !ny_x64_nir_load_value(c, c->indexed_index[in->a])))
      return false;
    if (indexed) {
      if (c->target && (c->target->caps & NY_NATIVE_CAP_MACH_AVX)) {
        if (src < 0)
          return false;
        int reg = -1;
        if (!ny_x64_nir_vec4_f64_load(c, in->b, -1, &reg) ||
            !nyr_printf(c->w, "\tvmovupd\t%%ymm%d, (%%rcx,%%rax,8)\n", reg))
          return false;
        ny_x64_nir_vec_release_single_use(c, in->b);
        return true;
      }
      for (unsigned pair = 0; pair < 2; ++pair)
        if (!ny_x64_nir_load_f64_pair(c, src, pair * 2, 0) ||
            !nyr_printf(c->w, "\tmovupd\t%%xmm0, %u(%%rcx,%%rax,8)\n",
                        pair * 16))
          return false;
      return true;
    }
    if (!ny_x64_nir_load_value(c, in->a))
      return false;
    if (c->target && (c->target->caps & NY_NATIVE_CAP_MACH_AVX)) {
      if (src < 0)
        return false;
      int reg = -1;
      if (!ny_x64_nir_vec4_f64_load(c, in->b, -1, &reg) ||
          !nyr_printf(c->w, "\tvmovupd\t%%ymm%d, (%%rax)\n", reg))
        return false;
      ny_x64_nir_vec_release_single_use(c, in->b);
      return true;
    }
    for (unsigned pair = 0; pair < 2; ++pair)
      if (!ny_x64_nir_load_f64_pair(c, src, pair * 2, 0) ||
          !nyr_printf(c->w, "\tmovupd\t%%xmm0, %u(%%rax)\n", pair * 16))
        return false;
    return true;
  }
  case NYIR_VEC4_SHUFFLE_F64: {
    int src = ny_x64_nir_slot(c, in->a);
    int dst = ny_x64_nir_slot(c, in->dst);
    for (unsigned lane = 0; lane < 4; ++lane) {
      unsigned selected = (unsigned)(((uint64_t)in->imm >> (lane * 2u)) & 3u);
      if (!ny_x64_nir_load_f64_lane(c, src, selected, 0) ||
          !ny_x64_nir_store_f64_lane(c, dst, lane, 0))
        return false;
    }
    return true;
  }
  case NYIR_VEC4_REDUCE_ADD_F64: {
    int vec = ny_x64_nir_slot(c, in->b);
    int dst = ny_x64_nir_slot(c, in->dst);
    if (!ny_x64_nir_load_xmm(c, ny_x64_nir_slot(c, in->a), 0))
      return false;
    for (unsigned lane = 0; lane < 4; ++lane)
      if (!ny_x64_nir_load_f64_lane(c, vec, lane, 1) ||
          !nyr_put(c->w, "\taddsd\t%xmm1, %xmm0\n"))
        return false;
    return ny_x64_nir_store_xmm(c, dst, 0);
  }
  case NYIR_VEC8_ADD_F32: {
    int bslot = ny_x64_nir_slot(c, in->b);
    return ny_x64_nir_load_xmm_f32(c, ny_x64_nir_slot(c, in->a), 0) &&
           nyr_printf(c->w, "\tmovaps\t-%d(%%rbp), %%xmm1\n",
                      c->slot_offset[bslot]) &&
           nyr_put(c->w, "\taddps\t%xmm1, %xmm0\n") &&
           ny_x64_nir_store_xmm_f32(c, ny_x64_nir_slot(c, in->dst), 0);
  }
  case NYIR_VEC8_SUB_F32: {
    int bslot = ny_x64_nir_slot(c, in->b);
    return ny_x64_nir_load_xmm_f32(c, ny_x64_nir_slot(c, in->a), 0) &&
           nyr_printf(c->w, "\tmovaps\t-%d(%%rbp), %%xmm1\n",
                      c->slot_offset[bslot]) &&
           nyr_put(c->w, "\tsubps\t%xmm1, %xmm0\n") &&
           ny_x64_nir_store_xmm_f32(c, ny_x64_nir_slot(c, in->dst), 0);
  }
  case NYIR_VEC8_MUL_F32: {
    int bslot = ny_x64_nir_slot(c, in->b);
    return ny_x64_nir_load_xmm_f32(c, ny_x64_nir_slot(c, in->a), 0) &&
           nyr_printf(c->w, "\tmovaps\t-%d(%%rbp), %%xmm1\n",
                      c->slot_offset[bslot]) &&
           nyr_put(c->w, "\tmulps\t%xmm1, %xmm0\n") &&
           ny_x64_nir_store_xmm_f32(c, ny_x64_nir_slot(c, in->dst), 0);
  }
  case NYIR_VEC8_DIV_F32: {
    int bslot = ny_x64_nir_slot(c, in->b);
    return ny_x64_nir_load_xmm_f32(c, ny_x64_nir_slot(c, in->a), 0) &&
           nyr_printf(c->w, "\tmovaps\t-%d(%%rbp), %%xmm1\n",
                      c->slot_offset[bslot]) &&
           nyr_put(c->w, "\tdivps\t%xmm1, %xmm0\n") &&
           ny_x64_nir_store_xmm_f32(c, ny_x64_nir_slot(c, in->dst), 0);
  }
  case NYIR_VEC8_FMA_F32: {
    int bslot = ny_x64_nir_slot(c, in->b);
    return ny_x64_nir_load_xmm_f32(c, ny_x64_nir_slot(c, in->a), 0) &&
           nyr_printf(c->w, "\tmovaps\t-%d(%%rbp), %%xmm1\n",
                      c->slot_offset[bslot]) &&
           ny_x64_nir_load_xmm_f32(c, ny_x64_nir_slot(c, in->c), 2) &&
           nyr_put(c->w, "\tmulps\t%xmm1, %xmm0\n\taddps\t%xmm2, %xmm0\n") &&
           ny_x64_nir_store_xmm_f32(c, ny_x64_nir_slot(c, in->dst), 0);
  }
  case NYIR_VEC8_SET1_F32: {
    return ny_x64_nir_load_xmm_f32(c, ny_x64_nir_slot(c, in->a), 0) &&
           nyr_put(c->w, "\tshufps\t$0, %xmm0, %xmm0\n") &&
           ny_x64_nir_store_xmm_f32(c, ny_x64_nir_slot(c, in->dst), 0);
  }
  case NYIR_VEC8_LOAD_F32: {
    return ny_x64_nir_load_value(c, in->a) &&
           nyr_put(c->w, "\tmovups\t(%rax), %xmm0\n") &&
           ny_x64_nir_store_xmm_f32(c, ny_x64_nir_slot(c, in->dst), 0);
  }
  case NYIR_VEC8_STORE_F32: {
    return ny_x64_nir_load_value(c, in->a) &&
           ny_x64_nir_load_xmm_f32(c, ny_x64_nir_slot(c, in->b), 0) &&
           nyr_put(c->w, "\tmovups\t%xmm0, (%rax)\n");
  }
  case NYIR_VEC8_SHUFFLE_F32: {
    return ny_x64_nir_load_xmm_f32(c, ny_x64_nir_slot(c, in->a), 0) &&
           nyr_printf(c->w, "\tshufps\t$%" PRId64 ", %%xmm0, %%xmm0\n",
                      in->imm & 0xff) &&
           ny_x64_nir_store_xmm_f32(c, ny_x64_nir_slot(c, in->dst), 0);
  }
  case NYIR_VEC4_SET1_I64: {
    return ny_x64_nir_load_value(c, in->a) &&
           nyr_put(c->w, "\tmovq\t%rax, %xmm0\n"
                         "\tpunpcklqdq\t%xmm0, %xmm0\n") &&
           ny_x64_nir_store_i64v2(c, ny_x64_nir_slot(c, in->dst), 0);
  }
  case NYIR_VEC4_ADD_I64: {
    int bslot = ny_x64_nir_slot(c, in->b);
    return ny_x64_nir_load_i64v2(c, ny_x64_nir_slot(c, in->a), 0) &&
           ny_x64_nir_load_i64v2(c, bslot, 1) &&
           nyr_put(c->w, "\tpaddq\t%xmm1, %xmm0\n") &&
           ny_x64_nir_store_i64v2(c, ny_x64_nir_slot(c, in->dst), 0);
  }
  case NYIR_VEC4_SUB_I64: {
    int bslot = ny_x64_nir_slot(c, in->b);
    return ny_x64_nir_load_i64v2(c, ny_x64_nir_slot(c, in->a), 0) &&
           ny_x64_nir_load_i64v2(c, bslot, 1) &&
           nyr_put(c->w, "\tpsubq\t%xmm1, %xmm0\n") &&
           ny_x64_nir_store_i64v2(c, ny_x64_nir_slot(c, in->dst), 0);
  }
  case NYIR_VEC4_AND_I64: {
    int bslot = ny_x64_nir_slot(c, in->b);
    return ny_x64_nir_load_i64v2(c, ny_x64_nir_slot(c, in->a), 0) &&
           ny_x64_nir_load_i64v2(c, bslot, 1) &&
           nyr_put(c->w, "\tpand\t%xmm1, %xmm0\n") &&
           ny_x64_nir_store_i64v2(c, ny_x64_nir_slot(c, in->dst), 0);
  }
  case NYIR_VEC4_OR_I64: {
    int bslot = ny_x64_nir_slot(c, in->b);
    return ny_x64_nir_load_i64v2(c, ny_x64_nir_slot(c, in->a), 0) &&
           ny_x64_nir_load_i64v2(c, bslot, 1) &&
           nyr_put(c->w, "\tpor\t%xmm1, %xmm0\n") &&
           ny_x64_nir_store_i64v2(c, ny_x64_nir_slot(c, in->dst), 0);
  }
  case NYIR_VEC4_XOR_I64: {
    int bslot = ny_x64_nir_slot(c, in->b);
    return ny_x64_nir_load_i64v2(c, ny_x64_nir_slot(c, in->a), 0) &&
           ny_x64_nir_load_i64v2(c, bslot, 1) &&
           nyr_put(c->w, "\tpxor\t%xmm1, %xmm0\n") &&
           ny_x64_nir_store_i64v2(c, ny_x64_nir_slot(c, in->dst), 0);
  }
  case NYIR_VEC4_LOAD_I64: {
    if (in->a >= 0 && in->a < NY_X64_NIR_MAX_SLOTS &&
        c->indexed_base[in->a] >= 0 && c->indexed_index[in->a] >= 0) {
      return ny_x64_nir_load_value(c, c->indexed_base[in->a]) &&
             nyr_put(c->w, "\tmovq\t%rax, %rcx\n") &&
             ny_x64_nir_load_value(c, c->indexed_index[in->a]) &&
             nyr_printf(c->w, "\tmovdqu\t(%%rcx,%%rax,8), %%xmm0\n") &&
             ny_x64_nir_store_i64v2(c, ny_x64_nir_slot(c, in->dst), 0);
    }
    return ny_x64_nir_load_value(c, in->a) &&
           nyr_put(c->w, "\tmovdqu\t(%rax), %xmm0\n") &&
           ny_x64_nir_store_i64v2(c, ny_x64_nir_slot(c, in->dst), 0);
  }
  case NYIR_VEC4_STORE_I64: {
    if (in->a >= 0 && in->a < NY_X64_NIR_MAX_SLOTS &&
        c->indexed_base[in->a] >= 0 && c->indexed_index[in->a] >= 0) {
      return ny_x64_nir_load_value(c, c->indexed_base[in->a]) &&
             nyr_put(c->w, "\tmovq\t%rax, %rcx\n") &&
             ny_x64_nir_load_value(c, c->indexed_index[in->a]) &&
             nyr_put(c->w, "\tmovq\t%rax, %r11\n") &&
             ny_x64_nir_load_i64v2(c, ny_x64_nir_slot(c, in->b), 0) &&
             nyr_put(c->w, "\tmovdqu\t%xmm0, (%rcx,%r11,8)\n");
    }
    return ny_x64_nir_load_value(c, in->a) &&
           ny_x64_nir_load_i64v2(c, ny_x64_nir_slot(c, in->b), 0) &&
           nyr_put(c->w, "\tmovdqu\t%xmm0, (%rax)\n");
  }
  case NYIR_VEC4_REDUCE_ADD_I64: {
    int vec = ny_x64_nir_slot(c, in->b);
    if (vec < 0 || vec + 1 >= NY_X64_NIR_MAX_SLOTS ||
        c->slot_offset[vec] <= 0 || c->slot_offset[vec + 1] <= 0 ||
        !ny_x64_nir_load_value(c, in->a) ||
        !nyr_put(c->w, "\tmovq\t%rax, %r11\n") ||
        !nyr_printf(c->w, "\taddq\t-%d(%%rbp), %%r11\n", c->slot_offset[vec]) ||
        !nyr_printf(c->w, "\taddq\t-%d(%%rbp), %%r11\n",
                    c->slot_offset[vec + 1]) ||
        !nyr_put(c->w, "\tmovq\t%r11, %rax\n"))
      return false;
    return ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  }
  case NYIR_VEC8_LOAD_I64: {
    int dst = ny_x64_nir_slot(c, in->dst);
    if (dst < 0 || !ny_x64_nir_load_value(c, in->a) ||
        !nyr_put(c->w, "\tmovq\t%rax, %r11\n"))
      return false;
    for (unsigned lane = 0; lane < 4; ++lane) {
      if (!nyr_printf(c->w, "\tmovq\t%u(%%r11), %%rax\n", lane * 8) ||
          !ny_x64_nir_store(c, dst + (int)lane))
        return false;
    }
    return true;
  }
  case NYIR_VEC8_STORE_I64: {
    int src = ny_x64_nir_slot(c, in->b);
    if (src < 0 || !ny_x64_nir_load_value(c, in->a) ||
        !nyr_put(c->w, "\tmovq\t%rax, %r11\n"))
      return false;
    for (unsigned lane = 0; lane < 4; ++lane) {
      if (!ny_x64_nir_load(c, src + (int)lane) ||
          !nyr_printf(c->w, "\tmovq\t%%rax, %u(%%r11)\n", lane * 8))
        return false;
    }
    return true;
  }
  case NYIR_VEC4_SHL_I64: {
    int shift = 0;
    if (ny_x64_nir_try_shift_imm(c, in->b, idx, &shift))
      return ny_x64_nir_load_xmm(c, ny_x64_nir_slot(c, in->a), 0) &&
             nyr_printf(c->w,
                        "\tmovdqa\t%%xmm0, %%xmm1\n\tmovq\t$%d, %%r11\n"
                        "\tmovq\t%%r11, %%xmm2\n"
                        "\tpsllq\t%%xmm2, %%xmm0\n",
                        shift) &&
             ny_x64_nir_store_xmm(c, ny_x64_nir_slot(c, in->dst), 0);
    return ny_x64_nir_load_xmm(c, ny_x64_nir_slot(c, in->a), 0) &&
           ny_x64_nir_load_value(c, in->b) &&
           nyr_put(c->w, "\tmovq\t%rax, %xmm1\n") &&
           nyr_put(c->w, "\tshufps\t$0xe0, %xmm1, %xmm1\n") &&
           nyr_put(c->w, "\tpsllq\t%xmm1, %xmm0\n") &&
           ny_x64_nir_store_xmm(c, ny_x64_nir_slot(c, in->dst), 0);
  }
  case NYIR_VEC4_SAR_I64:
    /*
     * No producer lowers this opcode today, and there is no single
     * SSE2/AVX2 instruction for packed-qword arithmetic right shift
     * (vpsraq is AVX512-only). Fail loudly if one appears rather than
     * executing garbage; a psrad-per-dword decomposition belongs with
     * whichever feature introduces a real producer.
     */
    nyr_set_error(c->err, c->err_len,
                  "nyir x86-64: VEC4_SAR_I64 has no producer and no "
                  "SSE2/AVX2 form");
    return false;
  case NYIR_STORE_LOCAL: {
    if (in->imm < 0 || (int)in->imm >= c->max_local_slot) {
      nyr_set_error(c->err, c->err_len,
                    "nyir x86-64: store.local invalid slot %" PRId64, in->imm);
      return false;
    }
    {
      int src_slot = ny_x64_nir_slot(c, in->a);
      int local_reg = c->home_reg[in->imm];
      if (local_reg >= 0 && src_slot >= 0 && c->home_reg[src_slot] == local_reg)
        return true;
      if (local_reg >= 0 && src_slot >= 0 && c->home_reg[src_slot] >= 0) {
        static const char *const regs[] = {"%rbx", "%r12", "%r13", "%r14",
                                           "%r15"};
        int src_reg = c->home_reg[src_slot];
        if (src_reg >= NY_X64_LOOP_REG_COUNT ||
            local_reg >= NY_X64_LOOP_REG_COUNT)
          return false;
        return nyr_printf(c->w, "\tmovq\t%s, %s\n", regs[src_reg],
                          regs[local_reg]);
      }
    }
    c->slot_offset[in->imm] = (in->imm + 1) * 8;
    if (getenv("NYTRIX_NATIVE_TRACE_LOCALS"))
      ny_trace_line(
          "native-local",
          "store slot=%" PRId64 " f64=%d f32=%d "
          "value=%d vf64=%d",
          in->imm, in->imm < NY_X64_NIR_MAX_SLOTS ? c->local_f64[in->imm] : 0,
          in->imm < NY_X64_NIR_MAX_SLOTS ? c->local_f32[in->imm] : 0, in->a,
          in->a >= 0 && in->a < NY_X64_NIR_MAX_SLOTS ? c->value_f64[in->a] : 0);
    if (in->a >= 0 && in->a < NY_X64_NIR_MAX_SLOTS && c->value_f64[in->a])
      return ny_x64_nir_load_xmm(c, ny_x64_nir_slot(c, in->a), 0) &&
             ny_x64_nir_store_xmm(c, (int)in->imm, 0);
    if (in->a >= 0 && in->a < NY_X64_NIR_MAX_SLOTS && c->value_f32[in->a])
      return ny_x64_nir_load_xmm_f32(c, ny_x64_nir_slot(c, in->a), 0) &&
             ny_x64_nir_store_xmm_f32(c, (int)in->imm, 0);
    return ny_x64_nir_load_value(c, in->a) && ny_x64_nir_store(c, (int)in->imm);
  }
  case NYIR_LOAD_I64:
    if (in->a >= 0 && in->a < NY_X64_NIR_MAX_SLOTS &&
        c->indexed_base[in->a] >= 0 && c->indexed_index[in->a] >= 0) {
      const char *load = (in->flags & NYIR_INST_F_MEM_BYTE) ? "movzbl" : "movq";
      return ny_x64_nir_load_value(c, c->indexed_base[in->a]) &&
             nyr_put(c->w, "\tmovq\t%rax, %rcx\n") &&
             ny_x64_nir_load_value(c, c->indexed_index[in->a]) &&
             nyr_printf(c->w, "\t%s\t(%%rcx,%%rax,8), %%rax\n", load) &&
             ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    }
    return ny_x64_nir_load_value(c, in->a) &&
           ((in->flags & NYIR_INST_F_MEM_BYTE)
                ? nyr_put(c->w, "\tmovzbl\t(%rax), %eax\n")
                : nyr_put(c->w, "\tmovq\t(%rax), %rax\n")) &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  case NYIR_STORE_I64: {
    const char *st = (in->flags & NYIR_INST_F_MEM_BYTE)
                         ? "\tmovb\t%r10b, (%rax)\n"
                         : "\tmovq\t%r10, (%rax)\n";
    if (in->a >= 0 && in->a < NY_X64_NIR_MAX_SLOTS &&
        c->indexed_base[in->a] >= 0 && c->indexed_index[in->a] >= 0) {
      const char *indexed_store = (in->flags & NYIR_INST_F_MEM_BYTE)
                                      ? "\tmovb\t%r11b, (%rcx,%rax,8)\n"
                                      : "\tmovq\t%r11, (%rcx,%rax,8)\n";
      return ny_x64_nir_load_value(c, in->c) &&
             nyr_put(c->w, "\tmovq\t%rax, %r11\n") &&
             ny_x64_nir_load_value(c, c->indexed_base[in->a]) &&
             nyr_put(c->w, "\tmovq\t%rax, %rcx\n") &&
             ny_x64_nir_load_value(c, c->indexed_index[in->a]) &&
             nyr_put(c->w, indexed_store);
    }
    return ny_x64_nir_load_value(c, in->c) &&
           nyr_put(c->w, "\tmovq\t%rax, %r10\n") &&
           ny_x64_nir_load_value(c, in->a) && nyr_put(c->w, st);
  }
  case NYIR_ADD_I64: {
    int64_t imm = 0;
    /*
     * addq $imm, %rax  (sign-extended 32-bit)
     */
    if (ny_x64_nir_try_const_i32(c, in->b, idx, &imm))
      return ny_x64_nir_load_value(c, in->a) &&
             nyr_printf(c->w, "\taddq\t$%" PRId64 ", %%rax\n", imm) &&
             ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    if (ny_x64_nir_try_const_i32(c, in->a, idx, &imm))
      return ny_x64_nir_load_value(c, in->b) &&
             nyr_printf(c->w, "\taddq\t$%" PRId64 ", %%rax\n", imm) &&
             ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    return ny_x64_nir_emit_i64_binop(c, in, "addq");
  }
  case NYIR_SUB_I64: {
    int64_t imm = 0;
    /*
     * subq $imm, %rax
     */
    if (ny_x64_nir_try_const_i32(c, in->b, idx, &imm))
      return ny_x64_nir_load_value(c, in->a) &&
             nyr_printf(c->w, "\tsubq\t$%" PRId64 ", %%rax\n", imm) &&
             ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    return ny_x64_nir_emit_i64_binop(c, in, "subq");
  }
  case NYIR_MUL_I64: {
    int64_t imm = 0;
    /*
     * x86 LEA computes x*3, x*5, and x*9 in one instruction.
     */
    if (ny_x64_nir_try_const_i32(c, in->b, idx, &imm) &&
        (imm == 3 || imm == 5 || imm == 9)) {
      int scale = imm == 3 ? 2 : imm == 5 ? 4 : 8;
      return ny_x64_nir_load_value(c, in->a) &&
             nyr_printf(c->w, "\tleaq\t(%%rax, %%rax, %d), %%rax\n", scale) &&
             ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    }
    if (ny_x64_nir_try_const_i32(c, in->a, idx, &imm) &&
        (imm == 3 || imm == 5 || imm == 9)) {
      int scale = imm == 3 ? 2 : imm == 5 ? 4 : 8;
      return ny_x64_nir_load_value(c, in->b) &&
             nyr_printf(c->w, "\tleaq\t(%%rax, %%rax, %d), %%rax\n", scale) &&
             ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    }
    /*
     * Power-of-2 strength reduction: a * 2^n → shlq $n, %rax
     */
    if (ny_x64_nir_try_const_i32(c, in->b, idx, &imm) && imm > 0 &&
        (imm & (imm - 1)) == 0) {
      int shift = 0;
      int64_t v = imm;
      while (v > 1) {
        v >>= 1;
        shift++;
      }
      return ny_x64_nir_load_value(c, in->a) &&
             nyr_printf(c->w, "\tshlq\t$%d, %%rax\n", shift) &&
             ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    }
    if (ny_x64_nir_try_const_i32(c, in->a, idx, &imm) && imm > 0 &&
        (imm & (imm - 1)) == 0) {
      int shift = 0;
      int64_t v = imm;
      while (v > 1) {
        v >>= 1;
        shift++;
      }
      return ny_x64_nir_load_value(c, in->b) &&
             nyr_printf(c->w, "\tshlq\t$%d, %%rax\n", shift) &&
             ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    }
    /*
     * imulq $imm, %rax, %rax
     */
    if (ny_x64_nir_try_const_i32(c, in->b, idx, &imm))
      return ny_x64_nir_load_value(c, in->a) &&
             nyr_printf(c->w, "\timulq\t$%" PRId64 ", %%rax, %%rax\n", imm) &&
             ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    if (ny_x64_nir_try_const_i32(c, in->a, idx, &imm))
      return ny_x64_nir_load_value(c, in->b) &&
             nyr_printf(c->w, "\timulq\t$%" PRId64 ", %%rax, %%rax\n", imm) &&
             ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    return ny_x64_nir_emit_i64_binop(c, in, "imulq");
  }
  case NYIR_DIV_I64: {
    int shift = 0;
    if (ny_x64_nir_try_pow2_divisor(c, in->b, idx, &shift)) {
      int64_t mask = (1LL << shift) - 1;
      if (!ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\tmovq\t%rax, %rcx\n\tmovq\t%rax, %rdx\n"))
        return false;
      if (shift == 1) {
        if (!nyr_put(c->w, "\tshrq\t$63, %rdx\n"))
          return false;
      } else if (!nyr_put(c->w, "\tsarq\t$63, %rdx\n") ||
                 !nyr_printf(c->w, "\tandq\t$%" PRId64 ", %%rdx\n", mask)) {
        return false;
      }
      return nyr_put(c->w, "\taddq\t%rdx, %rax\n") &&
             nyr_printf(c->w, "\tsarq\t$%d, %%rax\n", shift) &&
             ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    }
    int64_t d = 0;
    if (ny_x64_nir_try_const_i64(c, in->b, idx, &d) && d != 0 && d != 1 &&
        d != -1) {
      uint64_t ad = d >= 0 ? (uint64_t)d : (uint64_t)(-(d + 1)) + 1;
      if ((ad & (ad - 1)) != 0)
        return ny_x64_nir_emit_sdiv_magic(c, in, d, false);
    }
    return ny_x64_nir_load_value(c, in->b) &&
           nyr_put(c->w, "\tmovq\t%rax, %r10\n") &&
           ny_x64_nir_load_value(c, in->a) &&
           nyr_put(c->w, "\tcqto\n\tidivq\t%r10\n") &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  }
  case NYIR_MOD_I64: {
    int shift = 0;
    if (ny_x64_nir_try_pow2_divisor(c, in->b, idx, &shift)) {
      int64_t mask = (1LL << shift) - 1;
      if (!ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\tmovq\t%rax, %rcx\n\tmovq\t%rax, %rdx\n"))
        return false;
      if (shift == 1) {
        if (!nyr_put(c->w, "\tshrq\t$63, %rdx\n"))
          return false;
      } else if (!nyr_put(c->w, "\tsarq\t$63, %rdx\n") ||
                 !nyr_printf(c->w, "\tandq\t$%" PRId64 ", %%rdx\n", mask)) {
        return false;
      }
      return nyr_put(c->w, "\taddq\t%rdx, %rax\n") &&
             nyr_printf(c->w, "\tsarq\t$%d, %%rax\n", shift) &&
             nyr_printf(c->w, "\tshlq\t$%d, %%rax\n", shift) &&
             nyr_put(c->w, "\tsubq\t%rax, %rcx\n") &&
             nyr_put(c->w, "\tmovq\t%rcx, %rax\n") &&
             ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    }
    int64_t d = 0;
    if (ny_x64_nir_try_const_i64(c, in->b, idx, &d) && d != 0 && d != 1 &&
        d != -1) {
      uint64_t ad = d >= 0 ? (uint64_t)d : (uint64_t)(-(d + 1)) + 1;
      if ((ad & (ad - 1)) != 0)
        return ny_x64_nir_emit_sdiv_magic(c, in, d, true);
    }
    return ny_x64_nir_load_value(c, in->b) &&
           nyr_put(c->w, "\tmovq\t%rax, %r10\n") &&
           ny_x64_nir_load_value(c, in->a) &&
           nyr_put(c->w, "\tcqto\n\tidivq\t%r10\n\tmovq\t%rdx, %rax\n") &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  }
  case NYIR_AND_I64: {
    if (in->dst >= 0 && in->dst < NY_X64_NIR_MAX_SLOTS &&
        c->elided_mask_test[in->dst])
      return true;
    if (in->dst >= 0 && in->dst < NY_X64_NIR_MAX_SLOTS &&
        c->fused_sign_bias[in->dst]) {
      int bias = -1;
      int64_t imm = 0;
      if (ny_x64_nir_try_const_i32(c, in->b, idx, &imm) && imm == 1)
        bias = in->a;
      else if (ny_x64_nir_try_const_i32(c, in->a, idx, &imm) && imm == 1)
        bias = in->b;
      int shift_idx =
          bias >= 0 && bias < NY_X64_NIR_MAX_SLOTS ? c->def_index[bias] : -1;
      if (shift_idx < 0 || (size_t)shift_idx >= c->nyir->len)
        return false;
      const nyr_inst_t *shift = &c->nyir->data[shift_idx];
      return ny_x64_nir_load_value(c, shift->a) &&
             nyr_put(c->w, "\tshrq\t$63, %rax\n") &&
             ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    }
    int64_t imm = 0;
    if (ny_x64_nir_try_const_i32(c, in->b, idx, &imm))
      return ny_x64_nir_load_value(c, in->a) &&
             nyr_printf(c->w, "\tandq\t$%" PRId64 ", %%rax\n", imm) &&
             ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    if (ny_x64_nir_try_const_i32(c, in->a, idx, &imm))
      return ny_x64_nir_load_value(c, in->b) &&
             nyr_printf(c->w, "\tandq\t$%" PRId64 ", %%rax\n", imm) &&
             ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    return ny_x64_nir_emit_i64_binop(c, in, "andq");
  }
  case NYIR_OR_I64: {
    int64_t imm = 0;
    if (ny_x64_nir_try_const_i32(c, in->b, idx, &imm))
      return ny_x64_nir_load_value(c, in->a) &&
             nyr_printf(c->w, "\torq\t$%" PRId64 ", %%rax\n", imm) &&
             ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    if (ny_x64_nir_try_const_i32(c, in->a, idx, &imm))
      return ny_x64_nir_load_value(c, in->b) &&
             nyr_printf(c->w, "\torq\t$%" PRId64 ", %%rax\n", imm) &&
             ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    return ny_x64_nir_emit_i64_binop(c, in, "orq");
  }
  case NYIR_XOR_I64: {
    int64_t imm = 0;
    if (ny_x64_nir_try_const_i32(c, in->b, idx, &imm))
      return ny_x64_nir_load_value(c, in->a) &&
             nyr_printf(c->w, "\txorq\t$%" PRId64 ", %%rax\n", imm) &&
             ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    if (ny_x64_nir_try_const_i32(c, in->a, idx, &imm))
      return ny_x64_nir_load_value(c, in->b) &&
             nyr_printf(c->w, "\txorq\t$%" PRId64 ", %%rax\n", imm) &&
             ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    return ny_x64_nir_emit_i64_binop(c, in, "xorq");
  }
  case NYIR_SHL_I64: {
    int shift = 0;
    if (ny_x64_nir_try_shift_imm(c, in->b, idx, &shift))
      return ny_x64_nir_load_value(c, in->a) &&
             nyr_printf(c->w, "\tshlq\t$%d, %%rax\n", shift) &&
             ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    return ny_x64_nir_emit_i64_varshift(c, in, "shlq");
  }
  case NYIR_SAR_I64: {
    int shift = 0;
    if (ny_x64_nir_try_shift_imm(c, in->b, idx, &shift))
      return ny_x64_nir_load_value(c, in->a) &&
             nyr_printf(c->w, "\tsarq\t$%d, %%rax\n", shift) &&
             ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    return ny_x64_nir_emit_i64_varshift(c, in, "sarq");
  }
  case NYIR_ROR32_I64: {
    int64_t shift = 0;
    if (ny_x64_nir_try_const_i32(c, in->b, idx, &shift) && shift >= 0 &&
        shift < 32)
      return ny_x64_nir_load_value(c, in->a) &&
             nyr_printf(c->w,
                        "\tmovl\t%%eax, %%eax\n\trorl\t$%" PRId64 ", %%eax\n",
                        shift) &&
             ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    return ny_x64_nir_load_value(c, in->b) &&
           nyr_put(c->w, "\tmovq\t%rax, %r10\n") &&
           ny_x64_nir_load_value(c, in->a) &&
           nyr_put(
               c->w,
               "\tmovl\t%eax, %eax\n\tmovb\t%r10b, %cl\n\trorl\t%cl, %eax\n") &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  }
  case NYIR_BSWAP_I64:
    return ny_x64_nir_load_value(c, in->a) &&
           nyr_put(c->w, "\tbswapq\t%rax\n") &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  case NYIR_BSWAP32_I64:
    return ny_x64_nir_load_value(c, in->a) &&
           nyr_put(c->w, "\tbswapl\t%eax\n") &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  case NYIR_ADD_F64:
  case NYIR_SUB_F64:
  case NYIR_MUL_F64:
  case NYIR_DIV_F64: {
    const char *insn = in->op == NYIR_ADD_F64   ? "addsd"
                       : in->op == NYIR_SUB_F64 ? "subsd"
                       : in->op == NYIR_MUL_F64 ? "mulsd"
                                                : "divsd";
    int bslot = ny_x64_nir_slot(c, in->b);
    char baddr[64];
    ny_x64_nir_slot_str(c, bslot, baddr, sizeof(baddr));
    return ny_x64_nir_load_xmm(c, ny_x64_nir_slot(c, in->a), 0) &&
           nyr_printf(c->w, "\t%s\t%s, %%xmm0\n", insn, baddr) &&
           ny_x64_nir_store_xmm(c, ny_x64_nir_slot(c, in->dst), 0);
  }
  case NYIR_I64_TO_F64:
    return ny_x64_nir_load_value(c, in->a) &&
           nyr_put(c->w, "\tcvtsi2sdq\t%rax, %xmm0\n") &&
           ny_x64_nir_store_xmm(c, ny_x64_nir_slot(c, in->dst), 0);
  case NYIR_SQRT_F64:
    return ny_x64_nir_load_xmm(c, ny_x64_nir_slot(c, in->a), 0) &&
           nyr_put(c->w, "\tsqrtsd\t%xmm0, %xmm0\n") &&
           ny_x64_nir_store_xmm(c, ny_x64_nir_slot(c, in->dst), 0);
  case NYIR_SIN_F64:
  case NYIR_COS_F64: {
    char trig_addr[64];
    ny_x64_nir_slot_str(c, ny_x64_nir_slot(c, in->a), trig_addr,
                        sizeof(trig_addr));
    int dst_slot = ny_x64_nir_slot(c, in->dst);
    if (dst_slot < 0 || dst_slot >= NY_X64_NIR_MAX_SLOTS)
      return false;
    if (c->is_leaf) {
      int dst_off = c->frame_bytes - (dst_slot + 2) * 8;
      return nyr_printf(c->w, "\tfldl\t%s\n\tf%s\n\tfstpl\t%d(%%rsp)\n",
                        trig_addr, in->op == NYIR_SIN_F64 ? "sin" : "cos",
                        dst_off);
    }
    if (c->slot_offset[dst_slot] <= 0)
      return false;
    return nyr_printf(c->w, "\tfldl\t%s\n\tf%s\n\tfstpl\t-%d(%%rbp)\n",
                      trig_addr, in->op == NYIR_SIN_F64 ? "sin" : "cos",
                      c->slot_offset[dst_slot]);
  }
  case NYIR_ADD_F32:
  case NYIR_SUB_F32:
  case NYIR_MUL_F32:
  case NYIR_DIV_F32: {
    const char *insn = in->op == NYIR_ADD_F32   ? "addss"
                       : in->op == NYIR_SUB_F32 ? "subss"
                       : in->op == NYIR_MUL_F32 ? "mulss"
                                                : "divss";
    int bslot = ny_x64_nir_slot(c, in->b);
    char baddr[64];
    ny_x64_nir_slot_str(c, bslot, baddr, sizeof(baddr));
    return ny_x64_nir_load_xmm_f32(c, ny_x64_nir_slot(c, in->a), 0) &&
           nyr_printf(c->w, "\t%s\t%s, %%xmm0\n", insn, baddr) &&
           ny_x64_nir_store_xmm_f32(c, ny_x64_nir_slot(c, in->dst), 0);
  }
  case NYIR_I64_TO_F32:
    return ny_x64_nir_load_value(c, in->a) &&
           nyr_put(c->w, "\tcvtsi2ssq\t%rax, %xmm0\n") &&
           ny_x64_nir_store_xmm_f32(c, ny_x64_nir_slot(c, in->dst), 0);
  case NYIR_F32_TO_F64:
    return ny_x64_nir_load_xmm_f32(c, ny_x64_nir_slot(c, in->a), 0) &&
           nyr_put(c->w, "\tcvtss2sd\t%xmm0, %xmm0\n") &&
           ny_x64_nir_store_xmm(c, ny_x64_nir_slot(c, in->dst), 0);
  case NYIR_F64_TO_F32:
    return ny_x64_nir_load_xmm(c, ny_x64_nir_slot(c, in->a), 0) &&
           nyr_put(c->w, "\tcvtsd2ss\t%xmm0, %xmm0\n") &&
           ny_x64_nir_store_xmm_f32(c, ny_x64_nir_slot(c, in->dst), 0);
  case NYIR_CMP_I64: {
    if (c->fused_cmp[in->dst] || c->fused_select_cmp[in->dst])
      return true;
    int64_t imm = 0;
    /*
     * cmpq $imm, %rax + setcc
     */
    if (ny_x64_nir_try_const_i32(c, in->b, idx, &imm))
      return ny_x64_nir_load_value(c, in->a) &&
             nyr_printf(c->w, "\tcmpq\t$%" PRId64 ", %%rax\n", imm) &&
             nyr_printf(c->w, "\t%s\t%%al\n\tmovzbq\t%%al, %%rax\n",
                        ny_x64_nir_setcc(in->cmp)) &&
             ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    int bslot = ny_x64_nir_slot(c, in->b);
    if (in->b >= 0 && in->b < NY_X64_NIR_MAX_SLOTS && !c->fused_const[in->b] &&
        bslot >= 0) {
      char rhs[64];
      ny_x64_nir_slot_str(c, bslot, rhs, sizeof(rhs));
      if (rhs[0])
        return ny_x64_nir_load_value(c, in->a) &&
               nyr_printf(c->w, "\tcmpq\t%s, %%rax\n", rhs) &&
               nyr_printf(c->w, "\t%s\t%%al\n\tmovzbq\t%%al, %%rax\n",
                          ny_x64_nir_setcc(in->cmp)) &&
               ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    }
    return ny_x64_nir_load_value(c, in->b) &&
           nyr_put(c->w, "\tmovq\t%rax, %r10\n") &&
           ny_x64_nir_load_value(c, in->a) &&
           nyr_put(c->w, "\tcmpq\t%r10, %rax\n") &&
           nyr_printf(c->w, "\t%s\t%%al\n\tmovzbq\t%%al, %%rax\n",
                      ny_x64_nir_setcc(in->cmp)) &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  }
  case NYIR_CMP_F64: {
    char unordered[256];
    char done[256];
    snprintf(unordered, sizeof(unordered), ".Lnyir_%s_fcmp_unordered_%zu",
             c->label_prefix, idx);
    snprintf(done, sizeof(done), ".Lnyir_%s_fcmp_done_%zu", c->label_prefix,
             idx);
    int unordered_result = in->cmp == NYIR_CMP_NE ? 1 : 0;
    return ny_x64_nir_load_xmm(c, ny_x64_nir_slot(c, in->a), 0) &&
           ny_x64_nir_load_xmm(c, ny_x64_nir_slot(c, in->b), 1) &&
           nyr_put(c->w, "\tucomisd\t%xmm1, %xmm0\n") &&
           nyr_printf(c->w, "\tjp\t%s\n", unordered) &&
           nyr_printf(c->w, "\t%s\t%%al\n\tjmp\t%s\n",
                      ny_x64_nir_f64_setcc(in->cmp), done) &&
           nyr_printf(c->w,
                      "%s:\n\tmovb\t$%d, %%al\n%s:\n\tmovzbq\t%%al, %%rax\n",
                      unordered, unordered_result, done) &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  }
  case NYIR_CMP_F32: {
    char unordered[256];
    char done[256];
    snprintf(unordered, sizeof(unordered), ".Lnyir_%s_fcmp_unordered_%zu",
             c->label_prefix, idx);
    snprintf(done, sizeof(done), ".Lnyir_%s_fcmp_done_%zu", c->label_prefix,
             idx);
    int unordered_result = in->cmp == NYIR_CMP_NE ? 1 : 0;
    return ny_x64_nir_load_xmm_f32(c, ny_x64_nir_slot(c, in->a), 0) &&
           ny_x64_nir_load_xmm_f32(c, ny_x64_nir_slot(c, in->b), 1) &&
           nyr_put(c->w, "\tucomiss\t%xmm1, %xmm0\n") &&
           nyr_printf(c->w, "\tjp\t%s\n", unordered) &&
           nyr_printf(c->w, "\t%s\t%%al\n\tjmp\t%s\n",
                      ny_x64_nir_f64_setcc(in->cmp), done) &&
           nyr_printf(c->w,
                      "%s:\n\tmovb\t$%d, %%al\n%s:\n\tmovzbq\t%%al, %%rax\n",
                      unordered, unordered_result, done) &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  }
  case NYIR_SELECT_I64: {
    char true_addr[64];
    ny_x64_nir_slot_str(c, ny_x64_nir_slot(c, in->b), true_addr,
                        sizeof(true_addr));
    if (in->a >= 0 && in->a < NY_X64_NIR_MAX_SLOTS &&
        c->fused_select_cmp[in->a]) {
      int di = c->def_index[in->a];
      if (di < 0 || (size_t)di >= c->nyir->len)
        return false;
      const nyr_inst_t *cmp = &c->nyir->data[di];
      if (c->fused_mask_test_src[in->a] >= 0) {
        int source = c->fused_mask_test_src[in->a];
        return ny_x64_nir_load_value(c, source) &&
               nyr_put(c->w, "\ttestq\t$1, %rax\n") &&
               ny_x64_nir_load_value(c, in->c) &&
               nyr_printf(c->w, "\t%s\t%s, %%rax\n", ny_x64_nir_cmov(cmp->cmp),
                          true_addr) &&
               ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
      }
      int64_t imm = 0;
      bool rhs_imm = ny_x64_nir_try_const_i32(c, cmp->b, idx, &imm);
      if (rhs_imm) {
        if (!ny_x64_nir_load_value(c, cmp->a) ||
            !nyr_printf(c->w, "\tcmpq\t$%" PRId64 ", %%rax\n", imm))
          return false;
      } else if (!ny_x64_nir_load_value(c, cmp->b) ||
                 !nyr_put(c->w, "\tmovq\t%rax, %r10\n") ||
                 !ny_x64_nir_load_value(c, cmp->a) ||
                 !nyr_put(c->w, "\tcmpq\t%r10, %rax\n")) {
        return false;
      }
      /*
       * MOV preserves flags, so load the false arm before the condition move.
       */
      return ny_x64_nir_load_value(c, in->c) &&
             nyr_printf(c->w, "\t%s\t%s, %%rax\n", ny_x64_nir_cmov(cmp->cmp),
                        true_addr) &&
             ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    }
    return ny_x64_nir_load_value(c, in->a) &&
           nyr_put(c->w, "\ttestq\t%rax, %rax\n") &&
           ny_x64_nir_load_value(c, in->c) &&
           nyr_printf(c->w, "\tcmovne\t%s, %%rax\n", true_addr) &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  }
  case NYIR_BOUNDS_CHECK: {
    char ok[128];
    snprintf(ok, sizeof(ok), ".Lnyir_%s_bounds_ok_%zu", c->label_prefix, idx);
    if (!ny_x64_nir_load_value(c, in->b))
      return false;
    if (in->c >= 0) {
      if (!ny_x64_nir_load_value(c, in->c) ||
          !nyr_put(c->w, "\tmovq\t%rax, %r10\n") ||
          !ny_x64_nir_load_value(c, in->b) ||
          !nyr_put(c->w, "\tcmpq\t%r10, %rax\n"))
        return false;
    } else if (!nyr_printf(c->w, "\tcmpq\t$%" PRId64 ", %%rax\n", in->imm)) {
      return false;
    }
    /*
     * On the failure path call the shared runtime bounds helper so the
     * message matches the LLVM route (rt_panic "index_read out of range")
     * instead of raising SIGILL from a bare ud2. The helper never returns
     * for an out-of-range offset; if it did return, fall through to the ok
     * label is unreachable-by-contract, so just re-jump there.
     */
    if (!nyr_printf(c->w, "\tjb\t%s\n", ok))
      return false;
    if (!nyr_printf(c->w, "\tmovq\t%%rax, %%rdi\n"))
      return false;
    if (in->c >= 0) {
      if (!nyr_printf(c->w, "\tmovq\t%%r10, %%rsi\n"))
        return false;
    } else if (!nyr_printf(c->w, "\tmovabsq\t$%" PRId64 ", %%rsi\n", in->imm)) {
      return false;
    }
    if (!nyr_printf(c->w, "\tcall\t%srt_bounds_check\n",
                    c->target->symbol_prefix ? c->target->symbol_prefix : ""))
      return false;
    return nyr_printf(c->w, "%s:\n", ok);
  }
  case NYIR_LABEL:
    return nyr_printf(c->w, ".Lnyir_%s_L%" PRId64 ":\n", c->label_prefix,
                      in->imm);
  case NYIR_BR:
    return nyr_printf(c->w, "\tjmp\t.Lnyir_%s_L%" PRId64 "\n", c->label_prefix,
                      in->imm);
  case NYIR_BR_IF: {
    if (in->a >= 0 && in->a < NY_X64_NIR_MAX_SLOTS) {
      int di = c->def_index[in->a];
      if (di >= 0 && (size_t)di < c->nyir->len) {
        const nyr_inst_t *cmp = &c->nyir->data[di];
        if (cmp->op == NYIR_CMP_I64 && cmp->dst == in->a) {
          c->fused_cmp[in->a] = true;
          int64_t bimm = 0;
          bool b_is_imm = ny_x64_nir_try_const_i32(c, cmp->b, idx, &bimm);
          if (b_is_imm)
            return ny_x64_nir_load_value(c, cmp->a) &&
                   nyr_printf(c->w, "\tcmpq\t$%" PRId64 ", %%rax\n", bimm) &&
                   nyr_printf(c->w, "\t%s\t.Lnyir_%s_L%" PRId64 "\n",
                              ny_x64_nir_jcc(cmp->cmp), c->label_prefix,
                              in->imm);
          return ny_x64_nir_load_value(c, cmp->b) &&
                 nyr_put(c->w, "\tmovq\t%rax, %r10\n") &&
                 ny_x64_nir_load_value(c, cmp->a) &&
                 nyr_printf(c->w, "\tcmpq\t%%r10, %%rax\n") &&
                 nyr_printf(c->w, "\t%s\t.Lnyir_%s_L%" PRId64 "\n",
                            ny_x64_nir_jcc(cmp->cmp), c->label_prefix, in->imm);
        }
      }
    }
    return ny_x64_nir_load_value(c, in->a) &&
           nyr_put(c->w, "\ttestq\t%rax, %rax\n") &&
           nyr_printf(c->w, "\tjne\t.Lnyir_%s_L%" PRId64 "\n", c->label_prefix,
                      in->imm);
  }
  case NYIR_RET: {
    /*
     * Load return value into %rax (skip if -1 = void return).
     */
    if (in->a >= 0) {
      if (in->a < NY_X64_NIR_MAX_SLOTS && c->value_f64[in->a]) {
        if (!ny_x64_nir_load_xmm(c, ny_x64_nir_slot(c, in->a), 0))
          return false;
      } else if (in->a < NY_X64_NIR_MAX_SLOTS && c->value_f32[in->a]) {
        if (!ny_x64_nir_load_xmm_f32(c, ny_x64_nir_slot(c, in->a), 0))
          return false;
      } else if (!ny_x64_nir_load_value(c, in->a)) {
        return false;
      }
    } else {
      if (!nyr_put(c->w, "\txorq\t%rax, %rax\n"))
        return false;
    }
    return (!c->use_avx_vector || nyr_put(c->w, "\tvzeroupper\n")) &&
           nyr_printf(c->w, "\tjmp\t%s\n", c->epilogue_label);
  }
  case NYIR_CALL: {
    int argc = (int)in->imm;
    const char *sym_name = in->symbol ? in->symbol : "<null>";
    if (argc < 0 || argc > NYIR_CALL_MAX_ARGS) {
      nyr_set_error(
          c->err, c->err_len,
          "nyir x86-64: call exceeds the maximum supported argument count");
      return false;
    }
    int arg_vals[NYIR_CALL_MAX_ARGS];
    arg_vals[0] = in->a;
    if (argc > 1)
      arg_vals[1] = in->b;
    if (argc > 2)
      arg_vals[2] = in->c;
    if (argc > 3)
      arg_vals[3] = in->d;
    if (argc > 4)
      arg_vals[4] = in->e;
    if (argc > 5)
      arg_vals[5] = in->f;
    for (int i = 6; i < argc; ++i)
      arg_vals[i] = (in->extra_args && (size_t)(i - 6) < in->extra_args_len)
                        ? in->extra_args[i - 6]
                        : -1;
    for (int i = 0; i < argc; ++i) {
      if (arg_vals[i] < 0)
        return false;
    }
    if (c->use_avx_vector && !nyr_put(c->w, "\tvzeroupper\n"))
      return false;
    bool arg_f64[NYIR_CALL_MAX_ARGS] = {0};
    bool arg_f32[NYIR_CALL_MAX_ARGS] = {0};
    int gp_index[NYIR_CALL_MAX_ARGS];
    int sse_index[NYIR_CALL_MAX_ARGS];
    int agg_gp[NYIR_CALL_MAX_ARGS][2];
    int agg_sse[NYIR_CALL_MAX_ARGS][2];
    bool agg_in_regs[NYIR_CALL_MAX_ARGS] = {0};
    int gp = 0;
    int sse = 0;
    int stack_argc = 0;
    for (int i = 0; i < argc; ++i) {
      gp_index[i] = -1;
      sse_index[i] = -1;
      agg_gp[i][0] = agg_gp[i][1] = -1;
      agg_sse[i][0] = agg_sse[i][1] = -1;
      if (in->arg_sizes && in->arg_sizes[i] > 0) {
        arg_f64[i] = false;
        arg_f32[i] = false;
        uint32_t size = NYIR_ARG_AGG_SIZE(in->arg_sizes[i]);
        unsigned gp_need = 0, sse_need = 0;
        bool register_eligible = true;
        for (int chunk = 0; chunk < 2; ++chunk) {
          unsigned cls = NYIR_ARG_AGG_CLASS(in->arg_sizes[i], chunk);
          gp_need += cls == NYIR_ARG_CLASS_INTEGER;
          sse_need += cls == NYIR_ARG_CLASS_SSE;
          if (cls != NYIR_ARG_CLASS_NONE && cls != NYIR_ARG_CLASS_INTEGER &&
              cls != NYIR_ARG_CLASS_SSE)
            register_eligible = false;
        }
        if (register_eligible && size <= 16 && gp + (int)gp_need <= 6 &&
            sse + (int)sse_need <= 8) {
          for (int chunk = 0; chunk < 2; ++chunk) {
            unsigned cls = NYIR_ARG_AGG_CLASS(in->arg_sizes[i], chunk);
            if (cls == NYIR_ARG_CLASS_INTEGER)
              agg_gp[i][chunk] = gp++;
            else if (cls == NYIR_ARG_CLASS_SSE)
              agg_sse[i][chunk] = sse++;
          }
          agg_in_regs[i] = true;
        } else {
          stack_argc += (int)((size + 7) / 8);
        }
        continue;
      }
      nyr_scalar_class_t arg_class = nyr_call_arg_class_get(in, (size_t)i);
      bool inferred_f64 =
          arg_vals[i] < NY_X64_NIR_MAX_SLOTS && c->value_f64[arg_vals[i]];
      bool inferred_f32 =
          arg_vals[i] < NY_X64_NIR_MAX_SLOTS && c->value_f32[arg_vals[i]];
      if ((arg_class == NYIR_SCALAR_F64 && inferred_f32) ||
          (arg_class == NYIR_SCALAR_F32 && inferred_f64) ||
          (arg_class == NYIR_SCALAR_UNKNOWN && (inferred_f64 || inferred_f32)))
        return false;
      arg_f64[i] = arg_class == NYIR_SCALAR_F64;
      arg_f32[i] = arg_class == NYIR_SCALAR_F32;
      if (arg_f64[i] || arg_f32[i]) {
        if (sse < 8)
          sse_index[i] = sse++;
        else
          stack_argc++;
      } else {
        if (gp < 6)
          gp_index[i] = gp++;
        else
          stack_argc++;
      }
      /*
       * Call-site ABI placement (-vvvv ABI channel): the physical location
       * each argument occupies at the call instruction — GPR/SSE register
       * name or stack slot — straight from the final machine lowering.
       */
      if (ny_dbg_enabled & NY_DBG_ABI) {
        char place[64];
        if (gp_index[i] >= 0)
          snprintf(place, sizeof(place), "%s",
                   c->target->gp_arg_regs[gp_index[i]]);
        else if (sse_index[i] >= 0)
          snprintf(place, sizeof(place), "%s",
                   c->target->fp_arg_regs[sse_index[i]]);
        else
          snprintf(place, sizeof(place), "stack+%d", stack_argc * 8);
        char detail[128];
        snprintf(detail, sizeof(detail), "%s arg %d class=%s", sym_name, i,
                 arg_f64[i]                            ? "f64"
                 : arg_f32[i]                          ? "f32"
                 : arg_class == NYIR_SCALAR_I64_TAGGED ? "tagged"
                 : arg_class == NYIR_SCALAR_STRING     ? "str"
                                                       : "i64");
        ny_dbg_abi_classify("call-arg", place, detail);
      }
    }

    /*
     * Stack-passed args permanently move %rsp for
     * the duration of the call; pad to an even slot count so %rsp is
     * 16-byte aligned at the `call` instruction (shadow space, if any, is
     * itself a multiple of 16).
     */
    int pad = stack_argc % 2;
    if (pad && !nyr_put(c->w, "\tsubq\t$8, %rsp\n"))
      return false;
    /*
     * Push stack args highest-index first so the first stack arg ends up
     * closest to the top of stack (lowest address) at call time.
     */
    for (int i = argc - 1; i >= 0; --i) {
      if (gp_index[i] >= 0 || sse_index[i] >= 0 || agg_in_regs[i])
        continue;
      if (in->arg_sizes && in->arg_sizes[i] > 0) {
        uint32_t size = NYIR_ARG_AGG_SIZE(in->arg_sizes[i]);
        int slots = (int)((size + 7) / 8);
        if (!nyr_printf(c->w, "\tsubq\t$%d, %%rsp\n", slots * 8))
          return false;
        if (!ny_x64_nir_load_value(c, arg_vals[i]))
          return false;
        if (!nyr_put(c->w, "\tmovq\t%rax, %rsi\n\tmovq\t%rsp, %rdi\n") ||
            !nyr_printf(c->w, "\tmovq\t$%" PRIu32 ", %%rcx\n\trep movsb\n",
                        size))
          return false;
        continue;
      }
      if (arg_f64[i]) {
        if (!ny_x64_nir_load_xmm(c, ny_x64_nir_slot(c, arg_vals[i]), 0) ||
            !nyr_put(c->w, "\tsubq\t$8, %rsp\n\tmovsd\t%xmm0, (%rsp)\n"))
          return false;
      } else if (arg_f32[i]) {
        if (!ny_x64_nir_load_xmm_f32(c, ny_x64_nir_slot(c, arg_vals[i]), 0) ||
            !nyr_put(c->w, "\tsubq\t$8, %rsp\n\tmovss\t%xmm0, (%rsp)\n"))
          return false;
      } else if (!ny_x64_nir_load_value(c, arg_vals[i]) ||
                 !nyr_put(c->w, "\tpushq\t%rax\n")) {
        return false;
      }
    }
    for (int i = 0; i < argc; ++i) {
      if (agg_in_regs[i]) {
        if (!ny_x64_nir_load_value(c, arg_vals[i]))
          return false;
        for (int chunk = 0; chunk < 2; ++chunk) {
          int off = chunk * 8;
          if (agg_gp[i][chunk] >= 0) {
            if (!nyr_printf(c->w, "\tmovq\t%d(%%rax), %s\n", off,
                            c->target->gp_arg_regs[agg_gp[i][chunk]]))
              return false;
          } else if (agg_sse[i][chunk] >= 0 &&
                     !nyr_printf(c->w, "\tmovq\t%d(%%rax), %%xmm%d\n", off,
                                 agg_sse[i][chunk])) {
            return false;
          }
        }
      } else if (sse_index[i] >= 0) {
        if (arg_f32[i]) {
          if (!ny_x64_nir_load_xmm_f32(c, ny_x64_nir_slot(c, arg_vals[i]),
                                       sse_index[i]))
            return false;
        } else if (!ny_x64_nir_load_xmm(c, ny_x64_nir_slot(c, arg_vals[i]),
                                        sse_index[i])) {
          return false;
        }
      } else if (gp_index[i] >= 0) {
        int64_t carg_imm = 0;
        if (ny_x64_nir_try_const_i32(c, arg_vals[i], idx, &carg_imm)) {
          if (!nyr_printf(c->w, "\tmovq\t$%" PRId64 ", %s\n", carg_imm,
                          c->target->gp_arg_regs[gp_index[i]]))
            return false;
        } else {
          if (!ny_x64_nir_load_value(c, arg_vals[i]) ||
              !nyr_printf(c->w, "\tmovq\t%%rax, %s\n",
                          c->target->gp_arg_regs[gp_index[i]]))
            return false;
        }
      }
    }
    if (c->target->shadow_space_bytes > 0 &&
        !nyr_printf(c->w, "\tsubq\t$%zu, %%rsp\n",
                    c->target->shadow_space_bytes))
      return false;
    char fn_label[256];
    if (in->flags & NYIR_INST_F_EXTERN) {
      snprintf(fn_label, sizeof(fn_label), "%s%s", c->target->symbol_prefix,
               sym_name);
    } else if (strncmp(sym_name, "ny_fn_", 6) == 0) {
      snprintf(fn_label, sizeof(fn_label), "%s%s", c->target->symbol_prefix,
               sym_name);
    } else {
      snprintf(fn_label, sizeof(fn_label), NY_FMT_FN, c->target->symbol_prefix,
               sym_name);
    }
    if (!nyr_printf(c->w, "\tcall\t%s\n", fn_label))
      return false;
    if (c->target->shadow_space_bytes > 0 &&
        !nyr_printf(c->w, "\taddq\t$%zu, %%rsp\n",
                    c->target->shadow_space_bytes))
      return false;
    if (stack_argc + pad > 0 &&
        !nyr_printf(c->w, "\taddq\t$%d, %%rsp\n", (stack_argc + pad) * 8))
      return false;
    if (in->dst >= 0) {
      if (in->dst < NY_X64_NIR_MAX_SLOTS && c->value_f64[in->dst])
        return ny_x64_nir_store_xmm(c, ny_x64_nir_slot(c, in->dst), 0);
      if (in->dst < NY_X64_NIR_MAX_SLOTS && c->value_f32[in->dst])
        return ny_x64_nir_store_xmm_f32(c, ny_x64_nir_slot(c, in->dst), 0);
      return ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    }
    return true;
  }
  default:
    nyr_set_error(c->err, c->err_len, "nyir x86-64: unsupported opcode %s",
                  nyr_op_name(in->op));
    return false;
  }
  return false;
}

static bool ny_x64_nir_rax_cache_candidate(nyr_op_t op) {
  switch (op) {
  case NYIR_CONST_I64:
  case NYIR_COPY:
  case NYIR_LOAD_LOCAL:
  case NYIR_LOAD_I64:
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
  case NYIR_SELECT_I64:
  case NYIR_CMP_I64:
    return true;
  default:
    return false;
  }
}

static bool ny_x64_nir_gpr_only_op(nyr_op_t op) {
  switch (op) {
  case NYIR_NOP:
  case NYIR_CONST_I64:
  case NYIR_CONST_F64:
  case NYIR_CONST_F32:
  case NYIR_COPY:
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
  case NYIR_ROR_I64:
  case NYIR_ROR32_I64:
  case NYIR_BSWAP_I64:
  case NYIR_BSWAP32_I64:
  case NYIR_SELECT_I64:
  case NYIR_CMP_I64:
  case NYIR_ADDR_LOCAL:
  case NYIR_ADDR_SYMBOL:
  case NYIR_LOAD_I64:
  case NYIR_STORE_I64:
    return true;
  default:
    return false;
  }
}

static bool ny_x64_nir_vec_cache_op(const ny_x64_nir_ctx_t *c,
                                    const nyr_inst_t *in) {
  if (!c || !in || !c->target || !(c->target->caps & NY_NATIVE_CAP_MACH_AVX))
    return false;
  switch (in->op) {
  case NYIR_VEC4_ADD_F64:
  case NYIR_VEC4_SUB_F64:
  case NYIR_VEC4_MUL_F64:
  case NYIR_VEC4_DIV_F64:
  case NYIR_VEC4_LOAD_F64:
  case NYIR_VEC4_STORE_F64:
  case NYIR_VEC4_SET1_F64:
    return true;
  default:
    return false;
  }
}

/*
 * Keep a value in RAX across adjacent scalar instructions when the value's
 * stack home remains authoritative. The optimization is intentionally
 * limited to simple integer producers; calls, memory stores, floating-point
 * operations, and control-flow boundaries discard the cached register state.
 */
static bool ny_x64_nir_emit_inst(ny_x64_nir_ctx_t *c, const nyr_inst_t *in,
                                 size_t idx) {
  if (in->dst >= 0 && in->dst < NY_X64_NIR_MAX_SLOTS &&
      c->elided_sign_shift[in->dst]) {
    c->rax_cached_slot = -1;
    c->rax_store_slot = -1;
    return true;
  }
  if (in->dst >= 0 && in->dst < NY_X64_NIR_MAX_SLOTS &&
      c->elided_mask_test[in->dst]) {
    c->rax_cached_slot = -1;
    c->rax_store_slot = -1;
    return true;
  }
  bool gpr_only = ny_x64_nir_gpr_only_op(in->op);
  if (in->op == NYIR_LOAD_LOCAL && in->imm >= 0 &&
      in->imm < NY_X64_NIR_MAX_SLOTS)
    gpr_only = !c->local_f64[in->imm] && !c->local_f32[in->imm];
  else if (in->op == NYIR_STORE_LOCAL && in->imm >= 0 &&
           in->imm < NY_X64_NIR_MAX_SLOTS && in->a >= 0 &&
           in->a < NY_X64_NIR_MAX_SLOTS)
    gpr_only = !c->local_f64[in->imm] && !c->local_f32[in->imm] &&
               !c->value_f64[in->a] && !c->value_f32[in->a];
  if (!gpr_only && !ny_x64_nir_vec_cache_op(c, in) &&
      !ny_x64_nir_vec_cache_clear(c))
    return false;
  bool candidate = ny_x64_nir_rax_cache_candidate(in->op);
  if (!candidate && in->op != NYIR_NOP)
    c->rax_cached_slot = -1;
  c->rax_store_slot = -1;

  bool ok = ny_x64_nir_emit_inst_raw(c, in, idx);
  if (!ok) {
    c->rax_cached_slot = -1;
    return false;
  }
  if (in->op == NYIR_NOP)
    return true;

  int dst_slot = in->dst >= 0 ? ny_x64_nir_slot(c, in->dst) : -1;
  if (candidate && dst_slot >= 0 && c->rax_store_slot == dst_slot) {
    c->rax_cached_slot = dst_slot;
    return true;
  }
  if (in->op == NYIR_COPY && dst_slot >= 0 &&
      !(in->dst < NY_X64_NIR_MAX_SLOTS && c->fused_const[in->dst]) &&
      ny_x64_nir_slot(c, in->a) == dst_slot && c->rax_cached_slot == dst_slot)
    return true;
  c->rax_cached_slot = -1;
  return true;
}

bool nyr_x86_64_emit_ir(nyr_writer_t *w, const ny_native_target_info_t *target,
                        const nyr_func_t *nyir, const char *func_name,
                        bool tag_return, char *err, size_t err_len) {
  if (!w || !target || !nyir)
    return false;
  for (size_t i = 0; i < nyir->param_count; ++i)
    if (nyir->param_types && nyir->param_types[i] == NYIR_PARAM_AGGREGATE) {
      if (err && err_len)
        snprintf(err, err_len,
                 "x86-64 NYIR emitter: aggregate parameters are unsupported");
      return false;
    }
  if (nyir->return_aggregate_size) {
    if (err && err_len)
      snprintf(err, err_len,
               "x86-64 NYIR emitter: aggregate returns are unsupported");
    return false;
  }

  const char *name = func_name && func_name[0] ? func_name : "rt_main";

  ny_x64_nir_ctx_t ctx = {.w = w,
                          .target = target,
                          .nyir = nyir,
                          .frame_bytes = 0,
                          .rax_cached_slot = -1,
                          .rax_store_slot = -1,
                          .max_local_slot = 0,
                          .label_prefix = name,
                          .err = err,
                          .err_len = err_len};
  snprintf(ctx.epilogue_label, sizeof(ctx.epilogue_label), ".Lnyir_epilogue_%s",
           name);
  memset(ctx.slot_offset, 0, sizeof(ctx.slot_offset));
  for (int i = 0; i < NY_X64_NIR_MAX_SLOTS; ++i) {
    ctx.home_reg[i] = -1;
    if (i < NY_X64_VEC_CACHE_REGS) {
      ctx.vec_cached_value[i] = -1;
      ctx.vec_cache_dirty[i] = false;
    }
  }
  memset(ctx.fused_cmp, 0, sizeof(ctx.fused_cmp));
  memset(ctx.fused_select_cmp, 0, sizeof(ctx.fused_select_cmp));
  ny_x64_nir_classify_values(&ctx);
  ny_x64_nir_compute_frame(&ctx);
  ny_x64_nir_assign_loop_regs(&ctx);
  /*
   * Value homes reserve three extra words before their first spill slot so
   * vector lanes can share the same addressing scheme. The raw slot count
   * above only accounts for the logical homes, which can leave the deepest
   * assigned home below RSP (especially after loop-register alignment adds
   * eight bytes). Keep the ABI-required frame-size residue and grow by whole
   * 16-byte units until every assigned slot is inside the allocation.
   */
  int deepest_slot = 0;
  for (int slot = 0; slot < NY_X64_NIR_MAX_SLOTS; ++slot)
    if (ctx.slot_offset[slot] > deepest_slot)
      deepest_slot = ctx.slot_offset[slot];
  while (ctx.frame_bytes < deepest_slot)
    ctx.frame_bytes += 16;
  if (ny_dbg_is_enabled(NY_DBG_DEEP)) {
    size_t bounds_checks = 0, calls = 0, loads = 0, stores = 0;
    for (size_t i = 0; i < nyir->len; ++i) {
      switch (nyir->data[i].op) {
      case NYIR_BOUNDS_CHECK:
        ++bounds_checks;
        break;
      case NYIR_CALL:
        ++calls;
        break;
      case NYIR_LOAD_I64:
        ++loads;
        break;
      case NYIR_STORE_I64:
        ++stores;
        break;
      default:
        break;
      }
    }
    ny_dbg_enter_context(name);
    ny_dbg_heuristic_event(
        "X64_NYIR",
        "function=%s blocks=%zu insts=%zu values=%d locals=%d "
        "value_homes=%d resident_regs=%d frame_bytes=%d bounds=%zu "
        "calls=%zu loads=%zu stores=%zu",
        name, nyr_block_count(nyir), nyir->len, nyir->next_value,
        ctx.max_local_slot, ctx.value_home_count, ctx.home_reg_count,
        ctx.frame_bytes, bounds_checks, calls, loads, stores);
    ny_dbg_exit_context(name);
  }
  ny_dbg_mem_layout(name, 0, (size_t)ctx.frame_bytes, 16,
                    "x86-64 NYIR frame: local slots and value spill homes");
  if (ny_dbg_is_enabled(NY_DBG_MEM)) {
    for (int slot = 0; slot < ctx.max_local_slot; ++slot) {
      char row[96], detail[96];
      snprintf(row, sizeof(row), "local[%d]", slot);
      snprintf(detail, sizeof(detail), "rbp-relative=-%d", (slot + 1) * 8);
      ny_dbg_mem_layout(row, (size_t)(slot + 1) * 8, 8, 8, detail);
    }
    for (int value = 0; value < ctx.nyir->next_value; ++value) {
      int home = ny_x64_nir_slot(&ctx, value);
      if (home < 0 || home + 1 >= NY_X64_NIR_MAX_SLOTS)
        break;
      char row[96], detail[96];
      snprintf(row, sizeof(row), "v%d home", value);
      snprintf(detail, sizeof(detail), "rbp-relative=-%d spill/home word",
               ctx.slot_offset[home]);
      ny_dbg_mem_layout(row, (size_t)ctx.slot_offset[home], 8, 8, detail);
    }
  }

  const char *sym = target->symbol_prefix;

  /*
   * Function header.
   */
  if (strcmp(target->object_format, "macho") == 0) {
    if (!nyr_put(w, "\t.p2align 4, 0x90\n"))
      return false;
  } else if (!nyr_printf(w, "\t.type\t%s%s,@function\n", sym, name)) {
    return false;
  }
  if (!nyr_printf(w, "\t.globl\t%s%s\n%s%s:\n", sym, name, sym, name))
    return false;

  /*
   * Prologue: save rbp, allocate frame.
   */
  if (ctx.is_leaf) {
    if (ctx.frame_bytes > 0 &&
        !nyr_printf(w, "\tsubq\t$%d, %%rsp\n", ctx.frame_bytes))
      return false;
  } else {
    if (ctx.home_reg_count > 0 && !nyr_put(w, "\tpushq\t%rbx\n"))
      return false;
    if (ctx.home_reg_count > 1 && !nyr_put(w, "\tpushq\t%r12\n"))
      return false;
    if (ctx.home_reg_count > 2 && !nyr_put(w, "\tpushq\t%r13\n"))
      return false;
    if (ctx.home_reg_count > 3 && !nyr_put(w, "\tpushq\t%r14\n"))
      return false;
    if (ctx.home_reg_count > 4 && !nyr_put(w, "\tpushq\t%r15\n"))
      return false;
    if (!nyr_put(w, "\tpushq\t%rbp\n\tmovq\t%rsp, %rbp\n"))
      return false;
    if (ctx.frame_bytes > 0 &&
        !nyr_printf(w, "\tsubq\t$%d, %%rsp\n", ctx.frame_bytes))
      return false;
  }

  /*
   * Detect parameters: locals loaded before any store to that slot.
   */
  bool *param_init = NULL;
  bool *stored = NULL;
  int max_local = ctx.max_local_slot;
  if (strcmp(name, "rt_main") != 0 && max_local > 0) {
    param_init = (bool *)calloc((size_t)max_local, sizeof(bool));
    stored = (bool *)calloc((size_t)max_local, sizeof(bool));
    if (nyir->param_count > 0 && nyir->param_count <= (size_t)max_local) {
      for (size_t i = 0; i < nyir->param_count; ++i)
        param_init[i] = true;
    } else {
      for (size_t i = 0; nyir && i < nyir->len && param_init && stored; ++i) {
        const nyr_inst_t *in = &nyir->data[i];
        int lid = (int)in->imm;
        if (in->op == NYIR_STORE_LOCAL && lid >= 0 && lid < max_local)
          stored[lid] = true;
        else if (in->op == NYIR_LOAD_LOCAL && lid >= 0 && lid < max_local &&
                 !stored[lid])
          param_init[lid] = true;
      }
    }
    int gp = 0;
    int sse = 0;
    int stack = 0;
    for (int i = 0; i < max_local; ++i) {
      if (!param_init || !param_init[i])
        continue;
      bool is_f64 = i < NY_X64_NIR_MAX_SLOTS && ctx.local_f64[i];
      bool is_f32 = i < NY_X64_NIR_MAX_SLOTS && ctx.local_f32[i];
      char dst[64];
      ny_x64_nir_slot_str(&ctx, i, dst, sizeof(dst));
      if ((is_f64 || is_f32) && sse < 8) {
        if (!nyr_printf(w, "\t%s\t%%xmm%d, %s\n", is_f32 ? "movss" : "movsd",
                        sse, dst)) {
          free(param_init);
          free(stored);
          return false;
        }
        sse++;
      } else if (!is_f64 && !is_f32 && gp < 6) {
        if (!nyr_printf(w, "\tmovq\t%s, %s\n", target->gp_arg_regs[gp], dst)) {
          free(param_init);
          free(stored);
          return false;
        }
        gp++;
      } else {
        /*
         * Stack-passed parameters sit above the return address at entry.
         * A leaf frame has no %rbp save, so address it from the post-frame
         * %rsp there; non-leaf functions retain the conventional %rbp form.
         */
        int src_off = ctx.is_leaf
                          ? ctx.frame_bytes + 8 +
                                (int)target->shadow_space_bytes + stack * 8
                          : 16 + (int)target->shadow_space_bytes + stack * 8;
        const char *src_base = ctx.is_leaf ? "%rsp" : "%rbp";
        if (!nyr_printf(w, "\tmovq\t%d(%s), %%rax\n", src_off, src_base) ||
            !nyr_printf(w, "\tmovq\t%%rax, %s\n", dst)) {
          free(param_init);
          free(stored);
          return false;
        }
        stack++;
      }
    }
    free(param_init);
    free(stored);
  }

  /*
   * Emit each NYIR instruction.
   */
  for (size_t i = 0; i < nyir->len; ++i) {
    if (ny_trace_enabled("NY_TRACE_EMIT"))
      fprintf(stderr, "[emit %s] i=%zu op=%s\n", name, i,
              nyr_op_name(nyir->data[i].op));
    if (!ny_x64_nir_emit_inst(&ctx, &nyir->data[i], i)) {
      fprintf(stderr, "native NYIR repro (x86-64 emit failed):\n");
      nyr_dump_compact(stderr, nyir, name);
      return false;
    }
  }

  /*
   * Epilogue.
   */
  if (!nyr_printf(w, "%s:\n", ctx.epilogue_label))
    return false;
  if (tag_return && !nyr_put(w, "\tleaq\t1(,%rax,2), %rax\n"))
    return false;
  if (ctx.is_leaf) {
    if (ctx.frame_bytes > 0 &&
        !nyr_printf(w, "\taddq\t$%d, %%rsp\n", ctx.frame_bytes))
      return false;
    if (!nyr_put(w, "\tret\n"))
      return false;
  } else {
    if (!nyr_put(w, "\tleave\n"))
      return false;
    if (ctx.home_reg_count > 4 && !nyr_put(w, "\tpopq\t%r15\n"))
      return false;
    if (ctx.home_reg_count > 3 && !nyr_put(w, "\tpopq\t%r14\n"))
      return false;
    if (ctx.home_reg_count > 2 && !nyr_put(w, "\tpopq\t%r13\n"))
      return false;
    if (ctx.home_reg_count > 1 && !nyr_put(w, "\tpopq\t%r12\n"))
      return false;
    if (ctx.home_reg_count > 0 && !nyr_put(w, "\tpopq\t%rbx\n"))
      return false;
    if (!nyr_put(w, "\tret\n"))
      return false;
  }

  if (strcmp(target->object_format, "macho") != 0) {
    if (!nyr_printf(w, "\t.size\t%s%s, .-%s%s\n", sym, name, sym, name))
      return false;
  }
  return true;
}

/*
 * machine form scalar text emitter
 *
 * This is deliberately small and conservative: it is the first live
 * consumer of finalized machine form, not a second full backend.  The
 * established NYIR emitter remains the fallback for floats, pointers,
 * calls, aggregates, vectors, and any unsupported target shape.
 */

static bool nyr_x64_mach_is_gpr(const nyr_mach_func_t *mach,
                                const nyr_mach_operand_t *op) {
  return op && op->kind == NYR_MACH_OPERAND_VREG &&
         op->as.reg < mach->vreg_len &&
         (mach->vreg_types[op->as.reg] == NYR_MACH_TYPE_I64 ||
          mach->vreg_types[op->as.reg] == NYR_MACH_TYPE_PTR);
}

static bool nyr_x64_mach_is_i64(const nyr_mach_func_t *mach,
                                const nyr_mach_operand_t *op) {
  return op && op->kind == NYR_MACH_OPERAND_VREG &&
         op->as.reg < mach->vreg_len &&
         mach->vreg_types[op->as.reg] == NYR_MACH_TYPE_I64;
}

static bool nyr_x64_mach_is_ptr(const nyr_mach_func_t *mach,
                                const nyr_mach_operand_t *op) {
  return op && op->kind == NYR_MACH_OPERAND_VREG &&
         op->as.reg < mach->vreg_len &&
         mach->vreg_types[op->as.reg] == NYR_MACH_TYPE_PTR;
}

static bool nyr_x64_mach_is_f64(const nyr_mach_func_t *mach,
                                const nyr_mach_operand_t *op) {
  return op && op->kind == NYR_MACH_OPERAND_VREG &&
         op->as.reg < mach->vreg_len &&
         mach->vreg_types[op->as.reg] == NYR_MACH_TYPE_F64;
}

static bool nyr_x64_mach_is_f32(const nyr_mach_func_t *mach,
                                const nyr_mach_operand_t *op) {
  return op && op->kind == NYR_MACH_OPERAND_VREG &&
         op->as.reg < mach->vreg_len &&
         mach->vreg_types[op->as.reg] == NYR_MACH_TYPE_F32;
}

static bool nyr_x64_mach_is_float(const nyr_mach_func_t *mach,
                                  const nyr_mach_operand_t *op) {
  return nyr_x64_mach_is_f64(mach, op) || nyr_x64_mach_is_f32(mach, op);
}

static bool nyr_x64_mach_is_shift_count(const nyr_mach_func_t *mach,
                                        const nyr_mach_operand_t *op) {
  return op &&
         (op->kind == NYR_MACH_OPERAND_IMM || nyr_x64_mach_is_i64(mach, op));
}
static bool nyr_x64_mach_is_f64_operand(const nyr_mach_func_t *mach,
                                        const nyr_mach_operand_t *op) {
  if (!op)
    return false;
  if (op->kind == NYR_MACH_OPERAND_VREG) {
    return op->as.reg < mach->vreg_len &&
           mach->vreg_types[op->as.reg] == NYR_MACH_TYPE_F64;
  }
  if (op->kind == NYR_MACH_OPERAND_FRAME) {
    return op->as.frame_index < mach->frame_slot_len &&
           mach->frame_slots[op->as.frame_index].type == NYR_MACH_TYPE_F64;
  }
  return false;
}

/*
 * Formal parameters occupy a dense frame-slot prefix. Infer its end from the
 * highest slot read before its first write, preserving unused leading params.
 */
static bool nyr_x64_mach_params(const nyr_mach_func_t *mach, bool *params,
                                size_t *gp_count, size_t *fp_count) {
  if (!mach || !gp_count || !fp_count || (!params && mach->frame_slot_len))
    return false;
  bool *stored = calloc(mach->frame_slot_len, sizeof(*stored));
  if (mach->frame_slot_len && !stored)
    return false;
  size_t prefix = mach->param_count;
  if (prefix > mach->frame_slot_len) {
    free(stored);
    return false;
  }
  for (size_t i = 0; prefix == 0 && i < mach->inst_len; ++i) {
    const nyr_mach_inst_t *in = &mach->insts[i];
    if (in->opcode == NYR_MACH_STORE &&
        in->dst.kind == NYR_MACH_OPERAND_FRAME) {
      if (in->dst.as.frame_index >= mach->frame_slot_len) {
        free(stored);
        return false;
      }
      stored[in->dst.as.frame_index] = true;
    } else if (in->opcode == NYR_MACH_LOAD &&
               in->src0.kind == NYR_MACH_OPERAND_FRAME) {
      if (in->src0.as.frame_index >= mach->frame_slot_len) {
        free(stored);
        return false;
      }
      if (!stored[in->src0.as.frame_index] &&
          in->src0.as.frame_index + 1 > prefix)
        prefix = in->src0.as.frame_index + 1;
    }
  }
  for (size_t i = 0; i < prefix; ++i)
    params[i] = true;
  *gp_count = 0;
  *fp_count = 0;
  for (size_t i = 0; i < prefix; ++i) {
    if (mach->frame_slots[i].type == NYR_MACH_TYPE_F32 ||
        mach->frame_slots[i].type == NYR_MACH_TYPE_F64)
      ++*fp_count;
    else
      ++*gp_count;
  }
  free(stored);
  return true;
}

static bool nyr_x64_mach_call_supported(const nyr_mach_func_t *mach,
                                        const ny_native_target_info_t *target,
                                        const nyr_mach_inst_t *in) {
  if (!mach || !target || !in)
    return false;
  if (in->arg_sizes)
    for (size_t arg = 0; arg < in->args_len; ++arg)
      if (in->arg_sizes[arg] != 0)
        return false;
  for (size_t arg = 0; arg < in->args_len; ++arg) {
    if (!nyr_x64_mach_is_float(mach, &in->args[arg]) &&
        !nyr_x64_mach_is_gpr(mach, &in->args[arg]))
      return false;
  }
  return target->abi == NY_NATIVE_ABI_SYSV ||
         target->abi == NY_NATIVE_ABI_WIN64;
}

static bool nyr_x64_mach_scalar_reject(const nyr_mach_func_t *mach,
                                       size_t inst_index) {
  if (getenv("NY_DUMP_MACH")) {
    if (mach && inst_index < mach->inst_len) {
      const nyr_mach_inst_t *in = &mach->insts[inst_index];
      fprintf(stderr,
              "x86-64 machine eligibility: unsupported inst=%zu opcode=%s(%d) "
              "source_pc=%u "
              "dst=(%d,%u) src0=(%d,%u) src1=(%d,%u) src2=(%d,%u) cond=%d\n",
              inst_index, nyr_mach_opcode_name(in->opcode), (int)in->opcode,
              in->source_pc, (int)in->dst.kind,
              in->dst.kind == NYR_MACH_OPERAND_VREG ? in->dst.as.reg : 0,
              (int)in->src0.kind,
              in->src0.kind == NYR_MACH_OPERAND_VREG ? in->src0.as.reg : 0,
              (int)in->src1.kind,
              in->src1.kind == NYR_MACH_OPERAND_VREG ? in->src1.as.reg : 0,
              (int)in->src2.kind,
              in->src2.kind == NYR_MACH_OPERAND_VREG ? in->src2.as.reg : 0,
              (int)in->condition);
    } else {
      fprintf(stderr, "x86-64 machine eligibility: unsupported signature\n");
    }
  }
  return false;
}

static bool nyr_x64_mach_scalar_supported(const nyr_mach_func_t *mach,
                                          const ny_native_target_info_t *target,
                                          const char *name) {
  size_t check_inst = SIZE_MAX;
  if (!mach || !target || !name)
    return nyr_x64_mach_scalar_reject(mach, check_inst);
  bool *params = calloc(mach->frame_slot_len, sizeof(*params));
  if (mach->frame_slot_len && !params)
    return nyr_x64_mach_scalar_reject(mach, check_inst);
  size_t gp_params = 0, fp_params = 0;
  bool params_ok = nyr_x64_mach_params(mach, params, &gp_params, &fp_params);
  free(params);
  if (!params_ok ||
      (target->abi != NY_NATIVE_ABI_SYSV && target->abi != NY_NATIVE_ABI_WIN64))
    return nyr_x64_mach_scalar_reject(mach, check_inst);
  for (size_t v = 0; v < mach->vreg_len; ++v)
    if (mach->vreg_types[v] != NYR_MACH_TYPE_I64 &&
        mach->vreg_types[v] != NYR_MACH_TYPE_PTR &&
        mach->vreg_types[v] != NYR_MACH_TYPE_F64 &&
        mach->vreg_types[v] != NYR_MACH_TYPE_F32)
      return nyr_x64_mach_scalar_reject(mach, check_inst);
  for (size_t s = 0; s < mach->frame_slot_len; ++s)
    if (mach->frame_slots[s].type != NYR_MACH_TYPE_I64 &&
        mach->frame_slots[s].type != NYR_MACH_TYPE_F64 &&
        mach->frame_slots[s].type != NYR_MACH_TYPE_F32)
      return nyr_x64_mach_scalar_reject(mach, check_inst);
  for (size_t i = 0; i < mach->inst_len; ++i) {
    check_inst = i;
    const nyr_mach_inst_t *in = &mach->insts[i];
    switch (in->opcode) {
    case NYR_MACH_NOP:
      break;
    case NYR_MACH_COPY:
      if ((!nyr_x64_mach_is_gpr(mach, &in->dst) &&
           !nyr_x64_mach_is_float(mach, &in->dst)) ||
          (in->src0.kind != NYR_MACH_OPERAND_IMM &&
           ((nyr_x64_mach_is_f64(mach, &in->dst) &&
             !nyr_x64_mach_is_f64(mach, &in->src0)) ||
            (nyr_x64_mach_is_f32(mach, &in->dst) &&
             !nyr_x64_mach_is_f32(mach, &in->src0)) ||
            (!nyr_x64_mach_is_float(mach, &in->dst) &&
             !nyr_x64_mach_is_gpr(mach, &in->src0)))))
        return nyr_x64_mach_scalar_reject(mach, check_inst);
      break;
    case NYR_MACH_LEA:
      /*
       * NYIR address calculations can carry the machine-level result as an
       * integer when the source was already erased to an untyped word. LEA
       * still performs the same wrapping bit-address arithmetic in that
       * representation.
       */
      if ((!nyr_x64_mach_is_ptr(mach, &in->dst) &&
           !nyr_x64_mach_is_i64(mach, &in->dst)) ||
          !((in->src0.kind == NYR_MACH_OPERAND_FRAME &&
             in->src1.kind == NYR_MACH_OPERAND_NONE &&
             in->src2.kind == NYR_MACH_OPERAND_NONE) ||
            (in->src0.kind == NYR_MACH_OPERAND_SYMBOL &&
             in->src1.kind == NYR_MACH_OPERAND_NONE &&
             in->src2.kind == NYR_MACH_OPERAND_NONE) ||
            (nyr_x64_mach_is_gpr(mach, &in->src0) &&
             in->src1.kind == NYR_MACH_OPERAND_IMM &&
             (in->src1.as.imm == 2 || in->src1.as.imm == 4 ||
              in->src1.as.imm == 8) &&
             in->src2.kind == NYR_MACH_OPERAND_NONE) ||
            (nyr_x64_mach_is_gpr(mach, &in->src0) &&
             nyr_x64_mach_is_gpr(mach, &in->src1) &&
             in->src2.kind == NYR_MACH_OPERAND_IMM &&
             (in->src2.as.imm == 2 || in->src2.as.imm == 4 ||
              in->src2.as.imm == 8))))
        return nyr_x64_mach_scalar_reject(mach, check_inst);
      break;
    case NYR_MACH_CONVERT:
      if (!((nyr_x64_mach_is_i64(mach, &in->dst) &&
             (nyr_x64_mach_is_f64(mach, &in->src0) ||
              nyr_x64_mach_is_f32(mach, &in->src0))) ||
            (nyr_x64_mach_is_f64(mach, &in->dst) &&
             (nyr_x64_mach_is_i64(mach, &in->src0) ||
              nyr_x64_mach_is_f32(mach, &in->src0))) ||
            (nyr_x64_mach_is_f32(mach, &in->dst) &&
             (nyr_x64_mach_is_i64(mach, &in->src0) ||
              nyr_x64_mach_is_f64(mach, &in->src0)))))
        return nyr_x64_mach_scalar_reject(mach, check_inst);
      break;
    case NYR_MACH_SQRT:
    case NYR_MACH_SIN:
    case NYR_MACH_COS:
      if (!nyr_x64_mach_is_f64_operand(mach, &in->dst) ||
          !nyr_x64_mach_is_f64_operand(mach, &in->src0))
        return nyr_x64_mach_scalar_reject(mach, check_inst);
      break;
    case NYR_MACH_BSWAP64:
    case NYR_MACH_BSWAP32:
      if (!nyr_x64_mach_is_i64(mach, &in->dst) ||
          !nyr_x64_mach_is_i64(mach, &in->src0))
        return nyr_x64_mach_scalar_reject(mach, check_inst);
      break;
    case NYR_MACH_LOAD:
      if ((!nyr_x64_mach_is_i64(mach, &in->dst) &&
           !nyr_x64_mach_is_float(mach, &in->dst)) ||
          (in->src0.kind != NYR_MACH_OPERAND_FRAME &&
           !nyr_x64_mach_is_ptr(mach, &in->src0) &&
           !nyr_x64_mach_is_i64(mach, &in->src0)))
        return nyr_x64_mach_scalar_reject(mach, check_inst);
      break;
    case NYR_MACH_STORE:
      if ((in->dst.kind != NYR_MACH_OPERAND_FRAME &&
           !nyr_x64_mach_is_ptr(mach, &in->dst) &&
           !nyr_x64_mach_is_i64(mach, &in->dst)) ||
          (!nyr_x64_mach_is_i64(mach, &in->src0) &&
           !nyr_x64_mach_is_float(mach, &in->src0)))
        return nyr_x64_mach_scalar_reject(mach, check_inst);
      break;
    case NYR_MACH_ADD:
    case NYR_MACH_SUB:
    case NYR_MACH_MUL:
    case NYR_MACH_DIV:
    case NYR_MACH_MOD:
    case NYR_MACH_AND:
    case NYR_MACH_OR:
    case NYR_MACH_XOR:
    case NYR_MACH_SHL:
    case NYR_MACH_SAR:
    case NYR_MACH_ROR:
    case NYR_MACH_ROR32:
      if (!((nyr_x64_mach_is_i64(mach, &in->dst) &&
             nyr_x64_mach_is_i64(mach, &in->src0) &&
             nyr_x64_mach_is_shift_count(mach, &in->src1)) ||
            (in->opcode == NYR_MACH_ADD &&
             nyr_x64_mach_is_i64(mach, &in->dst) &&
             nyr_x64_mach_is_ptr(mach, &in->src0) &&
             nyr_x64_mach_is_i64(mach, &in->src1)) ||
            ((in->opcode == NYR_MACH_ADD || in->opcode == NYR_MACH_SUB ||
              in->opcode == NYR_MACH_MUL || in->opcode == NYR_MACH_DIV) &&
             nyr_x64_mach_is_f64(mach, &in->dst) &&
             nyr_x64_mach_is_f64(mach, &in->src0) &&
             nyr_x64_mach_is_f64(mach, &in->src1)) ||
            ((in->opcode == NYR_MACH_ADD || in->opcode == NYR_MACH_SUB ||
              in->opcode == NYR_MACH_MUL || in->opcode == NYR_MACH_DIV) &&
             nyr_x64_mach_is_f32(mach, &in->dst) &&
             nyr_x64_mach_is_f32(mach, &in->src0) &&
             nyr_x64_mach_is_f32(mach, &in->src1))))
        return nyr_x64_mach_scalar_reject(mach, check_inst);
      break;
    case NYR_MACH_SELECT:
      if (!(nyr_x64_mach_is_i64(mach, &in->dst) &&
            nyr_x64_mach_is_i64(mach, &in->src0) &&
            nyr_x64_mach_is_i64(mach, &in->src1) &&
            nyr_x64_mach_is_i64(mach, &in->src2) &&
            in->condition == NYR_MACH_COND_NE))
        return nyr_x64_mach_scalar_reject(mach, check_inst);
      break;
    case NYR_MACH_CMP:
      if (!(nyr_x64_mach_is_i64(mach, &in->dst) &&
            ((nyr_x64_mach_is_i64(mach, &in->src0) &&
              nyr_x64_mach_is_i64(mach, &in->src1)) ||
             (nyr_x64_mach_is_f64(mach, &in->src0) &&
              nyr_x64_mach_is_f64(mach, &in->src1)) ||
             (nyr_x64_mach_is_f32(mach, &in->src0) &&
              nyr_x64_mach_is_f32(mach, &in->src1))) &&
            in->condition != NYR_MACH_COND_ALWAYS))
        return nyr_x64_mach_scalar_reject(mach, check_inst);
      break;
    case NYR_MACH_RET:
      if (in->src0.kind != NYR_MACH_OPERAND_NONE &&
          !nyr_x64_mach_is_gpr(mach, &in->src0) &&
          !nyr_x64_mach_is_float(mach, &in->src0))
        return nyr_x64_mach_scalar_reject(mach, check_inst);
      break;
    case NYR_MACH_BR:
      if (in->src1.kind != NYR_MACH_OPERAND_BLOCK)
        return nyr_x64_mach_scalar_reject(mach, check_inst);
      break;
    case NYR_MACH_BR_IF:
      if (!nyr_x64_mach_is_gpr(mach, &in->src0) ||
          in->src1.kind != NYR_MACH_OPERAND_BLOCK)
        return nyr_x64_mach_scalar_reject(mach, check_inst);
      break;
    case NYR_MACH_TRAP:
      break;
    case NYR_MACH_CALL:
      if ((in->dst.kind != NYR_MACH_OPERAND_NONE &&
           !nyr_x64_mach_is_gpr(mach, &in->dst) &&
           !nyr_x64_mach_is_float(mach, &in->dst)) ||
          in->src0.kind != NYR_MACH_OPERAND_SYMBOL ||
          !nyr_x64_mach_call_supported(mach, target, in))
        return nyr_x64_mach_scalar_reject(mach, check_inst);
      break;
    default:
      return nyr_x64_mach_scalar_reject(mach, check_inst);
    }
  }
  return true;
}

static long long nyr_x64_mach_offset_ll(const nyr_mach_func_t *mach,
                                        const nyr_mach_operand_t *op) {
  /*
   * Offsets are printed into "-%d(%%rbp)" strings; compute in 64-bit so
   * large frame/vreg indices cannot overflow int (UBSan caught
   * 452884203*8 here on big functions).
   */
  if (!mach || !op)
    return 0;
  if (op->kind == NYR_MACH_OPERAND_FRAME)
    return ((long long)op->as.frame_index + 1) * 8;
  if (op->kind != NYR_MACH_OPERAND_VREG)
    return 0;
  long long v = (long long)mach->frame_slot_len +
                (mach->home_slots && op->as.reg < mach->vreg_len
                     ? mach->home_slots[op->as.reg]
                     : op->as.reg) +
                1;
  v *= 8;
  return v;
}
static int nyr_x64_mach_offset(const nyr_mach_func_t *mach,
                               const nyr_mach_operand_t *op) {
  long long v = nyr_x64_mach_offset_ll(mach, op);
  return v > INT_MAX ? INT_MAX : (int)v;
}

static const char *nyr_x64_mach_setcc(nyr_mach_cond_t condition) {
  switch (condition) {
  case NYR_MACH_COND_EQ:
    return "sete";
  case NYR_MACH_COND_NE:
    return "setne";
  case NYR_MACH_COND_LT:
    return "setl";
  case NYR_MACH_COND_LE:
    return "setle";
  case NYR_MACH_COND_GT:
    return "setg";
  case NYR_MACH_COND_GE:
    return "setge";
  case NYR_MACH_COND_B:
    return "setb";
  case NYR_MACH_COND_AE:
    return "setae";
  case NYR_MACH_COND_BE:
    return "setbe";
  case NYR_MACH_COND_A:
    return "seta";
  default:
    return NULL;
  }
}

static const char *nyr_x64_mach_jcc(nyr_mach_cond_t condition) {
  switch (condition) {
  case NYR_MACH_COND_EQ:
    return "je";
  case NYR_MACH_COND_NE:
    return "jne";
  case NYR_MACH_COND_LT:
    return "jl";
  case NYR_MACH_COND_LE:
    return "jle";
  case NYR_MACH_COND_GT:
    return "jg";
  case NYR_MACH_COND_GE:
    return "jge";
  case NYR_MACH_COND_B:
    return "jb";
  case NYR_MACH_COND_AE:
    return "jae";
  case NYR_MACH_COND_BE:
    return "jbe";
  case NYR_MACH_COND_A:
    return "ja";
  default:
    return NULL;
  }
}

static const char *nyr_x64_mach_float_setcc(nyr_mach_cond_t condition) {
  switch (condition) {
  case NYR_MACH_COND_EQ:
    return "sete";
  case NYR_MACH_COND_NE:
    return "setne";
  case NYR_MACH_COND_LT:
    return "setb";
  case NYR_MACH_COND_LE:
    return "setbe";
  case NYR_MACH_COND_GT:
    return "seta";
  case NYR_MACH_COND_GE:
    return "setae";
  default:
    return NULL;
  }
}

/*
 * Emit `movq -off(%rbp), %rax` unless %rax already holds this vreg slot, as
 * tracked by rax_cached_off (set only when the previous instruction stored or
 * loaded that slot and nothing since clobbered %rax). Only vreg operands are
 * cached: frame slots can be address-taken and are always reloaded.
 */
static bool ny_x64_scalar_emit_rax_load(nyr_writer_t *w,
                                        const nyr_mach_operand_t *op, int off,
                                        int rax_cached_off) {
  if (op->kind == NYR_MACH_OPERAND_VREG && rax_cached_off == off)
    return true;
  if (op->kind == NYR_MACH_OPERAND_IMM)
    return nyr_printf(w, "\tmovabsq\t$%" PRId64 ", %%rax\n", op->as.imm);
  return nyr_printf(w, "\tmovq\t-%d(%%rbp), %%rax\n", off);
}

/*
 * 32-bit counterpart of ny_x64_scalar_emit_rax_load, for nyr_mach_inst_t
 * narrow32 ops (see its field comment). A movl into %eax zero-extends the
 * upper 32 bits of %rax on real hardware, so the %rax cache stays a fully
 * valid 64-bit value afterward -- callers may keep treating
 * rax_cached_off the same way after a narrow load as after a full movq.
 */
static bool ny_x64_scalar_emit_eax_load(nyr_writer_t *w,
                                        const nyr_mach_operand_t *op, int off,
                                        int rax_cached_off) {
  if (op->kind == NYR_MACH_OPERAND_VREG && rax_cached_off == off)
    return true;
  if (op->kind == NYR_MACH_OPERAND_IMM)
    return nyr_printf(w, "\tmovl\t$%u, %%eax\n",
                      (unsigned)(uint32_t)op->as.imm);
  return nyr_printf(w, "\tmovl\t-%d(%%rbp), %%eax\n", off);
}

/*
 * Resolve only literal and COPY-defined machine values.  This is deliberately
 * conservative around PHIs and arithmetic: those require CFG-aware constant
 * propagation, while COPY chains preserve an exact literal by construction.
 */
static bool nyr_x64_mach_const_i64(const nyr_mach_func_t *mach,
                                   const nyr_mach_operand_t *operand,
                                   int64_t *out, unsigned depth) {
  if (!mach || !operand || !out || depth > 32)
    return false;
  if (operand->kind == NYR_MACH_OPERAND_IMM) {
    *out = operand->as.imm;
    return true;
  }
  if (operand->kind != NYR_MACH_OPERAND_VREG)
    return false;
  const nyr_mach_inst_t *definition = NULL;
  for (size_t i = 0; i < mach->inst_len; ++i) {
    const nyr_mach_inst_t *in = &mach->insts[i];
    if (in->dst.kind != NYR_MACH_OPERAND_VREG ||
        in->dst.as.reg != operand->as.reg)
      continue;
    if (definition)
      return false;
    definition = in;
  }
  return definition && definition->opcode == NYR_MACH_COPY &&
         nyr_x64_mach_const_i64(mach, &definition->src0, out, depth + 1);
}

static bool nyr_x64_mach_can_forward_rax_result(const nyr_mach_func_t *mach,
                                                size_t index, size_t block_end,
                                                const nyr_mach_inst_t *in) {
  if (!mach || !in || index + 1 >= block_end || block_end > mach->inst_len ||
      mach->inst_len > 2048 || mach->vreg_len > 4096 ||
      in->dst.kind != NYR_MACH_OPERAND_VREG)
    return false;
  const nyr_mach_inst_t *next = &mach->insts[index + 1];
  if (next->src0.kind != NYR_MACH_OPERAND_VREG ||
      next->src0.as.reg != in->dst.as.reg ||
      !nyr_x64_mach_is_gpr(mach, &next->src0))
    return false;
  /*
   * A coalesced COPY may have the same source and destination home. Its
   * emitter deliberately writes no instruction in that case, so although it
   * is the producer's sole SSA use, it does not actually consume the value
   * from %rax. Forwarding here would omit the producer's home store, then a
   * later instruction could clobber %rax before reloading this shared home.
   * Keep the canonical store for exactly this no-op-copy shape.
   */
  if (next->opcode == NYR_MACH_COPY && !next->narrow32 &&
      next->dst.kind == NYR_MACH_OPERAND_VREG &&
      next->src0.kind == NYR_MACH_OPERAND_VREG &&
      nyr_x64_mach_offset(mach, &next->dst) ==
          nyr_x64_mach_offset(mach, &next->src0) &&
      next->dst.as.reg < mach->vreg_len && next->src0.as.reg < mach->vreg_len &&
      mach->vreg_types[next->dst.as.reg] ==
          mach->vreg_types[next->src0.as.reg] &&
      mach->vreg_classes[next->dst.as.reg] ==
          mach->vreg_classes[next->src0.as.reg])
    return false;
  switch (next->opcode) {
  case NYR_MACH_COPY:
  case NYR_MACH_LOAD:
  case NYR_MACH_STORE:
  case NYR_MACH_ADD:
  case NYR_MACH_SUB:
  case NYR_MACH_MUL:
  case NYR_MACH_DIV:
  case NYR_MACH_MOD:
  case NYR_MACH_AND:
  case NYR_MACH_OR:
  case NYR_MACH_XOR:
  case NYR_MACH_SHL:
  case NYR_MACH_SAR:
  case NYR_MACH_ROR:
  case NYR_MACH_BSWAP64:
  case NYR_MACH_BSWAP32:
  case NYR_MACH_CMP:
  case NYR_MACH_RET:
    break;
  default:
    return false;
  }

  size_t uses = 0;
  for (size_t i = 0; i < mach->inst_len && uses <= 1; ++i) {
    const nyr_mach_inst_t *user = &mach->insts[i];
    const nyr_mach_operand_t operands[] = {user->src0, user->src1, user->src2};
    for (size_t k = 0; k < sizeof(operands) / sizeof(operands[0]); ++k)
      if (operands[k].kind == NYR_MACH_OPERAND_VREG &&
          operands[k].as.reg == in->dst.as.reg)
        ++uses;
    for (size_t k = 0; k < user->args_len && uses <= 1; ++k)
      if (user->args[k].kind == NYR_MACH_OPERAND_VREG &&
          user->args[k].as.reg == in->dst.as.reg)
        ++uses;
  }
  return uses == 1;
}

static bool nyr_x64_mach_compare_branch(const nyr_mach_func_t *mach,
                                        size_t index, size_t block_end,
                                        const nyr_mach_inst_t *compare,
                                        const nyr_mach_inst_t **branch_out) {
  if (branch_out)
    *branch_out = NULL;
  if (!mach || !compare || !branch_out || index + 1 >= block_end ||
      block_end > mach->inst_len || compare->opcode != NYR_MACH_CMP ||
      compare->dst.kind != NYR_MACH_OPERAND_VREG ||
      compare->condition == NYR_MACH_COND_ALWAYS ||
      (compare->src0.kind != NYR_MACH_OPERAND_IMM &&
       !nyr_x64_mach_is_i64(mach, &compare->src0)) ||
      (compare->src1.kind != NYR_MACH_OPERAND_IMM &&
       !nyr_x64_mach_is_i64(mach, &compare->src1)) ||
      !nyr_x64_mach_jcc(compare->condition))
    return false;
  const nyr_mach_inst_t *branch = &mach->insts[index + 1];
  if (branch->opcode != NYR_MACH_BR_IF ||
      branch->src0.kind != NYR_MACH_OPERAND_VREG ||
      branch->src0.as.reg != compare->dst.as.reg ||
      branch->src1.kind != NYR_MACH_OPERAND_BLOCK)
    return false;

  size_t uses = 0;
  for (size_t i = 0; i < mach->inst_len && uses <= 1; ++i) {
    const nyr_mach_inst_t *user = &mach->insts[i];
    const nyr_mach_operand_t *operands[] = {&user->src0, &user->src1,
                                            &user->src2};
    for (size_t k = 0; k < sizeof(operands) / sizeof(operands[0]); ++k)
      if (operands[k]->kind == NYR_MACH_OPERAND_VREG &&
          operands[k]->as.reg == compare->dst.as.reg)
        ++uses;
    for (size_t k = 0; k < user->args_len && uses <= 1; ++k)
      if (user->args[k].kind == NYR_MACH_OPERAND_VREG &&
          user->args[k].as.reg == compare->dst.as.reg)
        ++uses;
  }
  if (uses != 1)
    return false;
  *branch_out = branch;
  return true;
}

static bool nyr_x64_mach_operand_equal(const nyr_mach_operand_t *a,
                                       const nyr_mach_operand_t *b) {
  if (!a || !b || a->kind != b->kind || a->reg_class != b->reg_class ||
      a->relocation != b->relocation || a->addend != b->addend)
    return false;
  switch (a->kind) {
  case NYR_MACH_OPERAND_VREG:
  case NYR_MACH_OPERAND_PREG:
    return a->as.reg == b->as.reg;
  case NYR_MACH_OPERAND_IMM:
    return a->as.imm == b->as.imm;
  case NYR_MACH_OPERAND_FRAME:
    return a->as.frame_index == b->as.frame_index;
  case NYR_MACH_OPERAND_BLOCK:
    return a->as.block_index == b->as.block_index;
  case NYR_MACH_OPERAND_SYMBOL:
    return a->as.symbol == b->as.symbol;
  case NYR_MACH_OPERAND_NONE:
    return true;
  }
  return false;
}

static bool nyr_x64_mach_flags_preserving(const nyr_mach_inst_t *in) {
  if (!in)
    return false;
  switch (in->opcode) {
  case NYR_MACH_NOP:
  case NYR_MACH_COPY:
  case NYR_MACH_LOAD:
  case NYR_MACH_STORE:
  case NYR_MACH_LEA:
    return true;
  default:
    return false;
  }
}

static void nyr_x64_mach_add_predecessor(const nyr_mach_func_t *mach,
                                         uint32_t target, uint32_t candidate,
                                         uint32_t *count, uint32_t *last) {
  if (target >= mach->block_len)
    return;
  if (count[target] != UINT32_MAX)
    ++count[target];
  last[target] = candidate;
}

/*
 * Preserve the hardware flags from a direct compare/branch pair when the
 * false edge enters a single-predecessor block whose second compare repeats
 * the same operands. Only MOV-like operations may intervene: arithmetic,
 * calls, and other control-flow edges invalidate the flags proof.
 */
static void nyr_x64_mach_prepare_reused_cmp_flags(const nyr_mach_func_t *mach,
                                                  bool *reuse_flags) {
  if (!mach || !reuse_flags || !mach->block_len)
    return;
  uint32_t *pred_count = calloc(mach->block_len, sizeof(*pred_count));
  uint32_t *last_pred = malloc(mach->block_len * sizeof(*last_pred));
  if (!pred_count || !last_pred) {
    free(pred_count);
    free(last_pred);
    return;
  }
  for (size_t i = 0; i < mach->block_len; ++i)
    last_pred[i] = UINT32_MAX;
  for (size_t source = 0; source < mach->block_len; ++source) {
    const nyr_mach_block_t *block = &mach->blocks[source];
    size_t start = block->first_inst;
    size_t end = start + block->inst_count;
    if (start > end || end > mach->inst_len || start == end)
      continue;
    const nyr_mach_inst_t *last = &mach->insts[end - 1];
    if (last->opcode == NYR_MACH_BR) {
      nyr_x64_mach_add_predecessor(mach, last->src1.as.block_index,
                                   (uint32_t)source, pred_count, last_pred);
      if (end - start >= 2 && mach->insts[end - 2].opcode == NYR_MACH_BR_IF)
        nyr_x64_mach_add_predecessor(mach,
                                     mach->insts[end - 2].src1.as.block_index,
                                     (uint32_t)source, pred_count, last_pred);
    } else if (last->opcode == NYR_MACH_BR_IF) {
      nyr_x64_mach_add_predecessor(mach, last->src1.as.block_index,
                                   (uint32_t)source, pred_count, last_pred);
      if (source + 1 < mach->block_len)
        nyr_x64_mach_add_predecessor(mach, (uint32_t)(source + 1),
                                     (uint32_t)source, pred_count, last_pred);
    } else if (last->opcode != NYR_MACH_RET && last->opcode != NYR_MACH_TRAP &&
               source + 1 < mach->block_len) {
      nyr_x64_mach_add_predecessor(mach, (uint32_t)(source + 1),
                                   (uint32_t)source, pred_count, last_pred);
    }
  }

  for (size_t source = 0; source < mach->block_len; ++source) {
    const nyr_mach_block_t *first_block = &mach->blocks[source];
    size_t start = first_block->first_inst;
    size_t end = start + first_block->inst_count;
    if (start > end || end > mach->inst_len || end - start < 2)
      continue;
    size_t compare_index;
    uint32_t false_predecessor = (uint32_t)source;
    uint32_t next_block;
    if (mach->insts[end - 1].opcode == NYR_MACH_BR && end - start >= 3 &&
        mach->insts[end - 2].opcode == NYR_MACH_BR_IF) {
      compare_index = end - 3;
      next_block = mach->insts[end - 1].src1.as.block_index;
    } else if (mach->insts[end - 1].opcode == NYR_MACH_BR_IF &&
               source + 1 < mach->block_len) {
      const nyr_mach_block_t *trampoline = &mach->blocks[source + 1];
      size_t trampoline_start = trampoline->first_inst;
      size_t trampoline_end = trampoline_start + trampoline->inst_count;
      if (trampoline->inst_count != 1 || trampoline_start >= trampoline_end ||
          trampoline_end > mach->inst_len ||
          mach->insts[trampoline_start].opcode != NYR_MACH_BR ||
          mach->insts[trampoline_start].src1.kind != NYR_MACH_OPERAND_BLOCK ||
          pred_count[source + 1] != 1 || last_pred[source + 1] != source)
        continue;
      compare_index = end - 2;
      false_predecessor = (uint32_t)(source + 1);
      next_block = mach->insts[trampoline_start].src1.as.block_index;
    } else {
      continue;
    }
    const nyr_mach_inst_t *first_cmp = &mach->insts[compare_index];
    const nyr_mach_inst_t *first_branch = NULL;
    if (!nyr_x64_mach_compare_branch(mach, compare_index, end, first_cmp,
                                     &first_branch))
      continue;
    if (next_block >= mach->block_len || pred_count[next_block] != 1 ||
        last_pred[next_block] != false_predecessor ||
        first_branch->src1.as.block_index == next_block)
      continue;
    const nyr_mach_block_t *second_block = &mach->blocks[next_block];
    size_t second_start = second_block->first_inst;
    size_t second_end = second_start + second_block->inst_count;
    if (second_start > second_end || second_end > mach->inst_len)
      continue;
    size_t second_cmp_index = second_start;
    while (second_cmp_index < second_end &&
           nyr_x64_mach_flags_preserving(&mach->insts[second_cmp_index]))
      ++second_cmp_index;
    if (second_cmp_index >= second_end)
      continue;
    const nyr_mach_inst_t *second_cmp = &mach->insts[second_cmp_index];
    const nyr_mach_inst_t *second_branch = NULL;
    if (!nyr_x64_mach_compare_branch(mach, second_cmp_index, second_end,
                                     second_cmp, &second_branch) ||
        !nyr_x64_mach_operand_equal(&first_cmp->src0, &second_cmp->src0) ||
        !nyr_x64_mach_operand_equal(&first_cmp->src1, &second_cmp->src1) ||
        first_cmp->condition == NYR_MACH_COND_ALWAYS ||
        second_cmp->condition == NYR_MACH_COND_ALWAYS)
      continue;
    if (second_cmp->dst.kind == NYR_MACH_OPERAND_VREG &&
        second_cmp->dst.as.reg < mach->vreg_len &&
        second_cmp->dst.as.reg < NY_X64_NIR_MAX_SLOTS)
      reuse_flags[second_cmp->dst.as.reg] = true;
  }
  free(pred_count);
  free(last_pred);
}

#define NY_X64_MACH_CMP_AND_MAX 16

typedef struct {
  size_t compares[NY_X64_MACH_CMP_AND_MAX];
  size_t ands[NY_X64_MACH_CMP_AND_MAX];
  size_t compare_count;
  size_t and_count;
} nyr_x64_mach_cmp_and_t;

static size_t nyr_x64_mach_vreg_uses(const nyr_mach_func_t *mach,
                                     nyr_mach_reg_t reg) {
  size_t uses = 0;
  for (size_t i = 0; i < mach->inst_len && uses <= 1; ++i) {
    const nyr_mach_inst_t *in = &mach->insts[i];
    const nyr_mach_operand_t operands[] = {in->src0, in->src1, in->src2};
    for (size_t k = 0; k < sizeof(operands) / sizeof(operands[0]); ++k)
      if (operands[k].kind == NYR_MACH_OPERAND_VREG &&
          operands[k].as.reg == reg)
        ++uses;
    for (size_t k = 0; k < in->args_len && uses <= 1; ++k)
      if (in->args[k].kind == NYR_MACH_OPERAND_VREG &&
          in->args[k].as.reg == reg)
        ++uses;
  }
  return uses;
}

static bool nyr_x64_mach_collect_cmp_and(const nyr_mach_func_t *mach,
                                         nyr_mach_reg_t reg,
                                         nyr_x64_mach_cmp_and_t *out,
                                         unsigned depth) {
  if (!mach || !out || depth > NY_X64_MACH_CMP_AND_MAX ||
      reg >= mach->vreg_len || nyr_x64_mach_vreg_uses(mach, reg) != 1)
    return false;
  size_t def = SIZE_MAX, defs = 0;
  for (size_t i = 0; i < mach->inst_len; ++i) {
    const nyr_mach_inst_t *in = &mach->insts[i];
    if (in->dst.kind == NYR_MACH_OPERAND_VREG && in->dst.as.reg == reg) {
      def = i;
      ++defs;
    }
  }
  if (defs != 1 || def == SIZE_MAX)
    return false;
  const nyr_mach_inst_t *in = &mach->insts[def];
  if (in->opcode == NYR_MACH_CMP && nyr_x64_mach_is_i64(mach, &in->src0) &&
      nyr_x64_mach_is_i64(mach, &in->src1) &&
      in->condition != NYR_MACH_COND_ALWAYS &&
      nyr_x64_mach_jcc(in->condition)) {
    if (out->compare_count >= NY_X64_MACH_CMP_AND_MAX)
      return false;
    out->compares[out->compare_count++] = def;
    return true;
  }
  if (in->opcode != NYR_MACH_AND || in->src0.kind != NYR_MACH_OPERAND_VREG ||
      in->src1.kind != NYR_MACH_OPERAND_VREG ||
      out->and_count >= NY_X64_MACH_CMP_AND_MAX)
    return false;
  out->ands[out->and_count++] = def;
  return nyr_x64_mach_collect_cmp_and(mach, in->src0.as.reg, out, depth + 1) &&
         nyr_x64_mach_collect_cmp_and(mach, in->src1.as.reg, out, depth + 1);
}

static const char *nyr_x64_mach_inverse_jcc(nyr_mach_cond_t condition) {
  switch (condition) {
  case NYR_MACH_COND_EQ:
    return "jne";
  case NYR_MACH_COND_NE:
    return "je";
  case NYR_MACH_COND_LT:
    return "jge";
  case NYR_MACH_COND_LE:
    return "jg";
  case NYR_MACH_COND_GT:
    return "jle";
  case NYR_MACH_COND_GE:
    return "jl";
  case NYR_MACH_COND_B:
    return "jae";
  case NYR_MACH_COND_AE:
    return "jb";
  case NYR_MACH_COND_BE:
    return "ja";
  case NYR_MACH_COND_A:
    return "jbe";
  case NYR_MACH_COND_ALWAYS:
    break;
  }
  return NULL;
}

static void nyr_x64_mach_prepare_cmp_and(const nyr_mach_func_t *mach,
                                         bool *fused_cmp, bool *fused_and,
                                         uint32_t *false_target) {
  for (size_t block = 0; block < mach->block_len; ++block) {
    const nyr_mach_block_t *b = &mach->blocks[block];
    size_t end = b->first_inst + b->inst_count;
    for (size_t i = b->first_inst; i < end; ++i) {
      const nyr_mach_inst_t *br = &mach->insts[i];
      if (br->opcode != NYR_MACH_BR_IF ||
          br->src0.kind != NYR_MACH_OPERAND_VREG ||
          br->src1.kind != NYR_MACH_OPERAND_BLOCK)
        continue;
      bool has_false_br =
          i + 1 < end && mach->insts[i + 1].opcode == NYR_MACH_BR &&
          mach->insts[i + 1].src1.kind == NYR_MACH_OPERAND_BLOCK;
      if (!has_false_br && (i + 1 != end || block + 1 >= mach->block_len))
        continue;
      nyr_x64_mach_cmp_and_t tree = {0};
      bool collected =
          nyr_x64_mach_collect_cmp_and(mach, br->src0.as.reg, &tree, 0);
      if (!collected || tree.compare_count < 2)
        continue;
      /*
       * These compares are emitted as early exits. Restrict fusion to a
       * single machine block so a jump cannot bypass a predecessor edge or
       * change which path evaluates part of the boolean expression.
       */
      bool same_block = true;
      for (size_t j = 0; j < tree.compare_count; ++j)
        if (tree.compares[j] < b->first_inst || tree.compares[j] >= i)
          same_block = false;
      for (size_t j = 0; j < tree.and_count; ++j)
        if (tree.ands[j] < b->first_inst || tree.ands[j] >= i)
          same_block = false;
      size_t first_tree_inst = i;
      for (size_t j = 0; j < tree.compare_count; ++j)
        if (tree.compares[j] < first_tree_inst)
          first_tree_inst = tree.compares[j];
      for (size_t j = 0; j < tree.and_count; ++j)
        if (tree.ands[j] < first_tree_inst)
          first_tree_inst = tree.ands[j];
      /*
       * Early branches may bypass only the pure boolean tree itself.
       */
      for (size_t pc = first_tree_inst; same_block && pc < i; ++pc) {
        bool in_tree = false;
        for (size_t j = 0; j < tree.compare_count; ++j)
          in_tree |= tree.compares[j] == pc;
        for (size_t j = 0; j < tree.and_count; ++j)
          in_tree |= tree.ands[j] == pc;
        if (!in_tree)
          same_block = false;
      }
      if (!same_block)
        continue;
      uint32_t false_index = has_false_br
                                 ? mach->insts[i + 1].src1.as.block_index
                                 : (uint32_t)(block + 1);
      for (size_t j = 0; j < tree.compare_count; ++j) {
        const nyr_mach_inst_t *cmp = &mach->insts[tree.compares[j]];
        nyr_mach_reg_t reg = cmp->dst.as.reg;
        if (reg < mach->vreg_len) {
          fused_cmp[reg] = true;
          false_target[reg] = false_index;
        }
      }
      for (size_t j = 0; j < tree.and_count; ++j) {
        const nyr_mach_inst_t *and = &mach->insts[tree.ands[j]];
        if (and->dst.as.reg < mach->vreg_len)
          fused_and[and->dst.as.reg] = true;
      }
    }
  }
}

static bool nyr_x64_mach_emit_rax_result(nyr_writer_t *w,
                                         const nyr_mach_func_t *mach,
                                         size_t index, size_t block_end,
                                         const nyr_mach_inst_t *in, int dst,
                                         int *rax_cached_off) {
  if (nyr_x64_mach_can_forward_rax_result(mach, index, block_end, in)) {
    /*
     * The unique next use is source 0 of the immediately following
     * instruction. Its RAX load can consume this value without a home store.
     */
    *rax_cached_off = dst;
    return true;
  }
  if (!nyr_printf(w, "\tmovq\t%%rax, -%d(%%rbp)\n", dst))
    return false;
  *rax_cached_off = in->dst.kind == NYR_MACH_OPERAND_VREG ? dst : -1;
  return true;
}

bool nyr_x86_64_emit_mach_scalar(nyr_writer_t *w,
                                 const ny_native_target_info_t *target,
                                 const nyr_mach_func_t *mach,
                                 const char *func_name, bool tag_return,
                                 char *err, size_t err_len) {
  const char *name = func_name && func_name[0] ? func_name : "rt_main";
  if (!w || !target)
    return false;
  if (!nyr_x64_mach_scalar_supported(mach, target, name)) {
    if (getenv("NY_DUMP_MACH"))
      nyr_mach_dump(mach, stderr);
    if (err && err_len)
      nyr_set_error(err, err_len, "x86-64 scalar machine form rejected for %s",
                    name);
    return false;
  }
  if (tag_return) {
    for (size_t i = 0; i < mach->inst_len; ++i)
      if (mach->insts[i].opcode == NYR_MACH_RET &&
          nyr_x64_mach_is_float(mach, &mach->insts[i].src0))
        return false;
  }
  size_t vreg_homes = mach->home_slots ? mach->home_slot_count : mach->vreg_len;
  if (mach->frame_slot_len > (SIZE_MAX / 8u) - vreg_homes)
    return false;
  size_t frame_size = (mach->frame_slot_len + vreg_homes) * 8u;
  if (frame_size > (size_t)INT_MAX - 15u)
    return false;
  int frame_bytes = (int)frame_size;
  frame_bytes = (frame_bytes + 15) & ~15;
  /*
   * Register allocation may coalesce virtual registers onto home indices
   * whose highest mapped offset exceeds the nominal frame-slot count. Size
   * the AOT machine frame from the operands actually emitted, just as the
   * NYIR text emitter does, so no spill/home is placed below RSP.
   */
  long long deepest_slot = (long long)mach->frame_slot_len * 8;
  for (size_t i = 0; i < mach->inst_len; ++i) {
    const nyr_mach_inst_t *in = &mach->insts[i];
    const nyr_mach_operand_t *ops[] = {&in->dst, &in->src0, &in->src1,
                                       &in->src2};
    for (size_t op = 0; op < sizeof(ops) / sizeof(ops[0]); ++op) {
      long long off = nyr_x64_mach_offset_ll(mach, ops[op]);
      if (off > deepest_slot)
        deepest_slot = off;
    }
    for (size_t arg = 0; arg < in->args_len; ++arg) {
      long long off = nyr_x64_mach_offset_ll(mach, &in->args[arg]);
      if (off > deepest_slot)
        deepest_slot = off;
    }
  }
  if (deepest_slot > (long long)INT_MAX - 15)
    return false;
  int deepest_frame = ((int)deepest_slot + 15) & ~15;
  if (frame_bytes < deepest_frame)
    frame_bytes = deepest_frame;
  bool fused_cmp[NY_X64_NIR_MAX_SLOTS] = {0};
  bool fused_and[NY_X64_NIR_MAX_SLOTS] = {0};
  bool reuse_cmp_flags[NY_X64_NIR_MAX_SLOTS] = {0};
  uint32_t fused_false[NY_X64_NIR_MAX_SLOTS];
  for (size_t i = 0; i < NY_X64_NIR_MAX_SLOTS; ++i)
    fused_false[i] = UINT32_MAX;
  if (mach->vreg_len <= NY_X64_NIR_MAX_SLOTS) {
    nyr_x64_mach_prepare_cmp_and(mach, fused_cmp, fused_and, fused_false);
    nyr_x64_mach_prepare_reused_cmp_flags(mach, reuse_cmp_flags);
  }
  ny_dbg_mem_layout(
      name, 0, (size_t)frame_bytes, 16,
      "machine frame: source slots followed by virtual-register homes");
  const char *sym = target->symbol_prefix;
  char epilogue[160];
  snprintf(epilogue, sizeof(epilogue), ".Lny_mach_epilogue_%s", name);
  if ((strcmp(target->object_format, "macho") == 0 &&
       !nyr_put(w, "\t.p2align 4, 0x90\n")) ||
      (strcmp(target->object_format, "macho") != 0 &&
       !nyr_printf(w, "\t.type\t%s%s,@function\n", sym, name)) ||
      !nyr_printf(
          w, "\t.globl\t%s%s\n%s%s:\n\tpushq\t%%rbp\n\tmovq\t%%rsp, %%rbp\n",
          sym, name, sym, name) ||
      (frame_bytes && !nyr_printf(w, "\tsubq\t$%d, %%rsp\n", frame_bytes)))
    return false;
  if (strcmp(name, "rt_main") != 0 && mach->frame_slot_len) {
    bool *params = calloc(mach->frame_slot_len, sizeof(*params));
    if (!params)
      return false;
    size_t gp_params = 0, fp_params = 0;
    if (!nyr_x64_mach_params(mach, params, &gp_params, &fp_params)) {
      free(params);
      return false;
    }
    if (ny_trace_enabled("NY_TRACE_ABI"))
      ny_trace_line(
          "MACH_ABI",
          "function=%s formal-slots=%zu frame-slots=%zu gp=%zu fp=%zu abi=%s",
          name, mach->param_count, mach->frame_slot_len, gp_params, fp_params,
          target->abi == NY_NATIVE_ABI_WIN64 ? "win64" : "sysv");
    size_t arg = 0, fp_arg = 0, ordinal = 0, stack_arg = 0;
    for (size_t slot = 0; slot < mach->frame_slot_len; ++slot)
      if (params[slot]) {
        bool is_f32 = mach->frame_slots[slot].type == NYR_MACH_TYPE_F32;
        bool is_f64 = mach->frame_slots[slot].type == NYR_MACH_TYPE_F64;
        bool from_stack = target->abi == NY_NATIVE_ABI_WIN64
                              ? ordinal >= target->gp_arg_reg_count
                          : (is_f32 || is_f64)
                              ? fp_arg >= target->fp_arg_reg_count
                              : arg >= target->gp_arg_reg_count;
        size_t incoming = target->abi == NY_NATIVE_ABI_WIN64
                              ? 16 + ordinal * 8
                              : 16 + stack_arg * 8;
        if (ny_trace_enabled("NY_TRACE_ABI")) {
          const char *reg = NULL;
          if (!from_stack) {
            size_t index = target->abi == NY_NATIVE_ABI_WIN64
                               ? ordinal
                               : (is_f32 || is_f64 ? fp_arg : arg);
            if (is_f32 || is_f64)
              reg = target->fp_arg_regs[index];
            else
              reg = target->gp_arg_regs[index];
          }
          ny_trace_line(
              "MACH_ABI",
              "function=%s slot=%zu type=%s incoming=%s%zu frame=-%zu", name,
              slot,
              is_f64   ? "f64"
              : is_f32 ? "f32"
                       : "gpr",
              from_stack ? "stack+" : reg, from_stack ? incoming : 0,
              (slot + 1) * 8);
        }
        bool saved = false;
        if (from_stack) {
          const char *move = is_f32 ? "movss" : is_f64 ? "movsd" : "movq";
          saved = nyr_printf(
              w, "\t%s\t%zu(%%rbp), %s\n\t%s\t%s, -%zu(%%rbp)\n", move,
              incoming, is_f32 || is_f64 ? "%xmm0" : "%rax", move,
              is_f32 || is_f64 ? "%xmm0" : "%rax", (slot + 1) * 8);
          ++stack_arg;
        } else if (is_f32) {
          size_t index = target->abi == NY_NATIVE_ABI_WIN64 ? ordinal : fp_arg;
          saved = nyr_printf(w, "\tmovss\t%s, -%zu(%%rbp)\n",
                             target->fp_arg_regs[index], (slot + 1) * 8);
        } else if (is_f64) {
          size_t index = target->abi == NY_NATIVE_ABI_WIN64 ? ordinal : fp_arg;
          saved = nyr_printf(w, "\tmovsd\t%s, -%zu(%%rbp)\n",
                             target->fp_arg_regs[index], (slot + 1) * 8);
        } else {
          size_t index = target->abi == NY_NATIVE_ABI_WIN64 ? ordinal : arg;
          saved = nyr_printf(w, "\tmovq\t%s, -%zu(%%rbp)\n",
                             target->gp_arg_regs[index], (slot + 1) * 8);
        }
        if (!saved) {
          free(params);
          return false;
        }
        if (target->abi == NY_NATIVE_ABI_WIN64)
          ++ordinal;
        else if (is_f32 || is_f64)
          ++fp_arg;
        else
          ++arg;
      }
    free(params);
  }
  /*
   * Tracks the mach-offset of the vreg whose value currently sits live in
   * %rax within the current block, or -1 if unknown/stale. Only vregs are
   * ever cached here: frame slots can be address-taken (aliased through a
   * pointer), vregs by construction cannot be, so caching them is always
   * safe. Reset at every block boundary since %rax's contents on entry to
   * a block with multiple predecessors can't be assumed.
   */
  int rax_cached_off = -1;
  for (size_t block = 0; block < mach->block_len; ++block) {
    if (!nyr_printf(w, ".Lny_mach_%s_%zu:\n", name, block))
      return false;
    rax_cached_off = -1;
    const nyr_mach_block_t *b = &mach->blocks[block];
    for (size_t n = 0; n < b->inst_count; ++n) {
      const nyr_mach_inst_t *in = &mach->insts[b->first_inst + n];
      int dst = nyr_x64_mach_offset(mach, &in->dst);
      int a = nyr_x64_mach_offset(mach, &in->src0);
      int b_off = nyr_x64_mach_offset(mach, &in->src1);
      switch (in->opcode) {
      case NYR_MACH_NOP:
        break;
      case NYR_MACH_COPY:
        if (!in->narrow32 && in->dst.kind == NYR_MACH_OPERAND_VREG &&
            in->src0.kind == NYR_MACH_OPERAND_VREG && dst == a &&
            in->dst.as.reg < mach->vreg_len &&
            in->src0.as.reg < mach->vreg_len &&
            mach->vreg_types[in->dst.as.reg] ==
                mach->vreg_types[in->src0.as.reg] &&
            mach->vreg_classes[in->dst.as.reg] ==
                mach->vreg_classes[in->src0.as.reg])
          break;
        if (nyr_x64_mach_is_float(mach, &in->dst)) {
          bool f32 = nyr_x64_mach_is_f32(mach, &in->dst);
          if (in->src0.kind == NYR_MACH_OPERAND_IMM) {
            if (!nyr_printf(w,
                            "\tmovabsq\t$%" PRId64
                            ", %%rax\n\tmovq\t%%rax, %%xmm0\n",
                            in->src0.as.imm))
              return false;
          } else if (!nyr_printf(w, "\t%s\t-%d(%%rbp), %%xmm0\n",
                                 f32 ? "movss" : "movsd", a)) {
            return false;
          }
          if (!nyr_printf(w, "\t%s\t%%xmm0, -%d(%%rbp)\n",
                          f32 ? "movss" : "movsd", dst))
            return false;
          /* The immediate path used %rax; discard its cached stack home. */
          if (in->src0.kind == NYR_MACH_OPERAND_IMM)
            rax_cached_off = -1;
          break;
        }
        if (in->src0.kind == NYR_MACH_OPERAND_IMM) {
          if (!nyr_printf(w, "\tmovabsq\t$%" PRId64 ", %%rax\n",
                          in->src0.as.imm))
            return false;
        } else if (!ny_x64_scalar_emit_rax_load(w, &in->src0, a,
                                                rax_cached_off))
          return false;
        if (!nyr_printf(w, "\tmovq\t%%rax, -%d(%%rbp)\n", dst))
          return false;
        rax_cached_off =
            in->dst.kind == NYR_MACH_OPERAND_VREG
                ? dst
                : (in->src0.kind == NYR_MACH_OPERAND_VREG ? a : -1);
        break;
      case NYR_MACH_LEA:
        if (in->src0.kind == NYR_MACH_OPERAND_FRAME) {
          if (!nyr_printf(w, "\tleaq\t-%d(%%rbp), %%rax\n", a))
            return false;
        } else if (in->src0.kind == NYR_MACH_OPERAND_SYMBOL) {
          if (!nyr_printf(w, "\tleaq\t%s(%%rip), %%rax\n", in->src0.as.symbol))
            return false;
        } else if (in->src1.kind == NYR_MACH_OPERAND_VREG &&
                   in->src2.kind == NYR_MACH_OPERAND_IMM) {
          const char *scale = in->src2.as.imm == 2   ? "2"
                              : in->src2.as.imm == 4 ? "4"
                                                     : "8";
          if (!nyr_printf(w,
                          "\tmovq\t-%d(%%rbp), %%r10\n"
                          "\tmovq\t-%d(%%rbp), %%rax\n"
                          "\tleaq\t(%%rax,%%r10,%s), %%rax\n",
                          b_off, a, scale))
            return false;
        } else if (in->src1.kind == NYR_MACH_OPERAND_IMM) {
          const char *scale = in->src1.as.imm == 2   ? "2"
                              : in->src1.as.imm == 4 ? "4"
                                                     : "8";
          if (!ny_x64_scalar_emit_rax_load(w, &in->src0, a, rax_cached_off) ||
              !nyr_printf(w, "\tleaq\t(%%rax,%%rax,%s), %%rax\n", scale))
            return false;
        } else {
          return false;
        }
        if (!nyr_printf(w, "\tmovq\t%%rax, -%d(%%rbp)\n", dst))
          return false;
        rax_cached_off = in->dst.kind == NYR_MACH_OPERAND_VREG ? dst : -1;
        break;
      case NYR_MACH_CONVERT:
        if (nyr_x64_mach_is_i64(mach, &in->dst)) {
          const char *load =
              nyr_x64_mach_is_f32(mach, &in->src0) ? "movss" : "movsd";
          const char *convert = nyr_x64_mach_is_f32(mach, &in->src0)
                                    ? "cvttss2siq"
                                    : "cvttsd2siq";
          if (!nyr_printf(w,
                          "\t%s\t-%d(%%rbp), %%xmm0\n"
                          "\t%s\t%%xmm0, %%rax\n"
                          "\tmovq\t%%rax, -%d(%%rbp)\n",
                          load, a, convert, dst))
            return false;
          rax_cached_off = in->dst.kind == NYR_MACH_OPERAND_VREG ? dst : -1;
        } else if (nyr_x64_mach_is_f64(mach, &in->dst)) {
          if (nyr_x64_mach_is_i64(mach, &in->src0)) {
            if (!nyr_printf(
                    w,
                    "\tmovq\t-%d(%%rbp), %%rax\n\tcvtsi2sdq\t%%rax, %%xmm0\n",
                    a))
              return false;
          } else if (!nyr_printf(w,
                                 "\tmovss\t-%d(%%rbp), "
                                 "%%xmm0\n\tcvtss2sd\t%%xmm0, %%xmm0\n",
                                 a)) {
            return false;
          }
          if (!nyr_printf(w, "\tmovsd\t%%xmm0, -%d(%%rbp)\n", dst))
            return false;
        } else {
          if (nyr_x64_mach_is_i64(mach, &in->src0)) {
            if (!nyr_printf(
                    w,
                    "\tmovq\t-%d(%%rbp), %%rax\n\tcvtsi2ssq\t%%rax, %%xmm0\n",
                    a))
              return false;
          } else if (!nyr_printf(w,
                                 "\tmovsd\t-%d(%%rbp), "
                                 "%%xmm0\n\tcvtsd2ss\t%%xmm0, %%xmm0\n",
                                 a)) {
            return false;
          }
          if (!nyr_printf(w, "\tmovss\t%%xmm0, -%d(%%rbp)\n", dst))
            return false;
        }
        rax_cached_off = -1;
        break;
      case NYR_MACH_SQRT:
        if (!nyr_printf(w,
                        "\tmovsd\t-%d(%%rbp), %%xmm0\n\tsqrtsd\t%%xmm0, "
                        "%%xmm0\n\tmovsd\t%%xmm0, -%d(%%rbp)\n",
                        a, dst))
          return false;
        rax_cached_off = -1;
        break;
      case NYR_MACH_LOAD:
        if (nyr_x64_mach_is_float(mach, &in->dst)) {
          const char *move =
              nyr_x64_mach_is_f32(mach, &in->dst) ? "movss" : "movsd";
          if (in->src0.kind == NYR_MACH_OPERAND_FRAME) {
            if (!nyr_printf(
                    w, "\t%s\t-%d(%%rbp), %%xmm0\n\t%s\t%%xmm0, -%d(%%rbp)\n",
                    move, a, move, dst))
              return false;
          } else if (!ny_x64_scalar_emit_rax_load(w, &in->src0, a,
                                                  rax_cached_off) ||
                     !nyr_printf(
                         w, "\t%s\t(%%rax), %%xmm0\n\t%s\t%%xmm0, -%d(%%rbp)\n",
                         move, move, dst)) {
            return false;
          }
          rax_cached_off = -1;
          break;
        }
        if (in->src0.kind == NYR_MACH_OPERAND_FRAME) {
          if (!ny_x64_scalar_emit_rax_load(w, &in->src0, a, rax_cached_off))
            return false;
        } else {
          if (!ny_x64_scalar_emit_rax_load(w, &in->src0, a, rax_cached_off))
            return false;
          if (in->byte_width) {
            if (!nyr_printf(w, "\tmovzbl\t(%%rax), %%eax\n"))
              return false;
          } else if (!nyr_printf(w, "\tmovq\t(%%rax), %%rax\n")) {
            return false;
          }
        }
        if (!nyr_printf(w, "\tmovq\t%%rax, -%d(%%rbp)\n", dst))
          return false;
        rax_cached_off = in->dst.kind == NYR_MACH_OPERAND_VREG ? dst : -1;
        break;
      case NYR_MACH_STORE:
        if (nyr_x64_mach_is_float(mach, &in->src0)) {
          const char *move =
              nyr_x64_mach_is_f32(mach, &in->src0) ? "movss" : "movsd";
          if (in->dst.kind == NYR_MACH_OPERAND_FRAME) {
            if (!nyr_printf(
                    w, "\t%s\t-%d(%%rbp), %%xmm0\n\t%s\t%%xmm0, -%d(%%rbp)\n",
                    move, a, move, dst))
              return false;
          } else {
            /*
             * Indirect store: MACH_STORE convention is dst=POINTER,
             * src0=VALUE. The previous form loaded the value bits into
             * rax and dereferenced them (wild read + garbage store).
             */
            if (!nyr_printf(w, "\tmovq\t-%d(%%rbp), %%rax\n", dst))
              return false;
            if (!nyr_printf(w, "\t%s\t-%d(%%rbp), %%xmm0\n", move, a))
              return false;
            if (!nyr_printf(w, "\t%s\t%%xmm0, (%%rax)\n", move))
              return false;
          }
          rax_cached_off = -1;
          break;
        }
        if (!ny_x64_scalar_emit_rax_load(w, &in->src0, a, rax_cached_off))
          return false;
        if (in->dst.kind == NYR_MACH_OPERAND_FRAME) {
          if (!nyr_printf(w, "\tmovq\t%%rax, -%d(%%rbp)\n", dst))
            return false;
        } else if (in->byte_width) {
          if (!nyr_printf(
                  w, "\tmovq\t-%d(%%rbp), %%rcx\n\tmovb\t%%al, (%%rcx)\n", dst))
            return false;
        } else if (!nyr_printf(
                       w, "\tmovq\t-%d(%%rbp), %%rcx\n\tmovq\t%%rax, (%%rcx)\n",
                       dst)) {
          return false;
        }
        rax_cached_off = in->src0.kind == NYR_MACH_OPERAND_VREG ? a : -1;
        break;
      case NYR_MACH_ADD:
      case NYR_MACH_SUB:
      case NYR_MACH_AND:
      case NYR_MACH_OR:
      case NYR_MACH_XOR:
      case NYR_MACH_MUL:
        if (in->opcode == NYR_MACH_AND &&
            in->dst.kind == NYR_MACH_OPERAND_VREG &&
            in->dst.as.reg < mach->vreg_len &&
            in->dst.as.reg < NY_X64_NIR_MAX_SLOTS &&
            fused_and[in->dst.as.reg]) {
          rax_cached_off = -1;
          break;
        }
        if (nyr_x64_mach_is_float(mach, &in->dst)) {
          bool f32 = nyr_x64_mach_is_f32(mach, &in->dst);
          const char *op = in->opcode == NYR_MACH_ADD   ? "addsd"
                           : in->opcode == NYR_MACH_SUB ? "subsd"
                                                        : "mulsd";
          if (f32)
            op = in->opcode == NYR_MACH_ADD   ? "addss"
                 : in->opcode == NYR_MACH_SUB ? "subss"
                                              : "mulss";
          if (!nyr_printf(w,
                          "\t%s\t-%d(%%rbp), %%xmm0\n\t%s\t-%d(%%rbp), "
                          "%%xmm0\n\t%s\t%%xmm0, -%d(%%rbp)\n",
                          f32 ? "movss" : "movsd", a, op, b_off,
                          f32 ? "movss" : "movsd", dst))
            return false;
          rax_cached_off = -1;
          break;
        }
        {
          int64_t immediate = 0;
          const nyr_mach_operand_t *value_operand = &in->src0;
          bool immediate_rhs =
              nyr_x64_mach_const_i64(mach, &in->src1, &immediate, 0);
          bool commutative = in->opcode != NYR_MACH_SUB;
          if (!immediate_rhs && commutative &&
              nyr_x64_mach_const_i64(mach, &in->src0, &immediate, 0)) {
            immediate_rhs = true;
            value_operand = &in->src1;
          }
          if (immediate_rhs && (int64_t)(int32_t)immediate == immediate) {
            unsigned lea_scale =
                in->opcode == NYR_MACH_MUL && immediate == 3   ? 2u
                : in->opcode == NYR_MACH_MUL && immediate == 5 ? 4u
                : in->opcode == NYR_MACH_MUL && immediate == 9 ? 8u
                                                               : 0u;
            const char *op = in->opcode == NYR_MACH_ADD   ? "add"
                             : in->opcode == NYR_MACH_SUB ? "sub"
                             : in->opcode == NYR_MACH_AND ? "and"
                             : in->opcode == NYR_MACH_OR  ? "or"
                             : in->opcode == NYR_MACH_XOR ? "xor"
                                                          : "imul";
            int value_off = value_operand == &in->src0 ? a : b_off;
            if (in->narrow32) {
              if (!ny_x64_scalar_emit_eax_load(w, value_operand, value_off,
                                               rax_cached_off) ||
                  (lea_scale
                       ? !nyr_printf(w, "\tleal\t(%%eax,%%eax,%u), %%eax\n",
                                     lea_scale)
                       : !nyr_printf(w, "\t%sl\t$%" PRId64 ", %%eax\n", op,
                                     immediate)) ||
                  !nyr_x64_mach_emit_rax_result(w, mach, b->first_inst + n,
                                                b->first_inst + b->inst_count,
                                                in, dst, &rax_cached_off))
                return false;
            } else {
              if (!ny_x64_scalar_emit_rax_load(w, value_operand, value_off,
                                               rax_cached_off) ||
                  (lea_scale
                       ? !nyr_printf(w, "\tleaq\t(%%rax,%%rax,%u), %%rax\n",
                                     lea_scale)
                       : !nyr_printf(w, "\t%sq\t$%" PRId64 ", %%rax\n", op,
                                     immediate)) ||
                  !nyr_x64_mach_emit_rax_result(w, mach, b->first_inst + n,
                                                b->first_inst + b->inst_count,
                                                in, dst, &rax_cached_off))
                return false;
            }
            rax_cached_off = in->dst.kind == NYR_MACH_OPERAND_VREG ? dst : -1;
            break;
          }
        }
        if (in->src1.kind == NYR_MACH_OPERAND_IMM) {
          if (in->narrow32) {
            if (!ny_x64_scalar_emit_eax_load(w, &in->src0, a, rax_cached_off) ||
                !nyr_printf(w, "\tmovl\t$%u, %%r10d\n",
                            (unsigned)(uint32_t)in->src1.as.imm))
              return false;
            const char *op = in->opcode == NYR_MACH_ADD   ? "addl"
                             : in->opcode == NYR_MACH_SUB ? "subl"
                             : in->opcode == NYR_MACH_AND ? "andl"
                             : in->opcode == NYR_MACH_OR  ? "orl"
                             : in->opcode == NYR_MACH_XOR ? "xorl"
                                                          : "imull";
            if (!nyr_printf(w, "\t%s\t%%r10d, %%eax\n", op) ||
                !nyr_x64_mach_emit_rax_result(w, mach, b->first_inst + n,
                                              b->first_inst + b->inst_count, in,
                                              dst, &rax_cached_off))
              return false;
          } else {
            if (!ny_x64_scalar_emit_rax_load(w, &in->src0, a, rax_cached_off) ||
                !nyr_printf(w, "\tmovabsq\t$%" PRId64 ", %%r10\n",
                            in->src1.as.imm))
              return false;
            const char *op = in->opcode == NYR_MACH_ADD   ? "addq"
                             : in->opcode == NYR_MACH_SUB ? "subq"
                             : in->opcode == NYR_MACH_AND ? "andq"
                             : in->opcode == NYR_MACH_OR  ? "orq"
                             : in->opcode == NYR_MACH_XOR ? "xorq"
                                                          : "imulq";
            if (!nyr_printf(w, "\t%s\t%%r10, %%rax\n", op) ||
                !nyr_x64_mach_emit_rax_result(w, mach, b->first_inst + n,
                                              b->first_inst + b->inst_count, in,
                                              dst, &rax_cached_off))
              return false;
          }
          rax_cached_off = in->dst.kind == NYR_MACH_OPERAND_VREG ? dst : -1;
          break;
        }
        if (in->narrow32) {
          /*
           * The range pass guarantees either an exact u32 result or a
           * low-word-only consumer chain (see nyr_mach_inst_t).
           */
          if (!ny_x64_scalar_emit_eax_load(w, &in->src0, a, rax_cached_off))
            return false;
          {
            const char *op = in->opcode == NYR_MACH_ADD   ? "addl"
                             : in->opcode == NYR_MACH_SUB ? "subl"
                             : in->opcode == NYR_MACH_AND ? "andl"
                             : in->opcode == NYR_MACH_OR  ? "orl"
                             : in->opcode == NYR_MACH_XOR ? "xorl"
                                                          : "imull";
            if (!nyr_printf(w, "\t%s\t-%d(%%rbp), %%eax\n", op, b_off) ||
                !nyr_x64_mach_emit_rax_result(w, mach, b->first_inst + n,
                                              b->first_inst + b->inst_count, in,
                                              dst, &rax_cached_off))
              return false;
          }
          rax_cached_off = in->dst.kind == NYR_MACH_OPERAND_VREG ? dst : -1;
          break;
        }
        if (!ny_x64_scalar_emit_rax_load(w, &in->src0, a, rax_cached_off))
          return false;
        {
          const char *op = in->opcode == NYR_MACH_ADD   ? "addq"
                           : in->opcode == NYR_MACH_SUB ? "subq"
                           : in->opcode == NYR_MACH_AND ? "andq"
                           : in->opcode == NYR_MACH_OR  ? "orq"
                           : in->opcode == NYR_MACH_XOR ? "xorq"
                                                        : "imulq";
          if (!nyr_printf(w, "\t%s\t-%d(%%rbp), %%rax\n", op, b_off) ||
              !nyr_x64_mach_emit_rax_result(w, mach, b->first_inst + n,
                                            b->first_inst + b->inst_count, in,
                                            dst, &rax_cached_off))
            return false;
        }
        rax_cached_off = in->dst.kind == NYR_MACH_OPERAND_VREG ? dst : -1;
        break;
      case NYR_MACH_DIV:
      case NYR_MACH_MOD:
        if (nyr_x64_mach_is_float(mach, &in->dst)) {
          bool f32 = nyr_x64_mach_is_f32(mach, &in->dst);
          if (!nyr_printf(w,
                          "\t%s\t-%d(%%rbp), %%xmm0\n\t%s\t-%d(%%rbp), "
                          "%%xmm0\n\t%s\t%%xmm0, -%d(%%rbp)\n",
                          f32 ? "movss" : "movsd", a, f32 ? "divss" : "divsd",
                          b_off, f32 ? "movss" : "movsd", dst))
            return false;
          rax_cached_off = -1;
          break;
        }
        /*
         * Match the object and NYIR emitters for compile-time divisors.  The
         * scalar text emitter is the usual x86 path, so leaving this out made
         * constant remainders in optimized loops fall back to expensive idiv.
         */
        int64_t divisor = 0;
        if (nyr_x64_mach_const_i64(mach, &in->src1, &divisor, 0) &&
            divisor != 0 && divisor != 1 && divisor != -1) {
          uint64_t ad =
              divisor >= 0 ? (uint64_t)divisor : (uint64_t)(-(divisor + 1)) + 1;
          if ((ad & (ad - 1)) == 0) {
            /*
             * Signed power-of-two division truncates toward zero by adding
             * the sign mask's discarded-bit mask before the arithmetic
             * shift. The bias is valid for exact multiples too: for example,
             * (-4 + 1) >> 1 is still -2. Avoid testing the low bits and
             * conditionally selecting zero; those instructions do not change
             * the quotient and are especially costly in tight loops.
             */
            unsigned shift = 0;
            while ((UINT64_C(1) << shift) != ad)
              ++shift;
            uint64_t mask = ad - 1;
            if (!ny_x64_scalar_emit_rax_load(w, &in->src0, a, rax_cached_off) ||
                (in->opcode == NYR_MACH_MOD &&
                 !nyr_put(w, "\tmovq\t%rax, %r9\n")) ||
                !nyr_put(w, "\tmovq\t%rax, %rcx\n\tsarq\t$63, %rcx\n") ||
                !nyr_printf(w, "\tandq\t$%" PRIu64 ", %%rcx\n", mask) ||
                !nyr_put(w, "\taddq\t%rcx, %rax\n") ||
                !nyr_printf(w, "\tsarq\t$%u, %%rax\n", shift))
              return false;
            if (in->opcode == NYR_MACH_DIV && divisor < 0 &&
                !nyr_put(w, "\tnegq\t%rax\n"))
              return false;
            if (in->opcode == NYR_MACH_MOD) {
              if (!nyr_printf(w,
                              "\tmovabsq\t$%" PRId64 ", %%r10\n"
                              "\timulq\t%%r10, %%rax\n"
                              "\tmovq\t%%r9, %%r10\n"
                              "\tsubq\t%%rax, %%r10\n"
                              "\tmovq\t%%r10, %%rax\n",
                              (int64_t)ad))
                return false;
            }
            if (!nyr_printf(w, "\tmovq\t%%rax, -%d(%%rbp)\n", dst))
              return false;
            rax_cached_off = in->dst.kind == NYR_MACH_OPERAND_VREG ? dst : -1;
            break;
          }
          if ((ad & (ad - 1)) != 0) {
            int64_t magic = 0;
            unsigned shift = 0;
            nyr_sdiv_magic_i64(ad, &magic, &shift);
            if (!ny_x64_scalar_emit_rax_load(w, &in->src0, a, rax_cached_off) ||
                !nyr_put(w, "\tmovq\t%rax, %r9\n") ||
                !nyr_printf(w, "\tmovabsq\t$%" PRId64 ", %%r10\n", magic) ||
                !nyr_put(w, "\timulq\t%r10\n\tmovq\t%rdx, %rax\n"))
              return false;
            if (magic < 0 && !nyr_put(w, "\taddq\t%r9, %rax\n"))
              return false;
            if (shift && !nyr_printf(w, "\tsarq\t$%u, %%rax\n", shift))
              return false;
            if (!nyr_put(w, "\tmovq\t%r9, %rcx\n\tsarq\t$63, %rcx\n"
                            "\tsubq\t%rcx, %rax\n"))
              return false;
            if (in->opcode == NYR_MACH_DIV && divisor < 0 &&
                !nyr_put(w, "\tnegq\t%rax\n"))
              return false;
            if (in->opcode == NYR_MACH_MOD) {
              if (!nyr_put(w, "\tmovq\t%rax, %r10\n\tmovq\t%r9, %rax\n"))
                return false;
              if ((int64_t)(int32_t)ad == (int64_t)ad) {
                if (!nyr_printf(w, "\timulq\t$%" PRId64 ", %%r10, %%r10\n",
                                (int64_t)ad))
                  return false;
              } else if (!nyr_printf(w,
                                     "\tmovabsq\t$%" PRId64 ", %%r11\n"
                                     "\timulq\t%%r11, %%r10\n",
                                     (int64_t)ad)) {
                return false;
              }
              if (!nyr_put(w, "\tsubq\t%r10, %rax\n"))
                return false;
            }
            if (!nyr_printf(w, "\tmovq\t%%rax, -%d(%%rbp)\n", dst))
              return false;
            rax_cached_off = in->dst.kind == NYR_MACH_OPERAND_VREG ? dst : -1;
            break;
          }
        }
        if (!ny_x64_scalar_emit_rax_load(w, &in->src0, a, rax_cached_off))
          return false;
        if (in->src1.kind == NYR_MACH_OPERAND_IMM) {
          /*
           * x86 IDIV has no immediate form.  Treating an immediate operand
           * like a vreg home silently divides by an unrelated stack value;
           * materialize it in a caller-saved scratch register instead.
           */
          if (!nyr_printf(w,
                          "\tmovabsq\t$%" PRId64 ", %%r10\n"
                          "\tcqto\n\tidivq\t%%r10\n"
                          "\tmovq\t%%%s, -%d(%%rbp)\n",
                          in->src1.as.imm,
                          in->opcode == NYR_MACH_DIV ? "rax" : "rdx", dst))
            return false;
        } else if (!nyr_printf(w,
                               "\tcqto\n\tidivq\t-%d(%%rbp)\n\tmovq\t%%%s, "
                               "-%d(%%rbp)\n",
                               b_off,
                               in->opcode == NYR_MACH_DIV ? "rax" : "rdx", dst))
          return false;
        rax_cached_off =
            in->opcode == NYR_MACH_DIV && in->dst.kind == NYR_MACH_OPERAND_VREG
                ? dst
                : -1;
        break;
      case NYR_MACH_SHL:
      case NYR_MACH_SAR:
      case NYR_MACH_ROR:
        if (!ny_x64_scalar_emit_rax_load(w, &in->src0, a, rax_cached_off))
          return false;
        {
          int64_t shift_count = 0;
          if (nyr_x64_mach_const_i64(mach, &in->src1, &shift_count, 0)) {
            shift_count = (int64_t)((uint64_t)shift_count & UINT64_C(63));
            if (!nyr_printf(w, "\t%s\t$%" PRId64 ", %%rax\n",
                            in->opcode == NYR_MACH_SHL   ? "shlq"
                            : in->opcode == NYR_MACH_SAR ? "sarq"
                                                         : "rorq",
                            shift_count) ||
                !nyr_x64_mach_emit_rax_result(w, mach, b->first_inst + n,
                                              b->first_inst + b->inst_count, in,
                                              dst, &rax_cached_off))
              return false;
          } else {
            if (!nyr_printf(w,
                            "\tmovq\t-%d(%%rbp), %%rcx\n"
                            "\t%s\t%%cl, %%rax\n",
                            b_off,
                            in->opcode == NYR_MACH_SHL   ? "shlq"
                            : in->opcode == NYR_MACH_SAR ? "sarq"
                                                         : "rorq") ||
                !nyr_x64_mach_emit_rax_result(w, mach, b->first_inst + n,
                                              b->first_inst + b->inst_count, in,
                                              dst, &rax_cached_off))
              return false;
          }
        }
        rax_cached_off = in->dst.kind == NYR_MACH_OPERAND_VREG ? dst : -1;
        break;
      case NYR_MACH_ROR32:
        if (!nyr_printf(w, "\tmovl\t-%d(%%rbp), %%eax\n", a))
          return false;
        {
          int64_t rotate_count = 0;
          if (nyr_x64_mach_const_i64(mach, &in->src1, &rotate_count, 0)) {
            rotate_count = (int64_t)((uint64_t)rotate_count & UINT64_C(31));
            if (!nyr_printf(w, "\trorl\t$%" PRId64 ", %%eax\n", rotate_count) ||
                !nyr_x64_mach_emit_rax_result(w, mach, b->first_inst + n,
                                              b->first_inst + b->inst_count, in,
                                              dst, &rax_cached_off))
              return false;
          } else if (!nyr_printf(w,
                                 "\tmovq\t-%d(%%rbp), %%rcx\n"
                                 "\trorl\t%%cl, %%eax\n",
                                 b_off) ||
                     !nyr_x64_mach_emit_rax_result(w, mach, b->first_inst + n,
                                                   b->first_inst +
                                                       b->inst_count,
                                                   in, dst, &rax_cached_off)) {
            return false;
          }
        }
        rax_cached_off = in->dst.kind == NYR_MACH_OPERAND_VREG ? dst : -1;
        break;
      case NYR_MACH_BSWAP64:
      case NYR_MACH_BSWAP32:
        if (!ny_x64_scalar_emit_rax_load(w, &in->src0, a, rax_cached_off))
          return false;
        if (!nyr_printf(w, "\t%s\t%%%s\n\tmovq\t%%rax, -%d(%%rbp)\n",
                        in->opcode == NYR_MACH_BSWAP64 ? "bswapq" : "bswapl",
                        in->opcode == NYR_MACH_BSWAP64 ? "rax" : "eax", dst))
          return false;
        rax_cached_off = in->dst.kind == NYR_MACH_OPERAND_VREG ? dst : -1;
        break;
      case NYR_MACH_SELECT: {
        int false_off = nyr_x64_mach_offset(mach, &in->src2);
        if (!nyr_printf(
                w,
                "\tmovq\t-%d(%%rbp), %%rax\n\ttestq\t%%rax, "
                "%%rax\n\tmovq\t-%d(%%rbp), %%rax\n\tcmovne\t-%d(%%rbp), "
                "%%rax\n\tmovq\t%%rax, -%d(%%rbp)\n",
                a, false_off, b_off, dst))
          return false;
        rax_cached_off = in->dst.kind == NYR_MACH_OPERAND_VREG ? dst : -1;
        break;
      }
      case NYR_MACH_CMP: {
        const nyr_mach_inst_t *reuse_branch = NULL;
        size_t block_end = b->first_inst + b->inst_count;
        if (in->dst.kind == NYR_MACH_OPERAND_VREG &&
            in->dst.as.reg < mach->vreg_len &&
            in->dst.as.reg < NY_X64_NIR_MAX_SLOTS &&
            reuse_cmp_flags[in->dst.as.reg] &&
            nyr_x64_mach_compare_branch(mach, b->first_inst + n, block_end, in,
                                        &reuse_branch)) {
          const char *jcc = nyr_x64_mach_jcc(in->condition);
          if (!jcc || !nyr_printf(w, "\t%s\t.Lny_mach_%s_%u\n", jcc, name,
                                  reuse_branch->src1.as.block_index))
            return false;
          ++n; /* the conditional branch reuses the predecessor's flags */
          rax_cached_off = -1;
          break;
        }
        if (in->dst.kind == NYR_MACH_OPERAND_VREG &&
            in->dst.as.reg < mach->vreg_len &&
            in->dst.as.reg < NY_X64_NIR_MAX_SLOTS &&
            fused_cmp[in->dst.as.reg] &&
            fused_false[in->dst.as.reg] != UINT32_MAX) {
          const char *jcc = nyr_x64_mach_inverse_jcc(in->condition);
          int64_t immediate = 0;
          bool rhs_constant =
              nyr_x64_mach_const_i64(mach, &in->src1, &immediate, 0);
          if (!jcc ||
              !ny_x64_scalar_emit_rax_load(w, &in->src0, a, rax_cached_off))
            return false;
          if (rhs_constant && (int64_t)(int32_t)immediate == immediate) {
            if (!nyr_printf(w, "\tcmpq\t$%" PRId64 ", %%rax\n", immediate))
              return false;
          } else if (rhs_constant || in->src1.kind == NYR_MACH_OPERAND_IMM) {
            int64_t value = rhs_constant ? immediate : in->src1.as.imm;
            if (!nyr_printf(
                    w, "\tmovabsq\t$%" PRId64 ", %%r10\n\tcmpq\t%%r10, %%rax\n",
                    value))
              return false;
          } else if (!nyr_printf(w, "\tcmpq\t-%d(%%rbp), %%rax\n", b_off)) {
            return false;
          }
          if (!nyr_printf(w, "\t%s\t.Lny_mach_%s_%u\n", jcc, name,
                          fused_false[in->dst.as.reg]))
            return false;
          rax_cached_off = -1;
          break;
        }
        const nyr_mach_inst_t *branch = NULL;
        if (nyr_x64_mach_compare_branch(mach, b->first_inst + n, block_end, in,
                                        &branch)) {
          const char *jcc = nyr_x64_mach_jcc(in->condition);
          int64_t immediate = 0;
          bool rhs_constant =
              nyr_x64_mach_const_i64(mach, &in->src1, &immediate, 0);
          if (!ny_x64_scalar_emit_rax_load(w, &in->src0, a, rax_cached_off))
            return false;
          if (rhs_constant && (int64_t)(int32_t)immediate == immediate) {
            if (!nyr_printf(w, "\tcmpq\t$%" PRId64 ", %%rax\n", immediate))
              return false;
          } else if (rhs_constant || in->src1.kind == NYR_MACH_OPERAND_IMM) {
            int64_t value = rhs_constant ? immediate : in->src1.as.imm;
            if (!nyr_printf(
                    w, "\tmovabsq\t$%" PRId64 ", %%r10\n\tcmpq\t%%r10, %%rax\n",
                    value))
              return false;
          } else if (!nyr_printf(w, "\tcmpq\t-%d(%%rbp), %%rax\n", b_off)) {
            return false;
          }
          if (!jcc || !nyr_printf(w, "\t%s\t.Lny_mach_%s_%u\n", jcc, name,
                                  branch->src1.as.block_index))
            return false;
          ++n; /* the conditional branch was emitted with the comparison */
          rax_cached_off = -1;
          break;
        }
        if (nyr_x64_mach_is_float(mach, &in->src0)) {
          const char *cc = nyr_x64_mach_float_setcc(in->condition);
          const char *cmp =
              nyr_x64_mach_is_f32(mach, &in->src0) ? "ucomiss" : "ucomisd";
          const char *move =
              nyr_x64_mach_is_f32(mach, &in->src0) ? "movss" : "movsd";
          int unordered_result = in->condition == NYR_MACH_COND_NE ? 1 : 0;
          char unordered[384];
          char done[384];
          snprintf(unordered, sizeof(unordered),
                   ".Lny_mach_fcmp_unordered_%s_%u", name, in->dst.as.reg);
          snprintf(done, sizeof(done), ".Lny_mach_fcmp_done_%s_%u", name,
                   in->dst.as.reg);
          if (!cc || !nyr_printf(w,
                                 "\t%s\t-%d(%%rbp), %%xmm0\n\t%s\t-%d(%%rbp), "
                                 "%%xmm1\n\t%s\t%%xmm1, "
                                 "%%xmm0\n\tjp\t%s\n\t%s\t%%al\n\tjmp\t%s\n%s:"
                                 "\n\tmovb\t$%d, %%al\n%s:\n\tmovzbq\t%%al, "
                                 "%%rax\n\tmovq\t%%rax, -%d(%%rbp)\n",
                                 move, a, move, b_off, cmp, unordered, cc, done,
                                 unordered, unordered_result, done, dst))
            return false;
          rax_cached_off = -1;
          break;
        }
        const char *cc = nyr_x64_mach_setcc(in->condition);
        if (!cc ||
            !ny_x64_scalar_emit_rax_load(w, &in->src0, a, rax_cached_off))
          return false;
        if (!cc || !nyr_printf(
                       w,
                       "\tcmpq\t-%d(%%rbp), %%rax\n\t%s\t%%al\n\tmovzbq\t%%al, "
                       "%%rax\n\tmovq\t%%rax, -%d(%%rbp)\n",
                       b_off, cc, dst))
          return false;
        rax_cached_off = in->dst.kind == NYR_MACH_OPERAND_VREG ? dst : -1;
        break;
      }
      case NYR_MACH_BR:
        if (!nyr_printf(w, "\tjmp\t.Lny_mach_%s_%u\n", name,
                        in->src1.as.block_index))
          return false;
        break;
      case NYR_MACH_BR_IF:
        if (in->src0.kind == NYR_MACH_OPERAND_VREG &&
            in->src0.as.reg < mach->vreg_len &&
            in->src0.as.reg < NY_X64_NIR_MAX_SLOTS &&
            fused_and[in->src0.as.reg]) {
          if (!nyr_printf(w, "\tjmp\t.Lny_mach_%s_%u\n", name,
                          in->src1.as.block_index))
            return false;
          if (n + 1 < b->inst_count &&
              mach->insts[b->first_inst + n + 1].opcode == NYR_MACH_BR)
            ++n;
          rax_cached_off = -1;
          break;
        }
        if (!nyr_printf(w, "\tcmpq\t$0, -%d(%%rbp)\n\tjne\t.Lny_mach_%s_%u\n",
                        a, name, in->src1.as.block_index))
          return false;
        break;
      case NYR_MACH_TRAP:
        /*
         * ud2 — raises #UD, terminating the process.
         */
        if (!nyr_printf(w, "\tud2\n"))
          return false;
        break;
      case NYR_MACH_CALL: {
        size_t gp = 0, fp = 0, stack_args = 0;
        for (size_t arg = 0; arg < in->args_len; ++arg) {
          bool is_float = nyr_x64_mach_is_float(mach, &in->args[arg]);
          bool on_stack = target->abi == NY_NATIVE_ABI_WIN64
                              ? arg >= target->gp_arg_reg_count
                          : is_float ? fp >= target->fp_arg_reg_count
                                     : gp >= target->gp_arg_reg_count;
          if (on_stack)
            ++stack_args;
          else if (target->abi == NY_NATIVE_ABI_SYSV && is_float)
            ++fp;
          else if (target->abi == NY_NATIVE_ABI_SYSV)
            ++gp;
        }
        size_t call_bytes = target->shadow_space_bytes + stack_args * 8;
        size_t stack_bytes = (call_bytes + 15) & ~(size_t)15;
        if (stack_bytes && !nyr_printf(w, "\tsubq\t$%zu, %%rsp\n", stack_bytes))
          return false;
        gp = fp = 0;
        size_t stack_arg = 0;
        for (size_t arg = 0; arg < in->args_len; ++arg) {
          int arg_off = nyr_x64_mach_offset(mach, &in->args[arg]);
          size_t gp_index = target->abi == NY_NATIVE_ABI_WIN64 ? arg : gp;
          size_t fp_index = target->abi == NY_NATIVE_ABI_WIN64 ? arg : fp;
          bool is_f32 = nyr_x64_mach_is_f32(mach, &in->args[arg]);
          bool is_f64 = nyr_x64_mach_is_f64(mach, &in->args[arg]);
          bool on_stack = target->abi == NY_NATIVE_ABI_WIN64
                              ? arg >= target->gp_arg_reg_count
                          : (is_f32 || is_f64) ? fp >= target->fp_arg_reg_count
                                               : gp >= target->gp_arg_reg_count;
          if (on_stack) {
            size_t offset = target->shadow_space_bytes + stack_arg * 8;
            const char *move = is_f32 ? "movss" : is_f64 ? "movsd" : "movq";
            const char *scratch =
                is_f32 || is_f64
                    ? target->abi == NY_NATIVE_ABI_WIN64 ? "%xmm4" : "%xmm8"
                    : "%rax";
            if (!nyr_printf(w, "\t%s\t-%d(%%rbp), %s\n\t%s\t%s, %zu(%%rsp)\n",
                            move, arg_off, scratch, move, scratch, offset))
              return false;
            ++stack_arg;
          } else if (is_f32) {
            if (!nyr_printf(w, "\tmovss\t-%d(%%rbp), %s\n", arg_off,
                            target->fp_arg_regs[fp_index]))
              return false;
            if (target->abi == NY_NATIVE_ABI_SYSV)
              ++fp;
          } else if (is_f64) {
            if (!nyr_printf(w, "\tmovsd\t-%d(%%rbp), %s\n", arg_off,
                            target->fp_arg_regs[fp_index]))
              return false;
            if (target->abi == NY_NATIVE_ABI_SYSV)
              ++fp;
          } else {
            if (!nyr_printf(w, "\tmovq\t-%d(%%rbp), %s\n", arg_off,
                            target->gp_arg_regs[gp_index]))
              return false;
            if (target->abi == NY_NATIVE_ABI_SYSV)
              ++gp;
          }
        }
        if (!nyr_printf(w, "\tcall\t%s%s%s\n", target->symbol_prefix,
                        !in->call_is_extern &&
                                strncmp(in->src0.as.symbol, "ny_fn_", 6) != 0
                            ? "ny_fn_"
                            : "",
                        in->src0.as.symbol))
          return false;
        if (stack_bytes && !nyr_printf(w, "\taddq\t$%zu, %%rsp\n", stack_bytes))
          return false;
        if (nyr_x64_mach_is_f32(mach, &in->dst)) {
          if (!nyr_printf(w, "\tmovss\t%%xmm0, -%d(%%rbp)\n", dst))
            return false;
        } else if (nyr_x64_mach_is_f64(mach, &in->dst)) {
          if (!nyr_printf(w, "\tmovsd\t%%xmm0, -%d(%%rbp)\n", dst))
            return false;
        } else if (in->dst.kind != NYR_MACH_OPERAND_NONE &&
                   !nyr_printf(w, "\tmovq\t%%rax, -%d(%%rbp)\n", dst))
          return false;
      }
        rax_cached_off = in->dst.kind == NYR_MACH_OPERAND_VREG &&
                                 !nyr_x64_mach_is_float(mach, &in->dst)
                             ? dst
                             : -1;
        break;
      case NYR_MACH_RET:
        if (nyr_x64_mach_is_float(mach, &in->src0)) {
          if (nyr_x64_mach_is_f32(mach, &in->src0)) {
            if (!nyr_printf(
                    w, "\tmovss\t-%d(%%rbp), %%xmm0\n\tmovd\t%%xmm0, %%eax\n",
                    a))
              return false;
          } else if (!nyr_printf(
                         w,
                         "\tmovsd\t-%d(%%rbp), %%xmm0\n\tmovq\t%%xmm0, %%rax\n",
                         a))
            return false;
        } else if (in->src0.kind != NYR_MACH_OPERAND_NONE &&
                   !ny_x64_scalar_emit_rax_load(w, &in->src0, a,
                                                rax_cached_off))
          return false;
        if (!nyr_printf(w, "\tjmp\t%s\n", epilogue))
          return false;
        rax_cached_off = -1;
        break;
      default:
        return false;
      }
    }
  }
  if (!nyr_printf(w, "%s:\n", epilogue) ||
      (tag_return && !nyr_put(w, "\tleaq\t1(,%rax,2), %rax\n")) ||
      !nyr_put(w, "\tleave\n\tret\n"))
    return false;
  if (strcmp(target->object_format, "macho") != 0 &&
      !nyr_printf(w, "\t.size\t%s%s, .-%s%s\n", sym, name, sym, name))
    return false;
  (void)err;
  (void)err_len;
  return true;
}
