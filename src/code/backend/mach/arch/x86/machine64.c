/*
 * x86-64 native encoder: NYIR and machine-form lowering to object-code.
 * Source AST fallback emission lives in the language pipeline.
 */
#include "base/debug.h"
#include "base/trace.h"
#include "code/backend/core/abi.h"
#include "code/backend/core/internal.h"
#include "code/backend/passes/util.h"
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

#define NY_X64_NIR_MAX_SLOTS 32768
#define NY_X64_LOOP_REG_COUNT 5
#define NY_X64_VEC_CACHE_REGS 4

/*
 * Callee-saved GPR cache (file-static, single-threaded emission):
 * Tracks which stack slot each callee-saved register holds, or -1 if stale.
 * Reset at each block boundary; updated whenever a callee-saved GPR is
 * written to.  Callee-saved registers survive calls per the SysV ABI, so
 * cache entries remain valid across CALL instructions.
 */
static int x64_gpr_cached_off[6] = {-1, -1, -1, -1, -1, -1};
static const char *const x64_gpr_names[6] = {"%rbx", "%r12", "%r13",
                                             "%r14", "%r15", "%r10"};

enum {
  NY_X64_ASM_GET_USER = -24,
  NY_X64_ASM_VMCALL_SLOTS = -140,
  NY_X64_ASM_VMWARE_PORT = -141,
  NY_X64_ASM_KVM_VMMCALL = -142,
  NY_X64_ASM_CMPXCHG16B_TRY = -160
};

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
  bool stack_guard;
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
  bool vec_cache_i64v2[NY_X64_VEC_CACHE_REGS];
  uint64_t vec_cache_age[NY_X64_VEC_CACHE_REGS];
  uint64_t vec_cache_clock;
  size_t remat_scanned_until;
  size_t remat_last_clobber;
  int frame_slots;
  int frame_bytes;
  int max_local_slot;
  int alloca_offset;
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
  int single_user_inst[NY_X64_NIR_MAX_SLOTS];
  bool phi_used[NY_X64_NIR_MAX_SLOTS];
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
  bool retpoline_indirect_calls;
  bool retpoline_inline_calls;
  bool function_return_thunk_extern;
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
    c->slot_offset[s + lane] = (s + 4 - lane + c->stack_guard) * 8;
  return s;
}

static const char *ny_x64_nir_link_symbol(const char *symbol) {
  static const char prefix[] = "__nyc_extern_";
  return symbol && strncmp(symbol, prefix, sizeof(prefix) - 1) == 0
             ? symbol + sizeof(prefix) - 1
             : symbol;
}

