/*
 * Machine-form IR: target-specific intermediate representation
 * between NYIR and object code with register operands and addressing modes.
 */
#include "code/backend/lower/machine.h"
#include "base/debug.h"
#include "base/trace.h"
#include "code/backend/core/internal.h"
#include "code/backend/core/nyr.h"
#include "code/backend/passes/util.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <unistd.h>
#endif

static bool mach_sink_edge_adds(nyr_mach_func_t *m);

static bool mach_grow(void **data, size_t *cap, size_t elem_size) {
  size_t next = *cap ? *cap * 2 : 8;
  size_t bytes = 0;
  if (next < *cap || !nyr_size_mul_ok(next, elem_size, &bytes))
    return false;
  void *grown = realloc(*data, bytes);
  if (!grown)
    return false;
  *data = grown;
  *cap = next;
  return true;
}

/*
 * Duplicate a tiny, pure loop guard at a single-block latch. The initial
 * guard remains in place (including for zero trips); subsequent iterations
 * branch directly to the body rather than through an unconditional backedge
 * and a second branch. Do this after phi-edge lowering and copy propagation,
 * but before allocation: machine vregs, unlike NYIR SSA values, may have
 * multiple definitions. No memory operations or trapping guards are cloned.
 */
static bool mach_rotate_scalar_loops(nyr_mach_func_t *m) {
  for (size_t b = 2; b < m->block_len; ++b) {
    nyr_mach_block_t *body = &m->blocks[b];
    if (!body->inst_count)
      continue;
    size_t latch = body->first_inst + body->inst_count - 1;
    const nyr_mach_inst_t back = m->insts[latch];
    if (back.opcode != NYR_MACH_BR ||
        back.src1.kind != NYR_MACH_OPERAND_BLOCK ||
        back.src1.as.block_index + 2 != b)
      continue;
    const nyr_mach_block_t *head = &m->blocks[b - 2];
    const nyr_mach_block_t *false_edge = &m->blocks[b - 1];
    size_t guard_count = head->inst_count;
    size_t exit_count = false_edge->inst_count;
    if (guard_count < 2 || guard_count > 6 || exit_count < 1 || exit_count > 4)
      continue;
    /*
     * Leave loops with loop-carried (multi-def or live-across-backedge) vector
     * values unrotated: coalesce_loop_edges needs a latch distinct from the
     * header (`pred > h`) to keep mutable vector PHIs resident across the
     * backedge. Invariant vectors (`carry_invariant_vectors` handles `>= h`)
     * and loop-local vector temporaries rotate safely into single-branch loops.
     */
    bool has_carried_vector = false;
    for (size_t pc = body->first_inst; pc <= latch && !has_carried_vector; ++pc) {
      const nyr_mach_inst_t *in = &m->insts[pc];
      if (in->dst.kind != NYR_MACH_OPERAND_VREG ||
          in->dst.as.reg >= m->vreg_len)
        continue;
      nyr_mach_type_t dt = m->vreg_types[in->dst.as.reg];
      bool is_vec = dt == NYR_MACH_TYPE_V256_I64 ||
                    dt == NYR_MACH_TYPE_V128_I64 ||
                    dt == NYR_MACH_TYPE_V256_F64 ||
                    dt == NYR_MACH_TYPE_V128_F64 ||
                    dt == NYR_MACH_TYPE_V128_F32;
      if (!is_vec)
        continue;
      uint32_t v = in->dst.as.reg;
      for (size_t k = 0; k < m->inst_len && !has_carried_vector; ++k) {
        if (k == pc)
          continue;
        const nyr_mach_inst_t *o = &m->insts[k];
        if (o->dst.kind == NYR_MACH_OPERAND_VREG && o->dst.as.reg == v)
          has_carried_vector = true;
        else if (k < pc || k > latch) {
          if ((o->src0.kind == NYR_MACH_OPERAND_VREG && o->src0.as.reg == v) ||
              (o->src1.kind == NYR_MACH_OPERAND_VREG && o->src1.as.reg == v) ||
              (o->src2.kind == NYR_MACH_OPERAND_VREG && o->src2.as.reg == v))
            has_carried_vector = true;
        }
      }
    }
    if (has_carried_vector)
      continue;
    const nyr_mach_inst_t *guard = &m->insts[head->first_inst];
    const nyr_mach_inst_t *cmp = &guard[guard_count - 2];
    const nyr_mach_inst_t *branch = &guard[guard_count - 1];
    const nyr_mach_inst_t *exit_insts = &m->insts[false_edge->first_inst];
    const nyr_mach_inst_t *exit = &exit_insts[exit_count - 1];
    if (cmp->opcode != NYR_MACH_CMP || cmp->dst.kind != NYR_MACH_OPERAND_VREG ||
        cmp->dst.reg_class != NYR_MACH_REGCLASS_GPR ||
        branch->opcode != NYR_MACH_BR_IF ||
        branch->src0.kind != NYR_MACH_OPERAND_VREG ||
        branch->src0.as.reg != cmp->dst.as.reg ||
        branch->src1.kind != NYR_MACH_OPERAND_BLOCK ||
        branch->src1.as.block_index != b || branch->args || branch->arg_sizes ||
        exit->opcode != NYR_MACH_BR ||
        exit->src1.kind != NYR_MACH_OPERAND_BLOCK ||
        exit->src1.as.block_index != b + 1 || exit->args || exit->arg_sizes)
      continue;
    bool safe = true;
    for (size_t n = 0; n + 1 < guard_count; ++n) {
      const nyr_mach_inst_t *in = &guard[n];
      safe &= (in->opcode == NYR_MACH_COPY || in->opcode == NYR_MACH_CMP ||
               in->opcode == NYR_MACH_ADD || in->opcode == NYR_MACH_XOR) &&
              !in->effects && !in->args && !in->arg_sizes &&
              in->dst.kind == NYR_MACH_OPERAND_VREG &&
              in->dst.reg_class == NYR_MACH_REGCLASS_GPR &&
              (in->src0.kind == NYR_MACH_OPERAND_VREG ||
               in->src0.kind == NYR_MACH_OPERAND_IMM) &&
              (in->src1.kind == NYR_MACH_OPERAND_NONE ||
               in->src1.kind == NYR_MACH_OPERAND_VREG ||
               in->src1.kind == NYR_MACH_OPERAND_IMM);
    }
    for (size_t n = 0; n + 1 < exit_count; ++n) {
      const nyr_mach_inst_t *in = &exit_insts[n];
      safe &= in->opcode == NYR_MACH_COPY && !in->effects && !in->args &&
              !in->arg_sizes && in->dst.kind == NYR_MACH_OPERAND_VREG &&
              in->dst.reg_class == NYR_MACH_REGCLASS_GPR &&
              (in->src0.kind == NYR_MACH_OPERAND_VREG ||
               in->src0.kind == NYR_MACH_OPERAND_IMM);
    }
    for (size_t pc = body->first_inst; pc < latch; ++pc) {
      nyr_mach_opcode_t op = m->insts[pc].opcode;
      if (op == NYR_MACH_BR || op == NYR_MACH_BR_IF || op == NYR_MACH_RET ||
          op == NYR_MACH_TRAP || op == NYR_MACH_CALL)
        safe = false;
    }
    if (!safe)
      continue;
    if (exit_count == 1) {
      size_t count = guard_count + 1;
      nyr_mach_inst_t cloned[8];
      memcpy(cloned, guard, guard_count * sizeof(*cloned));
      cloned[guard_count] = *exit;
      while (m->inst_cap - m->inst_len < count - 1)
        if (!mach_grow((void **)&m->insts, &m->inst_cap, sizeof(*m->insts)))
          return false;
      memmove(&m->insts[latch + count], &m->insts[latch + 1],
              (m->inst_len - latch - 1) * sizeof(*m->insts));
      memcpy(&m->insts[latch], cloned, count * sizeof(*cloned));
      m->inst_len += count - 1;
      body->inst_count += count - 1;
      for (size_t next = b + 1; next < m->block_len; ++next)
        m->blocks[next].first_inst += count - 1;
    } else {
      /*
       * false_edge carries exit PHI copies before branching to b+1. End body
       * with `guard` (ending in BR_IF -> b) and insert a fresh fallthrough
       * exit block at b+1 containing a clone of false_edge, shifting targets
       * >= b+1 by 1 so block terminators and layout backedges stay valid.
       */
      uint32_t max_label = 0;
      for (size_t bi = 0; bi < m->block_len; ++bi)
        if (m->blocks[bi].label != UINT32_MAX &&
            m->blocks[bi].label > max_label)
          max_label = m->blocks[bi].label;
      if (max_label >= UINT32_MAX - 1)
        continue;
      uint32_t new_label = max_label + 1;
      size_t total_add = guard_count + exit_count - 1;
      nyr_mach_inst_t cloned_guard[6];
      nyr_mach_inst_t cloned_exit[4];
      memcpy(cloned_guard, guard, guard_count * sizeof(*cloned_guard));
      memcpy(cloned_exit, exit_insts, exit_count * sizeof(*cloned_exit));
      while (m->inst_cap - m->inst_len < total_add)
        if (!mach_grow((void **)&m->insts, &m->inst_cap, sizeof(*m->insts)))
          return false;
      if (m->block_len == m->block_cap &&
          !mach_grow((void **)&m->blocks, &m->block_cap, sizeof(*m->blocks)))
        return false;
      body = &m->blocks[b];
      memmove(&m->insts[latch + guard_count + exit_count],
              &m->insts[latch + 1],
              (m->inst_len - latch - 1) * sizeof(*m->insts));
      memcpy(&m->insts[latch], cloned_guard,
             guard_count * sizeof(*cloned_guard));
      memcpy(&m->insts[latch + guard_count], cloned_exit,
             exit_count * sizeof(*cloned_exit));
      m->inst_len += total_add;
      body->inst_count += (uint32_t)(guard_count - 1);
      memmove(&m->blocks[b + 2], &m->blocks[b + 1],
              (m->block_len - (b + 1)) * sizeof(*m->blocks));
      m->block_len += 1;
      m->blocks[b + 1] = (nyr_mach_block_t){
          .first_inst = (uint32_t)(latch + guard_count),
          .inst_count = (uint32_t)exit_count,
          .label = new_label,
          .source_pc = m->blocks[b - 1].source_pc};
      if (new_label > m->max_block_label)
        m->max_block_label = new_label;
      for (size_t next = b + 2; next < m->block_len; ++next)
        m->blocks[next].first_inst += (uint32_t)total_add;
      for (size_t pc = 0; pc < m->inst_len; ++pc) {
        nyr_mach_inst_t *in = &m->insts[pc];
        if (in->src0.kind == NYR_MACH_OPERAND_BLOCK &&
            in->src0.as.block_index >= b + 1)
          in->src0.as.block_index += 1;
        if (in->src1.kind == NYR_MACH_OPERAND_BLOCK &&
            in->src1.as.block_index >= b + 1)
          in->src1.as.block_index += 1;
      }
      ++b;
    }
  }
  return true;
}

/*
 * The same rotation for loops whose header is not directly before the latch,
 * typically a chain of exits such as `while j >= 0 && a[j] > key`. The latch's
 * unconditional backedge is replaced by a copy of the header's guard: its
 * pure compares, the conditional branch to the header's taken successor, and
 * the branch the header's fall-through block makes. That copy runs exactly
 * where the original header would have, so what lies between header and latch
 * does not matter. Only the jump back to the header disappears.
 */
static bool mach_rotate_chained_loops(nyr_mach_func_t *m) {
  for (size_t b = 1; b < m->block_len; ++b) {
    nyr_mach_block_t *body = &m->blocks[b];
    if (!body->inst_count)
      continue;
    size_t latch = body->first_inst + body->inst_count - 1;
    const nyr_mach_inst_t back = m->insts[latch];
    if (back.opcode != NYR_MACH_BR || back.args || back.arg_sizes ||
        back.src1.kind != NYR_MACH_OPERAND_BLOCK)
      continue;
    size_t h = back.src1.as.block_index;
    if (h + 2 >= b)
      continue; /* adjacent shapes belong to mach_rotate_scalar_loops */
    const nyr_mach_block_t *head = &m->blocks[h];
    const nyr_mach_block_t *false_edge = &m->blocks[h + 1];
    size_t guard_count = head->inst_count;
    if (guard_count < 2 || guard_count > 5 || false_edge->inst_count != 1)
      continue;
    const nyr_mach_inst_t *guard = &m->insts[head->first_inst];
    const nyr_mach_inst_t *cmp = &guard[guard_count - 2];
    const nyr_mach_inst_t *branch = &guard[guard_count - 1];
    const nyr_mach_inst_t *exit = &m->insts[false_edge->first_inst];
    if (cmp->opcode != NYR_MACH_CMP || cmp->dst.kind != NYR_MACH_OPERAND_VREG ||
        cmp->dst.reg_class != NYR_MACH_REGCLASS_GPR ||
        branch->opcode != NYR_MACH_BR_IF ||
        branch->src0.kind != NYR_MACH_OPERAND_VREG ||
        branch->src0.as.reg != cmp->dst.as.reg ||
        branch->src1.kind != NYR_MACH_OPERAND_BLOCK ||
        branch->src1.as.block_index <= h + 1 ||
        branch->src1.as.block_index > b || branch->args || branch->arg_sizes ||
        exit->opcode != NYR_MACH_BR ||
        exit->src1.kind != NYR_MACH_OPERAND_BLOCK || exit->args ||
        exit->arg_sizes)
      continue;
    bool safe = true;
    for (size_t n = 0; n + 1 < guard_count; ++n) {
      const nyr_mach_inst_t *in = &guard[n];
      safe &= (in->opcode == NYR_MACH_COPY || in->opcode == NYR_MACH_CMP) &&
              !in->effects && !in->args && !in->arg_sizes &&
              in->dst.kind == NYR_MACH_OPERAND_VREG &&
              in->dst.reg_class == NYR_MACH_REGCLASS_GPR &&
              (in->src0.kind == NYR_MACH_OPERAND_VREG ||
               in->src0.kind == NYR_MACH_OPERAND_IMM) &&
              (in->src1.kind == NYR_MACH_OPERAND_NONE ||
               in->src1.kind == NYR_MACH_OPERAND_VREG ||
               in->src1.kind == NYR_MACH_OPERAND_IMM);
    }
    /*
     * As above, vector carries need a latch distinct from the header, and
     * calls inside the loop keep it out of the scalar carry contract.
     */
    for (size_t pc = head->first_inst; safe && pc <= latch; ++pc) {
      const nyr_mach_inst_t *in = &m->insts[pc];
      if (in->opcode == NYR_MACH_CALL || in->opcode == NYR_MACH_INLINE_ASM ||
          in->opcode == NYR_MACH_INTRINSIC)
        safe = false;
      if (in->dst.kind == NYR_MACH_OPERAND_VREG &&
          in->dst.as.reg < m->vreg_len) {
        nyr_mach_type_t t = m->vreg_types[in->dst.as.reg];
        if (t == NYR_MACH_TYPE_V256_I64 || t == NYR_MACH_TYPE_V128_I64 ||
            t == NYR_MACH_TYPE_V256_F64 || t == NYR_MACH_TYPE_V128_F64 ||
            t == NYR_MACH_TYPE_V128_F32)
          safe = false;
      }
    }
    if (!safe)
      continue;
    size_t count = guard_count + 1;
    nyr_mach_inst_t cloned[6];
    memcpy(cloned, guard, guard_count * sizeof(*cloned));
    cloned[guard_count] = *exit;
    while (m->inst_cap - m->inst_len < count - 1)
      if (!mach_grow((void **)&m->insts, &m->inst_cap, sizeof(*m->insts)))
        return false;
    memmove(&m->insts[latch + count], &m->insts[latch + 1],
            (m->inst_len - latch - 1) * sizeof(*m->insts));
    memcpy(&m->insts[latch], cloned, count * sizeof(*cloned));
    m->inst_len += count - 1;
    body->inst_count += count - 1;
    for (size_t next = b + 1; next < m->block_len; ++next)
      m->blocks[next].first_inst += count - 1;
  }
  return true;
}

/*
 * Formal parameters occupy a dense frame-slot prefix. Older hand-built
 * machine functions infer the prefix from an incoming frame load.
 */
bool nyr_mach_param_slots(const nyr_mach_func_t *mach, bool *params) {
  if (!mach || (!params && mach->frame_slot_len))
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
  free(stored);
  return true;
}

bool nyr_mach_sysv_aggregate_parameter(uint32_t packed, size_t gp_limit,
                                       size_t fp_limit, size_t *gp, size_t *fp,
                                       nyr_mach_sysv_aggregate_t *out) {
  if (!gp || !fp || !out)
    return false;
  *out =
      (nyr_mach_sysv_aggregate_t){.size = NYIR_ARG_AGG_SIZE(packed),
                                  .classes = {NYIR_ARG_AGG_CLASS(packed, 0),
                                              NYIR_ARG_AGG_CLASS(packed, 1)}};
  if (!out->size)
    return false;
  out->chunks = (out->size + 7u) / 8u;
  if (out->classes[0] == NYIR_ARG_CLASS_MEMORY)
    return true;
  if (out->size > 16)
    return false;
  size_t gp_need = 0, fp_need = 0;
  for (unsigned i = 0; i < out->chunks; ++i) {
    if (out->classes[i] == NYIR_ARG_CLASS_INTEGER)
      ++gp_need;
    else if (out->classes[i] == NYIR_ARG_CLASS_SSE)
      ++fp_need;
    else if (out->classes[i] != NYIR_ARG_CLASS_NONE)
      return false;
  }
  out->in_registers = *gp <= gp_limit && gp_need <= gp_limit - *gp &&
                      *fp <= fp_limit && fp_need <= fp_limit - *fp;
  if (out->in_registers) {
    *gp += gp_need;
    *fp += fp_need;
  }
  return true;
}

void nyr_mach_func_free(nyr_mach_func_t *func) {
  if (!func)
    return;
  for (size_t i = 0; i < func->inst_len; ++i) {
    free(func->insts[i].args);
    free(func->insts[i].arg_sizes);
  }
  free(func->insts);
  free(func->blocks);
  free(func->frame_slots);
  free(func->param_types);
  free(func->param_abi);
  free(func->param_aggregate_home_slots);
  free(func->vreg_classes);
  free(func->vreg_types);
  free(func->home_slots);
  *func = (nyr_mach_func_t){0};
}

typedef struct {
  uint32_t reg;
  uint32_t block;
  size_t start;
  size_t end;
  size_t def_inst;
} mach_home_interval_t;

/*
 * Untargeted labels inside a straight-line region are not machine control
 * flow boundaries. Merge them before scheduling and allocation so local
 * copies and transient register results can flow through the whole region.
 */
static bool mach_merge_fallthrough_blocks(nyr_mach_func_t *m) {
  if (m->block_len < 2)
    return true;
  if (m->block_len > SIZE_MAX / (sizeof(uint32_t) * 4))
    return false;
  uint32_t *scratch = calloc(m->block_len * 4, sizeof(*scratch));
  if (!scratch)
    return false;
  uint32_t *targeted = scratch;
  uint32_t *target_count = scratch + m->block_len;
  uint32_t *target_pred = scratch + m->block_len * 2;
  uint32_t *map = scratch + m->block_len * 3;
  for (size_t b = 0; b < m->block_len; ++b)
    target_pred[b] = UINT32_MAX;
  for (size_t b = 0; b < m->block_len; ++b) {
    const nyr_mach_block_t *block = &m->blocks[b];
    for (size_t n = 0; n < block->inst_count; ++n) {
      const nyr_mach_inst_t *in = &m->insts[block->first_inst + n];
      if (in->src1.kind != NYR_MACH_OPERAND_BLOCK ||
          in->src1.as.block_index >= m->block_len)
        continue;
      uint32_t target = in->src1.as.block_index;
      targeted[target] = true;
      if (in->opcode != NYR_MACH_BR && in->opcode != NYR_MACH_BR_IF)
        continue;
      if (target_count[target] == 0)
        target_pred[target] = (uint32_t)b;
      else
        target_pred[target] = UINT32_MAX;
      if (target_count[target] < 2)
        ++target_count[target];
    }
  }
  size_t count = 0;
  for (size_t b = 0; b < m->block_len; ++b) {
    nyr_mach_block_t block = m->blocks[b];
    bool merge =
        count && (!targeted[b] || (target_count[b] == 1 &&
                                   target_pred[b] == (uint32_t)(b - 1)));
    if (merge) {
      nyr_mach_block_t *prev = &m->blocks[count - 1];
      merge = prev->first_inst + prev->inst_count == block.first_inst;
      if (merge && prev->inst_count) {
        nyr_mach_inst_t *last = &m->insts[block.first_inst - 1];
        if (last->opcode == NYR_MACH_BR) {
          merge = last->src1.kind == NYR_MACH_OPERAND_BLOCK &&
                  last->src1.as.block_index == b && target_count[b] == 1 &&
                  target_pred[b] == (uint32_t)(b - 1);
          if (merge)
            *last = (nyr_mach_inst_t){.opcode = NYR_MACH_NOP,
                                      .source_pc = last->source_pc};
        } else {
          merge = last->opcode != NYR_MACH_BR_IF &&
                  last->opcode != NYR_MACH_RET && last->opcode != NYR_MACH_TRAP;
        }
      }
      if (merge)
        prev->inst_count += block.inst_count;
    }
    if (!merge)
      m->blocks[count++] = block;
    map[b] = (uint32_t)(count - 1);
  }
  for (size_t i = 0; i < m->inst_len; ++i) {
    nyr_mach_inst_t *in = &m->insts[i];
    if (in->src1.kind == NYR_MACH_OPERAND_BLOCK &&
        in->src1.as.block_index < m->block_len)
      in->src1.as.block_index = map[in->src1.as.block_index];
  }
  m->block_len = count;
  free(scratch);
  return true;
}

static void mach_compact_nops(nyr_mach_func_t *m) {
  size_t next = 0;
  for (size_t b = 0; b < m->block_len; ++b) {
    nyr_mach_block_t *block = &m->blocks[b];
    size_t first = block->first_inst, count = block->inst_count;
    block->first_inst = (uint32_t)next;
    for (size_t n = 0; n < count; ++n) {
      nyr_mach_inst_t in = m->insts[first + n];
      if (in.opcode == NYR_MACH_NOP) {
        free(in.args);
        free(in.arg_sizes);
      } else {
        m->insts[next++] = in;
      }
    }
    block->inst_count = (uint32_t)(next - block->first_inst);
  }
  m->inst_len = next;
}

/*
 * Immediate/address folding can leave a literal's definition behind after
 * its last vreg use disappeared. Remove only truly unread pure literals;
 * rematerializable values with real consumers still keep their definitions
 * and canonical homes. STORE's destination is an address input.
 */
static bool mach_remove_unused_literals(nyr_mach_func_t *m) {
  if (!m || !m->vreg_len || !m->inst_len)
    return true;
  if (m->vreg_len > SIZE_MAX / sizeof(size_t) ||
      m->inst_len > SIZE_MAX / sizeof(size_t))
    return false;
  size_t *reads = calloc(m->vreg_len, sizeof(*reads));
  size_t *owner = malloc(m->vreg_len * sizeof(*owner));
  uint8_t *defs = calloc(m->vreg_len, sizeof(*defs));
  size_t *work = malloc(m->inst_len * sizeof(*work));
  if (!reads || !owner || !defs || !work) {
    free(reads);
    free(owner);
    free(defs);
    free(work);
    return false;
  }
  for (size_t v = 0; v < m->vreg_len; ++v)
    owner[v] = SIZE_MAX;
  for (size_t i = 0; i < m->inst_len; ++i) {
    const nyr_mach_inst_t *in = &m->insts[i];
    if (in->opcode == NYR_MACH_NOP)
      continue;
    if (in->opcode != NYR_MACH_STORE && in->dst.kind == NYR_MACH_OPERAND_VREG &&
        in->dst.as.reg < m->vreg_len) {
      uint32_t v = in->dst.as.reg;
      if (defs[v]++ == 0)
        owner[v] = i;
      if (defs[v] > 2)
        defs[v] = 2;
    }
    const nyr_mach_operand_t *ops[] = {&in->src0, &in->src1, &in->src2,
                                       in->opcode == NYR_MACH_STORE ? &in->dst
                                                                    : NULL};
    for (size_t k = 0; k < 4; ++k) {
      const nyr_mach_operand_t *op = ops[k];
      if (op && op->kind == NYR_MACH_OPERAND_VREG && op->as.reg < m->vreg_len)
        ++reads[op->as.reg];
    }
    for (size_t k = 0; k < in->args_len; ++k)
      if (in->args[k].kind == NYR_MACH_OPERAND_VREG &&
          in->args[k].as.reg < m->vreg_len)
        ++reads[in->args[k].as.reg];
  }
  size_t head = 0, tail = 0, removed = 0;
  for (size_t v = 0; v < m->vreg_len; ++v)
    if (defs[v] == 1 && reads[v] == 0)
      work[tail++] = owner[v];
  while (head < tail) {
    nyr_mach_inst_t *in = &m->insts[work[head++]];
    if (in->dst.kind != NYR_MACH_OPERAND_VREG ||
        in->dst.as.reg >= m->vreg_len || reads[in->dst.as.reg] != 0 ||
        defs[in->dst.as.reg] != 1 || in->source_pc == UINT32_MAX ||
        in->effects != NYR_MACH_EFFECT_NONE || in->args_len || in->args ||
        in->arg_sizes || in->call_clobbers || in->call_is_extern ||
        in->call_has_sret || in->call_is_variadic)
      continue;
    switch (in->opcode) {
    case NYR_MACH_COPY:
    case NYR_MACH_LEA:
    case NYR_MACH_ADD:
    case NYR_MACH_SUB:
    case NYR_MACH_MUL:
    case NYR_MACH_AND:
    case NYR_MACH_OR:
    case NYR_MACH_XOR:
    case NYR_MACH_BSWAP64:
    case NYR_MACH_BSWAP32:
      break;
    default:
      continue;
    }
    if (in->opcode != NYR_MACH_COPY && in->opcode != NYR_MACH_LEA &&
        (!m->vreg_types || m->vreg_types[in->dst.as.reg] != NYR_MACH_TYPE_I64))
      continue;
    const nyr_mach_operand_t *ops[] = {&in->src0, &in->src1, &in->src2};
    for (size_t k = 0; k < 3; ++k) {
      const nyr_mach_operand_t *op = ops[k];
      if (op->kind != NYR_MACH_OPERAND_VREG || op->as.reg >= m->vreg_len)
        continue;
      uint32_t v = op->as.reg;
      if (reads[v] && --reads[v] == 0 && defs[v] == 1)
        work[tail++] = owner[v];
    }
    uint32_t source_pc = in->source_pc;
    *in = (nyr_mach_inst_t){.opcode = NYR_MACH_NOP, .source_pc = source_pc};
    ++removed;
  }
  if (removed && ny_dbg_is_enabled(NY_DBG_HEURISTIC))
    ny_dbg_heuristic_event("MACHINE_DCE", "removed=%zu", removed);
  free(reads);
  free(owner);
  free(defs);
  free(work);
  return true;
}

static int mach_home_interval_cmp(const void *pa, const void *pb) {
  const mach_home_interval_t *a = pa;
  const mach_home_interval_t *b = pb;
  if (a->block != b->block)
    return a->block < b->block ? -1 : 1;
  if (a->start != b->start)
    return a->start < b->start ? -1 : 1;
  return a->reg < b->reg ? -1 : a->reg > b->reg;
}

/*
 * Color vregs whose definitions and every use stay in one machine block.
 * Cross-block values retain canonical homes, so branches, loops, and PHI edge
 * copies need no special alias reasoning. Immutable SSA COPY values can share
 * the source's home even across blocks: no instruction can redefine that
 * source value, and the copy emits no new bits. Within a block, ordinary
 * interval coloring is exact; a dying COPY may additionally reuse its source
 * interval's slot.
 *
 * Conservative SSA copy coalescing. PHI destruction leaves a COPY on every
 * edge, and each one costs a home load and store per iteration. A copy can go
 * when its two values never hold different live contents: at every other
 * definition of either value, the partner must be dead.
 */
static inline bool mach_cc_is_def(const nyr_mach_inst_t *in) {
  return in->dst.kind == NYR_MACH_OPERAND_VREG && in->opcode != NYR_MACH_STORE;
}

static inline bool mach_cc_uses(const nyr_mach_inst_t *in, uint32_t v) {
  const nyr_mach_operand_t *ops[] = {&in->src0, &in->src1, &in->src2};
  for (size_t k = 0; k < 3; ++k)
    if (ops[k]->kind == NYR_MACH_OPERAND_VREG && ops[k]->as.reg == v)
      return true;
  if (in->opcode == NYR_MACH_STORE && in->dst.kind == NYR_MACH_OPERAND_VREG &&
      in->dst.as.reg == v)
    return true;
  for (size_t k = 0; k < in->args_len; ++k)
    if (in->args[k].kind == NYR_MACH_OPERAND_VREG && in->args[k].as.reg == v)
      return true;
  return false;
}

static inline void mach_cc_rename(nyr_mach_operand_t *op, uint32_t from,
                                  uint32_t to) {
  if (op->kind == NYR_MACH_OPERAND_VREG && op->as.reg == from)
    op->as.reg = to;
}

#define MACH_CC_BIT(set, block, v)                                             \
  (((set)[(block) * words + (v) / 64] >> ((v) % 64)) & 1u)

/*
 * One block that defines a vreg; a vreg's entries form a linked list.
 */
typedef struct {
  uint32_t block;
  uint32_t next;
} mach_cc_def_block_t;
#define MACH_CC_NO_NODE UINT32_MAX

/*
 * Interference is only observable at a definition of d or s, so only the
 * blocks that define one of them need a scan.
 */
static bool mach_cc_interferes(const nyr_mach_func_t *m, const uint64_t *out,
                               size_t words, uint32_t d, uint32_t s,
                               const mach_cc_def_block_t *nodes,
                               uint32_t first_d, uint32_t first_s,
                               size_t *work) {
  uint32_t node = first_d != MACH_CC_NO_NODE ? first_d : first_s;
  bool in_d_list = first_d != MACH_CC_NO_NODE;
  while (node != MACH_CC_NO_NODE) {
    size_t b = nodes[node].block;
    node = nodes[node].next;
    if (node == MACH_CC_NO_NODE && in_d_list) {
      node = first_s;
      in_d_list = false;
    }
    *work += m->blocks[b].inst_count;
    bool live_d = MACH_CC_BIT(out, b, d), live_s = MACH_CC_BIT(out, b, s);
    const nyr_mach_block_t *block = &m->blocks[b];
    for (size_t n = block->inst_count; n > 0; --n) {
      const nyr_mach_inst_t *in = &m->insts[block->first_inst + n - 1];
      if (mach_cc_is_def(in)) {
        uint32_t def = in->dst.as.reg;
        if (def == d || def == s) {
          uint32_t other = def == d ? s : d;
          bool other_live = def == d ? live_s : live_d;
          bool copy_between = in->opcode == NYR_MACH_COPY &&
                              in->src0.kind == NYR_MACH_OPERAND_VREG &&
                              in->src0.as.reg == other;
          if (other_live && !copy_between)
            return true;
          if (def == d)
            live_d = false;
          else
            live_s = false;
        }
      }
      live_d = live_d || mach_cc_uses(in, d);
      live_s = live_s || mach_cc_uses(in, s);
    }
  }
  return false;
}

/*
 * Fuse the fixed shapes C integer conversions leave behind:
 *   load.i32 v; and w, v, 0xffffffff  -> zero-extending load
 *   shl v, x, 32; sar w, v, 32        -> movsxd w, x
 *   load.i8 v; shl w, v, 56; sar x, w, 56  -> movsx x, byte ptr
 *   load.u16 v; shl w, v, 48; sar x, w, 48 -> movsx x, word ptr
 */
static void mach_fuse_int_widening(nyr_mach_func_t *m) {
  size_t nv = m->vreg_len;
  uint32_t *uses = calloc(nv ? nv : 1, sizeof(*uses));
  int64_t *constant = calloc(nv ? nv : 1, sizeof(*constant));
  uint8_t *defs = calloc(nv ? nv : 1, 1);
  uint8_t *known = calloc(nv ? nv : 1, 1);
  if (!uses || !constant || !defs || !known)
    goto done;
  for (size_t i = 0; i < m->inst_len; ++i) {
    const nyr_mach_inst_t *in = &m->insts[i];
    if (mach_cc_is_def(in) && in->dst.as.reg < nv) {
      uint32_t v = in->dst.as.reg;
      defs[v] = defs[v] < 2 ? defs[v] + 1 : 2;
      if (in->opcode == NYR_MACH_COPY &&
          in->src0.kind == NYR_MACH_OPERAND_IMM) {
        known[v] = 1;
        constant[v] = in->src0.as.imm;
      }
    }
    const nyr_mach_operand_t *ops[] = {&in->src0, &in->src1, &in->src2,
                                       in->opcode == NYR_MACH_STORE ? &in->dst
                                                                    : NULL};
    for (size_t k = 0; k < 4 + in->args_len; ++k) {
      const nyr_mach_operand_t *op = k < 4 ? ops[k] : &in->args[k - 4];
      if (op && op->kind == NYR_MACH_OPERAND_VREG && op->as.reg < nv)
        uses[op->as.reg]++;
    }
  }
#define MACH_CONST_OP(op, value)                                               \
  ((op).kind == NYR_MACH_OPERAND_IMM                                           \
       ? (op).as.imm == (value)                                                \
       : ((op).kind == NYR_MACH_OPERAND_VREG && (op).as.reg < nv &&            \
          known[(op).as.reg] && defs[(op).as.reg] == 1 &&                      \
          constant[(op).as.reg] == (value)))
  bool changed = false;
  for (size_t b = 0; b < m->block_len; ++b) {
    const nyr_mach_block_t *block = &m->blocks[b];
    for (size_t n = 0; n + 1 < block->inst_count; ++n) {
      nyr_mach_inst_t *first = &m->insts[block->first_inst + n];
      nyr_mach_inst_t *second = &m->insts[block->first_inst + n + 1];
      /*
       * load.i32 ; copy ; and 0xffffffff  ==>  load.u32 (zero-extending).
       * C front ends emit a register copy between the sign-extending load
       * and the mask; both temporaries must be single-definition,
       * single-use so folding cannot change another consumer.
       */
      if (n + 2 < block->inst_count && first->opcode == NYR_MACH_LOAD &&
          first->mem_size == 4 && !first->narrow32 &&
          first->dst.kind == NYR_MACH_OPERAND_VREG && first->dst.as.reg < nv &&
          uses[first->dst.as.reg] == 1 && defs[first->dst.as.reg] == 1 &&
          second->opcode == NYR_MACH_COPY && !second->narrow32 &&
          second->src0.kind == NYR_MACH_OPERAND_VREG &&
          second->src0.as.reg == first->dst.as.reg &&
          second->dst.kind == NYR_MACH_OPERAND_VREG &&
          second->dst.as.reg < nv && uses[second->dst.as.reg] == 1 &&
          defs[second->dst.as.reg] == 1) {
        nyr_mach_inst_t *third = &m->insts[block->first_inst + n + 2];
        if (third->opcode == NYR_MACH_AND && !third->narrow32 &&
            third->dst.kind == NYR_MACH_OPERAND_VREG &&
            third->src0.kind == NYR_MACH_OPERAND_VREG &&
            third->src0.as.reg == second->dst.as.reg &&
            MACH_CONST_OP(third->src1, INT64_C(0xffffffff))) {
          first->mem_size = 5;
          first->dst = third->dst;
          *second = (nyr_mach_inst_t){.opcode = NYR_MACH_NOP,
                                      .source_pc = second->source_pc};
          *third = (nyr_mach_inst_t){.opcode = NYR_MACH_NOP,
                                     .source_pc = third->source_pc};
          changed = true;
          continue;
        }
      }
      if (first->dst.kind != NYR_MACH_OPERAND_VREG ||
          second->dst.kind != NYR_MACH_OPERAND_VREG ||
          second->src0.kind != NYR_MACH_OPERAND_VREG ||
          second->src0.as.reg != first->dst.as.reg || first->dst.as.reg >= nv ||
          uses[first->dst.as.reg] != 1 || defs[first->dst.as.reg] != 1 ||
          first->narrow32)
        continue;
      /*
       * A narrow load followed by the shift pair that sign-extends it is one
       * movsx. Every consumer of the shift result reads the sign-extended
       * value, so widening the load keeps its meaning at two fewer
       * instructions and two fewer dependencies per access.
       */
      if (n + 2 < block->inst_count && first->opcode == NYR_MACH_LOAD &&
          first->byte_width && !first->sext_load && !second->narrow32 &&
          (first->mem_size == 0 || first->mem_size == 1 ||
           first->mem_size == 2) &&
          MACH_CONST_OP(second->src1, first->mem_size == 2 ? 48 : 56) &&
          uses[second->dst.as.reg] == 1 && defs[second->dst.as.reg] == 1) {
        nyr_mach_inst_t *third = &m->insts[block->first_inst + n + 2];
        if (third->opcode == NYR_MACH_SAR && !third->narrow32 &&
            third->src0.kind == NYR_MACH_OPERAND_VREG &&
            third->src0.as.reg == second->dst.as.reg &&
            third->dst.kind == NYR_MACH_OPERAND_VREG &&
            MACH_CONST_OP(third->src1, first->mem_size == 2 ? 48 : 56)) {
          first->sext_load = true;
          first->dst = third->dst;
          *second = (nyr_mach_inst_t){.opcode = NYR_MACH_NOP,
                                      .source_pc = second->source_pc};
          *third = (nyr_mach_inst_t){.opcode = NYR_MACH_NOP,
                                     .source_pc = third->source_pc};
          changed = true;
          continue;
        }
      }
      /*
       * A 32-bit mask is exactly what narrow32 means for an AND, so the flag
       * does not block load-plus-mask folding; every other pairing keeps the
       * old rule.
       */
      if (second->narrow32 &&
          !(second->opcode == NYR_MACH_AND &&
            MACH_CONST_OP(second->src1, INT64_C(0xffffffff))))
        continue;
      if (first->opcode == NYR_MACH_LOAD && first->mem_size == 4 &&
          second->opcode == NYR_MACH_AND &&
          MACH_CONST_OP(second->src1, INT64_C(0xffffffff))) {
        first->mem_size = 5;
        first->dst = second->dst;
        *second = (nyr_mach_inst_t){.opcode = NYR_MACH_NOP,
                                    .source_pc = second->source_pc};
        changed = true;
      } else if (first->opcode == NYR_MACH_SHL &&
                 second->opcode == NYR_MACH_SAR &&
                 first->src0.kind == NYR_MACH_OPERAND_VREG &&
                 MACH_CONST_OP(first->src1, 32) &&
                 MACH_CONST_OP(second->src1, 32)) {
        second->src0 = first->src0;
        second->sext32 = true;
        *first = (nyr_mach_inst_t){.opcode = NYR_MACH_NOP,
                                   .source_pc = first->source_pc};
        changed = true;
      }
    }
  }
#undef MACH_CONST_OP
  if (changed)
    mach_compact_nops(m);
done:
  free(uses);
  free(constant);
  free(defs);
  free(known);
}

/*
 * Merging can make an instruction's destination one of its own inputs. The
 * encoders handle that only for the first operand of plain two-address
 * arithmetic, where the destination register is loaded from that operand.
 */
static bool mach_cc_alias_legal(const nyr_mach_func_t *m, uint32_t d,
                                uint32_t s) {
  for (size_t i = 0; i < m->inst_len; ++i) {
    const nyr_mach_inst_t *in = &m->insts[i];
    if (!mach_cc_is_def(in) || (in->dst.as.reg != d && in->dst.as.reg != s) ||
        in->opcode == NYR_MACH_COPY)
      continue;
    bool merged_src0 = in->src0.kind == NYR_MACH_OPERAND_VREG &&
                       (in->src0.as.reg == d || in->src0.as.reg == s);
    bool merged_other = false;
    const nyr_mach_operand_t *rest[] = {&in->src1, &in->src2};
    for (size_t k = 0; k < 2; ++k)
      merged_other |= rest[k]->kind == NYR_MACH_OPERAND_VREG &&
                      (rest[k]->as.reg == d || rest[k]->as.reg == s);
    for (size_t k = 0; k < in->args_len; ++k)
      merged_other |= in->args[k].kind == NYR_MACH_OPERAND_VREG &&
                      (in->args[k].as.reg == d || in->args[k].as.reg == s);
    if (merged_other)
      return false;
    if (merged_src0) {
      switch (in->opcode) {
      case NYR_MACH_ADD:
      case NYR_MACH_SUB:
      case NYR_MACH_AND:
      case NYR_MACH_OR:
      case NYR_MACH_XOR:
      case NYR_MACH_MUL:
      case NYR_MACH_SHL:
      case NYR_MACH_SAR:
      case NYR_MACH_SHR:
        break;
      default:
        return false;
      }
    }
  }
  return true;
}

static bool *mach_cc_raw_vregs(const nyr_mach_func_t *m,
                               const nyr_func_t *nyir) {
  if (!m || !nyir || !m->vreg_len)
    return NULL;
  bool *raw = malloc(m->vreg_len * sizeof(*raw));
  bool *defined = calloc(m->vreg_len, sizeof(*defined));
  if (!raw || !defined) {
    free(raw);
    free(defined);
    return NULL;
  }
  memset(raw, 1, m->vreg_len * sizeof(*raw));
  for (size_t i = 0; i < m->inst_len; ++i) {
    const nyr_mach_inst_t *in = &m->insts[i];
    if (!mach_cc_is_def(in) || in->dst.as.reg >= m->vreg_len)
      continue;
    uint32_t v = in->dst.as.reg;
    defined[v] = true;
    if (in->source_pc == UINT32_MAX && in->opcode == NYR_MACH_COPY &&
        in->src0.kind == NYR_MACH_OPERAND_VREG && in->src0.as.reg < m->vreg_len)
      continue;
    if (in->source_pc >= nyir->len || nyir->data[in->source_pc].dst != (int)v ||
        nyir->data[in->source_pc].scalar_class != NYIR_SCALAR_I64_RAW)
      raw[v] = false;
  }
  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t i = 0; i < m->inst_len; ++i) {
      const nyr_mach_inst_t *in = &m->insts[i];
      if (in->source_pc != UINT32_MAX || in->opcode != NYR_MACH_COPY ||
          in->dst.kind != NYR_MACH_OPERAND_VREG ||
          in->dst.as.reg >= m->vreg_len ||
          in->src0.kind != NYR_MACH_OPERAND_VREG ||
          in->src0.as.reg >= m->vreg_len)
        continue;
      uint32_t dst = in->dst.as.reg, src = in->src0.as.reg;
      if (raw[dst] && (!defined[src] || !raw[src])) {
        raw[dst] = false;
        changed = true;
      }
    }
  }
  free(defined);
  return raw;
}

static void mach_coalesce_copies(nyr_mach_func_t *m, const nyr_func_t *nyir) {
  const size_t nb = m->block_len, nv = m->vreg_len;
  if (!nb || !nv || nv > 20000 || nb > 5000 || m->inst_len > 100000)
    return;
  const size_t words = (nv + 63) / 64;
  uint64_t *in_sets = calloc(nb * words, sizeof(*in_sets));
  uint64_t *out_sets = calloc(nb * words, sizeof(*out_sets));
  uint64_t *gen = calloc(nb * words, sizeof(*gen));
  uint64_t *kill = calloc(nb * words, sizeof(*kill));
  uint32_t *def_count = calloc(nv, sizeof(*def_count));
  uint8_t *literal = calloc(nv, 1);
  uint32_t *first_def = malloc(nv * sizeof(*first_def));
  uint32_t *last_def = malloc(nv * sizeof(*last_def));
  bool *raw = m->c_abi ? NULL : mach_cc_raw_vregs(m, nyir);
  mach_cc_def_block_t *def_nodes =
      malloc((m->inst_len + 1) * sizeof(*def_nodes));
  uint32_t def_node_count = 0;
  if (!in_sets || !out_sets || !gen || !kill || !def_count || !literal ||
      !first_def || !last_def || !def_nodes || (!m->c_abi && !raw))
    goto done;
  memset(first_def, 0xff, nv * sizeof(*first_def));
  memset(last_def, 0xff, nv * sizeof(*last_def));
  for (size_t b = 0; b < nb; ++b) {
    const nyr_mach_block_t *block = &m->blocks[b];
    for (size_t n = block->inst_count; n > 0; --n) {
      const nyr_mach_inst_t *in = &m->insts[block->first_inst + n - 1];
      if (mach_cc_is_def(in) && in->dst.as.reg < nv) {
        uint32_t v = in->dst.as.reg;
        kill[b * words + v / 64] |= UINT64_C(1) << (v % 64);
        gen[b * words + v / 64] &= ~(UINT64_C(1) << (v % 64));
        ++def_count[v];
        if (last_def[v] == MACH_CC_NO_NODE ||
            def_nodes[last_def[v]].block != b) {
          def_nodes[def_node_count] =
              (mach_cc_def_block_t){(uint32_t)b, MACH_CC_NO_NODE};
          if (last_def[v] == MACH_CC_NO_NODE)
            first_def[v] = def_node_count;
          else
            def_nodes[last_def[v]].next = def_node_count;
          last_def[v] = def_node_count++;
        }
        if (in->opcode == NYR_MACH_COPY &&
            in->src0.kind != NYR_MACH_OPERAND_VREG)
          literal[v] = 1;
      }
      const nyr_mach_operand_t *ops[] = {&in->src0, &in->src1, &in->src2,
                                         in->opcode == NYR_MACH_STORE ? &in->dst
                                                                      : NULL};
      for (size_t k = 0; k < 4 + in->args_len; ++k) {
        const nyr_mach_operand_t *op = k < 4 ? ops[k] : &in->args[k - 4];
        if (op && op->kind == NYR_MACH_OPERAND_VREG && op->as.reg < nv)
          gen[b * words + op->as.reg / 64] |= UINT64_C(1) << (op->as.reg % 64);
      }
    }
  }
  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t b = nb; b > 0; --b) {
      size_t at = b - 1;
      const nyr_mach_block_t *block = &m->blocks[at];
      uint64_t *out = out_sets + at * words;
      bool falls = true;
      for (size_t n = 0; n < block->inst_count; ++n) {
        const nyr_mach_inst_t *in = &m->insts[block->first_inst + n];
        if ((in->opcode == NYR_MACH_BR || in->opcode == NYR_MACH_BR_IF) &&
            in->src1.kind == NYR_MACH_OPERAND_BLOCK &&
            in->src1.as.block_index < nb) {
          const uint64_t *succ = in_sets + in->src1.as.block_index * words;
          for (size_t w = 0; w < words; ++w)
            out[w] |= succ[w];
          if (in->opcode == NYR_MACH_BR)
            falls = false;
        } else if (in->opcode == NYR_MACH_RET) {
          falls = false;
        }
      }
      if (falls && at + 1 < nb) {
        const uint64_t *succ = in_sets + (at + 1) * words;
        for (size_t w = 0; w < words; ++w)
          out[w] |= succ[w];
      }
      for (size_t w = 0; w < words; ++w) {
        uint64_t value = gen[at * words + w] | (out[w] & ~kill[at * words + w]);
        if (value != in_sets[at * words + w]) {
          in_sets[at * words + w] = value;
          changed = true;
        }
      }
    }
  }
  size_t work = 0, merged = 0;
  int pass_start = m->c_abi ? 0 : 1;
  for (int pass = pass_start; pass < 2; ++pass) {
    for (size_t i = 0; i < m->inst_len && work < 40000000; ++i) {
      nyr_mach_inst_t *copy = &m->insts[i];
      if (copy->opcode != NYR_MACH_COPY || copy->narrow32 ||
          copy->dst.kind != NYR_MACH_OPERAND_VREG ||
          copy->src0.kind != NYR_MACH_OPERAND_VREG || copy->dst.as.reg >= nv ||
          copy->src0.as.reg >= nv || copy->dst.as.reg == copy->src0.as.reg ||
          (pass == 0 && copy->source_pc != UINT32_MAX))
        continue;
      uint32_t d = copy->dst.as.reg, s = copy->src0.as.reg;
      if (!m->c_abi && (copy->source_pc == UINT32_MAX || !raw[d] || !raw[s]))
        continue;
      nyr_mach_type_t type = m->vreg_types[d];
      if (type != m->vreg_types[s] ||
          m->vreg_classes[d] != m->vreg_classes[s] ||
          (type != NYR_MACH_TYPE_I64 && type != NYR_MACH_TYPE_F64 &&
           type != NYR_MACH_TYPE_F32) ||
          literal[s] || (literal[d] && def_count[d] == 1))
        continue;
      if (mach_cc_interferes(m, out_sets, words, d, s, def_nodes, first_def[d],
                             first_def[s], &work) ||
          !mach_cc_alias_legal(m, d, s))
        continue;
      work += m->inst_len;
      for (size_t k = 0; k < m->inst_len; ++k) {
        nyr_mach_inst_t *in = &m->insts[k];
        mach_cc_rename(&in->dst, s, d);
        mach_cc_rename(&in->src0, s, d);
        mach_cc_rename(&in->src1, s, d);
        mach_cc_rename(&in->src2, s, d);
        for (size_t a = 0; a < in->args_len; ++a)
          mach_cc_rename(&in->args[a], s, d);
      }
      for (size_t b = 0; b < nb; ++b) {
        if (MACH_CC_BIT(out_sets, b, s))
          out_sets[b * words + d / 64] |= UINT64_C(1) << (d % 64);
        if (MACH_CC_BIT(in_sets, b, s))
          in_sets[b * words + d / 64] |= UINT64_C(1) << (d % 64);
        out_sets[b * words + s / 64] &= ~(UINT64_C(1) << (s % 64));
        in_sets[b * words + s / 64] &= ~(UINT64_C(1) << (s % 64));
      }
      def_count[d] += def_count[s];
      def_count[s] = 0;
      if (first_def[s] != MACH_CC_NO_NODE) {
        if (first_def[d] == MACH_CC_NO_NODE)
          first_def[d] = first_def[s];
        else
          def_nodes[last_def[d]].next = first_def[s];
        last_def[d] = last_def[s];
        first_def[s] = last_def[s] = MACH_CC_NO_NODE;
      }
      literal[d] = literal[d] || literal[s];
      uint32_t source_pc = copy->source_pc;
      *copy = (nyr_mach_inst_t){.opcode = NYR_MACH_NOP, .source_pc = source_pc};
      ++merged;
    }
  }
  if (merged)
    mach_compact_nops(m);
done:
  free(in_sets);
  free(out_sets);
  free(gen);
  free(kill);
  free(def_count);
  free(literal);
  free(first_def);
  free(last_def);
  free(def_nodes);
  free(raw);
}
#undef MACH_CC_BIT

/*
 * A value live across a call elsewhere in the function cannot keep a
 * caller-saved register, and it competes for the five callee-saved ones. Inside
 * a call-free loop it does not need that protection, so give it a copy made
 * just before the loop and use the copy in the loop body. The copy's range has
 * no calls, which makes it eligible for a loop register.
 */
typedef struct {
  uint32_t vreg;
  uint32_t uses;
} mach_split_candidate_t;

static int mach_split_candidate_order(const void *a, const void *b) {
  const mach_split_candidate_t *x = a, *y = b;
  if (x->uses != y->uses)
    return x->uses > y->uses ? -1 : 1;
  return x->vreg < y->vreg ? -1 : x->vreg > y->vreg;
}

static bool mach_split_block_falls(const nyr_mach_func_t *m, size_t b) {
  const nyr_mach_block_t *block = &m->blocks[b];
  if (!block->inst_count)
    return true;
  nyr_mach_opcode_t op =
      m->insts[block->first_inst + block->inst_count - 1].opcode;
  return op != NYR_MACH_BR && op != NYR_MACH_RET && op != NYR_MACH_TRAP;
}

/*
 * Blocks jumping into or falling into the loop [head, tail] from outside.
 */
static bool mach_split_single_entry(const nyr_mach_func_t *m, size_t head,
                                    size_t tail, size_t *pre_out) {
  size_t pre = SIZE_MAX;
  for (size_t b = 0; b < m->block_len; ++b) {
    if (b >= head && b <= tail)
      continue;
    const nyr_mach_block_t *block = &m->blocks[b];
    bool enters = false;
    for (size_t n = 0; n < block->inst_count; ++n) {
      const nyr_mach_inst_t *in = &m->insts[block->first_inst + n];
      if ((in->opcode == NYR_MACH_BR || in->opcode == NYR_MACH_BR_IF) &&
          in->src1.kind == NYR_MACH_OPERAND_BLOCK) {
        size_t target = in->src1.as.block_index;
        if (target > head && target <= tail)
          return false;
        enters = enters || target == head;
      }
    }
    if (b + 1 == head && mach_split_block_falls(m, b))
      enters = true;
    if (enters) {
      if (pre != SIZE_MAX)
        return false;
      pre = b;
    }
  }
  *pre_out = pre;
  return pre != SIZE_MAX;
}

static size_t mach_split_insert_point(const nyr_mach_func_t *m, size_t pre) {
  const nyr_mach_block_t *block = &m->blocks[pre];
  size_t at = (size_t)block->first_inst + block->inst_count;
  while (at > block->first_inst) {
    nyr_mach_opcode_t op = m->insts[at - 1].opcode;
    if (op != NYR_MACH_BR && op != NYR_MACH_BR_IF && op != NYR_MACH_CMP)
      break;
    --at;
  }
  return at;
}

static bool mach_split_around_loops(nyr_mach_func_t *m) {
  const size_t nb = m->block_len, nv0 = m->vreg_len;
  if (nb < 2 || !nv0 || m->inst_len > 200000)
    return true;
  bool any_call = false;
  for (size_t i = 0; i < m->inst_len && !any_call; ++i)
    any_call = m->insts[i].opcode == NYR_MACH_CALL;
  if (!any_call)
    return true;
  uint32_t *uses = calloc(nv0, sizeof(*uses));
  uint8_t *defined = calloc(nv0, 1);
  uint8_t *taken = calloc(nb, 1);
  mach_split_candidate_t *list = malloc(nv0 * sizeof(*list));
  size_t *first_def = malloc(nv0 * sizeof(*first_def));
  size_t *last_use = calloc(nv0, sizeof(*last_use));
  size_t *calls = malloc((m->inst_len + 1) * sizeof(*calls));
  size_t call_count = 0;
  bool ok = uses && defined && taken && list && first_def && last_use && calls;
  if (ok)
    memset(first_def, 0xff, nv0 * sizeof(*first_def));
  for (size_t i = 0; ok && i < m->inst_len; ++i) {
    const nyr_mach_inst_t *in = &m->insts[i];
    if (in->opcode == NYR_MACH_CALL)
      calls[call_count++] = i;
    if (mach_cc_is_def(in) && in->dst.as.reg < nv0 &&
        first_def[in->dst.as.reg] == SIZE_MAX)
      first_def[in->dst.as.reg] = i;
    const nyr_mach_operand_t *ops[] = {&in->src0, &in->src1, &in->src2,
                                       in->opcode == NYR_MACH_STORE ? &in->dst
                                                                    : NULL};
    for (size_t k = 0; k < 4 + in->args_len; ++k) {
      const nyr_mach_operand_t *op = k < 4 ? ops[k] : &in->args[k - 4];
      if (op && op->kind == NYR_MACH_OPERAND_VREG && op->as.reg < nv0)
        last_use[op->as.reg] = i;
    }
  }
  /*
   * Outermost call-free loops first; a nested loop is covered by its parent.
   */
  for (size_t span = nb; ok && span > 0; --span) {
    for (size_t head = 0; ok && head + span <= nb; ++head) {
      size_t tail = head + span - 1;
      bool back_edge = false;
      const nyr_mach_block_t *last = &m->blocks[tail];
      for (size_t n = 0; n < last->inst_count; ++n) {
        const nyr_mach_inst_t *in = &m->insts[last->first_inst + n];
        back_edge =
            back_edge ||
            ((in->opcode == NYR_MACH_BR || in->opcode == NYR_MACH_BR_IF) &&
             in->src1.kind == NYR_MACH_OPERAND_BLOCK &&
             in->src1.as.block_index == head);
      }
      if (!back_edge)
        continue;
      bool usable = true;
      for (size_t b = head; b <= tail && usable; ++b)
        usable = !taken[b];
      size_t pre = SIZE_MAX;
      if (!usable || !mach_split_single_entry(m, head, tail, &pre))
        continue;
      size_t begin = m->blocks[head].first_inst;
      size_t end =
          (size_t)m->blocks[tail].first_inst + m->blocks[tail].inst_count;
      bool call_free = true;
      for (size_t i = begin; i < end && call_free; ++i) {
        nyr_mach_opcode_t op = m->insts[i].opcode;
        call_free = op != NYR_MACH_CALL && op != NYR_MACH_INLINE_ASM &&
                    op != NYR_MACH_INTRINSIC;
      }
      if (!call_free)
        continue;
      for (size_t b = head; b <= tail; ++b)
        taken[b] = 1;
      memset(uses, 0, nv0 * sizeof(*uses));
      memset(defined, 0, nv0);
      for (size_t i = begin; i < end; ++i) {
        const nyr_mach_inst_t *in = &m->insts[i];
        if (mach_cc_is_def(in) && in->dst.as.reg < nv0)
          defined[in->dst.as.reg] = 1;
        const nyr_mach_operand_t *ops[] = {
            &in->src0, &in->src1, &in->src2,
            in->opcode == NYR_MACH_STORE ? &in->dst : NULL};
        for (size_t k = 0; k < 4 + in->args_len; ++k) {
          const nyr_mach_operand_t *op = k < 4 ? ops[k] : &in->args[k - 4];
          if (op && op->kind == NYR_MACH_OPERAND_VREG && op->as.reg < nv0)
            ++uses[op->as.reg];
        }
      }
      size_t insert_at = mach_split_insert_point(m, pre);
      size_t count = 0;
      for (uint32_t v = 0; v < nv0; ++v) {
        nyr_mach_type_t type = m->vreg_types[v];
        if (!uses[v] || defined[v] || first_def[v] >= insert_at ||
            (type != NYR_MACH_TYPE_I64 && type != NYR_MACH_TYPE_F64 &&
             type != NYR_MACH_TYPE_F32))
          continue;

        /*
         * Worth splitting only when a call sits inside v's whole range.
         */
        bool crosses_call = false;
        for (size_t c = 0; c < call_count && !crosses_call; ++c)
          crosses_call = calls[c] > first_def[v] && calls[c] < last_use[v];
        if (crosses_call)
          list[count++] = (mach_split_candidate_t){v, uses[v]};
      }
      if (!count)
        continue;
      qsort(list, count, sizeof(*list), mach_split_candidate_order);
      if (count > 8)
        count = 8;
      while (m->inst_cap - m->inst_len < count)
        if (!mach_grow((void **)&m->insts, &m->inst_cap, sizeof(*m->insts))) {
          ok = false;
          break;
        }
      for (size_t c = 0; ok && c < count; ++c) {
        uint32_t original = list[c].vreg, copy = 0;
        nyr_mach_reg_class_t reg_class = m->vreg_classes[original];
        nyr_mach_type_t type = m->vreg_types[original];
        if (!nyr_mach_alloc_vreg(m, reg_class, &copy)) {
          ok = false;
          break;
        }
        m->vreg_types[copy] = type;
        for (size_t i = begin; i < end; ++i) {
          nyr_mach_inst_t *in = &m->insts[i];
          mach_cc_rename(&in->src0, original, copy);
          mach_cc_rename(&in->src1, original, copy);
          mach_cc_rename(&in->src2, original, copy);
          if (in->opcode == NYR_MACH_STORE)
            mach_cc_rename(&in->dst, original, copy);
          for (size_t a = 0; a < in->args_len; ++a)
            mach_cc_rename(&in->args[a], original, copy);
        }
        memmove(&m->insts[insert_at + 1], &m->insts[insert_at],
                (m->inst_len - insert_at) * sizeof(*m->insts));
        m->insts[insert_at] =
            (nyr_mach_inst_t){.opcode = NYR_MACH_COPY,
                              .source_pc = UINT32_MAX,
                              .dst = {.kind = NYR_MACH_OPERAND_VREG,
                                      .reg_class = reg_class,
                                      .as.reg = copy},
                              .src0 = {.kind = NYR_MACH_OPERAND_VREG,
                                       .reg_class = reg_class,
                                       .as.reg = original}};
        ++m->inst_len;
        ++m->blocks[pre].inst_count;
        for (size_t b = pre + 1; b < nb; ++b)
          ++m->blocks[b].first_inst;
        ++begin;
        ++end;
      }
      /*
       * Recorded positions past the insertion moved with the code.
       */
      for (size_t v = 0; ok && v < nv0; ++v) {
        if (first_def[v] != SIZE_MAX && first_def[v] >= insert_at)
          first_def[v] += count;
        if (last_use[v] >= insert_at)
          last_use[v] += count;
      }
      for (size_t c = 0; ok && c < call_count; ++c)
        if (calls[c] >= insert_at)
          calls[c] += count;
    }
  }
  free(calls);
  free(uses);
  free(defined);
  free(taken);
  free(list);
  free(first_def);
  free(last_use);
  return ok;
}

static void mach_color_local_homes(nyr_mach_func_t *mach) {
  if (!mach || !mach->vreg_len || mach->vreg_len > UINT32_MAX ||
      mach->vreg_len > SIZE_MAX / sizeof(uint32_t))
    return;
  const size_t nv = mach->vreg_len;
  const uint32_t no_block = UINT32_MAX;
  uint32_t *def_block = malloc(nv * sizeof(*def_block));
  size_t *def_inst = malloc(nv * sizeof(*def_inst));
  size_t *last_use = calloc(nv, sizeof(*last_use));
  uint8_t *local = calloc(nv, sizeof(*local));
  uint8_t *multi_def = calloc(nv, sizeof(*multi_def));
  uint32_t *homes = malloc(nv * sizeof(*homes));
  mach_home_interval_t *intervals = malloc(nv * sizeof(*intervals));
  size_t *slot_end = malloc(nv * sizeof(*slot_end));
  size_t *heap = malloc(nv * sizeof(*heap));
  size_t *heap_pos = malloc(nv * sizeof(*heap_pos));
  size_t *free_slots = malloc(nv * sizeof(*free_slots));
  if (!def_block || !def_inst || !last_use || !local || !multi_def || !homes ||
      !intervals || !slot_end || !heap || !heap_pos || !free_slots)
    goto done;
  for (size_t v = 0; v < nv; ++v) {
    def_block[v] = no_block;
    def_inst[v] = SIZE_MAX;
    homes[v] = (uint32_t)v;
    local[v] = 1;
  }
  for (size_t bi = 0; bi < mach->block_len; ++bi) {
    const nyr_mach_block_t *block = &mach->blocks[bi];
    for (size_t n = 0; n < block->inst_count; ++n) {
      size_t pc = block->first_inst + n;
      const nyr_mach_inst_t *in = &mach->insts[pc];
      const nyr_mach_operand_t *ops[] = {&in->src0, &in->src1, &in->src2};
      for (size_t oi = 0; oi < sizeof(ops) / sizeof(ops[0]); ++oi) {
        const nyr_mach_operand_t *op = ops[oi];
        if (op->kind == NYR_MACH_OPERAND_VREG && op->as.reg < nv) {
          size_t v = op->as.reg;
          if (def_block[v] == no_block || def_block[v] != bi)
            local[v] = 0;
          if (last_use[v] < pc)
            last_use[v] = pc;
        }
      }
      for (size_t ai = 0; ai < in->args_len; ++ai) {
        const nyr_mach_operand_t *op = &in->args[ai];
        if (op->kind == NYR_MACH_OPERAND_VREG && op->as.reg < nv) {
          size_t v = op->as.reg;
          if (def_block[v] == no_block || def_block[v] != bi)
            local[v] = 0;
          if (last_use[v] < pc)
            last_use[v] = pc;
        }
      }
      if (in->dst.kind == NYR_MACH_OPERAND_VREG && in->dst.as.reg < nv) {
        size_t v = in->dst.as.reg;
        if (def_block[v] != no_block) {
          local[v] = 0;
          multi_def[v] = 1;
        } else {
          def_block[v] = (uint32_t)bi;
          def_inst[v] = pc;
          last_use[v] = pc;
        }
      }
    }
  }
  size_t count = 0;
  for (size_t v = 0; v < nv; ++v) {
    if (!local[v] || def_block[v] == no_block || def_inst[v] == SIZE_MAX)
      continue;
    intervals[count++] = (mach_home_interval_t){
        .reg = (uint32_t)v,
        .block = def_block[v],
        .start = def_inst[v],
        .end = last_use[v],
        .def_inst = def_inst[v],
    };
  }
  if (!count)
    goto done;
  qsort(intervals, count, sizeof(*intervals), mach_home_interval_cmp);

  size_t nonlocal_count = 0;
  for (size_t v = 0; v < nv; ++v)
    if (!local[v] || def_block[v] == no_block || def_inst[v] == SIZE_MAX)
      homes[v] = (uint32_t)nonlocal_count++;

  /*
   * Cross-block COPYs are pure SSA aliases, including loop-carried copies
   * introduced while lowering PHI edges. Give a whole same-type copy chain
   * one canonical home. Restrict this to nonlocal values: local intervals may
   * reuse a slot after their last use, so aliasing a local slot across blocks
   * would let a later interval overwrite the copy's live value.
   *
   * The alias claim only holds for single-definition values. A PHI edge
   * destination carries one COPY per incoming edge, so it is multi-def and
   * each edge aliases a different producer. Sharing its home with one
   * producer lets a sibling edge overwrite that home (the nested backedge
   * case), while the self-copy on the first edge gets elided and can no
   * longer re-establish the value. Multi-def chain members keep private
   * homes and terminate the walk.
   */
  uint32_t *dense_homes = calloc(nv, sizeof(*dense_homes));
  if (dense_homes) {
    for (size_t v = 0; v < nv; ++v) {
      if (local[v] || def_block[v] == no_block || def_inst[v] == SIZE_MAX)
        continue;
      size_t root = v;
      bool valid = true;
      for (size_t depth = 0; depth < nv; ++depth) {
        if (multi_def[root]) {
          valid = false;
          break;
        }
        size_t di = def_inst[root];
        if (di == SIZE_MAX)
          break;
        const nyr_mach_inst_t *copy = &mach->insts[di];
        if (copy->opcode != NYR_MACH_COPY || copy->narrow32 ||
            copy->dst.kind != NYR_MACH_OPERAND_VREG ||
            copy->dst.as.reg != root ||
            copy->src0.kind != NYR_MACH_OPERAND_VREG || copy->src0.as.reg >= nv)
          break;
        size_t src = copy->src0.as.reg;
        if (local[src] || multi_def[src] ||
            mach->vreg_types[src] != mach->vreg_types[root] ||
            mach->vreg_classes[src] != mach->vreg_classes[root]) {
          valid = false;
          break;
        }
        root = src;
        if (depth + 1 == nv)
          valid = false; /* malformed cyclic COPY chain */
      }
      if (valid && root != v)
        homes[v] = homes[root];
    }

    /*
     * Compact the now-shared nonlocal home IDs before local slots are added.
     */
    size_t compact_count = 0;
    for (size_t v = 0; v < nv; ++v) {
      if (local[v] || def_block[v] == no_block || def_inst[v] == SIZE_MAX)
        continue;
      uint32_t old = homes[v];
      if (old >= nv)
        continue;
      if (dense_homes[old] == 0) {
        if (compact_count >= UINT32_MAX)
          break;
        dense_homes[old] = (uint32_t)(++compact_count);
      }
    }
    for (size_t v = 0; v < nv; ++v) {
      if (local[v] || def_block[v] == no_block || def_inst[v] == SIZE_MAX)
        continue;
      uint32_t old = homes[v];
      if (old < nv && dense_homes[old] != 0)
        homes[v] = dense_homes[old] - 1;
    }
    nonlocal_count = compact_count;
    free(dense_homes);
  }
  size_t local_home_count = 0, slot_count = 0;
  size_t heap_len = 0, free_len = 0;
  uint32_t block_id = no_block;
  for (size_t i = 0; i < count; ++i) {
    mach_home_interval_t *cur = &intervals[i];
    if (cur->block != block_id) {
      block_id = cur->block;
      slot_count = heap_len = free_len = 0;
    }

    while (heap_len && slot_end[heap[0]] < cur->start) {
      size_t expired = heap[0];
      size_t tail = heap[--heap_len];
      if (heap_len) {
        heap[0] = tail;
        heap_pos[tail] = 0;
        size_t at = 0;
        for (;;) {
          size_t child = at * 2 + 1;
          if (child >= heap_len)
            break;
          if (child + 1 < heap_len &&
              slot_end[heap[child + 1]] < slot_end[heap[child]])
            ++child;
          if (slot_end[heap[at]] <= slot_end[heap[child]])
            break;
          size_t tmp = heap[at];
          heap[at] = heap[child];
          heap[child] = tmp;
          heap_pos[heap[at]] = at;
          heap_pos[heap[child]] = child;
          at = child;
        }
      }
      free_slots[free_len++] = expired;
    }

    const nyr_mach_inst_t *def = &mach->insts[cur->def_inst];
    if (def->opcode == NYR_MACH_COPY && !def->narrow32 &&
        def->src0.kind == NYR_MACH_OPERAND_VREG && def->src0.as.reg < nv) {
      size_t src = def->src0.as.reg;
      if (local[src] && def_block[src] == cur->block &&
          last_use[src] == cur->start &&
          mach->vreg_types[src] == mach->vreg_types[cur->reg] &&
          mach->vreg_classes[src] == mach->vreg_classes[cur->reg]) {
        homes[cur->reg] = homes[src];
        size_t slot = homes[src] - nonlocal_count;
        if (slot < slot_count && slot_end[slot] < cur->end) {
          slot_end[slot] = cur->end;
          size_t at = heap_pos[slot];
          while (at < heap_len) {
            size_t child = at * 2 + 1;
            if (child >= heap_len)
              break;
            if (child + 1 < heap_len &&
                slot_end[heap[child + 1]] < slot_end[heap[child]])
              ++child;
            if (slot_end[heap[at]] <= slot_end[heap[child]])
              break;
            size_t tmp = heap[at];
            heap[at] = heap[child];
            heap[child] = tmp;
            heap_pos[heap[at]] = at;
            heap_pos[heap[child]] = child;
            at = child;
          }
        }
        continue;
      }
    }

    size_t slot = free_len ? free_slots[--free_len] : slot_count++;
    slot_end[slot] = cur->end;
    homes[cur->reg] = (uint32_t)(nonlocal_count + slot);
    if (slot_count > local_home_count)
      local_home_count = slot_count;
    size_t at = heap_len++;
    while (at) {
      size_t parent = (at - 1) / 2;
      if (slot_end[heap[parent]] <= slot_end[slot])
        break;
      heap[at] = heap[parent];
      heap_pos[heap[at]] = at;
      at = parent;
    }
    heap[at] = slot;
    heap_pos[slot] = at;
  }

  mach->home_slots = homes;
  mach->home_slot_count = nonlocal_count + local_home_count;
  homes = NULL;
done:
  free(def_block);
  free(def_inst);
  free(last_use);
  free(local);
  free(multi_def);
  free(homes);
  free(intervals);
  free(slot_end);
  free(heap);
  free(heap_pos);
  free(free_slots);
}

/*
 * Remove same-block SSA copies before register allocation.  Machine lowering
 * deliberately materializes many source-level moves (and PHI edge values) as
 * COPY instructions.  Copies are safe to forward within their defining block:
 * the source dominates each rewritten use, while keeping this local avoids
 * needing a dominator tree or making assumptions about alternate CFG edges.
 */
static void mach_propagate_local_copies(nyr_mach_func_t *mach) {
  if (!mach || !mach->vreg_len || mach->block_len > UINT32_MAX ||
      mach->vreg_len > SIZE_MAX / sizeof(uint32_t))
    return;
  uint32_t *alias = malloc(mach->vreg_len * sizeof(*alias));
  uint32_t *aliased = malloc(mach->vreg_len * sizeof(*aliased));
  uint32_t *first_use_block = malloc(mach->vreg_len * sizeof(*first_use_block));
  uint8_t *multi_block_use = calloc(mach->vreg_len, sizeof(*multi_block_use));
  if (!alias || !aliased || !first_use_block || !multi_block_use) {
    free(alias);
    free(aliased);
    free(first_use_block);
    free(multi_block_use);
    return;
  }
  for (size_t v = 0; v < mach->vreg_len; ++v) {
    alias[v] = (uint32_t)v;
    first_use_block[v] = UINT32_MAX;
  }

  /*
   * Snapshot original uses once; this keeps the pass linear in IR size.
   */
  for (size_t bi = 0; bi < mach->block_len; ++bi) {
    const nyr_mach_block_t *block = &mach->blocks[bi];
    size_t end = (size_t)block->first_inst + block->inst_count;
    if (end > mach->inst_len)
      continue;
    for (size_t pc = block->first_inst; pc < end; ++pc) {
      const nyr_mach_inst_t *in = &mach->insts[pc];
      /*
       * A PHI edge COPY consumes a snapshot that must survive this bundle.
       */
      if (in->source_pc == UINT32_MAX && in->opcode == NYR_MACH_COPY &&
          in->src0.kind == NYR_MACH_OPERAND_VREG &&
          in->src0.as.reg < mach->vreg_len)
        multi_block_use[in->src0.as.reg] = 1;
      const nyr_mach_operand_t *ops[] = {&in->src0, &in->src1, &in->src2,
                                         in->opcode == NYR_MACH_STORE ? &in->dst
                                                                      : NULL};
      for (size_t oi = 0; oi < sizeof(ops) / sizeof(ops[0]); ++oi) {
        const nyr_mach_operand_t *op = ops[oi];
        if (op && op->kind == NYR_MACH_OPERAND_VREG &&
            op->as.reg < mach->vreg_len) {
          uint32_t v = op->as.reg;
          if (first_use_block[v] == UINT32_MAX)
            first_use_block[v] = (uint32_t)bi;
          else if (first_use_block[v] != bi)
            multi_block_use[v] = 1;
        }
      }
      for (size_t ai = 0; in->args && ai < in->args_len; ++ai) {
        const nyr_mach_operand_t *op = &in->args[ai];
        if (op->kind != NYR_MACH_OPERAND_VREG || op->as.reg >= mach->vreg_len)
          continue;
        uint32_t v = op->as.reg;
        if (first_use_block[v] == UINT32_MAX)
          first_use_block[v] = (uint32_t)bi;
        else if (first_use_block[v] != bi)
          multi_block_use[v] = 1;
      }
    }
  }

  for (size_t bi = 0; bi < mach->block_len; ++bi) {
    const nyr_mach_block_t *block = &mach->blocks[bi];
    size_t aliased_len = 0;
    size_t end = (size_t)block->first_inst + block->inst_count;
    if (end > mach->inst_len)
      continue;
    for (size_t pc = block->first_inst; pc < end; ++pc) {
      nyr_mach_inst_t *in = &mach->insts[pc];
      /*
       * Synthetic PHI-edge copies form a parallel-copy bundle. Rewriting an
       * input through an alias from an earlier copy in the same block can
       * replace a snapshot with a destination that this bundle overwrites
       * first (for example, the two pointers in a ping-pong buffer swap).
       */
      if (in->source_pc == UINT32_MAX && in->opcode == NYR_MACH_COPY)
        continue;
      nyr_mach_operand_t *inputs[] = {&in->src0, &in->src1, &in->src2};
      for (size_t oi = 0; oi < sizeof(inputs) / sizeof(inputs[0]); ++oi) {
        nyr_mach_operand_t *op = inputs[oi];
        if (op->kind == NYR_MACH_OPERAND_VREG && op->as.reg < mach->vreg_len)
          op->as.reg = alias[op->as.reg];
      }
      if (in->args) {
        for (size_t ai = 0; ai < in->args_len; ++ai) {
          nyr_mach_operand_t *op = &in->args[ai];
          if (op->kind == NYR_MACH_OPERAND_VREG && op->as.reg < mach->vreg_len)
            op->as.reg = alias[op->as.reg];
        }
      }
      /*
       * STORE's dst is its address input; other dst operands define values.
       */
      if (in->opcode == NYR_MACH_STORE &&
          in->dst.kind == NYR_MACH_OPERAND_VREG &&
          in->dst.as.reg < mach->vreg_len)
        in->dst.as.reg = alias[in->dst.as.reg];

      /*
       * PHI edge copies use UINT32_MAX as their source_pc.  Their
       * destination's only ordinary uses are often in the successor block,
       * so the block-local use scan below cannot see that the edge copy is
       * what establishes the successor's live-in value.  Keep those copies
       * intact for register allocation and emission; folding one into the
       * value's local alias can silently drop a loop-carried update.
       */
      if (in->source_pc == UINT32_MAX || in->opcode != NYR_MACH_COPY ||
          in->narrow32 || in->dst.kind != NYR_MACH_OPERAND_VREG ||
          in->dst.as.reg >= mach->vreg_len ||
          in->src0.kind != NYR_MACH_OPERAND_VREG ||
          in->src0.as.reg >= mach->vreg_len || !mach->vreg_types ||
          !mach->vreg_classes ||
          mach->vreg_types[in->dst.as.reg] !=
              mach->vreg_types[in->src0.as.reg] ||
          mach->vreg_classes[in->dst.as.reg] !=
              mach->vreg_classes[in->src0.as.reg])
        continue;
      uint32_t dst = in->dst.as.reg;
      uint32_t src = in->src0.as.reg;
      if (dst != src) {
        alias[dst] = src;
        aliased[aliased_len++] = dst;
      }
      bool used_outside_block =
          first_use_block[dst] != UINT32_MAX &&
          (first_use_block[dst] != bi || multi_block_use[dst]);
      if (used_outside_block)
        continue;
      uint32_t source_pc = in->source_pc;
      *in = (nyr_mach_inst_t){.opcode = NYR_MACH_NOP, .source_pc = source_pc};
    }
    /*
     * SSA aliases are block-local. Restore only destinations actually
     * aliased in this block instead of resetting every vreg at every block.
     */
    for (size_t i = 0; i < aliased_len; ++i)
      alias[aliased[i]] = aliased[i];
  }
  if (mach->vreg_types && mach->vreg_classes) {
    uint8_t *defs = calloc(mach->vreg_len, sizeof(*defs));
    if (defs) {
      for (size_t pc = 0; pc < mach->inst_len; ++pc) {
        const nyr_mach_inst_t *in = &mach->insts[pc];
        if (in->opcode != NYR_MACH_NOP && in->opcode != NYR_MACH_STORE &&
            in->dst.kind == NYR_MACH_OPERAND_VREG &&
            in->dst.as.reg < mach->vreg_len && defs[in->dst.as.reg] < 2)
          ++defs[in->dst.as.reg];
      }
      for (size_t bi = 0; bi < mach->block_len; ++bi) {
        const nyr_mach_block_t *block = &mach->blocks[bi];
        size_t end = (size_t)block->first_inst + block->inst_count;
        if (end > mach->inst_len)
          continue;
        for (size_t pc = block->first_inst; pc < end; ++pc) {
          nyr_mach_inst_t *in = &mach->insts[pc];
          if (in->opcode != NYR_MACH_LOAD ||
              in->effects != NYR_MACH_EFFECT_READ_MEMORY || in->narrow32 ||
              in->dst.kind != NYR_MACH_OPERAND_VREG ||
              in->src0.kind != NYR_MACH_OPERAND_VREG ||
              in->src1.kind != NYR_MACH_OPERAND_NONE ||
              in->src2.kind != NYR_MACH_OPERAND_NONE || in->args_len ||
              in->args || in->arg_sizes || in->dst.as.reg >= mach->vreg_len ||
              in->src0.as.reg >= mach->vreg_len ||
              mach->vreg_types[in->dst.as.reg] != NYR_MACH_TYPE_I64 ||
              mach->vreg_classes[in->dst.as.reg] != NYR_MACH_REGCLASS_GPR ||
              defs[in->dst.as.reg] != 1)
            continue;
          uint32_t addr = in->src0.as.reg;
          size_t first = pc > 96 ? pc - 96 : block->first_inst;
          if (first < block->first_inst)
            first = block->first_inst;
          for (size_t j = pc; j-- > first;) {
            const nyr_mach_inst_t *prev = &mach->insts[j];
            if (prev->effects &
                (NYR_MACH_EFFECT_WRITE_MEMORY | NYR_MACH_EFFECT_CALL |
                 NYR_MACH_EFFECT_CONTROL))
              break;
            if (prev->opcode == NYR_MACH_INTRINSIC)
              break;
            if (prev->opcode == NYR_MACH_LOAD &&
                prev->effects == NYR_MACH_EFFECT_READ_MEMORY &&
                !prev->narrow32 && prev->byte_width == in->byte_width &&
                prev->mem_size == in->mem_size &&
                prev->src0.kind == NYR_MACH_OPERAND_VREG &&
                prev->src0.as.reg == addr &&
                prev->src1.kind == NYR_MACH_OPERAND_NONE &&
                prev->src2.kind == NYR_MACH_OPERAND_NONE &&
                prev->dst.kind == NYR_MACH_OPERAND_VREG &&
                prev->dst.as.reg < mach->vreg_len &&
                prev->dst.as.reg != in->dst.as.reg &&
                defs[prev->dst.as.reg] == 1 &&
                mach->vreg_types[prev->dst.as.reg] == NYR_MACH_TYPE_I64 &&
                mach->vreg_classes[prev->dst.as.reg] == NYR_MACH_REGCLASS_GPR &&
                !prev->args_len && !prev->args && !prev->arg_sizes) {
              in->opcode = NYR_MACH_COPY;
              in->src0 = prev->dst;
              in->effects = NYR_MACH_EFFECT_NONE;
              break;
            }
            if (prev->opcode != NYR_MACH_STORE &&
                prev->dst.kind == NYR_MACH_OPERAND_VREG &&
                prev->dst.as.reg == addr)
              break;
          }
        }
      }
      free(defs);
    }
  }
  free(alias);
  free(aliased);
  free(first_use_block);
  free(multi_block_use);
}

/*
 * Forward a non-PHI copy across a single-predecessor edge when its result is
 * used only in the successor.  A copy emitted immediately before a branch
 * often snapshots a loop value for the body while the original value remains
 * unchanged until the body latch.  Keeping the snapshot as a separate vreg
 * forces a home store/reload even though the source already carries exactly
 * the value needed by the successor.
 *
 * The proof is deliberately local: the successor has one predecessor, the
 * source is not redefined between the copy and the successor uses, and the
 * destination has no uses outside that successor.  Synthetic PHI parallel
 * copies are excluded because their source snapshots must survive the whole
 * edge bundle.
 */
static void mach_propagate_single_pred_copies(nyr_mach_func_t *mach) {
  if (!mach || !mach->block_len || !mach->vreg_len ||
      mach->block_len > UINT32_MAX ||
      mach->block_len > SIZE_MAX / sizeof(uint32_t) ||
      mach->vreg_len > SIZE_MAX / sizeof(uint32_t))
    return;
  size_t *pred_count = calloc(mach->block_len, sizeof(*pred_count));
  uint32_t *only_pred = malloc(mach->block_len * sizeof(*only_pred));
  uint32_t *use_block = malloc(mach->vreg_len * sizeof(*use_block));
  uint8_t *multi_use = calloc(mach->vreg_len, sizeof(*multi_use));
  if (!pred_count || !only_pred || !use_block || !multi_use) {
    free(pred_count);
    free(only_pred);
    free(use_block);
    free(multi_use);
    return;
  }
  for (size_t b = 0; b < mach->block_len; ++b)
    only_pred[b] = UINT32_MAX;
  for (size_t v = 0; v < mach->vreg_len; ++v)
    use_block[v] = UINT32_MAX;

  /*
   * Record unique predecessor blocks for explicit branches and fallthrough.
   */
  for (size_t b = 0; b < mach->block_len; ++b) {
    const nyr_mach_block_t *block = &mach->blocks[b];
    bool has_unconditional = false, has_return = false, has_conditional = false;
    for (size_t n = 0; n < block->inst_count; ++n) {
      const nyr_mach_inst_t *in = &mach->insts[block->first_inst + n];
      if ((in->opcode == NYR_MACH_BR || in->opcode == NYR_MACH_BR_IF) &&
          in->src1.kind == NYR_MACH_OPERAND_BLOCK &&
          in->src1.as.block_index < mach->block_len) {
        uint32_t target = in->src1.as.block_index;
        if (only_pred[target] != b) {
          only_pred[target] = (uint32_t)b;
          ++pred_count[target];
        }
        has_unconditional |= in->opcode == NYR_MACH_BR;
        has_conditional |= in->opcode == NYR_MACH_BR_IF;
      }
      has_return |= in->opcode == NYR_MACH_RET;
    }
    if ((has_conditional || (!has_unconditional && !has_return)) &&
        b + 1 < mach->block_len && only_pred[b + 1] != b) {
      only_pred[b + 1] = (uint32_t)b;
      ++pred_count[b + 1];
    }
  }

  /*
   * Destination uses must be confined to one successor block.
   */
  for (size_t b = 0; b < mach->block_len; ++b) {
    const nyr_mach_block_t *block = &mach->blocks[b];
    for (size_t n = 0; n < block->inst_count; ++n) {
      const nyr_mach_inst_t *in = &mach->insts[block->first_inst + n];
      const nyr_mach_operand_t *ops[] = {&in->src0, &in->src1, &in->src2,
                                         in->opcode == NYR_MACH_STORE ? &in->dst
                                                                      : NULL};
      for (size_t k = 0; k < sizeof(ops) / sizeof(ops[0]); ++k) {
        const nyr_mach_operand_t *op = ops[k];
        if (!op || op->kind != NYR_MACH_OPERAND_VREG ||
            op->as.reg >= mach->vreg_len)
          continue;
        uint32_t v = op->as.reg;
        if (use_block[v] == UINT32_MAX)
          use_block[v] = (uint32_t)b;
        else if (use_block[v] != b)
          multi_use[v] = 1;
      }
      for (size_t k = 0; k < in->args_len; ++k) {
        const nyr_mach_operand_t *op = &in->args[k];
        if (op->kind != NYR_MACH_OPERAND_VREG || op->as.reg >= mach->vreg_len)
          continue;
        uint32_t v = op->as.reg;
        if (use_block[v] == UINT32_MAX)
          use_block[v] = (uint32_t)b;
        else if (use_block[v] != b)
          multi_use[v] = 1;
      }
    }
  }

  size_t forwarded = 0;
  for (size_t p = 0; p < mach->block_len; ++p) {
    nyr_mach_block_t *pred = &mach->blocks[p];
    size_t end = (size_t)pred->first_inst + pred->inst_count;
    if (end > mach->inst_len)
      continue;
    for (size_t pc = pred->first_inst; pc < end; ++pc) {
      nyr_mach_inst_t *copy = &mach->insts[pc];
      if (copy->opcode != NYR_MACH_COPY || copy->source_pc == UINT32_MAX ||
          copy->narrow32 || copy->dst.kind != NYR_MACH_OPERAND_VREG ||
          copy->src0.kind != NYR_MACH_OPERAND_VREG ||
          copy->dst.as.reg >= mach->vreg_len ||
          copy->src0.as.reg >= mach->vreg_len)
        continue;
      uint32_t dst = copy->dst.as.reg, src = copy->src0.as.reg;
      if (dst == src || !mach->vreg_types || !mach->vreg_classes ||
          mach->vreg_types[dst] != mach->vreg_types[src] ||
          mach->vreg_classes[dst] != mach->vreg_classes[src] ||
          use_block[dst] == UINT32_MAX || multi_use[dst])
        continue;
      uint32_t succ = use_block[dst];
      if (succ >= mach->block_len || succ == p || pred_count[succ] != 1 ||
          only_pred[succ] != p)
        continue;

      /*
       * No source redefinition may intervene before or within the uses.
       */
      bool source_redefined = false, destination_redefined = false;
      for (size_t n = pc + 1; n < end; ++n) {
        const nyr_mach_inst_t *in = &mach->insts[n];
        if (in->opcode == NYR_MACH_STORE ||
            in->dst.kind != NYR_MACH_OPERAND_VREG)
          continue;
        source_redefined |= in->dst.as.reg == src;
        destination_redefined |= in->dst.as.reg == dst;
      }
      if (destination_redefined)
        continue;
      nyr_mach_block_t *target = &mach->blocks[succ];
      size_t target_end = (size_t)target->first_inst + target->inst_count;
      if (target_end > mach->inst_len)
        continue;
      bool saw_use = false, valid = true;
      for (size_t n = target->first_inst; n < target_end; ++n) {
        nyr_mach_inst_t *in = &mach->insts[n];
        if (in->source_pc == UINT32_MAX && in->opcode == NYR_MACH_COPY &&
            in->src0.kind == NYR_MACH_OPERAND_VREG && in->src0.as.reg == dst) {
          valid = false;
          break;
        }
        if (in->opcode != NYR_MACH_STORE &&
            in->dst.kind == NYR_MACH_OPERAND_VREG && in->dst.as.reg == dst) {
          valid = false;
          break;
        }
        const nyr_mach_operand_t *ops[] = {
            &in->src0, &in->src1, &in->src2,
            in->opcode == NYR_MACH_STORE ? &in->dst : NULL};
        for (size_t k = 0; k < sizeof(ops) / sizeof(ops[0]); ++k)
          if (ops[k] && ops[k]->kind == NYR_MACH_OPERAND_VREG &&
              ops[k]->as.reg == dst) {
            saw_use = true;
            if (source_redefined)
              valid = false;
          }
        for (size_t k = 0; k < in->args_len; ++k)
          if (in->args[k].kind == NYR_MACH_OPERAND_VREG &&
              in->args[k].as.reg == dst) {
            saw_use = true;
            if (source_redefined)
              valid = false;
          }
        if (in->opcode != NYR_MACH_STORE &&
            in->dst.kind == NYR_MACH_OPERAND_VREG && in->dst.as.reg == src)
          source_redefined = true;
      }
      if (!valid || !saw_use)
        continue;

      for (size_t n = target->first_inst; n < target_end; ++n) {
        nyr_mach_inst_t *in = &mach->insts[n];
        nyr_mach_operand_t *ops[] = {&in->src0, &in->src1, &in->src2,
                                     in->opcode == NYR_MACH_STORE ? &in->dst
                                                                  : NULL};
        for (size_t k = 0; k < sizeof(ops) / sizeof(ops[0]); ++k)
          if (ops[k] && ops[k]->kind == NYR_MACH_OPERAND_VREG &&
              ops[k]->as.reg == dst)
            ops[k]->as.reg = src;
        for (size_t k = 0; k < in->args_len; ++k)
          if (in->args[k].kind == NYR_MACH_OPERAND_VREG &&
              in->args[k].as.reg == dst)
            in->args[k].as.reg = src;
      }
      uint32_t source_pc = copy->source_pc;
      *copy = (nyr_mach_inst_t){.opcode = NYR_MACH_NOP, .source_pc = source_pc};
      ++forwarded;
    }
  }
  if (forwarded && ny_dbg_is_enabled(NY_DBG_HEURISTIC))
    ny_dbg_heuristic_event("MACHINE_EDGE_COPY", "forwarded=%zu", forwarded);

  size_t loads_forwarded = 0;
  for (size_t succ = 0; succ < mach->block_len; ++succ) {
    if (pred_count[succ] != 1 || only_pred[succ] == UINT32_MAX ||
        only_pred[succ] >= mach->block_len || only_pred[succ] == succ)
      continue;
    const nyr_mach_block_t *pb = &mach->blocks[only_pred[succ]];
    const nyr_mach_block_t *sb = &mach->blocks[succ];
    size_t pe = (size_t)pb->first_inst + pb->inst_count;
    size_t se = (size_t)sb->first_inst + sb->inst_count;
    if (pe > mach->inst_len || se > mach->inst_len)
      continue;
    for (size_t ti = sb->first_inst; ti < se; ++ti) {
      nyr_mach_inst_t *load = &mach->insts[ti];
      if (load->opcode != NYR_MACH_LOAD || load->byte_width || load->narrow32 ||
          load->effects != NYR_MACH_EFFECT_READ_MEMORY ||
          load->source_pc == UINT32_MAX ||
          load->dst.kind != NYR_MACH_OPERAND_VREG ||
          load->src0.kind != NYR_MACH_OPERAND_VREG)
        continue;
      size_t ta = SIZE_MAX;
      for (size_t i = sb->first_inst; i < ti; ++i) {
        const nyr_mach_inst_t *in = &mach->insts[i];
        if (in->opcode != NYR_MACH_STORE &&
            in->dst.kind == NYR_MACH_OPERAND_VREG &&
            in->dst.as.reg == load->src0.as.reg)
          ta = in->opcode == NYR_MACH_LEA ? i : SIZE_MAX;
      }
      if (ta == SIZE_MAX)
        continue;
      const nyr_mach_inst_t *tlea = &mach->insts[ta];
      if (tlea->src0.kind != NYR_MACH_OPERAND_VREG ||
          tlea->src1.kind != NYR_MACH_OPERAND_VREG ||
          tlea->src2.kind != NYR_MACH_OPERAND_IMM)
        continue;
      for (size_t pi = pe; pi-- > pb->first_inst;) {
        const nyr_mach_inst_t *prev = &mach->insts[pi];
        if (prev->opcode != NYR_MACH_LOAD || prev->byte_width ||
            prev->narrow32 || prev->source_pc == UINT32_MAX ||
            prev->effects != NYR_MACH_EFFECT_READ_MEMORY ||
            prev->dst.kind != NYR_MACH_OPERAND_VREG ||
            prev->src0.kind != NYR_MACH_OPERAND_VREG ||
            prev->dst.as.reg >= mach->vreg_len ||
            load->dst.as.reg >= mach->vreg_len || !mach->vreg_types ||
            !mach->vreg_classes ||
            mach->vreg_types[load->dst.as.reg] != NYR_MACH_TYPE_I64 ||
            mach->vreg_types[prev->dst.as.reg] !=
                mach->vreg_types[load->dst.as.reg] ||
            mach->vreg_classes[prev->dst.as.reg] !=
                mach->vreg_classes[load->dst.as.reg])
          continue;
        size_t pa = SIZE_MAX;
        for (size_t i = pb->first_inst; i < pi; ++i) {
          const nyr_mach_inst_t *in = &mach->insts[i];
          if (in->opcode != NYR_MACH_STORE &&
              in->dst.kind == NYR_MACH_OPERAND_VREG &&
              in->dst.as.reg == prev->src0.as.reg)
            pa = in->opcode == NYR_MACH_LEA ? i : SIZE_MAX;
        }
        if (pa == SIZE_MAX)
          continue;
        const nyr_mach_inst_t *plea = &mach->insts[pa];
        if (plea->src0.kind != NYR_MACH_OPERAND_VREG ||
            plea->src1.kind != NYR_MACH_OPERAND_VREG ||
            plea->src2.kind != NYR_MACH_OPERAND_IMM ||
            plea->immediate != tlea->immediate ||
            plea->src0.as.reg != tlea->src0.as.reg ||
            plea->src1.as.reg != tlea->src1.as.reg ||
            plea->src2.as.imm != tlea->src2.as.imm ||
            plea->narrow32 != tlea->narrow32)
          continue;
        uint32_t a = plea->src0.as.reg, b = plea->src1.as.reg;
        uint32_t value = prev->dst.as.reg;
        bool safe = true;
        for (size_t i = pb->first_inst; safe && i <= pa; ++i) {
          const nyr_mach_inst_t *in = &mach->insts[i];
          if (i != pa &&
              (in->opcode == NYR_MACH_BR || in->opcode == NYR_MACH_BR_IF ||
               in->opcode == NYR_MACH_RET || in->opcode == NYR_MACH_TRAP))
            safe = false;
        }
        for (size_t i = pa + 1; safe && i < pe; ++i) {
          const nyr_mach_inst_t *in = &mach->insts[i];
          if ((i < pi &&
               (in->opcode == NYR_MACH_BR || in->opcode == NYR_MACH_BR_IF ||
                in->opcode == NYR_MACH_RET || in->opcode == NYR_MACH_TRAP)) ||
              (i > pi && (in->effects & (NYR_MACH_EFFECT_WRITE_MEMORY |
                                         NYR_MACH_EFFECT_CALL))) ||
              in->opcode == NYR_MACH_INLINE_ASM ||
              in->opcode == NYR_MACH_INTRINSIC ||
              (in->opcode != NYR_MACH_STORE &&
               in->dst.kind == NYR_MACH_OPERAND_VREG &&
               (in->dst.as.reg == a || in->dst.as.reg == b ||
                (i > pi && in->dst.as.reg == value))))
            safe = false;
        }
        for (size_t i = sb->first_inst; safe && i < ti; ++i) {
          const nyr_mach_inst_t *in = &mach->insts[i];
          if ((in->effects &
               (NYR_MACH_EFFECT_WRITE_MEMORY | NYR_MACH_EFFECT_CALL |
                NYR_MACH_EFFECT_CONTROL)) ||
              in->opcode == NYR_MACH_INLINE_ASM ||
              in->opcode == NYR_MACH_INTRINSIC || in->opcode == NYR_MACH_BR ||
              in->opcode == NYR_MACH_BR_IF || in->opcode == NYR_MACH_RET ||
              in->opcode == NYR_MACH_TRAP ||
              (in->opcode != NYR_MACH_STORE &&
               in->dst.kind == NYR_MACH_OPERAND_VREG &&
               (in->dst.as.reg == a || in->dst.as.reg == b ||
                in->dst.as.reg == value)))
            safe = false;
        }
        if (!safe)
          continue;
        nyr_mach_operand_t dst = load->dst;
        uint32_t source_pc = load->source_pc;
        *load = (nyr_mach_inst_t){.opcode = NYR_MACH_COPY,
                                  .dst = dst,
                                  .src0 = prev->dst,
                                  .source_pc = source_pc};
        ++loads_forwarded;
        break;
      }
    }
  }
  if (loads_forwarded && ny_dbg_is_enabled(NY_DBG_HEURISTIC))
    ny_dbg_heuristic_event("MACHINE_EDGE_LOAD", "forwarded=%zu",
                           loads_forwarded);
  free(pred_count);
  free(only_pred);
  free(use_block);
  free(multi_use);
}

static nyr_mach_reg_class_t mach_type_reg_class(nyr_mach_type_t type) {
  switch (type) {
  case NYR_MACH_TYPE_I64:
  case NYR_MACH_TYPE_PTR:
    return NYR_MACH_REGCLASS_GPR;
  case NYR_MACH_TYPE_F32:
  case NYR_MACH_TYPE_F64:
    return NYR_MACH_REGCLASS_FPR;
  case NYR_MACH_TYPE_V128_I64:
  case NYR_MACH_TYPE_V128_F64:
  case NYR_MACH_TYPE_V128_F32:
  case NYR_MACH_TYPE_V256_I64:
  case NYR_MACH_TYPE_V256_F64:
    return NYR_MACH_REGCLASS_VECTOR;
  case NYR_MACH_TYPE_FLAGS:
    return NYR_MACH_REGCLASS_FLAGS;
  default:
    return NYR_MACH_REGCLASS_NONE;
  }
}

static bool mach_is_v128_type(nyr_mach_type_t type) {
  return type == NYR_MACH_TYPE_V128_I64 || type == NYR_MACH_TYPE_V128_F64 ||
         type == NYR_MACH_TYPE_V128_F32 || type == NYR_MACH_TYPE_V256_I64 ||
         type == NYR_MACH_TYPE_V256_F64;
}

static bool mach_type_valid(nyr_mach_type_t type) {
  return type > NYR_MACH_TYPE_NONE && type <= NYR_MACH_TYPE_FLAGS;
}

static int mach_u32_cmp(const void *left, const void *right) {
  uint32_t a = *(const uint32_t *)left;
  uint32_t b = *(const uint32_t *)right;
  return a < b ? -1 : a > b;
}

bool nyr_mach_alloc_typed_vreg(nyr_mach_func_t *func, nyr_mach_type_t type,
                               nyr_mach_reg_t *out) {
  nyr_mach_reg_class_t reg_class = mach_type_reg_class(type);
  if (!func || !out || reg_class == NYR_MACH_REGCLASS_NONE ||
      func->vreg_len >= UINT32_MAX)
    return false;
  if (func->vreg_len == func->vreg_cap) {
    size_t next = func->vreg_cap ? func->vreg_cap * 2 : 8;
    if (next < func->vreg_cap || next > SIZE_MAX / sizeof(*func->vreg_classes))
      return false;
    nyr_mach_reg_class_t *classes =
        ny_realloc_array(func->vreg_classes, next, sizeof(*func->vreg_classes));
    if (!classes)
      return false;
    nyr_mach_type_t *types =
        ny_realloc_array(func->vreg_types, next, sizeof(*func->vreg_types));
    if (!types) {
      func->vreg_classes = classes;
      return false;
    }
    func->vreg_classes = classes;
    func->vreg_types = types;
    func->vreg_cap = next;
  }
  *out = (nyr_mach_reg_t)func->vreg_len;
  func->vreg_classes[func->vreg_len] = reg_class;
  func->vreg_types[func->vreg_len++] = type;
  return true;
}

bool nyr_mach_alloc_vreg(nyr_mach_func_t *func, nyr_mach_reg_class_t reg_class,
                         nyr_mach_reg_t *out) {
  nyr_mach_type_t type = NYR_MACH_TYPE_NONE;
  switch (reg_class) {
  case NYR_MACH_REGCLASS_GPR:
    type = NYR_MACH_TYPE_I64;
    break;
  case NYR_MACH_REGCLASS_FPR:
    type = NYR_MACH_TYPE_F64;
    break;
  case NYR_MACH_REGCLASS_VECTOR:
    type = NYR_MACH_TYPE_V128;
    break;
  case NYR_MACH_REGCLASS_FLAGS:
    type = NYR_MACH_TYPE_FLAGS;
    break;
  default:
    break;
  }
  return nyr_mach_alloc_typed_vreg(func, type, out);
}

bool nyr_mach_alloc_typed_frame_slot(nyr_mach_func_t *func, size_t size,
                                     size_t align, nyr_mach_type_t type,
                                     bool address_taken, uint32_t *out) {
  if (!func || !out || mach_type_reg_class(type) == NYR_MACH_REGCLASS_NONE ||
      size == 0 || align == 0 || (align & (align - 1)) != 0 ||
      func->frame_slot_len >= UINT32_MAX)
    return false;
  if (func->frame_slot_len == func->frame_slot_cap &&
      !mach_grow((void **)&func->frame_slots, &func->frame_slot_cap,
                 sizeof(*func->frame_slots)))
    return false;
  *out = (uint32_t)func->frame_slot_len;
  func->frame_slots[func->frame_slot_len++] =
      (nyr_mach_frame_slot_t){.size = size,
                              .align = align,
                              .type = type,
                              .address_taken = address_taken};
  return true;
}

bool nyr_mach_alloc_frame_slot(nyr_mach_func_t *func, size_t size, size_t align,
                               bool address_taken, uint32_t *out) {
  return nyr_mach_alloc_typed_frame_slot(func, size, align, NYR_MACH_TYPE_I64,
                                         address_taken, out);
}

bool nyr_mach_begin_block(nyr_mach_func_t *func, uint32_t label,
                          uint32_t *out) {
  if (!func || !out || func->block_len >= UINT32_MAX)
    return false;
  if (func->block_len && label <= func->max_block_label)
    for (size_t i = 0; i < func->block_len; ++i)
      if (func->blocks[i].label == label)
        return false;
  if (label > func->max_block_label)
    func->max_block_label = label;
  if (func->block_len == func->block_cap &&
      !mach_grow((void **)&func->blocks, &func->block_cap,
                 sizeof(*func->blocks)))
    return false;
  *out = (uint32_t)func->block_len;
  func->blocks[func->block_len++] =
      (nyr_mach_block_t){.first_inst = (uint32_t)func->inst_len,
                         .inst_count = 0,
                         .label = label,
                         .source_pc = UINT32_MAX};
  return true;
}

bool nyr_mach_emit(nyr_mach_func_t *func, nyr_mach_inst_t inst) {
  if (!func || func->block_len == 0 || func->inst_len >= UINT32_MAX)
    return false;
  if (func->inst_len == func->inst_cap &&
      !mach_grow((void **)&func->insts, &func->inst_cap, sizeof(*func->insts)))
    return false;
  func->insts[func->inst_len++] = inst;
  ++func->blocks[func->block_len - 1].inst_count;
  return true;
}

static bool mach_verify_operand(const nyr_mach_func_t *func,
                                const nyr_mach_operand_t *operand) {
  if (operand->kind > NYR_MACH_OPERAND_SYMBOL ||
      operand->reg_class > NYR_MACH_REGCLASS_FLAGS ||
      operand->relocation > NYR_MACH_RELOC_CALL)
    return false;
  switch (operand->kind) {
  case NYR_MACH_OPERAND_NONE:
    return operand->reg_class == NYR_MACH_REGCLASS_NONE &&
           operand->relocation == NYR_MACH_RELOC_NONE && operand->addend == 0;
  case NYR_MACH_OPERAND_IMM:
    return operand->reg_class == NYR_MACH_REGCLASS_NONE &&
           operand->relocation == NYR_MACH_RELOC_NONE;
  case NYR_MACH_OPERAND_VREG:
    return operand->as.reg < func->vreg_len && func->vreg_classes &&
           func->vreg_types && operand->relocation == NYR_MACH_RELOC_NONE &&
           operand->addend == 0 &&
           func->vreg_classes[operand->as.reg] == operand->reg_class &&
           mach_type_reg_class(func->vreg_types[operand->as.reg]) ==
               operand->reg_class;
  case NYR_MACH_OPERAND_PREG:
    return operand->reg_class != NYR_MACH_REGCLASS_NONE &&
           operand->relocation == NYR_MACH_RELOC_NONE && operand->addend == 0;
  case NYR_MACH_OPERAND_FRAME:
    return operand->as.frame_index < func->frame_slot_len &&
           operand->reg_class == NYR_MACH_REGCLASS_NONE &&
           operand->relocation == NYR_MACH_RELOC_NONE && operand->addend == 0;
  case NYR_MACH_OPERAND_BLOCK:
    return operand->as.block_index < func->block_len &&
           operand->reg_class == NYR_MACH_REGCLASS_NONE &&
           operand->relocation == NYR_MACH_RELOC_NONE && operand->addend == 0;
  case NYR_MACH_OPERAND_SYMBOL:
    return operand->reg_class == NYR_MACH_REGCLASS_NONE && operand->as.symbol &&
           operand->as.symbol[0];
  }
  return false;
}

static bool mach_is_none(const nyr_mach_operand_t *operand) {
  return operand && operand->kind == NYR_MACH_OPERAND_NONE &&
         operand->reg_class == NYR_MACH_REGCLASS_NONE &&
         operand->relocation == NYR_MACH_RELOC_NONE;
}

static bool mach_is_value(const nyr_mach_operand_t *operand) {
  return operand && (operand->kind == NYR_MACH_OPERAND_VREG ||
                     operand->kind == NYR_MACH_OPERAND_PREG);
}

static nyr_mach_type_t mach_value_type(const nyr_mach_func_t *func,
                                       const nyr_mach_operand_t *operand) {
  if (!func || !operand || operand->kind != NYR_MACH_OPERAND_VREG ||
      operand->as.reg >= func->vreg_len || !func->vreg_types)
    return NYR_MACH_TYPE_NONE;
  return func->vreg_types[operand->as.reg];
}

static bool mach_same_value_type(const nyr_mach_func_t *func,
                                 const nyr_mach_operand_t *left,
                                 const nyr_mach_operand_t *right) {
  if (!mach_is_value(left) || !mach_is_value(right) ||
      left->reg_class != right->reg_class)
    return false;
  /*
   * Physical registers carry a class but not a complete type in machine form.
   * Keep manually constructed target-specific machine form legal while
   * requiring exact types whenever both operands are virtual registers.
   */
  nyr_mach_type_t left_type = mach_value_type(func, left);
  nyr_mach_type_t right_type = mach_value_type(func, right);
  return left_type == NYR_MACH_TYPE_NONE || right_type == NYR_MACH_TYPE_NONE ||
         left_type == right_type;
}

static bool mach_is_integer_value(const nyr_mach_func_t *func,
                                  const nyr_mach_operand_t *operand) {
  if (!mach_is_value(operand) || operand->reg_class != NYR_MACH_REGCLASS_GPR)
    return false;
  nyr_mach_type_t type = mach_value_type(func, operand);
  return type == NYR_MACH_TYPE_NONE || type == NYR_MACH_TYPE_I64 ||
         type == NYR_MACH_TYPE_PTR;
}

static bool mach_type_is_float(nyr_mach_type_t type) {
  return type == NYR_MACH_TYPE_F32 || type == NYR_MACH_TYPE_F64;
}

static bool mach_is_scalar_value(const nyr_mach_func_t *func,
                                 const nyr_mach_operand_t *operand) {
  nyr_mach_type_t type = mach_value_type(func, operand);
  return mach_is_value(operand) &&
         (type == NYR_MACH_TYPE_NONE || type == NYR_MACH_TYPE_I64 ||
          type == NYR_MACH_TYPE_PTR || type == NYR_MACH_TYPE_F32 ||
          type == NYR_MACH_TYPE_F64);
}

static bool mach_is_v128_value(const nyr_mach_func_t *func,
                               const nyr_mach_operand_t *operand) {
  nyr_mach_type_t type = mach_value_type(func, operand);
  return mach_is_value(operand) &&
         (type == NYR_MACH_TYPE_NONE || mach_is_v128_type(type));
}
static bool mach_is_scalar_value_or_frame(const nyr_mach_func_t *func,
                                          const nyr_mach_operand_t *operand) {
  nyr_mach_type_t type = mach_value_type(func, operand);
  if (operand->kind == NYR_MACH_OPERAND_VREG) {
    return mach_is_value(operand) &&
           (type == NYR_MACH_TYPE_NONE || type == NYR_MACH_TYPE_I64 ||
            type == NYR_MACH_TYPE_PTR || type == NYR_MACH_TYPE_F32 ||
            type == NYR_MACH_TYPE_F64);
  }
  if (operand->kind == NYR_MACH_OPERAND_FRAME) {
    return operand->as.frame_index < 1024 &&
           (type == NYR_MACH_TYPE_NONE || type == NYR_MACH_TYPE_I64 ||
            type == NYR_MACH_TYPE_PTR || type == NYR_MACH_TYPE_F32 ||
            type == NYR_MACH_TYPE_F64);
  }
  return false;
}

static bool mach_verify_opcode_shape(const nyr_mach_func_t *func,
                                     const nyr_mach_inst_t *inst) {
  if (!inst)
    return false;
  switch (inst->opcode) {
  case NYR_MACH_NOP:
    return inst->effects == NYR_MACH_EFFECT_NONE && mach_is_none(&inst->dst) &&
           mach_is_none(&inst->src0) && mach_is_none(&inst->src1) &&
           mach_is_none(&inst->src2);
  case NYR_MACH_COPY: {
    /*
     * Same-type move, IMM materialize, or scalar→V128 broadcast (SET1).
     */
    if (inst->effects != NYR_MACH_EFFECT_NONE || !mach_is_value(&inst->dst) ||
        !mach_is_none(&inst->src1) || !mach_is_none(&inst->src2))
      return false;
    if (inst->src0.kind == NYR_MACH_OPERAND_IMM)
      return true;
    if (!mach_is_value(&inst->src0))
      return false;
    if (mach_same_value_type(func, &inst->dst, &inst->src0))
      return true;
    nyr_mach_type_t dt = mach_value_type(func, &inst->dst);
    nyr_mach_type_t st = mach_value_type(func, &inst->src0);
    if (((dt == NYR_MACH_TYPE_I64 || dt == NYR_MACH_TYPE_PTR) &&
         (st == NYR_MACH_TYPE_I64 || st == NYR_MACH_TYPE_PTR ||
          st == NYR_MACH_TYPE_F64)) ||
        (dt == NYR_MACH_TYPE_F64 &&
         (st == NYR_MACH_TYPE_I64 || st == NYR_MACH_TYPE_PTR)))
      return true;
    if (dt == NYR_MACH_TYPE_V128_I64 &&
        (st == NYR_MACH_TYPE_I64 || st == NYR_MACH_TYPE_NONE))
      return true;
    if (dt == NYR_MACH_TYPE_V128_F64 &&
        (st == NYR_MACH_TYPE_F64 || st == NYR_MACH_TYPE_NONE))
      return true;
    if (dt == NYR_MACH_TYPE_V128_F32 &&
        (st == NYR_MACH_TYPE_F32 || st == NYR_MACH_TYPE_NONE))
      return true;
    if (dt == NYR_MACH_TYPE_V256_F64 &&
        (st == NYR_MACH_TYPE_F64 || st == NYR_MACH_TYPE_NONE))
      return true;
    if (dt == NYR_MACH_TYPE_V256_I64 &&
        (st == NYR_MACH_TYPE_I64 || st == NYR_MACH_TYPE_NONE))
      return true;
    return false;
  }
  case NYR_MACH_CONVERT:
    return inst->effects == NYR_MACH_EFFECT_NONE &&
           ((mach_is_scalar_value(func, &inst->dst) &&
             mach_is_scalar_value(func, &inst->src0)) ||
            (mach_is_value(&inst->dst) && mach_is_value(&inst->src0) &&
             mach_value_type(func, &inst->dst) == NYR_MACH_TYPE_V128_F64 &&
             mach_value_type(func, &inst->src0) == NYR_MACH_TYPE_V128_I64)) &&
           mach_is_none(&inst->src1) && mach_is_none(&inst->src2) &&
           !mach_same_value_type(func, &inst->dst, &inst->src0);
  case NYR_MACH_SQRT:
  case NYR_MACH_SIN:
  case NYR_MACH_COS:
    return inst->effects == NYR_MACH_EFFECT_NONE &&
           mach_is_scalar_value_or_frame(func, &inst->dst) &&
           mach_is_scalar_value_or_frame(func, &inst->src0) &&
           mach_is_none(&inst->src1) && mach_is_none(&inst->src2);
  case NYR_MACH_BSWAP64:
  case NYR_MACH_BSWAP32:
  case NYR_MACH_POPCOUNT64:
    return inst->effects == NYR_MACH_EFFECT_NONE &&
           mach_value_type(func, &inst->dst) == NYR_MACH_TYPE_I64 &&
           mach_value_type(func, &inst->src0) == NYR_MACH_TYPE_I64 &&
           mach_is_none(&inst->src1) && mach_is_none(&inst->src2);
  case NYR_MACH_LOAD:
    return (inst->effects & ~NYR_MACH_EFFECT_WRITE_MEMORY) ==
               NYR_MACH_EFFECT_READ_MEMORY &&
           mach_is_value(&inst->dst) &&
           (inst->src0.kind == NYR_MACH_OPERAND_FRAME ||
            mach_is_integer_value(func, &inst->src0)) &&
           mach_is_none(&inst->src1) && mach_is_none(&inst->src2);
  case NYR_MACH_STORE:
    return inst->effects == NYR_MACH_EFFECT_WRITE_MEMORY &&
           (inst->dst.kind == NYR_MACH_OPERAND_FRAME ||
            mach_is_integer_value(func, &inst->dst)) &&
           mach_is_value(&inst->src0) && mach_is_none(&inst->src1) &&
           mach_is_none(&inst->src2);
  case NYR_MACH_LEA:
    return inst->effects == NYR_MACH_EFFECT_NONE &&
           mach_is_integer_value(func, &inst->dst) &&
           (((inst->src0.kind == NYR_MACH_OPERAND_FRAME ||
              inst->src0.kind == NYR_MACH_OPERAND_SYMBOL) &&
             mach_is_none(&inst->src1) && mach_is_none(&inst->src2)) ||
            (mach_is_integer_value(func, &inst->src0) &&
             inst->src1.kind == NYR_MACH_OPERAND_IMM &&
             (inst->src1.as.imm == 2 || inst->src1.as.imm == 4 ||
              inst->src1.as.imm == 8) &&
             mach_is_none(&inst->src2)) ||
            (mach_is_integer_value(func, &inst->src0) &&
             mach_is_integer_value(func, &inst->src1) &&
             inst->src2.kind == NYR_MACH_OPERAND_IMM &&
             (inst->src2.as.imm == 2 || inst->src2.as.imm == 4 ||
              inst->src2.as.imm == 8)));
  case NYR_MACH_ADD:
  case NYR_MACH_SUB:
  case NYR_MACH_MUL:
  case NYR_MACH_DIV: {
    if (inst->effects != NYR_MACH_EFFECT_NONE || !mach_is_none(&inst->src2))
      return false;
    if (inst->opcode == NYR_MACH_DIV &&
        mach_is_integer_value(func, &inst->dst) &&
        mach_is_integer_value(func, &inst->src0) &&
        inst->src1.kind == NYR_MACH_OPERAND_IMM)
      return true;
    if (mach_is_v128_value(func, &inst->dst))
      return mach_same_value_type(func, &inst->dst, &inst->src0) &&
             mach_same_value_type(func, &inst->dst, &inst->src1);
    if (!mach_is_scalar_value(func, &inst->dst))
      return false;
    /*
     * Pointer arithmetic: ptr ± i64 → ptr (or i64).
     */
    if ((inst->opcode == NYR_MACH_ADD || inst->opcode == NYR_MACH_SUB) &&
        mach_is_integer_value(func, &inst->dst) &&
        mach_is_integer_value(func, &inst->src0) &&
        mach_is_integer_value(func, &inst->src1))
      return true;
    /*
     * Integer ops (div, mod, and, or, xor, shl, sar) accept any combination
     * of integer-value types (i64 or ptr) without requiring exact match.
     * Pointer-typed values flow from list.get() etc. into arithmetic.
     */
    if (mach_is_integer_value(func, &inst->dst) &&
        mach_is_integer_value(func, &inst->src0) &&
        mach_is_integer_value(func, &inst->src1))
      return true;
    return mach_same_value_type(func, &inst->dst, &inst->src0) &&
           mach_same_value_type(func, &inst->dst, &inst->src1);
  }
  case NYR_MACH_MOD:
  case NYR_MACH_AND:
  case NYR_MACH_OR:
  case NYR_MACH_XOR:
  case NYR_MACH_SHL:
  case NYR_MACH_SAR:
  case NYR_MACH_SHR:
  case NYR_MACH_ROR:
  case NYR_MACH_ROR32:
  case NYR_MACH_VSHR:
    if (inst->effects != NYR_MACH_EFFECT_NONE || !mach_is_none(&inst->src2))
      return false;
    if (inst->opcode == NYR_MACH_MOD && inst->src1.kind == NYR_MACH_OPERAND_IMM)
      return mach_is_integer_value(func, &inst->dst) &&
             mach_is_integer_value(func, &inst->src0);
    if (inst->opcode == NYR_MACH_VSHR)
      return inst->src1.kind == NYR_MACH_OPERAND_IMM &&
             inst->src1.as.imm >= 0 && inst->src1.as.imm <= 63 &&
             mach_value_type(func, &inst->dst) == NYR_MACH_TYPE_V256_I64 &&
             mach_same_value_type(func, &inst->dst, &inst->src0);
    if ((inst->opcode == NYR_MACH_SHL || inst->opcode == NYR_MACH_SAR ||
         inst->opcode == NYR_MACH_SHR || inst->opcode == NYR_MACH_ROR ||
         inst->opcode == NYR_MACH_ROR32) &&
        inst->src1.kind == NYR_MACH_OPERAND_IMM)
      return (mach_value_type(func, &inst->dst) == NYR_MACH_TYPE_I64 &&
              mach_value_type(func, &inst->src0) == NYR_MACH_TYPE_I64) ||
             ((inst->opcode == NYR_MACH_SAR || inst->opcode == NYR_MACH_SHL) &&
              inst->src1.as.imm >= 0 && inst->src1.as.imm <= 63 &&
              mach_value_type(func, &inst->dst) == NYR_MACH_TYPE_V256_I64 &&
              mach_same_value_type(func, &inst->dst, &inst->src0));
    if (inst->opcode == NYR_MACH_ROR32)
      return mach_value_type(func, &inst->dst) == NYR_MACH_TYPE_I64 &&
             mach_value_type(func, &inst->src0) == NYR_MACH_TYPE_I64 &&
             mach_value_type(func, &inst->src1) == NYR_MACH_TYPE_I64;
    if (mach_value_type(func, &inst->dst) == NYR_MACH_TYPE_V256_I64 &&
        (inst->opcode == NYR_MACH_AND || inst->opcode == NYR_MACH_OR ||
         inst->opcode == NYR_MACH_XOR))
      return mach_same_value_type(func, &inst->dst, &inst->src0) &&
             mach_same_value_type(func, &inst->dst, &inst->src1);
    if (mach_is_v128_value(func, &inst->dst))
      return mach_value_type(func, &inst->dst) == NYR_MACH_TYPE_V128_I64 &&
             mach_same_value_type(func, &inst->dst, &inst->src0) &&
             (inst->src1.kind == NYR_MACH_OPERAND_IMM ||
              mach_same_value_type(func, &inst->dst, &inst->src1) ||
              mach_is_integer_value(func, &inst->src1));
    /*
     * Integer-value mixing for MOD/AND/OR/XOR/SHL/SAR (same as DIV above).
     */
    if (mach_is_integer_value(func, &inst->dst) &&
        mach_is_integer_value(func, &inst->src0) &&
        mach_is_integer_value(func, &inst->src1))
      return true;
    return mach_same_value_type(func, &inst->dst, &inst->src0) &&
           mach_same_value_type(func, &inst->dst, &inst->src1);
  case NYR_MACH_FMA:
    return inst->effects == NYR_MACH_EFFECT_NONE &&
           (mach_is_scalar_value(func, &inst->dst) ||
            mach_is_v128_value(func, &inst->dst)) &&
           mach_same_value_type(func, &inst->dst, &inst->src0) &&
           mach_same_value_type(func, &inst->dst, &inst->src1) &&
           mach_same_value_type(func, &inst->dst, &inst->src2);
  case NYR_MACH_SHUFFLE:
    return inst->effects == NYR_MACH_EFFECT_NONE &&
           mach_is_v128_value(func, &inst->dst) &&
           mach_same_value_type(func, &inst->dst, &inst->src0) &&
           inst->src1.kind == NYR_MACH_OPERAND_IMM && mach_is_none(&inst->src2);
  case NYR_MACH_SELECT:
    return inst->effects == NYR_MACH_EFFECT_NONE &&
           inst->condition == NYR_MACH_COND_NE &&
           ((mach_value_type(func, &inst->dst) == NYR_MACH_TYPE_I64 &&
             mach_value_type(func, &inst->src0) == NYR_MACH_TYPE_I64 &&
             mach_value_type(func, &inst->src1) == NYR_MACH_TYPE_I64 &&
             mach_value_type(func, &inst->src2) == NYR_MACH_TYPE_I64) ||
            (mach_value_type(func, &inst->dst) == NYR_MACH_TYPE_V256_I64 &&
             mach_value_type(func, &inst->src0) == NYR_MACH_TYPE_V256_I64 &&
             mach_value_type(func, &inst->src1) == NYR_MACH_TYPE_V256_I64 &&
             mach_value_type(func, &inst->src2) == NYR_MACH_TYPE_V256_I64));
  case NYR_MACH_CMP:
    return inst->effects == NYR_MACH_EFFECT_NONE &&
           ((mach_is_integer_value(func, &inst->dst) &&
             mach_is_scalar_value(func, &inst->src0) &&
             (mach_same_value_type(func, &inst->src0, &inst->src1) ||
              (mach_is_integer_value(func, &inst->src0) &&
               mach_is_integer_value(func, &inst->src1)))) ||
            (mach_value_type(func, &inst->dst) == NYR_MACH_TYPE_V256_I64 &&
             mach_value_type(func, &inst->src0) == NYR_MACH_TYPE_V256_I64 &&
             mach_value_type(func, &inst->src1) == NYR_MACH_TYPE_V256_I64)) &&
           mach_is_none(&inst->src2);
  case NYR_MACH_CALL:
    return inst->effects == NYR_MACH_EFFECT_CALL &&
           (mach_is_none(&inst->dst) || mach_is_value(&inst->dst)) &&
           (inst->src0.kind == NYR_MACH_OPERAND_SYMBOL ||
            mach_is_value(&inst->src0)) &&
           mach_is_none(&inst->src1) && mach_is_none(&inst->src2);
  case NYR_MACH_RET:
    return inst->effects == NYR_MACH_EFFECT_CONTROL &&
           mach_is_none(&inst->dst) &&
           (mach_is_none(&inst->src0) || mach_is_value(&inst->src0)) &&
           (mach_is_none(&inst->src1) || mach_is_value(&inst->src1)) &&
           mach_is_none(&inst->src2);
  case NYR_MACH_BR:
    return inst->effects == NYR_MACH_EFFECT_CONTROL &&
           mach_is_none(&inst->dst) && mach_is_none(&inst->src0) &&
           inst->src1.kind == NYR_MACH_OPERAND_BLOCK &&
           mach_is_none(&inst->src2);
  case NYR_MACH_BR_IF:
    return inst->effects == NYR_MACH_EFFECT_CONTROL &&
           mach_is_none(&inst->dst) &&
           mach_is_integer_value(func, &inst->src0) &&
           inst->src1.kind == NYR_MACH_OPERAND_BLOCK &&
           mach_is_none(&inst->src2);
  case NYR_MACH_TRAP:
    return inst->effects == NYR_MACH_EFFECT_CONTROL &&
           mach_is_none(&inst->dst) && mach_is_none(&inst->src0) &&
           mach_is_none(&inst->src1) && mach_is_none(&inst->src2);
  case NYR_MACH_INLINE_ASM:
    return (inst->effects & NYR_MACH_EFFECT_CALL) != 0 &&
           inst->src0.kind == NYR_MACH_OPERAND_SYMBOL &&
           (mach_is_none(&inst->dst) || mach_is_value(&inst->dst)) &&
           (mach_is_none(&inst->src1) || mach_is_value(&inst->src1));
  case NYR_MACH_REDUCE_ADD: {
    nyr_mach_type_t dt = mach_value_type(func, &inst->dst);
    nyr_mach_type_t at = mach_value_type(func, &inst->src0);
    nyr_mach_type_t vt = mach_value_type(func, &inst->src1);
    bool integer_reduce =
        mach_is_integer_value(func, &inst->dst) &&
        mach_is_integer_value(func, &inst->src0) &&
        mach_is_v128_value(func, &inst->src1) &&
        (vt == NYR_MACH_TYPE_NONE || vt == NYR_MACH_TYPE_V128_I64 ||
         vt == NYR_MACH_TYPE_V256_I64);
    bool f64_reduce =
        mach_is_value(&inst->dst) && mach_is_value(&inst->src0) &&
        mach_is_v128_value(func, &inst->src1) &&
        (dt == NYR_MACH_TYPE_NONE || dt == NYR_MACH_TYPE_F64) &&
        (at == NYR_MACH_TYPE_NONE || at == NYR_MACH_TYPE_F64) &&
        (vt == NYR_MACH_TYPE_NONE || vt == NYR_MACH_TYPE_V128_F64 ||
         vt == NYR_MACH_TYPE_V256_F64);
    return inst->effects == NYR_MACH_EFFECT_NONE &&
           (integer_reduce || f64_reduce) && mach_is_none(&inst->src2);
  }
  case NYR_MACH_INTRINSIC:
    /*
     * (1) named call-like: src0=symbol + CALL effect
     * (2) CAPTURE_RET: dst=vreg, src0=IMM selector
     * (3) COPY_STRUCT: src0/src1=ptr vregs, src2=IMM size, memory effects
     */
    if (inst->src0.kind == NYR_MACH_OPERAND_SYMBOL)
      return (inst->effects & NYR_MACH_EFFECT_CALL) != 0 &&
             (mach_is_none(&inst->dst) || mach_is_value(&inst->dst));
    if (inst->src0.kind == NYR_MACH_OPERAND_IMM && mach_is_value(&inst->dst) &&
        mach_is_none(&inst->src1) && mach_is_none(&inst->src2))
      return true; /* CAPTURE_RET */
    if (mach_is_value(&inst->src0) && mach_is_value(&inst->src1) &&
        inst->src2.kind == NYR_MACH_OPERAND_IMM)
      return true; /* COPY_STRUCT */
    return false;
  case NYR_MACH_VA_SAVE:
    return inst->effects == NYR_MACH_EFFECT_WRITE_MEMORY &&
           mach_is_value(&inst->dst) &&
           inst->src0.kind == NYR_MACH_OPERAND_FRAME &&
           mach_is_none(&inst->src1) && mach_is_none(&inst->src2) &&
           (inst->immediate == 48 || inst->immediate == 176);
  case NYR_MACH_VA_START:
    return inst->effects ==
               (NYR_MACH_EFFECT_READ_MEMORY | NYR_MACH_EFFECT_WRITE_MEMORY) &&
           mach_is_none(&inst->dst) && mach_is_value(&inst->src0) &&
           mach_is_value(&inst->src1) &&
           inst->src2.kind == NYR_MACH_OPERAND_IMM;
  case NYR_MACH_VA_ARG:
    return inst->effects ==
               (NYR_MACH_EFFECT_READ_MEMORY | NYR_MACH_EFFECT_WRITE_MEMORY) &&
           mach_is_value(&inst->dst) && mach_is_value(&inst->src0) &&
           mach_is_none(&inst->src1) && mach_is_none(&inst->src2) &&
           (inst->immediate == 0 || inst->immediate == 1);
  default:
    return false;
  }
}

const char *nyr_mach_opcode_name(nyr_mach_opcode_t op) {
  switch (op) {
  case NYR_MACH_NOP:
    return "nop";
  case NYR_MACH_COPY:
    return "copy";
  case NYR_MACH_CONVERT:
    return "convert";
  case NYR_MACH_LOAD:
    return "load";
  case NYR_MACH_STORE:
    return "store";
  case NYR_MACH_LEA:
    return "lea";
  case NYR_MACH_ADD:
    return "add";
  case NYR_MACH_SUB:
    return "sub";
  case NYR_MACH_MUL:
    return "mul";
  case NYR_MACH_DIV:
    return "div";
  case NYR_MACH_MOD:
    return "mod";
  case NYR_MACH_AND:
    return "and";
  case NYR_MACH_OR:
    return "or";
  case NYR_MACH_XOR:
    return "xor";
  case NYR_MACH_SHL:
    return "shl";
  case NYR_MACH_SAR:
    return "sar";
  case NYR_MACH_SHR:
    return "shr";
  case NYR_MACH_VSHR:
    return "vshr";
  case NYR_MACH_ROR:
    return "ror";
  case NYR_MACH_ROR32:
    return "ror32";
  case NYR_MACH_BSWAP64:
    return "bswap64";
  case NYR_MACH_BSWAP32:
    return "bswap32";
  case NYR_MACH_POPCOUNT64:
    return "popcount64";
  case NYR_MACH_FMA:
    return "fma";
  case NYR_MACH_SQRT:
    return "sqrt";
  case NYR_MACH_SIN:
    return "sin";
  case NYR_MACH_COS:
    return "cos";
  case NYR_MACH_SHUFFLE:
    return "shuffle";
  case NYR_MACH_SELECT:
    return "select";
  case NYR_MACH_CMP:
    return "cmp";
  case NYR_MACH_CALL:
    return "call";
  case NYR_MACH_RET:
    return "ret";
  case NYR_MACH_BR:
    return "br";
  case NYR_MACH_BR_IF:
    return "br_if";
  case NYR_MACH_TRAP:
    return "trap";
  case NYR_MACH_INLINE_ASM:
    return "inline_asm";
  case NYR_MACH_INTRINSIC:
    return "intrinsic";
  case NYR_MACH_REDUCE_ADD:
    return "reduce_add";
  case NYR_MACH_VA_SAVE:
    return "va_save";
  case NYR_MACH_VA_START:
    return "va_start";
  case NYR_MACH_VA_ARG:
    return "va_arg";
  }
  return "???";
}

static const char *mach_type_name(nyr_mach_type_t t) {
  switch (t) {
  case NYR_MACH_TYPE_NONE:
    return "none";
  case NYR_MACH_TYPE_I64:
    return "i64";
  case NYR_MACH_TYPE_F32:
    return "f32";
  case NYR_MACH_TYPE_F64:
    return "f64";
  case NYR_MACH_TYPE_PTR:
    return "ptr";
  case NYR_MACH_TYPE_V128_I64:
    return "v128.i64";
  case NYR_MACH_TYPE_V256_I64:
    return "v256.i64";
  case NYR_MACH_TYPE_V128_F64:
    return "v128.f64";
  case NYR_MACH_TYPE_V128_F32:
    return "v128.f32";
  case NYR_MACH_TYPE_V256_F64:
    return "v256.f64";
  case NYR_MACH_TYPE_FLAGS:
    return "flags";
  }
  return "???";
}

static const char *mach_cond_name(nyr_mach_cond_t c) {
  switch (c) {
  case NYR_MACH_COND_ALWAYS:
    return "";
  case NYR_MACH_COND_EQ:
    return ".eq";
  case NYR_MACH_COND_NE:
    return ".ne";
  case NYR_MACH_COND_LT:
    return ".lt";
  case NYR_MACH_COND_LE:
    return ".le";
  case NYR_MACH_COND_GT:
    return ".gt";
  case NYR_MACH_COND_GE:
    return ".ge";
  case NYR_MACH_COND_B:
    return ".b";
  case NYR_MACH_COND_AE:
    return ".ae";
  case NYR_MACH_COND_BE:
    return ".be";
  case NYR_MACH_COND_A:
    return ".a";
  }
  return ".???";
}

static void mach_dump_operand(const nyr_mach_operand_t *op, FILE *out) {
  if (!out)
    return;
  switch (op->kind) {
  case NYR_MACH_OPERAND_VREG:
    fprintf(out, "v%u", op->as.reg);
    break;
  case NYR_MACH_OPERAND_PREG:
    fprintf(out, "p%u", op->as.reg);
    break;
  case NYR_MACH_OPERAND_IMM:
    fprintf(out, "#%ld", (long)op->as.imm);
    break;
  case NYR_MACH_OPERAND_FRAME:
    fprintf(out, "[fp+%u]", op->as.frame_index);
    break;
  case NYR_MACH_OPERAND_BLOCK:
    fprintf(out, "BB%u", op->as.block_index);
    break;
  case NYR_MACH_OPERAND_SYMBOL:
    fprintf(out, "%s", op->as.symbol ? op->as.symbol : "(null)");
    break;
  case NYR_MACH_OPERAND_NONE:
  default:
    fprintf(out, "_");
    break;
  }
  if (op->relocation != NYR_MACH_RELOC_NONE) {
    const char *rname = "reloc";
    switch (op->relocation) {
    case NYR_MACH_RELOC_ABS64:
      rname = "abs64";
      break;
    case NYR_MACH_RELOC_PC_REL32:
      rname = "pcrel32";
      break;
    case NYR_MACH_RELOC_CALL:
      rname = "call";
      break;
    default:
      break;
    }
    fprintf(out, "(%s+%ld)", rname, (long)op->addend);
  }
}

void nyr_mach_dump(const nyr_mach_func_t *func, FILE *out) {
  if (!func || !out)
    return;
  fprintf(out, "  vregs: %zu  frame_slots: %zu  blocks: %zu  insts: %zu\n",
          func->vreg_len, func->frame_slot_len, func->block_len,
          func->inst_len);
  for (size_t i = 0; i < func->vreg_len; i++) {
    fprintf(out, "  v%zu: %s %s\n", i, mach_type_name(func->vreg_types[i]),
            func->vreg_classes[i] == NYR_MACH_REGCLASS_GPR      ? "gpr"
            : func->vreg_classes[i] == NYR_MACH_REGCLASS_FPR    ? "fpr"
            : func->vreg_classes[i] == NYR_MACH_REGCLASS_VECTOR ? "vec"
            : func->vreg_classes[i] == NYR_MACH_REGCLASS_FLAGS  ? "flg"
                                                                : "???");
  }
  for (size_t s = 0; s < func->frame_slot_len; s++) {
    const nyr_mach_frame_slot_t *slot = &func->frame_slots[s];
    fprintf(out, "  slot%zu: %s align=%zu%s\n", s, mach_type_name(slot->type),
            slot->align, slot->address_taken ? " addr_taken" : "");
  }
  for (size_t b = 0; b < func->block_len; b++) {
    const nyr_mach_block_t *blk = &func->blocks[b];
    fprintf(out, "  BB%u:\n", blk->label);
    for (uint32_t ii = 0; ii < blk->inst_count; ii++) {
      uint32_t idx = blk->first_inst + ii;
      if (idx >= func->inst_len)
        break;
      const nyr_mach_inst_t *inst = &func->insts[idx];
      fprintf(out, "    %s%s", nyr_mach_opcode_name(inst->opcode),
              mach_cond_name(inst->condition));
      if (inst->source_pc != UINT32_MAX)
        fprintf(out, "  [nir=%u]", inst->source_pc);
      if (mach_is_value(&inst->dst)) {
        fprintf(out, "  ");
        mach_dump_operand(&inst->dst, out);
      }
      if (mach_is_value(&inst->src0) ||
          inst->src0.kind == NYR_MACH_OPERAND_IMM ||
          inst->src0.kind == NYR_MACH_OPERAND_FRAME ||
          inst->src0.kind == NYR_MACH_OPERAND_SYMBOL ||
          inst->src0.kind == NYR_MACH_OPERAND_BLOCK) {
        fprintf(out, ", ");
        mach_dump_operand(&inst->src0, out);
      }
      if (mach_is_value(&inst->src1) ||
          inst->src1.kind == NYR_MACH_OPERAND_IMM ||
          inst->src1.kind == NYR_MACH_OPERAND_BLOCK) {
        fprintf(out, ", ");
        mach_dump_operand(&inst->src1, out);
      }
      if (inst->src2.kind != NYR_MACH_OPERAND_NONE) {
        fprintf(out, ", ");
        mach_dump_operand(&inst->src2, out);
      }
      if (inst->opcode == NYR_MACH_LOAD || inst->opcode == NYR_MACH_STORE) {
        fprintf(out, " [mem=%u%s%s%s%s]", (unsigned)inst->mem_size,
                inst->byte_width ? " byte" : "",
                inst->sext_load ? " sext_load" : "",
                inst->sext32 ? " sext32" : "",
                inst->unsigned_arith ? " unsigned" : "");
      }
      if (inst->opcode == NYR_MACH_CALL && inst->args_len > 1) {
        fprintf(out, " [");
        for (size_t a = 1; a < inst->args_len; a++) {
          if (a > 1)
            fprintf(out, ", ");
          mach_dump_operand(&inst->args[a], out);
        }
        fprintf(out, "]");
      }
      if (inst->opcode == NYR_MACH_CALL && inst->call_clobbers)
        fprintf(out, "  clobbers=0x%lx", (unsigned long)inst->call_clobbers);
      fprintf(out, "\n");
    }
  }
}

static bool nyr_mach_mixed_enabled_s = false;
static const char *nyr_mach_mixed_filter_s = NULL;

void nyr_set_mixed_disasm(bool enabled, const char *filter) {
  nyr_mach_mixed_enabled_s = enabled;
  nyr_mach_mixed_filter_s = filter;
}

static bool nyr_mach_mixed_filter_match(const char *name) {
  if (!nyr_mach_mixed_filter_s || !nyr_mach_mixed_filter_s[0])
    return true;
  if (!name)
    return false;
  return strstr(name, nyr_mach_mixed_filter_s) != NULL;
}

/*
 * Prints NYIR source instructions [printed, upto] as a correlated group.
 */
static void mach_mixed_operand_text(const nyr_mach_operand_t *op, char *buf,
                                    size_t n) {
  switch (op->kind) {
  case NYR_MACH_OPERAND_VREG:
    snprintf(buf, n, "v%u", op->as.reg);
    break;
  case NYR_MACH_OPERAND_PREG:
    snprintf(buf, n, "p%u", op->as.reg);
    break;
  case NYR_MACH_OPERAND_IMM:
    snprintf(buf, n, "#%ld", (long)op->as.imm);
    break;
  case NYR_MACH_OPERAND_FRAME:
    snprintf(buf, n, "[fp+%u]", op->as.frame_index);
    break;
  case NYR_MACH_OPERAND_BLOCK:
    snprintf(buf, n, "BB%u", op->as.block_index);
    break;
  case NYR_MACH_OPERAND_SYMBOL:
    snprintf(buf, n, "%s", op->as.symbol ? op->as.symbol : "(null)");
    break;
  case NYR_MACH_OPERAND_NONE:
  default:
    snprintf(buf, n, "_");
    break;
  }
}

/*
 * Compact one-line rendering of a MIR instruction (same operand rules as
 * nyr_mach_dump, without the [nir=] tag: the NYIR line appears above it).
 */
static void mach_mixed_inst_text(const nyr_mach_inst_t *in, char *buf,
                                 size_t n) {
  size_t off =
      (size_t)snprintf(buf, n, "%s%s", nyr_mach_opcode_name(in->opcode),
                       mach_cond_name(in->condition));
#define MIXED_APP(...)                                                         \
  do {                                                                         \
    if (off < n)                                                               \
      off += (size_t)snprintf(buf + off, n - off, ##__VA_ARGS__);              \
  } while (0)
  char obuf[160];
  if (mach_is_value(&in->dst))
    MIXED_APP(" %s",
              (mach_mixed_operand_text(&in->dst, obuf, sizeof(obuf)), obuf));
  if (mach_is_value(&in->src0) || in->src0.kind == NYR_MACH_OPERAND_IMM ||
      in->src0.kind == NYR_MACH_OPERAND_FRAME ||
      in->src0.kind == NYR_MACH_OPERAND_SYMBOL ||
      in->src0.kind == NYR_MACH_OPERAND_BLOCK)
    MIXED_APP(", %s",
              (mach_mixed_operand_text(&in->src0, obuf, sizeof(obuf)), obuf));
  if (mach_is_value(&in->src1) || in->src1.kind == NYR_MACH_OPERAND_IMM ||
      in->src1.kind == NYR_MACH_OPERAND_BLOCK)
    MIXED_APP(", %s",
              (mach_mixed_operand_text(&in->src1, obuf, sizeof(obuf)), obuf));
  if (in->src2.kind != NYR_MACH_OPERAND_NONE)
    MIXED_APP(", %s",
              (mach_mixed_operand_text(&in->src2, obuf, sizeof(obuf)), obuf));
  if (in->opcode == NYR_MACH_CALL && in->args_len > 1) {
    MIXED_APP(" [");
    for (size_t a = 1; a < in->args_len; a++) {
      mach_mixed_operand_text(&in->args[a], obuf, sizeof(obuf));
      MIXED_APP("%s%s", a > 1 ? ", " : "", obuf);
    }
    MIXED_APP("]");
  }
#undef MIXED_APP
}

typedef struct {
  unsigned long long addr;
  unsigned long long end;
  char bytes[64];
  char text[240];
} nyr_mixed_nat_t;

/*
 * Mixed view: one NYIR source group per line, its MIR instruction on the
 * same line, then the native bytes it encoded to below.  The format is
 * intentionally terse (an optimizer diagnostic, not a reference listing):
 *
 *   bb10  L10
 *     NYIR #70-72  shl + add   -> MIR #53  lea v56, v28, v51, #8
 *       NAT 0x012e  49 8b c6   mov rax, r14
 *
 * `mov` source insts are elided when other ops share their group (they are
 * copy-expansion noise); repeated ops collapse to `nm x k`.  Colors are
 * enabled only for terminals (NO_COLOR / FORCE_COLOR / CLICOLOR_FORCE).
 */
#define MCLR_HDR "\033[1;36m"
#define MCLR_NAME "\033[1m"
#define MCLR_BB "\033[94;1m"
#define MCLR_NYIR "\033[93m"
#define MCLR_MIR "\033[35m"
#define MCLR_DIM "\033[90m"
#define MCLR_AMBER "\033[93m"
#define MCLR_CALL "\033[95m"
#define MCLR_RET "\033[91m"
#define MCLR_SEL "\033[96m"
#define MCLR_BAD "\033[1;91m"
#define MCLR_RESET "\033[0m"

static void mixed_c(FILE *out, bool on, const char *code) {
  if (on && code)
    fputs(code, out);
}

/*
 * Pad visible output to `target` columns (color codes are invisible).
 */
static void mixed_pad_to(FILE *out, bool col, size_t *vc, size_t target) {
  (void)col;
  if (*vc < target) {
    size_t pad = target - *vc;
    char sp[64];
    memset(sp, ' ', sizeof(sp));
    while (pad > 0) {
      size_t w = pad > 63 ? 63 : pad;
      fwrite(sp, 1, w, out);
      pad -= w;
    }
    *vc = target;
  }
}

static bool nyr_mixed_color_for(FILE *out) {
  if (getenv("NO_COLOR"))
    return false;
  const char *force = getenv("FORCE_COLOR");
  if (!force || !force[0])
    force = getenv("CLICOLOR_FORCE");
  if (force && force[0] && strcmp(force, "0") != 0)
    return true;
#if defined(_WIN32)
  (void)out;
  return false;
#else
  return isatty(fileno(out));
#endif
}

/*
 * Strips a leading `vN=` assignment so `v57=ld.i64 v56` renders `ld.i64 v56`.
 */
static void mixed_strip_assign(const char *src, char *dst, size_t n) {
  const char *eq = src ? strchr(src, '=') : NULL;
  if (eq && eq != src && eq[1])
    src = eq + 1;
  snprintf(dst, n, "%s", src ? src : "?");
}

/*
 * Colors the leading mnemonic of an operand text by instruction class.
 */
static void mixed_body(FILE *out, bool on, const char *text) {
  size_t wl = 0;
  while (text[wl] && text[wl] != ' ')
    wl++;
  const char *color = NULL;
  if (wl == 3 && strncmp(text, "ret", 3) == 0)
    color = MCLR_RET;
  else if (wl == 4 && strncmp(text, "call", 4) == 0)
    color = MCLR_CALL;
  else if (wl > 0 && text[0] == 'j')
    color = MCLR_AMBER;
  else if ((wl >= 4 && strncmp(text, "cmov", 4) == 0) ||
           (wl >= 3 && strncmp(text, "set", 3) == 0))
    color = MCLR_SEL;
  if (on && color) {
    fprintf(out, "%s%.*s" MCLR_RESET "%s", color, (int)wl, text, text + wl);
  } else {
    fprintf(out, "%.*s", (int)wl, text);
    fputs(text + wl, out);
  }
}

/*
 * Extracts the `op` keyword from a formatted NYIR inst, skipping a leading
 * `vN=` assignment: `v57=ld.i64 v56` -> `ld.i64`.  Pure-number texts (const
 * payloads like `v0=2000`) collapse to the typed op name `i64`/`f64`.
 */
static void mach_mixed_token(const nyr_func_t *nyir, size_t pc, char *tok,
                             size_t n, bool *is_label, char *full_out,
                             size_t full_n) {
  char full[256];
  tok[0] = '\0';
  *is_label = false;
  if (full_out && full_n)
    full_out[0] = '\0';
  if (pc >= nyir->len || !nyr_fmt_inst(&nyir->data[pc], full, sizeof(full)))
    return;
  const char *eq = strchr(full, '=');
  const char *text = eq && eq != full ? eq + 1 : full;
  if (full_out && full_n) {
    size_t fl = strlen(text);
    snprintf(full_out, full_n, "%.*s", (int)(fl > full_n - 1 ? full_n - 1 : fl),
             text);
  }
  /*
   * Trailing-colon texts are block labels: `L10`.
   */
  const char *sp = strchr(text, ' ');
  if (!sp)
    sp = text + strlen(text);
  size_t wl = (size_t)(sp - text);
  if (wl && text[wl - 1] == ':') {
    *is_label = true;
    snprintf(tok, n, "%.*s", (int)(wl - 1 > 23 ? 23 : wl - 1), text);
    return;
  }
  if (wl > 0) {
    char word[64];
    snprintf(word, sizeof(word), "%.*s", (int)(wl > 63 ? 63 : wl), text);
    bool numeric = true;
    for (size_t k = 0; word[k]; ++k) {
      if ((word[k] < '0' || word[k] > '9') && word[k] != '-' && word[k] != '.')
        numeric = false;
    }
    if (numeric) {
      bool is_float = strchr(word, '.') != NULL;
      snprintf(tok, n, "%s", is_float ? "f64" : "i64");
    } else {
      snprintf(tok, n, "%s", word);
    }
  }
}

/*
 * Mnemonic summary for NYIR insts [lo, hi]: consecutive identical ops
 * collapse to `nm x k`, MOVs hide when other ops share the group, and the
 * summary caps at four entries (`... +N`).  Returns 1 when a summary was
 * printed, -1 when the span only holds labels (caller keeps them pending
 * for the bb header), and 0 when no inst was found.
 */
static int mach_mixed_summary(const nyr_func_t *nyir, size_t lo, size_t hi,
                              char *buf, size_t n, bool *only_labels) {
  char names[24][24];
  size_t qty[24];
  size_t hn = 0;
  size_t ops_seen = 0;
  bool only_labels_all = true;
  if (hi < lo || lo >= nyir->len) {
    if (only_labels)
      *only_labels = false;
    return 0;
  }
  char single_text[64];
  size_t single_pc = (size_t)-1;
  for (size_t i = lo; i <= hi && i < nyir->len; ++i) {
    if (nyir->data[i].op == NYIR_NOP)
      continue;
    char tok[24];
    bool label;
    mach_mixed_token(nyir, i, tok, sizeof(tok), &label, single_text,
                     sizeof(single_text));
    if (!tok[0])
      continue;
    if (label) {
      continue;
    }
    single_pc = i;
    only_labels_all = false;
    const char *nm = tok[0] ? tok : "?";
    bool is_mov = strcmp(nm, "mov") == 0;
    if (!is_mov)
      ops_seen++;
    if (hn && strcmp(names[hn - 1], nm) == 0)
      qty[hn - 1]++;
    else if (hn < 24) {
      snprintf(names[hn], sizeof(names[hn]), "%s", nm);
      qty[hn] = 1;
      hn++;
    }
  }
  /*
   * Hide MOV entries when real ops share the summary.
   */
  if (ops_seen > 0) {
    size_t w = 0;
    for (size_t k = 0; k < hn; ++k)
      if (strcmp(names[k], "mov") != 0) {
        if (w != k) {
          char tmp[24];
          snprintf(tmp, sizeof(tmp), "%s", names[k]);
          snprintf(names[w], sizeof(names[w]), "%s", tmp);
          qty[w] = qty[k];
        }
        w++;
      }
    hn = w;
  }
  if (only_labels)
    *only_labels = only_labels_all;
  if (hn == 0) {
    if (only_labels_all)
      return -1;
    return 0;
  }
  /*
   * Identity groups (exactly one NYIR inst) render its full source text:
   * `ld.i64 v56`, `2000`, `mov v51` - denser info at no cost.
   */
  if (hn == 1 && qty[0] == 1 && single_pc != (size_t)-1 && single_text[0]) {
    size_t sl = strlen(single_text);
    snprintf(buf, n, "%s", sl > 40 ? single_text + sl - 40 : single_text);
    return 1;
  }
  size_t off = 0;
  for (size_t k = 0; k < hn; ++k) {
    int z = snprintf(buf + off, n - off, "%s%s%s", k ? " + " : "", names[k],
                     qty[k] > 1 ? " x " : "");
    if (z < 0 || (size_t)z >= n - off)
      break;
    off += (size_t)z;
    if (qty[k] > 1) {
      z = snprintf(buf + off, n - off, "%zu", qty[k]);
      if (z < 0 || (size_t)z >= n - off)
        break;
      off += (size_t)z;
    }
    if (k == 3) {
      z = snprintf(buf + off, n - off, " ...");
      if (z > 0 && (size_t)z < n - off)
        off += (size_t)z;
      break;
    }
  }
  return 1;
}

/*
 * Prints `NYIR <range> <summary> -> ` for the source group on one line.
 * Label-only spans stay pending (the bb header renders them later).
 * Advances *printed past the consumed span.  Returns whether anything was
 * emitted so the caller can pad the MIR line into alignment otherwise.
 */
static bool mach_mixed_nyir_header(const nyr_func_t *nyir, size_t lo, size_t hi,
                                   FILE *out, bool col, size_t *printed,
                                   size_t *vc) {
  if (hi < lo || lo >= nyir->len)
    return false;
  char summary[160];
  bool only_labels = false;
  int st =
      mach_mixed_summary(nyir, lo, hi, summary, sizeof(summary), &only_labels);
  if (st <= 0 && only_labels)
    return false; /* absorbed by `bbN Lx` */
  if (st > 0) {
    char range[24];
    if (lo == hi)
      snprintf(range, sizeof(range), "#%zu", lo);
    else
      snprintf(range, sizeof(range), "#%zu-%zu", lo, hi);
    /*
     * Colors replace the tag: the tier is yellow = source.
     */
    mixed_c(out, col, MCLR_NYIR);
    fprintf(out, col ? "%-7s " : "S %-9s ", range);
    mixed_c(out, col, MCLR_RESET);
    *vc += col ? 8u : 11u;
    fprintf(out, "%-14.14s", summary);
    *vc += 14;
    mixed_c(out, col, MCLR_DIM);
    fputs(" -> ", out);
    *vc += 4;
    mixed_c(out, col, MCLR_RESET);
  } else if (st == 0) {
    char pre[40];
    int pl =
        snprintf(pre, sizeof(pre), col ? "#%zu-%zu" : "NYIR #%zu-%zu", lo, hi);
    fputs("  ", out);
    *vc += 2;
    mixed_c(out, col, MCLR_DIM);
    fputs(pre, out);
    *vc += (size_t)pl;
    mixed_pad_to(out, col, vc, col ? 36u : 35u);
    mixed_c(out, col, MCLR_RESET);
  }
  if (printed && *printed < hi + 1)
    *printed = hi + 1;
  return true;
}

/*
 * Why the next function dumped without machine form skipped that pipeline.
 */
static _Thread_local char nyr_mach_mixed_note[256];

void nyr_mach_set_mixed_note(const char *reason) {
  snprintf(nyr_mach_mixed_note, sizeof(nyr_mach_mixed_note), "%s",
           reason && reason[0] ? reason : "machine form unavailable");
}

void nyr_mach_dump_mixed(const nyr_func_t *nyir, const nyr_mach_func_t *mach,
                         const unsigned char *native_code, size_t native_len,
                         const size_t *inst_offsets, size_t inst_offsets_len,
                         const char *name, FILE *out) {
  if (!nyir || !out || !nyr_mach_mixed_enabled_s ||
      !nyr_mach_mixed_filter_match(name))
    return;
  if (!native_code || native_len == 0 ||
      (mach && !mach->insts && !mach->blocks))
    return;
  extern const char *nyr_internal_x64_disasm(const unsigned char *b, int64_t n,
                                             int64_t base);
  const char *listing =
      nyr_internal_x64_disasm(native_code, (int64_t)native_len, (int64_t)0);
  if (!listing || !*listing)
    return;

  nyr_mixed_nat_t *rows = calloc(native_len + 1, sizeof(*rows));
  if (!rows)
    return;
  size_t row_count = 0;
  unsigned long long stop = 0;
  for (const char *p = listing; *p;) {
    const char *eol = strchr(p, '\n');
    size_t len = eol ? (size_t)(eol - p) : strlen(p);
    char line[512];
    size_t cl = len < sizeof(line) - 1 ? len : sizeof(line) - 1;
    memcpy(line, p, cl);
    line[cl] = '\0';
    char *tab1 = strchr(line, '\t');
    if (tab1) {
      char *tab2 = strchr(tab1 + 1, '\t');
      if (tab2) {
        char *tab3 = strchr(tab2 + 1, '\t');
        char *tab4 = tab3 ? strchr(tab3 + 1, '\t') : NULL;
        if (tab4) {
          char *mnem = tab1 + 1;
          char *ops = tab2 + 1;
          char *bytes = tab4 + 1;
          *tab2 = '\0';
          *tab3 = '\0';
          *tab4 = '\0';
          *tab1 = '\0';
          size_t bl = strlen(bytes);
          while (bl > 0 && bytes[bl - 1] == ' ')
            bytes[--bl] = '\0';
          nyr_mixed_nat_t *r = &rows[row_count++];
          r->addr = strtoull(line, NULL, 10);
          /*
           * The listing's size field is authoritative for the end address
           * (bytes-length/3 undercounts short encodings like `ret`).
           */
          {
            unsigned long long size = strtoull(tab3 + 1, NULL, 10);
            r->end = r->addr + size;
          }
          if (bl > 19) {
            char bb[64];
            memset(bb, 0, sizeof(bb));
            memcpy(bb, bytes, 19);
            snprintf(r->bytes, sizeof(r->bytes), "%-17s..", bb);
          } else
            snprintf(r->bytes, sizeof(r->bytes), "%-19s", bytes);
          snprintf(r->text, sizeof(r->text), ops[0] ? "%s %s" : "%s", mnem,
                   ops);
        }
      }
    }
    if (!eol)
      break;
    p = eol + 1;
  }
  if (row_count)
    stop = rows[row_count - 1].end;

  /*
   * Group ordinal: MIR insts when machine form exists, else NYIR insts.
   * inst_offsets maps group pc -> function-relative native start byte.
   */
  size_t groups = mach ? mach->inst_len : 0;
  if (!mach && inst_offsets_len == nyir->len)
    groups = nyir->len;
  bool mapped = groups > 0 && inst_offsets && inst_offsets_len >= groups;

  size_t *bounds = NULL;
  if (mapped) {
    bounds = calloc(groups + 1, sizeof(*bounds));
    if (!bounds) {
      free(rows);
      return;
    }
    size_t pending = native_len;
    for (size_t j = groups; j-- > 0;) {
      bounds[j] = pending;
      if (inst_offsets[j] != SIZE_MAX)
        pending = inst_offsets[j];
    }
  }

  bool col = nyr_mixed_color_for(out);

  mixed_c(out, col, MCLR_HDR);
  fputs("== ", out);
  mixed_c(out, col, MCLR_NAME);
  fprintf(out, "%s", name ? name : "<anon>");
  mixed_c(out, col, MCLR_DIM);
  fprintf(out, "  src=%zu ir=%zu asm=%zuB", nyir->len,
          mach ? mach->inst_len : 0, native_len);
  if (!mach && nyr_mach_mixed_note[0])
    fprintf(out, "  (%s)", nyr_mach_mixed_note);
  nyr_mach_mixed_note[0] = '\0';
  mixed_c(out, col, MCLR_RESET);
  fputc('\n', out);

  size_t nat_i = 0;
  /*
   * Native instructions emitted before the first mapped group belong to the
   * entry sequence (callee-saved pushes, frame setup) in the encoder.
   */
  if (mapped) {
    size_t first_off = SIZE_MAX;
    for (size_t j = 0; j < groups; ++j)
      if (inst_offsets[j] != SIZE_MAX) {
        first_off = inst_offsets[j];
        break;
      }
    if (first_off != SIZE_MAX && first_off != 0) {
      size_t shown = 0, total = 0;
      while (nat_i < row_count && rows[nat_i].addr < first_off) {
        total++;
        if (shown < 2) {
          const nyr_mixed_nat_t *r = &rows[nat_i++];
          mixed_c(out, col, MCLR_DIM);
          fprintf(out, col ? "      %04llx %-16s " : "  %04llx %-16s ",
                  (unsigned long long)r->addr, r->bytes);
          mixed_body(out, col, r->text);
          mixed_c(out, col, MCLR_RESET);
          fputc('\n', out);
          shown++;
        } else
          nat_i++;
      }
      if (total > shown)
        fprintf(out,
                col ? "      ... +%zu prelude\n" : "    ... +%zu prelude\n",
                total - shown);
    }
  }

  size_t printed = 0;
  /*
   * Fixed column where MIR text begins, so rows line up vertically.
   */
  const size_t MIR_COL = col ? 28u : 32u;

/*
 * Prints one NAT row in aligned sub-columns: address, bytes, mnemonic.
 */
#define MIXED_NAT_ROW(r)                                                       \
  do {                                                                         \
    const nyr_mixed_nat_t *_r = (r);                                           \
    fputs(col ? "      " : "    ", out);                                       \
    mixed_c(out, col, MCLR_DIM);                                               \
    fprintf(out, "%04llx %-16s ", (unsigned long long)_r->addr, _r->bytes);    \
    mixed_c(out, col, MCLR_RESET);                                             \
    mixed_body(out, col, _r->text);                                            \
    fputc('\n', out);                                                          \
  } while (0)

  if (mach) {
    for (size_t b = 0; b < mach->block_len; ++b) {
      const nyr_mach_block_t *blk = &mach->blocks[b];
      if (mach->block_len > 1) {
        mixed_c(out, col, MCLR_BB);
        fprintf(out, "bb%u", blk->label);
        mixed_c(out, col, MCLR_DIM);
        if (blk->source_pc != UINT32_MAX && blk->source_pc < nyir->len) {
          char lb[64];
          if (nyr_fmt_inst(&nyir->data[blk->source_pc], lb, sizeof(lb))) {
            const char *colon = strchr(lb, ':');
            if (colon && colon[1] == '\0') {
              fprintf(out, "  %.*s", (int)(colon - lb), lb);
              size_t label_pc = blk->source_pc + 1;
              if (printed < label_pc)
                printed = label_pc;
            }
          }
        }
        mixed_c(out, col, MCLR_RESET);
        fputc('\n', out);
      }
      for (uint32_t ii = 0; ii < blk->inst_count;) {
        uint32_t idx = blk->first_inst + ii;
        if (idx >= mach->inst_len)
          break;
        const nyr_mach_inst_t *inst = &mach->insts[idx];
        char mbuf[288];
        mach_mixed_inst_text(inst, mbuf, sizeof(mbuf));
        uint32_t pc = inst->source_pc;
        size_t vc = 2; /* leading "  " indent */
        fputs("  ", out);
        if (pc != UINT32_MAX && pc < nyir->len && pc >= printed)
          (void)mach_mixed_nyir_header(nyir, printed, pc, out, col, &printed,
                                       &vc);

        /*
         * Determine how many following MIR-only insts (no fresh NYIR source)
         * share this line: collapse register-shuffling into one summary.
         */
        uint32_t run_end = idx;
        while (run_end + 1 < blk->inst_count &&
               mach->insts[run_end + 1].source_pc != UINT32_MAX &&
               (mach->insts[run_end + 1].source_pc < printed ||
                mach->insts[run_end + 1].source_pc >= nyir->len))
          run_end++;

        if (run_end > idx) {
          /*
           * One collapsed line: `MIR #idx-end  copy x 2 + br`.
           */
          mixed_pad_to(out, col, &vc, MIR_COL);
          mixed_c(out, col, MCLR_MIR);
          fprintf(out, col ? "m%zu-%zu " : "M %zu-%zu ", (size_t)idx,
                  (size_t)run_end);
          mixed_c(out, col, MCLR_RESET);
          char run_sum[96];
          size_t off = 0;
          for (uint32_t k = idx; k <= run_end && off + 1 < sizeof(run_sum);
               ++k) {
            char nm[40];
            snprintf(nm, sizeof(nm), "%s%s",
                     nyr_mach_opcode_name(mach->insts[k].opcode),
                     mach_cond_name(mach->insts[k].condition));
            int z = snprintf(run_sum + off, sizeof(run_sum) - off, "%s%s",
                             off ? " + " : "", nm);
            if (z < 0 || (size_t)z >= sizeof(run_sum) - off)
              break;
            off += (size_t)z;
          }
          mixed_body(out, col, run_sum);
          fputc('\n', out);
          for (uint32_t k = idx; k <= run_end; ++k)
            if (mapped && k < inst_offsets_len && inst_offsets[k] != SIZE_MAX &&
                nat_i < row_count) {
              size_t hi_b = bounds[k];
              while (nat_i < row_count && rows[nat_i].addr < hi_b)
                MIXED_NAT_ROW(&rows[nat_i++]);
            }
          ii = (uint32_t)(run_end - blk->first_inst) + 1;
          continue;
        }

        /*
         * Single MIR instruction line.
         */
        mixed_pad_to(out, col, &vc, MIR_COL);
        mixed_c(out, col, MCLR_MIR);
        fprintf(out, col ? "m%-3zu " : "M %-4zu ", (size_t)idx);
        vc += col ? 5u : 6u;
        mixed_c(out, col, MCLR_RESET);
        mixed_body(out, col, mbuf);
        fputc('\n', out);
        if (mapped && idx < inst_offsets_len && inst_offsets[idx] != SIZE_MAX &&
            nat_i < row_count) {
          size_t hi = bounds[idx];
          while (nat_i < row_count && rows[nat_i].addr < hi)
            MIXED_NAT_ROW(&rows[nat_i++]);
        }
        ii += 1;
      }
    }
  } else if (groups) {

    for (size_t i = 0; i < nyir->len && i < inst_offsets_len; ++i) {
      if (nyir->data[i].op != NYIR_NOP) {
        char full[256];
        if (nyr_fmt_inst(&nyir->data[i], full, sizeof(full))) {
          char plain[256];
          mixed_strip_assign(full, plain, sizeof(plain));
          size_t vc = 2;
          fputs("  ", out);
          mixed_c(out, col, MCLR_NYIR);
          fprintf(out, "S %-7zu %-14.14s", i, plain);
          vc += 23;
          mixed_c(out, col, MCLR_RESET);
          mixed_pad_to(out, col, &vc, MIR_COL);
          /*
           * plain path prints full text inline (identity groups)
           */
          fputs(plain, out);
          fputc('\n', out);
        }
      }
      if (inst_offsets[i] != SIZE_MAX && nat_i < row_count && bounds) {
        while (nat_i < row_count && rows[nat_i].addr < bounds[i])
          MIXED_NAT_ROW(&rows[nat_i++]);
      }
    }
    free(bounds);
    bounds = NULL;
  }

  /*
   * Source instructions the machine form never materialized (phis, virtual
   * branches).
   */
  if (printed < nyir->len) {
    char summary[160];
    bool only_labels_tail = false;
    if (mach_mixed_summary(nyir, printed, nyir->len - 1, summary,
                           sizeof(summary), &only_labels_tail) > 0) {
      fputs("  ", out);
      mixed_c(out, col, MCLR_DIM);
      if (printed == nyir->len - 1)
        fprintf(out, "NYIR #%zu", printed);
      else
        fprintf(out, "NYIR #%zu-%zu", printed, nyir->len - 1);
      mixed_c(out, col, MCLR_RESET);
      if (summary[0]) {
        fputs("  ", out);
        fputs(summary, out);
      }
      fprintf(out, "  (virtual)");
      mixed_c(out, col, MCLR_RESET);
      fputc('\n', out);
    }
  }
  free(bounds);

  if (nat_i < row_count) {
    mixed_c(out, col, MCLR_DIM);
    fputs("- unassigned native -\n", out);
    mixed_c(out, col, MCLR_RESET);
    while (nat_i < row_count)
      MIXED_NAT_ROW(&rows[nat_i++]);
  }

  if (stop < native_len) {
    mixed_c(out, col, MCLR_BAD);
    fprintf(out, "! decode stopped at 0x%04llx - %zu of %zu bytes undecoded\n",
            stop, (size_t)(native_len - stop), native_len);
    mixed_c(out, col, MCLR_RESET);
  }
  free(rows);
}

bool nyr_mach_verify(const nyr_mach_func_t *func, unsigned caps, char *err,
                     size_t err_len) {
  (void)caps;
  if (!func)
    return nyr_err(err, err_len, "machine IR verify: missing function");
  if ((func->inst_len && !func->insts) || (func->block_len && !func->blocks) ||
      (func->frame_slot_len && !func->frame_slots))
    return nyr_err(
        err, err_len,
        "machine IR verify: instruction, block, or frame storage is missing");
  if (func->param_types || func->param_abi ||
      func->param_aggregate_home_slots) {
    if (!func->param_types || !func->param_abi ||
        !func->param_aggregate_home_slots || !func->param_count)
      return nyr_err(err, err_len,
                     "machine IR verify: incomplete aggregate parameter ABI");
    for (size_t i = 0; i < func->param_count; ++i) {
      if (func->param_types[i] > NYIR_PARAM_I8)
        return nyr_err(err, err_len,
                       "machine IR verify: invalid parameter type metadata");
      uint32_t packed = func->param_abi[i];
      if (func->param_types[i] == NYIR_PARAM_AGGREGATE) {
        uint32_t size = NYIR_ARG_AGG_SIZE(packed);
        uint32_t chunks = (size + 7u) / 8u;
        if (!chunks)
          chunks = 1;
        uint32_t home = func->param_aggregate_home_slots[i];
        bool indirect = NYIR_ARG_AGG_CLASS(packed, 0) == NYIR_ARG_CLASS_MEMORY;
        if ((!size && (NYIR_ARG_AGG_CLASS(packed, 0) != NYIR_ARG_CLASS_NONE ||
                       NYIR_ARG_AGG_CLASS(packed, 1) != NYIR_ARG_CLASS_NONE)) ||
            (indirect ? home != UINT32_MAX
                      : home == UINT32_MAX || home > func->frame_slot_len ||
                            chunks > func->frame_slot_len - home))
          return nyr_err(err, err_len,
                         "machine IR verify: invalid aggregate parameter home");
      } else if (packed || func->param_aggregate_home_slots[i] != UINT32_MAX) {
        return nyr_err(
            err, err_len,
            "machine IR verify: scalar parameter has aggregate metadata");
      }
    }
  }
  if (func->vreg_len && (!func->vreg_classes || !func->vreg_types))
    return nyr_err(err, err_len,
                   "machine IR verify: virtual-register metadata is missing");
  for (size_t reg = 0; reg < func->vreg_len; ++reg) {
    if (!mach_type_valid(func->vreg_types[reg]) ||
        mach_type_reg_class(func->vreg_types[reg]) != func->vreg_classes[reg])
      return nyr_err(
          err, err_len,
          "machine IR verify: virtual register %zu has invalid type metadata",
          reg);
  }
  for (size_t slot = 0; slot < func->frame_slot_len; ++slot) {
    const nyr_mach_frame_slot_t *frame = &func->frame_slots[slot];
    if (frame->size == 0 || frame->align == 0 ||
        (frame->align & (frame->align - 1)) != 0 ||
        !mach_type_valid(frame->type))
      return nyr_err(err, err_len,
                     "machine IR verify: frame slot %zu has invalid layout",
                     slot);
  }
  if (func->block_len) {
    if (func->block_len > SIZE_MAX / sizeof(uint32_t))
      return nyr_err(err, err_len,
                     "machine IR verify: block label table is too large");
    uint32_t *labels = ny_malloc_array(func->block_len, sizeof(*labels));
    if (!labels)
      return nyr_err(err, err_len, "machine IR verify: out of memory");
    for (size_t block = 0; block < func->block_len; ++block)
      labels[block] = func->blocks[block].label;
    qsort(labels, func->block_len, sizeof(*labels), mach_u32_cmp);
    for (size_t block = 1; block < func->block_len; ++block) {
      if (labels[block - 1] == labels[block]) {
        uint32_t duplicate = labels[block];
        free(labels);
        return nyr_err(err, err_len,
                       "machine IR verify: duplicate block label %u",
                       duplicate);
      }
    }
    free(labels);
  }
  size_t next_inst = 0;
  for (size_t block = 0; block < func->block_len; ++block) {
    const nyr_mach_block_t *current = &func->blocks[block];
    if (current->first_inst != next_inst ||
        current->inst_count > func->inst_len - next_inst)
      return nyr_err(
          err, err_len,
          "machine IR verify: block %zu instruction range is invalid", block);
    next_inst += current->inst_count;
  }
  if (next_inst != func->inst_len)
    return nyr_err(err, err_len,
                   "machine IR verify: instructions are outside blocks");
  for (size_t i = 0; i < func->inst_len; ++i) {
    const nyr_mach_inst_t *inst = &func->insts[i];
    const char *bad_field = NULL;
    const char *bad_operand_name = NULL;
    const nyr_mach_operand_t *bad_operand = NULL;
    if (inst->opcode > NYR_MACH_POPCOUNT64)
      bad_field = "opcode";
    else if (inst->condition > NYR_MACH_COND_GE)
      bad_field = "condition";
    else if ((inst->effects &
              ~(NYR_MACH_EFFECT_READ_MEMORY | NYR_MACH_EFFECT_WRITE_MEMORY |
                NYR_MACH_EFFECT_CALL | NYR_MACH_EFFECT_CONTROL)) != 0)
      bad_field = "effects";
    else if (!mach_verify_operand(func, &inst->dst)) {
      bad_field = "operand";
      bad_operand_name = "dst";
      bad_operand = &inst->dst;
    } else if (!mach_verify_operand(func, &inst->src0)) {
      bad_field = "operand";
      bad_operand_name = "src0";
      bad_operand = &inst->src0;
    } else if (!mach_verify_operand(func, &inst->src1)) {
      bad_field = "operand";
      bad_operand_name = "src1";
      bad_operand = &inst->src1;
    } else if (!mach_verify_operand(func, &inst->src2)) {
      bad_field = "operand";
      bad_operand_name = "src2";
      bad_operand = &inst->src2;
    }
    if (bad_field) {
      if (ny_trace_enabled("NY_TRACE_MACHINE_FAIL")) {
        if (bad_operand) {
          ny_trace_line("MACHINE_FAIL",
                        "malformed machine instruction %zu field=%s.%s "
                        "kind=%u reg_class=%u relocation=%u addend=%lld",
                        i, bad_field, bad_operand_name,
                        (unsigned)bad_operand->kind,
                        (unsigned)bad_operand->reg_class,
                        (unsigned)bad_operand->relocation,
                        (long long)bad_operand->addend);
          if (bad_operand->kind == NYR_MACH_OPERAND_FRAME)
            ny_trace_line(
                "MACHINE_FAIL",
                "machine instruction %zu source_pc=%u opcode=%u %s frame=%u "
                "frame_slots=%zu",
                i, inst->source_pc, (unsigned)inst->opcode, bad_operand_name,
                bad_operand->as.frame_index, func->frame_slot_len);
          else if (bad_operand->kind == NYR_MACH_OPERAND_BLOCK)
            ny_trace_line(
                "MACHINE_FAIL",
                "machine instruction %zu source_pc=%u opcode=%u %s block=%u "
                "blocks=%zu",
                i, inst->source_pc, (unsigned)inst->opcode, bad_operand_name,
                bad_operand->as.block_index, func->block_len);
        } else {
          ny_trace_line("MACHINE_FAIL",
                        "malformed machine instruction %zu field=%s "
                        "opcode=%u condition=%u effects=%u",
                        i, bad_field, (unsigned)inst->opcode,
                        (unsigned)inst->condition, (unsigned)inst->effects);
        }
      }
      return nyr_err(err, err_len,
                     "machine IR verify: instruction %zu is malformed", i);
    }
    if (inst->opcode != NYR_MACH_CALL &&
        (inst->args_len != 0 || inst->arg_sizes != NULL ||
         inst->call_is_extern || inst->call_has_sret ||
         inst->call_is_variadic || inst->call_clobbers != 0))
      return nyr_err(
          err, err_len,
          "machine IR verify: non-call instruction %zu has arguments", i);
    if (inst->args_len && !inst->args)
      return nyr_err(
          err, err_len,
          "machine IR verify: call instruction %zu has no argument storage", i);
    if (inst->arg_sizes && !inst->args)
      return nyr_err(err, err_len,
                     "machine IR verify: call instruction %zu has ABI "
                     "metadata without arguments",
                     i);
    if (inst->opcode == NYR_MACH_CMP) {
      if (inst->condition == NYR_MACH_COND_ALWAYS)
        return nyr_err(
            err, err_len,
            "machine IR verify: comparison instruction %zu has no predicate",
            i);
    } else if (inst->opcode == NYR_MACH_SELECT) {
      if (inst->condition != NYR_MACH_COND_NE)
        return nyr_err(
            err, err_len,
            "machine IR verify: select instruction %zu must test nonzero", i);
    } else if (inst->condition != NYR_MACH_COND_ALWAYS) {
      return nyr_err(
          err, err_len,
          "machine IR verify: non-comparison instruction %zu has a predicate",
          i);
    }
    for (size_t arg = 0; arg < inst->args_len; ++arg)
      if (!mach_verify_operand(func, &inst->args[arg]) ||
          !mach_is_value(&inst->args[arg]))
        return nyr_err(
            err, err_len,
            "machine IR verify: call instruction %zu has an invalid argument",
            i);
    if (!mach_verify_opcode_shape(func, inst))
      return nyr_err(
          err, err_len,
          "machine IR verify: instruction %zu (%s) has an invalid opcode shape"
          " [dst=%s src0=%s src1=%s]",
          i, nyr_mach_opcode_name(inst->opcode),
          mach_type_name(mach_value_type(func, &inst->dst)),
          mach_type_name(mach_value_type(func, &inst->src0)),
          mach_type_name(mach_value_type(func, &inst->src1)));
  }
  for (size_t block = 0; block < func->block_len; ++block) {
    const nyr_mach_block_t *current = &func->blocks[block];
    for (size_t n = 0; n < current->inst_count; ++n) {
      const nyr_mach_inst_t *inst = &func->insts[current->first_inst + n];
      size_t remaining = current->inst_count - n - 1;
      if ((inst->opcode == NYR_MACH_RET || inst->opcode == NYR_MACH_BR ||
           inst->opcode == NYR_MACH_TRAP) &&
          remaining)
        return nyr_err(
            err, err_len,
            "machine IR verify: block %zu has instructions after a terminator",
            block);
      /*
       * BR_IF transfers only its taken edge; following instructions are its
       * legal fallthrough.  Bounds checks deliberately use that form before
       * the rest of the source CFG block.
       */
    }
  }
  if (err && err_len)
    err[0] = '\0';
  return true;
}

static nyr_mach_operand_t mach_vreg(const nyr_mach_func_t *func,
                                    nyr_mach_reg_t reg) {
  return (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_VREG,
                              .reg_class = func->vreg_classes[reg],
                              .as.reg = reg};
}

static nyr_mach_operand_t mach_frame(uint32_t index) {
  return (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_FRAME,
                              .as.frame_index = index};
}

typedef struct {
  int64_t label;
  uint32_t block;
} mach_label_block_t;

static int mach_label_block_cmp(const void *left, const void *right) {
  const mach_label_block_t *a = left;
  const mach_label_block_t *b = right;
  return a->label < b->label ? -1 : a->label > b->label;
}

static int mach_find_nir_block(const mach_label_block_t *labels, size_t len,
                               int64_t label) {
  size_t low = 0;
  size_t high = len;
  while (low < high) {
    size_t mid = low + (high - low) / 2;
    if (labels[mid].label == label)
      return (int)labels[mid].block;
    if (labels[mid].label < label)
      low = mid + 1;
    else
      high = mid;
  }
  return -1;
}

typedef struct {
  uint32_t pred;
  uint32_t succ;
  uint32_t block;
  nyr_mach_reg_t *dst;
  nyr_mach_reg_t *src;
  size_t len;
  bool explicit_branch;
  bool inline_pred;
} mach_phi_edge_t;

static void mach_phi_edges_free(mach_phi_edge_t *edges, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    free(edges[i].dst);
    free(edges[i].src);
  }
  free(edges);
}

static mach_phi_edge_t *mach_phi_edge_find(mach_phi_edge_t *edges, size_t len,
                                           uint32_t pred, uint32_t succ) {
  for (size_t i = 0; i < len; ++i)
    if (edges[i].pred == pred && edges[i].succ == succ)
      return &edges[i];
  return NULL;
}

static bool mach_emit_parallel_copies(nyr_mach_func_t *out,
                                      const mach_phi_edge_t *edge) {
  if (!edge || edge->len == 0)
    return true;
  nyr_mach_reg_t *dst = ny_malloc_array(edge->len, sizeof(*dst));
  nyr_mach_reg_t *src = ny_malloc_array(edge->len, sizeof(*src));
  bool *pending = ny_calloc_array(edge->len, sizeof(*pending));
  if (!dst || !src || !pending) {
    free(dst);
    free(src);
    free(pending);
    return false;
  }
  memcpy(dst, edge->dst, edge->len * sizeof(*dst));
  memcpy(src, edge->src, edge->len * sizeof(*src));
  size_t left = 0;
  /*
   * Preserve an identity PHI copy. It encodes as a no-op once source and
   * destination receive the same preg, but it is the explicit entry-edge
   * seed required by liveness and loop-carried register allocation.
   */
  for (size_t i = 0; i < edge->len; ++i) {
    pending[i] = true;
    ++left;
  }
  while (left) {
    bool progressed = false;
    for (size_t i = 0; i < edge->len; ++i) {
      if (!pending[i])
        continue;
      bool dst_is_source = false;
      for (size_t j = 0; j < edge->len; ++j) {
        if (i != j && pending[j] && src[j] == dst[i]) {
          dst_is_source = true;
          break;
        }
      }
      if (dst_is_source)
        continue;
      nyr_mach_type_t dt = out->vreg_types[dst[i]];
      nyr_mach_type_t st = out->vreg_types[src[i]];
      bool bitwise64 = (dt == NYR_MACH_TYPE_I64 || dt == NYR_MACH_TYPE_PTR ||
                        dt == NYR_MACH_TYPE_F64) &&
                       (st == NYR_MACH_TYPE_I64 || st == NYR_MACH_TYPE_PTR ||
                        st == NYR_MACH_TYPE_F64);
      nyr_mach_inst_t copy = {
          .opcode = (dt == st || bitwise64) ? NYR_MACH_COPY : NYR_MACH_CONVERT,
          .source_pc = UINT32_MAX,
          .dst = mach_vreg(out, dst[i]),
          .src0 = mach_vreg(out, src[i])};
      if (!nyr_mach_emit(out, copy)) {
        free(dst);
        free(src);
        free(pending);
        return false;
      }
      pending[i] = false;
      --left;
      progressed = true;
    }
    if (progressed)
      continue;

    size_t cycle = 0;
    while (cycle < edge->len && !pending[cycle])
      ++cycle;
    if (cycle == edge->len)
      break;
    nyr_mach_reg_t old_src = src[cycle];
    nyr_mach_reg_t temp = NYR_MACH_REG_INVALID;
    if (old_src >= out->vreg_len ||
        !nyr_mach_alloc_typed_vreg(out, out->vreg_types[old_src], &temp)) {
      free(dst);
      free(src);
      free(pending);
      return false;
    }
    nyr_mach_inst_t save = {.opcode = NYR_MACH_COPY,
                            .source_pc = UINT32_MAX,
                            .dst = mach_vreg(out, temp),
                            .src0 = mach_vreg(out, old_src)};
    if (!nyr_mach_emit(out, save)) {
      free(dst);
      free(src);
      free(pending);
      return false;
    }
    for (size_t i = 0; i < edge->len; ++i)
      if (pending[i] && src[i] == old_src)
        src[i] = temp;
  }
  free(dst);
  free(src);
  free(pending);
  return true;
}

static bool mach_copy_metadata(const nyr_mach_func_t *src,
                               nyr_mach_func_t *dst) {
  dst->param_count = src->param_count;
  dst->return_aggregate_size = src->return_aggregate_size;
  dst->return_aggregate_classes[0] = src->return_aggregate_classes[0];
  dst->return_aggregate_classes[1] = src->return_aggregate_classes[1];
  if (src->param_types) {
    dst->param_types =
        ny_malloc_array(src->param_count, sizeof(*dst->param_types));
    dst->param_abi = ny_malloc_array(src->param_count, sizeof(*dst->param_abi));
    dst->param_aggregate_home_slots = ny_malloc_array(
        src->param_count, sizeof(*dst->param_aggregate_home_slots));
    if (!dst->param_types || !dst->param_abi ||
        !dst->param_aggregate_home_slots)
      return false;
    memcpy(dst->param_types, src->param_types,
           src->param_count * sizeof(*dst->param_types));
    memcpy(dst->param_abi, src->param_abi,
           src->param_count * sizeof(*dst->param_abi));
    memcpy(dst->param_aggregate_home_slots, src->param_aggregate_home_slots,
           src->param_count * sizeof(*dst->param_aggregate_home_slots));
  }
  if (src->frame_slot_len) {
    dst->frame_slots =
        ny_malloc_array(src->frame_slot_len, sizeof(*dst->frame_slots));
    if (!dst->frame_slots)
      return false;
    memcpy(dst->frame_slots, src->frame_slots,
           src->frame_slot_len * sizeof(*dst->frame_slots));
    dst->frame_slot_len = dst->frame_slot_cap = src->frame_slot_len;
  }
  if (src->vreg_len) {
    dst->vreg_classes =
        ny_malloc_array(src->vreg_len, sizeof(*dst->vreg_classes));
    dst->vreg_types = ny_malloc_array(src->vreg_len, sizeof(*dst->vreg_types));
    if (!dst->vreg_classes || !dst->vreg_types)
      return false;
    memcpy(dst->vreg_classes, src->vreg_classes,
           src->vreg_len * sizeof(*dst->vreg_classes));
    memcpy(dst->vreg_types, src->vreg_types,
           src->vreg_len * sizeof(*dst->vreg_types));
    dst->vreg_len = dst->vreg_cap = src->vreg_len;
  }
  return true;
}

/*
 * The PHI group at the head of a block. Passes may leave NOPs between PHIs
 * (DCE removing a dead PHI in place), so the group extends over NOPs up to
 * the first other instruction. With `out`, also records the PHI positions.
 */
static size_t mach_block_phis(const nyr_func_t *nyir, const nyr_cfg_t *cfg,
                              size_t block, size_t *out) {
  size_t at = cfg->block_start[block];
  if (at < cfg->block_end[block] && nyir->data[at].op == NYIR_LABEL)
    ++at;
  size_t count = 0;
  for (; at < cfg->block_end[block]; ++at) {
    nyr_op_t op = nyir->data[at].op;
    if (op == NYIR_NOP)
      continue;
    if (op != NYIR_PHI)
      break;
    if (out)
      out[count] = at;
    ++count;
  }
  return count;
}

static bool mach_lower_phi_edges(const nyr_func_t *nyir, const nyr_cfg_t *cfg,
                                 const nyr_mach_reg_t *values,
                                 nyr_mach_func_t *base, char *err,
                                 size_t err_len) {
  size_t edge_count = 0;
  for (size_t succ = 0; succ < cfg->block_count; ++succ) {
    if (!mach_block_phis(nyir, cfg, succ, NULL))
      continue;
    edge_count += cfg->pred_offsets[succ + 1] - cfg->pred_offsets[succ];
  }
  if (!edge_count)
    return true;
  mach_phi_edge_t *edges = ny_calloc_array(edge_count, sizeof(*edges));
  if (!edges)
    return nyr_err(err, err_len, "machine lower: out of memory for PHI edges");

  size_t ei = 0;
  size_t *phi_at = ny_malloc_array(nyir->len ? nyir->len : 1, sizeof(*phi_at));
  if (!phi_at) {
    mach_phi_edges_free(edges, edge_count);
    return nyr_err(err, err_len, "machine lower: out of memory for PHI edges");
  }
  for (size_t succ = 0; succ < cfg->block_count; ++succ) {
    size_t phi_count = mach_block_phis(nyir, cfg, succ, phi_at);
    if (!phi_count)
      continue;
    for (size_t pi = cfg->pred_offsets[succ]; pi < cfg->pred_offsets[succ + 1];
         ++pi) {
      size_t pred = cfg->pred_blocks[pi];
      mach_phi_edge_t *edge = &edges[ei];
      edge->pred = (uint32_t)pred;
      edge->succ = (uint32_t)succ;
      edge->inline_pred =
          cfg->succ_offsets[pred + 1] - cfg->succ_offsets[pred] == 1;
      edge->block = UINT32_MAX;
      edge->len = phi_count;
      edge->dst = ny_malloc_array(phi_count, sizeof(*edge->dst));
      edge->src = ny_malloc_array(phi_count, sizeof(*edge->src));
      if (!edge->dst || !edge->src) {
        free(phi_at);
        mach_phi_edges_free(edges, edge_count);
        return nyr_err(err, err_len,
                       "machine lower: out of memory for PHI copies");
      }
      int64_t pred_label = cfg->block_label[pred];
      for (size_t k = 0; k < phi_count; ++k) {
        const nyr_inst_t *phi = &nyir->data[phi_at[k]];
        int incoming = -1;
        for (size_t n = 0; n < phi->phi_incoming_len; ++n) {
          if (phi->phi_incoming[n].predecessor_label == pred_label) {
            incoming = phi->phi_incoming[n].value;
            break;
          }
        }
        if (incoming < 0 || phi->dst < 0) {
          free(phi_at);
          mach_phi_edges_free(edges, edge_count);
          return nyr_err(err, err_len,
                         "machine lower: PHI incoming edge is missing");
        }
        edge->dst[k] = values[phi->dst];
        edge->src[k] = values[incoming];
      }
      ++ei;
    }
  }
  free(phi_at);
  uint32_t next_edge_block = (uint32_t)base->block_len;
  for (size_t e = 0; e < edge_count; ++e)
    if (!edges[e].inline_pred)
      edges[e].block = next_edge_block++;

  nyr_mach_func_t rebuilt = {0};
  if (!mach_copy_metadata(base, &rebuilt))
    goto oom;
  for (size_t b = 0; b < base->block_len; ++b) {
    uint32_t emitted = 0;
    if (!nyr_mach_begin_block(&rebuilt, base->blocks[b].label, &emitted) ||
        emitted != b)
      goto oom;
    rebuilt.blocks[emitted].source_pc = base->blocks[b].source_pc;
    const nyr_mach_block_t *block = &base->blocks[b];
    for (size_t n = 0; n < block->inst_count; ++n) {
      size_t index = block->first_inst + n;
      nyr_mach_inst_t inst = base->insts[index];
      if ((inst.opcode == NYR_MACH_BR || inst.opcode == NYR_MACH_BR_IF) &&
          inst.src1.kind == NYR_MACH_OPERAND_BLOCK) {
        uint32_t target = inst.src1.as.block_index;
        mach_phi_edge_t *edge =
            mach_phi_edge_find(edges, edge_count, (uint32_t)b, target);
        if (edge) {
          if (edge->inline_pred) {
            if (!mach_emit_parallel_copies(&rebuilt, edge))
              goto oom;
          } else {
            inst.src1.as.block_index = edge->block;
          }
          edge->explicit_branch = true;
        }
      }
      if (!nyr_mach_emit(&rebuilt, inst))
        goto oom;
      base->insts[index].args = NULL;
      base->insts[index].arg_sizes = NULL;
      base->insts[index].args_len = 0;
    }
    mach_phi_edge_t *implicit = NULL;
    for (size_t e = 0; e < edge_count; ++e) {
      if (edges[e].pred != b || edges[e].explicit_branch)
        continue;
      if (implicit) {
        nyr_err(err, err_len,
                "machine lower: multiple implicit PHI successors");
        goto fail;
      }
      implicit = &edges[e];
    }
    if (implicit) {
      if (!implicit->inline_pred) {
        nyr_mach_inst_t br = {.opcode = NYR_MACH_BR,
                              .source_pc = UINT32_MAX,
                              .src1 = {.kind = NYR_MACH_OPERAND_BLOCK,
                                       .as.block_index = implicit->block},
                              .effects = NYR_MACH_EFFECT_CONTROL};
        if (!nyr_mach_emit(&rebuilt, br))
          goto oom;
        continue;
      }
      if (!mach_emit_parallel_copies(&rebuilt, implicit))
        goto oom;
      nyr_mach_inst_t br = {.opcode = NYR_MACH_BR,
                            .source_pc = UINT32_MAX,
                            .src1 = {.kind = NYR_MACH_OPERAND_BLOCK,
                                     .as.block_index = implicit->succ},
                            .effects = NYR_MACH_EFFECT_CONTROL};
      if (!nyr_mach_emit(&rebuilt, br))
        goto oom;
    }
  }
  for (size_t e = 0; e < edge_count; ++e) {
    if (edges[e].inline_pred)
      continue;
    uint32_t emitted = 0;
    if (!nyr_mach_begin_block(&rebuilt, edges[e].block, &emitted) ||
        emitted != edges[e].block ||
        !mach_emit_parallel_copies(&rebuilt, &edges[e]))
      goto oom;
    nyr_mach_inst_t br = {.opcode = NYR_MACH_BR,
                          .source_pc = UINT32_MAX,
                          .src1 = {.kind = NYR_MACH_OPERAND_BLOCK,
                                   .as.block_index = edges[e].succ},
                          .effects = NYR_MACH_EFFECT_CONTROL};
    if (!nyr_mach_emit(&rebuilt, br))
      goto oom;
  }
  mach_phi_edges_free(edges, edge_count);
  nyr_mach_func_free(base);
  *base = rebuilt;
  return true;

oom:
  nyr_err(err, err_len, "machine lower: out of memory lowering PHI edges");
fail:
  mach_phi_edges_free(edges, edge_count);
  nyr_mach_func_free(&rebuilt);
  return false;
}

static bool mach_opcode_from_nir(nyr_op_t op, nyr_mach_opcode_t *out) {
  if (!out)
    return false;
  switch (op) {
  case NYIR_COPY:
    *out = NYR_MACH_COPY;
    return true;
  case NYIR_ADD_I64:
    *out = NYR_MACH_ADD;
    return true;
  case NYIR_SUB_I64:
    *out = NYR_MACH_SUB;
    return true;
  case NYIR_MUL_I64:
    *out = NYR_MACH_MUL;
    return true;
  case NYIR_DIV_I64:
    *out = NYR_MACH_DIV;
    return true;
  case NYIR_MOD_I64:
    *out = NYR_MACH_MOD;
    return true;
  case NYIR_AND_I64:
    *out = NYR_MACH_AND;
    return true;
  case NYIR_OR_I64:
    *out = NYR_MACH_OR;
    return true;
  case NYIR_XOR_I64:
    *out = NYR_MACH_XOR;
    return true;
  case NYIR_SHL_I64:
    *out = NYR_MACH_SHL;
    return true;
  case NYIR_SAR_I64:
    *out = NYR_MACH_SAR;
    return true;
  case NYIR_ROR_I64:
    *out = NYR_MACH_ROR;
    return true;
  case NYIR_ROR32_I64:
    *out = NYR_MACH_ROR32;
    return true;
  case NYIR_BSWAP_I64:
    *out = NYR_MACH_BSWAP64;
    return true;
  case NYIR_BSWAP32_I64:
    *out = NYR_MACH_BSWAP32;
    return true;
  case NYIR_CMP_I64:
    *out = NYR_MACH_CMP;
    return true;
  case NYIR_SELECT_I64:
    *out = NYR_MACH_SELECT;
    return true;
  case NYIR_VA_SAVE:
  case NYIR_VA_START:
  case NYIR_VA_ARG:
    return false;
  case NYIR_I64_TO_F64:
  case NYIR_I64_TO_F32:
  case NYIR_F64_TO_I64:
  case NYIR_F32_TO_I64:
  case NYIR_F64_TO_F32:
  case NYIR_F32_TO_F64:
    *out = NYR_MACH_CONVERT;
    return true;
  case NYIR_SQRT_F64:
    *out = NYR_MACH_SQRT;
    return true;
  case NYIR_SIN_F64:
    *out = NYR_MACH_SIN;
    return true;
  case NYIR_COS_F64:
    *out = NYR_MACH_COS;
    return true;
  case NYIR_ADD_F64:
  case NYIR_ADD_F32:
    *out = NYR_MACH_ADD;
    return true;
  case NYIR_SUB_F64:
  case NYIR_SUB_F32:
    *out = NYR_MACH_SUB;
    return true;
  case NYIR_MUL_F64:
  case NYIR_MUL_F32:
    *out = NYR_MACH_MUL;
    return true;
  case NYIR_DIV_F64:
  case NYIR_DIV_F32:
    *out = NYR_MACH_DIV;
    return true;
  case NYIR_CMP_F64:
  case NYIR_CMP_F32:
    *out = NYR_MACH_CMP;
    return true;
  default:
    return false;
  }
}

static bool mach_condition_from_nir(int64_t condition, nyr_mach_cond_t *out) {
  if (!out || condition < NYIR_CMP_EQ || condition > NYIR_CMP_GE)
    return false;
  switch ((nyr_cmp_t)condition) {
  case NYIR_CMP_EQ:
    *out = NYR_MACH_COND_EQ;
    return true;
  case NYIR_CMP_NE:
    *out = NYR_MACH_COND_NE;
    return true;
  case NYIR_CMP_LT:
    *out = NYR_MACH_COND_LT;
    return true;
  case NYIR_CMP_LE:
    *out = NYR_MACH_COND_LE;
    return true;
  case NYIR_CMP_GT:
    *out = NYR_MACH_COND_GT;
    return true;
  case NYIR_CMP_GE:
    *out = NYR_MACH_COND_GE;
    return true;
  }
  return false;
}

static bool mach_nir_value_is_cmp_result(const nyr_func_t *nyir,
                                         const int *definitions, int value,
                                         unsigned depth) {
  if (!nyir || !definitions || value < 0 || value >= nyir->next_value ||
      depth > 16)
    return false;
  int di = definitions[value];
  if (di < 0 || (size_t)di >= nyir->len)
    return false;
  const nyr_inst_t *in = &nyir->data[(size_t)di];
  if (in->op == NYIR_CMP_I64 || in->op == NYIR_CMP_F64 ||
      in->op == NYIR_CMP_F32)
    return true;
  if (in->op == NYIR_COPY)
    return mach_nir_value_is_cmp_result(nyir, definitions, in->a, depth + 1);
  return false;
}

static nyr_mach_type_t mach_nir_value_type(const nyr_type_map_t *types,
                                           const bool *pointer_values,
                                           const int *definitions, int value,
                                           const nyr_func_t *nyir) {
  if (pointer_values && value >= 0 && pointer_values[value])
    return NYR_MACH_TYPE_PTR;
  if (types && value >= 0 && (size_t)value < types->value_count) {
    if (mach_nir_value_is_cmp_result(nyir, definitions, value, 0))
      return NYR_MACH_TYPE_I64;
    if (types->value_v256_f64 && types->value_v256_f64[value])
      return NYR_MACH_TYPE_V256_F64;
    if (types->value_v128_f64 && types->value_v128_f64[value])
      return NYR_MACH_TYPE_V128_F64;
    if (types->value_v128_f32 && types->value_v128_f32[value])
      return NYR_MACH_TYPE_V128_F32;
    if (types->value_v256_i64 && types->value_v256_i64[value])
      return NYR_MACH_TYPE_V256_I64;
    if (types->value_v128_i64 && types->value_v128_i64[value])
      return NYR_MACH_TYPE_V128_I64;
    if (types->value_f64[value])
      return NYR_MACH_TYPE_F64;
    if (types->value_f32[value])
      return NYR_MACH_TYPE_F32;
  }
  return NYR_MACH_TYPE_I64;
}

bool nyr_mach_opcode_supported(nyr_op_t op) {
  switch (op) {
  case NYIR_READ_X86_RSP:
  case NYIR_STACK_GUARD_LOAD:
  case NYIR_STACK_GUARD_STORE:
    return false;
  case NYIR_NOP:
  case NYIR_BOUNDS_CHECK:
  case NYIR_CONST_I64:
  case NYIR_COPY:
  case NYIR_PHI:
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
  case NYIR_LABEL:
  case NYIR_LOAD_LOCAL:
  case NYIR_STORE_LOCAL:
  case NYIR_CALL:
  case NYIR_INLINE_ASM:
  case NYIR_RET:
  case NYIR_BR:
  case NYIR_BR_IF:
  case NYIR_CONST_F64:
  case NYIR_ADD_F64:
  case NYIR_SUB_F64:
  case NYIR_MUL_F64:
  case NYIR_DIV_F64:
  case NYIR_SQRT_F64:
  case NYIR_SIN_F64:
  case NYIR_COS_F64:
  case NYIR_I64_TO_F64:
  case NYIR_F64_TO_I64:
  case NYIR_CMP_F64:
  case NYIR_CONST_F32:
  case NYIR_ADD_F32:
  case NYIR_SUB_F32:
  case NYIR_MUL_F32:
  case NYIR_DIV_F32:
  case NYIR_F32_TO_I64:
  case NYIR_I64_TO_F32:
  case NYIR_F64_TO_F32:
  case NYIR_F32_TO_F64:
  case NYIR_CMP_F32:
  case NYIR_ADDR_LOCAL:
  case NYIR_LOAD_I64:
  case NYIR_STORE_I64:
  case NYIR_ADDR_SYMBOL:
  case NYIR_ALLOCA:
  case NYIR_COPY_STRUCT:
  case NYIR_CAPTURE_RET:
  case NYIR_VEC4_LOAD_F64:
  case NYIR_VEC4_STORE_F64:
  case NYIR_VEC4_ADD_F64:
  case NYIR_VEC4_SUB_F64:
  case NYIR_VEC4_MUL_F64:
  case NYIR_VEC4_DIV_F64:
  case NYIR_VEC4_FMA_F64:
  case NYIR_VEC4_SET1_F64:
  case NYIR_VEC4_SHUFFLE_F64:
  case NYIR_VEC4_REDUCE_ADD_F64:
  case NYIR_VEC8_LOAD_F32:
  case NYIR_VEC8_STORE_F32:
  case NYIR_VEC8_ADD_F32:
  case NYIR_VEC8_SUB_F32:
  case NYIR_VEC8_MUL_F32:
  case NYIR_VEC8_DIV_F32:
  case NYIR_VEC8_FMA_F32:
  case NYIR_VEC8_SET1_F32:
  case NYIR_VEC8_SHUFFLE_F32:
  case NYIR_VEC4_LOAD_I64:
  case NYIR_VEC4_SET1_I64:
  case NYIR_VEC8_SAR_I64:
  case NYIR_VEC8_SHR_I64:
  case NYIR_VEC8_SHL_I64:
  case NYIR_VEC8_CMP_I64:
  case NYIR_VEC8_SELECT_I64:
  case NYIR_VEC8_SET1_I64:
  case NYIR_VEC4_STORE_I64:
  case NYIR_VEC4_ADD_I64:
  case NYIR_VEC4_SUB_I64:
  case NYIR_VEC4_AND_I64:
  case NYIR_VEC4_OR_I64:
  case NYIR_VEC4_XOR_I64:
  case NYIR_VEC4_SHL_I64:
  case NYIR_VEC4_SAR_I64:
  case NYIR_VEC8_LOAD_I64:
  case NYIR_VEC8_STORE_I64:
  case NYIR_VEC8_ADD_I64:
  case NYIR_VEC8_SUB_I64:
  case NYIR_VEC8_AND_I64:
  case NYIR_VEC8_OR_I64:
  case NYIR_VEC8_XOR_I64:
  case NYIR_VEC8_MUL_I64:
  case NYIR_VEC4_REDUCE_ADD_I64:
  case NYIR_VEC8_REDUCE_ADD_I64:
  case NYIR_VA_SAVE:
  case NYIR_VA_START:
  case NYIR_VA_ARG:
    return true;
  case NYIR_OP_COUNT:
    return false;
  }
  return false;
}

void nyr_mach_opcode_coverage(size_t *supported, size_t *total) {
  size_t n = 0;
  for (int op = 0; op < NYIR_OP_COUNT; ++op)
    n += nyr_mach_opcode_supported((nyr_op_t)op) ? 1u : 0u;
  if (supported)
    *supported = n;
  if (total)
    *total = NYIR_OP_COUNT;
}

static uint64_t mach_lower_trace_fingerprint(const nyr_func_t *nyir) {
  uint64_t hash = UINT64_C(1469598103934665603);
  for (size_t i = 0; i < nyir->len; ++i) {
    const nyr_inst_t *in = &nyir->data[i];
    const int fields[] = {(int)in->op, in->dst, in->a, in->b, in->c};
    for (size_t j = 0; j < sizeof(fields) / sizeof(fields[0]); ++j) {
      hash ^= (uint32_t)fields[j];
      hash *= UINT64_C(1099511628211);
    }
    hash ^= (uint64_t)in->imm;
    hash *= UINT64_C(1099511628211);
    if (in->symbol) {
      for (const unsigned char *p = (const unsigned char *)in->symbol; *p;
           ++p) {
        hash ^= *p;
        hash *= UINT64_C(1099511628211);
      }
    }
  }
  return hash;
}

static bool mach_nir_const_i64(const nyr_func_t *f, const int *definitions,
                               int value, int64_t *out) {
  for (unsigned depth = 0; depth < 32; ++depth) {
    if (!f || !definitions || value < 0 || value >= f->next_value)
      return false;
    int def = definitions[value];
    if (def < 0 || (size_t)def >= f->len)
      return false;
    const nyr_inst_t *in = &f->data[def];
    if (in->op == NYIR_CONST_I64) {
      *out = in->imm;
      return true;
    }
    if (in->op != NYIR_COPY)
      return false;
    value = in->a;
  }
  return false;
}

/*
 * memcpy(dst, src, n) with a constant byte count is a fixed-size copy. Every
 * backend already expands a constant COPY_STRUCT into straight-line moves,
 * whereas a call costs a PLT jump and forces the loop values that stay live
 * across it through their home slots. Constant propagation usually leaves the
 * count as a plain CONST_I64, so decide here instead of only in a front end.
 * Only the statement form is converted: a used result stays a call.
 */
static bool mach_small_memcpy_size(const nyr_func_t *f, const int *definitions,
                                   const nyr_inst_t *call, int64_t *size) {
  if (!f || !call || !size || call->op != NYIR_CALL || !call->symbol ||
      (call->flags & (NYIR_INST_F_INDIRECT | NYIR_INST_F_SRET)) != 0 ||
      call->imm != 3 || strcmp(call->symbol, "memcpy") != 0)
    return false;
  int args[NYIR_CALL_MAX_ARGS] = {0};
  int arg_count = 0;
  char args_err[128] = {0};
  if (!nyr_call_args(call, f->next_value, args, 3, &arg_count, args_err,
                     sizeof(args_err)) ||
      arg_count != 3)
    return false;
  int64_t count = 0;
  if (!mach_nir_const_i64(f, definitions, args[2], &count))
    return false;
  if (count <= 0 || count > 64)
    return false;
  *size = count;
  return true;
}

static int mach_nir_resolve_copy(const nyr_func_t *f, const int *definitions,
                                 int value) {
  for (unsigned depth = 0; depth < 32; ++depth) {
    if (!f || !definitions || value < 0 || value >= f->next_value)
      return -1;
    int def = definitions[value];
    if (def < 0 || (size_t)def >= f->len || f->data[def].op != NYIR_COPY)
      return value;
    value = f->data[def].a;
  }
  return -1;
}

static bool mach_nir_single_user(const nyr_use_def_t *uses, int value,
                                 size_t user) {
  if (!uses || value < 0 || (size_t)value >= uses->value_count)
    return false;
  size_t begin = uses->offsets[value];
  size_t end = uses->offsets[value + 1];
  return end == begin + 1 && uses->users[begin] == user;
}

static bool mach_nir_match_scaled_add(const nyr_func_t *f,
                                      const int *definitions,
                                      const nyr_cfg_t *cfg, size_t add_pc,
                                      int *base_out, int *index_out,
                                      size_t *scale_pc_out,
                                      unsigned *scale_out) {
  if (!f || !definitions || !cfg || add_pc >= f->len ||
      f->data[add_pc].op != NYIR_ADD_I64 ||
      (f->data[add_pc].flags & NYIR_INST_F_NARROW32))
    return false;
  const nyr_inst_t *add = &f->data[add_pc];
  for (unsigned side = 0; side < 2; ++side) {
    int scaled = side ? add->b : add->a;
    int base = side ? add->a : add->b;
    if (base == scaled)
      continue;
    int scaled_pc =
        scaled >= 0 && scaled < f->next_value ? definitions[scaled] : -1;
    int64_t amount = 0;
    if (scaled_pc < 0 || (size_t)scaled_pc >= add_pc ||
        (size_t)scaled_pc >= f->len ||
        (f->data[scaled_pc].flags & NYIR_INST_F_NARROW32))
      continue;
    /*
     * Cross-block reuse is legal only when the scale definition dominates
     * every folded add. A textual order check alone is not enough after CFG
     * cloning or block layout changes.
     */
    size_t scaled_block = cfg->inst_block[scaled_pc];
    size_t add_block = cfg->inst_block[add_pc];
    if (scaled_block >= cfg->block_count || add_block >= cfg->block_count ||
        (scaled_block == add_block
             ? (size_t)scaled_pc >= add_pc
             : !nyr_cfg_dominates(cfg, scaled_block, add_block)))
      continue;
    int index_value = -1;
    unsigned scale = 0;
    const nyr_inst_t *scale_in = &f->data[scaled_pc];
    if (scale_in->op == NYIR_SHL_I64 &&
        mach_nir_const_i64(f, definitions, scale_in->b, &amount) &&
        amount >= 1 && amount <= 3) {
      index_value = scale_in->a;
      scale = 1u << (unsigned)amount;
    } else if (scale_in->op == NYIR_MUL_I64) {
      int64_t factor = 0;
      if (mach_nir_const_i64(f, definitions, scale_in->a, &factor) &&
          (factor == 2 || factor == 4 || factor == 8))
        index_value = scale_in->b;
      else if (mach_nir_const_i64(f, definitions, scale_in->b, &factor) &&
               (factor == 2 || factor == 4 || factor == 8))
        index_value = scale_in->a;
      if (factor == 2 || factor == 4 || factor == 8)
        scale = (unsigned)factor;
      else
        index_value = -1;
    }
    if (index_value < 0 || scale == 0)
      continue;
    *base_out = base;
    *index_out = index_value;
    *scale_pc_out = (size_t)scaled_pc;
    *scale_out = scale;
    return true;
  }
  return false;
}

static bool mach_nir_scaled_value_only_feeds_fused_adds(
    const nyr_func_t *f, const int *definitions, const nyr_use_def_t *uses,
    const nyr_cfg_t *cfg, const int *fused_lea_base, size_t scale_pc) {
  if (!f || !definitions || !uses || !cfg || !fused_lea_base ||
      scale_pc >= f->len)
    return false;
  int scaled = f->data[scale_pc].dst;
  if (scaled < 0 || (size_t)scaled >= uses->value_count)
    return false;
  size_t begin = uses->offsets[scaled];
  size_t end = uses->offsets[scaled + 1];
  if (begin == end)
    return false;
  /*
   * Keep the scaled value if even one consumer cannot use its own LEA. This
   * also covers repeated operands in one instruction, represented as repeated
   * use-def entries.
   */
  for (size_t i = begin; i < end; ++i) {
    size_t user = uses->users[i];
    int base = -1, index = -1;
    size_t user_scale_pc = SIZE_MAX;
    unsigned scale = 0;
    if (user >= f->len || fused_lea_base[user] < 0 ||
        !mach_nir_match_scaled_add(f, definitions, cfg, user, &base, &index,
                                   &user_scale_pc, &scale) ||
        user_scale_pc != scale_pc)
      return false;
  }
  return true;
}

/*
 * Recover the exact truncating signed divide sequence made by
 * strength_reduce.c:
 *   (x + ((x >> 63) & (2^k - 1))) >> k
 * Fuse it only when all three intermediate results have a single in-block
 * consumer. The x86 machine DIV lowering has a one-instruction MIR form for
 * this operation, avoiding stack homes for the sign and biased temporaries.
 */
static bool
mach_nir_match_signed_pow2_div(const nyr_func_t *f, const int *definitions,
                               const nyr_use_def_t *uses, const nyr_cfg_t *cfg,
                               size_t final_pc, int *dividend_out,
                               int64_t *divisor_out, size_t skipped[3]) {
  if (!f || !definitions || !cfg || final_pc >= f->len)
    return false;
  const nyr_inst_t *final = &f->data[final_pc];
  int64_t shift = 0;
  if (final->op != NYIR_SAR_I64 ||
      !mach_nir_const_i64(f, definitions, final->b, &shift) || shift < 1 ||
      shift >= 31)
    return false;
  int add_pc = definitions[final->a];
  if (add_pc < 0 || (size_t)add_pc >= final_pc ||
      f->data[add_pc].op != NYIR_ADD_I64 ||
      cfg->inst_block[add_pc] != cfg->inst_block[final_pc])
    return false;
  const nyr_inst_t *add = &f->data[add_pc];
  int and_pc = -1, x = -1, sign_pc = -1;
  for (unsigned side = 0; side < 2; ++side) {
    int candidate = side == 0 ? add->a : add->b;
    int other = side == 0 ? add->b : add->a;
    int candidate_pc = candidate >= 0 && candidate < f->next_value
                           ? definitions[candidate]
                           : -1;
    if (candidate_pc < 0 || (size_t)candidate_pc >= (size_t)add_pc ||
        f->data[candidate_pc].op != NYIR_AND_I64 ||
        cfg->inst_block[candidate_pc] != cfg->inst_block[final_pc])
      continue;
    const nyr_inst_t *and_in = &f->data[candidate_pc];
    for (unsigned bit_side = 0; bit_side < 2; ++bit_side) {
      int sign_value = bit_side == 0 ? and_in->a : and_in->b;
      int mask_value = bit_side == 0 ? and_in->b : and_in->a;
      int candidate_sign_pc = sign_value >= 0 && sign_value < f->next_value
                                  ? definitions[sign_value]
                                  : -1;
      int64_t mask = 0, sign_shift = 0;
      int sign_base = candidate_sign_pc >= 0
                          ? mach_nir_resolve_copy(f, definitions,
                                                  f->data[candidate_sign_pc].a)
                          : -1;
      int add_base = mach_nir_resolve_copy(f, definitions, other);
      if (candidate_sign_pc < 0 ||
          (size_t)candidate_sign_pc >= (size_t)candidate_pc ||
          f->data[candidate_sign_pc].op != NYIR_SAR_I64 ||
          cfg->inst_block[candidate_sign_pc] != cfg->inst_block[final_pc] ||
          !mach_nir_const_i64(f, definitions, mask_value, &mask) ||
          !mach_nir_const_i64(f, definitions, f->data[candidate_sign_pc].b,
                              &sign_shift) ||
          sign_shift != 63 || sign_base < 0 || add_base < 0 ||
          sign_base != add_base || mask != ((INT64_C(1) << shift) - 1))
        continue;
      and_pc = candidate_pc;
      sign_pc = candidate_sign_pc;
      x = other;
      break;
    }
    if (and_pc >= 0)
      break;
  }
  if (and_pc < 0 || sign_pc < 0 ||
      (uses &&
       (!mach_nir_single_user(uses, f->data[sign_pc].dst, (size_t)and_pc) ||
        !mach_nir_single_user(uses, f->data[and_pc].dst, (size_t)add_pc) ||
        !mach_nir_single_user(uses, add->dst, final_pc))))
    return false;
  skipped[0] = (size_t)sign_pc;
  skipped[1] = (size_t)and_pc;
  skipped[2] = (size_t)add_pc;
  *dividend_out = x;
  *divisor_out = INT64_C(1) << shift;
  return true;
}

static int mach_swar_def(const nyr_func_t *f, const int *definitions, int value,
                         nyr_op_t op) {
  if (!f || !definitions || value < 0 || value >= f->next_value)
    return -1;
  int pc = definitions[value];
  if (pc < 0 || (size_t)pc >= f->len)
    return -1;
  return f->data[pc].op == op ? pc : -1;
}

/*
 * Recognize the 32-bit SWAR population count:
 *
 *   v = x & 0xffffffff
 *   v = v - ((v >> 1) & 0x55555555)
 *   v = (v & 0x33333333) + ((v >> 2) & 0x33333333)
 *   v = (v + (v >> 4)) & 0x0f0f0f0f
 *   n = ((v * 0x01010101) & 0xffffffff) >> 24
 *
 * The input already carries the 32-bit mask, so a 64-bit POPCNT is exact.
 * Every removed value must be consumed only inside the matched chain, and
 * each value keeps a single definition.
 */
static bool mach_nir_match_swar_popcount(const nyr_func_t *f,
                                         const int *definitions,
                                         const nyr_use_def_t *uses,
                                         const nyr_cfg_t *cfg, size_t final_pc,
                                         int *input_out, size_t skipped[16],
                                         size_t *skipped_len) {
  if (!f || !definitions || !cfg || !input_out || !skipped_len ||
      final_pc >= f->len)
    return false;
  const nyr_inst_t *fin = &f->data[final_pc];
  int64_t shift = 0;
  if (fin->op != NYIR_SAR_I64 ||
      !mach_nir_const_i64(f, definitions, fin->b, &shift) || shift != 24)
    return false;
  size_t block = cfg->inst_block[final_pc];
  size_t n = 0;
  int64_t mask = 0;
  int v = fin->a;
  int pc = mach_swar_def(f, definitions, v, NYIR_AND_I64);
  if (pc < 0 || !mach_nir_const_i64(f, definitions, f->data[pc].b, &mask) ||
      mask != INT64_C(0xffffffff))
    return false;
  skipped[n++] = (size_t)pc;
  v = f->data[pc].a;
  pc = mach_swar_def(f, definitions, v, NYIR_MUL_I64);
  if (pc < 0)
    return false;
  {
    const nyr_inst_t *mul = &f->data[pc];
    int64_t k = 0;
    if (!mach_nir_const_i64(f, definitions, mul->b, &k) ||
        k != INT64_C(0x01010101))
      return false;
    v = mul->a;
  }
  skipped[n++] = (size_t)pc;
  pc = mach_swar_def(f, definitions, v, NYIR_AND_I64);
  if (pc < 0)
    return false;
  {
    const nyr_inst_t *and4 = &f->data[pc];
    int64_t k = 0;
    if (!mach_nir_const_i64(f, definitions, and4->b, &k) ||
        k != INT64_C(0x0f0f0f0f))
      return false;
    v = and4->a;
  }
  skipped[n++] = (size_t)pc;
  pc = mach_swar_def(f, definitions, v, NYIR_ADD_I64);
  if (pc < 0)
    return false;
  {
    const nyr_inst_t *add2 = &f->data[pc];
    int base = -1;
    for (int side = 0; side < 2; ++side) {
      int s = side == 0 ? add2->a : add2->b;
      int other = side == 0 ? add2->b : add2->a;
      int spc = mach_swar_def(f, definitions, s, NYIR_SAR_I64);
      int64_t sh = 0;
      if (spc >= 0 && mach_nir_const_i64(f, definitions, f->data[spc].b, &sh) &&
          sh == 4 && f->data[spc].a == other) {
        base = other;
        skipped[n++] = (size_t)spc;
        break;
      }
    }
    if (base < 0)
      return false;
    v = base;
  }
  skipped[n++] = (size_t)pc;
  pc = mach_swar_def(f, definitions, v, NYIR_ADD_I64);
  if (pc < 0)
    return false;
  {
    const nyr_inst_t *add1 = &f->data[pc];
    int subv = -1;
    for (int side = 0; side < 2; ++side) {
      int a = side == 0 ? add1->a : add1->b;
      int b = side == 0 ? add1->b : add1->a;
      int apc = mach_swar_def(f, definitions, a, NYIR_AND_I64);
      int bpc = mach_swar_def(f, definitions, b, NYIR_AND_I64);
      int64_t ka = 0, kb = 0;
      int64_t sh = 0;
      int bsh = -1;
      if (apc < 0 || bpc < 0)
        continue;
      if (!mach_nir_const_i64(f, definitions, f->data[apc].b, &ka) ||
          ka != INT64_C(0x33333333))
        continue;
      if (!mach_nir_const_i64(f, definitions, f->data[bpc].b, &kb) ||
          kb != INT64_C(0x33333333))
        continue;
      bsh = mach_swar_def(f, definitions, f->data[bpc].a, NYIR_SAR_I64);
      if (bsh < 0 || !mach_nir_const_i64(f, definitions, f->data[bsh].b, &sh) ||
          sh != 2 || f->data[bsh].a != f->data[apc].a)
        continue;
      subv = f->data[apc].a;
      skipped[n++] = (size_t)apc;
      skipped[n++] = (size_t)bpc;
      skipped[n++] = (size_t)bsh;
      break;
    }
    if (subv < 0)
      return false;
    v = subv;
  }
  skipped[n++] = (size_t)pc;
  /*
   * The C frontend may explicitly truncate the unsigned subtraction.
   * Its right operand is at most v >> 1, so the subtraction already fits
   * in 32 bits once the input mask has been verified below.
   */
  pc = mach_swar_def(f, definitions, v, NYIR_AND_I64);
  if (pc >= 0 && mach_nir_const_i64(f, definitions, f->data[pc].b, &mask) &&
      mask == INT64_C(0xffffffff)) {
    skipped[n++] = (size_t)pc;
    v = f->data[pc].a;
  }
  pc = mach_swar_def(f, definitions, v, NYIR_SUB_I64);
  if (pc < 0)
    return false;
  {
    const nyr_inst_t *sub = &f->data[pc];
    int andpc = mach_swar_def(f, definitions, sub->b, NYIR_AND_I64);
    int64_t k = 0;
    int64_t sh = 0;
    int sarpc = -1;
    if (andpc < 0 ||
        !mach_nir_const_i64(f, definitions, f->data[andpc].b, &k) ||
        k != INT64_C(0x55555555))
      return false;
    sarpc = mach_swar_def(f, definitions, f->data[andpc].a, NYIR_SAR_I64);
    if (sarpc < 0 ||
        !mach_nir_const_i64(f, definitions, f->data[sarpc].b, &sh) || sh != 1 ||
        f->data[sarpc].a != sub->a)
      return false;
    skipped[n++] = (size_t)andpc;
    skipped[n++] = (size_t)sarpc;
    v = sub->a;
  }
  skipped[n++] = (size_t)pc;
  {
    int maskpc = mach_swar_def(f, definitions, v, NYIR_AND_I64);
    int64_t k = 0;
    if (maskpc < 0 ||
        !mach_nir_const_i64(f, definitions, f->data[maskpc].b, &k) ||
        k != INT64_C(0xffffffff))
      return false;
  }
  for (size_t i = 0; i < n; ++i) {
    size_t at = skipped[i];
    int dv = f->data[at].dst;
    if (cfg->inst_block[at] != block)
      return false;
    if (dv < 0 || dv >= f->next_value || definitions[dv] != (int)at)
      return false;
    if (uses) {
      if ((size_t)dv + 1 >= uses->value_count)
        return false;
      for (size_t u = uses->offsets[dv]; u < uses->offsets[dv + 1]; ++u) {
        size_t user = uses->users[u];
        bool inside = user == final_pc;
        for (size_t j = 0; j < n && !inside; ++j)
          if (skipped[j] == user)
            inside = true;
        if (!inside)
          return false;
      }
    }
  }
  *input_out = v;
  *skipped_len = n;
  return true;
}

bool nyr_mach_lower_nir(const nyr_func_t *nyir, nyr_mach_func_t *out,
                        unsigned caps, char *err, size_t err_len) {
  if (ny_trace_enabled("NY_TRACE_EMIT"))
    ny_trace_line("MACHINE", "lowering NYIR function (%zu instructions)",
                  nyir ? nyir->len : 0);
  if (!nyir || !out)
    return nyr_err(err, err_len, "machine lower: missing input or output");
  char verify_err[256] = {0};
  if (!nyr_ir_verify(nyir, verify_err, sizeof(verify_err)))
    return nyr_err(err, err_len, "machine lower: invalid NYIR: %s", verify_err);
  /*
   * Segment-relative accesses need a %fs:/%gs: prefix on the memory operand.
   * The machine form has no field for that, so let the NYIR emitter, which
   * does, take the function instead of emitting an unprefixed access.
   */
  for (size_t i = 0; i < nyir->len; ++i) {
    const nyr_inst_t *in = &nyir->data[i];
    if ((in->op == NYIR_LOAD_I64 || in->op == NYIR_STORE_I64) &&
        (in->flags & (NYIR_INST_F_MEM_SEG_FS | NYIR_INST_F_MEM_SEG_GS)))
      return nyr_err(err, err_len,
                     "machine lower: segment-relative access requires the "
                     "NYIR emitter");
  }
  bool trace_lowering =
      ny_dbg_is_enabled(NY_DBG_LOWER) || ny_dbg_is_enabled(NY_DBG_VALUE);
  if (ny_dbg_is_enabled(NY_DBG_LOWER) && trace_lowering &&
      ny_dbg_lower_sequence_duplicate(nyir->len,
                                      mach_lower_trace_fingerprint(nyir))) {
    NY_DBG(NY_DBG_LOWER, "identical lowering trace suppressed (set "
                         "NY_DEBUG_LOWER_DUPLICATES=1 to show it)");
    trace_lowering = false;
  }
  nyr_mach_func_t lowered = {
      .param_count = nyir->param_count,
      .return_aggregate_size = nyir->return_aggregate_size,
      .return_aggregate_classes = {nyir->return_aggregate_classes[0],
                                   nyir->return_aggregate_classes[1]}};
  nyr_mach_reg_t *values = NULL;
  int *definitions = NULL;
  int *fused_dividend = NULL;
  int64_t *fused_divisor = NULL;
  int *fused_lea_base = NULL;
  int *fused_lea_index = NULL;
  int *fused_popcount = NULL;
  unsigned *fused_lea_scale = NULL;
  bool *skip_fused_inst = NULL;
  nyr_use_def_t uses = {0};
  bool *pointer_values = NULL;
  mach_label_block_t *label_blocks = NULL;
  size_t label_block_len = 0;
  nyr_type_map_t types = {0};
  nyr_cfg_t cfg = {0};
  size_t local_count = nyir->param_count;
  bool *local_address_taken = NULL;
  bool has_bounds_checks = false;
  bool ok = false;
  bool has_aggregate_param = false;
  for (size_t i = 0; i < nyir->param_count; ++i)
    has_aggregate_param |= nyir->param_types[i] == NYIR_PARAM_AGGREGATE;
  if (nyir->param_count) {
    lowered.param_types =
        ny_malloc_array(nyir->param_count, sizeof(*lowered.param_types));
    lowered.param_abi =
        ny_malloc_array(nyir->param_count, sizeof(*lowered.param_abi));
    lowered.param_aggregate_home_slots = ny_malloc_array(
        nyir->param_count, sizeof(*lowered.param_aggregate_home_slots));
    if (!lowered.param_types || !lowered.param_abi ||
        !lowered.param_aggregate_home_slots)
      goto oom;
    memcpy(lowered.param_types, nyir->param_types,
           nyir->param_count * sizeof(*lowered.param_types));
    memset(lowered.param_abi, 0,
           nyir->param_count * sizeof(*lowered.param_abi));
    for (size_t i = 0; i < nyir->param_count; ++i)
      lowered.param_aggregate_home_slots[i] = UINT32_MAX;
    if (has_aggregate_param) {
      memcpy(lowered.param_abi, nyir->param_abi,
             nyir->param_count * sizeof(*lowered.param_abi));
    }
  }
  if (nyir->next_value > 0) {
    definitions = nyr_build_defs(nyir);
    if (!definitions)
      goto oom;
  }
  for (size_t i = 0; i < nyir->len; ++i) {
    const nyr_inst_t *in = &nyir->data[i];
    if ((in->op == NYIR_LOAD_LOCAL || in->op == NYIR_STORE_LOCAL ||
         in->op == NYIR_ADDR_LOCAL) &&
        in->imm >= 0 && (size_t)in->imm + 1 > local_count)
      local_count = (size_t)in->imm + 1;
  }
  if (!nyr_cfg_build(nyir, &cfg)) {
    nyr_err(err, err_len, "machine lower: failed to build CFG");
    goto done;
  }
  if ((caps & NY_NATIVE_CAP_MACH_SDIV_POW2) && nyir->len) {
    bool candidate = false;
    for (size_t i = 0; i < nyir->len; ++i) {
      int dividend = -1;
      int64_t divisor = 0;
      size_t skipped[3] = {0};
      if (mach_nir_match_signed_pow2_div(nyir, definitions, NULL, &cfg, i,
                                         &dividend, &divisor, skipped)) {
        candidate = true;
        break;
      }
    }
    if (candidate) {
      fused_dividend = ny_malloc_array(nyir->len, sizeof(*fused_dividend));
      fused_divisor = ny_calloc_array(nyir->len, sizeof(*fused_divisor));
      skip_fused_inst = ny_calloc_array(nyir->len, sizeof(*skip_fused_inst));
      if (!fused_dividend || !fused_divisor || !skip_fused_inst ||
          !nyr_build_use_def(nyir, &uses))
        goto oom;
      for (size_t i = 0; i < nyir->len; ++i)
        fused_dividend[i] = -1;
      for (size_t i = 0; i < nyir->len; ++i) {
        int dividend = -1;
        int64_t divisor = 0;
        size_t skipped[3] = {0};
        if (!mach_nir_match_signed_pow2_div(nyir, definitions, &uses, &cfg, i,
                                            &dividend, &divisor, skipped))
          continue;
        if (skip_fused_inst[skipped[0]] || skip_fused_inst[skipped[1]] ||
            skip_fused_inst[skipped[2]])
          continue;
        skip_fused_inst[skipped[0]] = true;
        skip_fused_inst[skipped[1]] = true;
        skip_fused_inst[skipped[2]] = true;
        fused_dividend[i] = dividend;
        fused_divisor[i] = divisor;
      }
    }
  }
  if ((caps & NY_NATIVE_CAP_MACH_SCALED_LEA) && nyir->len) {
    bool candidate = false;
    for (size_t i = 0; i < nyir->len; ++i) {
      int base = -1;
      int index = -1;
      size_t shift_pc = 0;
      unsigned scale = 0;
      if (nyir->data[i].op == NYIR_ADD_I64 &&
          mach_nir_match_scaled_add(nyir, definitions, &cfg, i, &base, &index,
                                    &shift_pc, &scale)) {
        candidate = true;
        break;
      }
    }
    if (candidate) {
      if (!skip_fused_inst)
        skip_fused_inst = ny_calloc_array(nyir->len, sizeof(*skip_fused_inst));
      fused_lea_base = ny_malloc_array(nyir->len, sizeof(*fused_lea_base));
      fused_lea_index = ny_malloc_array(nyir->len, sizeof(*fused_lea_index));
      fused_lea_scale = ny_calloc_array(nyir->len, sizeof(*fused_lea_scale));
      if (!skip_fused_inst || !fused_lea_base || !fused_lea_index ||
          !fused_lea_scale ||
          (!uses.offsets && !nyr_build_use_def(nyir, &uses)))
        goto oom;
      for (size_t i = 0; i < nyir->len; ++i) {
        fused_lea_base[i] = -1;
        fused_lea_index[i] = -1;
      }
      for (size_t i = 0; i < nyir->len; ++i) {
        int base = -1;
        int index = -1;
        size_t shift_pc = 0;
        unsigned scale = 0;
        if (!mach_nir_match_scaled_add(nyir, definitions, &cfg, i, &base,
                                       &index, &shift_pc, &scale) ||
            skip_fused_inst[shift_pc])
          continue;
        fused_lea_base[i] = base;
        fused_lea_index[i] = index;
        fused_lea_scale[i] = scale;
      }
      for (size_t i = 0; i < nyir->len; ++i) {
        if (skip_fused_inst[i] ||
            !mach_nir_scaled_value_only_feeds_fused_adds(
                nyir, definitions, &uses, &cfg, fused_lea_base, i))
          continue;
        skip_fused_inst[i] = true;
      }
    }
  }
  if (nyir->len && definitions) {
    bool candidate = false;
    for (size_t i = 0; i < nyir->len; ++i) {
      int input = -1;
      size_t skipped[16];
      size_t skipped_len = 0;
      if (mach_nir_match_swar_popcount(nyir, definitions, NULL, &cfg, i, &input,
                                       skipped, &skipped_len)) {
        candidate = true;
        break;
      }
    }
    if (candidate) {
      if (!skip_fused_inst)
        skip_fused_inst = ny_calloc_array(nyir->len, sizeof(*skip_fused_inst));
      fused_popcount = ny_malloc_array(nyir->len, sizeof(*fused_popcount));
      if (!skip_fused_inst || !fused_popcount ||
          (!uses.offsets && !nyr_build_use_def(nyir, &uses)))
        goto oom;
      for (size_t i = 0; i < nyir->len; ++i)
        fused_popcount[i] = -1;
      for (size_t i = 0; i < nyir->len; ++i) {
        int input = -1;
        size_t skipped[16];
        size_t skipped_len = 0;
        if (!mach_nir_match_swar_popcount(nyir, definitions, &uses, &cfg, i,
                                          &input, skipped, &skipped_len))
          continue;
        bool clash = false;
        for (size_t k = 0; k < skipped_len; ++k)
          if (skip_fused_inst[skipped[k]])
            clash = true;
        if (clash)
          continue;
        for (size_t k = 0; k < skipped_len; ++k)
          skip_fused_inst[skipped[k]] = true;
        fused_popcount[i] = input;
      }
    }
  }
  if (cfg.block_count > SIZE_MAX / sizeof(*label_blocks))
    goto oom;
  if (cfg.block_count) {
    label_blocks = ny_calloc_array(cfg.block_count, sizeof(*label_blocks));
    if (!label_blocks)
      goto oom;
    for (size_t block = 0; block < cfg.block_count; ++block) {
      if (cfg.block_label[block] < 0)
        continue;
      label_blocks[label_block_len++] = (mach_label_block_t){
          .label = cfg.block_label[block], .block = (uint32_t)block};
    }
    qsort(label_blocks, label_block_len, sizeof(*label_blocks),
          mach_label_block_cmp);
  }
  if (!nyr_type_map_init(&types, nyir, local_count))
    goto oom;
  if (local_count) {
    local_address_taken =
        ny_calloc_array(local_count, sizeof(*local_address_taken));
    if (!local_address_taken)
      goto oom;
    for (size_t i = 0; i < nyir->len; ++i) {
      const nyr_inst_t *in = &nyir->data[i];
      if (in->op == NYIR_ADDR_LOCAL && in->imm >= 0 &&
          (size_t)in->imm < local_count)
        local_address_taken[in->imm] = true;
    }
  }
  {
    /*
     * Keep a valid base even for degenerate zero-value functions.  Normal
     * instructions still require verifier-valid SSA indices; the one-slot
     * allocation prevents a malformed/empty function from indexing NULL.
     */
    size_t value_count = nyir->next_value > 0 ? (size_t)nyir->next_value : 1u;
    values = ny_calloc_array(value_count, sizeof(*values));
    pointer_values = ny_calloc_array(value_count, sizeof(*pointer_values));
    if (!values || !pointer_values)
      goto oom;
    /*
     * Pointer values may cross SSA copies and PHIs before entering integer
     * address arithmetic.  Propagate that class with the existing use-def
     * graph rather than rescanning the complete function to a fixed point.
     */
    if (nyir->next_value > 0) {
      nyr_use_def_t pointer_uses = {0};
      int *pointer_queue =
          ny_malloc_array((size_t)nyir->next_value, sizeof(*pointer_queue));
      if (!pointer_queue || !nyr_build_use_def(nyir, &pointer_uses)) {
        free(pointer_queue);
        nyr_use_def_free(&pointer_uses);
        goto oom;
      }
      size_t pointer_head = 0, pointer_tail = 0;
      for (size_t i = 0; i < nyir->len; ++i) {
        const nyr_inst_t *in = &nyir->data[i];
        if ((in->op == NYIR_ADDR_LOCAL || in->op == NYIR_ADDR_SYMBOL ||
             in->op == NYIR_ALLOCA || in->op == NYIR_VA_SAVE) &&
            in->dst >= 0 && in->dst < nyir->next_value &&
            !pointer_values[in->dst]) {
          pointer_values[in->dst] = true;
          pointer_queue[pointer_tail++] = in->dst;
        }
      }
      while (pointer_head < pointer_tail) {
        int value = pointer_queue[pointer_head++];
        if (value < 0 || (size_t)value >= pointer_uses.value_count)
          continue;
        for (size_t u = pointer_uses.offsets[value];
             u < pointer_uses.offsets[value + 1]; ++u) {
          size_t user_idx = pointer_uses.users[u];
          if (user_idx >= nyir->len)
            continue;
          const nyr_inst_t *user = &nyir->data[user_idx];
          if ((user->op != NYIR_COPY && user->op != NYIR_PHI) ||
              user->dst < 0 || user->dst >= nyir->next_value ||
              pointer_values[user->dst])
            continue;
          pointer_values[user->dst] = true;
          pointer_queue[pointer_tail++] = user->dst;
        }
      }
      free(pointer_queue);
      nyr_use_def_free(&pointer_uses);
      for (int value = 0; value < nyir->next_value; ++value) {
        if (!nyr_mach_alloc_typed_vreg(
                &lowered,
                mach_nir_value_type(&types, pointer_values, definitions, value,
                                    nyir),
                &values[value]))
          goto oom;
      }
      for (size_t i = 0; i < nyir->len; ++i) {
        const nyr_inst_t *in = &nyir->data[i];
        if ((in->op == NYIR_CMP_I64 || in->op == NYIR_CMP_F64 ||
             in->op == NYIR_CMP_F32) &&
            in->dst >= 0 && in->dst < nyir->next_value) {
          nyr_mach_reg_t reg = values[in->dst];
          lowered.vreg_classes[reg] = NYR_MACH_REGCLASS_GPR;
          lowered.vreg_types[reg] = NYR_MACH_TYPE_I64;
        }
      }
    }
  }
  for (size_t local = 0; local < local_count; ++local) {
    uint32_t slot = 0;
    nyr_mach_type_t type = NYR_MACH_TYPE_I64;
    size_t slot_size = 8;
    if (types.local_v256_i64 && types.local_v256_i64[local]) {
      type = NYR_MACH_TYPE_V256_I64;
      slot_size = 32;
    } else if (types.local_v256_f64 && types.local_v256_f64[local]) {
      type = NYR_MACH_TYPE_V256_F64;
      slot_size = 32;
    } else if (types.local_v128_f64 && types.local_v128_f64[local]) {
      type = NYR_MACH_TYPE_V128_F64;
      slot_size = 16;
    } else if (types.local_v128_f32 && types.local_v128_f32[local]) {
      type = NYR_MACH_TYPE_V128_F32;
      slot_size = 16;
    } else if (types.local_v128_i64 && types.local_v128_i64[local]) {
      type = NYR_MACH_TYPE_V128_I64;
      slot_size = 16;
    } else if (types.local_f64[local]) {
      type = NYR_MACH_TYPE_F64;
    } else if (types.local_f32[local]) {
      type = NYR_MACH_TYPE_F32;
    }
    if (!nyr_mach_alloc_typed_frame_slot(&lowered, slot_size, slot_size, type,
                                         local_address_taken[local], &slot) ||
        slot != local)
      goto oom;
  }
  for (size_t i = 0; i < nyir->param_count; ++i) {
    if (!has_aggregate_param || nyir->param_types[i] != NYIR_PARAM_AGGREGATE)
      continue;
    uint32_t size = NYIR_ARG_AGG_SIZE(nyir->param_abi[i]);
    if (NYIR_ARG_AGG_CLASS(nyir->param_abi[i], 0) == NYIR_ARG_CLASS_MEMORY)
      continue;
    size_t chunks = (size + 7u) / 8u;
    uint32_t first = 0;
    if (!chunks)
      chunks = 1;
    if (!nyr_mach_alloc_typed_frame_slot(&lowered, 8, 8, NYR_MACH_TYPE_I64,
                                         true, &first))
      goto oom;
    lowered.param_aggregate_home_slots[i] = first;
    for (size_t chunk = 1; chunk < chunks; ++chunk) {
      uint32_t slot = 0;
      if (!nyr_mach_alloc_typed_frame_slot(&lowered, 8, 8, NYR_MACH_TYPE_I64,
                                           true, &slot) ||
          slot != first + chunk)
        goto oom;
    }
  }
  if (trace_lowering)
    NY_DBG(NY_DBG_LOWER, "NYIR -> MIR lowering (%zu instructions)", nyir->len);
  for (size_t i = 0, block = 0; i < nyir->len; ++i) {
    const nyr_inst_t *in = &nyir->data[i];
    if (block < cfg.block_count && cfg.block_start[block] == i) {
      uint32_t emitted_block = 0;
      /*
       * Machine blocks use stable dense IDs. Source labels remain NYIR
       * metadata; branches below resolve them to these canonical IDs.
       */
      if (!nyr_mach_begin_block(&lowered, (uint32_t)block, &emitted_block) ||
          emitted_block != block)
        goto oom;
      lowered.blocks[emitted_block].source_pc = (uint32_t)i;
      ++block;
    }
    if (skip_fused_inst && skip_fused_inst[i])
      continue;
    if (trace_lowering)
      ny_dbg_lower_instruction(i, nyr_op_name(in->op), in->dst, in->a, in->b,
                               in->c, in->imm, in->symbol);
    if ((in->op == NYIR_LOAD_I64 || in->op == NYIR_STORE_I64) &&
        (in->flags & NYIR_INST_F_MEM_BYTE) &&
        !(caps & NY_NATIVE_CAP_MACH_BYTE)) {
      nyr_err(err, err_len, "machine lower: target has no machine byte access");
      goto done;
    }
    nyr_mach_inst_t inst = {0};
    int64_t copy_size = 0;
    inst.source_pc = (uint32_t)i;
    if (in->op == NYIR_LABEL || in->op == NYIR_PHI) {
      continue;
    }
    if (in->op == NYIR_CONST_I64 || in->op == NYIR_CONST_F64 ||
        in->op == NYIR_CONST_F32) {
      inst.opcode = NYR_MACH_COPY;
      inst.dst = mach_vreg(&lowered, values[in->dst]);
      inst.src0 =
          (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_IMM, .as.imm = in->imm};
    } else if (in->op == NYIR_LOAD_LOCAL) {
      inst.opcode = NYR_MACH_LOAD;
      inst.dst = mach_vreg(&lowered, values[in->dst]);
      inst.src0 = mach_frame((uint32_t)in->imm);
      inst.effects = NYR_MACH_EFFECT_READ_MEMORY;
    } else if (in->op == NYIR_STORE_LOCAL) {
      inst.opcode = NYR_MACH_STORE;
      inst.dst = mach_frame((uint32_t)in->imm);
      inst.src0 = mach_vreg(&lowered, values[in->a]);
      inst.effects = NYR_MACH_EFFECT_WRITE_MEMORY;
    } else if (in->op == NYIR_ADDR_LOCAL) {
      inst.opcode = NYR_MACH_LEA;
      inst.dst = mach_vreg(&lowered, values[in->dst]);
      inst.src0 = mach_frame((uint32_t)in->imm);
    } else if (in->op == NYIR_ALLOCA) {
      /*
       * Stack allocation: N dense 8-byte frame slots. Slot index grows toward
       * lower addresses (slot i at -8*(base+i+1)), so LEA must use the *last*
       * slot: then [last, last+8*N) covers the N slots contiguously upward.
       * LEA(first) + sequential stores would write into earlier slots / CS.
       */
      size_t nbytes = in->imm > 0 ? (size_t)in->imm : 8;
      size_t nslots = (nbytes + 7) / 8;
      if (nslots == 0)
        nslots = 1;
      uint32_t last = 0;
      for (size_t k = 0; k < nslots; ++k) {
        uint32_t slot = 0;
        if (!nyr_mach_alloc_typed_frame_slot(&lowered, 8, 8, NYR_MACH_TYPE_I64,
                                             true, &slot))
          goto oom;
        last = slot;
      }
      if (in->dst < 0) {
        nyr_err(err, err_len,
                "machine lower: NYIR opcode %s has no destination",
                nyr_op_name(in->op));
        goto done;
      }
      inst.opcode = NYR_MACH_LEA;
      inst.dst = mach_vreg(&lowered, values[in->dst]);
      inst.src0 = mach_frame(last);
    } else if (in->op == NYIR_VA_SAVE) {
      if (in->dst < 0 || (in->imm != 48 && in->imm != 176)) {
        nyr_err(err, err_len, "machine lower: invalid va.save area");
        goto done;
      }
      uint32_t last = 0;
      for (size_t slot_index = 0; slot_index < (size_t)in->imm / 8;
           ++slot_index) {
        uint32_t slot = 0;
        if (!nyr_mach_alloc_typed_frame_slot(&lowered, 8, 8, NYR_MACH_TYPE_I64,
                                             true, &slot))
          goto oom;
        last = slot;
      }
      inst.opcode = NYR_MACH_VA_SAVE;
      inst.dst = mach_vreg(&lowered, values[in->dst]);
      inst.src0 = mach_frame(last);
      inst.immediate = in->imm;
      inst.effects = NYR_MACH_EFFECT_WRITE_MEMORY;
    } else if (in->op == NYIR_VA_START) {
      inst.opcode = NYR_MACH_VA_START;
      inst.src0 = mach_vreg(&lowered, values[in->a]);
      inst.src1 = mach_vreg(&lowered, values[in->b]);
      inst.src2 =
          (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_IMM,
                               .as.imm = (int64_t)((uint64_t)in->imm >> 32)};
      inst.immediate = (uint32_t)in->imm;
      inst.effects = NYR_MACH_EFFECT_READ_MEMORY | NYR_MACH_EFFECT_WRITE_MEMORY;
    } else if (in->op == NYIR_VA_ARG) {
      inst.opcode = NYR_MACH_VA_ARG;
      inst.dst = mach_vreg(&lowered, values[in->dst]);
      inst.src0 = mach_vreg(&lowered, values[in->a]);
      inst.immediate = in->imm;
      inst.effects = NYR_MACH_EFFECT_READ_MEMORY | NYR_MACH_EFFECT_WRITE_MEMORY;
    } else if (in->op == NYIR_CAPTURE_RET) {
      /*
       * Secondary ABI return register capture after CALL (SysV selectors).
       * Shape: INTRINSIC dst=vreg, src0=IMM selector
       * (0=rdx,1=rax,2=xmm0,3=xmm1).
       */
      if (in->dst < 0) {
        nyr_err(err, err_len,
                "machine lower: NYIR opcode %s has no destination",
                nyr_op_name(in->op));
        goto done;
      }
      inst.opcode = NYR_MACH_INTRINSIC;
      inst.dst = mach_vreg(&lowered, values[in->dst]);
      inst.src0 =
          (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_IMM, .as.imm = in->imm};
    } else if (in->op == NYIR_COPY_STRUCT) {
      /*
       * Byte copy *dst ← *src of imm bytes.
       * Shape: INTRINSIC src0=dst-ptr, src1=src-ptr, src2=IMM size.
       */
      if (in->a < 0 || in->b < 0 || in->imm <= 0) {
        nyr_err(err, err_len,
                "machine lower: NYIR opcode %s has invalid size or operands",
                nyr_op_name(in->op));
        goto done;
      }
      inst.opcode = NYR_MACH_INTRINSIC;
      inst.src0 = mach_vreg(&lowered, values[in->a]);
      inst.src1 = mach_vreg(&lowered, values[in->b]);
      inst.src2 =
          (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_IMM, .as.imm = in->imm};
      inst.effects = NYR_MACH_EFFECT_READ_MEMORY | NYR_MACH_EFFECT_WRITE_MEMORY;
    } else if (in->op == NYIR_ADDR_SYMBOL) {
      inst.opcode = NYR_MACH_LEA;
      inst.dst = mach_vreg(&lowered, values[in->dst]);
      inst.src0 = (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_SYMBOL,
                                       .as.symbol = in->symbol,
                                       .relocation = NYR_MACH_RELOC_PC_REL32,
                                       .nonpreemptible =
                                           (in->flags &
                                            NYIR_INST_F_SYMBOL_NONPREEMPTIBLE) != 0};
    } else if (in->op == NYIR_LOAD_I64) {
      inst.opcode = NYR_MACH_LOAD;
      inst.dst = mach_vreg(&lowered, values[in->dst]);
      inst.src0 = mach_vreg(&lowered, values[in->a]);
      inst.mem_size = (in->flags & NYIR_INST_F_MEM_U16)   ? 2
                      : (in->flags & NYIR_INST_F_MEM_I32) ? 4
                                                          : 0;
      inst.byte_width =
          (in->flags & NYIR_INST_F_MEM_BYTE) != 0 || inst.mem_size != 0;
      /*
       * A volatile read is observable, so it orders like a write: nothing may
       * forward, merge, or reorder across it.
       */
      inst.effects =
          (in->effects & NYIR_EFFECT_VOLATILE)
              ? NYR_MACH_EFFECT_READ_MEMORY | NYR_MACH_EFFECT_WRITE_MEMORY
              : NYR_MACH_EFFECT_READ_MEMORY;
    } else if (in->op == NYIR_STORE_I64) {
      inst.opcode = NYR_MACH_STORE;
      inst.dst = mach_vreg(&lowered, values[in->a]);
      inst.src0 = mach_vreg(&lowered, values[in->c]);
      inst.mem_size = (in->flags & NYIR_INST_F_MEM_U16)   ? 2
                      : (in->flags & NYIR_INST_F_MEM_I32) ? 4
                                                          : 0;
      inst.byte_width =
          (in->flags & NYIR_INST_F_MEM_BYTE) != 0 || inst.mem_size != 0;
      inst.effects = NYR_MACH_EFFECT_WRITE_MEMORY;
    } else if (in->op == NYIR_RET) {
      inst.opcode = NYR_MACH_RET;
      if (in->a >= 0)
        inst.src0 = mach_vreg(&lowered, values[in->a]);
      if (in->b >= 0)
        inst.src1 = mach_vreg(&lowered, values[in->b]);
      inst.effects = NYR_MACH_EFFECT_CONTROL;
    } else if (in->op == NYIR_BR || in->op == NYIR_BR_IF) {
      int target = mach_find_nir_block(label_blocks, label_block_len, in->imm);
      if (target < 0) {
        nyr_err(err, err_len, "machine lower: branch target is missing");
        goto done;
      }
      inst.opcode = in->op == NYIR_BR ? NYR_MACH_BR : NYR_MACH_BR_IF;
      if (in->op == NYIR_BR_IF)
        inst.src0 = mach_vreg(&lowered, values[in->a]);
      inst.src1 = (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_BLOCK,
                                       .as.block_index = (uint32_t)target};
      inst.effects = NYR_MACH_EFFECT_CONTROL;
    } else if (in->op == NYIR_INLINE_ASM && in->symbol && in->dst >= 0 &&
               in->a >= 0 && in->b >= 0 &&
               ((in->imm == -23 &&
                 strcmp(in->symbol, "unsigned-shift-right") == 0) ||
                ((in->imm == -21 || in->imm == -22) &&
                 strcmp(in->symbol, "unsigned-div") == 0))) {
      /*
       * The C frontend's unsigned operations are ordinary arithmetic.
       */
      inst.opcode = in->imm == -23   ? NYR_MACH_SHR
                    : in->imm == -21 ? NYR_MACH_DIV
                                     : NYR_MACH_MOD;
      inst.unsigned_arith = in->imm != -23;
      inst.dst = mach_vreg(&lowered, values[in->dst]);
      inst.src0 = mach_vreg(&lowered, values[in->a]);
      inst.src1 = mach_vreg(&lowered, values[in->b]);
    } else if (in->op == NYIR_INLINE_ASM) {
      inst.opcode = NYR_MACH_INLINE_ASM;
      inst.src0 = (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_SYMBOL,
                                       .as.symbol = in->symbol};
      if (in->imm == -32 && in->a >= 0)
        inst.src1 = mach_vreg(&lowered, values[in->a]);
      inst.effects = NYR_MACH_EFFECT_CALL;
    } else if (in->op == NYIR_CALL &&
               mach_small_memcpy_size(nyir, definitions, in, &copy_size)) {
      /*
       * Shape matches a lowered COPY_STRUCT: src0=dst-ptr, src1=src-ptr,
       * src2=IMM size. memcpy forbids overlap, so splitting the copy into
       * typed moves cannot change the result.
       */
      int args[NYIR_CALL_MAX_ARGS] = {0};
      int arg_count = 0;
      char args_err[128] = {0};
      if (!nyr_call_args(in, nyir->next_value, args,
                         sizeof(args) / sizeof(args[0]), &arg_count, args_err,
                         sizeof(args_err))) {
        nyr_err(err, err_len, "machine lower: invalid call arguments: %s",
                args_err[0] ? args_err : NY_NATIVE_UNKNOWN_ERR);
        goto done;
      }
      inst.opcode = NYR_MACH_INTRINSIC;
      inst.src0 = mach_vreg(&lowered, values[args[0]]);
      inst.src1 = mach_vreg(&lowered, values[args[1]]);
      inst.src2 = (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_IMM,
                                       .as.imm = copy_size};
      inst.effects = NYR_MACH_EFFECT_READ_MEMORY | NYR_MACH_EFFECT_WRITE_MEMORY;
      if (!nyr_mach_emit(&lowered, inst))
        goto oom;
      if (in->dst >= 0) {
        /*
         * memcpy returns dst, so its users read the pointer we copied from.
         */
        nyr_mach_inst_t copy = {0};
        copy.opcode = NYR_MACH_COPY;
        copy.source_pc = (uint32_t)i;
        copy.dst = mach_vreg(&lowered, values[in->dst]);
        copy.src0 = inst.src0;
        if (!nyr_mach_emit(&lowered, copy))
          goto oom;
      }
      continue;
    } else if (in->op == NYIR_CALL) {
      /*
       * Float-to-int casts can be lowered as `rt_f64_to_i64` calls even though
       * the helper is exactly a truncating conversion. Expand that call into
       * NYR_MACH_CONVERT so the encoder emits one cvttsd2si instead of a call
       * plus the stack round trip needed to marshal the value through xmm0.
       * Anything the fast path cannot prove (tagged results, indirect calls,
       * untyped values) keeps the ordinary call shape.
       */
      if (in->symbol && in->dst >= 0 &&
          (in->flags & NYIR_INST_F_INDIRECT) == 0 &&
          in->scalar_class == NYIR_SCALAR_I64_RAW &&
          strcmp(in->symbol, "rt_f64_to_i64") == 0) {
        int conv_args[NYIR_CALL_MAX_ARGS] = {0};
        int conv_argc = 0;
        if (nyr_call_args(in, nyir->next_value, conv_args, NYIR_CALL_MAX_ARGS,
                          &conv_argc, NULL, 0) &&
            conv_argc == 1 && conv_args[0] >= 0 &&
            nyr_call_arg_class_get(in, 0) == NYIR_SCALAR_F64) {
          nyr_mach_operand_t conv_src =
              mach_vreg(&lowered, values[conv_args[0]]);
          nyr_mach_operand_t conv_dst = mach_vreg(&lowered, values[in->dst]);
          nyr_mach_type_t conv_dst_type = mach_value_type(&lowered, &conv_dst);
          if (mach_value_type(&lowered, &conv_src) == NYR_MACH_TYPE_F64 &&
              (conv_dst_type == NYR_MACH_TYPE_I64 ||
               conv_dst_type == NYR_MACH_TYPE_PTR)) {
            nyr_mach_inst_t conv = {.opcode = NYR_MACH_CONVERT,
                                    .source_pc = (uint32_t)i,
                                    .dst = conv_dst,
                                    .src0 = conv_src,
                                    .effects = NYR_MACH_EFFECT_NONE};
            if (!nyr_mach_emit(&lowered, conv))
              goto oom;
            continue;
          }
        }
      }
      inst.opcode = NYR_MACH_CALL;
      inst.runtime_operation = ny_native_runtime_operation(in->symbol);
      if (in->dst >= 0)
        inst.dst = mach_vreg(&lowered, values[in->dst]);
      bool indirect = (in->flags & NYIR_INST_F_INDIRECT) != 0;
      if (in->imm > 0) {
        int args[NYIR_CALL_MAX_ARGS] = {0};
        int arg_count = 0;
        char args_err[128] = {0};
        if (!nyr_call_args(in, nyir->next_value, args,
                           sizeof(args) / sizeof(args[0]), &arg_count, args_err,
                           sizeof(args_err))) {
          nyr_err(err, err_len, "machine lower: invalid call arguments: %s",
                  args_err[0] ? args_err : NY_NATIVE_UNKNOWN_ERR);
          goto done;
        }
        if (indirect) {
          if (arg_count < 1 || in->symbol) {
            nyr_err(err, err_len,
                    "machine lower: indirect call is missing its target");
            goto done;
          }
          inst.src0 = mach_vreg(&lowered, values[args[0]]);
          --arg_count;
          memmove(args, args + 1, (size_t)arg_count * sizeof(*args));
        } else {
          inst.src0 = (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_SYMBOL,
                                           .as.symbol = in->symbol,
                                           .relocation = NYR_MACH_RELOC_CALL};
        }
        inst.args_len = (size_t)arg_count;
        inst.args = ny_calloc_array(inst.args_len, sizeof(*inst.args));
        if (inst.args_len && !inst.args)
          goto oom;
        if (in->arg_sizes && inst.args_len) {
          inst.arg_sizes =
              ny_calloc_array(inst.args_len, sizeof(*inst.arg_sizes));
          if (!inst.arg_sizes) {
            free(inst.args);
            goto oom;
          }
        }
        for (size_t arg = 0; arg < inst.args_len; ++arg)
          inst.args[arg] = mach_vreg(&lowered, values[args[arg]]);
        if (inst.arg_sizes)
          memcpy(inst.arg_sizes, in->arg_sizes + (indirect ? 1u : 0u),
                 inst.args_len * sizeof(*inst.arg_sizes));
      } else {
        if (indirect) {
          nyr_err(err, err_len,
                  "machine lower: indirect call is missing its target");
          goto done;
        }
        inst.src0 = (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_SYMBOL,
                                         .as.symbol = in->symbol,
                                         .relocation = NYR_MACH_RELOC_CALL,
                                         .nonpreemptible =
                                             (in->flags &
                                              NYIR_INST_F_SYMBOL_NONPREEMPTIBLE) != 0};
      }
      inst.call_is_extern = !indirect && (in->flags & NYIR_INST_F_EXTERN) != 0;
      inst.call_has_sret = (in->flags & NYIR_INST_F_SRET) != 0;
      inst.call_is_variadic = (in->flags & NYIR_INST_F_VARIADIC) != 0;
      inst.effects = NYR_MACH_EFFECT_CALL;
    } else if (in->op == NYIR_SELECT_I64) {
      inst.opcode = NYR_MACH_SELECT;
      inst.dst = mach_vreg(&lowered, values[in->dst]);
      inst.src0 = mach_vreg(&lowered, values[in->a]);
      inst.src1 = mach_vreg(&lowered, values[in->b]);
      inst.src2 = mach_vreg(&lowered, values[in->c]);
      inst.condition = NYR_MACH_COND_NE;
    } else if (in->op == NYIR_SQRT_F64 || in->op == NYIR_SIN_F64 ||
               in->op == NYIR_COS_F64) {
      /*
       * x87 trig functions require memory operands; spill src to frame if
       * needed
       */
      nyr_mach_operand_t src0 = mach_vreg(&lowered, values[in->a]);
      bool src0_is_vreg = (src0.kind == NYR_MACH_OPERAND_VREG);
      uint32_t src0_frame = 0;
      if (src0_is_vreg) {
        if (!nyr_mach_alloc_typed_frame_slot(&lowered, 8, 8, NYR_MACH_TYPE_F64,
                                             false, &src0_frame))
          goto oom;
        nyr_mach_inst_t spill = {.opcode = NYR_MACH_STORE,
                                 .source_pc = (uint32_t)i,
                                 .dst = mach_frame(src0_frame),
                                 .src0 = mach_vreg(&lowered, values[in->a]),
                                 .effects = NYR_MACH_EFFECT_WRITE_MEMORY};
        if (!nyr_mach_emit(&lowered, spill))
          goto oom;
        src0 = mach_frame(src0_frame);
      } else {
        src0 = mach_vreg(&lowered, values[in->a]);
      }
      nyr_mach_operand_t dst = mach_vreg(&lowered, values[in->dst]);
      bool dst_is_vreg = (dst.kind == NYR_MACH_OPERAND_VREG);
      uint32_t dst_frame = 0;
      if (dst_is_vreg) {
        if (!nyr_mach_alloc_typed_frame_slot(&lowered, 8, 8, NYR_MACH_TYPE_F64,
                                             false, &dst_frame))
          goto oom;
        dst = mach_frame(dst_frame);
      }
      inst.opcode = (in->op == NYIR_SQRT_F64)  ? NYR_MACH_SQRT
                    : (in->op == NYIR_SIN_F64) ? NYR_MACH_SIN
                                               : NYR_MACH_COS;
      inst.src0 = src0;
      inst.dst = mach_frame(dst_frame);
      inst.effects = NYR_MACH_EFFECT_NONE;
      if (!nyr_mach_emit(&lowered, inst))
        goto oom;
      if (dst_is_vreg) {
        /*
         * Reload result from frame to vreg
         */
        nyr_mach_inst_t reload = {.opcode = NYR_MACH_LOAD,
                                  .source_pc = (uint32_t)i,
                                  .dst = mach_vreg(&lowered, values[in->dst]),
                                  .src0 = mach_frame(dst_frame),
                                  .effects = NYR_MACH_EFFECT_READ_MEMORY};
        if (!nyr_mach_emit(&lowered, reload))
          goto oom;
      }
      continue;
    } else if (fused_popcount && fused_popcount[i] >= 0) {
      inst.opcode = NYR_MACH_POPCOUNT64;
      inst.dst = mach_vreg(&lowered, values[in->dst]);
      inst.src0 = mach_vreg(&lowered, values[fused_popcount[i]]);
      inst.effects = NYR_MACH_EFFECT_NONE;
    } else if (fused_dividend && fused_dividend[i] >= 0) {
      inst.opcode = NYR_MACH_DIV;
      inst.dst = mach_vreg(&lowered, values[in->dst]);
      inst.src0 = mach_vreg(&lowered, values[fused_dividend[i]]);
      inst.src1 = (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_IMM,
                                       .as.imm = fused_divisor[i]};
    } else if (fused_lea_base && fused_lea_base[i] >= 0) {
      inst.opcode = NYR_MACH_LEA;
      inst.dst = mach_vreg(&lowered, values[in->dst]);
      inst.src0 = mach_vreg(&lowered, values[fused_lea_base[i]]);
      if (fused_lea_base[i] == fused_lea_index[i]) {
        /*
         * Preserve the compact x + x*scale form for existing clients.
         */
        inst.src1 = (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_IMM,
                                         .as.imm = fused_lea_scale[i]};
      } else {
        inst.src1 = mach_vreg(&lowered, values[fused_lea_index[i]]);
        inst.src2 = (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_IMM,
                                         .as.imm = fused_lea_scale[i]};
      }
    } else if (mach_opcode_from_nir(in->op, &inst.opcode)) {
      inst.dst = mach_vreg(&lowered, values[in->dst]);
      inst.src0 = mach_vreg(&lowered, values[in->a]);
      inst.narrow32 = (in->flags & NYIR_INST_F_NARROW32) != 0;
      inst.dividend_u31 = (in->flags & NYIR_INST_F_DIVIDEND_U31) != 0;
      /*
       * Untyped source parameters may already acquire the destination's
       * floating type through their call sites.  A conversion between equal
       * machine types is a move, not an invalid f64-to-f64 conversion.
       */
      if (inst.opcode == NYR_MACH_CONVERT &&
          mach_same_value_type(&lowered, &inst.dst, &inst.src0))
        inst.opcode = NYR_MACH_COPY;

      bool unary = in->op == NYIR_COPY || in->op == NYIR_I64_TO_F64 ||
                   in->op == NYIR_I64_TO_F32 || in->op == NYIR_F64_TO_I64 ||
                   in->op == NYIR_F32_TO_I64 || in->op == NYIR_F64_TO_F32 ||
                   in->op == NYIR_F32_TO_F64 || in->op == NYIR_SQRT_F64 ||
                   in->op == NYIR_SIN_F64 || in->op == NYIR_COS_F64 ||
                   in->op == NYIR_BSWAP_I64 || in->op == NYIR_BSWAP32_I64;
      if (!unary) {
        bool shift_like = in->op == NYIR_SHL_I64 || in->op == NYIR_SAR_I64 ||
                          in->op == NYIR_ROR_I64 || in->op == NYIR_ROR32_I64;
        int bdef = (in->b >= 0 && definitions) ? definitions[in->b] : -1;
        int64_t rhs_constant = 0;
        bool has_constant_rhs =
            bdef >= 0 && nyir->data[bdef].op == NYIR_CONST_I64 &&
            mach_nir_const_i64(nyir, definitions, in->b, &rhs_constant);
        /*
         * Keep signed constant divisors explicit before register lowering.
         * A materialized loop constant can share a vreg with other
         * definitions, hiding it from the encoder's constant recovery.
         */
        bool immediate_divisor =
            (caps & NY_NATIVE_CAP_MACH_SDIV_POW2) && !inst.unsigned_arith &&
            (in->op == NYIR_DIV_I64 || in->op == NYIR_MOD_I64) &&
            (has_constant_rhs ||
             mach_nir_const_i64(nyir, definitions, in->b, &rhs_constant)) &&
            (rhs_constant > 1 || rhs_constant < -1);
        bool immediate_shift =
            bdef >= 0 && nyir->data[bdef].op == NYIR_CONST_I64 && shift_like;
        bool immediate_rhs = immediate_shift || immediate_divisor;
        if (immediate_rhs) {
          inst.src1 = (nyr_mach_operand_t){
              .kind = NYR_MACH_OPERAND_IMM,
              .as.imm = immediate_shift ? nyir->data[bdef].imm : rhs_constant};
        } else {
          inst.src1 = mach_vreg(&lowered, values[in->b]);
        }
        /*
         * Put integer constants on the encodable side of commutative ALU
         * operations. Keeping `3 * x` as a constant LHS otherwise prevents
         * the object writer from selecting its immediate/LEA forms.
         */
        bool commutative_i64 = in->op == NYIR_ADD_I64 ||
                               in->op == NYIR_MUL_I64 ||
                               in->op == NYIR_AND_I64 ||
                               in->op == NYIR_OR_I64 || in->op == NYIR_XOR_I64;
        int64_t lhs_constant = 0;
        if (commutative_i64 && !has_constant_rhs &&
            mach_value_type(&lowered, &inst.src0) == NYR_MACH_TYPE_I64 &&
            mach_value_type(&lowered, &inst.src1) == NYR_MACH_TYPE_I64 &&
            mach_nir_const_i64(nyir, definitions, in->a, &lhs_constant)) {
          nyr_mach_operand_t lhs = inst.src0;
          inst.src0 = inst.src1;
          inst.src1 = lhs;
        }
      }

      if (inst.opcode == NYR_MACH_CMP) {
        if (in->a >= 0 && in->a < nyir->next_value) {
          int di = definitions ? definitions[in->a] : -1;
          bool cmp_result = di >= 0 && (nyir->data[di].op == NYIR_CMP_I64 ||
                                        nyir->data[di].op == NYIR_CMP_F64 ||
                                        nyir->data[di].op == NYIR_CMP_F32);
          if (cmp_result) {
            lowered.vreg_classes[values[in->a]] = NYR_MACH_REGCLASS_GPR;
            lowered.vreg_types[values[in->a]] = NYR_MACH_TYPE_I64;
            inst.src0 = mach_vreg(&lowered, values[in->a]);
          }
        }
        if (in->b >= 0 && in->b < nyir->next_value) {
          int di = definitions ? definitions[in->b] : -1;
          bool cmp_result = di >= 0 && (nyir->data[di].op == NYIR_CMP_I64 ||
                                        nyir->data[di].op == NYIR_CMP_F64 ||
                                        nyir->data[di].op == NYIR_CMP_F32);
          if (cmp_result) {
            lowered.vreg_classes[values[in->b]] = NYR_MACH_REGCLASS_GPR;
            lowered.vreg_types[values[in->b]] = NYR_MACH_TYPE_I64;
            inst.src1 = mach_vreg(&lowered, values[in->b]);
          }
        }
        if (!mach_condition_from_nir(in->cmp, &inst.condition)) {
          nyr_err(err, err_len, "machine lower: invalid comparison condition");
          goto done;
        }
      }
    } else if (in->op == NYIR_VEC8_SAR_I64 || in->op == NYIR_VEC8_SHR_I64 ||
               in->op == NYIR_VEC8_SHL_I64) {
      if (!(caps & NY_NATIVE_CAP_MACH_AVX2)) {
        nyr_err(err, err_len, "machine lower: vec8 shift requires AVX2");
        goto done;
      }
      inst.opcode = in->op == NYIR_VEC8_SHR_I64   ? NYR_MACH_VSHR
                    : in->op == NYIR_VEC8_SHL_I64 ? NYR_MACH_SHL
                                                  : NYR_MACH_SAR;
      inst.dst = mach_vreg(&lowered, values[in->dst]);
      inst.src0 = mach_vreg(&lowered, values[in->a]);
      inst.src1 =
          (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_IMM, .as.imm = in->imm};
    } else if (in->op == NYIR_VEC8_CMP_I64) {
      if (!(caps & NY_NATIVE_CAP_MACH_AVX2)) {
        nyr_err(err, err_len, "machine lower: vec8 cmp requires AVX2");
        goto done;
      }
      inst.opcode = NYR_MACH_CMP;
      inst.dst = mach_vreg(&lowered, values[in->dst]);
      inst.src0 = mach_vreg(&lowered, values[in->a]);
      inst.src1 = mach_vreg(&lowered, values[in->b]);
      if (!mach_condition_from_nir(in->cmp, &inst.condition)) {
        nyr_err(err, err_len, "machine lower: invalid vec8 cmp condition");
        goto done;
      }
    } else if (in->op == NYIR_VEC8_SELECT_I64) {
      if (!(caps & NY_NATIVE_CAP_MACH_AVX2)) {
        nyr_err(err, err_len, "machine lower: vec8 select requires AVX2");
        goto done;
      }
      inst.opcode = NYR_MACH_SELECT;
      inst.condition = NYR_MACH_COND_NE;
      inst.dst = mach_vreg(&lowered, values[in->dst]);
      inst.src0 = mach_vreg(&lowered, values[in->a]);
      inst.src1 = mach_vreg(&lowered, values[in->b]);
      inst.src2 = mach_vreg(&lowered, values[in->c]);
    } else if (in->op == NYIR_VEC4_REDUCE_ADD_F64 ||
               in->op == NYIR_VEC4_REDUCE_ADD_I64 ||
               in->op == NYIR_VEC8_REDUCE_ADD_I64) {
      inst.opcode = NYR_MACH_REDUCE_ADD;
      inst.dst = mach_vreg(&lowered, values[in->dst]);
      inst.src0 = mach_vreg(&lowered, values[in->a]);
      inst.src1 = mach_vreg(&lowered, values[in->b]);
    } else if (in->op == NYIR_VEC4_ADD_I64 || in->op == NYIR_VEC4_SUB_I64 ||
               in->op == NYIR_VEC4_AND_I64 || in->op == NYIR_VEC4_OR_I64 ||
               in->op == NYIR_VEC4_XOR_I64 || in->op == NYIR_VEC4_ADD_F64 ||
               in->op == NYIR_VEC4_SUB_F64 || in->op == NYIR_VEC4_MUL_F64 ||
               in->op == NYIR_VEC4_DIV_F64 || in->op == NYIR_VEC8_ADD_F32 ||
               in->op == NYIR_VEC8_SUB_F32 || in->op == NYIR_VEC8_MUL_F32 ||
               in->op == NYIR_VEC8_DIV_F32 || in->op == NYIR_VEC4_SHL_I64 ||
               in->op == NYIR_VEC4_SAR_I64 || in->op == NYIR_VEC8_ADD_I64 ||
               in->op == NYIR_VEC8_SUB_I64 || in->op == NYIR_VEC8_AND_I64 ||
               in->op == NYIR_VEC8_OR_I64 || in->op == NYIR_VEC8_XOR_I64 ||
               in->op == NYIR_VEC8_MUL_I64) {
      /*
       * Packed vector ALU: one machine op, element type on the vreg.
       */
      if (in->op == NYIR_VEC4_SUB_I64 || in->op == NYIR_VEC4_SUB_F64 ||
          in->op == NYIR_VEC8_SUB_I64 || in->op == NYIR_VEC8_SUB_F32)
        inst.opcode = NYR_MACH_SUB;
      else if (in->op == NYIR_VEC4_MUL_F64 || in->op == NYIR_VEC8_MUL_F32)
        inst.opcode = NYR_MACH_MUL;
      else if (in->op == NYIR_VEC4_DIV_F64 || in->op == NYIR_VEC8_DIV_F32)
        inst.opcode = NYR_MACH_DIV;
      else if (in->op == NYIR_VEC4_AND_I64)
        inst.opcode = NYR_MACH_AND;
      else if (in->op == NYIR_VEC4_OR_I64)
        inst.opcode = NYR_MACH_OR;
      else if (in->op == NYIR_VEC4_XOR_I64)
        inst.opcode = NYR_MACH_XOR;
      else if (in->op == NYIR_VEC8_AND_I64)
        inst.opcode = NYR_MACH_AND;
      else if (in->op == NYIR_VEC8_OR_I64)
        inst.opcode = NYR_MACH_OR;
      else if (in->op == NYIR_VEC8_XOR_I64)
        inst.opcode = NYR_MACH_XOR;
      else if (in->op == NYIR_VEC8_MUL_I64) {
        inst.opcode = NYR_MACH_MUL;
        inst.unsigned_arith = (in->imm == 1);
      } else if (in->op == NYIR_VEC4_SHL_I64)
        inst.opcode = NYR_MACH_SHL;
      else if (in->op == NYIR_VEC4_SAR_I64)
        inst.opcode = NYR_MACH_SAR;
      else
        inst.opcode = NYR_MACH_ADD;
      inst.dst = mach_vreg(&lowered, values[in->dst]);
      inst.src0 = mach_vreg(&lowered, values[in->a]);
      inst.src1 = mach_vreg(&lowered, values[in->b]);
    } else if (in->op == NYIR_VEC4_FMA_F64 || in->op == NYIR_VEC8_FMA_F32) {
      if (in->op == NYIR_VEC4_FMA_F64 && !(caps & NY_NATIVE_CAP_MACH_FMA)) {
        nyr_err(err, err_len,
                "machine lower: four-lane f64 FMA requires a fused-multiply-"
                "add target; refusing the two-rounding mul+add substitute");
        goto done;
      }
      inst.opcode = NYR_MACH_FMA;
      inst.dst = mach_vreg(&lowered, values[in->dst]);
      inst.src0 = mach_vreg(&lowered, values[in->a]);
      inst.src1 = mach_vreg(&lowered, values[in->b]);
      inst.src2 = mach_vreg(&lowered, values[in->c]);
    } else if (in->op == NYIR_VEC4_SHUFFLE_F64 ||
               in->op == NYIR_VEC8_SHUFFLE_F32) {
      inst.opcode = NYR_MACH_SHUFFLE;
      inst.dst = mach_vreg(&lowered, values[in->dst]);
      inst.src0 = mach_vreg(&lowered, values[in->a]);
      inst.src1 =
          (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_IMM, .as.imm = in->imm};
    } else if (in->op == NYIR_VEC4_LOAD_I64 || in->op == NYIR_VEC4_LOAD_F64 ||
               in->op == NYIR_VEC8_LOAD_F32 || in->op == NYIR_VEC8_LOAD_I64) {
      inst.opcode = NYR_MACH_LOAD;
      inst.dst = mach_vreg(&lowered, values[in->dst]);
      inst.src0 = mach_vreg(&lowered, values[in->a]);
      inst.effects = NYR_MACH_EFFECT_READ_MEMORY;
    } else if (in->op == NYIR_VEC4_STORE_I64 || in->op == NYIR_VEC4_STORE_F64 ||
               in->op == NYIR_VEC8_STORE_F32 || in->op == NYIR_VEC8_STORE_I64) {
      inst.opcode = NYR_MACH_STORE;
      inst.dst = mach_vreg(&lowered, values[in->a]);
      inst.src0 = mach_vreg(&lowered, values[in->b >= 0 ? in->b : in->c]);
      inst.effects = NYR_MACH_EFFECT_WRITE_MEMORY;
    } else if (in->op == NYIR_VEC4_SET1_F64 || in->op == NYIR_VEC8_SET1_F32 ||
               in->op == NYIR_VEC4_SET1_I64 || in->op == NYIR_VEC8_SET1_I64) {
      /*
       * Broadcast scalar → packed vector via COPY; encoder expands.
       */
      inst.opcode = NYR_MACH_COPY;
      inst.dst = mach_vreg(&lowered, values[in->dst]);
      inst.src0 = mach_vreg(&lowered, values[in->a]);
    } else if (in->op == NYIR_BOUNDS_CHECK) {
      if (!(caps & NY_NATIVE_CAP_MACH_TRAP)) {
        nyr_err(
            err, err_len,
            "machine lower: NYIR opcode %s requires machine trap capability",
            nyr_op_name(in->op));
        goto done;
      }
      /*
       * Compare offset against byte_len; branch to trap block on violation.
       * .a=base(ignored here), .b=offset(vreg), .imm=byte_len
       */
      nyr_mach_reg_t len_vreg;
      if (!nyr_mach_alloc_typed_vreg(&lowered, NYR_MACH_TYPE_I64, &len_vreg))
        goto oom;
      nyr_mach_inst_t copy_len = {
          .opcode = NYR_MACH_COPY,
          .source_pc = (uint32_t)i,
          .dst = mach_vreg(&lowered, len_vreg),
          .src0 = in->c >= 0
                      ? mach_vreg(&lowered, values[in->c])
                      : (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_IMM,
                                             .as.imm = in->imm}};
      if (!nyr_mach_emit(&lowered, copy_len))
        goto oom;
      nyr_mach_reg_t cmp_vreg;
      if (!nyr_mach_alloc_typed_vreg(&lowered, NYR_MACH_TYPE_I64, &cmp_vreg))
        goto oom;
      nyr_mach_inst_t cmp = {.opcode = NYR_MACH_CMP,
                             .source_pc = (uint32_t)i,
                             .dst = mach_vreg(&lowered, cmp_vreg),
                             .src0 = mach_vreg(&lowered, values[in->b]),
                             .src1 = mach_vreg(&lowered, len_vreg),
                             .condition = NYR_MACH_COND_GE};
      if (!nyr_mach_emit(&lowered, cmp))
        goto oom;
      /*
       * Bounds checks are emitted while the source block layout is still
       * being lowered.  Their target is patched to one trap block appended
       * after every source block, so it cannot perturb source CFG order.
       * Register allocation runs only after this complete machine function
       * has been verified; the trap has no value operands and therefore adds
       * no live range.
       */
      nyr_mach_inst_t br = {.opcode = NYR_MACH_BR_IF,
                            .source_pc = (uint32_t)i,
                            .src0 = mach_vreg(&lowered, cmp_vreg),
                            .src1 = {.kind = NYR_MACH_OPERAND_BLOCK,
                                     .as.block_index = UINT32_MAX},
                            .effects = NYR_MACH_EFFECT_CONTROL};
      if (!nyr_mach_emit(&lowered, br))
        goto oom;
      has_bounds_checks = true;
      continue;
    } else if (in->op == NYIR_NOP) {
      continue;
    } else {
      nyr_err(err, err_len, "machine lower: unsupported NYIR opcode %s",
              nyr_op_name(in->op));
      goto done;
    }
    if (inst.opcode == NYR_MACH_ADD || inst.opcode == NYR_MACH_SUB ||
        inst.opcode == NYR_MACH_MUL || inst.opcode == NYR_MACH_DIV ||
        inst.opcode == NYR_MACH_CMP) {
      nyr_mach_type_t t0 = mach_value_type(&lowered, &inst.src0);
      nyr_mach_type_t t1 = mach_value_type(&lowered, &inst.src1);
      if (mach_type_is_float(t0) &&
          mach_is_integer_value(&lowered, &inst.src1)) {
        nyr_mach_reg_t conv_reg;
        if (!nyr_mach_alloc_typed_vreg(&lowered, t0, &conv_reg))
          goto oom;
        nyr_mach_inst_t conv = {.opcode = NYR_MACH_CONVERT,
                                .source_pc = (uint32_t)i,
                                .dst = mach_vreg(&lowered, conv_reg),
                                .src0 = inst.src1};
        if (!nyr_mach_emit(&lowered, conv))
          goto oom;
        inst.src1 = mach_vreg(&lowered, conv_reg);
      } else if (mach_type_is_float(t1) &&
                 mach_is_integer_value(&lowered, &inst.src0)) {
        nyr_mach_reg_t conv_reg;
        if (!nyr_mach_alloc_typed_vreg(&lowered, t1, &conv_reg))
          goto oom;
        nyr_mach_inst_t conv = {.opcode = NYR_MACH_CONVERT,
                                .source_pc = (uint32_t)i,
                                .dst = mach_vreg(&lowered, conv_reg),
                                .src0 = inst.src0};
        if (!nyr_mach_emit(&lowered, conv))
          goto oom;
        inst.src0 = mach_vreg(&lowered, conv_reg);
      }
    }
    if (!mach_verify_opcode_shape(&lowered, &inst)) {
      const char *a_def = "none";
      if (in->a >= 0 && in->a < nyir->next_value && definitions) {
        int di = definitions[in->a];
        if (di >= 0)
          a_def = nyr_op_name(nyir->data[di].op);
      }
      const char *file = in->debug.file ? in->debug.file : "<unknown>";
      if (ny_trace_enabled("NY_TRACE_MACHINE_FAIL"))
        nyr_dump_compact(stderr, nyir, "machine-fail");
      nyr_err(err, err_len,
              "machine lower: NYIR instruction %zu (%s v%d v%d -> v%d) has "
              "invalid machine shape"
              " [dst=%s src0=%s src1=%s a_def=%s at %s:%d]",
              i, nyr_op_name(in->op), in->a, in->b, in->dst,
              mach_type_name(mach_value_type(&lowered, &inst.dst)),
              mach_type_name(mach_value_type(&lowered, &inst.src0)),
              mach_type_name(mach_value_type(&lowered, &inst.src1)), a_def,
              file, in->debug.line);
      free(inst.args);
      free(inst.arg_sizes);
      goto done;
    }
    if (!nyr_mach_emit(&lowered, inst)) {
      free(inst.args);
      free(inst.arg_sizes);
      goto oom;
    }
  }
  if (!mach_lower_phi_edges(nyir, &cfg, values, &lowered, err, err_len))
    goto done;
  if (has_bounds_checks) {
    uint32_t trap_block_idx = 0;
    if (!nyr_mach_begin_block(&lowered, UINT32_MAX, &trap_block_idx) ||
        trap_block_idx != (uint32_t)lowered.block_len - 1) {
      nyr_err(err, err_len,
              "machine lower: failed to create bounds-trap block");
      goto done;
    }
    /*
     * Patch all sentinel block references to the real trap block.
     */
    for (size_t ii = 0; ii < lowered.inst_len; ++ii) {
      if (lowered.insts[ii].opcode == NYR_MACH_BR_IF &&
          lowered.insts[ii].src1.kind == NYR_MACH_OPERAND_BLOCK &&
          lowered.insts[ii].src1.as.block_index == UINT32_MAX)
        lowered.insts[ii].src1.as.block_index = trap_block_idx;
    }
    /*
     * The trap block calls the runtime helper so a failed bounds check
     * panics with the shared message instead of terminating via a bare
     * trap instruction; the TRAP stays as the unreachable terminator so
     * machine-form verification keeps a well-formed CFG.
     */
    nyr_mach_inst_t trap_call = {.opcode = NYR_MACH_CALL,
                                 .source_pc = UINT32_MAX,
                                 .src0 = {.kind = NYR_MACH_OPERAND_SYMBOL,
                                          .as.symbol = "rt_bounds_trap"},
                                 .call_is_extern = 1,
                                 .effects = NYR_MACH_EFFECT_CALL};
    if (!nyr_mach_emit(&lowered, trap_call))
      goto oom;
    nyr_mach_inst_t trap = {.opcode = NYR_MACH_TRAP,
                            .source_pc = UINT32_MAX,
                            .effects = NYR_MACH_EFFECT_CONTROL};
    if (!nyr_mach_emit(&lowered, trap))
      goto oom;
    /*
     * Bounds-check traps are lowered to CMP+BR_IF+TRAP for backends
     * with NY_NATIVE_CAP_MACH_TRAP (x86-64 ud2, AArch64 brk #0).
     * Other backends skip the machine-form lowering and retain the
     * NYIR VM guard path (eval.c bounds_violation).
     */
  }
  if (!mach_merge_fallthrough_blocks(&lowered))
    goto oom;
  if (!nyr_mach_verify(&lowered, 0, err, err_len)) {
    if (ny_trace_enabled("NY_TRACE_MACHINE_FAIL")) {
      ny_trace_line(
          "MACHINE_FAIL",
          "machine IR verification failed after fallthrough merge: %s",
          err && err[0] ? err : "unknown error");
      nyr_mach_dump(&lowered, stderr);
    }
    goto done;
  }
  /*
   * Publish only the scheduled stream. Register allocation is constructed by
   * encoders after this function returns, so no position-sensitive allocation
   * view exists while `lowered` is still mutable.
   */
  if (!nyr_mach_schedule_post_ra(&lowered))
    goto oom;
  mach_propagate_local_copies(&lowered);
  mach_propagate_single_pred_copies(&lowered);
  if (!mach_remove_unused_literals(&lowered))
    goto oom;
  mach_compact_nops(&lowered);
  if (!mach_sink_edge_adds(&lowered))
    goto oom;
  if (!mach_rotate_scalar_loops(&lowered))
    goto oom;
  if (!mach_rotate_chained_loops(&lowered))
    goto oom;
  if (nyir->untagged_values)
    mach_fuse_int_widening(&lowered);
  mach_coalesce_copies(&lowered, nyir);
  if (!mach_split_around_loops(&lowered))
    goto oom;
  if (!nyr_mach_verify(&lowered, 0, err, err_len)) {
    if (ny_trace_enabled("NY_TRACE_MACHINE_FAIL")) {
      ny_trace_line("MACHINE_FAIL",
                    "machine IR verification failed after post-RA schedule: %s",
                    err && err[0] ? err : "unknown error");
      nyr_mach_dump(&lowered, stderr);
    }
    goto done;
  }
  mach_color_local_homes(&lowered);
  nyr_mach_func_free(out);
  *out = lowered;
  lowered = (nyr_mach_func_t){0};
  ok = true;
done:
  free(definitions);
  free(fused_dividend);
  free(fused_popcount);
  free(fused_divisor);
  free(fused_lea_base);
  free(fused_lea_index);
  free(fused_lea_scale);
  free(skip_fused_inst);
  nyr_use_def_free(&uses);
  free(values);
  free(pointer_values);
  free(label_blocks);
  free(local_address_taken);
  nyr_type_map_free(&types);
  nyr_cfg_free(&cfg);
  nyr_mach_func_free(&lowered);
  return ok;
oom:
  nyr_err(err, err_len, "machine lower: out of memory");
  goto done;
}
/*
 * This virtual-register scheduling pass runs before register allocation. It
 * reorders only side-effect-free operations with modeled value dependencies.
 * Memory, control flow, calls, flags, and physical-register uses split blocks
 * into independent segments; fixed-register operations are admitted only
 * when their implicit clobbers are modeled by register allocation.
 */

static bool mach_schedule_value_equal(const nyr_mach_operand_t *left,
                                      const nyr_mach_operand_t *right) {
  return left && right && mach_is_value(left) && mach_is_value(right) &&
         left->kind == right->kind && left->as.reg == right->as.reg &&
         left->reg_class == right->reg_class;
}

static bool mach_schedule_operand_is_fixed(const nyr_mach_operand_t *operand) {
  /*
   * PREG is intentionally treated as a fixed-register barrier.  Machine form
   * does not carry target register allocation metadata, so assuming that a
   * physical register is freely renameable would make shifts using %cl,
   * implicit div/mod registers, and ABI sequences reorderable by accident.
   */
  return operand && (operand->kind == NYR_MACH_OPERAND_PREG ||
                     operand->reg_class == NYR_MACH_REGCLASS_FLAGS);
}

static bool mach_schedule_inst_has_fixed_operand(const nyr_mach_inst_t *inst) {
  if (!inst)
    return true;
  const nyr_mach_operand_t *ops[] = {&inst->dst, &inst->src0, &inst->src1,
                                     &inst->src2};
  for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); ++i)
    if (mach_schedule_operand_is_fixed(ops[i]))
      return true;
  for (size_t i = 0; i < inst->args_len; ++i)
    if (mach_schedule_operand_is_fixed(&inst->args[i]))
      return true;
  return false;
}

static bool mach_schedule_opcode_allowed(nyr_mach_opcode_t opcode) {
  switch (opcode) {
  case NYR_MACH_NOP:
  case NYR_MACH_COPY:
  case NYR_MACH_CONVERT:
  case NYR_MACH_LEA:
  case NYR_MACH_ADD:
  case NYR_MACH_SUB:
  case NYR_MACH_MUL:
  case NYR_MACH_DIV:
  case NYR_MACH_AND:
  case NYR_MACH_OR:
  case NYR_MACH_XOR:
  case NYR_MACH_SHL:
  case NYR_MACH_SAR:
  case NYR_MACH_SHR:
  case NYR_MACH_ROR:
  case NYR_MACH_ROR32:
  case NYR_MACH_BSWAP64:
  case NYR_MACH_BSWAP32:
  case NYR_MACH_POPCOUNT64:
  case NYR_MACH_FMA:
  case NYR_MACH_SQRT:
  case NYR_MACH_SIN:
  case NYR_MACH_COS:
  case NYR_MACH_SHUFFLE:
  case NYR_MACH_SELECT:
  case NYR_MACH_REDUCE_ADD:
    return true;
  default:
    return false;
  }
}

static bool mach_schedule_inst_safe(const nyr_mach_func_t *func,
                                    const nyr_mach_inst_t *inst) {
  if (!inst || !mach_schedule_opcode_allowed(inst->opcode) ||
      inst->effects != NYR_MACH_EFFECT_NONE || inst->args_len != 0 ||
      inst->args != NULL || inst->arg_sizes != NULL ||
      inst->call_clobbers != 0 || inst->call_is_extern || inst->call_has_sret ||
      inst->call_is_variadic || mach_schedule_inst_has_fixed_operand(inst) ||
      inst->source_pc == UINT32_MAX)
    return false;
  /*
   * Constant signed division is safe to move only when it cannot trap:
   * division by zero and INT64_MIN / -1 remain ordering barriers. The
   * register allocator models the fixed-register clobbers after scheduling.
   */
  if (inst->opcode == NYR_MACH_SHL || inst->opcode == NYR_MACH_SAR ||
      inst->opcode == NYR_MACH_SHR || inst->opcode == NYR_MACH_ROR ||
      inst->opcode == NYR_MACH_ROR32)
    return inst->src1.kind == NYR_MACH_OPERAND_IMM;
  if (inst->opcode == NYR_MACH_DIV)
    return mach_value_type(func, &inst->dst) == NYR_MACH_TYPE_I64 &&
           inst->src1.kind == NYR_MACH_OPERAND_IMM && inst->src1.as.imm != 0 &&
           inst->src1.as.imm != -1;
  /*
   * Synthetic edge copies have already been ordered as a parallel-copy
   * sequence. Their virtual registers can represent successive values of a
   * loop PHI, so ordinary SSA read/write dependencies do not preserve the
   * snapshot-before-overwrite order.
   *
   * Shift-count lowering may select %cl even for a virtual source. Without
   * target constraint metadata, retain every shift block unchanged.
   */
  return inst->opcode != NYR_MACH_CMP && inst->opcode != NYR_MACH_MOD &&
         inst->opcode != NYR_MACH_SHL && inst->opcode != NYR_MACH_SAR &&
         inst->opcode != NYR_MACH_SHR && !inst->unsigned_arith &&
         inst->opcode != NYR_MACH_ROR && inst->opcode != NYR_MACH_ROR32;
}

static bool mach_schedule_inst_reads(const nyr_mach_inst_t *inst,
                                     const nyr_mach_operand_t *value) {
  if (!inst || !value)
    return false;
  const nyr_mach_operand_t *ops[] = {&inst->src0, &inst->src1, &inst->src2};
  for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); ++i)
    if (mach_schedule_value_equal(ops[i], value))
      return true;
  for (size_t i = 0; i < inst->args_len; ++i)
    if (mach_schedule_value_equal(&inst->args[i], value))
      return true;
  return false;
}

static bool mach_schedule_inst_writes(const nyr_mach_inst_t *inst,
                                      const nyr_mach_operand_t *value) {
  return inst && value && mach_schedule_value_equal(&inst->dst, value);
}

static unsigned mach_schedule_latency(nyr_mach_opcode_t opcode) {
  /*
   * These are intentionally coarse x86-64-oriented classes.  The scheduler
   * only uses them to prefer a ready long-latency producer; correctness comes
   * solely from the dependency graph and barriers.
   */
  switch (opcode) {
  case NYR_MACH_MUL:
  case NYR_MACH_FMA:
    return 3;
  case NYR_MACH_DIV:
    return 12;
  case NYR_MACH_CONVERT:
    return 3;
  case NYR_MACH_SQRT:
    return 12;
  case NYR_MACH_SIN:
  case NYR_MACH_COS:
    return 20;
  case NYR_MACH_SHUFFLE:
  case NYR_MACH_REDUCE_ADD:
    return 3;
  case NYR_MACH_COPY:
  case NYR_MACH_LEA:
    return 1;
  default:
    return 1;
  }
}

static size_t mach_schedule_operand_uses(const nyr_mach_inst_t *inst,
                                         const nyr_mach_operand_t *value) {
  if (!inst || !value)
    return 0;
  size_t uses = 0;
  const nyr_mach_operand_t *ops[] = {&inst->src0, &inst->src1, &inst->src2};
  for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); ++i)
    if (mach_schedule_value_equal(ops[i], value))
      ++uses;
  for (size_t i = 0; i < inst->args_len; ++i)
    if (mach_schedule_value_equal(&inst->args[i], value))
      ++uses;
  return uses;
}

/*
 * Delay single-use register additions and subtractions until their PHI-edge
 * copy. Computing an induction update after the old value's final use permits
 * the encoder to coalesce the update with its loop-home register.
 * Machine registers may have multiple definitions, so check every crossed
 * instruction for input redefinitions as well as effects and control flow.
 */
static bool mach_sink_edge_adds(nyr_mach_func_t *m) {
  size_t sunk = 0, sunk_across_call = 0;
  size_t *defs = calloc(m->vreg_len, sizeof(*defs));
  size_t *uses = calloc(m->vreg_len, sizeof(*uses));
  if (m->vreg_len && (!defs || !uses)) {
    free(defs);
    free(uses);
    return false;
  }
  for (size_t pc = 0; pc < m->inst_len; ++pc) {
    const nyr_mach_inst_t *in = &m->insts[pc];
    if (in->opcode != NYR_MACH_STORE && in->dst.kind == NYR_MACH_OPERAND_VREG)
      ++defs[in->dst.as.reg];
    const nyr_mach_operand_t *ops[] = {&in->src0, &in->src1, &in->src2};
    for (size_t n = 0; n < 3; ++n)
      if (ops[n]->kind == NYR_MACH_OPERAND_VREG)
        ++uses[ops[n]->as.reg];
    if (in->opcode == NYR_MACH_STORE && in->dst.kind == NYR_MACH_OPERAND_VREG)
      ++uses[in->dst.as.reg];
    for (size_t n = 0; n < in->args_len; ++n)
      if (in->args[n].kind == NYR_MACH_OPERAND_VREG)
        ++uses[in->args[n].as.reg];
  }
  for (size_t b = 0; b < m->block_len; ++b) {
    const nyr_mach_block_t *block = &m->blocks[b];
    size_t end = block->first_inst + block->inst_count;
    for (size_t pc = end; pc > block->first_inst;) {
      --pc;
      nyr_mach_inst_t add = m->insts[pc];
      if ((add.opcode != NYR_MACH_ADD && add.opcode != NYR_MACH_SUB) ||
          add.narrow32 || !mach_schedule_inst_safe(m, &add) ||
          add.dst.kind != NYR_MACH_OPERAND_VREG ||
          add.dst.reg_class != NYR_MACH_REGCLASS_GPR ||
          mach_value_type(m, &add.dst) != NYR_MACH_TYPE_I64 ||
          defs[add.dst.as.reg] != 1 || uses[add.dst.as.reg] != 1)
        continue;
      /*
       * Bound both the search and the eventual memmove. The pass is a
       * scheduling heuristic; distant consumers may keep source order.
       * Together with the linear use census, this avoids quadratic work
       * in large generated blocks.
       */
      bool crossed_call = false;
      for (size_t next = pc + 1; next < end && next - pc <= 64; ++next) {
        const nyr_mach_inst_t *in = &m->insts[next];
        if (in->opcode == NYR_MACH_COPY && in->source_pc == UINT32_MAX &&
            mach_schedule_value_equal(&in->src0, &add.dst)) {
          if (next > pc + 1) {
            memmove(&m->insts[pc], &m->insts[pc + 1],
                    (next - pc - 1) * sizeof(*m->insts));
            m->insts[next - 1] = add;
            ++sunk;
            sunk_across_call += crossed_call;
          }
          break;
        }
        bool crosses_call = !crossed_call && in->opcode == NYR_MACH_CALL &&
                            add.src0.kind == NYR_MACH_OPERAND_VREG &&
                            mach_schedule_inst_reads(in, &add.src0);
        if (crosses_call && add.src1.kind != NYR_MACH_OPERAND_IMM) {
          /*
           * Only move a second operand that the encoder can rematerialize.
           */
          crosses_call = false;
          for (size_t def = pc; def > block->first_inst && pc - def < 64;) {
            const nyr_mach_inst_t *prior = &m->insts[--def];
            if (!mach_schedule_inst_writes(prior, &add.src1))
              continue;
            if (prior->opcode == NYR_MACH_COPY &&
                prior->src0.kind == NYR_MACH_OPERAND_IMM &&
                prior->src0.relocation == NYR_MACH_RELOC_NONE) {
              crosses_call = true;
            }
            break;
          }
        }
        /*
         * This move preserves the order of the edge copies themselves. It
         * can cross a sibling copy provided neither input is overwritten.
         */
        bool crosses_edge_copy = in->opcode == NYR_MACH_COPY &&
                                 in->source_pc == UINT32_MAX &&
                                 in->effects == NYR_MACH_EFFECT_NONE &&
                                 in->dst.kind == NYR_MACH_OPERAND_VREG &&
                                 (in->src0.kind == NYR_MACH_OPERAND_VREG ||
                                  in->src0.kind == NYR_MACH_OPERAND_IMM);
        if ((!crosses_call && !crosses_edge_copy &&
             !mach_schedule_inst_safe(m, in)) ||
            mach_schedule_inst_reads(in, &add.dst) ||
            mach_schedule_inst_writes(in, &add.src0) ||
            mach_schedule_inst_writes(in, &add.src1) ||
            mach_schedule_inst_writes(in, &add.src2))
          break;
        crossed_call |= crosses_call;
      }
    }
  }
  if (sunk && ny_dbg_is_enabled(NY_DBG_HEURISTIC))
    ny_dbg_heuristic_event("MACHINE_EDGE_ADD_SINK",
                           "moved=%zu across_call=%zu", sunk,
                           sunk_across_call);
  free(defs);
  free(uses);
  return true;
}

static bool mach_schedule_segment(nyr_mach_inst_t *insts, size_t count,
                                  const nyr_mach_operand_t *prev_def) {
  if (!insts || count < 2)
    return true;

  /*
   * Scheduling is a profitability heuristic, not a legality requirement.
   * The current dependency builder is intentionally dense (O(n^2) edges and
   * scans), which is useful for ordinary basic blocks but pathological for a
   * large straight-line region.  Leaving such a region in its original order
   * is always valid and avoids quadratic compile time and memory growth.
   */
  enum { MACH_SCHEDULE_MAX_SEGMENT_INSTS = 512 };
  if (count > MACH_SCHEDULE_MAX_SEGMENT_INSTS)
    return true;

  if (count > SIZE_MAX / count)
    return false;
  const size_t edge_count = count * count;
  unsigned char *edges = calloc(edge_count, sizeof(*edges));
  unsigned *indegree = calloc(count, sizeof(*indegree));
  unsigned *critical = calloc(count, sizeof(*critical));
  bool *selected = calloc(count, sizeof(*selected));
  size_t *order = malloc(count * sizeof(*order));
  size_t *src0_user = malloc(count * sizeof(*src0_user));
  size_t *single_user = malloc(count * sizeof(*single_user));
  unsigned *local_uses = calloc(count, sizeof(*local_uses));
  unsigned *rem_uses = calloc(count, sizeof(*rem_uses));
  nyr_mach_inst_t *ordered = malloc(count * sizeof(*ordered));
  if (!edges || !indegree || !critical || !selected || !order || !src0_user ||
      !single_user || !local_uses || !rem_uses || !ordered) {
    free(edges);
    free(indegree);
    free(critical);
    free(selected);
    free(order);
    free(src0_user);
    free(single_user);
    free(local_uses);
    free(rem_uses);
    free(ordered);
    return false;
  }

  /*
   * Preserve every value dependency and every write/write ordering.  The
   * original order is otherwise not a dependency, which is what lets the
   * ready-list choose independent work from later in the block.
   */
  unsigned prev_rem_uses = 0;
  bool track_prev = prev_def && mach_is_value(prev_def);
  for (size_t i = 0; i < count; ++i) {
    if (track_prev) {
      if (mach_schedule_inst_reads(&insts[i], prev_def))
        ++prev_rem_uses;
      if (mach_schedule_inst_writes(&insts[i], prev_def))
        track_prev = false;
    }
    src0_user[i] = SIZE_MAX;
    single_user[i] = SIZE_MAX;
    size_t user = SIZE_MAX, uses = 0;
    for (size_t j = i + 1; j < count; ++j) {
      bool dependency = false;
      bool reads_i = false;
      if (mach_is_value(&insts[i].dst)) {
        reads_i = mach_schedule_inst_reads(&insts[j], &insts[i].dst);
        dependency = reads_i || mach_schedule_inst_writes(&insts[j], &insts[i].dst);
      }
      if (!dependency && mach_is_value(&insts[j].dst))
        dependency = mach_schedule_inst_reads(&insts[i], &insts[j].dst);
      if (dependency) {
        edges[i * count + j] = 1;
        ++indegree[j];
      }
      if (reads_i)
        ++local_uses[i];
      if (uses < 2 && mach_is_value(&insts[i].dst)) {
        size_t found = mach_schedule_operand_uses(&insts[j], &insts[i].dst);
        if (found) {
          uses += found;
          user = j;
        }
      }
    }
    rem_uses[i] = local_uses[i];
    if (!mach_is_value(&insts[i].dst))
      continue;
    if (local_uses[i] == 1 && user != SIZE_MAX)
      single_user[i] = user;
    if (uses == 1 && user != SIZE_MAX &&
        mach_schedule_value_equal(&insts[user].src0, &insts[i].dst))
      src0_user[i] = user;
  }

  for (size_t i = count; i-- > 0;) {
    unsigned latency = mach_schedule_latency(insts[i].opcode);
    unsigned successor = 0;
    for (size_t j = i + 1; j < count; ++j)
      if (edges[i * count + j] && critical[j] > successor)
        successor = critical[j];
    critical[i] = latency + successor;
  }

  int live_temps = 0;
  bool pressure_mode = count >= 4;
  for (size_t out = 0; out < count; ++out) {
    size_t best = SIZE_MAX;
    int best_delta = 1000;
    /*
     * Priority 1: If the barrier instruction right before this segment (e.g. a
     * load of w[i] in SHA-256) produced a value with a single remaining read in
     * this segment, consume it immediately so its scratch register is freed
     * before an unrelated expression tree begins.
     */
    if (pressure_mode && prev_def && prev_rem_uses == 1) {
      for (size_t i = 0; i < count; ++i) {
        if (selected[i] || indegree[i] != 0)
          continue;
        if (!mach_schedule_inst_reads(&insts[i], prev_def))
          continue;
        if (best == SIZE_MAX || critical[i] > critical[best] ||
            (critical[i] == critical[best] && i < best))
          best = i;
      }
    }
    /*
     * Priority 2: When register pressure is building inside a basic block (such
     * as SHA-256 rounds with 9+ live loop PHIs), prefer ready instructions that
     * reduce live temporaries (negative net delta) before opening a new tree.
     */
    if (best == SIZE_MAX && pressure_mode && live_temps >= 1) {
      for (size_t i = 0; i < count; ++i) {
        if (selected[i] || indegree[i] != 0)
          continue;
        int delta = local_uses[i] > 0 ? 1 : 0;
        for (size_t p = 0; p < i; ++p)
          if (selected[p] && rem_uses[p] == 1 &&
              mach_is_value(&insts[p].dst) &&
              mach_schedule_inst_reads(&insts[i], &insts[p].dst))
            --delta;
        if (delta < 0 &&
            (best == SIZE_MAX || delta < best_delta ||
             (delta == best_delta && critical[i] > critical[best]) ||
             (delta == best_delta && critical[i] == critical[best] &&
              i < best))) {
          best = i;
          best_delta = delta;
        }
      }
    }
    /*
     * Priority 3 (Sethi-Ullman tree completion): When at least one local temp
     * is already live, prefer a ready instruction whose sole consumer is only
     * waiting on it and will retire an already-live temp (e.g. ror(a,22) before
     * starting Maj), rather than opening a second concurrent expression tree.
     */
    if (best == SIZE_MAX && pressure_mode && live_temps >= 1) {
      for (size_t i = 0; i < count; ++i) {
        if (selected[i] || indegree[i] != 0)
          continue;
        size_t u = single_user[i];
        if (u == SIZE_MAX || selected[u] || indegree[u] != 1)
          continue;
        bool kills_live = false;
        for (size_t p = 0; p < u; ++p)
          if (selected[p] && rem_uses[p] == 1 &&
              mach_is_value(&insts[p].dst) &&
              mach_schedule_inst_reads(&insts[u], &insts[p].dst)) {
            kills_live = true;
            break;
          }
        if (kills_live &&
            (best == SIZE_MAX || critical[i] > critical[best] ||
             (critical[i] == critical[best] && i < best)))
          best = i;
      }
    }
    /*
     * Keep a single-use source-0 value live in %rax through its consumer.
     * Wait to issue that producer until the consumer's other inputs are ready,
     * then force the consumer into the immediately following slot.
     */
    if (best == SIZE_MAX) {
      for (size_t i = 0; i < count; ++i) {
        if (selected[i] || indegree[i] != 0 || src0_user[i] == SIZE_MAX)
          continue;
        size_t user = src0_user[i];
        if (indegree[user] == 1 &&
            (best == SIZE_MAX || critical[i] > critical[best] ||
             (critical[i] == critical[best] && i < best)))
          best = i;
      }
    }
    if (best == SIZE_MAX) {
      size_t deferred = SIZE_MAX;
      for (size_t i = 0; i < count; ++i) {
        if (selected[i] || indegree[i] != 0)
          continue;
        size_t user = src0_user[i];
        if (user != SIZE_MAX && indegree[user] > 1) {
          if (deferred == SIZE_MAX || critical[i] > critical[deferred])
            deferred = i;
          continue;
        }
        if (best == SIZE_MAX || critical[i] > critical[best] ||
            (critical[i] == critical[best] && i < best))
          best = i;
      }
      /*
       * A dependency cycle cannot be introduced by the deferral rule: if it
       * is the only ready work, emit the highest critical deferred producer.
       */
      if (best == SIZE_MAX)
        best = deferred;
    }
    if (best == SIZE_MAX) {
      free(edges);
      free(indegree);
      free(critical);
      free(selected);
      free(order);
      free(src0_user);
      free(single_user);
      free(local_uses);
      free(rem_uses);
      free(ordered);
      return false;
    }
    selected[best] = true;
    order[out] = best;
    ordered[out] = insts[best];
    if (prev_def && prev_rem_uses > 0 &&
        mach_schedule_inst_reads(&insts[best], prev_def))
      --prev_rem_uses;
    for (size_t p = 0; p < best; ++p)
      if (selected[p] && rem_uses[p] > 0 && mach_is_value(&insts[p].dst) &&
          mach_schedule_inst_reads(&insts[best], &insts[p].dst) &&
          --rem_uses[p] == 0)
        --live_temps;
    if (local_uses[best] > 0)
      ++live_temps;
    size_t forced_user = src0_user[best];
    for (size_t j = best + 1; j < count; ++j)
      if (edges[best * count + j])
        --indegree[j];
    if (forced_user != SIZE_MAX && indegree[forced_user] == 0 &&
        out + 1 < count && !selected[forced_user]) {
      selected[forced_user] = true;
      order[out + 1] = forced_user;
      ordered[out + 1] = insts[forced_user];
      if (prev_def && prev_rem_uses > 0 &&
          mach_schedule_inst_reads(&insts[forced_user], prev_def))
        --prev_rem_uses;
      for (size_t p = 0; p < forced_user; ++p)
        if (selected[p] && rem_uses[p] > 0 && mach_is_value(&insts[p].dst) &&
            mach_schedule_inst_reads(&insts[forced_user], &insts[p].dst) &&
            --rem_uses[p] == 0)
          --live_temps;
      if (local_uses[forced_user] > 0)
        ++live_temps;
      ++out;
      for (size_t j = forced_user + 1; j < count; ++j)
        if (edges[forced_user * count + j])
          --indegree[j];
    }
  }

  bool changed = false;
  for (size_t i = 0; i < count; ++i)
    if (order[i] != i) {
      changed = true;
      break;
    }
  if (changed) {
    for (size_t i = 0; i < count; ++i)
      insts[i] = ordered[i];
  }

  free(edges);
  free(indegree);
  free(critical);
  free(selected);
  free(order);
  free(src0_user);
  free(single_user);
  free(local_uses);
  free(rem_uses);
  free(ordered);
  return true;
}

static bool mach_schedule_block(nyr_mach_func_t *func,
                                const nyr_mach_block_t *block) {
  if (!func || !block || block->first_inst > func->inst_len ||
      block->inst_count > func->inst_len - block->first_inst ||
      block->inst_count < 2)
    return true;
  nyr_mach_inst_t *insts = func->insts + block->first_inst;
  size_t start = 0;
  while (start < block->inst_count) {
    while (start < block->inst_count &&
           !mach_schedule_inst_safe(func, &insts[start]))
      ++start;
    size_t end = start;
    while (end < block->inst_count &&
           mach_schedule_inst_safe(func, &insts[end]))
      ++end;
    const nyr_mach_operand_t *prev_def =
        (start > 0 && mach_is_value(&insts[start - 1].dst))
            ? &insts[start - 1].dst
            : NULL;
    if (!mach_schedule_segment(insts + start, end - start, prev_def))
      return false;
    start = end;
  }
  return true;
}

static uint64_t mach_outline_mix(uint64_t hash, uint64_t value) {
  hash ^= value + UINT64_C(0x9e3779b97f4a7c15) + (hash << 6) + (hash >> 2);
  return hash * UINT64_C(0x100000001b3);
}

static bool mach_outline_operand_ok(const nyr_mach_func_t *func,
                                    const nyr_mach_operand_t *operand) {
  if (!func || !operand || operand->relocation != NYR_MACH_RELOC_NONE ||
      operand->addend != 0)
    return false;
  if (operand->kind == NYR_MACH_OPERAND_NONE)
    return operand->reg_class == NYR_MACH_REGCLASS_NONE;
  if (operand->kind == NYR_MACH_OPERAND_IMM)
    return operand->reg_class == NYR_MACH_REGCLASS_NONE;
  if (operand->kind != NYR_MACH_OPERAND_VREG ||
      operand->as.reg >= func->vreg_len || !func->vreg_types ||
      !func->vreg_classes || operand->reg_class == NYR_MACH_REGCLASS_FLAGS)
    return false;
  return func->vreg_classes[operand->as.reg] == operand->reg_class &&
         func->vreg_types[operand->as.reg] != NYR_MACH_TYPE_FLAGS;
}

static bool mach_outline_inst_ok(const nyr_mach_func_t *func,
                                 const nyr_mach_inst_t *inst) {
  if (!func || !inst || inst->effects != NYR_MACH_EFFECT_NONE ||
      inst->condition != NYR_MACH_COND_ALWAYS || inst->args ||
      inst->arg_sizes || inst->args_len || inst->call_is_extern ||
      inst->call_has_sret || inst->call_is_variadic || inst->call_clobbers != 0)
    return false;
  switch (inst->opcode) {
  case NYR_MACH_COPY:
  case NYR_MACH_CONVERT:
  case NYR_MACH_ADD:
  case NYR_MACH_SUB:
  case NYR_MACH_MUL:
  case NYR_MACH_AND:
  case NYR_MACH_OR:
  case NYR_MACH_XOR:
  case NYR_MACH_SHL:
  case NYR_MACH_SAR:
  case NYR_MACH_SHR:
  case NYR_MACH_ROR:
  case NYR_MACH_ROR32:
  case NYR_MACH_VSHR:
  case NYR_MACH_BSWAP64:
  case NYR_MACH_BSWAP32:
  case NYR_MACH_POPCOUNT64:
  case NYR_MACH_FMA:
  case NYR_MACH_SQRT:
  case NYR_MACH_SIN:
  case NYR_MACH_COS:
  case NYR_MACH_SHUFFLE:
    break;
  default:
    return false;
  }
  return mach_outline_operand_ok(func, &inst->dst) &&
         mach_outline_operand_ok(func, &inst->src0) &&
         mach_outline_operand_ok(func, &inst->src1) &&
         mach_outline_operand_ok(func, &inst->src2);
}

static size_t mach_outline_body_len(const nyr_mach_func_t *func) {
  if (!func || !func->insts || !func->blocks || func->block_len != 1 ||
      func->inst_len < 2 || func->blocks[0].inst_count != func->inst_len ||
      func->insts[func->inst_len - 1].opcode != NYR_MACH_RET)
    return 0;
  for (size_t i = 0; i + 1 < func->inst_len; ++i)
    if (func->insts[i].opcode == NYR_MACH_RET)
      return 0;
  return func->inst_len - 1;
}

static bool mach_outline_operand_hash(const nyr_mach_func_t *func,
                                      const nyr_mach_operand_t *operand,
                                      uint32_t *map, uint32_t *next,
                                      uint64_t *hash) {
  if (!mach_outline_operand_ok(func, operand) || !next || !hash)
    return false;
  *hash = mach_outline_mix(*hash, operand->kind);
  *hash = mach_outline_mix(*hash, operand->reg_class);
  if (operand->kind == NYR_MACH_OPERAND_IMM) {
    *hash = mach_outline_mix(*hash, (uint64_t)operand->as.imm);
  } else if (operand->kind == NYR_MACH_OPERAND_VREG) {
    uint32_t id = operand->as.reg;
    uint32_t ordinal = map[id];
    if (ordinal == UINT32_MAX)
      map[id] = ordinal = (*next)++;
    *hash = mach_outline_mix(*hash, func->vreg_types[id]);
    *hash = mach_outline_mix(*hash, ordinal);
  }
  return true;
}

typedef struct {
  size_t body_len;
  uint64_t hash;
  bool eligible;
} nyr_mach_outline_shape_t;

static bool mach_outline_shape(const nyr_mach_func_t *func,
                               nyr_mach_outline_shape_t *out) {
  if (!out)
    return false;
  *out = (nyr_mach_outline_shape_t){0};
  size_t body_len = mach_outline_body_len(func);
  if (!body_len)
    return true;
  if (func->vreg_len > SIZE_MAX / sizeof(uint32_t))
    return false;
  uint32_t *map = func->vreg_len ? malloc(func->vreg_len * sizeof(*map)) : NULL;
  if (func->vreg_len && !map)
    return false;
  for (size_t i = 0; i < func->vreg_len; ++i)
    map[i] = UINT32_MAX;
  uint64_t hash = UINT64_C(0xcbf29ce484222325);
  uint32_t next = 0;
  for (size_t i = 0; i < body_len; ++i) {
    const nyr_mach_inst_t *inst = &func->insts[i];
    if (!mach_outline_inst_ok(func, inst)) {
      free(map);
      return true;
    }
    hash = mach_outline_mix(hash, inst->opcode);
    hash = mach_outline_mix(hash, inst->narrow32);
    hash = mach_outline_mix(hash, inst->dividend_u31);
    hash = mach_outline_mix(hash, inst->condition);
    hash = mach_outline_mix(hash, inst->byte_width);
    hash = mach_outline_mix(hash, inst->mem_size);
    hash = mach_outline_mix(hash, inst->sext32);
    hash = mach_outline_mix(hash, inst->sext_load);
    hash = mach_outline_mix(hash, inst->unsigned_arith);
    const nyr_mach_operand_t *operands[] = {&inst->dst, &inst->src0,
                                            &inst->src1, &inst->src2};
    for (size_t j = 0; j < 4; ++j)
      if (!mach_outline_operand_hash(func, operands[j], map, &next, &hash)) {
        free(map);
        return true;
      }
  }
  free(map);
  out->body_len = body_len;
  out->hash = hash;
  out->eligible = true;
  return true;
}

static bool mach_outline_operand_equal(const nyr_mach_func_t *left,
                                       const nyr_mach_operand_t *a,
                                       const nyr_mach_func_t *right,
                                       const nyr_mach_operand_t *b,
                                       uint32_t *left_map, uint32_t *right_map,
                                       uint32_t *left_next,
                                       uint32_t *right_next) {
  if (!a || !b || a->kind != b->kind || a->reg_class != b->reg_class ||
      a->relocation != b->relocation || a->addend != b->addend)
    return false;
  if (a->kind == NYR_MACH_OPERAND_NONE)
    return true;
  if (a->kind == NYR_MACH_OPERAND_IMM)
    return a->as.imm == b->as.imm;
  if (a->kind != NYR_MACH_OPERAND_VREG || !mach_outline_operand_ok(left, a) ||
      !mach_outline_operand_ok(right, b))
    return false;
  uint32_t al = left_map[a->as.reg], br = right_map[b->as.reg];
  if (al == UINT32_MAX)
    left_map[a->as.reg] = al = (*left_next)++;
  if (br == UINT32_MAX)
    right_map[b->as.reg] = br = (*right_next)++;
  return al == br &&
         left->vreg_types[a->as.reg] == right->vreg_types[b->as.reg];
}

static bool mach_outline_equal(const nyr_mach_func_t *left,
                               const nyr_mach_func_t *right, size_t body_len) {
  if (left->vreg_len > SIZE_MAX / sizeof(uint32_t) ||
      right->vreg_len > SIZE_MAX / sizeof(uint32_t))
    return false;
  uint32_t *left_map =
      left->vreg_len ? malloc(left->vreg_len * sizeof(*left_map)) : NULL;
  uint32_t *right_map =
      right->vreg_len ? malloc(right->vreg_len * sizeof(*right_map)) : NULL;
  if ((left->vreg_len && !left_map) || (right->vreg_len && !right_map)) {
    free(left_map);
    free(right_map);
    return false;
  }
  for (size_t i = 0; i < left->vreg_len; ++i)
    left_map[i] = UINT32_MAX;
  for (size_t i = 0; i < right->vreg_len; ++i)
    right_map[i] = UINT32_MAX;
  uint32_t left_next = 0, right_next = 0;
  bool equal = true;
  for (size_t i = 0; equal && i < body_len; ++i) {
    const nyr_mach_inst_t *a = &left->insts[i];
    const nyr_mach_inst_t *b = &right->insts[i];
    if (a->opcode != b->opcode || a->narrow32 != b->narrow32 ||
        a->dividend_u31 != b->dividend_u31 || a->condition != b->condition ||
        a->byte_width != b->byte_width || a->mem_size != b->mem_size ||
        a->sext32 != b->sext32 || a->sext_load != b->sext_load ||
        a->unsigned_arith != b->unsigned_arith)
      equal = false;
    const nyr_mach_operand_t *ao[] = {&a->dst, &a->src0, &a->src1, &a->src2};
    const nyr_mach_operand_t *bo[] = {&b->dst, &b->src0, &b->src1, &b->src2};
    for (size_t j = 0; equal && j < 4; ++j)
      equal = mach_outline_operand_equal(left, ao[j], right, bo[j], left_map,
                                         right_map, &left_next, &right_next);
  }
  free(left_map);
  free(right_map);
  return equal;
}

bool nyr_mach_audit_size_candidates(const nyr_mach_func_t *funcs,
                                    size_t func_count,
                                    nyr_mach_outline_audit_t *out) {
  if (!out || (func_count && !funcs))
    return false;
  *out = (nyr_mach_outline_audit_t){0};
  if (func_count > SIZE_MAX / sizeof(nyr_mach_outline_shape_t))
    return false;
  nyr_mach_outline_shape_t *shapes =
      func_count ? calloc(func_count, sizeof(*shapes)) : NULL;
  if (func_count && !shapes)
    return false;
  bool ok = true;
  for (size_t i = 0; i < func_count; ++i) {
    if (!mach_outline_shape(&funcs[i], &shapes[i])) {
      ok = false;
      break;
    }
    if (shapes[i].eligible)
      out->eligible_functions++;
  }
  for (size_t i = 0; ok && i < func_count; ++i) {
    if (!shapes[i].eligible)
      continue;
    for (size_t j = i + 1; j < func_count; ++j) {
      if (!shapes[j].eligible || shapes[i].body_len != shapes[j].body_len ||
          shapes[i].hash != shapes[j].hash ||
          !mach_outline_equal(&funcs[i], &funcs[j], shapes[i].body_len))
        continue;
      out->duplicate_pairs++;
      if (out->estimated_saved_insts <= SIZE_MAX - shapes[i].body_len)
        out->estimated_saved_insts += shapes[i].body_len;
    }
  }
  free(shapes);
  return ok;
}

static bool mach_outline_selftest_func(nyr_mach_func_t *func, bool pad_vreg,
                                       bool relocated) {
  nyr_mach_reg_t src, dst, ignored;
  uint32_t block;
  bool built =
      !pad_vreg || nyr_mach_alloc_typed_vreg(func, NYR_MACH_TYPE_I64, &ignored);
  built =
      built && nyr_mach_alloc_typed_vreg(func, NYR_MACH_TYPE_I64, &src) &&
      nyr_mach_alloc_typed_vreg(func, NYR_MACH_TYPE_I64, &dst) &&
      nyr_mach_begin_block(func, 0, &block) &&
      nyr_mach_emit(
          func,
          (nyr_mach_inst_t){
              .opcode = NYR_MACH_COPY,
              .dst = {.kind = NYR_MACH_OPERAND_VREG,
                      .reg_class = NYR_MACH_REGCLASS_GPR,
                      .as.reg = src},
              .src0 = {.kind = NYR_MACH_OPERAND_IMM,
                       .as.imm = 7,
                       .relocation = relocated ? NYR_MACH_RELOC_ABS64
                                               : NYR_MACH_RELOC_NONE}}) &&
      nyr_mach_emit(func,
                    (nyr_mach_inst_t){
                        .opcode = NYR_MACH_ADD,
                        .dst = {.kind = NYR_MACH_OPERAND_VREG,
                                .reg_class = NYR_MACH_REGCLASS_GPR,
                                .as.reg = dst},
                        .src0 = {.kind = NYR_MACH_OPERAND_VREG,
                                 .reg_class = NYR_MACH_REGCLASS_GPR,
                                 .as.reg = src},
                        .src1 = {.kind = NYR_MACH_OPERAND_IMM, .as.imm = 5}}) &&
      nyr_mach_emit(func,
                    (nyr_mach_inst_t){.opcode = NYR_MACH_RET,
                                      .effects = NYR_MACH_EFFECT_CONTROL});
  return built;
}

bool nyr_mach_audit_size_candidates_selftest(void) {
  nyr_mach_func_t funcs[3] = {{0}};
  nyr_mach_inst_t before[3][3];
  bool built = mach_outline_selftest_func(&funcs[0], false, false) &&
               mach_outline_selftest_func(&funcs[1], true, false) &&
               mach_outline_selftest_func(&funcs[2], false, true);
  bool unchanged = built;
  if (built)
    for (size_t i = 0; i < 3; ++i) {
      if (funcs[i].inst_len != 3) {
        unchanged = false;
        break;
      }
      memcpy(before[i], funcs[i].insts, sizeof(before[i]));
    }

  nyr_mach_outline_audit_t first, second;
  bool ok = built && unchanged &&
            nyr_mach_audit_size_candidates(funcs, 3, &first) &&
            nyr_mach_audit_size_candidates(funcs, 3, &second);
  if (ok) {
    for (size_t i = 0; i < 3; ++i)
      if (memcmp(before[i], funcs[i].insts, sizeof(before[i])) != 0)
        ok = false;
    ok = ok && first.eligible_functions == 2 && first.duplicate_pairs == 1 &&
         first.estimated_saved_insts == 2 &&
         memcmp(&first, &second, sizeof(first)) == 0;
  }
  for (size_t i = 0; i < 3; ++i)
    nyr_mach_func_free(&funcs[i]);
  return ok;
}

/*
 * Keep integer vector load consumers adjacent before allocation.  Only the
 * pure consumer moves: loads, stores, traps and fixed-register operations
 * retain their order.  This also leaves the memory operand on the RHS for
 * two-address encoders, without extending the lifetime of the address.
 */
static void mach_schedule_vector_load_users(nyr_mach_func_t *func,
                                            const nyr_mach_block_t *block) {
  if (block->first_inst > func->inst_len ||
      block->inst_count > func->inst_len - block->first_inst)
    return;
  size_t end = block->first_inst + block->inst_count;
  for (size_t pc = block->first_inst; pc + 1 < end; ++pc) {
    nyr_mach_inst_t *load = &func->insts[pc];
    nyr_mach_type_t type = mach_value_type(func, &load->dst);
    if (load->opcode != NYR_MACH_LOAD ||
        load->dst.kind != NYR_MACH_OPERAND_VREG ||
        (type != NYR_MACH_TYPE_V128_I64 && type != NYR_MACH_TYPE_V256_I64))
      continue;
    for (size_t next = pc + 1; next < end && next - pc <= 16; ++next) {
      nyr_mach_inst_t use = func->insts[next];
      if (!mach_schedule_inst_safe(func, &use))
        break;
      if (use.opcode == NYR_MACH_ADD &&
          mach_value_type(func, &use.dst) == type &&
          (mach_schedule_value_equal(&use.src0, &load->dst) ||
           mach_schedule_value_equal(&use.src1, &load->dst))) {
        bool safe = true;
        for (size_t between = pc + 1; between < next; ++between) {
          const nyr_mach_inst_t *in = &func->insts[between];
          if (mach_schedule_inst_reads(in, &use.dst) ||
              mach_schedule_inst_writes(in, &use.dst) ||
              mach_schedule_inst_reads(&use, &in->dst)) {
            safe = false;
            break;
          }
        }
        if (safe) {
          if (mach_schedule_value_equal(&use.src0, &load->dst)) {
            nyr_mach_operand_t tmp = use.src0;
            use.src0 = use.src1;
            use.src1 = tmp;
          }
          memmove(&func->insts[pc + 2], &func->insts[pc + 1],
                  (next - pc - 1) * sizeof(*func->insts));
          func->insts[pc + 1] = use;
        }
        break;
      }
      if (mach_schedule_inst_writes(&use, &load->dst) ||
          mach_schedule_inst_reads(&use, &load->dst))
        break;
    }
  }
}

bool nyr_mach_schedule_post_ra(nyr_mach_func_t *func) {
  if (!func || !func->insts || !func->blocks)
    return true;
  for (size_t block = 0; block < func->block_len; ++block) {
    if (!mach_schedule_block(func, &func->blocks[block]))
      return false;
    mach_schedule_vector_load_users(func, &func->blocks[block]);
  }
  return true;
}

static nyr_mach_operand_t mach_schedule_selftest_vreg(nyr_mach_reg_t reg) {
  return (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_VREG,
                              .reg_class = NYR_MACH_REGCLASS_GPR,
                              .as.reg = reg};
}

bool nyr_mach_sink_edge_adds_selftest(void) {
  /*
   * Positive case plus an input redefinition, additional result use,
   * side-effect barrier, and an ordinary (non-edge) copy.
   */
  for (unsigned test = 0; test < 5; ++test) {
    nyr_mach_func_t m = {0};
    nyr_mach_reg_t regs[4];
    bool ok = true;
    for (size_t v = 0; v < 4 && ok; ++v)
      ok = nyr_mach_alloc_typed_vreg(&m, NYR_MACH_TYPE_I64, &regs[v]);
    uint32_t block = 0;
    ok = ok && nyr_mach_begin_block(&m, 0, &block);
    nyr_mach_inst_t add = {.opcode = NYR_MACH_ADD,
                           .source_pc = 1,
                           .dst = mach_schedule_selftest_vreg(regs[1]),
                           .src0 = mach_schedule_selftest_vreg(regs[0]),
                           .src1 = {.kind = NYR_MACH_OPERAND_IMM, .as.imm = 1}};
    nyr_mach_inst_t crossed = {
        .opcode = NYR_MACH_ADD,
        .source_pc = 2,
        .dst = mach_schedule_selftest_vreg(regs[test == 1 ? 0 : 2]),
        .src0 = mach_schedule_selftest_vreg(regs[test == 2 ? 1 : 0]),
        .src1 = {.kind = NYR_MACH_OPERAND_IMM, .as.imm = 7}};
    if (test == 3)
      crossed.effects = NYR_MACH_EFFECT_WRITE_MEMORY;
    nyr_mach_inst_t copy = {.opcode = NYR_MACH_COPY,
                            .source_pc = test == 4 ? 3 : UINT32_MAX,
                            .dst = mach_schedule_selftest_vreg(regs[3]),
                            .src0 = mach_schedule_selftest_vreg(regs[1])};
    ok = ok && nyr_mach_emit(&m, add) && nyr_mach_emit(&m, crossed) &&
         nyr_mach_emit(&m, copy) && mach_sink_edge_adds(&m);
    ok = ok && m.insts[0].source_pc == (test == 0 ? 2 : 1) &&
         m.insts[1].source_pc == (test == 0 ? 1 : 2) &&
         m.insts[2].source_pc == copy.source_pc;
    nyr_mach_func_free(&m);
    if (!ok)
      return false;
  }
  for (unsigned test = 0; test < 2; ++test) {
    nyr_mach_func_t m = {0};
    nyr_mach_reg_t regs[5] = {0};
    bool ok = true;
    for (size_t v = 0; v < 5 && ok; ++v)
      ok = nyr_mach_alloc_typed_vreg(&m, NYR_MACH_TYPE_I64, &regs[v]);
    uint32_t block = 0;
    ok = ok && nyr_mach_begin_block(&m, 0, &block);
    nyr_mach_operand_t arg = mach_schedule_selftest_vreg(regs[0]);
    nyr_mach_operand_t literal = mach_schedule_selftest_vreg(regs[1]);
    nyr_mach_operand_t update = mach_schedule_selftest_vreg(regs[2]);
    nyr_mach_operand_t call_result = mach_schedule_selftest_vreg(regs[3]);
    nyr_mach_inst_t constant = {
        .opcode = NYR_MACH_COPY,
        .source_pc = 1,
        .dst = literal,
        .src0 = {.kind = NYR_MACH_OPERAND_IMM, .as.imm = 1}};
    nyr_mach_inst_t sub = {.opcode = NYR_MACH_SUB,
                           .source_pc = 2,
                           .dst = update,
                           .src0 = arg,
                           .src1 = literal};
    nyr_mach_inst_t call = {
        .opcode = NYR_MACH_CALL,
        .source_pc = 3,
        .dst = call_result,
        .src0 = {.kind = NYR_MACH_OPERAND_SYMBOL, .as.symbol = "callee"},
        .effects = NYR_MACH_EFFECT_CALL};
    call.args = malloc(sizeof(*call.args));
    if (ok && call.args) {
      call.args[0] = arg;
      call.args_len = 1;
      ok = nyr_mach_emit(&m, constant) && nyr_mach_emit(&m, sub) &&
           nyr_mach_emit(&m, call);
    } else {
      ok = false;
    }
    if (!ok)
      free(call.args);
    nyr_mach_inst_t sibling = {
        .opcode = NYR_MACH_COPY,
        .source_pc = UINT32_MAX,
        .dst = test == 0 ? mach_schedule_selftest_vreg(regs[4]) : arg,
        .src0 = call_result};
    nyr_mach_inst_t edge = {.opcode = NYR_MACH_COPY,
                            .source_pc = UINT32_MAX,
                            .dst = arg,
                            .src0 = update};
    ok = ok && nyr_mach_emit(&m, sibling) && nyr_mach_emit(&m, edge) &&
         mach_sink_edge_adds(&m) &&
         m.insts[test == 0 ? 3 : 1].opcode == NYR_MACH_SUB;
    nyr_mach_func_free(&m);
    if (!ok)
      return false;
  }
  const size_t gaps[] = {63, 64, 8192};
  for (size_t trial = 0; trial < sizeof(gaps) / sizeof(gaps[0]); ++trial) {
    nyr_mach_func_t m = {0};
    nyr_mach_reg_t regs[4];
    bool ok = true;
    for (size_t v = 0; v < 4 && ok; ++v)
      ok = nyr_mach_alloc_typed_vreg(&m, NYR_MACH_TYPE_I64, &regs[v]);
    uint32_t block;
    ok = ok && nyr_mach_begin_block(&m, 0, &block);
    nyr_mach_inst_t add = {.opcode = NYR_MACH_ADD,
                           .source_pc = 1,
                           .dst = mach_schedule_selftest_vreg(regs[1]),
                           .src0 = mach_schedule_selftest_vreg(regs[0]),
                           .src1 = {.kind = NYR_MACH_OPERAND_IMM, .as.imm = 1}};
    ok = ok && nyr_mach_emit(&m, add);
    nyr_mach_inst_t filler = add;
    filler.source_pc = 2;
    filler.dst = mach_schedule_selftest_vreg(regs[2]);
    for (size_t i = 0; ok && i < gaps[trial]; ++i)
      ok = nyr_mach_emit(&m, filler);
    nyr_mach_inst_t copy = {.opcode = NYR_MACH_COPY,
                            .source_pc = UINT32_MAX,
                            .dst = mach_schedule_selftest_vreg(regs[3]),
                            .src0 = mach_schedule_selftest_vreg(regs[1])};
    ok = ok && nyr_mach_emit(&m, copy) && mach_sink_edge_adds(&m);
    bool moved = gaps[trial] < 64;
    ok = ok && m.insts[moved ? gaps[trial] : 0].source_pc == 1 &&
         m.insts[gaps[trial] + 1].source_pc == UINT32_MAX;
    nyr_mach_func_free(&m);
    if (!ok)
      return false;
  }
  return true;
}

bool nyr_mach_schedule_post_ra_selftest(void) {
  nyr_mach_func_t mach = {0};
  nyr_mach_reg_t regs[8];
  nyr_mach_reg_t div_regs[7];
  uint32_t safe_block, call_block, branch_block, div_block, shift_block;
  uint32_t overflow_div_block, scheduled_div_block;
  bool built = true;
  for (size_t i = 0; built && i < sizeof(regs) / sizeof(regs[0]); ++i)
    built = nyr_mach_alloc_vreg(&mach, NYR_MACH_REGCLASS_GPR, &regs[i]);
  built =
      built && nyr_mach_begin_block(&mach, 0, &safe_block) &&
      nyr_mach_emit(&mach,
                    (nyr_mach_inst_t){
                        .opcode = NYR_MACH_COPY,
                        .dst = mach_schedule_selftest_vreg(regs[0]),
                        .src0 = {.kind = NYR_MACH_OPERAND_IMM, .as.imm = 1}}) &&
      nyr_mach_emit(&mach,
                    (nyr_mach_inst_t){
                        .opcode = NYR_MACH_COPY,
                        .dst = mach_schedule_selftest_vreg(regs[1]),
                        .src0 = {.kind = NYR_MACH_OPERAND_IMM, .as.imm = 4}}) &&
      nyr_mach_emit(
          &mach,
          (nyr_mach_inst_t){.opcode = NYR_MACH_SQRT,
                            .dst = mach_schedule_selftest_vreg(regs[2]),
                            .src0 = mach_schedule_selftest_vreg(regs[1])}) &&
      nyr_mach_emit(
          &mach,
          (nyr_mach_inst_t){.opcode = NYR_MACH_ADD,
                            .dst = mach_schedule_selftest_vreg(regs[3]),
                            .src0 = mach_schedule_selftest_vreg(regs[0]),
                            .src1 = mach_schedule_selftest_vreg(regs[2])}) &&
      nyr_mach_emit(&mach,
                    (nyr_mach_inst_t){
                        .opcode = NYR_MACH_CMP,
                        .dst = mach_schedule_selftest_vreg(regs[0]),
                        .src0 = {.kind = NYR_MACH_OPERAND_IMM, .as.imm = 1},
                        .src1 = {.kind = NYR_MACH_OPERAND_IMM, .as.imm = 2}}) &&
      nyr_mach_emit(&mach,
                    (nyr_mach_inst_t){
                        .opcode = NYR_MACH_COPY,
                        .dst = mach_schedule_selftest_vreg(regs[4]),
                        .src0 = {.kind = NYR_MACH_OPERAND_IMM, .as.imm = 1}}) &&
      nyr_mach_emit(&mach,
                    (nyr_mach_inst_t){
                        .opcode = NYR_MACH_COPY,
                        .dst = mach_schedule_selftest_vreg(regs[5]),
                        .src0 = {.kind = NYR_MACH_OPERAND_IMM, .as.imm = 4}}) &&
      nyr_mach_emit(
          &mach,
          (nyr_mach_inst_t){.opcode = NYR_MACH_SQRT,
                            .dst = mach_schedule_selftest_vreg(regs[6]),
                            .src0 = mach_schedule_selftest_vreg(regs[5])}) &&
      nyr_mach_emit(&mach, (nyr_mach_inst_t){
                               .opcode = NYR_MACH_ADD,
                               .dst = mach_schedule_selftest_vreg(regs[7]),
                               .src0 = mach_schedule_selftest_vreg(regs[4]),
                               .src1 = mach_schedule_selftest_vreg(regs[6])});
  built =
      built && nyr_mach_begin_block(&mach, 1, &call_block) &&
      nyr_mach_emit(&mach,
                    (nyr_mach_inst_t){
                        .opcode = NYR_MACH_COPY,
                        .dst = mach_schedule_selftest_vreg(regs[4]),
                        .src0 = {.kind = NYR_MACH_OPERAND_IMM, .as.imm = 1}}) &&
      nyr_mach_emit(&mach,
                    (nyr_mach_inst_t){.opcode = NYR_MACH_CALL,
                                      .effects = NYR_MACH_EFFECT_CALL}) &&
      nyr_mach_emit(&mach,
                    (nyr_mach_inst_t){
                        .opcode = NYR_MACH_COPY,
                        .dst = mach_schedule_selftest_vreg(regs[5]),
                        .src0 = {.kind = NYR_MACH_OPERAND_IMM, .as.imm = 2}});
  built =
      built && nyr_mach_begin_block(&mach, 2, &branch_block) &&
      nyr_mach_emit(&mach,
                    (nyr_mach_inst_t){
                        .opcode = NYR_MACH_COPY,
                        .dst = mach_schedule_selftest_vreg(regs[4]),
                        .src0 = {.kind = NYR_MACH_OPERAND_IMM, .as.imm = 1}}) &&
      nyr_mach_emit(
          &mach,
          (nyr_mach_inst_t){.opcode = NYR_MACH_SQRT,
                            .dst = mach_schedule_selftest_vreg(regs[6]),
                            .src0 = mach_schedule_selftest_vreg(regs[7])}) &&
      nyr_mach_emit(
          &mach, (nyr_mach_inst_t){.opcode = NYR_MACH_BR,
                                   .effects = NYR_MACH_EFFECT_CONTROL,
                                   .src1 = {.kind = NYR_MACH_OPERAND_BLOCK,
                                            .as.block_index = branch_block}});
  built =
      built && nyr_mach_begin_block(&mach, 3, &div_block) &&
      nyr_mach_emit(
          &mach, (nyr_mach_inst_t){.opcode = NYR_MACH_COPY,
                                   .dst = mach_schedule_selftest_vreg(regs[4]),
                                   .src0 = {.kind = NYR_MACH_OPERAND_IMM,
                                            .as.imm = 12}}) &&
      nyr_mach_emit(&mach,
                    (nyr_mach_inst_t){
                        .opcode = NYR_MACH_DIV,
                        .dst = mach_schedule_selftest_vreg(regs[5]),
                        .src0 = mach_schedule_selftest_vreg(regs[4]),
                        .src1 = {.kind = NYR_MACH_OPERAND_IMM, .as.imm = 0}}) &&
      nyr_mach_emit(&mach, (nyr_mach_inst_t){
                               .opcode = NYR_MACH_SQRT,
                               .dst = mach_schedule_selftest_vreg(regs[6]),
                               .src0 = mach_schedule_selftest_vreg(regs[7])});
  built =
      built && nyr_mach_begin_block(&mach, 4, &shift_block) &&
      nyr_mach_emit(
          &mach, (nyr_mach_inst_t){.opcode = NYR_MACH_COPY,
                                   .dst = mach_schedule_selftest_vreg(regs[4]),
                                   .src0 = {.kind = NYR_MACH_OPERAND_IMM,
                                            .as.imm = 12}}) &&
      nyr_mach_emit(&mach,
                    (nyr_mach_inst_t){
                        .opcode = NYR_MACH_SHL,
                        .dst = mach_schedule_selftest_vreg(regs[5]),
                        .src0 = mach_schedule_selftest_vreg(regs[4]),
                        .src1 = {.kind = NYR_MACH_OPERAND_IMM, .as.imm = 1}}) &&
      nyr_mach_emit(&mach, (nyr_mach_inst_t){
                               .opcode = NYR_MACH_SQRT,
                               .dst = mach_schedule_selftest_vreg(regs[6]),
                               .src0 = mach_schedule_selftest_vreg(regs[7])});

  for (size_t i = 0; built && i < sizeof(div_regs) / sizeof(div_regs[0]); ++i)
    built = nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &div_regs[i]);
  built =
      built && nyr_mach_begin_block(&mach, 5, &overflow_div_block) &&
      nyr_mach_emit(
          &mach, (nyr_mach_inst_t){.opcode = NYR_MACH_COPY,
                                   .dst = mach_schedule_selftest_vreg(regs[4]),
                                   .src0 = {.kind = NYR_MACH_OPERAND_IMM,
                                            .as.imm = 12}}) &&
      nyr_mach_emit(
          &mach, (nyr_mach_inst_t){.opcode = NYR_MACH_DIV,
                                   .dst = mach_schedule_selftest_vreg(regs[5]),
                                   .src0 = mach_schedule_selftest_vreg(regs[4]),
                                   .src1 = {.kind = NYR_MACH_OPERAND_IMM,
                                            .as.imm = -1}}) &&
      nyr_mach_emit(&mach, (nyr_mach_inst_t){
                               .opcode = NYR_MACH_SQRT,
                               .dst = mach_schedule_selftest_vreg(regs[6]),
                               .src0 = mach_schedule_selftest_vreg(regs[7])});
  built =
      built && nyr_mach_begin_block(&mach, 6, &scheduled_div_block) &&
      nyr_mach_emit(
          &mach,
          (nyr_mach_inst_t){
              .opcode = NYR_MACH_COPY,
              .dst = mach_schedule_selftest_vreg(div_regs[0]),
              .src0 = {.kind = NYR_MACH_OPERAND_IMM, .as.imm = 12}}) &&
      nyr_mach_emit(&mach,
                    (nyr_mach_inst_t){
                        .opcode = NYR_MACH_DIV,
                        .dst = mach_schedule_selftest_vreg(div_regs[1]),
                        .src0 = mach_schedule_selftest_vreg(div_regs[0]),
                        .src1 = {.kind = NYR_MACH_OPERAND_IMM, .as.imm = 3}}) &&
      nyr_mach_emit(&mach,
                    (nyr_mach_inst_t){
                        .opcode = NYR_MACH_COPY,
                        .dst = mach_schedule_selftest_vreg(div_regs[2]),
                        .src0 = {.kind = NYR_MACH_OPERAND_IMM, .as.imm = 1}}) &&
      nyr_mach_emit(&mach,
                    (nyr_mach_inst_t){
                        .opcode = NYR_MACH_COPY,
                        .dst = mach_schedule_selftest_vreg(div_regs[3]),
                        .src0 = {.kind = NYR_MACH_OPERAND_IMM, .as.imm = 2}}) &&
      nyr_mach_emit(&mach,
                    (nyr_mach_inst_t){
                        .opcode = NYR_MACH_COPY,
                        .dst = mach_schedule_selftest_vreg(div_regs[4]),
                        .src0 = {.kind = NYR_MACH_OPERAND_IMM, .as.imm = 3}}) &&
      nyr_mach_emit(&mach,
                    (nyr_mach_inst_t){
                        .opcode = NYR_MACH_SELECT,
                        .dst = mach_schedule_selftest_vreg(div_regs[5]),
                        .src0 = mach_schedule_selftest_vreg(div_regs[2]),
                        .src1 = mach_schedule_selftest_vreg(div_regs[3]),
                        .src2 = mach_schedule_selftest_vreg(div_regs[4])}) &&
      nyr_mach_emit(
          &mach,
          (nyr_mach_inst_t){.opcode = NYR_MACH_ADD,
                            .dst = mach_schedule_selftest_vreg(div_regs[6]),
                            .src0 = mach_schedule_selftest_vreg(div_regs[1]),
                            .src1 = mach_schedule_selftest_vreg(div_regs[5])});

  nyr_mach_inst_t call_before[3], div_before[3], overflow_div_before[3],
      shift_before[3];
  if (built) {
    memcpy(call_before, mach.insts + mach.blocks[call_block].first_inst,
           sizeof(call_before));
    memcpy(div_before, mach.insts + mach.blocks[div_block].first_inst,
           sizeof(div_before));
    memcpy(shift_before, mach.insts + mach.blocks[shift_block].first_inst,
           sizeof(shift_before));
    memcpy(overflow_div_before,
           mach.insts + mach.blocks[overflow_div_block].first_inst,
           sizeof(overflow_div_before));
  }
  bool ok = built && nyr_mach_schedule_post_ra(NULL) &&
            nyr_mach_schedule_post_ra(&mach);
  if (ok) {
    const nyr_mach_inst_t *safe =
        mach.insts + mach.blocks[safe_block].first_inst;
    const nyr_mach_inst_t *branch =
        mach.insts + mach.blocks[branch_block].first_inst;
    const nyr_mach_inst_t *scheduled_div =
        mach.insts + mach.blocks[scheduled_div_block].first_inst;
    size_t div_at = SIZE_MAX, select_at = SIZE_MAX, add_at = SIZE_MAX;
    for (size_t i = 0; i < mach.blocks[scheduled_div_block].inst_count; ++i) {
      const nyr_mach_inst_t *inst = &scheduled_div[i];
      if (inst->opcode == NYR_MACH_DIV && inst->dst.as.reg == div_regs[1])
        div_at = i;
      else if (inst->opcode == NYR_MACH_SELECT &&
               inst->dst.as.reg == div_regs[5])
        select_at = i;
      else if (inst->opcode == NYR_MACH_ADD && inst->dst.as.reg == div_regs[6])
        add_at = i;
    }
    bool branch_segment_kept = branch[2].opcode == NYR_MACH_BR &&
                               ((branch[0].opcode == NYR_MACH_COPY &&
                                 branch[0].dst.as.reg == regs[4] &&
                                 branch[1].opcode == NYR_MACH_SQRT &&
                                 branch[1].dst.as.reg == regs[6]) ||
                                (branch[1].opcode == NYR_MACH_COPY &&
                                 branch[1].dst.as.reg == regs[4] &&
                                 branch[0].opcode == NYR_MACH_SQRT &&
                                 branch[0].dst.as.reg == regs[6]));
    ok = safe[0].dst.as.reg == regs[1] && safe[1].dst.as.reg == regs[2] &&
         safe[2].dst.as.reg == regs[0] && safe[3].dst.as.reg == regs[3] &&
         safe[4].opcode == NYR_MACH_CMP && safe[5].dst.as.reg == regs[5] &&
         safe[6].dst.as.reg == regs[6] && safe[7].dst.as.reg == regs[4] &&
         safe[8].dst.as.reg == regs[7] && branch_segment_kept &&
         memcmp(call_before, mach.insts + mach.blocks[call_block].first_inst,
                sizeof(call_before)) == 0 &&
         memcmp(div_before, mach.insts + mach.blocks[div_block].first_inst,
                sizeof(div_before)) == 0 &&
         memcmp(overflow_div_before,
                mach.insts + mach.blocks[overflow_div_block].first_inst,
                sizeof(overflow_div_before)) == 0 &&
         memcmp(shift_before, mach.insts + mach.blocks[shift_block].first_inst,
                sizeof(shift_before)) == 0 &&
         div_at != SIZE_MAX && select_at != SIZE_MAX && add_at != SIZE_MAX &&
         div_at < select_at && select_at < add_at &&
         scheduled_div[0].opcode == NYR_MACH_COPY &&
         scheduled_div[0].dst.as.reg == div_regs[0];
  }
  nyr_mach_func_free(&mach);
  return ok;
}