static bool ny_x64_nir_local_label_id(const char *symbol, uint64_t *id) {
  static const char prefix[] = "__nyc_local_label_";
  if (!symbol || strncmp(symbol, prefix, sizeof(prefix) - 1) != 0)
    return false;
  const char *digit = symbol + sizeof(prefix) - 1;
  if (!*digit)
    return false;
  uint64_t value = 0;
  for (; *digit; ++digit) {
    if (*digit < '0' || *digit > '9' ||
        value > (UINT64_MAX - (uint64_t)(*digit - '0')) / 10)
      return false;
    value = value * 10 + (uint64_t)(*digit - '0');
  }
  *id = value;
  return true;
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

static bool ny_x64_nir_root_less(size_t lhs, size_t rhs, const size_t *first) {
  return first[lhs] < first[rhs] || (first[lhs] == first[rhs] && lhs < rhs);
}

static void ny_x64_nir_root_sift(size_t *order, size_t start, size_t count,
                                 const size_t *first) {
  size_t root = start;
  for (;;) {
    size_t child = root * 2 + 1;
    if (child >= count)
      break;
    if (child + 1 < count &&
        ny_x64_nir_root_less(order[child], order[child + 1], first))
      ++child;
    if (!ny_x64_nir_root_less(order[root], order[child], first))
      break;
    size_t tmp = order[root];
    order[root] = order[child];
    order[child] = tmp;
    root = child;
  }
}

static void ny_x64_nir_sort_roots(size_t *order, size_t count,
                                  const size_t *first) {
  for (size_t start = count / 2; start > 0;)
    ny_x64_nir_root_sift(order, --start, count, first);
  for (size_t end = count; end > 1;) {
    --end;
    size_t tmp = order[0];
    order[0] = order[end];
    order[end] = tmp;
    ny_x64_nir_root_sift(order, 0, end, first);
  }
}

static void ny_x64_nir_compute_frame(ny_x64_nir_ctx_t *c) {
  int max_val = c->nyir->next_value;
  int max_local = c->nyir->param_count > 0 ? (int)c->nyir->param_count - 1 : -1;
  for (int v = 0; v < max_val && v < NY_X64_NIR_MAX_SLOTS; ++v) {
    c->def_index[v] = -1;
    c->def_count[v] = 0;
    c->uniform_const_state[v] = 0;
    c->uniform_const_value[v] = 0;
    c->use_count[v] = 0;
    c->single_user_inst[v] = -1;
    c->phi_used[v] = false;
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
    c->stack_guard |=
        in->op == NYIR_STACK_GUARD_LOAD || in->op == NYIR_STACK_GUARD_STORE;
    bool i64_vector_result =
        in->op == NYIR_VEC4_LOAD_I64 || in->op == NYIR_VEC8_LOAD_I64 ||
        in->op == NYIR_VEC4_SET1_I64 || in->op == NYIR_VEC8_SET1_I64 ||
        in->op == NYIR_VEC4_ADD_I64 || in->op == NYIR_VEC8_ADD_I64 ||
        in->op == NYIR_VEC4_SUB_I64 || in->op == NYIR_VEC8_SUB_I64 ||
        in->op == NYIR_VEC4_AND_I64 || in->op == NYIR_VEC8_AND_I64 ||
        in->op == NYIR_VEC4_OR_I64 || in->op == NYIR_VEC8_OR_I64 ||
        in->op == NYIR_VEC4_XOR_I64 || in->op == NYIR_VEC8_XOR_I64 ||
        in->op == NYIR_VEC4_SHL_I64 || in->op == NYIR_VEC4_SAR_I64 ||
        in->op == NYIR_VEC8_SAR_I64 || in->op == NYIR_VEC8_SHR_I64;
    if (i64_vector_result && in->dst >= 0 && in->dst < NY_X64_NIR_MAX_SLOTS)
      c->value_i64v2[in->dst] = true;
    if ((in->op == NYIR_VEC4_REDUCE_ADD_I64 ||
         in->op == NYIR_VEC8_REDUCE_ADD_I64) &&
        in->b >= 0 && in->b < NY_X64_NIR_MAX_SLOTS)
      c->value_i64v2[in->b] = true;
    if ((in->op == NYIR_LOAD_LOCAL || in->op == NYIR_STORE_LOCAL) &&
        (int)in->imm > max_local)
      max_local = (int)in->imm;
    if (in->op == NYIR_ADDR_LOCAL && (int)in->imm > max_local)
      max_local = (int)in->imm;
    if (in->op == NYIR_INLINE_ASM && in->imm > 0 && in->imm - 1 > max_local)
      max_local = (int)(in->imm - 1);
    if (in->op == NYIR_INLINE_ASM && in->imm == NY_X64_ASM_GET_USER) {
      int return_slot = -1;
      int value_slot = -1;
      if (in->symbol && sscanf(in->symbol, "call %*[^;];nyc_slots=%d,%d",
                               &return_slot, &value_slot) == 2) {
        if (return_slot > max_local)
          max_local = return_slot;
        if (value_slot > max_local)
          max_local = value_slot;
      }
    }
    if (in->op == NYIR_INLINE_ASM && (in->imm == NY_X64_ASM_VMCALL_SLOTS ||
                                      in->imm == NY_X64_ASM_VMWARE_PORT)) {
      int slots[6] = {-1, -1, -1, -1, -1, -1};
      int parsed =
          in->imm == NY_X64_ASM_VMWARE_PORT
              ? sscanf(in->symbol ? in->symbol : "",
                       "vmware_port;nyc_slots=%d,%d,%d,%d,%d,%d;port=%*u",
                       &slots[0], &slots[1], &slots[2], &slots[3], &slots[4],
                       &slots[5])
              : sscanf(in->symbol ? in->symbol : "",
                       "%*[^;];nyc_slots=%d,%d,%d,%d,%d,%d", &slots[0],
                       &slots[1], &slots[2], &slots[3], &slots[4], &slots[5]);
      if (parsed == 6)
        for (size_t s = 0; s < 6; ++s)
          if (slots[s] > max_local)
            max_local = slots[s];
    }
    if (in->op == NYIR_INLINE_ASM && in->imm == NY_X64_ASM_KVM_VMMCALL) {
      int slot = -1;
      if (in->symbol && sscanf(in->symbol, "kvm_vmmcall;slot=%d", &slot) == 1 &&
          slot > max_local)
        max_local = slot;
    }
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
      if (args[j] >= 0 && args[j] < NY_X64_NIR_MAX_SLOTS) {
        c->single_user_inst[args[j]] = (int)i;
        c->use_count[args[j]]++;
      }
    }
    if (in->op == NYIR_CALL && in->extra_args && in->extra_args_len > 0) {
      for (size_t j = 0; j < in->extra_args_len; ++j) {
        int a = in->extra_args[j];
        if (a >= 0 && a < NY_X64_NIR_MAX_SLOTS) {
          c->single_user_inst[a] = (int)i;
          c->use_count[a]++;
        }
      }
    }
    for (size_t j = 0; j < in->phi_incoming_len; ++j) {
      int value = in->phi_incoming[j].value;
      if (value >= 0 && value < NY_X64_NIR_MAX_SLOTS)
        c->phi_used[value] = true;
    }
  }
  for (size_t i = 1; i < c->nyir->len; ++i) {
    const nyr_inst_t *shl = &c->nyir->data[i - 1];
    const nyr_inst_t *sar = &c->nyir->data[i];
    if (shl->op != NYIR_SHL_I64 || sar->op != NYIR_SAR_I64 || shl->dst < 0 ||
        shl->dst >= NY_X64_NIR_MAX_SLOTS || sar->a != shl->dst ||
        c->def_count[shl->dst] != 1 || c->use_count[shl->dst] != 1)
      continue;
    int64_t left = 0, right = 0;
    if (!ny_x64_nir_try_const_i32(c, shl->b, i - 1, &left) ||
        !ny_x64_nir_try_const_i32(c, sar->b, i, &right) || left != right ||
        (left != 32 && left != 48 && left != 56))
      continue;
    if (!c->phi_used[shl->dst])
      c->elided_sign_shift[shl->dst] = true;
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
    if (!c->phi_used[bias]) {
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
    if (!c->phi_used[cond])
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
    if (!c->phi_used[cond])
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
    if (c->phi_used[masked])
      continue;
    c->elided_mask_test[masked] = true;
    c->fused_mask_test_src[select->a] = source;
  }
  /*
   * A SELECT whose condition is directly `x & 1` needs no compare at all.
   * Ruby-style `if x.odd?` lowering and canonicalization can hand the mask
   * value straight to the select; reuse the same TEST+CMOVNE form as the
   * compare-fed case above.
   */
  for (size_t i = 0; i < c->nyir->len; ++i) {
    const nyr_inst_t *select = &c->nyir->data[i];
    if (select->op != NYIR_SELECT_I64 || select->a < 0 ||
        select->a >= NY_X64_NIR_MAX_SLOTS || c->fused_select_cmp[select->a])
      continue;
    int cond = select->a;
    if (c->def_count[cond] != 1 || c->use_count[cond] != 1)
      continue;
    int and_idx = c->def_index[cond];
    if (and_idx < 0 || (size_t)and_idx >= i || (size_t)and_idx >= c->nyir->len)
      continue;
    const nyr_inst_t *and = &c->nyir->data[and_idx];
    if (and->op != NYIR_AND_I64 || and->dst != cond)
      continue;
    int source = -1;
    int64_t mask = 0;
    if (ny_x64_nir_try_const_i32(c, and->b, (size_t)and_idx, &mask) &&
        mask == 1)
      source = and->a;
    else if (ny_x64_nir_try_const_i32(c, and->a, (size_t)and_idx, &mask) &&
             mask == 1)
      source = and->b;
    if (source < 0 || source >= NY_X64_NIR_MAX_SLOTS || c->phi_used[cond])
      continue;
    c->elided_mask_test[cond] = true;
    c->fused_mask_test_src[cond] = source;
  }
  /*
   * Fold `(x & 1) ==/!= 0` into TEST even when the compare materializes a
   * boolean.  Mark the AND before emission so its stack home is not written;
   * CMP_I64 emits the TEST+SETcc form.
   */
  for (size_t i = 0; i < c->nyir->len; ++i) {
    const nyr_inst_t *cmp = &c->nyir->data[i];
    if (cmp->op != NYIR_CMP_I64 || cmp->dst < 0 ||
        cmp->dst >= NY_X64_NIR_MAX_SLOTS)
      continue;
    if (c->fused_cmp[cmp->dst] || c->fused_select_cmp[cmp->dst])
      continue;
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
    if (source < 0 || source >= NY_X64_NIR_MAX_SLOTS || c->phi_used[masked])
      continue;
    c->elided_mask_test[masked] = true;
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
  size_t scratch_len = max_val > 0 ? (size_t)max_val : 0;
  bool home_reuse_ready =
      scratch_len &&
      scratch_len <= SIZE_MAX / (10 * sizeof(size_t) + sizeof(bool));
  size_t *scratch =
      home_reuse_ready ? calloc(scratch_len, 10 * sizeof(size_t) + sizeof(bool))
                       : NULL;
  home_reuse_ready = scratch != NULL;
  size_t *home_first_def = scratch;
  size_t *home_last_def = scratch ? scratch + scratch_len : NULL;
  size_t *home_first_use = scratch ? scratch + 2 * scratch_len : NULL;
  size_t *home_last_use = scratch ? scratch + 3 * scratch_len : NULL;
  size_t *home_block = scratch ? scratch + 4 * scratch_len : NULL;
  size_t *root_first = scratch ? scratch + 5 * scratch_len : NULL;
  size_t *root_last = scratch ? scratch + 6 * scratch_len : NULL;
  size_t *root_block = scratch ? scratch + 7 * scratch_len : NULL;
  size_t *root_order = scratch ? scratch + 8 * scratch_len : NULL;
  size_t *color_last = scratch ? scratch + 9 * scratch_len : NULL;
  bool *root_pinned = scratch ? (bool *)(scratch + 10 * scratch_len) : NULL;
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
      /*
       * Indexed memory fusion removes the materialized address's add/shift
       * chain from emission and reloads its base and index at the access.
       * Keep those source values live through the access as well; otherwise
       * stack-home coloring can reuse a source home for the store value
       * between address construction and the fused store.
       */
      if (in->a >= 0 && in->a < NY_X64_NIR_MAX_SLOTS &&
          c->indexed_base[in->a] >= 0 && c->indexed_index[in->a] >= 0) {
        const int indexed_args[] = {c->indexed_base[in->a],
                                    c->indexed_index[in->a]};
        for (size_t a = 0; a < sizeof(indexed_args) / sizeof(indexed_args[0]);
             ++a) {
          int value = indexed_args[a];
          if (value < 0 || value >= tracked_val_count)
            continue;
          size_t v = (size_t)value;
          if (i > home_last_use[v])
            home_last_use[v] = i;
          if (home_block[v] == SIZE_MAX)
            home_block[v] = current_block;
          else if (home_block[v] != current_block)
            home_block[v] = SIZE_MAX - 1;
        }
      }
      /*
       * Mask-test fusion hides the original source from the SELECT's IR
       * operands. Keep it live through that SELECT so stack-home coloring
       * cannot reuse its slot for either arm before emission reloads it.
       */
      if (in->op == NYIR_SELECT_I64 && in->a >= 0 &&
          in->a < NY_X64_NIR_MAX_SLOTS && c->fused_mask_test_src[in->a] >= 0) {
        int value = c->fused_mask_test_src[in->a];
        if (value < tracked_val_count) {
          size_t v = (size_t)value;
          if (i > home_last_use[v])
            home_last_use[v] = i;
          if (home_block[v] == SIZE_MAX)
            home_block[v] = current_block;
          else if (home_block[v] != current_block)
            home_block[v] = SIZE_MAX - 1;
        }
      }
      /*
       * Compare fusion moves a CMP immediately before its consumer. The
       * comparison operands are then consumed there, even though
       * their ordinary IR use sites are on the preceding CMP. Keep both
       * operands live until the delayed comparison is emitted.
       */
      if (in->a >= 0 && in->a < NY_X64_NIR_MAX_SLOTS &&
          ((in->op == NYIR_SELECT_I64 && c->fused_select_cmp[in->a]) ||
           (in->op == NYIR_BR_IF && c->fused_cmp[in->a]))) {
        int di = c->def_index[in->a];
        if (di >= 0 && (size_t)di < c->nyir->len) {
          const nyr_inst_t *cmp = &c->nyir->data[di];
          const int compare_args[] = {cmp->a, cmp->b};
          for (size_t a = 0; a < sizeof(compare_args) / sizeof(compare_args[0]);
               ++a) {
            int value = compare_args[a];
            if (value < 0 || value >= tracked_val_count)
              continue;
            size_t v = (size_t)value;
            if (i > home_last_use[v])
              home_last_use[v] = i;
            if (home_block[v] == SIZE_MAX)
              home_block[v] = current_block;
            else if (home_block[v] != current_block)
              home_block[v] = SIZE_MAX - 1;
          }
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
      root_order[root_count++] = (size_t)root;
    }
    ny_x64_nir_sort_roots(root_order, root_count, root_first);
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
  free(scratch);
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
    if (op == NYIR_CALL || op == NYIR_INLINE_ASM || op == NYIR_ALLOCA ||
        op == NYIR_VA_SAVE || op == NYIR_STACK_GUARD_LOAD ||
        op == NYIR_STACK_GUARD_STORE) {
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
        op == NYIR_VEC8_SET1_I64 || op == NYIR_VEC8_ADD_I64 ||
        op == NYIR_VEC8_SUB_I64 || op == NYIR_VEC8_AND_I64 ||
        op == NYIR_VEC8_OR_I64 || op == NYIR_VEC8_XOR_I64 ||
        op == NYIR_VEC8_SAR_I64 || op == NYIR_VEC8_SHR_I64 ||
        op == NYIR_VEC8_REDUCE_ADD_I64 || op == NYIR_VEC8_LOAD_F32 ||
        op == NYIR_VEC8_STORE_F32 || op == NYIR_VEC8_ADD_F32 ||
        op == NYIR_VEC8_SUB_F32 || op == NYIR_VEC8_MUL_F32 ||
        op == NYIR_VEC8_DIV_F32 || op == NYIR_VEC8_FMA_F32 ||
        op == NYIR_VEC8_SET1_F32) {
      c->is_leaf = false;
      break;
    }
  }
  c->max_local_slot = max_local + 1;
  c->frame_slots = c->max_local_slot;
  c->alloca_offset = 0;
  /*
   * Value homes are packed by the width selected during interval coloring.
   */
  for (int v = 0; v < max_val &&
                  c->frame_slots + c->value_home_count < NY_X64_NIR_MAX_SLOTS;
       ++v)
    ny_x64_nir_slot(c, v);
  /*
   * Compute the actual deepest slot offset to size the frame correctly.
   * Vector homes use (s + 4 - lane) * 8 addressing which can extend
   * beyond frame_slots + value_home_count.
   */
  int deepest = 0;
  for (int slot = 0; slot < NY_X64_NIR_MAX_SLOTS; ++slot)
    if (c->slot_offset[slot] > deepest)
      deepest = c->slot_offset[slot];
  /*
   * Also account for local slots: deepest local is at frame_slots * 8.
   */
  if ((c->frame_slots + c->stack_guard) * 8 > deepest)
    deepest = (c->frame_slots + c->stack_guard) * 8;
  /*
   * Sum up ALLOCA sizes for non-leaf functions. Each ALLOCA with a constant
   * size adds to the frame requirement. We align each to 16 bytes.
   */
  int alloca_bytes = 0;
  if (!c->is_leaf) {
    for (size_t i = 0; i < c->nyir->len; ++i) {
      const nyr_inst_t *in = &c->nyir->data[i];
      if (in->op == NYIR_ALLOCA && in->imm > 0) {
        int64_t sz = in->imm;
        sz = (sz + 15) & ~15;
        if (sz > INT_MAX - alloca_bytes)
          sz = INT_MAX - alloca_bytes;
        alloca_bytes += (int)sz;
      }
    }
  }
  int raw = deepest + alloca_bytes;
  /*
   * Keep the frame size 16-byte aligned for nested calls.
   */
  c->frame_bytes = (raw + 15) & ~15;
  c->alloca_offset = c->frame_bytes;
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
    snprintf(buf, len, "%d(%%rsp)",
             c->frame_bytes - (slot + 2 + c->stack_guard) * 8);
  } else {
    int off = slot < c->frame_slots ? (slot + 1 + c->stack_guard) * 8
                                    : c->slot_offset[slot];
    if (off <= 0 && getenv("NYTRIX_TRACE_BAD_SLOT"))
      fprintf(stderr, "bad NYR slot: %s slot=%d frame=%d home=%d offset=%d\n",
              c->label_prefix, slot, c->frame_slots,
              slot >= c->frame_slots &&
                      slot - c->frame_slots < NY_X64_NIR_MAX_SLOTS
                  ? c->value_home_index[slot - c->frame_slots]
                  : -1,
              off);
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
    if (in->op == NYIR_INLINE_ASM && (in->imm == NY_X64_ASM_VMCALL_SLOTS ||
                                      in->imm == NY_X64_ASM_VMWARE_PORT ||
                                      in->imm == NY_X64_ASM_KVM_VMMCALL)) {
      /*
       * Fixed-register hypercalls write C locals outside normal stores.
       */
      for (int slot = 0; slot < c->max_local_slot; ++slot)
        address_taken[slot] = true;
    }
    if (in->op == NYIR_INLINE_ASM && in->imm > 0 &&
        in->imm - 1 < c->max_local_slot)
      address_taken[in->imm - 1] = true;
    if (in->op == NYIR_INLINE_ASM && in->imm == NY_X64_ASM_GET_USER) {
      int return_slot = -1;
      int value_slot = -1;
      if (in->symbol && sscanf(in->symbol, "call %*[^;];nyc_slots=%d,%d",
                               &return_slot, &value_slot) == 2) {
        if (return_slot >= 0 && return_slot < c->max_local_slot)
          address_taken[return_slot] = true;
        if (value_slot >= 0 && value_slot < c->max_local_slot)
          address_taken[value_slot] = true;
      }
    }
  }

  size_t len = c->nyir->len;
  int64_t *delta = len < SIZE_MAX / sizeof(*delta) - 1
                       ? calloc(len + 1, sizeof(*delta))
                       : NULL;
  if (delta) {
    for (size_t end = 0; end < len; ++end) {
      const nyr_inst_t *branch = &c->nyir->data[end];
      if ((branch->op != NYIR_BR && branch->op != NYIR_BR_IF) ||
          branch->imm < 0 || branch->imm >= NY_X64_NIR_MAX_SLOTS)
        continue;
      int start = labels[branch->imm];
      if (start < 0 || (size_t)start >= end)
        continue;
      ++delta[(size_t)start + 1];
      --delta[end];
    }
    int64_t active = 0;
    for (size_t i = 0; i < len; ++i) {
      active += delta[i];
      const nyr_inst_t *in = &c->nyir->data[i];
      if (active <= 0 ||
          (in->op != NYIR_LOAD_LOCAL && in->op != NYIR_STORE_LOCAL) ||
          in->imm < 0 || in->imm >= c->max_local_slot ||
          address_taken[in->imm] || c->local_f64[in->imm] ||
          c->local_f32[in->imm])
        continue;
      uint64_t sum = (uint64_t)score[in->imm] + (uint64_t)active;
      score[in->imm] = sum > UINT32_MAX ? UINT32_MAX : (uint32_t)sum;
    }
    free(delta);
  } else {
    for (size_t end = 0; end < len; ++end) {
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
  }
  /*
   * Prevent callee-saved register allocation for locals live across CALLs.
   * A local is live across a call if it is defined before the call and used
   * after the call. We conservatively mark all locals accessed in a loop
   * containing a CALL as address_taken (no register home).
   */
  bool *live_at_call = calloc((size_t)c->max_local_slot, sizeof(bool));
  if (live_at_call) {
    for (size_t i = 0; i < len; ++i) {
      const nyr_inst_t *in = &c->nyir->data[i];
      if (in->op == NYIR_CALL) {
        /*
         * Backward scan: find locals defined before this call
         */
        for (size_t j = 0; j < i; ++j) {
          const nyr_inst_t *def = &c->nyir->data[j];
          if ((def->op == NYIR_STORE_LOCAL || def->op == NYIR_LOAD_LOCAL) &&
              def->imm >= 0 && def->imm < c->max_local_slot)
            live_at_call[def->imm] = true;
        }
        /*
         * Forward scan: find locals used after this call
         */
        for (size_t j = i + 1; j < len; ++j) {
          const nyr_inst_t *use = &c->nyir->data[j];
          if ((use->op == NYIR_STORE_LOCAL || use->op == NYIR_LOAD_LOCAL) &&
              use->imm >= 0 && use->imm < c->max_local_slot)
            live_at_call[use->imm] = true;
        }
      }
    }
    for (int slot = 0; slot < c->max_local_slot; ++slot) {
      if (live_at_call[slot])
        address_taken[slot] = true;
    }
    free(live_at_call);
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
 * A rematerialized immediate must not cross an instruction that consumes a
 * fixed register.  The value's stack home is the conservative handoff across
 * those boundaries.
 */
static bool ny_x64_nir_is_fixed_clobber(ny_x64_nir_ctx_t *c,
                                        const nyr_inst_t *in, size_t idx);
static bool ny_x64_nir_find_const_val(ny_x64_nir_ctx_t *c, int value_id,
                                      int64_t *out);

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
  if (use_idx >= c->remat_scanned_until) {
    for (size_t i = c->remat_scanned_until; i < use_idx; ++i)
      if (ny_x64_nir_is_fixed_clobber(c, &c->nyir->data[i], i))
        c->remat_last_clobber = i;
    c->remat_scanned_until = use_idx;
    if (c->remat_last_clobber != SIZE_MAX && c->remat_last_clobber > root)
      return false;
  } else {
    for (size_t i = root + 1; i < use_idx; ++i)
      if (ny_x64_nir_is_fixed_clobber(c, &c->nyir->data[i], i))
        return false;
  }
  *out = imm;
  return true;
}

static bool ny_x64_nir_is_fixed_clobber(ny_x64_nir_ctx_t *c,
                                        const nyr_inst_t *in, size_t idx) {
  (void)idx;
  switch (in->op) {
  case NYIR_CALL:
  case NYIR_DIV_I64:
  case NYIR_MOD_I64:
    return true;
  case NYIR_SHL_I64:
  case NYIR_SAR_I64: {
    int64_t count = 0;
    return !ny_x64_nir_find_const_val(c, in->b, &count) || count < 1 ||
           count > 63;
  }
  case NYIR_ROR32_I64: {
    int64_t count = 0;
    return !ny_x64_nir_find_const_val(c, in->b, &count) || count < 0 ||
           count >= 32;
  }
  default:
    return false;
  }
}

static bool ny_x64_nir_find_const_val(ny_x64_nir_ctx_t *c, int value_id,
                                      int64_t *out) {
  if (!c || !c->nyir || value_id < 0 || value_id >= NY_X64_NIR_MAX_SLOTS ||
      value_id >= (int)c->nyir->next_value)
    return false;
  int current = value_id;
  for (size_t hops = 0; hops < 32; ++hops) {
    if (current < 0 || current >= NY_X64_NIR_MAX_SLOTS ||
        current >= (int)c->nyir->next_value)
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
    if (def->op == NYIR_LOAD_LOCAL && def->imm >= 0) {
      /*
       * C parameter canonicalization can store a narrowed value back to its
       * entry local before later uses.  Recover that value only from an
       * unambiguous store in the same basic block; do not carry facts across
       * control-flow joins or an address escape.
       */
      for (int at = di - 1; at >= 0; --at) {
        const nyr_inst_t *prior = &c->nyir->data[at];
        if (prior->op == NYIR_LABEL || prior->op == NYIR_BR ||
            prior->op == NYIR_BR_IF || prior->op == NYIR_RET)
          break;
        if (prior->op == NYIR_ADDR_LOCAL && prior->imm == def->imm)
          return false;
        if (prior->op == NYIR_STORE_LOCAL && prior->imm == def->imm) {
          if (prior->a < 0 || !ny_x64_nir_find_const_val(c, prior->a, out))
            return false;
          return true;
        }
      }
    }
    if (def->op == NYIR_AND_I64 && def->a >= 0 && def->b >= 0) {
      int64_t lhs = 0;
      int64_t rhs = 0;
      if (!ny_x64_nir_find_const_val(c, def->a, &lhs) ||
          !ny_x64_nir_find_const_val(c, def->b, &rhs))
        return false;
      if (out)
        *out = lhs & rhs;
      return true;
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
    int single_user = c->single_user_inst[current];
    if (single_user < 0 || (size_t)single_user <= current_def ||
        (size_t)single_user >= c->nyir->len)
      return false;
    size_t use_idx = (size_t)single_user;
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
  if (c->slot_offset[slot] <= 0) {
    nyr_set_error(c->err, c->err_len,
                  "nyir x86-64: value v%d has no allocated spill home", slot);
    return false;
  }
  if (!nyr_printf(c->w, "\tmovq\t-%d(%%rbp), %%rax\n", c->slot_offset[slot]))
    return false;
  c->rax_cached_slot = slot;
  return true;
}

static bool ny_x64_nir_store(ny_x64_nir_ctx_t *c, int slot) {
  if (slot < 0 || slot >= NY_X64_NIR_MAX_SLOTS)
    return false;
  /*
   * Redundant store elimination: if rax already holds this slot's value and
   * was already stored here, skip the duplicate store. Require both cached
   * and store slot to match so we never skip a store after rax was clobbered.
   */
  if (c->rax_cached_slot == slot && c->rax_store_slot == slot)
    return true;
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
  if (c->slot_offset[slot] <= 0) {
    nyr_set_error(c->err, c->err_len,
                  "nyir x86-64: value v%d has no allocated spill home", slot);
    return false;
  }
  if (!nyr_printf(c->w, "\tmovq\t%%rax, -%d(%%rbp)\n", c->slot_offset[slot]))
    return false;
  c->rax_cached_slot = slot;
  c->rax_store_slot = slot;
  return true;
}

static bool ny_x64_nir_emit_i64_reg(ny_x64_nir_ctx_t *c, int64_t imm,
                                    const char *reg) {
  const char *reg32 = strcmp(reg, "%rax") == 0   ? "%eax"
                      : strcmp(reg, "%rsi") == 0 ? "%esi"
                      : strcmp(reg, "%rdi") == 0 ? "%edi"
                      : strcmp(reg, "%rdx") == 0 ? "%edx"
                      : strcmp(reg, "%r10") == 0 ? "%r10d"
                      : strcmp(reg, "%r11") == 0 ? "%r11d"
                                                 : NULL;
  if (imm >= 0 && (uint64_t)imm <= UINT32_MAX && reg32)
    return nyr_printf(c->w, "\tmovl\t$%" PRIu32 ", %s\n", (uint32_t)imm, reg32);
  if (imm >= INT32_MIN && imm <= INT32_MAX)
    return nyr_printf(c->w, "\tmovq\t$%" PRId64 ", %s\n", imm, reg);
  return nyr_printf(c->w, "\tmovabsq\t$%" PRId64 ", %s\n", imm, reg);
}

static bool ny_x64_nir_materialize_i64(ny_x64_nir_ctx_t *c, int64_t imm) {
  c->rax_cached_slot = -1;
  return ny_x64_nir_emit_i64_reg(c, imm, "%rax");
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

static bool nyr_x64_emit_linux_syscall(ny_x64_nir_ctx_t *c,
                                       const nyr_inst_t *in, const int *args,
                                       int argc) {
  static const char *const syscall_arg_regs[] = {"%rdi", "%rsi", "%rdx",
                                                 "%r10", "%r8",  "%r9"};
  if (!c || !in || !args || argc < 1 || argc > 7 ||
      !c->target->syscalls.available)
    return false;
  for (size_t i = 0; i < sizeof(syscall_arg_regs) / sizeof(syscall_arg_regs[0]);
       ++i) {
    if (i + 1 < (size_t)argc) {
      if (!ny_x64_nir_load_value(c, args[i + 1]) ||
          !nyr_printf(c->w, "\tmovq\t%%rax, %s\n", syscall_arg_regs[i]))
        return false;
    } else if (!nyr_printf(c->w, "\txorq\t%s, %s\n", syscall_arg_regs[i],
                           syscall_arg_regs[i])) {
      return false;
    }
  }
  if (!ny_x64_nir_load_value(c, args[0]) || !nyr_put(c->w, "\tsyscall\n"))
    return false;
  c->rax_cached_slot = -1;
  return in->dst < 0 || ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
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

/*
 * VEC4 i64 values contain two lanes. Their homes are four scalar slots wide
 * in the shared allocator; the pair beginning at slot is contiguous.
 */
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
  const char *move = c->vec_cache_i64v2[reg] ? "vmovdqu" : "vmovupd";
  const char *width = c->vec_cache_i64v2[reg] ? "xmm" : "ymm";
  if (off <= 0 || !(c->is_leaf ? nyr_printf(c->w, "\t%s\t%%%s%d, %d(%%rsp)\n",
                                            move, width, reg, off)
                               : nyr_printf(c->w, "\t%s\t%%%s%d, -%d(%%rbp)\n",
                                            move, width, reg, off)))
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

static void ny_x64_nir_vec_cache_set(ny_x64_nir_ctx_t *c, int reg, int value,
                                     bool i64v2) {
  if (!c || reg < 0 || reg >= NY_X64_VEC_CACHE_REGS)
    return;
  c->vec_cached_value[reg] = value;
  c->vec_cache_dirty[reg] = false;
  c->vec_cache_i64v2[reg] = i64v2;
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
  ny_x64_nir_vec_cache_set(c, reg, value, false);
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
  c->vec_cache_i64v2[reg] = false;
  c->vec_cache_age[reg] = ++c->vec_cache_clock;
  return true;
}

static bool ny_x64_nir_vec_i64v2_load(ny_x64_nir_ctx_t *c, int value,
                                      int avoid_reg, int *out_reg) {
  if (!c || !out_reg || value < 0)
    return false;
  int reg = ny_x64_nir_vec_cached_reg(c, value);
  if (reg >= 0) {
    if (!c->vec_cache_i64v2[reg])
      return false;
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
            ? nyr_printf(c->w, "\tvmovdqu\t%d(%%rsp), %%xmm%d\n", off, reg)
            : nyr_printf(c->w, "\tvmovdqu\t-%d(%%rbp), %%xmm%d\n", off, reg)))
    return false;
  ny_x64_nir_vec_cache_set(c, reg, value, true);
  *out_reg = reg;
  return true;
}

static bool ny_x64_nir_vec_i64v2_store(ny_x64_nir_ctx_t *c, int value,
                                       int reg) {
  if (!c || value < 0 || reg < 0 || reg >= NY_X64_VEC_CACHE_REGS)
    return false;
  int slot = ny_x64_nir_slot(c, value);
  if (slot < 0)
    return false;
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
  c->vec_cache_i64v2[reg] = true;
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
    ny_x64_nir_vec_cache_set(c, a_reg, in->dst, false);
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

static bool ny_x64_nir_emit_vec4_i64v2_binop(ny_x64_nir_ctx_t *c,
                                             const nyr_inst_t *in,
                                             const char *mnemonic) {
  if (!c || !in || !mnemonic)
    return false;

  /*
   * Fast path: Utilize AVX2 256-bit ymm registers directly if available
   */
  if (c->target && (c->target->caps & NY_NATIVE_CAP_MACH_AVX)) {
    int bslot = ny_x64_nir_slot(c, in->b);
    int dslot = ny_x64_nir_slot(c, in->dst);
    int bo = bslot < 0 ? 0
                       : (c->is_leaf ? c->frame_bytes - (bslot + 2) * 8
                                     : c->slot_offset[bslot]);
    if (bslot < 0 || dslot < 0 || bo <= 0)
      return false;

    int a_reg = -1;
    if (!ny_x64_nir_vec_i64v2_load(c, in->a, -1, &a_reg))
      return false;

    /*
     * Zero-spill register caching: keep working set in ymm/xmm without stack
     * roundtrips
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
    if (!ny_x64_nir_vec_i64v2_store(c, in->dst, a_reg))
      return false;
    if (b_reg >= 0 && b_reg != a_reg)
      ny_x64_nir_vec_release_single_use(c, in->b);
    return true;
  }

  /*
   * Fallback for non-AVX targets
   */
  int aslot = ny_x64_nir_slot(c, in->a);
  int bslot = ny_x64_nir_slot(c, in->b);
  int dslot = ny_x64_nir_slot(c, in->dst);
  return ny_x64_nir_load_i64v2(c, aslot, 0) &&
         ny_x64_nir_load_i64v2(c, bslot, 1) &&
         nyr_printf(c->w, "\t%s\t%%xmm1, %%xmm0\n", mnemonic) &&
         ny_x64_nir_store_i64v2(c, dslot, 0);
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

typedef struct {
  uint32_t to, next;
} ny_x64_nir_type_edge_t;

static bool ny_x64_nir_type_known(const ny_x64_nir_ctx_t *c, size_t span,
                                  uint32_t node) {
  size_t kind = node / span, id = node % span;
  switch (kind) {
  case 0:
    return c->value_f64[id];
  case 1:
    return c->value_f32[id];
  case 2:
    return c->value_i64v2[id];
  case 3:
    return c->local_f64[id];
  default:
    return c->local_f32[id];
  }
}

static void ny_x64_nir_type_mark(ny_x64_nir_ctx_t *c, size_t span,
                                 uint32_t node) {
  size_t kind = node / span, id = node % span;
  switch (kind) {
  case 0:
    c->value_f64[id] = true;
    break;
  case 1:
    c->value_f32[id] = true;
    break;
  case 2:
    c->value_i64v2[id] = true;
    break;
  case 3:
    c->local_f64[id] = true;
    break;
  default:
    c->local_f32[id] = true;
    break;
  }
}

static void ny_x64_nir_type_edge(uint32_t *head, ny_x64_nir_type_edge_t *edges,
                                 size_t *count, uint32_t from, uint32_t to) {
  size_t i = (*count)++;
  edges[i].to = to;
  edges[i].next = head[from];
  head[from] = (uint32_t)i;
}

static bool ny_x64_nir_type_worklist(ny_x64_nir_ctx_t *c) {
  const nyr_func_t *f = c->nyir;
  if (f->len < 128)
    return false;
  size_t span = f->next_value > 0 ? (size_t)f->next_value : 0;
  if (span < f->param_count)
    span = f->param_count;
  if (span > NY_X64_NIR_MAX_SLOTS)
    span = NY_X64_NIR_MAX_SLOTS;
  size_t capacity = 0;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    const int ids[] = {in->dst, in->a, in->b};
    for (size_t j = 0; j < 3; ++j)
      if (ids[j] >= 0 && ids[j] < NY_X64_NIR_MAX_SLOTS &&
          span <= (size_t)ids[j])
        span = (size_t)ids[j] + 1;
    if ((in->op == NYIR_LOAD_LOCAL || in->op == NYIR_STORE_LOCAL) &&
        in->imm >= 0 && in->imm < NY_X64_NIR_MAX_SLOTS &&
        span <= (size_t)in->imm)
      span = (size_t)in->imm + 1;
    if (in->op == NYIR_PHI)
      for (size_t j = 0; j < in->phi_incoming_len; ++j) {
        int id = in->phi_incoming[j].value;
        if (id >= 0 && id < NY_X64_NIR_MAX_SLOTS && span <= (size_t)id)
          span = (size_t)id + 1;
      }
    if (capacity > SIZE_MAX - 4 ||
        in->phi_incoming_len > SIZE_MAX - capacity - 4)
      return false;
    capacity += in->phi_incoming_len + 4;
  }
  if (!span || capacity > UINT32_MAX)
    return false;
  size_t nodes = span * 5;
  uint32_t *head = malloc(nodes * sizeof(*head));
  uint32_t *queue = malloc(nodes * sizeof(*queue));
  ny_x64_nir_type_edge_t *edges = malloc(capacity * sizeof(*edges));
  if (!head || !queue || !edges) {
    free(head);
    free(queue);
    free(edges);
    return false;
  }
  for (size_t i = 0; i < nodes; ++i)
    head[i] = UINT32_MAX;
  size_t edge_count = 0;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    int dst = in->dst, a = in->a;
    bool valid_dst = dst >= 0 && (size_t)dst < span;
    bool valid_a = a >= 0 && (size_t)a < span;
    bool valid_local = in->imm >= 0 && (size_t)in->imm < span;
    uint32_t local = valid_local ? (uint32_t)in->imm : 0;
    switch (in->op) {
    case NYIR_COPY:
      if (valid_dst && valid_a)
        for (uint32_t k = 0; k < 3; ++k)
          ny_x64_nir_type_edge(head, edges, &edge_count,
                               (uint32_t)(k * span + (size_t)a),
                               (uint32_t)(k * span + (size_t)dst));
      break;
    case NYIR_PHI:
      if (valid_dst)
        for (size_t j = 0; j < in->phi_incoming_len; ++j) {
          int value = in->phi_incoming[j].value;
          if (value >= 0 && (size_t)value < span)
            ny_x64_nir_type_edge(head, edges, &edge_count,
                                 (uint32_t)(2 * span + (size_t)value),
                                 (uint32_t)(2 * span + (size_t)dst));
        }
      break;
    case NYIR_LOAD_LOCAL:
      if (valid_dst && valid_local)
        for (uint32_t k = 0; k < 2; ++k) {
          uint32_t v = (uint32_t)(k * span + (size_t)dst);
          uint32_t l = (uint32_t)((k + 3) * span + local);
          ny_x64_nir_type_edge(head, edges, &edge_count, l, v);
          ny_x64_nir_type_edge(head, edges, &edge_count, v, l);
        }
      break;
    case NYIR_STORE_LOCAL:
      if (valid_a && valid_local)
        for (uint32_t k = 0; k < 2; ++k)
          ny_x64_nir_type_edge(head, edges, &edge_count,
                               (uint32_t)(k * span + (size_t)a),
                               (uint32_t)((k + 3) * span + local));
      break;
    default:
      break;
    }
    bool f64_op = in->op == NYIR_ADD_F64 || in->op == NYIR_SUB_F64 ||
                  in->op == NYIR_MUL_F64 || in->op == NYIR_DIV_F64 ||
                  in->op == NYIR_SQRT_F64 || in->op == NYIR_SIN_F64 ||
                  in->op == NYIR_COS_F64 || in->op == NYIR_CMP_F64;
    bool f32_op = in->op == NYIR_ADD_F32 || in->op == NYIR_SUB_F32 ||
                  in->op == NYIR_MUL_F32 || in->op == NYIR_DIV_F32 ||
                  in->op == NYIR_CMP_F32;
    if (f64_op || f32_op) {
      size_t off = f32_op ? span : 0;
      if (valid_a)
        ny_x64_nir_type_mark(c, span, (uint32_t)(off + (size_t)a));
      if (in->b >= 0 && (size_t)in->b < span)
        ny_x64_nir_type_mark(c, span, (uint32_t)(off + (size_t)in->b));
    }
  }
  size_t read = 0, write = 0;
  for (size_t node = 0; node < nodes; ++node)
    if (ny_x64_nir_type_known(c, span, (uint32_t)node))
      queue[write++] = (uint32_t)node;
  while (read < write) {
    uint32_t src = queue[read++];
    for (uint32_t ei = head[src]; ei != UINT32_MAX; ei = edges[ei].next) {
      uint32_t dst = edges[ei].to;
      if (ny_x64_nir_type_known(c, span, dst))
        continue;
      ny_x64_nir_type_mark(c, span, dst);
      queue[write++] = dst;
    }
  }
  free(edges);
  free(queue);
  free(head);
  return true;
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
        (nyr_op_is_f32(in->op) || in->scalar_class == NYIR_SCALAR_F32))
      c->value_f32[in->dst] = true;
    if ((in->op == NYIR_LOAD_LOCAL || in->op == NYIR_STORE_LOCAL) &&
        in->imm >= 0 && in->imm < NY_X64_NIR_MAX_SLOTS) {
      if (in->scalar_class == NYIR_SCALAR_F64)
        c->local_f64[in->imm] = true;
      else if (in->scalar_class == NYIR_SCALAR_F32)
        c->local_f32[in->imm] = true;
    }
  }
  if (ny_x64_nir_type_worklist(c))
    return;
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
  case NYIR_READ_X86_RSP:
    if (in->dst < 0)
      return true;
    return nyr_put(c->w, "\tmovq\t%rsp, %rax\n") &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
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
    return ny_x64_nir_materialize_i64(c, in->imm) &&
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
    c->slot_offset[in->imm] = (in->imm + 1 + c->stack_guard) * 8;
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
  case NYIR_STACK_GUARD_LOAD:
    c->rax_cached_slot = -1;
    return nyr_put(c->w, "\tmovq\t-8(%rbp), %rax\n") &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  case NYIR_STACK_GUARD_STORE:
    return ny_x64_nir_load_value(c, in->a) &&
           nyr_put(c->w, "\tmovq\t%rax, -8(%rbp)\n");
  case NYIR_ADDR_LOCAL: {
    if (in->dst < 0)
      return true;
    if (in->imm < 0 || (int)in->imm >= c->max_local_slot) {
      nyr_set_error(c->err, c->err_len,
                    "nyir x86-64: addr.local invalid slot %" PRId64, in->imm);
      return false;
    }
    c->slot_offset[in->imm] = (in->imm + 1 + c->stack_guard) * 8;
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
    {
      uint64_t label_id = 0;
      if (ny_x64_nir_local_label_id(in->symbol, &label_id))
        return nyr_printf(c->w,
                          "\tleaq\t.Lnyir_%s_L%" PRIu64 "(%%rip), %%rax\n",
                          c->label_prefix, label_id) &&
               ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
      return nyr_printf(c->w, "\tleaq\t%s(%%rip), %%rax\n",
                        ny_x64_nir_link_symbol(in->symbol)) &&
             ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    }
  case NYIR_ALLOCA:
    if (in->dst < 0)
      return true;
    if (c->is_leaf) {
      return nyr_printf(c->w,
                        "\tsubq\t$%" PRId64
                        ", %%rsp\n\tandq\t$-16, %%rsp\n\tmovq\t%%rsp, %%rax\n",
                        in->imm) &&
             ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    } else {
      int64_t sz = in->imm;
      sz = (sz + 15) & ~15;
      int off = c->alloca_offset;
      c->alloca_offset -= (int)sz;
      return nyr_printf(c->w, "\tleaq\t-%d(%%rbp), %%rax\n", off) &&
             ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    }
  case NYIR_VA_SAVE: {
    static const char *const gp_regs[] = {"%rdi", "%rsi", "%rdx",
                                          "%rcx", "%r8",  "%r9"};
    if (in->dst < 0 || in->imm != 176 || !nyr_put(c->w, "\tsubq\t$176, %rsp\n"))
      return false;
    for (size_t i = 0; i < sizeof(gp_regs) / sizeof(gp_regs[0]); ++i) {
      if (!nyr_printf(c->w, "\tmovq\t%s, %zu(%%rsp)\n", gp_regs[i], i * 8))
        return false;
    }
    for (size_t i = 0; i < 8; ++i) {
      if (!nyr_printf(c->w, "\tmovdqu\t%%xmm%zu, %zu(%%rsp)\n", i, 48 + i * 16))
        return false;
    }
    return nyr_put(c->w, "\tmovq\t%rsp, %rax\n") &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  }
  case NYIR_VA_START: {
    uint64_t imm = (uint64_t)in->imm;
    unsigned gp_offset = (unsigned)(imm & 0xffffu);
    unsigned fp_offset = (unsigned)((imm >> 16) & 0xffffu);
    uint32_t stack_bytes = (uint32_t)(imm >> 32);
    if (c->target->abi != NY_NATIVE_ABI_SYSV || in->dst >= 0 || in->a < 0 ||
        in->b < 0 || in->c != -1 || in->imm < 0 || gp_offset > 48 ||
        fp_offset < 48 || fp_offset > 176 || stack_bytes > INT_MAX - 16 ||
        !ny_x64_nir_load_value(c, in->a) ||
        !nyr_printf(c->w,
                    "\tmovq\t%%rax, %%r10\n"
                    "\tmovl\t$%u, (%%r10)\n"
                    "\tmovl\t$%u, 4(%%r10)\n"
                    "\tleaq\t%u(%%rbp), %%rax\n"
                    "\tmovq\t%%rax, 8(%%r10)\n",
                    gp_offset, fp_offset,
                    16u + (unsigned)c->home_reg_count * 8u + stack_bytes) ||
        !ny_x64_nir_load_value(c, in->b) ||
        !nyr_put(c->w, "\tmovq\t%rax, 16(%r10)\n"))
      return false;
    c->rax_cached_slot = -1;
    return true;
  }
  case NYIR_VA_ARG: {
    if (c->target->abi != NY_NATIVE_ABI_SYSV || in->dst < 0 || in->a < 0 ||
        (in->imm != 0 && in->imm != 1) || !ny_x64_nir_load_value(c, in->a) ||
        !nyr_put(c->w, "\tmovq\t%rax, %r10\n"))
      return false;
    bool is_float = in->imm == 1;
    const char *offset_field = is_float ? "4" : "0";
    unsigned max_offset = is_float ? 160u : 40u;
    unsigned step = is_float ? 16u : 8u;
    if (!nyr_printf(c->w,
                    "\tmovl\t%s(%%r10), %%ecx\n"
                    "\tcmpl\t$%u, %%ecx\n"
                    "\tja\t.Lny_va_stack_%s_%zu\n"
                    "\tmovq\t16(%%r10), %%r11\n"
                    "\taddq\t%%rcx, %%r11\n",
                    offset_field, max_offset, c->label_prefix, idx))
      return false;
    if (is_float) {
      if (!nyr_put(c->w, "\tmovsd\t(%r11), %xmm0\n"))
        return false;
    } else if (!nyr_put(c->w, "\tmovq\t(%r11), %rax\n")) {
      return false;
    }
    if (!nyr_printf(c->w,
                    "\taddl\t$%u, %s(%%r10)\n"
                    "\tjmp\t.Lny_va_done_%s_%zu\n"
                    ".Lny_va_stack_%s_%zu:\n"
                    "\tmovq\t8(%%r10), %%r11\n",
                    step, offset_field, c->label_prefix, idx, c->label_prefix,
                    idx))
      return false;
    if (is_float) {
      if (!nyr_put(c->w, "\tmovsd\t(%r11), %xmm0\n"))
        return false;
    } else if (!nyr_put(c->w, "\tmovq\t(%r11), %rax\n")) {
      return false;
    }
    if (!nyr_printf(c->w,
                    "\taddq\t$8, 8(%%r10)\n"
                    ".Lny_va_done_%s_%zu:\n",
                    c->label_prefix, idx))
      return false;
    bool stored = is_float
                      ? ny_x64_nir_store_xmm(c, ny_x64_nir_slot(c, in->dst), 0)
                      : ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    c->rax_cached_slot = -1;
    return stored;
  }
  case NYIR_COPY_STRUCT:
    if (in->imm == 0)
      return true;
    if (in->imm == -2)
      return ny_x64_nir_load_value(c, in->c) &&
             nyr_put(c->w, "\tmovq\t%rax, %rcx\n") &&
             ny_x64_nir_load_value(c, in->a) &&
             nyr_put(c->w, "\tmovq\t%rax, %rdi\n") &&
             ny_x64_nir_load_value(c, in->b) && nyr_put(c->w, "\trep stosb\n");
    if (!ny_x64_nir_load_value(c, in->b) ||
        !nyr_put(c->w, "\tmovq\t%rax, %rsi\n") ||
        !ny_x64_nir_load_value(c, in->a) ||
        !nyr_put(c->w, "\tmovq\t%rax, %rdi\n"))
      return false;
    if (in->imm == -1)
      return ny_x64_nir_load_value(c, in->c) &&
             nyr_put(c->w, "\tmovq\t%rax, %rcx\n\trep movsb\n");
    return nyr_printf(c->w, "\tmovq\t$%" PRId64 ", %%rcx\n\trep movsb\n",
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
      ny_x64_nir_vec_cache_set(c, reg, in->dst, false);
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
        ny_x64_nir_vec_cache_set(c, reg, in->dst, false);
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
      ny_x64_nir_vec_cache_set(c, reg, in->dst, false);
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
  case NYIR_VEC8_SET1_I64: {
    int dst = ny_x64_nir_slot(c, in->dst);
    if (dst < 0 || !ny_x64_nir_load_value(c, in->a))
      return false;
    for (unsigned lane = 0; lane < 4; ++lane)
      if (!ny_x64_nir_store(c, dst + (int)lane))
        return false;
    return true;
  }
  case NYIR_VEC4_SET1_I64: {
    return ny_x64_nir_load_value(c, in->a) &&
           nyr_put(c->w, "\tmovq\t%rax, %xmm0\n"
                         "\tpunpcklqdq\t%xmm0, %xmm0\n") &&
           ny_x64_nir_store_i64v2(c, ny_x64_nir_slot(c, in->dst), 0);
  }
  case NYIR_VEC8_ADD_I64:
  case NYIR_VEC8_SUB_I64:
  case NYIR_VEC8_AND_I64:
  case NYIR_VEC8_OR_I64:
  case NYIR_VEC8_XOR_I64: {
    int a = ny_x64_nir_slot(c, in->a);
    int b = ny_x64_nir_slot(c, in->b);
    int dst = ny_x64_nir_slot(c, in->dst);
    const char *op = in->op == NYIR_VEC8_ADD_I64   ? "paddq"
                     : in->op == NYIR_VEC8_SUB_I64 ? "psubq"
                     : in->op == NYIR_VEC8_AND_I64 ? "pand"
                     : in->op == NYIR_VEC8_OR_I64  ? "por"
                                                   : "pxor";
    if (a < 0 || b < 0 || dst < 0)
      return false;
    for (int half = 0; half < 4; half += 2)
      if (!ny_x64_nir_load_i64v2(c, a + half, 0) ||
          !ny_x64_nir_load_i64v2(c, b + half, 1) ||
          !nyr_printf(c->w, "\t%s\t%%xmm1, %%xmm0\n", op) ||
          !ny_x64_nir_store_i64v2(c, dst + half, 0))
        return false;
    return true;
  }
  case NYIR_VEC4_ADD_I64: {
    return ny_x64_nir_emit_vec4_i64v2_binop(c, in, "paddq");
  }
  case NYIR_VEC4_SUB_I64: {
    return ny_x64_nir_emit_vec4_i64v2_binop(c, in, "psubq");
  }
  case NYIR_VEC4_AND_I64: {
    return ny_x64_nir_emit_vec4_i64v2_binop(c, in, "pand");
  }
  case NYIR_VEC4_OR_I64: {
    return ny_x64_nir_emit_vec4_i64v2_binop(c, in, "por");
  }
  case NYIR_VEC4_XOR_I64: {
    return ny_x64_nir_emit_vec4_i64v2_binop(c, in, "pxor");
  }
  case NYIR_VEC4_LOAD_I64: {
    bool avx = c->target && (c->target->caps & NY_NATIVE_CAP_MACH_AVX);
    if (in->a >= 0 && in->a < NY_X64_NIR_MAX_SLOTS &&
        c->indexed_base[in->a] >= 0 && c->indexed_index[in->a] >= 0) {
      if (!ny_x64_nir_load_value(c, c->indexed_base[in->a]) ||
          !nyr_put(c->w, "\tmovq\t%rax, %rcx\n") ||
          !ny_x64_nir_load_value(c, c->indexed_index[in->a]))
        return false;
      if (avx) {
        int reg = ny_x64_nir_vec_alloc_reg(c, -1);
        return reg >= 0 &&
               nyr_printf(c->w, "\tvmovdqu\t(%%rcx,%%rax,8), %%xmm%d\n", reg) &&
               ny_x64_nir_vec_i64v2_store(c, in->dst, reg);
      }
      return nyr_printf(c->w, "\tmovdqu\t(%%rcx,%%rax,8), %%xmm0\n") &&
             ny_x64_nir_store_i64v2(c, ny_x64_nir_slot(c, in->dst), 0);
    }
    if (!ny_x64_nir_load_value(c, in->a))
      return false;
    if (avx) {
      int reg = ny_x64_nir_vec_alloc_reg(c, -1);
      return reg >= 0 &&
             nyr_printf(c->w, "\tvmovdqu\t(%%rax), %%xmm%d\n", reg) &&
             ny_x64_nir_vec_i64v2_store(c, in->dst, reg);
    }
    return nyr_put(c->w, "\tmovdqu\t(%rax), %xmm0\n") &&
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
  case NYIR_VEC8_REDUCE_ADD_I64: {
    int vec = ny_x64_nir_slot(c, in->b);
    if (vec < 0 || vec + 3 >= NY_X64_NIR_MAX_SLOTS ||
        !ny_x64_nir_load_value(c, in->a) ||
        !nyr_put(c->w, "\tmovq\t%rax, %r11\n"))
      return false;
    for (int lane = 0; lane < 4; ++lane)
      if (c->slot_offset[vec + lane] <= 0 ||
          !nyr_printf(c->w, "\taddq\t-%d(%%rbp), %%r11\n",
                      c->slot_offset[vec + lane]))
        return false;
    return nyr_put(c->w, "\tmovq\t%r11, %rax\n") &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  }
  case NYIR_VEC8_SAR_I64:
  case NYIR_VEC8_SHR_I64: {
    /*
     * Lane-wise shift by the immediate count; each lane is its own 8-byte
     * slot here, so plain scalar sarq/shrq is exact.
     */
    int a = ny_x64_nir_slot(c, in->a);
    int dst = ny_x64_nir_slot(c, in->dst);
    if (a < 0 || dst < 0 || in->imm < 0 || in->imm > 63)
      return false;
    for (int lane = 0; lane < 4; ++lane)
      if (!ny_x64_nir_load(c, a + lane) ||
          !nyr_printf(c->w,
                      in->op == NYIR_VEC8_SHR_I64 ? "\tshrq\t$%d, %%rax\n"
                                                  : "\tsarq\t$%d, %%rax\n",
                      (int)in->imm) ||
          !ny_x64_nir_store(c, dst + lane))
        return false;
    return true;
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
    c->slot_offset[in->imm] = (in->imm + 1 + c->stack_guard) * 8;
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
    if (in->dst >= 0 && in->dst < NY_X64_NIR_MAX_SLOTS &&
        (c->value_f32[in->dst] || in->scalar_class == NYIR_SCALAR_F32) &&
        (in->flags & NYIR_INST_F_MEM_I32))
      return ny_x64_nir_load_value(c, in->a) &&
             nyr_put(c->w, "\tmovss\t(%rax), %xmm0\n") &&
             ny_x64_nir_store_xmm_f32(c, ny_x64_nir_slot(c, in->dst), 0);
    if (in->a >= 0 && in->a < NY_X64_NIR_MAX_SLOTS &&
        c->indexed_base[in->a] >= 0 && c->indexed_index[in->a] >= 0) {
      const char *load = (in->flags & NYIR_INST_F_MEM_BYTE)  ? "movzbl"
                         : (in->flags & NYIR_INST_F_MEM_U16) ? "movzwl"
                         : (in->flags & NYIR_INST_F_MEM_I32) ? "movl"
                                                             : "movq";
      const char *destination =
          (in->flags &
           (NYIR_INST_F_MEM_BYTE | NYIR_INST_F_MEM_U16 | NYIR_INST_F_MEM_I32))
              ? "%eax"
              : "%rax";
      return ny_x64_nir_load_value(c, c->indexed_base[in->a]) &&
             nyr_put(c->w, "\tmovq\t%rax, %rcx\n") &&
             ny_x64_nir_load_value(c, c->indexed_index[in->a]) &&
             nyr_printf(c->w, "\t%s\t(%%rcx,%%rax,8), %s\n", load,
                        destination) &&
             ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    }
    return ny_x64_nir_load_value(c, in->a) &&
           ((in->flags & NYIR_INST_F_MEM_BYTE)
                ? nyr_put(c->w, "\tmovzbl\t(%rax), %eax\n")
            : (in->flags & NYIR_INST_F_MEM_U16)
                ? nyr_put(c->w, "\tmovzwl\t(%rax), %eax\n")
            : (in->flags & NYIR_INST_F_MEM_I32)
                ? nyr_put(c->w, "\tmovl\t(%rax), %eax\n")
                : nyr_put(c->w, "\tmovq\t(%rax), %rax\n")) &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  case NYIR_STORE_I64: {
    if (in->c >= 0 && in->c < NY_X64_NIR_MAX_SLOTS &&
        (c->value_f32[in->c] || in->scalar_class == NYIR_SCALAR_F32) &&
        (in->flags & NYIR_INST_F_MEM_I32))
      return ny_x64_nir_load_xmm_f32(c, ny_x64_nir_slot(c, in->c), 0) &&
             ny_x64_nir_load_value(c, in->a) &&
             nyr_put(c->w, "\tmovss\t%xmm0, (%rax)\n");
    const char *st =
        (in->flags & NYIR_INST_F_MEM_BYTE)  ? "\tmovb\t%r10b, (%rax)\n"
        : (in->flags & NYIR_INST_F_MEM_U16) ? "\tmovw\t%r10w, (%rax)\n"
        : (in->flags & NYIR_INST_F_MEM_I32) ? "\tmovl\t%r10d, (%rax)\n"
                                            : "\tmovq\t%r10, (%rax)\n";
    if (in->a >= 0 && in->a < NY_X64_NIR_MAX_SLOTS &&
        c->indexed_base[in->a] >= 0 && c->indexed_index[in->a] >= 0) {
      const char *indexed_store =
          (in->flags & NYIR_INST_F_MEM_BYTE)  ? "\tmovb\t%r11b, (%rcx,%rax,8)\n"
          : (in->flags & NYIR_INST_F_MEM_U16) ? "\tmovw\t%r11w, (%rcx,%rax,8)\n"
          : (in->flags & NYIR_INST_F_MEM_I32) ? "\tmovl\t%r11d, (%rcx,%rax,8)\n"
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
    if (idx && in->a >= 0 && in->a < NY_X64_NIR_MAX_SLOTS &&
        c->elided_sign_shift[in->a]) {
      const nyr_inst_t *shl = &c->nyir->data[idx - 1];
      if (shl->op == NYIR_SHL_I64 && shl->dst == in->a) {
        int64_t width = 0;
        if (ny_x64_nir_try_const_i32(c, shl->b, idx - 1, &width)) {
          const char *ext = width == 32   ? "movslq"
                            : width == 48 ? "movswq"
                                          : "movsbq";
          const char *reg = width == 32 ? "%eax" : width == 48 ? "%ax" : "%al";
          return ny_x64_nir_load_value(c, shl->a) &&
                 nyr_printf(c->w, "\t%s\t%s, %%rax\n", ext, reg) &&
                 ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
        }
      }
    }
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
  case NYIR_F64_TO_I64:
    return ny_x64_nir_load_xmm(c, ny_x64_nir_slot(c, in->a), 0) &&
           nyr_put(c->w, "\tcvttsd2siq\t%xmm0, %rax\n") &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
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
    /*
     * (x & 1) ==/!= 0: emit TEST directly.  Keep the compare result form
     * (0/1 in rax) for cases where the boolean is materialized.
     */
    if ((in->cmp == NYIR_CMP_EQ || in->cmp == NYIR_CMP_NE) && in->dst >= 0 &&
        in->dst < NY_X64_NIR_MAX_SLOTS) {
      int masked = -1;
      int64_t zero = 1, mask = 0;
      if (ny_x64_nir_try_const_i32(c, in->b, idx, &zero) && zero == 0)
        masked = in->a;
      else if (ny_x64_nir_try_const_i32(c, in->a, idx, &zero) && zero == 0)
        masked = in->b;
      if (masked >= 0 && masked < NY_X64_NIR_MAX_SLOTS &&
          c->def_count[masked] == 1 && c->use_count[masked] == 1) {
        int and_idx = c->def_index[masked];
        if (and_idx >= 0 && (size_t)and_idx < idx) {
          const nyr_inst_t *and = &c->nyir->data[and_idx];
          int source = -1;
          if (and->op == NYIR_AND_I64 && and->dst == masked) {
            if (ny_x64_nir_try_const_i32(c, and->b, (size_t)and_idx, &mask) &&
                mask == 1)
              source = and->a;
            else if (ny_x64_nir_try_const_i32(c, and->a, (size_t)and_idx,
                                              &mask) &&
                     mask == 1)
              source = and->b;
          }
          if (source >= 0 && source < NY_X64_NIR_MAX_SLOTS &&
              !c->phi_used[masked]) {
            /*
             * If AND was elided by select fusion state, it has no home;
             * use the original source.  If not, still prefer the source
             * (AND is redundant work for a flag-only test).
             */
            const char *setcc = in->cmp == NYIR_CMP_EQ ? "sete" : "setne";
            c->elided_mask_test[masked] = true;
            return ny_x64_nir_load_value(c, source) &&
                   nyr_put(c->w, "\ttestq\t$1, %rax\n") &&
                   nyr_printf(c->w, "\t%s\t%%al\n\tmovzbq\t%%al, %%rax\n",
                              setcc) &&
                   ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
          }
        }
      }
    }
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
        c->fused_mask_test_src[in->a] >= 0) {
      int source = c->fused_mask_test_src[in->a];
      const char *cc = "cmovne";
      if (c->fused_select_cmp[in->a]) {
        int di = c->def_index[in->a];
        if (di < 0 || (size_t)di >= c->nyir->len)
          return false;
        cc = ny_x64_nir_cmov(c->nyir->data[di].cmp);
      }
      return ny_x64_nir_load_value(c, source) &&
             nyr_put(c->w, "\ttestq\t$1, %rax\n") &&
             ny_x64_nir_load_value(c, in->c) &&
             nyr_printf(c->w, "\t%s\t%s, %%rax\n", cc, true_addr) &&
             ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    }
    if (in->a >= 0 && in->a < NY_X64_NIR_MAX_SLOTS &&
        c->fused_select_cmp[in->a]) {
      int di = c->def_index[in->a];
      if (di < 0 || (size_t)di >= c->nyir->len)
        return false;
      const nyr_inst_t *cmp = &c->nyir->data[di];
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
    } else if (!ny_x64_nir_emit_i64_reg(c, in->imm, "%rsi")) {
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
  case NYIR_BR: {
    size_t next = idx + 1;
    while (next < c->nyir->len && c->nyir->data[next].op == NYIR_NOP)
      ++next;
    if (next < c->nyir->len && c->nyir->data[next].op == NYIR_LABEL &&
        c->nyir->data[next].imm == in->imm)
      return true;
    return nyr_printf(c->w, "\tjmp\t.Lnyir_%s_L%" PRId64 "\n", c->label_prefix,
                      in->imm);
  }
  case NYIR_BR_IF: {
    if (in->a >= 0 && in->a < NY_X64_NIR_MAX_SLOTS && c->fused_cmp[in->a]) {
      int di = c->def_index[in->a];
      if (di >= 0 && (size_t)di < c->nyir->len) {
        const nyr_inst_t *cmp = &c->nyir->data[di];
        if (cmp->op == NYIR_CMP_I64 && cmp->dst == in->a) {
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
    if (c->nyir->return_aggregate_size &&
        c->nyir->return_aggregate_classes[0] != NYIR_ARG_CLASS_MEMORY) {
      unsigned class1 = c->nyir->return_aggregate_classes[1];
      if (class1 == NYIR_ARG_CLASS_INTEGER) {
        if (in->b < 0 || !ny_x64_nir_load_value(c, in->b) ||
            !nyr_put(c->w, "\tmovq\t%rax, %rdx\n"))
          return false;
      } else if (class1 == NYIR_ARG_CLASS_SSE) {
        if (in->b < 0)
          return false;
        if (in->b < NY_X64_NIR_MAX_SLOTS && c->value_f32[in->b]) {
          if (!ny_x64_nir_load_xmm_f32(c, ny_x64_nir_slot(c, in->b), 1))
            return false;
        } else if (!ny_x64_nir_load_xmm(c, ny_x64_nir_slot(c, in->b), 1)) {
          return false;
        }
      }
    }
    return (!c->use_avx_vector || nyr_put(c->w, "\tvzeroupper\n")) &&
           nyr_printf(c->w, "\tjmp\t%s\n", c->epilogue_label);
  }
  case NYIR_CALL: {
    int argc = (int)in->imm;
    bool indirect = (in->flags & NYIR_INST_F_INDIRECT) != 0;
    int first_call_arg = indirect ? 1 : 0;
    const char *sym_name = in->symbol ? in->symbol : "<null>";
    if (in->flags & NYIR_INST_F_EXTERN)
      sym_name = ny_x64_nir_link_symbol(sym_name);
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
    for (int i = first_call_arg; i < argc; ++i) {
      if (arg_vals[i] < 0)
        return false;
    }
    if (ny_native_runtime_operation(in->symbol) ==
            NY_NATIVE_RUNTIME_OP_LINUX_SYSCALL &&
        c->target->syscalls.available)
      return nyr_x64_emit_linux_syscall(c, in, arg_vals, argc);
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
      if (i < first_call_arg)
        continue;
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
    for (int i = argc - 1; i >= first_call_arg; --i) {
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
    for (int i = first_call_arg; i < argc; ++i) {
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
    if (indirect) {
      if (!ny_x64_nir_load_value(c, arg_vals[0]))
        return false;
      if (c->retpoline_indirect_calls || c->retpoline_inline_calls) {
        if (!nyr_put(c->w, "\tmovq\t%rax, %r11\n"))
          return false;
        if (c->retpoline_inline_calls) {
          if (!nyr_printf(c->w,
                          "\tcall\t.Lnyir_%s_retpoline_dispatch_%zu\n"
                          ".Lnyir_%s_retpoline_return_%zu:\n"
                          "\tjmp\t.Lnyir_%s_retpoline_after_%zu\n"
                          ".Lnyir_%s_retpoline_dispatch_%zu:\n"
                          "\tcall\t.Lnyir_%s_retpoline_capture_%zu\n"
                          ".Lnyir_%s_retpoline_trap_%zu:\n"
                          "\tpause\n"
                          "\tlfence\n"
                          "\tjmp\t.Lnyir_%s_retpoline_trap_%zu\n"
                          ".Lnyir_%s_retpoline_capture_%zu:\n"
                          "\tmovq\t%%r11, (%%rsp)\n"
                          "\tret\n"
                          ".Lnyir_%s_retpoline_after_%zu:\n",
                          c->label_prefix, idx, c->label_prefix, idx,
                          c->label_prefix, idx, c->label_prefix, idx,
                          c->label_prefix, idx, c->label_prefix, idx,
                          c->label_prefix, idx, c->label_prefix, idx,
                          c->label_prefix, idx))
            return false;
        } else if (!nyr_put(c->w, "\tcall\t__x86_indirect_thunk_r11\n")) {
          return false;
        }
      } else if (!nyr_put(c->w, "\tcall\t*%rax\n")) {
        return false;
      }
    } else {
      char fn_label[256];
      if (in->flags & NYIR_INST_F_EXTERN) {
        snprintf(fn_label, sizeof(fn_label), "%s%s", c->target->symbol_prefix,
                 sym_name);
      } else if (strncmp(sym_name, "ny_fn_", 6) == 0) {
        snprintf(fn_label, sizeof(fn_label), "%s%s", c->target->symbol_prefix,
                 sym_name);
      } else {
        snprintf(fn_label, sizeof(fn_label), NY_FMT_FN,
                 c->target->symbol_prefix, sym_name);
      }
      if (!nyr_printf(c->w, "\tcall\t%s\n", fn_label))
        return false;
    }
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
  case NYIR_INLINE_ASM:
    if (in->imm == -108 || in->imm == -193) {
      const char *segment = in->symbol[0] == 'f' ? "fs" : "gs";
      bool byte = in->symbol[2] == '8';
      bool half = in->symbol[2] == '1';
      c->rax_cached_slot = -1;
      if (in->imm == -108) {
        if (!ny_x64_nir_load_value(c, in->a) ||
            !nyr_printf(c->w, "\t%s\t%%%s:(%%rax), %%eax\n",
                        byte   ? "movzbl"
                        : half ? "movzwl"
                               : "movl",
                        segment))
          return false;
        c->rax_cached_slot = -1;
        return ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
      }
      bool ok = ny_x64_nir_load_value(c, in->b) &&
                nyr_put(c->w, "\tmovl\t%eax, %edx\n") &&
                ny_x64_nir_load_value(c, in->a) &&
                nyr_printf(c->w, "\t%s\t%s, %%%s:(%%rax)\n",
                           byte   ? "movb"
                           : half ? "movw"
                                  : "movl",
                           byte   ? "%dl"
                           : half ? "%dx"
                                  : "%edx",
                           segment);
      c->rax_cached_slot = -1;
      return ok;
    }
    if (in->imm == -192)
      return ny_x64_nir_load_value(c, in->a) &&
             nyr_put(c->w, "\tmovq\t%rax, %r9\n");
    if (in->imm == -191) {
      if (!in->symbol || strcmp(in->symbol, "rep_movsq_byte_tail") != 0)
        return false;
      c->rax_cached_slot = -1;
      if (!ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\tmovq\t%rax, %rdi\n") ||
          !ny_x64_nir_load_value(c, in->b) ||
          !nyr_put(c->w, "\tmovq\t%rax, %rsi\n") ||
          !ny_x64_nir_load_value(c, in->c) ||
          !nyr_put(c->w, "\tmovq\t%rax, %rcx\n\trep movsq\n") ||
          !ny_x64_nir_load_value(c, in->d) ||
          !nyr_put(c->w, "\tmovq\t%rax, %rcx\n\trep movsb\n"))
        return false;
      c->rax_cached_slot = -1;
      bool ok = ny_x64_nir_load_value(c, in->e) &&
                nyr_put(c->w, "\tmovq\t%rdi, (%rax)\n") &&
                ny_x64_nir_load_value(c, in->f) &&
                nyr_put(c->w, "\tmovq\t%rsi, (%rax)\n");
      c->rax_cached_slot = -1;
      return ok;
    }
    if (in->imm == -190) {
      c->rax_cached_slot = -1;
      if (!ny_x64_nir_load_value(c, in->c) ||
          !nyr_put(c->w, "\tmovl\t%eax, %ecx\n") ||
          !ny_x64_nir_load_value(c, in->b) ||
          !nyr_put(c->w, "\tmovl\t%eax, %edx\n") ||
          !ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\tdivl\t%ecx\n\tmovl\t%eax, %r8d\n"
                         "\tmovl\t%edx, %r9d\n"))
        return false;
      c->rax_cached_slot = -1;
      bool ok = ny_x64_nir_load_value(c, in->d) &&
                nyr_put(c->w, "\tmovl\t%r8d, (%rax)\n") &&
                ny_x64_nir_load_value(c, in->e) &&
                nyr_put(c->w, "\tmovl\t%r9d, (%rax)\n");
      c->rax_cached_slot = -1;
      return ok;
    }
    if (in->imm == -189) {
      c->rax_cached_slot = -1;
      if (!ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\tmovq\t%rax, %rdi\n") ||
          !ny_x64_nir_load_value(c, in->b) ||
          !nyr_put(c->w, "\tmovq\t%rax, %rsi\n") ||
          !ny_x64_nir_load_value(c, in->c) ||
          !nyr_put(c->w, "\tmovq\t%rax, %rcx\n\ttestq\t%rcx, %rcx\n"
                         "\trepe cmpsb\n\tmovq\t%rdi, %r8\n"
                         "\tmovq\t%rsi, %r9\n\tmovq\t%rcx, %r10\n"
                         "\tsetne\t%r11b\n\tmovzbl\t%r11b, %r11d\n"))
        return false;
      c->rax_cached_slot = -1;
      const int addresses[] = {in->d, in->e, in->f};
      const char *registers[] = {"%r8", "%r9", "%r10"};
      for (size_t output = 0; output < 3; ++output)
        if (!ny_x64_nir_load_value(c, addresses[output]) ||
            !nyr_printf(c->w, "\tmovq\t%s, (%%rax)\n", registers[output]))
          return false;
      bool ok = nyr_put(c->w, "\tmovl\t%r11d, %eax\n") &&
                ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
      c->rax_cached_slot = -1;
      return ok;
    }
    if (in->imm == -187) {
      if (!ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\tmovl\t%eax, %ecx\n\trdmsr\n"
                         "\tmovl\t%eax, %r8d\n\tmovl\t%edx, %r9d\n"))
        return false;
      c->rax_cached_slot = -1;
      bool ok = ny_x64_nir_load_value(c, in->b) &&
                nyr_put(c->w, "\tmovl\t%r8d, (%rax)\n") &&
                ny_x64_nir_load_value(c, in->c) &&
                nyr_put(c->w, "\tmovl\t%r9d, (%rax)\n");
      c->rax_cached_slot = -1;
      return ok;
    }
    if (in->imm == -188) {
      bool ok = ny_x64_nir_load_value(c, in->a) &&
                nyr_put(c->w, "\tmovl\t%eax, %ecx\n") &&
                ny_x64_nir_load_value(c, in->b) &&
                nyr_put(c->w, "\tmovl\t%eax, %r8d\n") &&
                ny_x64_nir_load_value(c, in->c) &&
                nyr_put(c->w, "\tmovl\t%eax, %edx\n\tmovl\t%r8d, %eax\n"
                              "\twrmsr\n");
      c->rax_cached_slot = -1;
      return ok;
    }
    if (in->imm == -184 || in->imm == -185) {
      c->rax_cached_slot = -1;
      const char *segment = in->imm == -184 ? "fs" : "gs";
      if (in->symbol[0]) {
        if (!nyr_printf(c->w, "\tmovq\t%%%s:%s, %%rax\n", segment, in->symbol))
          return false;
      } else if (!ny_x64_nir_load_value(c, in->a) ||
                 !nyr_printf(c->w, "\tmovq\t%%%s:(%%rax), %%rax\n", segment))
        return false;
      return ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    }
    if (in->imm == -176) {
      if (in->dst < 0 || in->a < 0 || in->b < 0 ||
          !ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\tmovq\t%rax, %r10\n") ||
          !ny_x64_nir_load_value(c, in->b) ||
          !nyr_put(c->w, "\tmovl\t%eax, %r11d\n"
                         "\tmovl\t(%r10), %eax\n"
                         "\tsubl\t$4, %r11d\n"
                         "\tjbe\t2f\n"
                         "\taddl\t4(%r10), %eax\n"
                         "\tadcl\t8(%r10), %eax\n"
                         "\tadcl\t12(%r10), %eax\n"
                         "1:\tadcl\t16(%r10), %eax\n"
                         "\tleaq\t4(%r10), %r10\n"
                         "\tdecl\t%r11d\n"
                         "\tjne\t1b\n"
                         "\tadcl\t$0, %eax\n"
                         "\tmovl\t%eax, %r11d\n"
                         "\tshrl\t$16, %eax\n"
                         "\taddw\t%r11w, %ax\n"
                         "\tadcl\t$0, %eax\n"
                         "\tnotl\t%eax\n"
                         "2:\n"))
        return false;
      c->rax_cached_slot = -1;
      if (!ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst)))
        return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == NY_X64_ASM_CMPXCHG16B_TRY) {
      if (in->dst < 0 || in->a < 0 || in->b < 0 || in->c < 0 || in->d != -1 ||
          in->e != -1 || in->f != -1 || !in->symbol ||
          (strcmp(in->symbol, "cmpxchg16b_try") != 0 &&
           strcmp(in->symbol, "cmpxchg16b") != 0) ||
          !ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\tmovq\t%rax, %rsi\n") ||
          !ny_x64_nir_load_value(c, in->b) ||
          !nyr_put(c->w, "\tmovq\t%rax, %rdi\n") ||
          !ny_x64_nir_load_value(c, in->c) ||
          !nyr_put(c->w, "\tmovq\t%rax, %r8\n"
                         "\tpushq\t%rbx\n"
                         "\tmovq\t(%rdi), %rax\n"
                         "\tmovq\t8(%rdi), %rdx\n"
                         "\tmovq\t(%r8), %rbx\n"
                         "\tmovq\t8(%r8), %rcx\n") ||
          !nyr_put(c->w, strcmp(in->symbol, "cmpxchg16b_try") == 0
                             ? "\tlock cmpxchg16b\t(%rsi)\n"
                             : "\tcmpxchg16b\t(%rsi)\n") ||
          !nyr_put(c->w, "\tsetz\t%r9b\n"
                         "\tmovq\t%rax, (%rdi)\n"
                         "\tmovq\t%rdx, 8(%rdi)\n"
                         "\tmovzbl\t%r9b, %eax\n"
                         "\tpopq\t%rbx\n"))
        return false;
      c->rax_cached_slot = -1;
      return ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    }
    if (in->imm == -154) {
      if (in->dst != -1 || in->a < 0 || in->b < 0 || in->c != -1 ||
          in->d != -1 || in->e != -1 || in->f != -1 || !in->symbol ||
          strcmp(in->symbol, "invpcid") != 0 ||
          !ny_x64_nir_load_value(c, in->b) ||
          !nyr_put(c->w, "\tmovq\t%rax, %rdx\n") ||
          !ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\tinvpcid\t(%rax), %rdx\n"))
        return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -150) {
      if (in->dst < 0 || in->a != -1 || in->b != -1 || in->c != -1 ||
          in->d != -1 || in->e != -1 || in->f != -1 ||
          !nyr_put(c->w, "\tleaq\t(%rip), %rax\n") ||
          !ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst)))
        return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -151) {
      if (in->dst < 0 || in->a != -1 || in->b != -1 || in->c != -1 ||
          in->d != -1 || in->e != -1 || in->f != -1 ||
          !nyr_put(c->w, "\tmovq\t%rbp, %rax\n") ||
          !ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst)))
        return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -134) {
      if (in->dst != -1 || in->a < 0 || in->b != -1 || in->c != -1 ||
          in->d != -1 || in->e != -1 || in->f != -1 || !in->symbol ||
          (strcmp(in->symbol, "ss") != 0 && strcmp(in->symbol, "ds") != 0 &&
           strcmp(in->symbol, "es") != 0 && strcmp(in->symbol, "fs") != 0) ||
          !ny_x64_nir_load_value(c, in->a) ||
          !nyr_printf(c->w,
                      "1:\tmovl\t%%eax, %%%s\n"
                      "\t.pushsection __ex_table,\"aM\",@progbits,12\n"
                      "\t.balign 4\n\t.long 1b - .\n\t.long 1b - .\n"
                      "\t.long 17\n\t.popsection\n",
                      in->symbol))
        return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -132) {
      if (in->dst < 0 || in->a < 0 || in->b != -1 || in->c != -1 ||
          in->d != -1 || !in->symbol ||
          (strcmp(in->symbol, "rdrand") != 0 &&
           strcmp(in->symbol, "rdseed") != 0) ||
          !ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\tmovq\t%rax, %r11\n") ||
          !nyr_printf(c->w, "\t%s\t%%rax\n", in->symbol) ||
          !nyr_put(c->w, "\tsetc\t%dl\n"
                         "\tmovq\t%rax, (%r11)\n"
                         "\tmovzbl\t%dl, %eax\n") ||
          !ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst)))
        return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -131) {
      if (in->dst < 0 || in->a < 0 || in->b < 0 || in->c < 0 || in->d < 0 ||
          in->e != -1 || in->f != -1 || !in->symbol ||
          (strcmp(in->symbol, "xsave_user") != 0 &&
           strcmp(in->symbol, "xsaves_user") != 0 &&
           strcmp(in->symbol, "xrstor_user") != 0 &&
           strcmp(in->symbol, "xrstors_user") != 0)) {
        nyr_set_error(c->err, c->err_len,
                      "nyir x86-64: invalid user XSAVE operands");
        return false;
      }
      const char *mnemonic =
          strcmp(in->symbol, "xrstors_user") == 0  ? "xrstors64"
          : strcmp(in->symbol, "xsaves_user") == 0 ? "xsaves64"
          : strcmp(in->symbol, "xrstor_user") == 0 ? "xrstor64"
                                                   : "xsave64";
      c->rax_cached_slot = -1;
      if (!ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\tmovq\t%rax, %r10\n") ||
          !ny_x64_nir_load_value(c, in->b) ||
          !nyr_put(c->w, "\tmovl\t%eax, %r11d\n") ||
          !ny_x64_nir_load_value(c, in->c) ||
          !nyr_put(c->w, "\tmovl\t%eax, %edx\n"
                         "\tmovl\t%r11d, %eax\n") ||
          !nyr_printf(c->w, "1:\t%s\t(%%r10)\n", mnemonic) ||
          !nyr_put(c->w, "\txorl\t%eax, %eax\n"
                         "2:\n"
                         "\t.pushsection __ex_table, \"aM\", @progbits, 12\n"
                         "\t.balign 4\n"
                         "\t.long 1b - .\n"
                         "\t.long 2b - .\n"
                         "\t.long 15\n"
                         "\t.popsection\n"
                         "\tmovl\t%eax, %r11d\n") ||
          !ny_x64_nir_load_value(c, in->d) ||
          !nyr_put(c->w, "\tmovl\t%r11d, (%rax)\n\tmovl\t%r11d, %eax\n") ||
          !ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst)))
        return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -130) {
      int64_t width = 0;
      if (in->dst < 0 || in->a < 0 || in->b < 0 || in->c < 0 || in->d < 0 ||
          in->e != -1 || in->f != -1 || !in->symbol ||
          strcmp(in->symbol, "put_user_nocheck") != 0 ||
          !ny_x64_nir_find_const_val(c, in->d, &width) ||
          (width != 1 && width != 2 && width != 4 && width != 8)) {
        nyr_set_error(c->err, c->err_len,
                      "nyir x86-64: invalid put_user_nocheck operands");
        return false;
      }
      c->rax_cached_slot = -1;
      if (!ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\tmovq\t%rax, %rcx\n") ||
          !ny_x64_nir_load_value(c, in->b) ||
          !nyr_printf(c->w, "\tcall\t__put_user_nocheck_%" PRId64 "\n",
                      width) ||
          !nyr_put(c->w, "\tmovl\t%ecx, %r11d\n") ||
          !ny_x64_nir_load_value(c, in->c) ||
          !nyr_put(c->w, "\tmovl\t%r11d, (%rax)\n\tmovl\t%r11d, %eax\n") ||
          !ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst)))
        return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -129) {
      if (in->dst < 0 || in->a < 0 || in->b != -1 || in->c != -1 ||
          in->d != -1 || in->e != -1 || in->f != -1 || !in->symbol ||
          (strcmp(in->symbol, "fnsave_user") != 0 &&
           strcmp(in->symbol, "fxsave_user") != 0)) {
        nyr_set_error(c->err, c->err_len,
                      "nyir x86-64: invalid user fnsave operands");
        return false;
      }
      bool fnsave = strcmp(in->symbol, "fnsave_user") == 0;
      c->rax_cached_slot = -1;
      if (!ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\tmovq\t%rax, %r10\n"
                         "\txorl\t%eax, %eax\n"
                         "\tstac\n") ||
          !nyr_put(c->w, fnsave ? "1:\tfnsave\t(%r10)\n\tfwait\n"
                                : "1:\tfxsave64\t(%r10)\n") ||
          !nyr_put(c->w, "2:\tclac\n"
                         "\t.pushsection __ex_table, \"aM\", @progbits, 12\n"
                         "\t.balign 4\n"
                         "\t.long 1b - .\n"
                         "\t.long 2b - .\n"
                         "\t.long 15\n"
                         "\t.popsection\n") ||
          !ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst)))
        return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -127) {
      if (in->dst != -1 || in->a < 0 || in->b < 0 || in->c < 0 || in->d != -1 ||
          in->e != -1 || in->f != -1 || !in->symbol ||
          strcmp(in->symbol, "xrstor_alternative") != 0) {
        nyr_set_error(c->err, c->err_len,
                      "nyir x86-64: invalid XRSTOR operands");
        return false;
      }
      c->rax_cached_slot = -1;
      if (!ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\tmovq\t%rax, %r10\n") ||
          !ny_x64_nir_load_value(c, in->b) ||
          !nyr_put(c->w, "\tmovl\t%eax, %r11d\n") ||
          !ny_x64_nir_load_value(c, in->c) ||
          !nyr_put(c->w, "\tmovl\t%eax, %edx\n"
                         "\tmovl\t%r11d, %eax\n"
                         "1:\txrstor64\t(%r10)\n"
                         "2:\n"
                         "\t.pushsection __ex_table, \"aM\", @progbits, 12\n"
                         "\t.balign 4\n"
                         "\t.long 1b - .\n"
                         "\t.long 2b - .\n"
                         "\t.long 6\n"
                         "\t.popsection\n"))
        return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -126) {
      if (in->dst != -1 || in->a < 0 || in->b < 0 || in->c < 0 || in->d < 0 ||
          in->e != -1 || in->f != -1 || !in->symbol ||
          strcmp(in->symbol, "xsave_alternative") != 0) {
        nyr_set_error(c->err, c->err_len,
                      "nyir x86-64: invalid XSAVE operands");
        return false;
      }
      c->rax_cached_slot = -1;
      if (!ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\tmovq\t%rax, %r10\n") ||
          !ny_x64_nir_load_value(c, in->b) ||
          !nyr_put(c->w, "\tmovl\t%eax, %r11d\n") ||
          !ny_x64_nir_load_value(c, in->c) ||
          !nyr_put(c->w, "\tmovl\t%eax, %edx\n"
                         "\tmovl\t%r11d, %eax\n"
                         "1:\txsave64\t(%r10)\n"
                         "\txorl\t%r10d, %r10d\n"
                         "2:\n"
                         "\t.pushsection __ex_table, \"aM\", @progbits, 12\n"
                         "\t.balign 4\n"
                         "\t.long 1b - .\n"
                         "\t.long 2b - .\n"
                         "\t.long 0xfff20a11\n"
                         "\t.popsection\n") ||
          !ny_x64_nir_load_value(c, in->d) ||
          !nyr_put(c->w, "\tmovl\t%r10d, (%rax)\n"))
        return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -125) {
      if (in->dst != -1 || in->a < 0 || in->b < 0 || in->c < 0 || in->d != -1 ||
          in->e != -1 || in->f != -1 || !in->symbol ||
          strcmp(in->symbol, "xgetbv") != 0) {
        nyr_set_error(c->err, c->err_len,
                      "nyir x86-64: invalid xgetbv operands");
        return false;
      }
      c->rax_cached_slot = -1;
      if (!ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\tmovl\t%eax, %ecx\n\txgetbv\n"
                         "\tmovl\t%eax, %r10d\n"
                         "\tmovl\t%edx, %r11d\n") ||
          !ny_x64_nir_load_value(c, in->b) ||
          !nyr_put(c->w, "\tmovl\t%r10d, (%rax)\n") ||
          !ny_x64_nir_load_value(c, in->c) ||
          !nyr_put(c->w, "\tmovl\t%r11d, (%rax)\n"))
        return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -124) {
      if (in->dst < 0 || in->a < 0 || in->b != -1 || in->c != -1 ||
          in->d != -1 || in->e != -1 || in->f != -1 || !in->symbol ||
          (strcmp(in->symbol, "fxrstor_user") != 0 &&
           strcmp(in->symbol, "frstor_user") != 0)) {
        nyr_set_error(c->err, c->err_len,
                      "nyir x86-64: invalid user fxrstor operands");
        return false;
      }
      c->rax_cached_slot = -1;
      if (!ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\tmovq\t%rax, %r10\n\txorl\t%eax, %eax\n"
                         "\tstac\n") ||
          !nyr_printf(c->w,
                      strcmp(in->symbol, "frstor_user") == 0
                          ? "1:\tfrstor\t(%%r10)\n"
                            "2:\tclac\n"
                            "\t.pushsection __ex_table, \"aM\", @progbits, 12\n"
                            "\t.balign 4\n"
                            "\t.long 1b - .\n"
                            "\t.long 2b - .\n"
                            "\t.long 15\n"
                            "\t.popsection\n"
                          : "1:\tfxrstor64\t(%%r10)\n"
                            "2:\tclac\n"
                            "\t.pushsection __ex_table, \"aM\", @progbits, 12\n"
                            "\t.balign 4\n"
                            "\t.long 1b - .\n"
                            "\t.long 2b - .\n"
                            "\t.long 15\n"
                            "\t.popsection\n"))
        return false;
      c->rax_cached_slot = -1;
      return ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    }
    if (in->imm == -123) {
      if (in->dst < 0 || in->a < 0 || in->b != -1 || in->c != -1 ||
          in->d != -1 || in->e != -1 || in->f != -1 || !in->symbol ||
          (strcmp(in->symbol, "fxrstor_error") != 0 &&
           strcmp(in->symbol, "frstor_error") != 0)) {
        nyr_set_error(c->err, c->err_len,
                      "nyir x86-64: invalid safe fxrstor operands");
        return false;
      }
      c->rax_cached_slot = -1;
      if (!ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\tmovq\t%rax, %r10\n\txorl\t%eax, %eax\n") ||
          !nyr_printf(c->w,
                      strcmp(in->symbol, "frstor_error") == 0
                          ? "1:\tfrstor\t(%%r10)\n"
                            "2:\n"
                            "\t.pushsection __ex_table, \"aM\", @progbits, 12\n"
                            "\t.balign 4\n"
                            "\t.long 1b - .\n"
                            "\t.long 2b - .\n"
                            "\t.long 0xfff20011\n"
                            "\t.popsection\n"
                          : "1:\tfxrstor64\t(%%r10)\n"
                            "2:\n"
                            "\t.pushsection __ex_table, \"aM\", @progbits, 12\n"
                            "\t.balign 4\n"
                            "\t.long 1b - .\n"
                            "\t.long 2b - .\n"
                            "\t.long 0xfff20011\n"
                            "\t.popsection\n"))
        return false;
      c->rax_cached_slot = -1;
      return ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    }
    if (in->imm == -122) {
      if (in->dst != -1 || in->a < 0 || in->b != -1 || in->c != -1 ||
          in->d != -1 || in->e != -1 || in->f != -1 || !in->symbol ||
          (strcmp(in->symbol, "fxrstor_extable") != 0 &&
           strcmp(in->symbol, "frstor_extable") != 0)) {
        nyr_set_error(c->err, c->err_len,
                      "nyir x86-64: invalid fxrstor fixup operands");
        return false;
      }
      c->rax_cached_slot = -1;
      return ny_x64_nir_load_value(c, in->a) &&
             nyr_printf(
                 c->w,
                 strcmp(in->symbol, "frstor_extable") == 0
                     ? "1:\tfrstor\t(%%rax)\n"
                       "2:\n"
                       "\t.pushsection __ex_table, \"aM\", @progbits, 12\n"
                       "\t.balign 4\n"
                       "\t.long 1b - .\n"
                       "\t.long 2b - .\n"
                       "\t.long 6\n"
                       "\t.popsection\n"
                     : "1:\tfxrstor64\t(%%rax)\n"
                       "2:\n"
                       "\t.pushsection __ex_table, \"aM\", @progbits, 12\n"
                       "\t.balign 4\n"
                       "\t.long 1b - .\n"
                       "\t.long 2b - .\n"
                       "\t.long 6\n"
                       "\t.popsection\n");
    }
    if (in->imm == -121) {
      if (in->dst != -1 || in->a < 0 || in->b != -1 || in->c != -1 ||
          in->d != -1 || in->e != -1 || in->f != -1 || !in->symbol ||
          strcmp(in->symbol, "ldmxcsr") != 0) {
        nyr_set_error(c->err, c->err_len,
                      "nyir x86-64: invalid ldmxcsr operands");
        return false;
      }
      c->rax_cached_slot = -1;
      return ny_x64_nir_load_value(c, in->a) &&
             nyr_put(c->w, "\tldmxcsr\t(%rax)\n");
    }
    if (in->imm == -120) {
      if (in->dst != -1 || in->a < 0 || in->b != -1 || in->c != -1 ||
          in->d != -1 || in->e != -1 || in->f != -1 || !in->symbol ||
          strcmp(in->symbol, "fildl_clear") != 0) {
        nyr_set_error(c->err, c->err_len,
                      "nyir x86-64: invalid fildl clear operands");
        return false;
      }
      c->rax_cached_slot = -1;
      return ny_x64_nir_load_value(c, in->a) &&
             nyr_put(c->w, "\tfnclex\n\temms\n\tfildl\t(%rax)\n");
    }
    if (in->imm == -119) {
      if (in->dst != -1 || in->a < 0 || in->b < 0 || in->c < 0 || in->d != -1 ||
          in->e != -1 || in->f != -1 || !in->symbol ||
          strcmp(in->symbol, "fdiv_bug_probe") != 0) {
        nyr_set_error(c->err, c->err_len,
                      "nyir x86-64: invalid FDIV probe operands");
        return false;
      }
      c->rax_cached_slot = -1;
      return ny_x64_nir_load_value(c, in->a) &&
             nyr_put(c->w, "\tmovq\t%rax, %r10\n") &&
             ny_x64_nir_load_value(c, in->b) &&
             nyr_put(c->w, "\tmovq\t%rax, %r11\n") &&
             ny_x64_nir_load_value(c, in->c) &&
             nyr_put(c->w, "\tmovq\t%rax, %r12\n"
                           "\tfninit\n\tfldl\t(%r11)\n"
                           "\tfdivl\t(%r12)\n\tfmull\t(%r12)\n"
                           "\tfldl\t(%r11)\n\tfsubp\t%st, %st(1)\n"
                           "\tfistpl\t(%r10)\n\tfwait\n\tfninit\n");
    }
    if (in->imm == -118) {
      if (in->dst != -1 || in->a < 0 || in->b < 0 || in->c != -1 ||
          in->d != -1 || in->e != -1 || in->f != -1 || !in->symbol ||
          strcmp(in->symbol, "fninit_status") != 0) {
        nyr_set_error(c->err, c->err_len,
                      "nyir x86-64: invalid x87 status operands");
        return false;
      }
      c->rax_cached_slot = -1;
      return ny_x64_nir_load_value(c, in->a) &&
             nyr_put(c->w, "\tmovq\t%rax, %r10\n") &&
             ny_x64_nir_load_value(c, in->b) &&
             nyr_put(c->w, "\tmovq\t%rax, %r11\n"
                           "\tfninit\n\tfnstsw\t(%r10)\n"
                           "\tfnstcw\t(%r11)\n");
    }
    if (in->imm == -117) {
      if (in->dst != -1 || in->a < 0 || in->b != -1 || in->c != -1 ||
          in->d != -1 || in->e != -1 || in->f != -1 || !in->symbol ||
          (strcmp(in->symbol, "fxsave64") != 0 &&
           strcmp(in->symbol, "fxsave") != 0 &&
           strcmp(in->symbol, "fnsave") != 0)) {
        nyr_set_error(c->err, c->err_len,
                      "nyir x86-64: invalid fxsave operands");
        return false;
      }
      c->rax_cached_slot = -1;
      return ny_x64_nir_load_value(c, in->a) &&
             nyr_printf(c->w,
                        strcmp(in->symbol, "fnsave") == 0
                            ? "\tfnsave\t(%%rax)\n\tfwait\n"
                            : "\t%s\t(%%rax)\n",
                        in->symbol);
    }
    if (in->imm == -116) {
      if (in->dst != -1 || in->a < 0 || in->b < 0 || in->c < 0 || in->d != -1 ||
          in->e != -1 || in->f != -1 || !in->symbol ||
          (strcmp(in->symbol, "monitor") != 0 &&
           strcmp(in->symbol, "monitorx") != 0 &&
           strcmp(in->symbol, "mwaitx") != 0)) {
        nyr_set_error(c->err, c->err_len,
                      "nyir x86-64: invalid monitor operands");
        return false;
      }
      c->rax_cached_slot = -1;
      if (strcmp(in->symbol, "mwaitx") == 0) {
        if (!ny_x64_nir_load_value(c, in->a) ||
            !nyr_put(c->w, "\tmovl\t%eax, %r10d\n") ||
            !ny_x64_nir_load_value(c, in->b) ||
            !nyr_put(c->w, "\tmovl\t%eax, %r11d\n") ||
            !ny_x64_nir_load_value(c, in->c) ||
            !nyr_put(c->w, "\tmovl\t%eax, %edx\n"
                           "\tmovl\t%r10d, %eax\n"
                           "\tmovl\t%r11d, %ebx\n"
                           "\tmovl\t%edx, %ecx\n\tmwaitx\n"))
          return false;
        c->rax_cached_slot = -1;
        return true;
      }
      if (!ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\tmovl\t%eax, %r10d\n") ||
          !ny_x64_nir_load_value(c, in->b) ||
          !nyr_put(c->w, "\tmovl\t%eax, %r11d\n") ||
          !ny_x64_nir_load_value(c, in->c) ||
          !nyr_put(c->w, "\tmovl\t%eax, %edx\n"
                         "\tmovl\t%r10d, %eax\n"
                         "\tmovl\t%r11d, %ecx\n") ||
          !nyr_printf(c->w, "\t%s\n", in->symbol))
        return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -115) {
      if (in->dst != -1 || in->a < 0 || in->b != -1 || in->c != -1 ||
          in->d != -1 || in->e != -1 || in->f != -1 || !in->symbol ||
          strcmp(in->symbol, "clflush") != 0) {
        nyr_set_error(c->err, c->err_len,
                      "nyir x86-64: invalid clflush operands");
        return false;
      }
      c->rax_cached_slot = -1;
      return ny_x64_nir_load_value(c, in->a) &&
             nyr_put(c->w, "\tclflush\t(%rax)\n");
    }
    if (in->imm == -111) {
      const char *target = ny_x64_nir_link_symbol(in->symbol);
      if (in->dst != -1 || in->a < 0 || in->b < 0 || in->c < -1 ||
          in->d != -1 || in->e != -1 || in->f != -1 || !target) {
        nyr_set_error(c->err, c->err_len,
                      "nyir x86-64: invalid IRQ stack-switch operands");
        return false;
      }
      c->rax_cached_slot = -1;
      if (!nyr_put(c->w, in->c >= 0 ? "\tpushq\t%r12\n\tpushq\t%r13\n"
                                    : "\tpushq\t%r12\n") ||
          !ny_x64_nir_load_value(c, in->b) ||
          !nyr_put(c->w, "\tmovq\t%rax, %r12\n") ||
          (in->c >= 0 && (!ny_x64_nir_load_value(c, in->c) ||
                          !nyr_put(c->w, "\tmovq\t%rax, %r13\n"))) ||
          !ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\tmovq\t%rax, %r11\n"
                         "\tmovq\t%rsp, (%r11)\n"
                         "\tmovq\t%r11, %rsp\n"
                         "\tcall\tirq_enter_rcu\n") ||
          (in->c >= 0 && !nyr_put(c->w, "\tmovq\t%r13, %rsi\n")) ||
          !nyr_put(c->w, "\tmovq\t%r12, %rdi\n1:\tcall\t") ||
          !nyr_put(c->w, target) ||
          !nyr_put(c->w, "\n\t.pushsection .discard.annotate_insn, \"M\", "
                         "@progbits, 8; .long 1b - ., 8; .popsection\n"
                         "\tcall\tirq_exit_rcu\n"
                         "\tpopq\t%rsp\n") ||
          (in->c >= 0 && !nyr_put(c->w, "\tpopq\t%r13\n")) ||
          !nyr_put(c->w, "\tpopq\t%r12\n"))
        return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -153) {
      const char *target = ny_x64_nir_link_symbol(in->symbol);
      if (in->dst != -1 || in->a < 0 || in->b < -1 || in->c < -1 ||
          in->d < -1 || (in->c >= 0 && in->b < 0) ||
          (in->d >= 0 && in->c < 0) || in->e != -1 || in->f != -1 || !target) {
        nyr_set_error(c->err, c->err_len,
                      "nyir x86-64: invalid call_on_stack operands");
        return false;
      }
      c->rax_cached_slot = -1;
      if (!nyr_put(c->w, "\tpushq\t%r12\n") ||
          (in->c >= 0 && !nyr_put(c->w, "\tpushq\t%r13\n")) ||
          (in->d >= 0 && !nyr_put(c->w, "\tpushq\t%r14\n")) ||
          (in->b >= 0 && (!ny_x64_nir_load_value(c, in->b) ||
                          !nyr_put(c->w, "\tmovq\t%rax, %r12\n"))) ||
          (in->c >= 0 && (!ny_x64_nir_load_value(c, in->c) ||
                          !nyr_put(c->w, "\tmovq\t%rax, %r13\n"))) ||
          (in->d >= 0 && (!ny_x64_nir_load_value(c, in->d) ||
                          !nyr_put(c->w, "\tmovq\t%rax, %r14\n"))) ||
          !ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\tmovq\t%rax, %r11\n"
                         "\tmovq\t%rsp, (%r11)\n"
                         "\tmovq\t%r11, %rsp\n") ||
          (in->b >= 0 && !nyr_put(c->w, "\tmovq\t%r12, %rdi\n")) ||
          (in->c >= 0 && !nyr_put(c->w, "\tmovq\t%r13, %rsi\n")) ||
          (in->d >= 0 && !nyr_put(c->w, "\tmovq\t%r14, %rdx\n")) ||
          !nyr_put(c->w, "1:\tcall\t") || !nyr_put(c->w, target) ||
          !nyr_put(c->w, "\n\t.pushsection .discard.annotate_insn, \"M\", "
                         "@progbits, 8; .long 1b - ., 8; .popsection\n"
                         "\tpopq\t%rsp\n") ||
          (in->d >= 0 && !nyr_put(c->w, "\tpopq\t%r14\n")) ||
          (in->c >= 0 && !nyr_put(c->w, "\tpopq\t%r13\n")) ||
          !nyr_put(c->w, "\tpopq\t%r12\n"))
        return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -106) {
      if (in->dst < 0 || in->a < 0 || in->b < 0 || in->c < 0 || in->d != -1 ||
          in->e != -1 || in->f != -1 || !in->symbol ||
          strcmp(in->symbol, "divq_u64") != 0) {
        nyr_set_error(c->err, c->err_len,
                      "nyir x86-64: invalid unsigned 128-by-64 divide");
        return false;
      }
      c->rax_cached_slot = -1;
      if (!ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\tmovq\t%rax, %r10\n") ||
          !ny_x64_nir_load_value(c, in->b) ||
          !nyr_put(c->w, "\tmovq\t%rax, %rdx\n") ||
          !ny_x64_nir_load_value(c, in->c) ||
          !nyr_put(c->w, "\tmovq\t%rax, %rcx\n\tmovq\t%r10, %rax\n"
                         "\tdivq\t%rcx\n") ||
          !ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst)))
        return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -100) {
      int64_t feature = 0;
      if (in->dst != -1 || in->a < 0 || in->b < 0 || in->c < 0 || in->d < 0 ||
          in->e != -1 || in->f != -1 || !in->symbol ||
          strcmp(in->symbol, "alternative_wrmsr") != 0 ||
          !ny_x64_nir_find_const_val(c, in->d, &feature) || feature < 0 ||
          feature > UINT32_MAX) {
        nyr_set_error(c->err, c->err_len,
                      "nyir x86-64: invalid alternative wrmsr operands");
        return false;
      }
      c->rax_cached_slot = -1;
      if (!ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\tmovl\t%eax, %ecx\n") ||
          !ny_x64_nir_load_value(c, in->c) ||
          !nyr_put(c->w, "\tmovl\t%eax, %edx\n") ||
          !ny_x64_nir_load_value(c, in->b) ||
          !nyr_put(c->w, "771:\n\t.byte\t0x90, 0x90\n772:\n"
                         "773:\n"
                         "\t.pushsection .altinstructions, \"aM\", "
                         "@progbits, 14\n"
                         "\t.long\t771b - .\n"
                         "\t.long\t774f - .\n") ||
          !nyr_printf(c->w,
                      "\t.4byte\t%" PRId64 "\n"
                      "\t.byte\t773b-771b\n"
                      "\t.byte\t775f-774f\n"
                      "\t.popsection\n"
                      "\t.pushsection .altinstr_replacement, \"ax\"\n"
                      "912:\t.pushsection .discard.annotate_data, "
                      "\"M\", @progbits, 8\n"
                      "\t.long\t912b - ., 1\n\t.popsection\n"
                      "774:\n\twrmsr\n775:\n\t.popsection\n",
                      feature))
        return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -78) {
      if (in->dst != -1 || in->a < 0 || in->b != -1 || in->c != -1 ||
          !in->symbol || strcmp(in->symbol, "lkgs") != 0 ||
          !ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\tmovw\t%ax, %di\n"
                         "1:\t.byte\t0xf2,0x0f,0x00,0xf7\n"
                         "\t.pushsection __ex_table,\"aM\",@progbits,12\n"
                         "\t.balign 4\n"
                         "\t.long 1b - .\n"
                         "\t.long 1b - .\n"
                         "\t.long 1809\n"
                         "\t.popsection\n"))
        return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -32) {
      unsigned debug_reg = 8;
      if (in->dst < -1 || in->a < 0 || in->b != -1 || in->c != -1 ||
          in->d != -1 || in->e != -1 || in->f != -1 || !in->symbol) {
        nyr_set_error(c->err, c->err_len,
                      "nyir x86-64: invalid debug-register write");
        return false;
      }
      for (unsigned i = 0; i < 8; ++i) {
        char name[4] = {'d', 'b', (char)('0' + i), '\0'};
        if (strcmp(in->symbol, name) == 0) {
          debug_reg = i;
          break;
        }
      }
      if (debug_reg == 8 || !ny_x64_nir_load_value(c, in->a) ||
          !nyr_printf(c->w, "\t.byte\t0x0f, 0x23, 0x%02x\n",
                      0xc0u | (debug_reg << 3)))
        return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == NY_X64_ASM_KVM_VMMCALL) {
      int slot = -1;
      int consumed = -1;
      char address[64] = {0};
      if (in->dst != -1 || in->a < 0 || in->f != -1 ||
          in->extra_args_len != 0 || !in->symbol ||
          sscanf(in->symbol, "kvm_vmmcall;slot=%d%n", &slot, &consumed) != 1 ||
          consumed < 0 || slot < 0 || slot >= c->max_local_slot)
        return false;
      const bool alternative = strcmp(in->symbol + consumed, ";alt=271") == 0;
      if (!alternative && in->symbol[consumed] != '\0')
        return false;
      const int inputs[] = {in->a, in->b, in->c, in->d, in->e};
      static const char *const input_regs[] = {"%r10", "%rbx", "%rcx", "%rdx",
                                               "%rsi"};
      for (size_t i = 0; i < 5; ++i) {
        if (inputs[i] < 0) {
          for (size_t later = i + 1; later < 5; ++later)
            if (inputs[later] >= 0)
              return false;
          break;
        }
      }
      ny_x64_nir_slot_str(c, slot, address, sizeof(address));
      if (!address[0] || address[0] == '%')
        return false;
      c->rax_cached_slot = -1;
      size_t input_count = 0;
      for (size_t i = 0; i < 5 && inputs[i] >= 0; ++i) {
        ++input_count;
        if (!ny_x64_nir_load_value(c, inputs[i]) ||
            !nyr_printf(c->w, "\tmovq\t%%rax, %s\n", input_regs[i]))
          return false;
      }
      if (input_count == 0 || !nyr_put(c->w, "\tmovq\t%r10, %rax\n"))
        return false;
      if (alternative) {
        if (!nyr_put(c->w, "771:\n\tvmcall\n772:\n"
                           "\t.skip\t-(((775f-774f)-(772b-771b)) > 0) * "
                           "((775f-774f)-(772b-771b)),0x90\n"
                           "773:\n"
                           "\t.pushsection .altinstructions, \"aM\", "
                           "@progbits, 14\n"
                           "\t.long\t771b - .\n\t.long\t774f - .\n"
                           "\t.4byte\t271\n\t.byte\t773b-771b\n"
                           "\t.byte\t775f-774f\n\t.popsection\n"
                           "\t.pushsection .altinstr_replacement, \"ax\"\n"
                           "912:\t.pushsection .discard.annotate_data, \"M\", "
                           "@progbits, 8\n\t.long\t912b - ., 1\n"
                           "\t.popsection\n774:\n\tvmmcall\n775:\n"
                           "\t.popsection\n"))
          return false;
      } else if (!nyr_put(c->w, "\tvmmcall\n")) {
        return false;
      }
      c->rax_cached_slot = -1;
      return nyr_printf(c->w, "\tmovq\t%%rax, %s\n", address);
    }
    if (in->imm == NY_X64_ASM_VMWARE_PORT) {
      int slots[6] = {-1, -1, -1, -1, -1, -1};
      int consumed = -1;
      unsigned port = 0;
      char addresses[6][64] = {{0}};
      bool alternative = false;
      if (in->dst != -1 || in->a < 0 || in->b < 0 || in->c < 0 || in->d < 0 ||
          in->e < 0 || in->f < 0 || in->extra_args_len != 0 || !in->symbol ||
          sscanf(in->symbol,
                 "vmware_port;nyc_slots=%d,%d,%d,%d,%d,%d;port=%u%n", &slots[0],
                 &slots[1], &slots[2], &slots[3], &slots[4], &slots[5], &port,
                 &consumed) != 7 ||
          consumed < 0 || port > UINT16_MAX)
        return false;
      if (strcmp(in->symbol + consumed, "") != 0 &&
          strcmp(in->symbol + consumed, ";alt=274") != 0)
        return false;
      alternative = strcmp(in->symbol + consumed, ";alt=274") == 0;
      const int inputs[] = {in->a, in->b, in->c, in->d, in->e, in->f};
      for (size_t i = 0; i < 6; ++i) {
        if (slots[i] < 0 || slots[i] >= c->max_local_slot) {
          nyr_set_error(c->err, c->err_len,
                        "nyir x86-64: invalid VMware output slot %d/%d",
                        slots[i], c->max_local_slot);
          return false;
        }
        ny_x64_nir_slot_str(c, slots[i], addresses[i], sizeof(addresses[i]));
        if (!addresses[i][0] || addresses[i][0] == '%') {
          nyr_set_error(c->err, c->err_len,
                        "nyir x86-64: VMware output slot %d has address %s",
                        slots[i], addresses[i]);
          return false;
        }
      }
      static const char *const input_regs[] = {"%r10", "%rbx", "%rcx",
                                               "%rdx", "%rsi", "%rdi"};
      c->rax_cached_slot = -1;
      for (size_t i = 0; i < 6; ++i)
        if (!ny_x64_nir_load_value(c, inputs[i]) ||
            !nyr_printf(c->w, "\tmovq\t%%rax, %s\n", input_regs[i]))
          return false;
      if (!nyr_put(c->w, "\tmovq\t%r10, %rax\n") ||
          (alternative && !nyr_put(c->w, "771:\n")) ||
          !nyr_printf(c->w, "\tmovw\t$%u, %%dx\n\tinl\t(%%dx), %%eax\n", port))
        return false;
      if (alternative &&
          !nyr_put(c->w,
                   "772:\n"
                   "\t.skip\t-(((775f-774f)-(772b-771b)) > 0) * "
                   "((775f-774f)-(772b-771b)),0x90\n"
                   "773:\n"
                   "\t.pushsection .altinstructions, \"aM\", @progbits, 14\n"
                   "\t.long\t771b - .\n"
                   "\t.long\t774f - .\n"
                   "\t.4byte\t274\n"
                   "\t.byte\t773b-771b\n"
                   "\t.byte\t775f-774f\n"
                   "\t.popsection\n"
                   "\t.pushsection .altinstr_replacement, \"ax\"\n"
                   "912:\t.pushsection .discard.annotate_data, \"M\", "
                   "@progbits, 8\n"
                   "\t.long\t912b - ., 1\n"
                   "\t.popsection\n"
                   "774:\n775:\n\t.popsection\n"))
        return false;
      static const char *const output_regs[] = {"%rax", "%rbx", "%rcx",
                                                "%rdx", "%rsi", "%rdi"};
      for (size_t i = 0; i < 6; ++i)
        if (!nyr_printf(c->w, "\tmovq\t%s, %s\n", output_regs[i], addresses[i]))
          return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == NY_X64_ASM_VMCALL_SLOTS) {
      int slots[6] = {-1, -1, -1, -1, -1, -1};
      int consumed = -1;
      char mnemonic[8] = {0};
      char addresses[6][64] = {{0}};
      if (in->dst != -1 || in->a < 0 || in->b < 0 || in->c < 0 || in->d < 0 ||
          in->e < 0 || in->f < 0 || in->extra_args_len != 0 || !in->symbol ||
          sscanf(in->symbol, "%7[^;];nyc_slots=%d,%d,%d,%d,%d,%d%n", mnemonic,
                 &slots[0], &slots[1], &slots[2], &slots[3], &slots[4],
                 &slots[5], &consumed) != 7 ||
          consumed < 0 || in->symbol[consumed] != '\0' ||
          (strcmp(mnemonic, "vmcall") != 0 && strcmp(mnemonic, "vmmcall") != 0))
        return false;
      const int inputs[] = {in->a, in->b, in->c, in->d, in->e, in->f};
      for (size_t i = 0; i < 6; ++i) {
        if (slots[i] < 0 || slots[i] >= c->max_local_slot) {
          nyr_set_error(c->err, c->err_len,
                        "nyir x86-64: invalid VMware output slot %d/%d",
                        slots[i], c->max_local_slot);
          return false;
        }
        ny_x64_nir_slot_str(c, slots[i], addresses[i], sizeof(addresses[i]));
        if (!addresses[i][0] || addresses[i][0] == '%') {
          nyr_set_error(c->err, c->err_len,
                        "nyir x86-64: VMware output slot %d has address %s",
                        slots[i], addresses[i]);
          return false;
        }
      }
      static const char *const input_regs[] = {"%r10", "%rbx", "%rcx",
                                               "%rdx", "%rsi", "%rdi"};
      c->rax_cached_slot = -1;
      for (size_t i = 0; i < 6; ++i) {
        if (!ny_x64_nir_load_value(c, inputs[i]) ||
            !nyr_printf(c->w, "\tmovq\t%%rax, %s\n", input_regs[i])) {
          nyr_set_error(c->err, c->err_len,
                        "nyir x86-64: VMware input %zu could not be loaded", i);
          return false;
        }
      }
      if (!nyr_put(c->w, "\tmovq\t%r10, %rax\n") ||
          !nyr_printf(c->w, "\t%s\n", mnemonic))
        return false;
      static const char *const output_regs[] = {"%rax", "%rbx", "%rcx",
                                                "%rdx", "%rsi", "%rdi"};
      for (size_t i = 0; i < 6; ++i)
        if (!nyr_printf(c->w, "\tmovq\t%s, %s\n", output_regs[i], addresses[i]))
          return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -139) {
      if (in->dst != -1 || in->a < 0 || in->b != -1 || in->c != -1 ||
          in->d != -1 || in->e != -1 || in->f != -1 || !in->symbol ||
          strcmp(in->symbol, "invlpg") != 0 ||
          !ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\tinvlpg\t(%rax)\n"))
        return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -138) {
      if (in->dst != -1 || in->a < 0 || in->b < 0 || in->c < 0 || in->d < 0 ||
          in->e < 0 || in->f < 0 || !in->symbol ||
          !strstr(in->symbol, "# ALT: oldinstr") ||
          !strstr(in->symbol, "call rep_movs_alternative") ||
          !strstr(in->symbol, ".altinstructions") ||
          !ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\tmovq\t%rax, %rdi\n") ||
          !ny_x64_nir_load_value(c, in->b) ||
          !nyr_put(c->w, "\tmovq\t%rax, %rsi\n") ||
          !ny_x64_nir_load_value(c, in->c) ||
          !nyr_put(c->w, "\tmovq\t%rax, %rcx\n") || !nyr_put(c->w, in->symbol))
        return false;
      const int addresses[] = {in->d, in->e, in->f};
      const char *registers[] = {"%rdi", "%rsi", "%rcx"};
      for (size_t i = 0; i < 3; ++i) {
        if (!ny_x64_nir_load_value(c, addresses[i]) ||
            !nyr_printf(c->w, "\tmovq\t%s, (%%rax)\n", registers[i]))
          return false;
      }
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -137) {
      if (in->dst != -1 || in->a != -1 || in->b != -1 || in->c != -1 ||
          in->d != -1 || in->e != -1 || in->f != -1 || !in->symbol ||
          !strstr(in->symbol, "# ALT: oldinstr") ||
          !strstr(in->symbol, ".altinstructions") ||
          !strstr(in->symbol, "call __alt_reloc_selftest"))
        return false;
      c->rax_cached_slot = -1;
      return nyr_put(c->w, in->symbol);
    }
    if (in->imm == -136) {
      const char *store_instruction =
          !in->symbol                            ? NULL
          : strcmp(in->symbol, "rep_stosb") == 0 ? "\trep stosb\n"
          : strcmp(in->symbol, "rep_stosw") == 0 ? "\trep stosw\n"
          : strcmp(in->symbol, "rep_stosl") == 0 ? "\trep stosl\n"
          : strcmp(in->symbol, "rep_stosq") == 0 ? "\trep stosq\n"
                                                 : NULL;
      if (in->dst != -1 || in->a < 0 || in->b < 0 || in->c < 0 || in->d < 0 ||
          in->e < 0 || in->f != -1 || !store_instruction ||
          !ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\tmovq\t%rax, %rdi\n") ||
          !ny_x64_nir_load_value(c, in->b) ||
          !nyr_put(c->w, "\tmovq\t%rax, %rcx\n") ||
          !ny_x64_nir_load_value(c, in->c) || !nyr_put(c->w, store_instruction))
        return false;
      const int addresses[] = {in->d, in->e};
      const char *registers[] = {"%rdi", "%rcx"};
      for (size_t i = 0; i < 2; ++i) {
        if (!ny_x64_nir_load_value(c, addresses[i]) ||
            !nyr_printf(c->w, "\tmovq\t%s, (%%rax)\n", registers[i]))
          return false;
      }
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -167) {
      if (in->dst < 0 || in->a < 0 || in->b != -1 || in->c != -1 ||
          in->d != -1 || in->e != -1 || in->f != -1 || !in->symbol ||
          strcmp(in->symbol, "x86_delay_loop") != 0 ||
          !ny_x64_nir_load_value(c, in->a) ||
          !nyr_printf(c->w,
                      "\ttestq\t%%rax, %%rax\n"
                      "\tjz\t.Lnyir_%s_delay_end_%zu\n"
                      "\tjmp\t.Lnyir_%s_delay_start_%zu\n"
                      "\t.p2align\t4\n"
                      ".Lnyir_%s_delay_start_%zu:\n"
                      "\tjmp\t.Lnyir_%s_delay_count_%zu\n"
                      "\t.p2align\t4\n"
                      ".Lnyir_%s_delay_count_%zu:\n"
                      "\tdecq\t%%rax\n"
                      "\tjnz\t.Lnyir_%s_delay_count_%zu\n"
                      ".Lnyir_%s_delay_end_%zu:\n"
                      "\tdecq\t%%rax\n",
                      c->label_prefix, idx, c->label_prefix, idx,
                      c->label_prefix, idx, c->label_prefix, idx,
                      c->label_prefix, idx, c->label_prefix, idx,
                      c->label_prefix, idx) ||
          !ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst)))
        return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -168) {
      if (in->dst != -1 || in->a < 0 || in->b < 0 || in->c != -1 ||
          in->d != -1 || in->e != -1 || in->f != -1 || !in->symbol ||
          (strcmp(in->symbol, "usercopy_movnti_4x8") != 0 &&
           strcmp(in->symbol, "usercopy_movnti_1x8") != 0 &&
           strcmp(in->symbol, "usercopy_movnti_1x4") != 0) ||
          !ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\tmovq\t%rax, %rdi\n") ||
          !ny_x64_nir_load_value(c, in->b) ||
          !nyr_put(c->w, "\tmovq\t%rax, %rsi\n"))
        return false;
      if (strcmp(in->symbol, "usercopy_movnti_4x8") == 0) {
        if (!nyr_put(c->w, "\tmovq\t(%rdi), %r8\n"
                           "\tmovq\t8(%rdi), %r9\n"
                           "\tmovq\t16(%rdi), %r10\n"
                           "\tmovq\t24(%rdi), %r11\n"
                           "\tmovnti\t%r8, (%rsi)\n"
                           "\tmovnti\t%r9, 8(%rsi)\n"
                           "\tmovnti\t%r10, 16(%rsi)\n"
                           "\tmovnti\t%r11, 24(%rsi)\n"))
          return false;
      } else if (strcmp(in->symbol, "usercopy_movnti_1x8") == 0) {
        if (!nyr_put(c->w, "\tmovq\t(%rdi), %r8\n"
                           "\tmovnti\t%r8, (%rsi)\n"))
          return false;
      } else if (!nyr_put(c->w, "\tmovl\t(%rdi), %r8d\n"
                                "\tmovnti\t%r8d, (%rsi)\n")) {
        return false;
      }
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -179) {
      bool four_vectors =
          in->symbol && (strcmp(in->symbol, "movntdqa_movaps_4") == 0 ||
                         strcmp(in->symbol, "movntdqa_movups_4") == 0);
      bool unaligned_store =
          in->symbol && (strcmp(in->symbol, "movntdqa_movups_1") == 0 ||
                         strcmp(in->symbol, "movntdqa_movups_4") == 0);
      if (in->dst != -1 || in->a < 0 || in->b < 0 || in->c != -1 ||
          in->d != -1 || in->e != -1 || in->f != -1 || !in->symbol ||
          (!four_vectors && !unaligned_store &&
           strcmp(in->symbol, "movntdqa_movaps_1") != 0) ||
          !ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\tmovq\t%rax, %rdi\n") ||
          !ny_x64_nir_load_value(c, in->b) ||
          !nyr_put(c->w, "\tmovq\t%rax, %rsi\n"))
        return false;
      c->rax_cached_slot = -1;
      if (four_vectors) {
        if (!nyr_put(c->w, "\tmovntdqa\t(%rdi), %xmm0\n"
                           "\tmovntdqa\t16(%rdi), %xmm1\n"
                           "\tmovntdqa\t32(%rdi), %xmm2\n"
                           "\tmovntdqa\t48(%rdi), %xmm3\n"))
          return false;
        const char *store = unaligned_store ? "movups" : "movaps";
        if (!nyr_printf(c->w,
                        "\t%s\t%%xmm0, (%%rsi)\n"
                        "\t%s\t%%xmm1, 16(%%rsi)\n"
                        "\t%s\t%%xmm2, 32(%%rsi)\n"
                        "\t%s\t%%xmm3, 48(%%rsi)\n",
                        store, store, store, store))
          return false;
      } else if (!nyr_printf(c->w,
                             "\tmovntdqa\t(%%rdi), %%xmm0\n"
                             "\t%s\t%%xmm0, (%%rsi)\n",
                             unaligned_store ? "movups" : "movaps")) {
        return false;
      }
      return true;
    }
    if (in->imm == -182) {
      if (in->dst != -1 || in->a < 0 || in->b < 0 || in->c < 0 || in->d < 0 ||
          !in->symbol ||
          (strcmp(in->symbol, "movsb") != 0 &&
           strcmp(in->symbol, "movsw") != 0 &&
           strcmp(in->symbol, "movsl") != 0 &&
           strcmp(in->symbol, "movsq") != 0) ||
          !ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\tmovq\t%rax, %rdi\n") ||
          !ny_x64_nir_load_value(c, in->b) ||
          !nyr_put(c->w, "\tmovq\t%rax, %rsi\n") ||
          !nyr_printf(c->w, "\t%s\n", in->symbol) ||
          !ny_x64_nir_load_value(c, in->c) ||
          !nyr_put(c->w, "\tmovq\t%rdi, (%rax)\n") ||
          !ny_x64_nir_load_value(c, in->d) ||
          !nyr_put(c->w, "\tmovq\t%rsi, (%rax)\n"))
        return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -186) {
      const char *pointer =
          strncmp(in->symbol, "rep ins", 7) == 0 ? "%rdi" : "%rsi";
      if (!ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\tmovq\t%rax, %r8\n") ||
          !ny_x64_nir_load_value(c, in->b) ||
          !nyr_put(c->w, "\tmovq\t%rax, %r9\n") ||
          !ny_x64_nir_load_value(c, in->c) ||
          !nyr_put(c->w, "\tmovl\t%eax, %edx\n") ||
          !nyr_printf(c->w,
                      "\tmovq\t(%%r8), %s\n"
                      "\tmovq\t(%%r9), %%rcx\n\t%s\n"
                      "\tmovq\t%s, (%%r8)\n"
                      "\tmovq\t%%rcx, (%%r9)\n",
                      pointer, in->symbol, pointer))
        return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -183) {
      if (in->dst != -1 || in->a < 0 || in->b < 0 || in->c < 0 || in->d < 0 ||
          !in->symbol || strcmp(in->symbol, "xsha256") != 0 ||
          !ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\tmovq\t%rax, %r8\n") ||
          !ny_x64_nir_load_value(c, in->b) ||
          !nyr_put(c->w, "\tmovq\t%rax, %r9\n") ||
          !ny_x64_nir_load_value(c, in->c) ||
          !nyr_put(c->w, "\tmovq\t%rax, %r10\n") ||
          !ny_x64_nir_load_value(c, in->d) ||
          !nyr_put(c->w, "\tmovq\t%rax, %rdi\n"
                         "\tmovq\t(%r8), %rax\n"
                         "\tmovq\t(%r9), %rcx\n"
                         "\tmovq\t(%r10), %rsi\n"
                         "\t.byte\t0xf3, 0x0f, 0xa6, 0xd0\n"
                         "\tmovq\t%rax, (%r8)\n"
                         "\tmovq\t%rcx, (%r9)\n"
                         "\tmovq\t%rsi, (%r10)\n"))
        return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -178) {
      if (in->dst != -1 || in->a < 0 || in->b < 0 || in->c < 0 || in->d < 0 ||
          in->e < 0 || in->f != -1 || !in->symbol ||
          strcmp(in->symbol, "xstore") != 0 ||
          !ny_x64_nir_load_value(c, in->b) ||
          !nyr_put(c->w, "\tmovq\t%rax, %rdi\n") ||
          !ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\tmovl\t%eax, %edx\n"))
        return false;
      c->rax_cached_slot = -1;
      if (!nyr_put(c->w, "\t.byte\t0x0f, 0xa7, 0xc0\n"
                         "\tmovl\t%eax, %r8d\n"
                         "\tmovl\t%edx, %r9d\n"
                         "\tmovq\t%rdi, %r10\n"))
        return false;
      const int addresses[] = {in->e, in->d, in->c};
      const char *registers[] = {"%r8d", "%r9d", "%r10"};
      const char *stores[] = {"movl", "movl", "movq"};
      for (size_t i = 0; i < 3; ++i) {
        if (!ny_x64_nir_load_value(c, addresses[i]) ||
            !nyr_printf(c->w, "\t%s\t%s, (%%rax)\n", stores[i], registers[i]))
          return false;
      }
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -177) {
      if (in->dst != -1 || in->a < 0 || in->b < 0 || in->c < 0 || in->d < 0 ||
          in->e < 0 || in->f < 0 || !in->symbol ||
          strcmp(in->symbol, "rep_movsl_tail") != 0 ||
          !ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\tmovq\t%rax, %rdi\n") ||
          !ny_x64_nir_load_value(c, in->b) ||
          !nyr_put(c->w, "\tmovq\t%rax, %rsi\n") ||
          !ny_x64_nir_load_value(c, in->c) ||
          !nyr_put(c->w, "\tmovq\t%rax, %rcx\n") ||
          !ny_x64_nir_load_value(c, in->d) ||
          !nyr_put(c->w, "\tmovq\t%rax, %r10\n"
                         "\trep movsl\n"
                         "\ttestb\t$2, %r10b\n"
                         "\tje\t1f\n"
                         "\tmovsw\n"
                         "1:\ttestb\t$1, %r10b\n"
                         "\tje\t2f\n"
                         "\tmovsb\n"
                         "2:\n"))
        return false;
      const int addresses[] = {in->e, in->f};
      const char *registers[] = {"%rdi", "%rsi"};
      for (size_t i = 0; i < 2; ++i) {
        if (!ny_x64_nir_load_value(c, addresses[i]) ||
            !nyr_printf(c->w, "\tmovq\t%s, (%%rax)\n", registers[i]))
          return false;
      }
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -135) {
      if (in->dst != -1 || in->a < 0 || in->b < 0 || in->c < 0 || in->d < 0 ||
          in->e < 0 || in->f < 0 || !in->symbol ||
          strcmp(in->symbol, "rep_movsb") != 0 ||
          !ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\tmovq\t%rax, %rdi\n") ||
          !ny_x64_nir_load_value(c, in->b) ||
          !nyr_put(c->w, "\tmovq\t%rax, %rsi\n") ||
          !ny_x64_nir_load_value(c, in->c) ||
          !nyr_put(c->w, "\tmovq\t%rax, %rcx\n\trep movsb\n"))
        return false;
      const int addresses[] = {in->d, in->e, in->f};
      const char *registers[] = {"%rdi", "%rsi", "%rcx"};
      for (size_t i = 0; i < 3; ++i) {
        if (!ny_x64_nir_load_value(c, addresses[i]) ||
            !nyr_printf(c->w, "\tmovq\t%s, (%%rax)\n", registers[i]))
          return false;
      }
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -31) {
      if (in->dst != -1 || in->a < 0 || in->b < 0 || in->c < 0 || in->d < 0 ||
          in->e < 0 || in->f < 0 || !in->symbol ||
          strcmp(in->symbol, "cpuid") != 0) {
        nyr_set_error(c->err, c->err_len,
                      "nyir x86-64: invalid cpuid inline assembly");
        return false;
      }
      c->rax_cached_slot = -1;
      if (!ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\tmovl\t%eax, %r10d\n") ||
          !ny_x64_nir_load_value(c, in->b) ||
          !nyr_put(c->w, "\tmovl\t%eax, %ecx\n\tmovl\t%r10d, %eax\n"
                         "\tpushq\t%rbx\n\tcpuid\n\tmovl\t%eax, %r8d\n"
                         "\tmovl\t%ebx, %r9d\n"
                         "\tmovl\t%ecx, %r10d\n"
                         "\tmovl\t%edx, %r11d\n\tpopq\t%rbx\n"))
        return false;
      c->rax_cached_slot = -1;
      const int addresses[] = {in->c, in->d, in->e, in->f};
      const char *registers[] = {"%r8d", "%r9d", "%r10d", "%r11d"};
      for (size_t i = 0; i < 4; ++i) {
        if (!ny_x64_nir_load_value(c, addresses[i]) ||
            !nyr_printf(c->w, "\tmovl\t%s, (%%rax)\n", registers[i]))
          return false;
      }
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -30) {
      int low_slot = -1;
      int high_slot = -1;
      int consumed = -1;
      if (in->dst != -1 || in->a < 0 || in->b != -1 || in->c != -1 ||
          in->d < 0 || in->e < 0 || !in->symbol ||
          sscanf(in->symbol, "rdpmc;nyc_slots=%d,%d%n", &low_slot, &high_slot,
                 &consumed) != 2 ||
          consumed < 0 || in->symbol[consumed] != '\0' || low_slot < 0 ||
          high_slot < 0 || low_slot == high_slot ||
          low_slot >= c->max_local_slot || high_slot >= c->max_local_slot) {
        nyr_set_error(c->err, c->err_len,
                      "nyir x86-64: invalid rdpmc inline assembly");
        return false;
      }
      char low_address[64];
      char high_address[64];
      ny_x64_nir_slot_str(c, low_slot, low_address, sizeof(low_address));
      ny_x64_nir_slot_str(c, high_slot, high_address, sizeof(high_address));
      c->rax_cached_slot = -1;
      if (!low_address[0] || low_address[0] == '%' || !high_address[0] ||
          high_address[0] == '%' || !ny_x64_nir_load_value(c, in->a) ||
          !nyr_printf(c->w,
                      "\tmovl\t%%eax, %%ecx\n\trdpmc\n"
                      "\tmovq\t%%rax, %s\n\tmovq\t%%rdx, %s\n",
                      low_address, high_address))
        return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -29) {
      int error_slot = -1;
      int consumed = -1;
      if (in->dst != -1 || in->a < 0 || in->b < 0 || in->c < 0 || in->d < 0 ||
          in->e != -1 || !in->symbol ||
          sscanf(in->symbol, "wrmsr_safe;nyc_slots=%d%n", &error_slot,
                 &consumed) != 1 ||
          consumed < 0 || in->symbol[consumed] != '\0' || error_slot < 0 ||
          error_slot >= c->max_local_slot) {
        nyr_set_error(c->err, c->err_len,
                      "nyir x86-64: invalid safe wrmsr inline assembly");
        return false;
      }
      char error_address[64];
      ny_x64_nir_slot_str(c, error_slot, error_address, sizeof(error_address));
      c->rax_cached_slot = -1;
      if (!error_address[0] || error_address[0] == '%' ||
          !ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\tmovl\t%eax, %ecx\n") ||
          !ny_x64_nir_load_value(c, in->c) ||
          !nyr_put(c->w, "\tmovl\t%eax, %edx\n") ||
          !ny_x64_nir_load_value(c, in->b) ||
          !nyr_printf(c->w,
                      "1:\twrmsr ; xorl %%eax, %%eax\n2:\n"
                      "\t.pushsection __ex_table,\"aM\",@progbits,12\n"
                      "\t.balign 4\n\t.long 1b - .\n\t.long 2b - .\n"
                      "\t.long 11\n\t.popsection\n\tmovl\t%%eax, %s\n",
                      error_address))
        return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -30) {
      int error_slot = -1;
      int value_slot = -1;
      int consumed = -1;
      if (in->dst != -1 || in->a < 0 || in->b < 0 || in->c < 0 || in->d != -1 ||
          in->e != -1 || in->f != -1 || !in->symbol ||
          sscanf(in->symbol, "rdmsr_safe_pair32;nyc_slots=%d,%d%n", &error_slot,
                 &value_slot, &consumed) != 2 ||
          consumed < 0 || in->symbol[consumed] != '\0' || error_slot < 0 ||
          value_slot < 0 || error_slot == value_slot ||
          error_slot >= c->max_local_slot || value_slot >= c->max_local_slot) {
        nyr_set_error(c->err, c->err_len,
                      "nyir x86: invalid paired safe rdmsr inline assembly");
        return false;
      }
      char error_address[64];
      char value_address[64];
      ny_x64_nir_slot_str(c, error_slot, error_address, sizeof(error_address));
      ny_x64_nir_slot_str(c, value_slot, value_address, sizeof(value_address));
      c->rax_cached_slot = -1;
      if (!error_address[0] || error_address[0] == '%' || !value_address[0] ||
          value_address[0] == '%' || !ny_x64_nir_load_value(c, in->a) ||
          !nyr_printf(c->w,
                      "\tmovl\t%%eax, %%ecx\n"
                      "1:\trdmsr ; xorl %%ecx, %%ecx\n2:\n"
                      "\t.pushsection __ex_table,\"aM\",@progbits,12\n"
                      "\t.balign 4\n\t.long 1b - .\n\t.long 2b - .\n"
                      "\t.long 267\n\t.popsection\n"
                      "\tmovl\t%%ecx, %s\n"
                      "\tmovl\t%%eax, %s\n\tmovl\t%%edx, 4+%s\n",
                      error_address, value_address, value_address))
        return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -28) {
      int error_slot = -1;
      int low_slot = -1;
      int high_slot = -1;
      int consumed = -1;
      if (in->dst != -1 || in->a < 0 || in->b < 0 || in->c != -1 || in->d < 0 ||
          in->e < 0 || !in->symbol ||
          sscanf(in->symbol, "rdmsr_safe;nyc_slots=%d,%d,%d%n", &error_slot,
                 &low_slot, &high_slot, &consumed) != 3 ||
          consumed < 0 || in->symbol[consumed] != '\0' || error_slot < 0 ||
          low_slot < 0 || high_slot < 0 || error_slot == low_slot ||
          error_slot == high_slot || low_slot == high_slot ||
          error_slot >= c->max_local_slot || low_slot >= c->max_local_slot ||
          high_slot >= c->max_local_slot) {
        nyr_set_error(c->err, c->err_len,
                      "nyir x86-64: invalid safe rdmsr inline assembly");
        return false;
      }
      char error_address[64];
      char low_address[64];
      char high_address[64];
      ny_x64_nir_slot_str(c, error_slot, error_address, sizeof(error_address));
      ny_x64_nir_slot_str(c, low_slot, low_address, sizeof(low_address));
      ny_x64_nir_slot_str(c, high_slot, high_address, sizeof(high_address));
      c->rax_cached_slot = -1;
      if (!error_address[0] || error_address[0] == '%' || !low_address[0] ||
          low_address[0] == '%' || !high_address[0] || high_address[0] == '%' ||
          !ny_x64_nir_load_value(c, in->a) ||
          !nyr_printf(c->w,
                      "\tmovl\t%%eax, %%ecx\n"
                      "1:\trdmsr ; xorl %%r10d, %%r10d\n2:\n"
                      "\t.pushsection __ex_table,\"aM\",@progbits,12\n"
                      "\t.balign 4\n\t.long 1b - .\n\t.long 2b - .\n"
                      "\t.long 2571\n\t.popsection\n"
                      "\tmovl\t%%r10d, %s\n\tmovq\t%%rax, %s\n"
                      "\tmovq\t%%rdx, %s\n",
                      error_address, low_address, high_address))
        return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -27) {
      int low_slot = -1;
      int high_slot = -1;
      int consumed = -1;
      bool mce =
          in->symbol && strncmp(in->symbol, "rdmsr_mce;nyc_slots=", 20) == 0;
      if (in->dst != -1 || in->a < 0 || in->b != -1 || in->c != -1 ||
          in->d < 0 || in->e < 0 || !in->symbol ||
          sscanf(in->symbol,
                 mce ? "rdmsr_mce;nyc_slots=%d,%d%n"
                     : "rdmsr;nyc_slots=%d,%d%n",
                 &low_slot, &high_slot, &consumed) != 2 ||
          consumed < 0 || in->symbol[consumed] != '\0' || low_slot < 0 ||
          high_slot < 0 || low_slot == high_slot ||
          low_slot >= c->max_local_slot || high_slot >= c->max_local_slot) {
        nyr_set_error(c->err, c->err_len,
                      "nyir x86-64: invalid rdmsr inline assembly");
        return false;
      }
      char low_address[64];
      char high_address[64];
      ny_x64_nir_slot_str(c, low_slot, low_address, sizeof(low_address));
      ny_x64_nir_slot_str(c, high_slot, high_address, sizeof(high_address));
      c->rax_cached_slot = -1;
      if (!low_address[0] || low_address[0] == '%' || !high_address[0] ||
          high_address[0] == '%' || !ny_x64_nir_load_value(c, in->a) ||
          !nyr_printf(c->w,
                      "\tmovl\t%%eax, %%ecx\n1:\trdmsr\n2:\n"
                      "\t.pushsection __ex_table,\"aM\",@progbits,12\n"
                      "\t.balign 4\n\t.long 1b - .\n\t.long 2b - .\n"
                      "\t.long %d\n\t.popsection\n"
                      "\tmovl\t%%eax, %s\n\tmovl\t%%edx, %s\n",
                      mce ? 13 : 9, low_address, high_address))
        return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -25) {
      if (in->dst < 0 || in->a < 0 || in->b < 0 || in->c != -1 || !in->symbol ||
          strcmp(in->symbol, "bsrq") != 0) {
        nyr_set_error(c->err, c->err_len,
                      "nyir x86-64: invalid bsr inline assembly");
        return false;
      }
      c->rax_cached_slot = -1;
      if (!ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\tmovq\t%rax, %r10\n") ||
          !ny_x64_nir_load_value(c, in->b) ||
          !nyr_put(c->w, "\tmovq\t%rax, %r11\n\tmovq\t%r10, %rax\n"
                         "\tbsrq\t%rax, %rax\n\tcmovzq\t%r11, %rax\n"))
        return false;
      c->rax_cached_slot = -1;
      return ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    }
    if (in->imm == -34) {
      char helper[96] = {0};
      int return_slot = -1;
      if (in->dst != -1 || in->a < 0 || in->b < 0 || in->c != -1 ||
          !in->symbol ||
          strncmp(in->symbol, "call __put_user_",
                  sizeof("call __put_user_") - 1) != 0 ||
          sscanf(in->symbol, "call %95[^;];nyc_slots=%d", helper,
                 &return_slot) != 2 ||
          return_slot < 0) {
        nyr_set_error(c->err, c->err_len,
                      "nyir x86-64: invalid put_user inline assembly");
        return false;
      }
      if (return_slot >= c->max_local_slot)
        return false;
      char return_address[64];
      ny_x64_nir_slot_str(c, return_slot, return_address,
                          sizeof(return_address));
      c->rax_cached_slot = -1;
      if (!return_address[0] || return_address[0] == '%' ||
          !ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\tmovq\t%rax, %rcx\n") ||
          !ny_x64_nir_load_value(c, in->b) ||
          !nyr_printf(c->w,
                      "\tcall\t%s\n\tmovslq\t%%ecx, %%r11\n"
                      "\tmovq\t%%r11, %s\n",
                      helper, return_address))
        return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == NY_X64_ASM_GET_USER) {
      char helper[96] = {0};
      int return_slot = -1;
      int value_slot = -1;
      if (in->dst != -1 || in->a < 0 || in->b != -1 || in->c != -1 ||
          !in->symbol ||
          strncmp(in->symbol, "call __get_user_",
                  sizeof("call __get_user_") - 1) != 0 ||
          sscanf(in->symbol, "call %95[^;];nyc_slots=%d,%d", helper,
                 &return_slot, &value_slot) != 3 ||
          return_slot < 0 || value_slot < 0) {
        nyr_set_error(c->err, c->err_len,
                      "nyir x86-64: invalid get_user inline assembly");
        return false;
      }
      if (return_slot >= c->max_local_slot || value_slot >= c->max_local_slot)
        return false;
      char return_address[64];
      char value_address[64];
      ny_x64_nir_slot_str(c, return_slot, return_address,
                          sizeof(return_address));
      ny_x64_nir_slot_str(c, value_slot, value_address, sizeof(value_address));
      c->rax_cached_slot = -1;
      if (!return_address[0] || return_address[0] == '%' || !value_address[0] ||
          value_address[0] == '%' || !ny_x64_nir_load_value(c, in->a) ||
          !nyr_printf(c->w,
                      "\tcall\t%s\n\tmovq\t%%rax, %s\n\tmovq\t%%rdx, %s\n",
                      helper, return_address, value_address))
        return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->symbol && strcmp(in->symbol, "__nyc_builtin_alloca_dynamic") == 0) {
      if (in->dst < 0 || in->a < 0 || !ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\taddq\t$15, %rax\n\tandq\t$-16, %rax\n"
                         "\tsubq\t%rax, %rsp\n\tmovq\t%rsp, %rax\n"))
        return false;
      return ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    }
    if (!in->symbol || strncmp(in->symbol, "__nyc_unsupported_",
                               sizeof("__nyc_unsupported_") - 1) == 0) {
      const char *details = in->symbol;
      const char *detail_prefix = "__nyc_unsupported_inline_asm:";
      bool is_inline_asm =
          details && strncmp(details, detail_prefix,
                             sizeof("__nyc_unsupported_inline_asm:") - 1) == 0;
      if (is_inline_asm)
        details += sizeof("__nyc_unsupported_inline_asm:") - 1;
      nyr_set_error(c->err, c->err_len,
                    details && is_inline_asm && details[0]
                        ? "nyir x86-64: unsupported inline-assembly operation "
                          "for template: %.400s"
                        : "nyir x86-64: unsupported inline-assembly operation",
                    details && is_inline_asm ? details : "");
      return false;
    }
    if (strcmp(in->symbol, "__nyc_builtin_return_address_0") == 0) {
      if (in->dst < 0 || in->a != -1 || in->imm != 0) {
        nyr_set_error(c->err, c->err_len,
                      "nyir x86-64: invalid return-address builtin operands");
        return false;
      }
      if (!nyr_printf(c->w, "\tmovq\t8(%%rbp), %%rax\n"))
        return false;
      return ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    }
    if (strcmp(in->symbol, "__nyc_builtin_frame_address_0") == 0) {
      if (in->dst < 0 || in->a != -1 || in->imm != 0) {
        nyr_set_error(c->err, c->err_len,
                      "nyir x86-64: invalid frame-address builtin operands");
        return false;
      }
      if (!nyr_printf(c->w, "\tmovq\t%%rbp, %%rax\n"))
        return false;
      return ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    }
    if (in->imm == -14) {
      if (in->dst != -1 || in->a < 0 || in->b < 0 || in->c < 0 || !in->symbol) {
        nyr_set_error(c->err, c->err_len,
                      "nyir x86-64: invalid jump-label asm operands");
        return false;
      }
      int key_value = in->a;
      const char *key_symbol = NULL;
      int64_t key_symbol_offset = 0;
      for (size_t hops = 0; hops < 32; ++hops) {
        if (key_value < 0 || key_value >= c->nyir->next_value ||
            key_value >= NY_X64_NIR_MAX_SLOTS)
          break;
        int definition = c->def_index[key_value];
        if (definition < 0 || (size_t)definition >= c->nyir->len)
          break;
        const nyr_inst_t *def = &c->nyir->data[definition];
        if (def->dst != key_value)
          break;
        if (def->op == NYIR_ADDR_SYMBOL) {
          key_symbol = ny_x64_nir_link_symbol(def->symbol);
          break;
        }
        if (def->op == NYIR_ADD_I64 || def->op == NYIR_SUB_I64) {
          int64_t constant = 0;
          bool have_constant = ny_x64_nir_find_const_val(c, def->b, &constant);
          if (have_constant) {
            key_value = def->a;
            if (def->op == NYIR_SUB_I64) {
              if (constant == INT64_MIN ||
                  key_symbol_offset < INT64_MIN + constant)
                break;
              key_symbol_offset -= constant;
            } else {
              if ((constant > 0 && key_symbol_offset > INT64_MAX - constant) ||
                  (constant < 0 && key_symbol_offset < INT64_MIN - constant))
                break;
              key_symbol_offset += constant;
            }
          } else if (def->op == NYIR_ADD_I64 &&
                     ny_x64_nir_find_const_val(c, def->a, &constant)) {
            key_value = def->b;
            if ((constant > 0 && key_symbol_offset > INT64_MAX - constant) ||
                (constant < 0 && key_symbol_offset < INT64_MIN - constant))
              break;
            key_symbol_offset += constant;
          } else {
            break;
          }
          continue;
        }
        if (def->op != NYIR_COPY)
          break;
        key_value = def->a;
      }
      int64_t branch_value = 0;
      char key_symbol_with_offset[512];
      if (key_symbol && key_symbol_offset) {
        int written =
            snprintf(key_symbol_with_offset, sizeof(key_symbol_with_offset),
                     "%s%+" PRId64, key_symbol, key_symbol_offset);
        if (written < 0 || (size_t)written >= sizeof(key_symbol_with_offset))
          return false;
        key_symbol = key_symbol_with_offset;
      }
      if (!key_symbol || !ny_x64_nir_find_const_val(c, in->b, &branch_value)) {
        nyr_set_error(c->err, c->err_len,
                      "nyir x86-64: jump-label asm needs a symbol address and "
                      "constant branch operand");
        return false;
      }
      const char *text = in->symbol;
      size_t length = strlen(text);
      size_t start = 0;
      for (size_t i = 0; i < length;) {
        const char *replacement = NULL;
        size_t consumed = 0;
        char label[256];
        if (i + 3 <= length && memcmp(text + i, "%c0", 3) == 0) {
          replacement = key_symbol;
          consumed = 3;
        } else if (i + 3 <= length && memcmp(text + i, "%c1", 3) == 0) {
          int written =
              snprintf(label, sizeof(label), "%" PRId64, branch_value);
          if (written < 0 || (size_t)written >= sizeof(label))
            return false;
          replacement = label;
          consumed = 3;
        } else if (i + 3 <= length && memcmp(text + i, "%l[", 3) == 0) {
          size_t close = i + 3;
          while (close < length && text[close] != ']')
            ++close;
          if (close == length) {
            nyr_set_error(c->err, c->err_len,
                          "nyir x86-64: unterminated jump-label target");
            return false;
          }
          int written = snprintf(label, sizeof(label), ".Lnyir_%s_L%d",
                                 c->label_prefix, in->c);
          if (written < 0 || (size_t)written >= sizeof(label))
            return false;
          replacement = label;
          consumed = close - i + 1;
        }
        if (!replacement) {
          ++i;
          continue;
        }
        if (i > start &&
            !nyr_printf(c->w, "%.*s", (int)(i - start), text + start))
          return false;
        if (!nyr_printf(c->w, "%s", replacement))
          return false;
        i += consumed;
        start = i;
      }
      return start == length ||
             nyr_printf(c->w, "%.*s\n", (int)(length - start), text + start);
    }
    if (in->imm == -13) {
      if (in->dst < 0 || in->a < 0 || !ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\ttzcntq\t%rax, %rax\n"))
        return false;
      c->rax_cached_slot = -1;
      return ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    }
    if (in->imm == -146) {
      if (in->a < 0 || in->b < 0 || !ny_x64_nir_load_value(c, in->b) ||
          !nyr_put(c->w, "\tmovq\t%rax, %rdi\n") ||
          !ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\tljmpl\t*(%rax)\n"))
        return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -157) {
      if (in->dst != -1 || in->a < 0 || in->b < 0 ||
          !ny_x64_nir_load_value(c, in->b) ||
          !nyr_put(c->w, "\tmovq\t%rax, %rsi\n") ||
          !ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\tleave\n\tjmp\t*%rax\n"))
        return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -181) {
      const char *source_move = NULL;
      const char *add_instruction = NULL;
      if (in->symbol && strcmp(in->symbol, "gs_addb") == 0) {
        source_move = "\tmovb\t%al, %r11b\n";
        add_instruction = "\taddb\t%r11b, %gs:(%rax)\n";
      } else if (in->symbol && strcmp(in->symbol, "gs_addw") == 0) {
        source_move = "\tmovw\t%ax, %r11w\n";
        add_instruction = "\taddw\t%r11w, %gs:(%rax)\n";
      } else if (in->symbol && strcmp(in->symbol, "gs_addl") == 0) {
        source_move = "\tmovl\t%eax, %r11d\n";
        add_instruction = "\taddl\t%r11d, %gs:(%rax)\n";
      } else if (in->symbol && strcmp(in->symbol, "gs_addq") == 0) {
        source_move = "\tmovq\t%rax, %r11\n";
        add_instruction = "\taddq\t%r11, %gs:(%rax)\n";
      }
      if (in->dst != -1 || in->a < 0 || in->b < 0 || !source_move ||
          !add_instruction || !ny_x64_nir_load_value(c, in->b) ||
          !nyr_put(c->w, source_move) || !ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, add_instruction))
        return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -147) {
      if (in->dst < 0 || in->a < 0 || in->b < 0 || !in->symbol ||
          !ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\tmovl\t%eax, %edx\n") ||
          !ny_x64_nir_load_value(c, in->b) || !nyr_put(c->w, in->symbol) ||
          !nyr_put(c->w, "\tmovl\t%edx, %eax\n"))
        return false;
      c->rax_cached_slot = -1;
      return ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    }
    if (in->imm == -148) {
      if (in->dst != -1 || in->a < 0 || !in->symbol ||
          !ny_x64_nir_load_value(c, in->a) || !nyr_put(c->w, in->symbol))
        return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -149) {
      if (in->dst < 0 || in->a < 0 || !in->symbol || !in->symbol[0] ||
          !((in->symbol[0] >= 'A' && in->symbol[0] <= 'Z') ||
            (in->symbol[0] >= 'a' && in->symbol[0] <= 'z') ||
            in->symbol[0] == '_'))
        return false;
      for (const unsigned char *p = (const unsigned char *)in->symbol; *p; ++p)
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
              (*p >= '0' && *p <= '9') || *p == '_'))
          return false;
      c->rax_cached_slot = -1;
      if (!ny_x64_nir_load_value(c, in->a) ||
          !nyr_printf(c->w,
                      "\tshrl\t$12, %%eax\n"
                      "1:\n"
                      "\t.pushsection runtime_shift_%s,\"a\"\n"
                      "\t.long 1b - 1 - .\n"
                      "\t.popsection\n",
                      in->symbol))
        return false;
      c->rax_cached_slot = -1;
      return ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    }
    if (in->imm == -156) {
      if (in->dst < 0 || in->a < 0 || in->b < 0 || in->c != -1 || in->d != -1 ||
          in->e != -1 || in->f != -1 || !in->symbol ||
          strcmp(in->symbol, "runtime_ptr_clamp") != 0)
        return false;
      c->rax_cached_slot = -1;
      if (!ny_x64_nir_load_value(c, in->b) ||
          !nyr_put(c->w, "\tmovq\t%rax, %rdx\n") ||
          !ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\tmovq\t%rax, %r11\n\tmovq\t%rdx, %rax\n"
                         "\tcmpq\t%r11, %rax\n\tcmovaq\t%r11, %rax\n"))
        return false;
      c->rax_cached_slot = -1;
      return ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    }
    if (in->imm == -10 || (in->imm >= -46 && in->imm <= -40)) {
      const char *placeholder = strstr(in->symbol, "%P1");
      if (in->a < 0 || in->b < 0 || in->dst < 0 || !placeholder ||
          strstr(placeholder + 3, "%P1")) {
        nyr_set_error(c->err, c->err_len,
                      "nyir x86-64: invalid read-write register/memory asm");
        return false;
      }
      c->rax_cached_slot = -1;
      if (!ny_x64_nir_load_value(c, in->b) ||
          !nyr_put(c->w, "\tmovq\t%rax, %rdx\n") ||
          !ny_x64_nir_load_value(c, in->a))
        return false;
      size_t prefix_len = (size_t)(placeholder - in->symbol);
      if (prefix_len > INT_MAX ||
          !nyr_printf(c->w, "%.*s(%%rax)%s\n", (int)prefix_len, in->symbol,
                      placeholder + 3))
        return false;
      const char *normalize = in->imm == -40   ? "\tmovzbq\t%dl, %rax\n"
                              : in->imm == -41 ? "\tmovsbq\t%dl, %rax\n"
                              : in->imm == -42 ? "\tmovzwq\t%dx, %rax\n"
                              : in->imm == -43 ? "\tmovswq\t%dx, %rax\n"
                              : in->imm == -44 ? "\tmovl\t%edx, %eax\n"
                              : in->imm == -45 ? "\tmovslq\t%edx, %rax\n"
                              : in->imm == -46 ? "\tmovq\t%rdx, %rax\n"
                                               : "\tmovslq\t%edx, %rax\n";
      if (normalize && !nyr_put(c->w, normalize))
        return false;
      c->rax_cached_slot = -1;
      return ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    }
    if (in->imm == -50) {
      static const char *const xadd_symbols[] = {
          "xaddq",     "xaddbu",    "xaddbs",           "xaddwu",
          "xaddws",    "xaddlu",    "xaddls",           "unlocked:xaddq",
          "xaddbu-gs", "xaddbs-gs", "xaddwu-gs",        "xaddws-gs",
          "xaddlu-gs", "xaddls-gs", "unlocked:gs:xaddq"};
      static const char *const xadd_templates[] = {
          "\tlock xaddq\t%rdx, (%rax)\n\tmovq\t%rdx, %rax\n",
          "\tlock xaddb\t%dl, (%rax)\n",
          "\tlock xaddb\t%dl, (%rax)\n",
          "\tlock xaddw\t%dx, (%rax)\n",
          "\tlock xaddw\t%dx, (%rax)\n",
          "\tlock xaddl\t%edx, (%rax)\n",
          "\tlock xaddl\t%edx, (%rax)\n",
          "\txaddq\t%rdx, (%rax)\n\tmovq\t%rdx, %rax\n",
          "\txaddb\t%dl, %gs:(%rax)\n",
          "\txaddb\t%dl, %gs:(%rax)\n",
          "\txaddw\t%dx, %gs:(%rax)\n",
          "\txaddw\t%dx, %gs:(%rax)\n",
          "\txaddl\t%edx, %gs:(%rax)\n",
          "\txaddl\t%edx, %gs:(%rax)\n",
          "\txaddq\t%rdx, %gs:(%rax)\n\tmovq\t%rdx, %rax\n"};
      unsigned variant = 15;
      for (unsigned i = 0; i < sizeof(xadd_symbols) / sizeof(xadd_symbols[0]);
           ++i)
        if (in->symbol && strcmp(in->symbol, xadd_symbols[i]) == 0) {
          variant = i;
          break;
        }
      if (in->dst < 0 || in->a < 0 || in->b < 0 || in->c != -1 ||
          variant == 15) {
        nyr_set_error(c->err, c->err_len, "nyir x86-64: invalid xadd operands");
        return false;
      }
      c->rax_cached_slot = -1;
      if (!ny_x64_nir_load_value(c, in->b) ||
          !nyr_put(c->w, "\tmovq\t%rax, %rdx\n") ||
          !ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, xadd_templates[variant]))
        return false;
      if ((variant == 1 || variant == 8) &&
          !nyr_put(c->w, "\tmovzbl\t%dl, %eax\n"))
        return false;
      if ((variant == 2 || variant == 9) &&
          !nyr_put(c->w, "\tmovsbq\t%dl, %rax\n"))
        return false;
      if ((variant == 3 || variant == 10) &&
          !nyr_put(c->w, "\tmovzwl\t%dx, %eax\n"))
        return false;
      if ((variant == 4 || variant == 11) &&
          !nyr_put(c->w, "\tmovswq\t%dx, %rax\n"))
        return false;
      if ((variant == 5 || variant == 12) &&
          !nyr_put(c->w, "\tmovl\t%edx, %eax\n"))
        return false;
      if ((variant == 6 || variant == 13) &&
          !nyr_put(c->w, "\tmovslq\t%edx, %rax\n"))
        return false;
      if (!ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst)))
        return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -11 || in->imm == -12 || in->imm == -155 ||
        in->imm == -145 || in->imm == -16) {
      /*
       * cmpxchg: b=expected, c=replacement, a=memory address, dst=old value.
       */
      if (in->a < 0 || in->b < 0 || in->c < 0 || in->dst < 0)
        return false;
      if (in->imm == -16) {
        if (!ny_x64_nir_load_value(c, in->b) ||
            !nyr_put(c->w, "\tmovq\t%rax, %r9\n") ||
            !ny_x64_nir_load_value(c, in->c) ||
            !nyr_put(c->w, "\tmovq\t%rax, %rdx\n") ||
            !ny_x64_nir_load_value(c, in->a) ||
            !nyr_put(c->w, "\tmovq\t%rax, %rcx\n") ||
            !nyr_put(c->w, "\tmovq\t%r9, %rax\n") ||
            !nyr_printf(c->w, "\t%s\n", in->symbol))
          return false;
      } else if (in->imm == -155) {
        if (!ny_x64_nir_load_value(c, in->b) ||
            !nyr_put(c->w, "\tmovl\t%eax, %r9d\n") ||
            !ny_x64_nir_load_value(c, in->c) ||
            !nyr_put(c->w, "\tmovb\t%al, %dl\n") ||
            !ny_x64_nir_load_value(c, in->a) ||
            !nyr_put(c->w, "\tmovq\t%rax, %rcx\n") ||
            !nyr_put(c->w, "\tmovl\t%r9d, %eax\n") ||
            !nyr_printf(c->w, "\t%s\n", in->symbol) ||
            !nyr_put(c->w, "\tmovl\t%eax, %eax\n"))
          return false;
      } else if (in->imm == -145) {
        if (!ny_x64_nir_load_value(c, in->b) ||
            !nyr_put(c->w, "\tmovl\t%eax, %r9d\n") ||
            !ny_x64_nir_load_value(c, in->c) ||
            !nyr_put(c->w, "\tmovw\t%ax, %dx\n") ||
            !ny_x64_nir_load_value(c, in->a) ||
            !nyr_put(c->w, "\tmovq\t%rax, %rcx\n") ||
            !nyr_put(c->w, "\tmovl\t%r9d, %eax\n") ||
            !nyr_printf(c->w, "\t%s\n", in->symbol) ||
            !nyr_put(c->w, "\tmovl\t%eax, %eax\n"))
          return false;
      } else if (!ny_x64_nir_load_value(c, in->b) ||
                 !nyr_put(c->w, "\tmovl\t%eax, %r9d\n") ||
                 !ny_x64_nir_load_value(c, in->c) ||
                 !nyr_put(c->w, "\tmovl\t%eax, %edx\n") ||
                 !ny_x64_nir_load_value(c, in->a) ||
                 !nyr_put(c->w, "\tmovq\t%rax, %rcx\n") ||
                 !nyr_put(c->w, "\tmovl\t%r9d, %eax\n") ||
                 !nyr_printf(c->w, "\t%s\n", in->symbol) ||
                 !nyr_put(c->w, in->imm == -11 ? "\tmovslq\t%eax, %rax\n"
                                               : "\tmovl\t%eax, %eax\n"))
        return false;
      c->rax_cached_slot = -1;
      return ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    }

    if (in->imm == -2 || in->imm == -3 || in->imm == -4 || in->imm == -8) {
      if (in->a < 0 || ((in->imm == -3 || in->imm == -8) && in->b < 0) ||
          ((in->imm == -4 || in->imm == -8) && in->dst < 0)) {
        nyr_set_error(c->err, c->err_len,
                      "nyir x86-64: invalid inline-asm memory output");
        return false;
      }
      if ((in->imm == -3 || in->imm == -8) &&
          (!ny_x64_nir_load_value(c, in->b) ||
           !nyr_printf(c->w, "\tmovq\t%%rax, %%rdx\n"))) {
        c->rax_cached_slot = -1;
        return false;
      }
      c->rax_cached_slot = -1;
      if (!ny_x64_nir_load_value(c, in->a) ||
          !nyr_printf(c->w, "\t%s\n", in->symbol)) {
        c->rax_cached_slot = -1;
        return false;
      }
      c->rax_cached_slot = -1;
      if (in->imm == -4 || in->imm == -8)
        return ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
      return true;
    }
    if (in->imm == -15 || in->imm == -16) {
      const char *expected =
          in->imm == -15 ? "lock incq (%rax)" : "lock decq (%rax)";
      const char *instruction = in->imm == -15 ? "incq" : "decq";
      if (in->dst != -1 || in->a < 0 || in->b != -1 || in->c != -1 ||
          !in->symbol || strcmp(in->symbol, expected) != 0) {
        nyr_set_error(c->err, c->err_len,
                      "nyir x86-64: invalid atomic counter asm operands");
        return false;
      }
      c->rax_cached_slot = -1;
      if (!ny_x64_nir_load_value(c, in->a) ||
          !nyr_printf(c->w, "\tlock %s\t(%%rax)\n", instruction))
        return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -33) {
      if (in->dst != -1 || in->a < 0 || in->b < 0 || in->c != -1 ||
          !in->symbol) {
        nyr_set_error(c->err, c->err_len,
                      "nyir x86-64: invalid atomic arithmetic/bitwise asm");
        return false;
      }
      bool is_byte = in->symbol[strlen(in->symbol) - 1] == 'b';
      bool is_word = in->symbol[strlen(in->symbol) - 1] == 'w';
      if (!is_byte && !is_word && strcmp(in->symbol, "andl") != 0 &&
          strcmp(in->symbol, "orl") != 0 && strcmp(in->symbol, "xorl") != 0 &&
          strcmp(in->symbol, "addl") != 0 && strcmp(in->symbol, "subl") != 0 &&
          strcmp(in->symbol, "btsl") != 0 && strcmp(in->symbol, "btrl") != 0 &&
          strcmp(in->symbol, "btcl") != 0) {
        nyr_set_error(c->err, c->err_len,
                      "nyir x86-64: invalid atomic arithmetic/bitwise asm");
        return false;
      }
      c->rax_cached_slot = -1;
      if (!ny_x64_nir_load_value(c, in->b) ||
          !nyr_put(c->w, is_byte   ? "\tmovb\t%al, %r11b\n"
                         : is_word ? "\tmovw\t%ax, %r11w\n"
                                   : "\tmovl\t%eax, %r11d\n") ||
          !ny_x64_nir_load_value(c, in->a) ||
          !nyr_printf(c->w, "\t%s%s\t%s, (%%rax)\n",
                      in->flags & NYIR_INST_F_ASM_UNLOCKED ? "" : "lock ",
                      in->symbol,
                      is_byte   ? "%r11b"
                      : is_word ? "%r11w"
                                : "%r11d"))
        return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -77) {
      if (in->dst != -1 || in->a < 0 || in->b < 0 || in->c != -1 ||
          !in->symbol ||
          (strcmp(in->symbol, "andq") != 0 && strcmp(in->symbol, "orq") != 0 &&
           strcmp(in->symbol, "xorq") != 0 && strcmp(in->symbol, "addq") != 0 &&
           strcmp(in->symbol, "subq") != 0)) {
        nyr_set_error(
            c->err, c->err_len,
            "nyir x86-64: invalid atomic 64-bit arithmetic/bitwise asm");
        return false;
      }
      c->rax_cached_slot = -1;
      if (!ny_x64_nir_load_value(c, in->b) ||
          !nyr_put(c->w, "\tmovq\t%rax, %r11\n") ||
          !ny_x64_nir_load_value(c, in->a) ||
          !nyr_printf(c->w, "\t%s%s\t%%r11, (%%rax)\n",
                      in->flags & NYIR_INST_F_ASM_UNLOCKED ? "" : "lock ",
                      in->symbol))
        return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -47) {
      bool bit_test_set = in->symbol && strcmp(in->symbol, "btsl") == 0;
      bool bit_test = in->symbol && (strcmp(in->symbol, "btl") == 0 ||
                                     strcmp(in->symbol, "btq") == 0);
      bool bit_test_nz = in->symbol && (strcmp(in->symbol, "testb_nz") == 0);
      if (in->dst < 0 || in->a < 0 || in->b < 0 || in->c != -1 || !in->symbol ||
          (!bit_test_set && !bit_test && !bit_test_nz &&
           strcmp(in->symbol, "xorb") != 0)) {
        nyr_set_error(c->err, c->err_len,
                      "nyir x86-64: invalid atomic bitwise condition operands");
        return false;
      }
      c->rax_cached_slot = -1;
      if (bit_test_set) {
        if (!ny_x64_nir_load_value(c, in->b) ||
            !nyr_put(c->w, "\tmovl\t%eax, %ecx\n") ||
            !ny_x64_nir_load_value(c, in->a) ||
            !nyr_put(c->w, "\tlock btsl\t%ecx, (%rax)\n"
                           "\tsetc\t%al\n\tmovzbl\t%al, %eax\n"))
          return false;
      } else if (bit_test) {
        bool q = strcmp(in->symbol, "btq") == 0;
        if (!ny_x64_nir_load_value(c, in->b) ||
            !nyr_put(c->w,
                     q ? "\tmovq\t%rax, %rcx\n" : "\tmovl\t%eax, %ecx\n") ||
            !ny_x64_nir_load_value(c, in->a) ||
            !nyr_printf(c->w,
                        q ? "\tbtq\t%%rcx, "
                            "(%%rax)\n\tsetc\t%%al\n\tmovzbl\t%%al, %%eax\n"
                          : "\tbtl\t%%ecx, "
                            "(%%rax)\n\tsetc\t%%al\n\tmovzbl\t%%al, %%eax\n"))
          return false;
      } else if (bit_test_nz) {
        if (!ny_x64_nir_load_value(c, in->b) ||
            !nyr_put(c->w, "\tmovl\t%eax, %ecx\n") ||
            !ny_x64_nir_load_value(c, in->a) ||
            !nyr_put(
                c->w,
                "\ttestb\t%cl, (%rax)\n\tsetnz\t%al\n\tmovzbl\t%al, %eax\n"))
          return false;
      } else if (!ny_x64_nir_load_value(c, in->b) ||
                 !nyr_put(c->w, "\tmovl\t%eax, %r11d\n") ||
                 !ny_x64_nir_load_value(c, in->a) ||
                 !nyr_put(c->w, "\tlock xorb\t%r11b, (%rax)\n"
                                "\tsets\t%al\n\tmovzbl\t%al, %eax\n"))
        return false;
      if (!ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst)))
        return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -21 || in->imm == -22 || in->imm == -23) {
      const char *expected_symbol =
          in->imm == -23 ? "unsigned-shift-right" : "unsigned-div";
      if (in->dst < 0 || in->a < 0 || in->b < 0 || in->c != -1 || in->d != -1 ||
          !in->symbol || strcmp(in->symbol, expected_symbol) != 0) {
        nyr_set_error(c->err, c->err_len,
                      "nyir x86-64: invalid unsigned arithmetic operands");
        return false;
      }
      c->rax_cached_slot = -1;
      if (in->imm == -23) {
        if (!ny_x64_nir_load_value(c, in->b) ||
            !nyr_put(c->w, "\tmovq\t%rax, %rcx\n") ||
            !ny_x64_nir_load_value(c, in->a) ||
            !nyr_put(c->w, "\tshrq\t%cl, %rax\n"))
          return false;
      } else if (!ny_x64_nir_load_value(c, in->b) ||
                 !nyr_put(c->w, "\tmovq\t%rax, %rcx\n") ||
                 !ny_x64_nir_load_value(c, in->a) ||
                 !nyr_put(c->w, "\txorq\t%rdx, %rdx\n\tdivq\t%rcx\n") ||
                 (in->imm == -22 && !nyr_put(c->w, "\tmovq\t%rdx, %rax\n"))) {
        return false;
      }
      c->rax_cached_slot = -1;
      return ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    }
    if (in->imm == -20) {
      if (in->dst < 0 || in->a < 0 || in->b < 0 || in->c < 0 || in->d < -1 ||
          !in->symbol || strcmp(in->symbol, "syscall") != 0) {
        nyr_set_error(c->err, c->err_len,
                      "nyir x86-64: invalid syscall asm operands");
        return false;
      }
      c->rax_cached_slot = -1;
      if ((in->d >= 0 && (!ny_x64_nir_load_value(c, in->d) ||
                          !nyr_put(c->w, "\tmovq\t%rax, %rdx\n"))) ||
          !ny_x64_nir_load_value(c, in->c) ||
          !nyr_put(c->w, "\tmovq\t%rax, %rsi\n") ||
          !ny_x64_nir_load_value(c, in->b) ||
          !nyr_put(c->w, "\tmovq\t%rax, %rdi\n") ||
          !ny_x64_nir_load_value(c, in->a) || !nyr_put(c->w, "\tsyscall\n"))
        return false;
      c->rax_cached_slot = -1;
      return ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    }
    if (in->imm == -19) {
      if (in->dst < 0 || in->a < 0 || in->b != -1 || in->c != -1 ||
          in->d != -1 || !in->symbol || strcmp(in->symbol, "lsl") != 0) {
        nyr_set_error(c->err, c->err_len,
                      "nyir x86-64: invalid LSL asm operands");
        return false;
      }
      c->rax_cached_slot = -1;
      if (!ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\tlsl\t%ax, %eax\n"))
        return false;
      return ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    }
    if (in->imm == -17 || in->imm == -18) {
      if (in->dst < 0 || in->a != -1 || in->b != -1 || in->c != -1 ||
          in->d != -1 || !in->symbol || strcmp(in->symbol, "rdtsc") != 0) {
        nyr_set_error(c->err, c->err_len,
                      "nyir x86-64: invalid RDTSC asm operands");
        return false;
      }
      c->rax_cached_slot = -1;
      if ((in->imm == -18 && !nyr_put(c->w, "\tlfence\n")) ||
          !nyr_put(c->w, "\trdtsc\n\tshlq\t$32, %rdx\n\torq\t%rdx, %rax\n"))
        return false;
      return ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    }
    if (in->imm == -107) {
      if (in->dst < 0 || in->a < 0 || in->b != -1 || in->c != -1 ||
          in->d != -1 || in->e != -1 || in->f != -1 || !in->symbol ||
          strcmp(in->symbol, "rdpkru") != 0) {
        nyr_set_error(c->err, c->err_len,
                      "nyir x86-64: invalid RDPKRU asm operands");
        return false;
      }
      c->rax_cached_slot = -1;
      if (!ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\tmovl\t%eax, %ecx\n\trdpkru\n") ||
          !ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst)))
        return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -109) {
      if (in->dst != -1 || in->a < 0 || in->b != -1 || in->c != -1 ||
          in->d != -1 || in->e != -1 || in->f != -1 || !in->symbol ||
          (strcmp(in->symbol, "wrfsbase") != 0 &&
           strcmp(in->symbol, "wrgsbase") != 0)) {
        nyr_set_error(c->err, c->err_len,
                      "nyir x86-64: invalid WRFSBASE asm operands");
        return false;
      }
      c->rax_cached_slot = -1;
      if (!ny_x64_nir_load_value(c, in->a) ||
          !nyr_printf(c->w, "\t%s\t%%rax\n", in->symbol))
        return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -110) {
      if (in->dst < 0 || in->a < 0 || in->b != -1 || in->c != -1 ||
          in->d != -1 || in->e != -1 || in->f != -1 || !in->symbol ||
          strcmp(in->symbol, "lar-valid-ss") != 0) {
        nyr_set_error(c->err, c->err_len,
                      "nyir x86-64: invalid LAR validity operands");
        return false;
      }
      c->rax_cached_slot = -1;
      if (!ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\tlar\t%ax, %ecx\n\tjz\t1f\n"
                         "\txorl\t%ecx, %ecx\n1:\n") ||
          !ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst)))
        return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -5) {
      c->rax_cached_slot = -1;
      if (in->symbol && strcmp(in->symbol, "outb %b0, %w1") == 0) {
        if (in->a < 0 || in->b >= 0 || in->c < 0 ||
            !ny_x64_nir_load_value(c, in->a) ||
            !nyr_put(c->w, "\tmovq\t%rax, %r11\n") ||
            !ny_x64_nir_load_value(c, in->c) ||
            !nyr_put(c->w, "\tmovw\t%ax, %dx\n\tmovq\t%r11, %rax\n"
                           "\toutb\t%al, %dx\n"))
          return false;
        c->rax_cached_slot = -1;
        return true;
      }
      if (in->b >= 0 && (!ny_x64_nir_load_value(c, in->b) ||
                         !nyr_printf(c->w, "\tmovq\t%%rax, %%rcx\n")))
        return false;
      if (in->c >= 0 && (!ny_x64_nir_load_value(c, in->c) ||
                         !nyr_printf(c->w, "\tmovq\t%%rax, %%rdx\n")))
        return false;
      if (in->a >= 0 && !ny_x64_nir_load_value(c, in->a)) {
        c->rax_cached_slot = -1;
        return false;
      }
      if (!nyr_printf(c->w, "\t%s\n", in->symbol)) {
        c->rax_cached_slot = -1;
        return false;
      }
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm >= -57 && in->imm <= -52) {
      c->rax_cached_slot = -1;
      if (in->imm >= -54) {
        if (in->dst != -1 || in->a < 0 || in->b != -1 || in->c < 0 ||
            !ny_x64_nir_load_value(c, in->a) ||
            !nyr_put(c->w, "\tmovq\t%rax, %r11\n") ||
            !ny_x64_nir_load_value(c, in->c) ||
            !nyr_put(c->w, "\tmovw\t%ax, %dx\n\tmovq\t%r11, %rax\n"))
          return false;
        const char *instruction = in->imm == -52   ? "outb\t%al, %dx\n"
                                  : in->imm == -53 ? "outw\t%ax, %dx\n"
                                                   : "outl\t%eax, %dx\n";
        bool ok = nyr_printf(c->w, "\t%s", instruction);
        c->rax_cached_slot = -1;
        return ok;
      }
      if (in->dst < 0 || in->a < 0 || in->b != -1 || in->c != -1 ||
          !ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\tmovw\t%ax, %dx\n"))
        return false;
      if (in->imm == -55) {
        if (!nyr_put(c->w, "\tinb\t%dx, %al\n\tmovzbl\t%al, %eax\n"))
          return false;
      } else if (in->imm == -56) {
        if (!nyr_put(c->w, "\tinw\t%dx, %ax\n\tmovzwl\t%ax, %eax\n"))
          return false;
      } else if (!nyr_put(c->w, "\tinl\t%dx, %eax\n\tmovl\t%eax, %eax\n")) {
        return false;
      }
      c->rax_cached_slot = -1;
      return ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    }
    if (in->imm == -58) {
      if (in->dst < 0 || in->a < 0 || in->b != -1 || in->c != -1 ||
          !ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\tmovl\t(%rax), %eax\n"))
        return false;
      c->rax_cached_slot = -1;
      return ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    }
    if (in->imm == -101) {
      if (in->dst < 0 || in->a < 0 || in->b != -1 || in->c != -1 ||
          !in->symbol ||
          (strcmp(in->symbol, "gs_movl") != 0 &&
           strcmp(in->symbol, "gs_movq") != 0 &&
           strcmp(in->symbol, "gs_movb") != 0 &&
           strcmp(in->symbol, "gs_movw") != 0) ||
          !ny_x64_nir_load_value(c, in->a) ||
          !(strcmp(in->symbol, "gs_movq") == 0
                ? nyr_put(c->w, "\tmovq\t%gs:(%rax), %rax\n")
            : strcmp(in->symbol, "gs_movw") == 0
                ? nyr_put(c->w, "\tmovzwq\t%gs:(%rax), %rax\n")
            : strcmp(in->symbol, "gs_movb") == 0
                ? nyr_put(c->w, "\tmovzbl\t%gs:(%rax), %eax\n")
                : nyr_put(c->w, "\tmovl\t%gs:(%rax), %eax\n")))
        return false;
      c->rax_cached_slot = -1;
      return ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    }
    if (in->imm == -102) {
      if (in->dst != -1 || in->a < 0 || in->b != -1 || in->c != -1 ||
          in->d != -1 || in->e != -1 || in->f != -1 || !in->symbol ||
          strcmp(in->symbol, "reload_ds_ss_es") != 0 ||
          !ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\tmovw\t%ax, %ds\n"
                         "\tmovw\t%ax, %ss\n"
                         "\tmovw\t%ax, %es\n"))
        return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -59) {
      if (in->dst != -1 || in->a != -1 || in->b != -1 || in->c != -1 ||
          !in->symbol || strcmp(in->symbol, "iret_to_self") != 0 ||
          !nyr_put(c->w, "\tmovw\t%ss, %ax\n"
                         "\tpushq\t%rax\n"
                         "\tpushq\t%rsp\n"
                         "\taddq\t$8, (%rsp)\n"
                         "\tpushfq\n"
                         "\tmovw\t%cs, %ax\n"
                         "\tpushq\t%rax\n"
                         "\tpushq\t$1f\n"
                         "\tiretq\n"
                         "1:\n"))
        return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm >= -64 && in->imm <= -60) {
      unsigned control_register = (unsigned)(-60 - in->imm);
      char expected_symbol[16];
      snprintf(expected_symbol, sizeof(expected_symbol), "cr%u",
               control_register);
      if (in->dst != -1 || in->a < 0 || in->b != -1 || in->c != -1 ||
          !in->symbol || strcmp(in->symbol, expected_symbol) != 0 ||
          !ny_x64_nir_load_value(c, in->a) ||
          !nyr_printf(c->w, "\tmovq\t%%rax, %%%s\n", expected_symbol))
        return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm >= -69 && in->imm <= -65) {
      unsigned control_register = (unsigned)(-65 - in->imm);
      char expected_symbol[16];
      snprintf(expected_symbol, sizeof(expected_symbol), "cr%u",
               control_register);
      if (in->dst < 0 || in->a != -1 || in->b != -1 || in->c != -1 ||
          !in->symbol || strcmp(in->symbol, expected_symbol) != 0 ||
          !nyr_printf(c->w, "\tmovq\t%%%s, %%rax\n", expected_symbol))
        return false;
      c->rax_cached_slot = -1;
      return ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    }
    if (in->imm == -83) {
      const char *instruction =
          in->symbol && strcmp(in->symbol, "b") == 0   ? "movzbl"
          : in->symbol && strcmp(in->symbol, "w") == 0 ? "movzwl"
          : in->symbol && strcmp(in->symbol, "l") == 0 ? "movl"
          : in->symbol && strcmp(in->symbol, "q") == 0 ? "movq"
                                                       : NULL;
      const char *destination =
          in->symbol && strcmp(in->symbol, "q") == 0 ? "%rax" : "%eax";
      if (in->dst < 0 || in->a < 0 || in->b != -1 || in->c < 0 ||
          !instruction || !ny_x64_nir_load_value(c, in->a) ||
          !nyr_printf(c->w,
                      "1:\t%s\t(%%rax), %s\n"
                      "2:\n"
                      "\t.pushsection __ex_table,\"aM\",@progbits,12\n"
                      "\t.balign 4\n"
                      "\t.long 1b - .\n"
                      "\t.long .Lnyir_%s_L%d - .\n"
                      "\t.long 3\n"
                      "\t.popsection\n",
                      instruction, destination, c->label_prefix, in->c))
        return false;
      c->rax_cached_slot = -1;
      return ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    }
    if (in->imm == -84) {
      int error_slot = -1;
      int consumed = -1;
      const char *load = in->symbol && in->symbol[0] == 'b'   ? "movb"
                         : in->symbol && in->symbol[0] == 'w' ? "movw"
                         : in->symbol && in->symbol[0] == 'l' ? "movl"
                         : in->symbol && in->symbol[0] == 'q' ? "movq"
                                                              : NULL;
      const char *result = in->symbol && in->symbol[0] == 'b'   ? "%al"
                           : in->symbol && in->symbol[0] == 'w' ? "%ax"
                           : in->symbol && in->symbol[0] == 'l' ? "%eax"
                                                                : "%rax";
      const char *extend =
          in->symbol && in->symbol[0] == 'b'   ? "\tmovzbl\t%al, %eax\n"
          : in->symbol && in->symbol[0] == 'w' ? "\tmovzwl\t%ax, %eax\n"
                                               : NULL;
      if (in->symbol)
        sscanf(in->symbol, "%*1[bwlq];nyc_slot=%d%n", &error_slot, &consumed);
      if (in->dst < 0 || in->a < 0 || in->b < 0 || in->c < 0 || !load ||
          error_slot < 0 || error_slot >= c->max_local_slot || consumed < 0 ||
          in->symbol[consumed] != '\0' || !ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\tmovq\t%rax, %r11\n") ||
          !ny_x64_nir_load_value(c, in->b) ||
          !nyr_put(c->w, "\tmovq\t%rax, %r10\n"
                         "1:\t") ||
          !nyr_printf(c->w, "%s\t(%%r11), %s\n", load, result) ||
          !nyr_put(c->w, "2:\n"
                         "\t.pushsection __ex_table,\"aM\",@progbits,12\n"
                         "\t.balign 4\n"
                         "\t.long 1b - .\n"
                         "\t.long 2b - .\n"
                         "\t.long 0xfff21a11\n"
                         "\t.popsection\n") ||
          (extend && !nyr_put(c->w, extend)) ||
          !ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst)) ||
          !ny_x64_nir_load_value(c, in->c) ||
          !nyr_put(c->w, "\tmovq\t%rax, %r11\n"
                         "\tmovq\t%r10, (%r11)\n"))
        return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -162) {
      if (in->dst < 0 || in->a < 0 || in->b < 0 || in->c < 0 || in->d < 0 ||
          in->e < 0 || in->f != -1 || !in->symbol ||
          strcmp(in->symbol, "futex_cmpxchg32_extable") != 0 ||
          !ny_x64_nir_load_value(c, in->d) ||
          !nyr_put(c->w, "\tmovl\t%eax, %r10d\n") ||
          !ny_x64_nir_load_value(c, in->b) ||
          !nyr_put(c->w, "\tmovl\t%eax, %r9d\n") ||
          !ny_x64_nir_load_value(c, in->c) ||
          !nyr_put(c->w, "\tmovl\t%eax, %r11d\n") ||
          !ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\tmovq\t%rax, %rcx\n"
                         "\tmovl\t%r9d, %eax\n"
                         "1:\tlock cmpxchgl %r11d, (%rcx)\n"
                         "2:\n"
                         "\t.pushsection __ex_table,\"aM\",@progbits,12\n"
                         "\t.balign 4\n"
                         "\t.long 1b - .\n"
                         "\t.long 2b - .\n"
                         "\t.long 0xfff21a11\n"
                         "\t.popsection\n") ||
          !ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst)) ||
          !ny_x64_nir_load_value(c, in->e) ||
          !nyr_put(c->w, "\tmovl\t%r10d, (%rax)\n"))
        return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -163) {
      if (in->dst < 0 || in->a < 0 || in->b != -1 || in->c != -1 ||
          in->d != -1 || in->e != -1 || in->f != -1 || !in->symbol ||
          strcmp(in->symbol, "runtime_mask___futex_mask") != 0 ||
          !ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\tandl\t$0x12345678, %eax\n"
                         "1:\n"
                         "\t.pushsection runtime_mask___futex_mask,\"a\"\n"
                         "\t.long 1b - 4 - .\n"
                         "\t.popsection\n") ||
          !ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst)))
        return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -164) {
      if (in->dst < 0 || in->a < 0 || in->b < 0 || in->c < 0 || in->d < 0 ||
          in->e != -1 || in->f != -1 || !in->symbol ||
          strcmp(in->symbol, "futex_xchg32_extable") != 0 ||
          !ny_x64_nir_load_value(c, in->c) ||
          !nyr_put(c->w, "\tmovl\t%eax, %r10d\n") ||
          !ny_x64_nir_load_value(c, in->b) ||
          !nyr_put(c->w, "\tmovl\t%eax, %r11d\n") ||
          !ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\tmovq\t%rax, %rcx\n"
                         "1:\txchgl\t%r11d, (%rcx)\n"
                         "2:\n"
                         "\t.pushsection __ex_table,\"aM\",@progbits,12\n"
                         "\t.balign 4\n"
                         "\t.long 1b - .\n"
                         "\t.long 2b - .\n"
                         "\t.long 0xfff21a11\n"
                         "\t.popsection\n"
                         "\tmovl\t%r11d, %eax\n") ||
          !ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst)) ||
          !ny_x64_nir_load_value(c, in->d) ||
          !nyr_put(c->w, "\tmovl\t%r10d, (%rax)\n"))
        return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -165) {
      if (in->dst < 0 || in->a < 0 || in->b < 0 || in->c < 0 || in->d < 0 ||
          in->e != -1 || in->f != -1 || !in->symbol ||
          strcmp(in->symbol, "futex_xadd32_extable") != 0 ||
          !ny_x64_nir_load_value(c, in->c) ||
          !nyr_put(c->w, "\tmovl\t%eax, %r10d\n") ||
          !ny_x64_nir_load_value(c, in->b) ||
          !nyr_put(c->w, "\tmovl\t%eax, %r11d\n") ||
          !ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\tmovq\t%rax, %rcx\n"
                         "1:\tlock xaddl\t%r11d, (%rcx)\n"
                         "2:\n"
                         "\t.pushsection __ex_table,\"aM\",@progbits,12\n"
                         "\t.balign 4\n"
                         "\t.long 1b - .\n"
                         "\t.long 2b - .\n"
                         "\t.long 0xfff21a11\n"
                         "\t.popsection\n"
                         "\tmovl\t%r11d, %eax\n") ||
          !ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst)) ||
          !ny_x64_nir_load_value(c, in->d) ||
          !nyr_put(c->w, "\tmovl\t%r10d, (%rax)\n"))
        return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -166) {
      const char *operation =
          in->symbol && strcmp(in->symbol, "or") == 0    ? "orl"
          : in->symbol && strcmp(in->symbol, "and") == 0 ? "andl"
          : in->symbol && strcmp(in->symbol, "xor") == 0 ? "xorl"
                                                         : NULL;
      if (in->dst < 0 || in->a < 0 || in->b < 0 || in->c < 0 || in->d < 0 ||
          in->e != -1 || in->f != -1 || !operation ||
          !ny_x64_nir_load_value(c, in->c) ||
          !nyr_put(c->w, "\tmovl\t%eax, %r10d\n") ||
          !ny_x64_nir_load_value(c, in->b) ||
          !nyr_put(c->w, "\tmovl\t%eax, %r8d\n") ||
          !ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\tmovq\t%rax, %rcx\n"
                         "1:\tmovl\t(%rcx), %eax\n"
                         "2:\tmovl\t%eax, %r9d\n") ||
          !nyr_printf(c->w, "\t%s\t%%r8d, %%r9d\n", operation) ||
          !nyr_put(c->w, "3:\tlock cmpxchgl\t%r9d, (%rcx)\n"
                         "\tjnz\t2b\n"
                         "4:\n"
                         "\t.pushsection __ex_table,\"aM\",@progbits,12\n"
                         "\t.balign 4\n"
                         "\t.long 1b - .\n"
                         "\t.long 4b - .\n"
                         "\t.long 0xfff21a11\n"
                         "\t.long 3b - .\n"
                         "\t.long 4b - .\n"
                         "\t.long 0xfff21a11\n"
                         "\t.popsection\n") ||
          !ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst)) ||
          !ny_x64_nir_load_value(c, in->d) ||
          !nyr_put(c->w, "\tmovl\t%r10d, (%rax)\n"))
        return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -82) {
      const char *source_register =
          in->symbol && strcmp(in->symbol, "b") == 0   ? "%r11b"
          : in->symbol && strcmp(in->symbol, "w") == 0 ? "%r11w"
          : in->symbol && strcmp(in->symbol, "l") == 0 ? "%r11d"
          : in->symbol && strcmp(in->symbol, "q") == 0 ? "%r11"
                                                       : NULL;
      if (in->dst != -1 || in->a < 0 || in->b < 0 || in->c < 0 ||
          !source_register || !ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\tmovq\t%rax, %r11\n") ||
          !ny_x64_nir_load_value(c, in->b) ||
          !nyr_put(c->w, "\tmovq\t%rax, %rcx\n") ||
          !nyr_printf(c->w,
                      "1:\tmov%s\t%s, (%%rcx)\n"
                      "2:\n"
                      "\t.pushsection __ex_table,\"aM\",@progbits,12\n"
                      "\t.balign 4\n"
                      "\t.long 1b - .\n"
                      "\t.long .Lnyir_%s_L%d - .\n"
                      "\t.long 3\n"
                      "\t.popsection\n",
                      in->symbol, source_register, c->label_prefix, in->c))
        return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -81) {
      bool zero_condition = in->symbol && strstr(in->symbol, "_zero") != NULL;
      bool unlocked = in->symbol && strstr(in->symbol, "_unlocked") != NULL;
      const char *mnemonic = in->symbol;
      if (zero_condition) {
        static const char *const zero_names[] = {
            "addl_zero",          "subl_zero",          "addq_zero",
            "subq_zero",          "addl_unlocked_zero", "subl_unlocked_zero",
            "addq_unlocked_zero", "subq_unlocked_zero"};
        static const char *const base_names[] = {
            "addl", "subl", "addq", "subq", "addl", "subl", "addq", "subq"};
        for (size_t i = 0; i < sizeof(zero_names) / sizeof(zero_names[0]); ++i)
          if (strcmp(in->symbol, zero_names[i]) == 0)
            mnemonic = base_names[i];
      } else if (unlocked) {
        static const char *const unlocked_names[] = {
            "addl_unlocked", "subl_unlocked", "addq_unlocked", "subq_unlocked"};
        static const char *const base_names[] = {"addl", "subl", "addq",
                                                 "subq"};
        for (size_t i = 0;
             i < sizeof(unlocked_names) / sizeof(unlocked_names[0]); ++i)
          if (strcmp(in->symbol, unlocked_names[i]) == 0)
            mnemonic = base_names[i];
      }
      bool width64 = in->symbol && (strcmp(mnemonic, "addq") == 0 ||
                                    strcmp(mnemonic, "subq") == 0);
      if (in->dst < 0 || in->a < 0 || in->b < 0 || in->c != -1 || !in->symbol ||
          (strcmp(mnemonic, "addl") != 0 && strcmp(mnemonic, "subl") != 0 &&
           strcmp(mnemonic, "addq") != 0 && strcmp(mnemonic, "subq") != 0) ||
          !ny_x64_nir_load_value(c, in->b) ||
          !(width64 ? nyr_put(c->w, "\tmovq\t%rax, %r11\n")
                    : nyr_put(c->w, "\tmovl\t%eax, %r11d\n")) ||
          !ny_x64_nir_load_value(c, in->a) ||
          !(width64 ? nyr_printf(c->w,
                                 "\t%s%s\t%%r11, (%%rax)\n"
                                 "\tset%s\t%%r10b\n\tmovzbl\t%%r10b, %%eax\n",
                                 unlocked ? "" : "lock ", mnemonic,
                                 zero_condition ? "z" : "s")
                    : nyr_printf(c->w,
                                 "\t%s%s\t%%r11d, (%%rax)\n"
                                 "\tset%s\t%%r10b\n\tmovzbl\t%%r10b, %%eax\n",
                                 unlocked ? "" : "lock ", mnemonic,
                                 zero_condition ? "z" : "s")))
        return false;
      c->rax_cached_slot = -1;
      return ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    }
    if (in->imm == -80) {
      static const char *const segments[] = {"ds", "es", "ss",
                                             "fs", "gs", "cs"};
      bool known_segment = false;
      if (in->symbol)
        for (size_t i = 0; i < sizeof(segments) / sizeof(segments[0]); ++i)
          known_segment |= strcmp(in->symbol, segments[i]) == 0;
      if (in->dst < 0 || in->a != -1 || in->b != -1 || in->c != -1 ||
          !known_segment || !nyr_put(c->w, "\txorl\t%eax, %eax\n") ||
          !nyr_printf(c->w, "\tmovw\t%%%s, %%ax\n", in->symbol))
        return false;
      c->rax_cached_slot = -1;
      return ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    }
    if (in->imm == -114) {
      if (in->dst != -1 || in->a < 0 || in->b != -1 || in->c != -1 ||
          !in->symbol ||
          (strcmp(in->symbol, "verw") != 0 &&
           strcmp(in->symbol, "verw_mem") != 0) ||
          !ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, strcmp(in->symbol, "verw_mem") == 0
                             ? "\tverw\t(%rax)\n"
                             : "\tverw\t%ax\n"))
        return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -79) {
      static const char *const segments[] = {"ds", "es", "ss", "fs", "gs"};
      bool known_segment = false;
      if (in->symbol)
        for (size_t i = 0; i < sizeof(segments) / sizeof(segments[0]); ++i)
          known_segment |= strcmp(in->symbol, segments[i]) == 0;
      if (in->dst != -1 || in->a < 0 || in->b != -1 || in->c != -1 ||
          !known_segment || !ny_x64_nir_load_value(c, in->a) ||
          !nyr_printf(c->w, "\tmovw\t%%ax, %%%s\n", in->symbol))
        return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -70) {
      if (in->dst != -1 || in->a < 0 || in->b != -1 || in->c != -1 ||
          !in->symbol || strcmp(in->symbol, "load_fs_clear") != 0 ||
          !ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "1:\tmovw\t%ax, %fs\n"
                         "2:\n"
                         ".pushsection __ex_table, \"aM\", @progbits, 12\n"
                         ".balign 4\n"
                         ".long (1b) - .\n"
                         ".long (2b) - .\n"
                         ".long 5\n"
                         ".popsection\n"))
        return false;
      /*
       * Label 1 must immediately precede the faulting instruction.
       */
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm >= -74 && in->imm <= -71) {
      static const char *const names[] = {"sgdt %0", "sidt %0", "lgdt %0",
                                          "lidt %0"};
      static const char *const instructions[] = {"sgdt", "sidt", "lgdt",
                                                 "lidt"};
      unsigned index = (unsigned)(-71 - in->imm);
      if (in->dst != -1 || in->a < 0 || in->b != -1 || in->c != -1 ||
          !in->symbol || strcmp(in->symbol, names[index]) != 0 ||
          !ny_x64_nir_load_value(c, in->a) ||
          !nyr_printf(c->w, "\t%s\t(%%rax)\n", instructions[index]))
        return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -152) {
      if (in->dst != -1 || in->a < 0 || in->b != -1 || in->c != -1 ||
          !in->symbol || strcmp(in->symbol, "sldt %0") != 0 ||
          !ny_x64_nir_load_value(c, in->a) ||
          !nyr_put(c->w, "\tsldt\t(%rax)\n"))
        return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -75 || in->imm == -76) {
      const char *expected = in->imm == -75 ? "ltr" : "lldt";
      if (in->dst != -1 || in->a < 0 || in->b != -1 || in->c != -1 ||
          !in->symbol || strcmp(in->symbol, expected) != 0 ||
          !ny_x64_nir_load_value(c, in->a) ||
          !nyr_printf(c->w, "\t%s\t%%ax\n", expected))
        return false;
      c->rax_cached_slot = -1;
      return true;
    }
    if (in->imm == -9) {
      const char *placeholder = strstr(in->symbol, "%P1");
      if (in->a < 0 || in->dst < 0 || !placeholder ||
          strstr(placeholder + 3, "%P1")) {
        nyr_set_error(c->err, c->err_len,
                      "nyir x86-64: invalid register-output memory input");
        return false;
      }
      c->rax_cached_slot = -1;
      if (!ny_x64_nir_load_value(c, in->a))
        return false;
      size_t prefix_len = (size_t)(placeholder - in->symbol);
      if (prefix_len > INT_MAX ||
          !nyr_printf(c->w, "%.*s(%%rax)%s\n", (int)prefix_len, in->symbol,
                      placeholder + 3)) {
        c->rax_cached_slot = -1;
        return false;
      }
      c->rax_cached_slot = -1;
      return ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    }
    if (in->imm == -6 || in->imm == -7) {
      const char *placeholder = strstr(in->symbol, "%P1");
      if (in->a < 0 || in->b < 0 || !placeholder ||
          strstr(placeholder + 3, "%P1")) {
        nyr_set_error(c->err, c->err_len,
                      "nyir x86-64: invalid inline-asm memory/register input");
        return false;
      }
      c->rax_cached_slot = -1;
      if (!ny_x64_nir_load_value(c, in->b) ||
          !nyr_printf(c->w, "\tmovq\t%%rax, %%rdx\n") ||
          !ny_x64_nir_load_value(c, in->a))
        return false;
      size_t prefix_len = (size_t)(placeholder - in->symbol);
      if (!nyr_printf(c->w, "%.*s(%%rax)%s\n", (int)prefix_len, in->symbol,
                      placeholder + 3))
        return false;
      c->rax_cached_slot = -1;
      if (in->imm == -7)
        return ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
      return true;
    }
    if (in->imm == -180) {
      /*
       * CRC32 with a register-or-memory input. The frontend keeps both the
       * read-write 32-bit accumulator (a) and the input operand (b) as
       * values, so materialize them in scratch registers instead of relying
       * on a %P1 memory placeholder.
       */
      const char *src = NULL;
      const char *dst = NULL;
      const char *from = NULL;
      const char *move = NULL;
      if (in->symbol && strcmp(in->symbol, "crc32q") == 0) {
        src = "%rdx";
        dst = "%rax";
        from = "%rax";
        move = "movq";
      } else if (in->symbol && strcmp(in->symbol, "crc32l") == 0) {
        src = "%edx";
        dst = "%eax";
        from = "%eax";
        move = "movl";
      } else if (in->symbol && strcmp(in->symbol, "crc32w") == 0) {
        src = "%dx";
        dst = "%eax";
        from = "%ax";
        move = "movw";
      } else if (in->symbol && strcmp(in->symbol, "crc32b") == 0) {
        src = "%dl";
        dst = "%eax";
        from = "%al";
        move = "movb";
      }
      if (!src || in->dst < 0 || in->a < 0 || in->b < 0 || in->c != -1 ||
          in->d != -1) {
        nyr_set_error(c->err, c->err_len,
                      "nyir x86-64: invalid inline-asm crc32 operands");
        return false;
      }
      c->rax_cached_slot = -1;
      if (!ny_x64_nir_load_value(c, in->b) ||
          !nyr_printf(c->w, "\t%s\t%s, %s\n", move, from, src) ||
          !ny_x64_nir_load_value(c, in->a) ||
          !nyr_printf(c->w, "\t%s\t%s, %s\n", in->symbol, src, dst))
        return false;
      c->rax_cached_slot = -1;
      return ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    }
    if (in->a >= 0) {
      const char *placeholder = strstr(in->symbol, "%P1");
      if (in->imm > 0 || !placeholder) {
        nyr_set_error(c->err, c->err_len,
                      "nyir x86-64: invalid inline-asm memory input");
        return false;
      }
      if (!ny_x64_nir_load_value(c, in->a))
        return false;
      const char *cursor = in->symbol;
      do {
        size_t prefix_len = (size_t)(placeholder - cursor);
        if (prefix_len > INT_MAX ||
            !nyr_printf(c->w, "%.*s(%%rax)", (int)prefix_len, cursor))
          return false;
        cursor = placeholder + 3;
        placeholder = strstr(cursor, "%P1");
      } while (placeholder);
      return nyr_printf(c->w, "%s\n", cursor);
    }
    if (in->imm > 0) {
      int local = (int)(in->imm - 1);
      char address[64];
      const char *placeholder = strstr(in->symbol, "%0");
      if (local < 0 || local >= c->max_local_slot || !placeholder ||
          strstr(placeholder + 2, "%0")) {
        nyr_set_error(c->err, c->err_len,
                      "nyir x86-64: invalid inline-asm local output");
        return false;
      }
      c->slot_offset[local] = (local + 1 + c->stack_guard) * 8;
      ny_x64_nir_slot_str(c, local, address, sizeof(address));
      if (!address[0] || address[0] == '%') {
        nyr_set_error(c->err, c->err_len,
                      "nyir x86-64: inline-asm output is not in a memory slot");
        return false;
      }
      size_t prefix_len = (size_t)(placeholder - in->symbol);
      if (prefix_len > INT_MAX ||
          !nyr_printf(c->w, "%.*s%s%s\n", (int)prefix_len, in->symbol, address,
                      placeholder + 2))
        return false;
      return true;
    }
    if (in->imm < 0) {
      if (in->dst < 0 || !nyr_printf(c->w, "\t%s\n", in->symbol)) {
        nyr_set_error(c->err, c->err_len,
                      "nyir x86-64: invalid inline-asm register output");
        return false;
      }
      if (!ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst))) {
        nyr_set_error(
            c->err, c->err_len,
            "nyir x86-64: inline-asm register output has no writable home");
        return false;
      }
      return true;
    }
    return nyr_printf(c->w, "\t%s\n", in->symbol);
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
  case NYIR_VEC4_ADD_I64:
  case NYIR_VEC4_SUB_I64:
  case NYIR_VEC4_AND_I64:
  case NYIR_VEC4_OR_I64:
  case NYIR_VEC4_XOR_I64:
  case NYIR_VEC4_LOAD_I64:
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

static const char *ny_x64_byte_arg_reg(const ny_native_target_info_t *target,
                                       size_t index) {
  static const char *const sysv[] = {"%dil", "%sil", "%dl",
                                     "%cl",  "%r8b", "%r9b"};
  static const char *const win64[] = {"%cl", "%dl", "%r8b", "%r9b"};
  if (!target)
    return NULL;
  if (target->abi == NY_NATIVE_ABI_WIN64)
    return index < sizeof(win64) / sizeof(win64[0]) ? win64[index] : NULL;
  return index < sizeof(sysv) / sizeof(sysv[0]) ? sysv[index] : NULL;
}

static const char *ny_x64_dword_arg_reg(const ny_native_target_info_t *target,
                                        size_t index) {
  static const char *const sysv[] = {"%edi", "%esi", "%edx",
                                     "%ecx", "%r8d", "%r9d"};
  static const char *const win64[] = {"%ecx", "%edx", "%r8d", "%r9d"};
  if (!target)
    return NULL;
  if (target->abi == NY_NATIVE_ABI_WIN64)
    return index < sizeof(win64) / sizeof(win64[0]) ? win64[index] : NULL;
  return index < sizeof(sysv) / sizeof(sysv[0]) ? sysv[index] : NULL;
}

static bool ny_x64_emit_ir_with_linkage(
    nyr_writer_t *w, const ny_native_target_info_t *target,
    const nyr_func_t *nyir, const char *func_name, bool tag_return,
    bool is_local, bool retpoline_indirect_calls, bool retpoline_inline_calls,
    bool function_return_thunk_extern, char *err, size_t err_len) {
  if (!w || !target || !nyir)
    return false;
  if (nyir->next_value < 0 || nyir->next_value > NY_X64_NIR_MAX_SLOTS) {
    if (err && err_len)
      snprintf(err, err_len, "x86-64 NYIR encoder: value limit exceeded");
    return false;
  }
  const char *name = func_name && func_name[0] ? func_name : "rt_main";

  ny_x64_nir_ctx_t ctx = {.w = w,
                          .target = target,
                          .nyir = nyir,
                          .frame_bytes = 0,
                          .rax_cached_slot = -1,
                          .rax_store_slot = -1,
                          .remat_last_clobber = SIZE_MAX,
                          .max_local_slot = 0,
                          .retpoline_indirect_calls = retpoline_indirect_calls,
                          .retpoline_inline_calls = retpoline_inline_calls,
                          .function_return_thunk_extern =
                              function_return_thunk_extern,
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
  if (nyir->param_types)
    for (size_t i = 0; i < nyir->param_count; ++i)
      if (nyir->param_types[i] == NYIR_PARAM_AGGREGATE) {
        ctx.is_leaf = false;
        break;
      }
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
  ctx.alloca_offset = ctx.frame_bytes;
  int parameter_frame_bytes = ctx.frame_bytes;
  int aggregate_parameter_bytes = 0;
  if (nyir->param_types && nyir->param_abi) {
    for (size_t i = 0; i < nyir->param_count; ++i) {
      if (nyir->param_types[i] != NYIR_PARAM_AGGREGATE)
        continue;
      uint32_t size = NYIR_ARG_AGG_SIZE(nyir->param_abi[i]);
      if ((uint64_t)size + (uint64_t)aggregate_parameter_bytes + 7u >
          (uint64_t)INT_MAX)
        return false;
      aggregate_parameter_bytes += (int)((size + 7u) & ~7u);
    }
  }
  if (aggregate_parameter_bytes > 0) {
    if (ctx.frame_bytes > INT_MAX - aggregate_parameter_bytes - 15)
      return false;
    ctx.frame_bytes = (ctx.frame_bytes + aggregate_parameter_bytes + 15) & ~15;
  }
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
  if (strcmp(target->object_format, "macho") != 0 &&
      !nyr_printf(w, is_local ? "\t.local\t%s%s\n" : "\t.globl\t%s%s\n", sym,
                  name))
    return false;
  if (!nyr_printf(w, "%s%s:\n", sym, name))
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
    if (param_init && stored && nyir->param_count > 0 &&
        nyir->param_count <= (size_t)max_local) {
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
    int aggregate_parameter_cursor = parameter_frame_bytes;
    for (int i = 0; i < max_local; ++i) {
      if (!param_init || !param_init[i])
        continue;
      nyr_param_type_t param_type =
          nyir->param_types && (size_t)i < nyir->param_count
              ? nyir->param_types[i]
              : NYIR_PARAM_I64;
      bool is_f64 = i < NY_X64_NIR_MAX_SLOTS && ctx.local_f64[i];
      bool is_f32 = i < NY_X64_NIR_MAX_SLOTS && ctx.local_f32[i];
      char dst[64];
      ny_x64_nir_slot_str(&ctx, i, dst, sizeof(dst));
      if (param_type == NYIR_PARAM_AGGREGATE) {
        if (!nyir->param_abi || (size_t)i >= nyir->param_count)
          goto parameter_init_failed;
        uint32_t packed = nyir->param_abi[i];
        uint32_t size = NYIR_ARG_AGG_SIZE(packed);
        uint32_t area_size = (size + 7u) & ~7u;
        aggregate_parameter_cursor += (int)area_size;
        unsigned class0 = NYIR_ARG_AGG_CLASS(packed, 0);
        unsigned class1 = NYIR_ARG_AGG_CLASS(packed, 1);
        unsigned gp_need = (class0 == NYIR_ARG_CLASS_INTEGER) +
                           (class1 == NYIR_ARG_CLASS_INTEGER);
        unsigned sse_need =
            (class0 == NYIR_ARG_CLASS_SSE) + (class1 == NYIR_ARG_CLASS_SSE);
        bool register_eligible =
            size <= 16 &&
            (class0 == NYIR_ARG_CLASS_INTEGER || class0 == NYIR_ARG_CLASS_SSE ||
             class0 == NYIR_ARG_CLASS_MEMORY) &&
            (class1 == NYIR_ARG_CLASS_NONE ||
             class1 == NYIR_ARG_CLASS_INTEGER || class1 == NYIR_ARG_CLASS_SSE);
        bool in_registers =
            register_eligible && class0 != NYIR_ARG_CLASS_MEMORY &&
            gp + (int)gp_need <= (int)target->gp_arg_reg_count &&
            sse + (int)sse_need <= (int)target->fp_arg_reg_count;
        if (!nyr_printf(w, "\tleaq\t-%d(%%rbp), %%rax\n\tmovq\t%%rax, %s\n",
                        aggregate_parameter_cursor, dst))
          goto parameter_init_failed;
        if (in_registers) {
          unsigned classes[] = {class0, class1};
          uint32_t offset = 0;
          for (size_t chunk = 0; chunk < 2; ++chunk) {
            if (classes[chunk] == NYIR_ARG_CLASS_INTEGER) {
              if (gp >= (int)target->gp_arg_reg_count ||
                  !nyr_printf(w, "\tmovq\t%s, -%d(%%rbp)\n",
                              target->gp_arg_regs[gp++],
                              aggregate_parameter_cursor - (int)offset))
                goto parameter_init_failed;
              offset += 8;
            } else if (classes[chunk] == NYIR_ARG_CLASS_SSE) {
              if (sse >= (int)target->fp_arg_reg_count ||
                  !nyr_printf(w, "\tmovq\t%%xmm%d, -%d(%%rbp)\n", sse++,
                              aggregate_parameter_cursor - (int)offset))
                goto parameter_init_failed;
              offset += 8;
            }
          }
        } else {
          int source_offset =
              ctx.is_leaf ? ctx.frame_bytes + 8 +
                                (int)target->shadow_space_bytes + stack * 8
                          : 16 + ctx.home_reg_count * 8 +
                                (int)target->shadow_space_bytes + stack * 8;
          uint32_t copied = 0;
          const char *source_base = ctx.is_leaf ? "%rsp" : "%rbp";
          while (copied < size) {
            if (!nyr_printf(w,
                            "\tmovq\t%d(%s), %%rax\n\tmovq\t%%rax, "
                            "-%d(%%rbp)\n",
                            source_offset + (int)copied, source_base,
                            aggregate_parameter_cursor - (int)copied))
              goto parameter_init_failed;
            copied += 8;
          }
          stack += (int)((size + 7u) / 8u);
        }
        continue;
      }
      if ((is_f64 || is_f32) && sse < 8) {
        if (!nyr_printf(w, "\t%s\t%%xmm%d, %s\n", is_f32 ? "movss" : "movsd",
                        sse, dst)) {
          free(param_init);
          free(stored);
          return false;
        }
        sse++;
      } else if (!is_f64 && !is_f32 && (size_t)gp < target->gp_arg_reg_count) {
        bool byte_param =
            param_type == NYIR_PARAM_U8 || param_type == NYIR_PARAM_I8;
        const char *byte_reg =
            byte_param ? ny_x64_byte_arg_reg(target, gp) : NULL;
        if (byte_param && byte_reg) {
          bool extended =
              param_type == NYIR_PARAM_I8
                  ? nyr_printf(w,
                               "\tmovsbq\t%s, %%rax\n\tmovq\t%%rax, "
                               "%s\n",
                               byte_reg, dst)
                  : nyr_printf(w,
                               "\tmovzbl\t%s, %%eax\n\tmovq\t%%rax, "
                               "%s\n",
                               byte_reg, dst);
          if (!extended) {
            free(param_init);
            free(stored);
            return false;
          }
        } else if (!nyr_printf(w, "\tmovq\t%s, %s\n", target->gp_arg_regs[gp],
                               dst)) {
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
                          : 16 + ctx.home_reg_count * 8 +
                                (int)target->shadow_space_bytes + stack * 8;
        const char *src_base = ctx.is_leaf ? "%rsp" : "%rbp";
        bool byte_param =
            param_type == NYIR_PARAM_U8 || param_type == NYIR_PARAM_I8;
        if (byte_param) {
          bool loaded = param_type == NYIR_PARAM_I8
                            ? nyr_printf(w, "\tmovsbq\t%d(%s), %%rax\n",
                                         src_off, src_base)
                            : nyr_printf(w, "\tmovzbl\t%d(%s), %%eax\n",
                                         src_off, src_base);
          if (!loaded || !nyr_printf(w, "\tmovq\t%%rax, %s\n", dst)) {
            free(param_init);
            free(stored);
            return false;
          }
        } else if (!nyr_printf(w, "\tmovq\t%d(%s), %%rax\n", src_off,
                               src_base) ||
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
    goto parameter_init_complete;
  parameter_init_failed:
    free(param_init);
    free(stored);
    return false;
  parameter_init_complete:
    (void)0;
  }

  /*
   * Emit each NYIR instruction.
   */
  for (size_t i = 0; i < nyir->len; ++i) {
    if (ny_trace_enabled("NY_TRACE_EMIT"))
      fprintf(stderr, "[emit %s] i=%zu op=%s\n", name, i,
              nyr_op_name(nyir->data[i].op));
    if (!ny_x64_nir_emit_inst(&ctx, &nyir->data[i], i)) {
      fprintf(stderr,
              "native NYIR repro (x86-64 emit failed at instruction %zu, "
              "op %s):\n",
              i, nyr_op_name(nyir->data[i].op));
      fprintf(stderr,
              "failing instruction operands: dst=%d a=%d b=%d c=%d d=%d "
              "imm=%" PRId64 "\n",
              nyir->data[i].dst, nyir->data[i].a, nyir->data[i].b,
              nyir->data[i].c, nyir->data[i].d, nyir->data[i].imm);
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
    if (!nyr_put(w, ctx.function_return_thunk_extern
                        ? "\tjmp\t__x86_return_thunk\n"
                        : "\tret\n"))
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
    if (!nyr_put(w, ctx.function_return_thunk_extern
                        ? "\tjmp\t__x86_return_thunk\n"
                        : "\tret\n"))
      return false;
  }

  if (strcmp(target->object_format, "macho") != 0) {
    if (!nyr_printf(w, "\t.size\t%s%s, .-%s%s\n", sym, name, sym, name))
      return false;
  }
  return true;
}

bool nyr_x86_64_emit_ir(nyr_writer_t *w, const ny_native_target_info_t *target,
                        const nyr_func_t *nyir, const char *func_name,
                        bool tag_return, char *err, size_t err_len) {
  return ny_x64_emit_ir_with_linkage(w, target, nyir, func_name, tag_return,
                                     false, false, false, false, err, err_len);
}

bool nyr_x86_64_emit_ir_local(nyr_writer_t *w,
                              const ny_native_target_info_t *target,
                              const nyr_func_t *nyir, const char *func_name,
                              bool tag_return, char *err, size_t err_len) {
  return ny_x64_emit_ir_with_linkage(w, target, nyir, func_name, tag_return,
                                     true, false, false, false, err, err_len);
}

bool nyr_x86_64_emit_ir_with_options(
    nyr_writer_t *w, const ny_native_target_info_t *target,
    const nyr_func_t *nyir, const char *func_name, bool tag_return,
    bool is_local, bool retpoline_indirect_calls, bool retpoline_inline_calls,
    bool function_return_thunk_extern, char *err, size_t err_len) {
  return ny_x64_emit_ir_with_linkage(
      w, target, nyir, func_name, tag_return, is_local,
      retpoline_indirect_calls, retpoline_inline_calls,
      function_return_thunk_extern, err, err_len);
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
 * Count GPR and FP parameters using the shared machine-frame slot map.
 */
static bool nyr_x64_mach_params(const nyr_mach_func_t *mach, bool *params,
                                size_t *gp_count, size_t *fp_count) {
  if (!mach || !gp_count || !fp_count || (!params && mach->frame_slot_len))
    return false;
  if (!nyr_mach_param_slots(mach, params))
    return false;
  *gp_count = 0;
  *fp_count = 0;
  for (size_t i = 0; i < mach->frame_slot_len; ++i) {
    if (!params[i])
      continue;
    if (mach->frame_slots[i].type == NYR_MACH_TYPE_F32 ||
        mach->frame_slots[i].type == NYR_MACH_TYPE_F64)
      ++*fp_count;
    else
      ++*gp_count;
  }
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
          (in->src0.kind != NYR_MACH_OPERAND_SYMBOL &&
           !nyr_x64_mach_is_gpr(mach, &in->src0)) ||
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
static bool ny_x64_scalar_emit_imm64(nyr_writer_t *w, int64_t imm,
                                     const char *reg) {
  const char *reg32 = strcmp(reg, "%rax") == 0   ? "%eax"
                      : strcmp(reg, "%r10") == 0 ? "%r10d"
                      : strcmp(reg, "%r11") == 0 ? "%r11d"
                                                 : NULL;
  /*
   * Zero is cheaper as `xor reg, reg` (2 bytes, no flags dependence on
   * prior value) than `movl $0, reg` (5 bytes).
   */
  if (imm == 0 && reg32)
    return nyr_printf(w, "\txorl\t%s, %s\n", reg32, reg32);
  if (imm >= 0 && (uint64_t)imm <= UINT32_MAX && reg32)
    return nyr_printf(w, "\tmovl\t$%" PRIu32 ", %s\n", (uint32_t)imm, reg32);
  if (imm >= INT32_MIN && imm <= INT32_MAX)
    return nyr_printf(w, "\tmovq\t$%" PRId64 ", %s\n", imm, reg);
  return nyr_printf(w, "\tmovabsq\t$%" PRId64 ", %s\n", imm, reg);
}

static int nyr_x64_mach_emit_leaf_return(nyr_writer_t *w,
                                         const ny_native_target_info_t *target,
                                         const nyr_mach_func_t *mach,
                                         const char *name, bool tag_return,
                                         bool function_return_thunk_extern) {
  if (!w || !target || !mach || mach->block_len != 1 || !mach->blocks ||
      !mach->insts || mach->blocks[0].first_inst != 0 ||
      mach->blocks[0].inst_count != mach->inst_len || !mach->vreg_types ||
      mach->inst_len != 2)
    return 0;

  const nyr_mach_inst_t *value = &mach->insts[0];
  const nyr_mach_inst_t *ret = &mach->insts[1];
  if (ret->opcode != NYR_MACH_RET || ret->src0.kind != NYR_MACH_OPERAND_VREG ||
      value->dst.kind != NYR_MACH_OPERAND_VREG ||
      value->dst.as.reg >= mach->vreg_len ||
      ret->src0.as.reg != value->dst.as.reg ||
      mach->vreg_types[value->dst.as.reg] != NYR_MACH_TYPE_I64 ||
      value->narrow32)
    return 0;

  bool immediate = mach->param_count == 0 && mach->frame_slot_len == 0 &&
                   value->opcode == NYR_MACH_COPY &&
                   value->src0.kind == NYR_MACH_OPERAND_IMM;
  bool identity =
      !tag_return && mach->param_count == 1 && mach->frame_slot_len == 1 &&
      mach->frame_slots && mach->frame_slots[0].type == NYR_MACH_TYPE_I64 &&
      !mach->frame_slots[0].address_taken && target->gp_arg_reg_count >= 1 &&
      value->opcode == NYR_MACH_LOAD &&
      value->src0.kind == NYR_MACH_OPERAND_FRAME &&
      value->src0.as.frame_index == 0 && !value->byte_width;
  if (!immediate && !identity)
    return 0;

  const char *sym = target->symbol_prefix ? target->symbol_prefix : "";
  if ((strcmp(target->object_format, "macho") == 0 &&
       !nyr_put(w, "\t.p2align 4, 0x90\n")) ||
      (strcmp(target->object_format, "macho") != 0 &&
       !nyr_printf(w, "\t.type\t%s%s,@function\n", sym, name)) ||
      !nyr_printf(w, "\t.globl\t%s%s\n%s%s:\n", sym, name, sym, name))
    return -1;
  if (immediate) {
    int64_t imm = value->src0.as.imm;
    if (imm >= 0 && imm <= UINT8_MAX) {
      if (!nyr_put(w, "\txorl\t%eax, %eax\n") ||
          (imm != 0 && !nyr_printf(w, "\tmovb\t$%d, %%al\n", (int)imm)))
        return -1;
    } else if (!ny_x64_scalar_emit_imm64(w, imm, "%rax")) {
      return -1;
    }
  } else if (mach->param_types && (mach->param_types[0] == NYIR_PARAM_U8 ||
                                   mach->param_types[0] == NYIR_PARAM_I8)) {
    const char *byte_reg = ny_x64_byte_arg_reg(target, 0);
    bool extended =
        byte_reg && (mach->param_types[0] == NYIR_PARAM_I8
                         ? nyr_printf(w, "\tmovsbq\t%s, %%rax\n", byte_reg)
                         : nyr_printf(w, "\tmovzbl\t%s, %%eax\n", byte_reg));
    if (!extended)
      return -1;
  } else if (!nyr_printf(w, "\tmovq\t%s, %%rax\n", target->gp_arg_regs[0])) {
    return -1;
  }
  if ((tag_return && !nyr_put(w, "\tleaq\t1(,%rax,2), %rax\n")) ||
      !nyr_put(w, function_return_thunk_extern ? "\tjmp\t__x86_return_thunk\n"
                                               : "\tret\n") ||
      (strcmp(target->object_format, "macho") != 0 &&
       !nyr_printf(w, "\t.size\t%s%s, .-%s%s\n", sym, name, sym, name)))
    return -1;
  return 1;
}

/*
 * A two-argument integer leaf with one binary operation does not need the
 * machine backend's stack homes. Keep this pattern conservative: all other
 * functions continue through the regular frame-based emitter.
 */
static int nyr_x64_mach_emit_binary_leaf(nyr_writer_t *w,
                                         const ny_native_target_info_t *target,
                                         const nyr_mach_func_t *mach,
                                         const char *name, bool tag_return,
                                         bool function_return_thunk_extern) {
  (void)function_return_thunk_extern;
  if (!w || !target || !mach || tag_return || mach->param_count != 2 ||
      mach->frame_slot_len != 2 || mach->inst_len != 4 ||
      mach->block_len != 1 || !mach->blocks || !mach->insts ||
      mach->blocks[0].first_inst != 0 || mach->blocks[0].inst_count != 4 ||
      mach->vreg_len < 3 || !mach->vreg_types || !mach->frame_slots ||
      target->gp_arg_reg_count < 2 ||
      mach->frame_slots[0].type != NYR_MACH_TYPE_I64 ||
      mach->frame_slots[1].type != NYR_MACH_TYPE_I64 ||
      mach->frame_slots[0].address_taken || mach->frame_slots[1].address_taken)
    return false;

  const nyr_mach_inst_t *load0 = &mach->insts[0];
  const nyr_mach_inst_t *load1 = &mach->insts[1];
  const nyr_mach_inst_t *add = &mach->insts[2];
  const nyr_mach_inst_t *ret = &mach->insts[3];
  if (load0->opcode != NYR_MACH_LOAD || load1->opcode != NYR_MACH_LOAD ||
      (add->opcode != NYR_MACH_ADD && add->opcode != NYR_MACH_SUB &&
       add->opcode != NYR_MACH_MUL && add->opcode != NYR_MACH_AND &&
       add->opcode != NYR_MACH_OR && add->opcode != NYR_MACH_XOR) ||
      ret->opcode != NYR_MACH_RET || load0->dst.kind != NYR_MACH_OPERAND_VREG ||
      load1->dst.kind != NYR_MACH_OPERAND_VREG ||
      load0->src0.kind != NYR_MACH_OPERAND_FRAME ||
      load1->src0.kind != NYR_MACH_OPERAND_FRAME ||
      load0->src0.as.frame_index != 0 || load1->src0.as.frame_index != 1 ||
      add->dst.kind != NYR_MACH_OPERAND_VREG ||
      add->src0.kind != NYR_MACH_OPERAND_VREG ||
      add->src1.kind != NYR_MACH_OPERAND_VREG ||
      add->src0.as.reg != load0->dst.as.reg ||
      add->src1.as.reg != load1->dst.as.reg ||
      ret->src0.kind != NYR_MACH_OPERAND_VREG ||
      ret->src0.as.reg != add->dst.as.reg ||
      load0->dst.as.reg >= mach->vreg_len ||
      load1->dst.as.reg >= mach->vreg_len ||
      add->dst.as.reg >= mach->vreg_len ||
      mach->vreg_types[load0->dst.as.reg] != NYR_MACH_TYPE_I64 ||
      mach->vreg_types[load1->dst.as.reg] != NYR_MACH_TYPE_I64 ||
      mach->vreg_types[add->dst.as.reg] != NYR_MACH_TYPE_I64 ||
      load0->byte_width || load1->byte_width || load0->narrow32 ||
      load1->narrow32 || add->narrow32)
    return 0;

  const char *opcode = add->opcode == NYR_MACH_ADD   ? "addq"
                       : add->opcode == NYR_MACH_SUB ? "subq"
                       : add->opcode == NYR_MACH_MUL ? "imulq"
                       : add->opcode == NYR_MACH_AND ? "andq"
                       : add->opcode == NYR_MACH_OR  ? "orq"
                                                     : "xorq";
  const char *sym = target->symbol_prefix ? target->symbol_prefix : "";
  if ((strcmp(target->object_format, "macho") == 0 &&
       !nyr_put(w, "\t.p2align 4, 0x90\n")) ||
      (strcmp(target->object_format, "macho") != 0 &&
       !nyr_printf(w, "\t.type\t%s%s,@function\n", sym, name)) ||
      !nyr_printf(w, "\t.globl\t%s%s\n%s%s:\n", sym, name, sym, name) ||
      (mach->param_types &&
       (mach->param_types[0] == NYIR_PARAM_U8 ||
        mach->param_types[0] == NYIR_PARAM_I8) &&
       !(mach->param_types[0] == NYIR_PARAM_I8
             ? nyr_printf(w, "\tmovsbq\t%s, %s\n",
                          ny_x64_byte_arg_reg(target, 0),
                          target->gp_arg_regs[0])
             : nyr_printf(w, "\tmovzbl\t%s, %s\n",
                          ny_x64_byte_arg_reg(target, 0),
                          ny_x64_dword_arg_reg(target, 0)))) ||
      (mach->param_types &&
       (mach->param_types[1] == NYIR_PARAM_U8 ||
        mach->param_types[1] == NYIR_PARAM_I8) &&
       !(mach->param_types[1] == NYIR_PARAM_I8
             ? nyr_printf(w, "\tmovsbq\t%s, %s\n",
                          ny_x64_byte_arg_reg(target, 1),
                          target->gp_arg_regs[1])
             : nyr_printf(w, "\tmovzbl\t%s, %s\n",
                          ny_x64_byte_arg_reg(target, 1),
                          ny_x64_dword_arg_reg(target, 1)))) ||
      !nyr_printf(w, "\tmovq\t%s, %%rax\n\t%s\t%s, %%rax\n%s",
                  target->gp_arg_regs[0], opcode, target->gp_arg_regs[1],
                  function_return_thunk_extern ? "\tjmp\t__x86_return_thunk\n"
                                               : "\tret\n") ||
      (strcmp(target->object_format, "macho") != 0 &&
       !nyr_printf(w, "\t.size\t%s%s, .-%s%s\n", sym, name, sym, name)))
    return -1;
  return 1;
}

/*
 * Straight-line integer leaves can stay in ABI registers. Loads only establish
 * immutable argument aliases; each arithmetic result is kept in %rax, and
 * unsupported dataflow falls back to the ordinary framed emitter.
 */
static bool nyr_x64_mach_is_imm_copy(const nyr_mach_func_t *mach,
                                     const nyr_mach_inst_t *in) {
  return mach && in && in->opcode == NYR_MACH_COPY &&
         in->dst.kind == NYR_MACH_OPERAND_VREG &&
         in->dst.as.reg < mach->vreg_len &&
         mach->vreg_types[in->dst.as.reg] == NYR_MACH_TYPE_I64 &&
         in->src0.kind == NYR_MACH_OPERAND_IMM &&
         in->src1.kind == NYR_MACH_OPERAND_NONE &&
         in->src2.kind == NYR_MACH_OPERAND_NONE;
}

static int nyr_x64_mach_emit_multiarg_leaf(
    nyr_writer_t *w, const ny_native_target_info_t *target,
    const nyr_mach_func_t *mach, const char *name, bool tag_return,
    bool function_return_thunk_extern) {
  if (!w || !target || !mach || tag_return || mach->param_count < 1 ||
      mach->param_count > target->gp_arg_reg_count ||
      mach->param_count != mach->frame_slot_len ||
      mach->vreg_len > NY_X64_NIR_MAX_SLOTS ||
      mach->vreg_len < mach->param_count + 1 ||
      mach->inst_len < mach->param_count + 2 || mach->block_len != 1 ||
      !mach->blocks || !mach->insts || !mach->frame_slots ||
      !mach->vreg_types || mach->blocks[0].first_inst != 0 ||
      mach->blocks[0].inst_count != mach->inst_len)
    return 0;
  for (size_t slot = 0; slot < mach->param_count; ++slot)
    if (mach->frame_slots[slot].type != NYR_MACH_TYPE_I64 ||
        mach->frame_slots[slot].address_taken)
      return 0;

  int arg_for_vreg[NY_X64_NIR_MAX_SLOTS];
  bool const_known[NY_X64_NIR_MAX_SLOTS] = {0};
  int64_t const_value[NY_X64_NIR_MAX_SLOTS] = {0};
  for (size_t reg = 0; reg < mach->vreg_len; ++reg)
    arg_for_vreg[reg] = -1;
  for (size_t arg = 0; arg < mach->param_count; ++arg) {
    const nyr_mach_inst_t *load = &mach->insts[arg];
    if (load->opcode != NYR_MACH_LOAD ||
        load->src0.kind != NYR_MACH_OPERAND_FRAME ||
        load->src0.as.frame_index != arg ||
        load->dst.kind != NYR_MACH_OPERAND_VREG ||
        load->dst.as.reg >= mach->vreg_len ||
        mach->vreg_types[load->dst.as.reg] != NYR_MACH_TYPE_I64 ||
        load->byte_width || load->narrow32)
      return 0;
    arg_for_vreg[load->dst.as.reg] = (int)arg;
  }

  size_t ret_index = mach->inst_len - 1;
  const nyr_mach_inst_t *ret = &mach->insts[ret_index];
  if (ret->opcode != NYR_MACH_RET || ret->src0.kind != NYR_MACH_OPERAND_VREG ||
      ret->src0.as.reg >= mach->vreg_len)
    return 0;

  const char *opcodes[NY_X64_NIR_MAX_SLOTS];
  int rhs_regs[NY_X64_NIR_MAX_SLOTS];
  int64_t rhs_immediates[NY_X64_NIR_MAX_SLOTS];
  size_t op_count = 0;
  for (size_t i = mach->param_count; i < ret_index; ++i) {
    const nyr_mach_inst_t *in = &mach->insts[i];
    if (nyr_x64_mach_is_imm_copy(mach, in)) {
      const_known[in->dst.as.reg] = true;
      const_value[in->dst.as.reg] = in->src0.as.imm;
      continue;
    }
    const char *opcode = in->opcode == NYR_MACH_ADD   ? "addq"
                         : in->opcode == NYR_MACH_SUB ? "subq"
                         : in->opcode == NYR_MACH_MUL ? "imulq"
                         : in->opcode == NYR_MACH_AND ? "andq"
                         : in->opcode == NYR_MACH_OR  ? "orq"
                         : in->opcode == NYR_MACH_XOR ? "xorq"
                                                      : NULL;
    if (!opcode || in->narrow32 || in->dst.kind != NYR_MACH_OPERAND_VREG ||
        in->src0.kind != NYR_MACH_OPERAND_VREG ||
        in->dst.as.reg >= mach->vreg_len || in->src0.as.reg >= mach->vreg_len ||
        mach->vreg_types[in->dst.as.reg] != NYR_MACH_TYPE_I64 ||
        arg_for_vreg[in->src0.as.reg] == -1 || op_count >= NY_X64_NIR_MAX_SLOTS)
      return 0;
    rhs_regs[op_count] = -1;
    rhs_immediates[op_count] = 0;
    if (in->src1.kind == NYR_MACH_OPERAND_VREG) {
      if (in->src1.as.reg >= mach->vreg_len)
        return 0;
      if (const_known[in->src1.as.reg]) {
        if ((int64_t)(int32_t)const_value[in->src1.as.reg] !=
            const_value[in->src1.as.reg])
          return 0;
        rhs_immediates[op_count] = const_value[in->src1.as.reg];
      } else {
        if (arg_for_vreg[in->src1.as.reg] == -1 ||
            (arg_for_vreg[in->src1.as.reg] == -2 &&
             arg_for_vreg[in->src0.as.reg] != -2))
          return 0;
        rhs_regs[op_count] = arg_for_vreg[in->src1.as.reg];
      }
    } else if (in->src1.kind == NYR_MACH_OPERAND_IMM &&
               (int64_t)(int32_t)in->src1.as.imm == in->src1.as.imm) {
      rhs_immediates[op_count] = in->src1.as.imm;
    } else {
      return 0;
    }
    opcodes[op_count++] = opcode;
    const_known[in->dst.as.reg] = false;
    arg_for_vreg[in->dst.as.reg] = -2;
  }
  if (op_count == 0 || arg_for_vreg[ret->src0.as.reg] == -1)
    return 0;

  const char *sym = target->symbol_prefix ? target->symbol_prefix : "";
  if ((strcmp(target->object_format, "macho") == 0 &&
       !nyr_put(w, "\t.p2align 4, 0x90\n")) ||
      (strcmp(target->object_format, "macho") != 0 &&
       !nyr_printf(w, "\t.type\t%s%s,@function\n", sym, name)) ||
      !nyr_printf(w, "\t.globl\t%s%s\n%s%s:\n", sym, name, sym, name))
    return -1;

  for (size_t arg = 0; mach->param_types && arg < mach->param_count; ++arg) {
    if (mach->param_types[arg] != NYIR_PARAM_U8 &&
        mach->param_types[arg] != NYIR_PARAM_I8)
      continue;
    const char *byte_reg = ny_x64_byte_arg_reg(target, arg);
    const char *dword_reg = ny_x64_dword_arg_reg(target, arg);
    bool extended =
        byte_reg && (mach->param_types[arg] == NYIR_PARAM_I8
                         ? nyr_printf(w, "\tmovsbq\t%s, %s\n", byte_reg,
                                      target->gp_arg_regs[arg])
                         : dword_reg && nyr_printf(w, "\tmovzbl\t%s, %s\n",
                                                   byte_reg, dword_reg));
    if (!extended)
      return -1;
  }

  size_t op_index = 0;
  for (size_t i = mach->param_count; i < ret_index; ++i) {
    const nyr_mach_inst_t *in = &mach->insts[i];
    if (nyr_x64_mach_is_imm_copy(mach, in))
      continue;
    int src0 = arg_for_vreg[in->src0.as.reg];
    if (src0 != -2 &&
        !nyr_printf(w, "\tmovq\t%s, %%rax\n", target->gp_arg_regs[src0]))
      return -1;
    if (rhs_regs[op_index] == -1) {
      if (rhs_immediates[op_index] == 1 &&
          strcmp(opcodes[op_index], "addq") == 0) {
        if (!nyr_put(w, "\tincq\t%rax\n"))
          return -1;
      } else if (rhs_immediates[op_index] == 1 &&
                 strcmp(opcodes[op_index], "subq") == 0) {
        if (!nyr_put(w, "\tdecq\t%rax\n"))
          return -1;
      } else if (!nyr_printf(w, "\t%s\t$%" PRId64 ", %%rax\n",
                             opcodes[op_index], rhs_immediates[op_index])) {
        return -1;
      }
    } else {
      const char *rhs = rhs_regs[op_index] == -2
                            ? "%rax"
                            : target->gp_arg_regs[rhs_regs[op_index]];
      if (!nyr_printf(w, "\t%s\t%s, %%rax\n", opcodes[op_index], rhs))
        return -1;
    }
    ++op_index;
  }
  int ret_reg = arg_for_vreg[ret->src0.as.reg];
  if ((ret_reg != -2 &&
       !nyr_printf(w, "\tmovq\t%s, %%rax\n", target->gp_arg_regs[ret_reg])) ||
      !nyr_put(w, function_return_thunk_extern ? "\tjmp\t__x86_return_thunk\n"
                                               : "\tret\n") ||
      (strcmp(target->object_format, "macho") != 0 &&
       !nyr_printf(w, "\t.size\t%s%s, .-%s%s\n", sym, name, sym, name)))
    return -1;
  return 1;
}

static bool ny_x64_scalar_emit_rax_load(nyr_writer_t *w,
                                        const nyr_mach_operand_t *op, int off,
                                        int rax_cached_off) {
  if (op->kind == NYR_MACH_OPERAND_VREG && rax_cached_off == off)
    return true;
  if (op->kind == NYR_MACH_OPERAND_IMM)
    return ny_x64_scalar_emit_imm64(w, op->as.imm, "%rax");
  /*
   * Callee-saved GPR cache: if the stack slot is already in a callee-saved
   * register, move from that register instead of reloading from memory.
   * Callee-saved registers survive calls, so this avoids redundant loads.
   */
  for (size_t g = 0; g < 6; ++g) {
    if (x64_gpr_cached_off[g] == off)
      return nyr_printf(w, "\tmovq\t%s, %%rax\n", x64_gpr_names[g]);
  }
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
                                   const size_t *definitions,
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
  if (definitions) {
    if (operand->as.reg >= mach->vreg_len)
      return false;
    size_t index = definitions[operand->as.reg];
    if (index >= mach->inst_len)
      return false;
    definition = &mach->insts[index];
  } else {
    for (size_t i = 0; i < mach->inst_len; ++i) {
      const nyr_mach_inst_t *in = &mach->insts[i];
      if (in->dst.kind != NYR_MACH_OPERAND_VREG ||
          in->dst.as.reg != operand->as.reg)
        continue;
      if (definition)
        return false;
      definition = in;
    }
  }
  return definition && definition->opcode == NYR_MACH_COPY &&
         nyr_x64_mach_const_i64(mach, definitions, &definition->src0, out,
                                depth + 1);
}

static size_t *nyr_x64_mach_index_definitions(const nyr_mach_func_t *mach) {
  if (!mach || !mach->vreg_len || mach->inst_len < 64 ||
      mach->vreg_len > SIZE_MAX / sizeof(size_t))
    return NULL;
  size_t *definitions = malloc(mach->vreg_len * sizeof(*definitions));
  if (!definitions)
    return NULL;
  for (size_t i = 0; i < mach->vreg_len; ++i)
    definitions[i] = SIZE_MAX;
  for (size_t i = 0; i < mach->inst_len; ++i) {
    const nyr_mach_operand_t *dst = &mach->insts[i].dst;
    if (dst->kind != NYR_MACH_OPERAND_VREG || dst->as.reg >= mach->vreg_len)
      continue;
    size_t *slot = &definitions[dst->as.reg];
    *slot = *slot == SIZE_MAX ? i : SIZE_MAX - 1;
  }
  return definitions;
}

typedef struct {
  size_t only_user;
  unsigned char count;
} nyr_x64_mach_use_t;

static void nyr_x64_mach_note_use(nyr_x64_mach_use_t *uses, size_t len,
                                  const nyr_mach_operand_t *op, size_t pc) {
  if (op->kind != NYR_MACH_OPERAND_VREG || op->as.reg >= len)
    return;
  nyr_x64_mach_use_t *use = &uses[op->as.reg];
  if (use->count < 2) {
    ++use->count;
    use->only_user = pc;
  }
}

static nyr_x64_mach_use_t *
nyr_x64_mach_count_uses(const nyr_mach_func_t *mach) {
  if (!mach || !mach->vreg_len)
    return NULL;
  nyr_x64_mach_use_t *uses = calloc(mach->vreg_len, sizeof(*uses));
  if (!uses)
    return NULL;
  for (size_t i = 0; i < mach->inst_len; ++i) {
    const nyr_mach_inst_t *in = &mach->insts[i];
    nyr_x64_mach_note_use(uses, mach->vreg_len, &in->src0, i);
    nyr_x64_mach_note_use(uses, mach->vreg_len, &in->src1, i);
    nyr_x64_mach_note_use(uses, mach->vreg_len, &in->src2, i);
    if (in->opcode == NYR_MACH_STORE)
      nyr_x64_mach_note_use(uses, mach->vreg_len, &in->dst, i);
    for (size_t j = 0; j < in->args_len; ++j)
      nyr_x64_mach_note_use(uses, mach->vreg_len, &in->args[j], i);
  }
  return uses;
}

static bool nyr_x64_mach_can_forward_rax_result(const nyr_mach_func_t *mach,
                                                const nyr_x64_mach_use_t *uses,
                                                size_t index, size_t block_end,
                                                const nyr_mach_inst_t *in) {
  if (!mach || !in || index + 1 >= block_end || block_end > mach->inst_len ||
      !uses || in->dst.kind != NYR_MACH_OPERAND_VREG)
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
  case NYR_MACH_SELECT:
  case NYR_MACH_RET:
    break;
  default:
    return false;
  }

  return in->dst.as.reg < mach->vreg_len && uses[in->dst.as.reg].count == 1 &&
         uses[in->dst.as.reg].only_user == index + 1;
}

static bool nyr_x64_mach_preserves_xmm0(const nyr_mach_func_t *mach,
                                        const nyr_mach_inst_t *in) {
  switch (in->opcode) {
  case NYR_MACH_NOP:
  case NYR_MACH_LEA:
    return true;
  case NYR_MACH_COPY:
  case NYR_MACH_LOAD:
  case NYR_MACH_STORE:
  case NYR_MACH_ADD:
  case NYR_MACH_SUB:
  case NYR_MACH_MUL:
  case NYR_MACH_AND:
  case NYR_MACH_OR:
  case NYR_MACH_XOR:
  case NYR_MACH_SHL:
  case NYR_MACH_SAR:
  case NYR_MACH_ROR:
  case NYR_MACH_ROR32:
  case NYR_MACH_BSWAP32:
  case NYR_MACH_BSWAP64:
    return !nyr_x64_mach_is_float(mach, &in->dst) &&
           !nyr_x64_mach_is_float(mach, &in->src0) &&
           !nyr_x64_mach_is_float(mach, &in->src1);
  default:
    return false;
  }
}

static bool nyr_x64_mach_can_forward_xmm_result(const nyr_mach_func_t *mach,
                                                const nyr_x64_mach_use_t *uses,
                                                size_t index, size_t block_end,
                                                const nyr_mach_inst_t *in) {
  if (!mach || !uses || !in || index + 1 >= block_end ||
      block_end > mach->inst_len || in->dst.kind != NYR_MACH_OPERAND_VREG ||
      in->dst.as.reg >= mach->vreg_len ||
      !nyr_x64_mach_is_float(mach, &in->dst) || uses[in->dst.as.reg].count != 1)
    return false;

  size_t consumer = uses[in->dst.as.reg].only_user;
  if (consumer <= index || consumer >= block_end || consumer - index > 32)
    return false;
  for (size_t i = index + 1; i < consumer; ++i)
    if (!nyr_x64_mach_preserves_xmm0(mach, &mach->insts[i]))
      return false;

  const nyr_mach_inst_t *next = &mach->insts[consumer];
  if (next->src0.kind != NYR_MACH_OPERAND_VREG ||
      next->src0.as.reg != in->dst.as.reg ||
      !nyr_x64_mach_is_float(mach, &next->src0))
    return false;
  if (next->opcode == NYR_MACH_STORE)
    return true;
  if (next->opcode == NYR_MACH_COPY)
    return !next->narrow32 && next->dst.kind == NYR_MACH_OPERAND_VREG &&
           nyr_x64_mach_is_float(mach, &next->dst) &&
           nyr_x64_mach_is_f32(mach, &next->dst) ==
               nyr_x64_mach_is_f32(mach, &next->src0) &&
           nyr_x64_mach_offset(mach, &next->dst) !=
               nyr_x64_mach_offset(mach, &next->src0);
  if (next->opcode == NYR_MACH_SQRT)
    return nyr_x64_mach_is_f64(mach, &next->src0) &&
           nyr_x64_mach_is_f64(mach, &next->dst);
  return (next->opcode == NYR_MACH_ADD || next->opcode == NYR_MACH_SUB ||
          next->opcode == NYR_MACH_MUL || next->opcode == NYR_MACH_DIV) &&
         nyr_x64_mach_is_float(mach, &next->dst) &&
         nyr_x64_mach_is_f32(mach, &next->src0) ==
             nyr_x64_mach_is_f32(mach, &next->dst);
}

static bool nyr_x64_mach_compare_branch(const nyr_mach_func_t *mach,
                                        const nyr_x64_mach_use_t *uses,
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

  size_t use_count = 0;
  if (uses && compare->dst.as.reg < mach->vreg_len) {
    use_count = uses[compare->dst.as.reg].count;
  } else {
    for (size_t i = 0; i < mach->inst_len && use_count <= 1; ++i) {
      const nyr_mach_inst_t *user = &mach->insts[i];
      const nyr_mach_operand_t *operands[] = {&user->src0, &user->src1,
                                              &user->src2};
      for (size_t k = 0; k < sizeof(operands) / sizeof(operands[0]); ++k)
        if (operands[k]->kind == NYR_MACH_OPERAND_VREG &&
            operands[k]->as.reg == compare->dst.as.reg)
          ++use_count;
      if (user->opcode == NYR_MACH_STORE &&
          user->dst.kind == NYR_MACH_OPERAND_VREG &&
          user->dst.as.reg == compare->dst.as.reg)
        ++use_count;
      for (size_t k = 0; k < user->args_len && use_count <= 1; ++k)
        if (user->args[k].kind == NYR_MACH_OPERAND_VREG &&
            user->args[k].as.reg == compare->dst.as.reg)
          ++use_count;
    }
  }
  if (use_count != 1)
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
  case NYR_MACH_VA_SAVE:
  case NYR_MACH_VA_START:
  case NYR_MACH_VA_ARG:
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
static void
nyr_x64_mach_prepare_reused_cmp_flags(const nyr_mach_func_t *mach,
                                      const nyr_x64_mach_use_t *uses,
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
    if (!nyr_x64_mach_compare_branch(mach, uses, compare_index, end, first_cmp,
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
    if (!nyr_x64_mach_compare_branch(mach, uses, second_cmp_index, second_end,
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
                                     const nyr_x64_mach_use_t *indexed_uses,
                                     nyr_mach_reg_t reg) {
  if (indexed_uses && reg < mach->vreg_len)
    return indexed_uses[reg].count;
  size_t uses = 0;
  for (size_t i = 0; i < mach->inst_len && uses <= 1; ++i) {
    const nyr_mach_inst_t *in = &mach->insts[i];
    const nyr_mach_operand_t operands[] = {in->src0, in->src1, in->src2};
    for (size_t k = 0; k < sizeof(operands) / sizeof(operands[0]); ++k)
      if (operands[k].kind == NYR_MACH_OPERAND_VREG &&
          operands[k].as.reg == reg)
        ++uses;
    if (in->opcode == NYR_MACH_STORE && in->dst.kind == NYR_MACH_OPERAND_VREG &&
        in->dst.as.reg == reg)
      ++uses;
    for (size_t k = 0; k < in->args_len && uses <= 1; ++k)
      if (in->args[k].kind == NYR_MACH_OPERAND_VREG &&
          in->args[k].as.reg == reg)
        ++uses;
  }
  return uses;
}

static bool nyr_x64_mach_collect_cmp_and(const nyr_mach_func_t *mach,
                                         const nyr_x64_mach_use_t *uses,
                                         const size_t *definitions,
                                         nyr_mach_reg_t reg,
                                         nyr_x64_mach_cmp_and_t *out,
                                         unsigned depth) {
  if (!mach || !out || depth > NY_X64_MACH_CMP_AND_MAX ||
      reg >= mach->vreg_len || nyr_x64_mach_vreg_uses(mach, uses, reg) != 1)
    return false;
  size_t def = SIZE_MAX, defs = 0;
  if (definitions) {
    def = definitions[reg];
    if (def >= mach->inst_len)
      return false;
  } else {
    for (size_t i = 0; i < mach->inst_len; ++i) {
      const nyr_mach_inst_t *in = &mach->insts[i];
      if (in->dst.kind == NYR_MACH_OPERAND_VREG && in->dst.as.reg == reg) {
        def = i;
        ++defs;
      }
    }
    if (defs != 1 || def == SIZE_MAX)
      return false;
  }
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
  return nyr_x64_mach_collect_cmp_and(mach, uses, definitions, in->src0.as.reg,
                                      out, depth + 1) &&
         nyr_x64_mach_collect_cmp_and(mach, uses, definitions, in->src1.as.reg,
                                      out, depth + 1);
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
                                         const nyr_x64_mach_use_t *uses,
                                         const size_t *definitions,
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
      bool collected = nyr_x64_mach_collect_cmp_and(mach, uses, definitions,
                                                    br->src0.as.reg, &tree, 0);
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
                                         const nyr_x64_mach_use_t *uses,
                                         size_t index, size_t block_end,
                                         const nyr_mach_inst_t *in, int dst,
                                         int *rax_cached_off) {
  if (nyr_x64_mach_can_forward_rax_result(mach, uses, index, block_end, in)) {
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

static bool nyr_x86_64_emit_mach_scalar_impl(
    nyr_writer_t *w, const ny_native_target_info_t *target,
    const nyr_mach_func_t *mach, const char *func_name, bool tag_return,
    bool function_return_thunk_extern, char *err, size_t err_len,
    const nyr_x64_mach_use_t *uses, const size_t *definitions) {
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
  int leaf_return = nyr_x64_mach_emit_leaf_return(
      w, target, mach, name, tag_return, function_return_thunk_extern);
  if (leaf_return != 0)
    return leaf_return > 0;
  int multiarg_leaf = nyr_x64_mach_emit_multiarg_leaf(
      w, target, mach, name, tag_return, function_return_thunk_extern);
  if (multiarg_leaf != 0)
    return multiarg_leaf > 0;
  int binary_leaf = nyr_x64_mach_emit_binary_leaf(
      w, target, mach, name, tag_return, function_return_thunk_extern);
  if (binary_leaf != 0)
    return binary_leaf > 0;
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
    nyr_x64_mach_prepare_cmp_and(mach, uses, definitions, fused_cmp, fused_and,
                                 fused_false);
    nyr_x64_mach_prepare_reused_cmp_flags(mach, uses, reuse_cmp_flags);
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
        if (mach->param_types && slot < mach->param_count &&
            mach->param_types[slot] == NYIR_PARAM_AGGREGATE) {
          nyr_mach_sysv_aggregate_t aggregate;
          size_t gp_first = arg, fp_first = fp_arg;
          if (target->abi != NY_NATIVE_ABI_SYSV || !mach->param_abi ||
              !nyr_mach_sysv_aggregate_parameter(
                  mach->param_abi[slot], target->gp_arg_reg_count,
                  target->fp_arg_reg_count, &arg, &fp_arg, &aggregate)) {
            free(params);
            return false;
          }
          size_t home = 0;
          if (aggregate.in_registers) {
            if (!mach->param_aggregate_home_slots ||
                mach->param_aggregate_home_slots[slot] == UINT32_MAX ||
                (size_t)mach->param_aggregate_home_slots[slot] +
                        aggregate.chunks >
                    mach->frame_slot_len) {
              free(params);
              return false;
            }
            home = ((size_t)mach->param_aggregate_home_slots[slot] +
                    aggregate.chunks) *
                   8;
            for (unsigned chunk = 0; chunk < aggregate.chunks; ++chunk) {
              unsigned cls = aggregate.classes[chunk];
              if (cls == NYIR_ARG_CLASS_NONE)
                continue;
              const char *reg = cls == NYIR_ARG_CLASS_INTEGER
                                    ? target->gp_arg_regs[gp_first++]
                                    : target->fp_arg_regs[fp_first++];
              if (!nyr_printf(w, "\tmovq\t%s, -%zu(%%rbp)\n", reg,
                              home - chunk * 8)) {
                free(params);
                return false;
              }
            }
          }
          bool saved = aggregate.in_registers
                           ? nyr_printf(w, "\tleaq\t-%zu(%%rbp), %%rax\n", home)
                           : nyr_printf(w, "\tleaq\t%zu(%%rbp), %%rax\n",
                                        16 + stack_arg * 8);
          if (!saved ||
              !nyr_printf(w, "\tmovq\t%%rax, -%zu(%%rbp)\n", (slot + 1) * 8)) {
            free(params);
            return false;
          }
          if (!aggregate.in_registers)
            stack_arg += aggregate.chunks;
          continue;
        }
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
        nyr_param_type_t param_type =
            mach->param_types && slot < mach->param_count
                ? mach->param_types[slot]
                : NYIR_PARAM_I64;
        bool byte_param =
            param_type == NYIR_PARAM_U8 || param_type == NYIR_PARAM_I8;
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
          if (byte_param) {
            saved = param_type == NYIR_PARAM_I8
                        ? nyr_printf(w,
                                     "\tmovsbq\t%zu(%%rbp), %%rax\n\tmovq\t"
                                     "%%rax, -%zu(%%rbp)\n",
                                     incoming, (slot + 1) * 8)
                        : nyr_printf(w,
                                     "\tmovzbl\t%zu(%%rbp), %%eax\n\tmovq\t"
                                     "%%rax, -%zu(%%rbp)\n",
                                     incoming, (slot + 1) * 8);
          } else {
            const char *move = is_f32 ? "movss" : is_f64 ? "movsd" : "movq";
            saved = nyr_printf(
                w, "\t%s\t%zu(%%rbp), %s\n\t%s\t%s, -%zu(%%rbp)\n", move,
                incoming, is_f32 || is_f64 ? "%xmm0" : "%rax", move,
                is_f32 || is_f64 ? "%xmm0" : "%rax", (slot + 1) * 8);
          }
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
          const char *byte_reg =
              byte_param ? ny_x64_byte_arg_reg(target, index) : NULL;
          if (byte_param && byte_reg) {
            saved = param_type == NYIR_PARAM_I8
                        ? nyr_printf(w,
                                     "\tmovsbq\t%s, %%rax\n\tmovq\t%%rax, "
                                     "-%zu(%%rbp)\n",
                                     byte_reg, (slot + 1) * 8)
                        : nyr_printf(w,
                                     "\tmovzbl\t%s, %%eax\n\tmovq\t%%rax, "
                                     "-%zu(%%rbp)\n",
                                     byte_reg, (slot + 1) * 8);
          } else {
            saved = nyr_printf(w, "\tmovq\t%s, -%zu(%%rbp)\n",
                               target->gp_arg_regs[index], (slot + 1) * 8);
          }
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
  bool tail_frame_checked = false;
  bool tail_frame_unsafe = false;
  int rax_cached_off = -1;
  size_t xmm_cached_reg = SIZE_MAX;
  /*
   * Zero register tracker: which register currently holds the value 0, or -1.
   * Set when we emit `xor reg, reg`; cleared when that register is written
   * with a non-zero value or at block boundaries.  Used to avoid redundant
   * zeroing when storing or copying zero to a vreg.
   */
  int zero_reg = -1;
  static const char *const zero_reg_names[6] = {"%rax", "%rbx", "%r12",
                                                "%r13", "%r14", "%r15"};
  for (size_t block = 0; block < mach->block_len; ++block) {
    if (!nyr_printf(w, ".Lny_mach_%s_%zu:\n", name, block))
      return false;
    rax_cached_off = -1;
    xmm_cached_reg = SIZE_MAX;
    zero_reg = -1;
    const nyr_mach_block_t *b = &mach->blocks[block];
    for (size_t n = 0; n < b->inst_count; ++n) {
      const nyr_mach_inst_t *in = &mach->insts[b->first_inst + n];
      int dst = nyr_x64_mach_offset(mach, &in->dst);
      int a = nyr_x64_mach_offset(mach, &in->src0);
      int b_off = nyr_x64_mach_offset(mach, &in->src1);
      if (xmm_cached_reg != SIZE_MAX &&
          !nyr_x64_mach_preserves_xmm0(mach, in) &&
          (in->src0.kind != NYR_MACH_OPERAND_VREG ||
           in->src0.as.reg != xmm_cached_reg))
        xmm_cached_reg = SIZE_MAX;
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
            if (!ny_x64_scalar_emit_imm64(w, in->src0.as.imm, "%rax") ||
                !nyr_put(w, "\tmovq\t%rax, %xmm0\n"))
              return false;
          } else if (xmm_cached_reg == SIZE_MAX &&
                     !nyr_printf(w, "\t%s\t-%d(%%rbp), %%xmm0\n",
                                 f32 ? "movss" : "movsd", a)) {
            return false;
          }
          bool forward = nyr_x64_mach_can_forward_xmm_result(
              mach, uses, b->first_inst + n, b->first_inst + b->inst_count, in);
          if (!forward && !nyr_printf(w, "\t%s\t%%xmm0, -%d(%%rbp)\n",
                                      f32 ? "movss" : "movsd", dst))
            return false;
          xmm_cached_reg = forward ? in->dst.as.reg : SIZE_MAX;
          /*
           * The immediate path used %rax; discard its cached stack home.
           */
          if (in->src0.kind == NYR_MACH_OPERAND_IMM)
            rax_cached_off = -1;
          break;
        }
        if (in->src0.kind == NYR_MACH_OPERAND_IMM) {
          if (!ny_x64_scalar_emit_imm64(w, in->src0.as.imm, "%rax"))
            return false;
          if (in->src0.as.imm == 0)
            zero_reg = 0; /* rax now holds zero */
          else
            zero_reg = -1; /* rax no longer holds zero */
        } else if (!ny_x64_scalar_emit_rax_load(w, &in->src0, a,
                                                rax_cached_off)) {
          return false;
        } else {
          zero_reg = -1; /* rax now holds an unknown value */
        }
        if (!nyr_x64_mach_emit_rax_result(w, mach, uses, b->first_inst + n,
                                          b->first_inst + b->inst_count, in,
                                          dst, &rax_cached_off))
          return false;
        break;
      case NYR_MACH_LEA: {
        if (in->src0.kind == NYR_MACH_OPERAND_FRAME) {
          if (!nyr_printf(w, "\tleaq\t-%d(%%rbp), %%rax\n", a))
            return false;
        } else if (in->src0.kind == NYR_MACH_OPERAND_SYMBOL) {
          if (!nyr_printf(w, "\tleaq\t%s(%%rip), %%rax\n",
                          ny_x64_nir_link_symbol(in->src0.as.symbol)))
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
        size_t pc = b->first_inst + n;
        if (uses && in->dst.kind == NYR_MACH_OPERAND_VREG &&
            in->dst.as.reg < mach->vreg_len &&
            uses[in->dst.as.reg].count == 1 &&
            uses[in->dst.as.reg].only_user == pc + 1 && n + 1 < b->inst_count) {
          const nyr_mach_inst_t *next = &mach->insts[pc + 1];
          int value_off = nyr_x64_mach_offset(mach, &next->src0);
          if (next->opcode == NYR_MACH_STORE &&
              next->dst.kind == NYR_MACH_OPERAND_VREG &&
              next->dst.as.reg == in->dst.as.reg &&
              (next->src0.kind == NYR_MACH_OPERAND_VREG ||
               next->src0.kind == NYR_MACH_OPERAND_FRAME) &&
              nyr_x64_mach_is_float(mach, &next->src0) && value_off > 0 &&
              value_off != dst) {
            const char *move =
                nyr_x64_mach_is_f32(mach, &next->src0) ? "movss" : "movsd";
            bool cached = next->src0.kind == NYR_MACH_OPERAND_VREG &&
                          xmm_cached_reg == next->src0.as.reg;
            if (!cached &&
                !nyr_printf(w, "\t%s\t-%d(%%rbp), %%xmm0\n", move, value_off))
              return false;
            if (!nyr_printf(w, "\t%s\t%%xmm0, (%%rax)\n", move))
              return false;
            xmm_cached_reg = SIZE_MAX;
            rax_cached_off = -1;
            ++n;
            break;
          }
          if (next->opcode == NYR_MACH_STORE &&
              next->dst.kind == NYR_MACH_OPERAND_VREG &&
              next->dst.as.reg == in->dst.as.reg &&
              (next->src0.kind == NYR_MACH_OPERAND_VREG ||
               next->src0.kind == NYR_MACH_OPERAND_FRAME ||
               next->src0.kind == NYR_MACH_OPERAND_IMM) &&
              !nyr_x64_mach_is_float(mach, &next->src0) &&
              (next->src0.kind == NYR_MACH_OPERAND_IMM ||
               (value_off > 0 && value_off != dst))) {
            if (next->src0.kind == NYR_MACH_OPERAND_IMM) {
              if (!ny_x64_scalar_emit_imm64(w, next->src0.as.imm, "%rcx"))
                return false;
            } else if (!nyr_printf(w, "\tmovq\t-%d(%%rbp), %%rcx\n",
                                   value_off)) {
              return false;
            }
            if (!nyr_put(w, next->byte_width ? "\tmovb\t%cl, (%rax)\n"
                                             : "\tmovq\t%rcx, (%rax)\n"))
              return false;
            rax_cached_off = -1;
            ++n;
            break;
          }
        }
        if (!nyr_x64_mach_emit_rax_result(w, mach, uses, pc,
                                          b->first_inst + b->inst_count, in,
                                          dst, &rax_cached_off))
          return false;
        break;
      }
      case NYR_MACH_VA_SAVE: {
        if (target->abi != NY_NATIVE_ABI_SYSV ||
            in->src0.kind != NYR_MACH_OPERAND_FRAME || a < 176)
          return false;
        static const char *const gp_regs[] = {"%rdi", "%rsi", "%rdx",
                                              "%rcx", "%r8",  "%r9"};
        for (size_t reg = 0; reg < sizeof(gp_regs) / sizeof(gp_regs[0]); ++reg)
          if (!nyr_printf(w, "\tmovq\t%s, -%d(%%rbp)\n", gp_regs[reg],
                          a - (int)(reg * 8)))
            return false;
        for (size_t reg = 0; reg < 8; ++reg)
          if (!nyr_printf(w, "\tmovups\t%%xmm%zu, -%d(%%rbp)\n", reg,
                          a - 48 - (int)(reg * 16)))
            return false;
        if (!nyr_printf(w,
                        "\tleaq\t-%d(%%rbp), %%rax\n"
                        "\tmovq\t%%rax, -%d(%%rbp)\n",
                        a, dst))
          return false;
        rax_cached_off = in->dst.kind == NYR_MACH_OPERAND_VREG ? dst : -1;
        break;
      }
      case NYR_MACH_VA_START: {
        if (target->abi != NY_NATIVE_ABI_SYSV ||
            in->src2.kind != NYR_MACH_OPERAND_IMM || in->immediate < 0 ||
            in->immediate > UINT32_MAX)
          return false;
        unsigned gp_offset = (unsigned)in->immediate & 0xffffu;
        unsigned fp_offset = (unsigned)((uint64_t)in->immediate >> 16);
        if (gp_offset > 48 || fp_offset < 48 || fp_offset > 176 ||
            in->src2.as.imm < 0 || in->src2.as.imm > INT_MAX - 16)
          return false;
        if (!nyr_printf(w,
                        "\tmovq\t-%d(%%rbp), %%r10\n"
                        "\tmovq\t-%d(%%rbp), %%r11\n"
                        "\tmovl\t$%u, (%%r10)\n"
                        "\tmovl\t$%u, 4(%%r10)\n"
                        "\tleaq\t%lld(%%rbp), %%rax\n"
                        "\tmovq\t%%rax, 8(%%r10)\n"
                        "\tmovq\t%%r11, 16(%%r10)\n",
                        a, b_off, gp_offset, fp_offset,
                        (long long)(16 + in->src2.as.imm)))
          return false;
        rax_cached_off = -1;
        break;
      }
      case NYR_MACH_VA_ARG: {
        if (target->abi != NY_NATIVE_ABI_SYSV ||
            (in->immediate != 0 && in->immediate != 1))
          return false;
        size_t pc = b->first_inst + n;
        const char *offset_field = in->immediate == 1 ? "4" : "0";
        unsigned max_offset = in->immediate == 1 ? 160 : 40;
        unsigned step = in->immediate == 1 ? 16 : 8;
        if (!nyr_printf(w,
                        "\tmovq\t-%d(%%rbp), %%r10\n"
                        "\tmovl\t%s(%%r10), %%ecx\n"
                        "\tcmpl\t$%u, %%ecx\n"
                        "\tja\t.Lny_va_stack_%s_%zu\n"
                        "\tmovq\t16(%%r10), %%r11\n"
                        "\taddq\t%%rcx, %%r11\n",
                        a, offset_field, max_offset, name, pc))
          return false;
        if (in->immediate == 1) {
          if (!nyr_printf(w,
                          "\tmovsd\t(%%r11), %%xmm0\n"
                          "\tmovsd\t%%xmm0, -%d(%%rbp)\n",
                          dst))
            return false;
        } else if (!nyr_printf(w,
                               "\tmovq\t(%%r11), %%rax\n"
                               "\tmovq\t%%rax, -%d(%%rbp)\n",
                               dst)) {
          return false;
        }
        if (!nyr_printf(w,
                        "\taddl\t$%u, %s(%%r10)\n"
                        "\tjmp\t.Lny_va_done_%s_%zu\n"
                        ".Lny_va_stack_%s_%zu:\n"
                        "\tmovq\t8(%%r10), %%r11\n",
                        step, offset_field, name, pc, name, pc))
          return false;
        if (in->immediate == 1) {
          if (!nyr_printf(w,
                          "\tmovsd\t(%%r11), %%xmm0\n"
                          "\tmovsd\t%%xmm0, -%d(%%rbp)\n",
                          dst))
            return false;
        } else if (!nyr_printf(w,
                               "\tmovq\t(%%r11), %%rax\n"
                               "\tmovq\t%%rax, -%d(%%rbp)\n",
                               dst)) {
          return false;
        }
        if (!nyr_printf(w,
                        "\taddq\t$8, %%r11\n"
                        "\tmovq\t%%r11, 8(%%r10)\n"
                        ".Lny_va_done_%s_%zu:\n",
                        name, pc))
          return false;
        rax_cached_off = -1;
        break;
      }
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
      case NYR_MACH_SQRT: {
        if ((xmm_cached_reg == SIZE_MAX &&
             !nyr_printf(w, "\tmovsd\t-%d(%%rbp), %%xmm0\n", a)) ||
            !nyr_put(w, "\tsqrtsd\t%xmm0, %xmm0\n"))
          return false;
        bool forward = nyr_x64_mach_is_f64(mach, &in->dst) &&
                       nyr_x64_mach_can_forward_xmm_result(
                           mach, uses, b->first_inst + n,
                           b->first_inst + b->inst_count, in);
        if (!forward && !nyr_printf(w, "\tmovsd\t%%xmm0, -%d(%%rbp)\n", dst))
          return false;
        xmm_cached_reg = forward ? in->dst.as.reg : SIZE_MAX;
        rax_cached_off = -1;
        break;
      }
      case NYR_MACH_LOAD:
        if (nyr_x64_mach_is_float(mach, &in->dst)) {
          const char *move =
              nyr_x64_mach_is_f32(mach, &in->dst) ? "movss" : "movsd";
          bool forward = nyr_x64_mach_can_forward_xmm_result(
              mach, uses, b->first_inst + n, b->first_inst + b->inst_count, in);
          if (in->src0.kind == NYR_MACH_OPERAND_FRAME) {
            if (!nyr_printf(w, "\t%s\t-%d(%%rbp), %%xmm0\n", move, a))
              return false;
          } else if (!ny_x64_scalar_emit_rax_load(w, &in->src0, a,
                                                  rax_cached_off) ||
                     !nyr_printf(w, "\t%s\t(%%rax), %%xmm0\n", move)) {
            return false;
          }
          if (!forward &&
              !nyr_printf(w, "\t%s\t%%xmm0, -%d(%%rbp)\n", move, dst))
            return false;
          xmm_cached_reg = forward ? in->dst.as.reg : SIZE_MAX;
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
        if (!nyr_x64_mach_emit_rax_result(w, mach, uses, b->first_inst + n,
                                          b->first_inst + b->inst_count, in,
                                          dst, &rax_cached_off))
          return false;
        break;
      case NYR_MACH_STORE:
        if (nyr_x64_mach_is_float(mach, &in->src0)) {
          const char *move =
              nyr_x64_mach_is_f32(mach, &in->src0) ? "movss" : "movsd";
          if (xmm_cached_reg == SIZE_MAX &&
              !nyr_printf(w, "\t%s\t-%d(%%rbp), %%xmm0\n", move, a))
            return false;
          if (in->dst.kind == NYR_MACH_OPERAND_FRAME) {
            if (!nyr_printf(w, "\t%s\t%%xmm0, -%d(%%rbp)\n", move, dst))
              return false;
          } else if (!nyr_printf(w,
                                 "\tmovq\t-%d(%%rbp), %%rax\n"
                                 "\t%s\t%%xmm0, (%%rax)\n",
                                 dst, move)) {
            return false;
          }
          xmm_cached_reg = SIZE_MAX;
          rax_cached_off = -1;
          break;
        }
        if (!ny_x64_scalar_emit_rax_load(w, &in->src0, a, rax_cached_off))
          return false;
        if (in->dst.kind == NYR_MACH_OPERAND_FRAME) {
          /*
           * Store zero directly from a zero register if available,
           * avoiding a redundant zeroing + store through %rax.
           */
          if (in->src0.kind == NYR_MACH_OPERAND_IMM && in->src0.as.imm == 0 &&
              zero_reg >= 0) {
            if (!nyr_printf(w, "\tmovq\t%s, -%d(%%rbp)\n",
                            zero_reg_names[zero_reg], dst))
              return false;
          } else if (!nyr_printf(w, "\tmovq\t%%rax, -%d(%%rbp)\n", dst))
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
          const char *move = f32 ? "movss" : "movsd";
          if (xmm_cached_reg == SIZE_MAX &&
              !nyr_printf(w, "\t%s\t-%d(%%rbp), %%xmm0\n", move, a))
            return false;
          if (!nyr_printf(w, "\t%s\t-%d(%%rbp), %%xmm0\n", op, b_off))
            return false;
          bool forward = nyr_x64_mach_can_forward_xmm_result(
              mach, uses, b->first_inst + n, b->first_inst + b->inst_count, in);
          if (!forward &&
              !nyr_printf(w, "\t%s\t%%xmm0, -%d(%%rbp)\n", move, dst))
            return false;
          xmm_cached_reg = forward ? in->dst.as.reg : SIZE_MAX;
          rax_cached_off = -1;
          break;
        }
        if (in->opcode == NYR_MACH_SUB &&
            in->src0.kind == NYR_MACH_OPERAND_VREG &&
            in->src1.kind == NYR_MACH_OPERAND_VREG &&
            in->src0.as.reg == in->src1.as.reg) {
          if (!nyr_put(w, "\txorl\t%eax, %eax\n") ||
              !nyr_x64_mach_emit_rax_result(w, mach, uses, b->first_inst + n,
                                            b->first_inst + b->inst_count, in,
                                            dst, &rax_cached_off))
            return false;
          rax_cached_off = in->dst.kind == NYR_MACH_OPERAND_VREG ? dst : -1;
          break;
        }
        {
          int64_t immediate = 0;
          const nyr_mach_operand_t *value_operand = &in->src0;
          bool immediate_rhs = nyr_x64_mach_const_i64(mach, definitions,
                                                      &in->src1, &immediate, 0);
          bool commutative = in->opcode != NYR_MACH_SUB;
          if (!immediate_rhs && commutative &&
              nyr_x64_mach_const_i64(mach, definitions, &in->src0, &immediate,
                                     0)) {
            immediate_rhs = true;
            value_operand = &in->src1;
          }
          if (immediate_rhs &&
              (in->narrow32 || (int64_t)(int32_t)immediate == immediate)) {
            if (in->narrow32)
              immediate = (int64_t)(int32_t)immediate;
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
                  !nyr_x64_mach_emit_rax_result(
                      w, mach, uses, b->first_inst + n,
                      b->first_inst + b->inst_count, in, dst, &rax_cached_off))
                return false;
            } else {
              if (!ny_x64_scalar_emit_rax_load(w, value_operand, value_off,
                                               rax_cached_off) ||
                  (lea_scale
                       ? !nyr_printf(w, "\tleaq\t(%%rax,%%rax,%u), %%rax\n",
                                     lea_scale)
                       : !nyr_printf(w, "\t%sq\t$%" PRId64 ", %%rax\n", op,
                                     immediate)) ||
                  !nyr_x64_mach_emit_rax_result(
                      w, mach, uses, b->first_inst + n,
                      b->first_inst + b->inst_count, in, dst, &rax_cached_off))
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
                !nyr_x64_mach_emit_rax_result(w, mach, uses, b->first_inst + n,
                                              b->first_inst + b->inst_count, in,
                                              dst, &rax_cached_off))
              return false;
          } else {
            if (!ny_x64_scalar_emit_rax_load(w, &in->src0, a, rax_cached_off) ||
                !ny_x64_scalar_emit_imm64(w, in->src1.as.imm, "%r10"))
              return false;
            const char *op = in->opcode == NYR_MACH_ADD   ? "addq"
                             : in->opcode == NYR_MACH_SUB ? "subq"
                             : in->opcode == NYR_MACH_AND ? "andq"
                             : in->opcode == NYR_MACH_OR  ? "orq"
                             : in->opcode == NYR_MACH_XOR ? "xorq"
                                                          : "imulq";
            if (!nyr_printf(w, "\t%s\t%%r10, %%rax\n", op) ||
                !nyr_x64_mach_emit_rax_result(w, mach, uses, b->first_inst + n,
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
                !nyr_x64_mach_emit_rax_result(w, mach, uses, b->first_inst + n,
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
              !nyr_x64_mach_emit_rax_result(w, mach, uses, b->first_inst + n,
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
          const char *move = f32 ? "movss" : "movsd";
          if ((xmm_cached_reg == SIZE_MAX &&
               !nyr_printf(w, "\t%s\t-%d(%%rbp), %%xmm0\n", move, a)) ||
              !nyr_printf(w, "\t%s\t-%d(%%rbp), %%xmm0\n",
                          f32 ? "divss" : "divsd", b_off))
            return false;
          bool forward = nyr_x64_mach_can_forward_xmm_result(
              mach, uses, b->first_inst + n, b->first_inst + b->inst_count, in);
          if (!forward &&
              !nyr_printf(w, "\t%s\t%%xmm0, -%d(%%rbp)\n", move, dst))
            return false;
          xmm_cached_reg = forward ? in->dst.as.reg : SIZE_MAX;
          rax_cached_off = -1;
          break;
        }
        /*
         * Match the object and NYIR emitters for compile-time divisors.  The
         * scalar text emitter is the usual x86 path, so leaving this out made
         * constant remainders in optimized loops fall back to expensive idiv.
         */
        int64_t divisor = 0;
        if (nyr_x64_mach_const_i64(mach, definitions, &in->src1, &divisor, 0) &&
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
            if (in->opcode == NYR_MACH_MOD) {
              if (!ny_x64_scalar_emit_rax_load(w, &in->src0, a,
                                               rax_cached_off) ||
                  !nyr_put(w, "\tmovq\t%rax, %rcx\n\tsarq\t$63, %rcx\n"))
                return false;
              if (ad <= (UINT64_C(1) << 31)) {
                if (!nyr_printf(w, "\tandq\t$%" PRIu64 ", %%rcx\n", mask))
                  return false;
              } else if (!ny_x64_scalar_emit_imm64(w, (int64_t)mask, "%r10") ||
                         !nyr_put(w, "\tandq\t%r10, %rcx\n")) {
                return false;
              }
              if (!nyr_put(w, "\taddq\t%rax, %rcx\n"))
                return false;
              if (ad <= (UINT64_C(1) << 31)) {
                if (!nyr_printf(w, "\tandq\t$-%" PRIu64 ", %%rcx\n", ad))
                  return false;
              } else {
                int64_t clear =
                    ad == (UINT64_C(1) << 63) ? INT64_MIN : -(int64_t)ad;
                if (!ny_x64_scalar_emit_imm64(w, clear, "%r10") ||
                    !nyr_put(w, "\tandq\t%r10, %rcx\n"))
                  return false;
              }
              if (!nyr_put(w, "\tsubq\t%rcx, %rax\n") ||
                  !nyr_printf(w, "\tmovq\t%%rax, -%d(%%rbp)\n", dst))
                return false;
              rax_cached_off = in->dst.kind == NYR_MACH_OPERAND_VREG ? dst : -1;
              break;
            }
            if (!ny_x64_scalar_emit_rax_load(w, &in->src0, a, rax_cached_off) ||
                !nyr_put(w, "\tmovq\t%rax, %rcx\n\tsarq\t$63, %rcx\n") ||
                !nyr_printf(w, "\tandq\t$%" PRIu64 ", %%rcx\n", mask) ||
                !nyr_put(w, "\taddq\t%rcx, %rax\n") ||
                !nyr_printf(w, "\tsarq\t$%u, %%rax\n", shift))
              return false;
            if (divisor < 0 && !nyr_put(w, "\tnegq\t%rax\n"))
              return false;
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
                !ny_x64_scalar_emit_imm64(w, magic, "%r10") ||
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
              } else if (!ny_x64_scalar_emit_imm64(w, (int64_t)ad, "%r11") ||
                         !nyr_put(w, "\timulq\t%r11, %r10\n")) {
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
          if (!ny_x64_scalar_emit_imm64(w, in->src1.as.imm, "%r10") ||
              !nyr_printf(w,
                          "\tcqto\n\tidivq\t%%r10\n"
                          "\tmovq\t%%%s, -%d(%%rbp)\n",
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
      case NYR_MACH_ROR: {
        size_t pc = b->first_inst + n;
        size_t end = b->first_inst + b->inst_count;
        int64_t count = 0;
        bool constant =
            nyr_x64_mach_const_i64(mach, definitions, &in->src1, &count, 0);

        if (constant)
          count = (int64_t)((uint64_t)count & UINT64_C(63));

        if (in->opcode == NYR_MACH_SHL && constant && pc + 1 < end &&
            nyr_x64_mach_can_forward_rax_result(mach, uses, pc, end, in)) {
          const nyr_mach_inst_t *next = &mach->insts[pc + 1];
          int64_t right = 0;

          if (next->opcode == NYR_MACH_SAR &&
              next->src0.kind == NYR_MACH_OPERAND_VREG &&
              next->src0.as.reg == in->dst.as.reg &&
              nyr_x64_mach_const_i64(mach, definitions, &next->src1, &right,
                                     0) &&
              count == (int64_t)((uint64_t)right & UINT64_C(63)) &&
              (count == 32 || count == 48 || count == 56)) {
            const char *ext = count == 32   ? "movslq"
                              : count == 48 ? "movswq"
                                            : "movsbq";
            const char *reg = count == 32   ? "%eax"
                              : count == 48 ? "%ax"
                                            : "%al";

            bool from_home =
                in->src0.kind == NYR_MACH_OPERAND_FRAME ||
                (in->src0.kind == NYR_MACH_OPERAND_VREG && rax_cached_off != a);

            if (from_home) {
              if (!nyr_printf(w, "\t%s\t-%d(%%rbp), %%rax\n", ext, a))
                return false;
            } else if (!ny_x64_scalar_emit_rax_load(w, &in->src0, a,
                                                    rax_cached_off) ||
                       !nyr_printf(w, "\t%s\t%s, %%rax\n", ext, reg)) {
              return false;
            }

            int next_dst = nyr_x64_mach_offset(mach, &next->dst);
            if (!nyr_x64_mach_emit_rax_result(w, mach, uses, pc + 1, end, next,
                                              next_dst, &rax_cached_off))
              return false;

            rax_cached_off =
                next->dst.kind == NYR_MACH_OPERAND_VREG ? next_dst : -1;
            ++n;
            break;
          }
        }

        if (!ny_x64_scalar_emit_rax_load(w, &in->src0, a, rax_cached_off))
          return false;

        if (constant) {
          if (count && !nyr_printf(w, "\t%s\t$%" PRId64 ", %%rax\n",
                                   in->opcode == NYR_MACH_SHL   ? "shlq"
                                   : in->opcode == NYR_MACH_SAR ? "sarq"
                                                                : "rorq",
                                   count))
            return false;
        } else if (!nyr_printf(w,
                               "\tmovq\t-%d(%%rbp), %%rcx\n"
                               "\t%s\t%%cl, %%rax\n",
                               b_off,
                               in->opcode == NYR_MACH_SHL   ? "shlq"
                               : in->opcode == NYR_MACH_SAR ? "sarq"
                                                            : "rorq")) {
          return false;
        }

        if (!nyr_x64_mach_emit_rax_result(w, mach, uses, pc, end, in, dst,
                                          &rax_cached_off))
          return false;

        rax_cached_off = in->dst.kind == NYR_MACH_OPERAND_VREG ? dst : -1;
        break;
      }
      case NYR_MACH_ROR32:
        if (!nyr_printf(w, "\tmovl\t-%d(%%rbp), %%eax\n", a))
          return false;
        {
          int64_t rotate_count = 0;
          if (nyr_x64_mach_const_i64(mach, definitions, &in->src1,
                                     &rotate_count, 0)) {
            rotate_count = (int64_t)((uint64_t)rotate_count & UINT64_C(31));
            if (!nyr_printf(w, "\trorl\t$%" PRId64 ", %%eax\n", rotate_count) ||
                !nyr_x64_mach_emit_rax_result(w, mach, uses, b->first_inst + n,
                                              b->first_inst + b->inst_count, in,
                                              dst, &rax_cached_off))
              return false;
          } else if (!nyr_printf(w,
                                 "\tmovq\t-%d(%%rbp), %%rcx\n"
                                 "\trorl\t%%cl, %%eax\n",
                                 b_off) ||
                     !nyr_x64_mach_emit_rax_result(
                         w, mach, uses, b->first_inst + n,
                         b->first_inst + b->inst_count, in, dst,
                         &rax_cached_off)) {
            return false;
          }
        }
        rax_cached_off = in->dst.kind == NYR_MACH_OPERAND_VREG ? dst : -1;
        break;
      case NYR_MACH_BSWAP64:
      case NYR_MACH_BSWAP32:
        if (!ny_x64_scalar_emit_rax_load(w, &in->src0, a, rax_cached_off))
          return false;
        if (!nyr_printf(w, "\t%s\t%%%s\n",
                        in->opcode == NYR_MACH_BSWAP64 ? "bswapq" : "bswapl",
                        in->opcode == NYR_MACH_BSWAP64 ? "rax" : "eax") ||
            !nyr_x64_mach_emit_rax_result(w, mach, uses, b->first_inst + n,
                                          b->first_inst + b->inst_count, in,
                                          dst, &rax_cached_off))
          return false;
        break;
      case NYR_MACH_SELECT: {
        int false_off = nyr_x64_mach_offset(mach, &in->src2);
        if (!ny_x64_scalar_emit_rax_load(w, &in->src0, a, rax_cached_off) ||
            !nyr_printf(w,
                        "\ttestq\t%%rax, %%rax\n"
                        "\tmovq\t-%d(%%rbp), %%rax\n"
                        "\tcmovne\t-%d(%%rbp), %%rax\n",
                        false_off, b_off) ||
            !nyr_x64_mach_emit_rax_result(w, mach, uses, b->first_inst + n,
                                          b->first_inst + b->inst_count, in,
                                          dst, &rax_cached_off))
          return false;
        break;
      }
      case NYR_MACH_CMP: {
        const nyr_mach_inst_t *reuse_branch = NULL;
        size_t block_end = b->first_inst + b->inst_count;
        if (in->dst.kind == NYR_MACH_OPERAND_VREG &&
            in->dst.as.reg < mach->vreg_len &&
            in->dst.as.reg < NY_X64_NIR_MAX_SLOTS &&
            reuse_cmp_flags[in->dst.as.reg] &&
            nyr_x64_mach_compare_branch(mach, uses, b->first_inst + n,
                                        block_end, in, &reuse_branch)) {
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
          bool rhs_constant = nyr_x64_mach_const_i64(mach, definitions,
                                                     &in->src1, &immediate, 0);
          if (!jcc ||
              !ny_x64_scalar_emit_rax_load(w, &in->src0, a, rax_cached_off))
            return false;
          if (rhs_constant && (int64_t)(int32_t)immediate == immediate) {
            if (!nyr_printf(w, "\tcmpq\t$%" PRId64 ", %%rax\n", immediate))
              return false;
          } else if (rhs_constant || in->src1.kind == NYR_MACH_OPERAND_IMM) {
            int64_t value = rhs_constant ? immediate : in->src1.as.imm;
            if (!ny_x64_scalar_emit_imm64(w, value, "%r10") ||
                !nyr_put(w, "\tcmpq\t%r10, %rax\n"))
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
        if (in->dst.kind == NYR_MACH_OPERAND_VREG &&
            in->dst.as.reg < mach->vreg_len && uses &&
            uses[in->dst.as.reg].count == 1 && n + 1 < b->inst_count &&
            !nyr_x64_mach_is_float(mach, &in->src0) &&
            (in->src0.kind == NYR_MACH_OPERAND_IMM ||
             nyr_x64_mach_is_i64(mach, &in->src0)) &&
            (in->src1.kind == NYR_MACH_OPERAND_IMM ||
             nyr_x64_mach_is_i64(mach, &in->src1))) {
          const nyr_mach_inst_t *sel = &mach->insts[b->first_inst + n + 1];
          const char *cc = nyr_x64_mach_jcc(in->condition);
          if (cc && sel->opcode == NYR_MACH_SELECT &&
              sel->src0.kind == NYR_MACH_OPERAND_VREG &&
              sel->src0.as.reg == in->dst.as.reg &&
              (sel->src1.kind == NYR_MACH_OPERAND_IMM ||
               nyr_x64_mach_is_gpr(mach, &sel->src1)) &&
              (sel->src2.kind == NYR_MACH_OPERAND_IMM ||
               nyr_x64_mach_is_gpr(mach, &sel->src2))) {
            int64_t imm = 0;
            bool const_rhs =
                nyr_x64_mach_const_i64(mach, definitions, &in->src1, &imm, 0);
            if (!ny_x64_scalar_emit_rax_load(w, &in->src0, a, rax_cached_off))
              return false;
            if (const_rhs && (int64_t)(int32_t)imm == imm) {
              if (!nyr_printf(w, "\tcmpq\t$%" PRId64 ", %%rax\n", imm))
                return false;
            } else if (const_rhs || in->src1.kind == NYR_MACH_OPERAND_IMM) {
              if (!ny_x64_scalar_emit_imm64(
                      w, const_rhs ? imm : in->src1.as.imm, "%r10") ||
                  !nyr_put(w, "\tcmpq\t%r10, %rax\n"))
                return false;
            } else if (!nyr_printf(w, "\tcmpq\t-%d(%%rbp), %%rax\n", b_off)) {
              return false;
            }
            int false_off = nyr_x64_mach_offset(mach, &sel->src2);
            int true_off = nyr_x64_mach_offset(mach, &sel->src1);
            if (!ny_x64_scalar_emit_rax_load(w, &sel->src2, false_off, -1))
              return false;
            if (sel->src1.kind == NYR_MACH_OPERAND_IMM) {
              if (!ny_x64_scalar_emit_imm64(w, sel->src1.as.imm, "%r10") ||
                  !nyr_printf(w, "\tcmov%s\t%%r10, %%rax\n", cc + 1))
                return false;
            } else if (!nyr_printf(w, "\tcmov%s\t-%d(%%rbp), %%rax\n", cc + 1,
                                   true_off)) {
              return false;
            }
            int sel_dst = nyr_x64_mach_offset(mach, &sel->dst);
            if (!nyr_x64_mach_emit_rax_result(w, mach, uses,
                                              b->first_inst + n + 1, block_end,
                                              sel, sel_dst, &rax_cached_off))
              return false;
            ++n;
            break;
          }
        }
        const nyr_mach_inst_t *branch = NULL;
        if (nyr_x64_mach_compare_branch(mach, uses, b->first_inst + n,
                                        block_end, in, &branch)) {
          const char *jcc = nyr_x64_mach_jcc(in->condition);
          int64_t immediate = 0;
          bool rhs_constant = nyr_x64_mach_const_i64(mach, definitions,
                                                     &in->src1, &immediate, 0);
          if (!ny_x64_scalar_emit_rax_load(w, &in->src0, a, rax_cached_off))
            return false;
          if (rhs_constant && (int64_t)(int32_t)immediate == immediate) {
            if (!nyr_printf(w, "\tcmpq\t$%" PRId64 ", %%rax\n", immediate))
              return false;
          } else if (rhs_constant || in->src1.kind == NYR_MACH_OPERAND_IMM) {
            int64_t value = rhs_constant ? immediate : in->src1.as.imm;
            if (!ny_x64_scalar_emit_imm64(w, value, "%r10") ||
                !nyr_put(w, "\tcmpq\t%r10, %rax\n"))
              return false;
          } else if (!nyr_printf(w, "\tcmpq\t-%d(%%rbp), %%rax\n", b_off)) {
            return false;
          }
          const nyr_mach_inst_t *fallthrough =
              n + 3 == b->inst_count ? &mach->insts[b->first_inst + n + 2]
                                     : NULL;
          bool invert = fallthrough && fallthrough->opcode == NYR_MACH_BR &&
                        fallthrough->src1.kind == NYR_MACH_OPERAND_BLOCK &&
                        block + 1 < mach->block_len &&
                        branch->src1.as.block_index == block + 1;
          const char *branch_cc =
              invert ? nyr_x64_mach_inverse_jcc(in->condition) : jcc;
          uint32_t target_block = invert ? fallthrough->src1.as.block_index
                                         : branch->src1.as.block_index;
          if (!branch_cc || !nyr_printf(w, "\t%s\t.Lny_mach_%s_%u\n", branch_cc,
                                        name, target_block))
            return false;
          n += invert ? 2 : 1;
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
          size_t compare_id = b->first_inst + n;
          snprintf(unordered, sizeof(unordered),
                   ".Lny_mach_fcmp_unordered_%s_%zu", name, compare_id);
          snprintf(done, sizeof(done), ".Lny_mach_fcmp_done_%s_%zu", name,
                   compare_id);
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
        int64_t rhs = 0;
        bool rhs_constant =
            nyr_x64_mach_const_i64(mach, definitions, &in->src1, &rhs, 0);
        if (rhs_constant && (int64_t)(int32_t)rhs == rhs) {
          if (!nyr_printf(w, "\tcmpq\t$%" PRId64 ", %%rax\n", rhs))
            return false;
        } else if (rhs_constant || in->src1.kind == NYR_MACH_OPERAND_IMM) {
          if (!ny_x64_scalar_emit_imm64(w, rhs_constant ? rhs : in->src1.as.imm,
                                        "%r10") ||
              !nyr_put(w, "\tcmpq\t%r10, %rax\n"))
            return false;
        } else if (!nyr_printf(w, "\tcmpq\t-%d(%%rbp), %%rax\n", b_off)) {
          return false;
        }
        if (!nyr_printf(w, "\t%s\t%%al\n\tmovzbq\t%%al, %%rax\n", cc) ||
            !nyr_x64_mach_emit_rax_result(w, mach, uses, b->first_inst + n,
                                          b->first_inst + b->inst_count, in,
                                          dst, &rax_cached_off))
          return false;
        break;
      }
      case NYR_MACH_BR:
        if (!(n + 1 == b->inst_count && block + 1 < mach->block_len &&
              in->src1.kind == NYR_MACH_OPERAND_BLOCK &&
              in->src1.as.block_index == block + 1) &&
            !nyr_printf(w, "\tjmp\t.Lny_mach_%s_%u\n", name,
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
        if (n + 2 == b->inst_count && block + 1 < mach->block_len &&
            in->src1.kind == NYR_MACH_OPERAND_BLOCK &&
            in->src1.as.block_index == block + 1 &&
            mach->insts[b->first_inst + n + 1].opcode == NYR_MACH_BR &&
            mach->insts[b->first_inst + n + 1].src1.kind ==
                NYR_MACH_OPERAND_BLOCK) {
          const nyr_mach_inst_t *fallthrough =
              &mach->insts[b->first_inst + n + 1];
          if (!nyr_printf(w,
                          "\tcmpq\t$0, -%d(%%rbp)\n"
                          "\tje\t.Lny_mach_%s_%u\n",
                          a, name, fallthrough->src1.as.block_index))
            return false;
          ++n;
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
        if (in->call_is_variadic && target->abi != NY_NATIVE_ABI_SYSV)
          return false;
        if (target->syscalls.available &&
            in->src0.kind == NYR_MACH_OPERAND_SYMBOL && in->src0.as.symbol &&
            ny_native_runtime_operation(in->src0.as.symbol) ==
                NY_NATIVE_RUNTIME_OP_LINUX_SYSCALL) {
          static const char *const syscall_arg_regs[] = {"%rdi", "%rsi", "%rdx",
                                                         "%r10", "%r8",  "%r9"};
          if (in->args_len < 1 || in->args_len > 7)
            return false;
          for (size_t arg = 1; arg < 7; ++arg) {
            if (arg < in->args_len) {
              int arg_off = nyr_x64_mach_offset(mach, &in->args[arg]);
              if (!ny_x64_scalar_emit_rax_load(w, &in->args[arg], arg_off,
                                               rax_cached_off) ||
                  !nyr_printf(w, "\tmovq\t%%rax, %s\n",
                              syscall_arg_regs[arg - 1]))
                return false;
            } else if (!nyr_printf(w, "\txorq\t%s, %s\n",
                                   syscall_arg_regs[arg - 1],
                                   syscall_arg_regs[arg - 1])) {
              return false;
            }
          }
          int syscall_number_off = nyr_x64_mach_offset(mach, &in->args[0]);
          if (!ny_x64_scalar_emit_rax_load(w, &in->args[0], syscall_number_off,
                                           rax_cached_off) ||
              !nyr_put(w, "\tsyscall\n"))
            return false;
          if (in->dst.kind != NYR_MACH_OPERAND_NONE &&
              !nyr_printf(w, "\tmovq\t%%rax, -%d(%%rbp)\n", dst))
            return false;
          rax_cached_off = in->dst.kind == NYR_MACH_OPERAND_VREG ? dst : -1;
          break;
        }
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
        size_t fp_register_args = fp;
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
        bool tail_self =
            !tag_return && target->abi == NY_NATIVE_ABI_SYSV &&
            !in->call_is_variadic && stack_bytes == 0 &&
            in->src0.kind == NYR_MACH_OPERAND_SYMBOL && in->src0.as.symbol &&
            in->call_is_extern && strcmp(target->object_format, "elf") == 0 &&
            strcmp(in->src0.as.symbol, name) == 0 &&
            in->dst.kind == NYR_MACH_OPERAND_VREG &&
            !nyr_x64_mach_is_float(mach, &in->dst) && n + 1 < b->inst_count;
        if (tail_self) {
          const nyr_mach_inst_t *ret = &mach->insts[b->first_inst + n + 1];
          tail_self = ret->opcode == NYR_MACH_RET &&
                      nyr_x64_mach_operand_equal(&ret->src0, &in->dst);
          if (tail_self && !tail_frame_checked) {
            tail_frame_checked = true;
            for (size_t k = 0; k < mach->inst_len; ++k) {
              const nyr_mach_inst_t *v = &mach->insts[k];
              if ((v->opcode == NYR_MACH_LEA &&
                   v->src0.kind == NYR_MACH_OPERAND_FRAME) ||
                  v->opcode == NYR_MACH_VA_SAVE ||
                  v->opcode == NYR_MACH_VA_START) {
                tail_frame_unsafe = true;
                break;
              }
            }
          }
          tail_self = tail_self && !tail_frame_unsafe;
        }
        if (tail_self) {
          if (!nyr_printf(w, "\tleave\n\tjmp\t%s%s\n", target->symbol_prefix,
                          name))
            return false;
          ++n;
          rax_cached_off = -1;
          break;
        }
        if (in->call_is_variadic &&
            !nyr_printf(w, "\tmovb\t$%zu, %%al\n", fp_register_args))
          return false;
        if (in->src0.kind == NYR_MACH_OPERAND_SYMBOL) {
          if (!in->src0.as.symbol ||
              !nyr_printf(w, "\tcall\t%s%s%s\n", target->symbol_prefix,
                          !in->call_is_extern &&
                                  strncmp(in->src0.as.symbol, "ny_fn_", 6) != 0
                              ? "ny_fn_"
                              : "",
                          in->src0.as.symbol))
            return false;
        } else if (!nyr_printf(w, "\tcall\t*-%d(%%rbp)\n",
                               nyr_x64_mach_offset(mach, &in->src0))) {
          return false;
        }
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
      !nyr_put(w, function_return_thunk_extern
                      ? "\tleave\n\tjmp\t__x86_return_thunk\n"
                      : "\tleave\n\tret\n"))
    return false;
  if (strcmp(target->object_format, "macho") != 0 &&
      !nyr_printf(w, "\t.size\t%s%s, .-%s%s\n", sym, name, sym, name))
    return false;
  (void)err;
  (void)err_len;
  return true;
}

bool nyr_x86_64_emit_mach_scalar(nyr_writer_t *w,
                                 const ny_native_target_info_t *target,
                                 const nyr_mach_func_t *mach,
                                 const char *func_name, bool tag_return,
                                 bool function_return_thunk_extern, char *err,
                                 size_t err_len) {
  /*
   * 16- and 32-bit accesses exist only in the object encoder.
   */
  for (size_t mi = 0; mach && mi < mach->inst_len; ++mi)
    if (mach->insts[mi].mem_size >= 2 || mach->insts[mi].sext32 ||
        mach->insts[mi].unsigned_arith ||
        mach->insts[mi].opcode == NYR_MACH_SHR)
      return false;
  nyr_x64_mach_use_t *uses = nyr_x64_mach_count_uses(mach);
  size_t *definitions = nyr_x64_mach_index_definitions(mach);
  bool ok = nyr_x86_64_emit_mach_scalar_impl(
      w, target, mach, func_name, tag_return, function_return_thunk_extern, err,
      err_len, uses, definitions);
  free(definitions);
  free(uses);
  return ok;
}
