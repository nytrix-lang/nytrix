/*
 * Machine-form IR: target-specific intermediate representation
 * between NYIR and object code with register operands and addressing modes.
 */
#include "code/backend/ir/machine.h"
#include "base/debug.h"
#include "base/trace.h"
#include "code/backend/internal.h"
#include "code/backend/ir/internal.h"
#include "code/backend/ir/opt/util.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
  bool *targeted = calloc(m->block_len, sizeof(*targeted));
  uint8_t *target_count = calloc(m->block_len, sizeof(*target_count));
  uint32_t *target_pred = malloc(m->block_len * sizeof(*target_pred));
  uint32_t *map = malloc(m->block_len * sizeof(*map));
  if (!targeted || !target_count || !target_pred || !map) {
    free(targeted);
    free(target_count);
    free(target_pred);
    free(map);
    return false;
  }
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
  free(targeted);
  free(target_count);
  free(target_pred);
  free(map);
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
  if (!m->vreg_len)
    return true;
  bool *read = calloc(m->vreg_len, sizeof(*read));
  if (!read)
    return false;
  for (size_t i = 0; i < m->inst_len; ++i) {
    const nyr_mach_inst_t *in = &m->insts[i];
    if (in->opcode == NYR_MACH_NOP)
      continue;
    const nyr_mach_operand_t *inputs[] = {&in->src0, &in->src1, &in->src2};
    for (size_t k = 0; k < sizeof(inputs) / sizeof(inputs[0]); ++k)
      if (inputs[k]->kind == NYR_MACH_OPERAND_VREG &&
          inputs[k]->as.reg < m->vreg_len)
        read[inputs[k]->as.reg] = true;
    for (size_t k = 0; k < in->args_len; ++k)
      if (in->args[k].kind == NYR_MACH_OPERAND_VREG &&
          in->args[k].as.reg < m->vreg_len)
        read[in->args[k].as.reg] = true;
    if (in->opcode == NYR_MACH_STORE && in->dst.kind == NYR_MACH_OPERAND_VREG &&
        in->dst.as.reg < m->vreg_len)
      read[in->dst.as.reg] = true;
  }
  size_t removed = 0;
  for (size_t i = 0; i < m->inst_len; ++i) {
    nyr_mach_inst_t *in = &m->insts[i];
    if (in->opcode == NYR_MACH_COPY && !in->effects &&
        in->src0.kind == NYR_MACH_OPERAND_IMM &&
        in->dst.kind == NYR_MACH_OPERAND_VREG && in->dst.as.reg < m->vreg_len &&
        !read[in->dst.as.reg]) {
      in->opcode = NYR_MACH_NOP;
      ++removed;
    }
  }
  if (removed && ny_dbg_is_enabled(NY_DBG_HEURISTIC))
    ny_dbg_heuristic_event("MACHINE_LITERAL_DCE", "removed=%zu", removed);
  free(read);
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
 */
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
  if (!def_block || !def_inst || !last_use || !local || !multi_def || !homes ||
      !intervals || !slot_end)
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
  size_t local_home_count = 0;
  size_t slot_count = 0;
  uint32_t block_id = no_block;
  for (size_t i = 0; i < count; ++i) {
    mach_home_interval_t *cur = &intervals[i];
    if (cur->block != block_id) {
      block_id = cur->block;
      slot_count = 0;
    }

    const nyr_mach_inst_t *def = &mach->insts[cur->def_inst];
    bool coalesced = false;
    if (def->opcode == NYR_MACH_COPY && !def->narrow32 &&
        def->src0.kind == NYR_MACH_OPERAND_VREG && def->src0.as.reg < nv) {
      size_t src = def->src0.as.reg;
      if (local[src] && def_block[src] == cur->block &&
          last_use[src] == cur->start &&
          mach->vreg_types[src] == mach->vreg_types[cur->reg] &&
          mach->vreg_classes[src] == mach->vreg_classes[cur->reg]) {
        homes[cur->reg] = homes[src];
        size_t local_slot = homes[src] - nonlocal_count;
        if (local_slot < slot_count && slot_end[local_slot] < cur->end)
          slot_end[local_slot] = cur->end;
        coalesced = true;
      }
    }
    if (coalesced)
      continue;

    size_t slot = 0;
    while (slot < slot_count && slot_end[slot] >= cur->start)
      ++slot;
    if (slot == slot_count) {
      slot_end[slot_count++] = cur->end;
      if (slot_count > local_home_count)
        local_home_count = slot_count;
    } else {
      slot_end[slot] = cur->end;
    }
    homes[cur->reg] = (uint32_t)(nonlocal_count + slot);
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
  for (size_t i = 0; i < func->block_len; ++i)
    if (func->blocks[i].label == label)
      return false;
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
    return false;
  }
  case NYR_MACH_CONVERT:
    return inst->effects == NYR_MACH_EFFECT_NONE &&
           mach_is_scalar_value(func, &inst->dst) &&
           mach_is_scalar_value(func, &inst->src0) &&
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
    return inst->effects == NYR_MACH_EFFECT_NONE &&
           mach_value_type(func, &inst->dst) == NYR_MACH_TYPE_I64 &&
           mach_value_type(func, &inst->src0) == NYR_MACH_TYPE_I64 &&
           mach_is_none(&inst->src1) && mach_is_none(&inst->src2);
  case NYR_MACH_LOAD:
    return inst->effects == NYR_MACH_EFFECT_READ_MEMORY &&
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
        mach_value_type(func, &inst->dst) == NYR_MACH_TYPE_I64 &&
        mach_is_integer_value(func, &inst->src0) &&
        inst->src1.kind == NYR_MACH_OPERAND_IMM) {
      int64_t divisor = inst->src1.as.imm;
      return divisor > 1 && divisor <= INT32_MAX &&
             ((uint64_t)divisor & ((uint64_t)divisor - 1)) == 0;
    }
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
  case NYR_MACH_ROR:
  case NYR_MACH_ROR32:
    if (inst->effects != NYR_MACH_EFFECT_NONE || !mach_is_none(&inst->src2))
      return false;
    if ((inst->opcode == NYR_MACH_SHL || inst->opcode == NYR_MACH_SAR ||
         inst->opcode == NYR_MACH_ROR || inst->opcode == NYR_MACH_ROR32) &&
        inst->src1.kind == NYR_MACH_OPERAND_IMM)
      return mach_value_type(func, &inst->dst) == NYR_MACH_TYPE_I64 &&
             mach_value_type(func, &inst->src0) == NYR_MACH_TYPE_I64;
    if (inst->opcode == NYR_MACH_ROR32)
      return mach_value_type(func, &inst->dst) == NYR_MACH_TYPE_I64 &&
             mach_value_type(func, &inst->src0) == NYR_MACH_TYPE_I64 &&
             mach_value_type(func, &inst->src1) == NYR_MACH_TYPE_I64;
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
           mach_value_type(func, &inst->dst) == NYR_MACH_TYPE_I64 &&
           mach_value_type(func, &inst->src0) == NYR_MACH_TYPE_I64 &&
           mach_value_type(func, &inst->src1) == NYR_MACH_TYPE_I64 &&
           mach_value_type(func, &inst->src2) == NYR_MACH_TYPE_I64;
  case NYR_MACH_CMP:
    return inst->effects == NYR_MACH_EFFECT_NONE &&
           mach_is_integer_value(func, &inst->dst) &&
           mach_is_scalar_value(func, &inst->src0) &&
           (mach_same_value_type(func, &inst->src0, &inst->src1) ||
            (mach_is_integer_value(func, &inst->src0) &&
             mach_is_integer_value(func, &inst->src1))) &&
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
           (mach_is_none(&inst->dst) || mach_is_value(&inst->dst));
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
  case NYR_MACH_ROR:
    return "ror";
  case NYR_MACH_ROR32:
    return "ror32";
  case NYR_MACH_BSWAP64:
    return "bswap64";
  case NYR_MACH_BSWAP32:
    return "bswap32";
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
      if (func->param_types[i] > NYIR_PARAM_AGGREGATE)
        return nyr_err(err, err_len,
                       "machine IR verify: invalid parameter type metadata");
      uint32_t packed = func->param_abi[i];
      if (func->param_types[i] == NYIR_PARAM_AGGREGATE) {
        uint32_t size = NYIR_ARG_AGG_SIZE(packed);
        uint32_t chunks = (size + 7u) / 8u;
        uint32_t home = func->param_aggregate_home_slots[i];
        bool indirect = NYIR_ARG_AGG_CLASS(packed, 0) == NYIR_ARG_CLASS_MEMORY;
        if (!size ||
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
    if (inst->opcode > NYR_MACH_REDUCE_ADD)
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
         inst->call_clobbers != 0))
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

static bool mach_lower_phi_edges(const nyr_func_t *nyir, const nyr_cfg_t *cfg,
                                 const nyr_mach_reg_t *values,
                                 nyr_mach_func_t *base, char *err,
                                 size_t err_len) {
  size_t edge_count = 0;
  for (size_t succ = 0; succ < cfg->block_count; ++succ) {
    size_t at = cfg->block_start[succ];
    if (at < cfg->block_end[succ] && nyir->data[at].op == NYIR_LABEL)
      ++at;
    if (at >= cfg->block_end[succ] || nyir->data[at].op != NYIR_PHI)
      continue;
    edge_count += cfg->pred_offsets[succ + 1] - cfg->pred_offsets[succ];
  }
  if (!edge_count)
    return true;
  mach_phi_edge_t *edges = ny_calloc_array(edge_count, sizeof(*edges));
  if (!edges)
    return nyr_err(err, err_len, "machine lower: out of memory for PHI edges");

  size_t ei = 0;
  for (size_t succ = 0; succ < cfg->block_count; ++succ) {
    size_t first = cfg->block_start[succ];
    if (first < cfg->block_end[succ] && nyir->data[first].op == NYIR_LABEL)
      ++first;
    size_t phi_count = 0;
    while (first + phi_count < cfg->block_end[succ] &&
           nyir->data[first + phi_count].op == NYIR_PHI)
      ++phi_count;
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
        mach_phi_edges_free(edges, edge_count);
        return nyr_err(err, err_len,
                       "machine lower: out of memory for PHI copies");
      }
      int64_t pred_label = cfg->block_label[pred];
      for (size_t k = 0; k < phi_count; ++k) {
        const nyr_inst_t *phi = &nyir->data[first + k];
        int incoming = -1;
        for (size_t n = 0; n < phi->phi_incoming_len; ++n) {
          if (phi->phi_incoming[n].predecessor_label == pred_label) {
            incoming = phi->phi_incoming[n].value;
            break;
          }
        }
        if (incoming < 0 || phi->dst < 0) {
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
  case NYIR_VEC4_REDUCE_ADD_I64:
  case NYIR_VEC8_REDUCE_ADD_I64:
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
  if (has_aggregate_param) {
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
    memcpy(lowered.param_abi, nyir->param_abi,
           nyir->param_count * sizeof(*lowered.param_abi));
    for (size_t i = 0; i < nyir->param_count; ++i)
      lowered.param_aggregate_home_slots[i] = UINT32_MAX;
  }
  if (nyir->next_value > 0) {
    definitions = nyr_build_defs(nyir);
    if (!definitions)
      return nyr_err(err, err_len, "machine lower: out of memory");
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
             in->op == NYIR_ALLOCA) &&
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
    if (!size || !chunks ||
        !nyr_mach_alloc_typed_frame_slot(&lowered, 8, 8, NYR_MACH_TYPE_I64,
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
        (in->flags & NYIR_INST_F_MEM_BYTE) && !(caps & NY_NATIVE_CAP_MACH_BYTE))
      goto done;
    nyr_mach_inst_t inst = {0};
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
                                       .relocation = NYR_MACH_RELOC_PC_REL32};
    } else if (in->op == NYIR_LOAD_I64) {
      inst.opcode = NYR_MACH_LOAD;
      inst.dst = mach_vreg(&lowered, values[in->dst]);
      inst.src0 = mach_vreg(&lowered, values[in->a]);
      inst.byte_width = (in->flags & NYIR_INST_F_MEM_BYTE) != 0;
      inst.effects = NYR_MACH_EFFECT_READ_MEMORY;
    } else if (in->op == NYIR_STORE_I64) {
      inst.opcode = NYR_MACH_STORE;
      inst.dst = mach_vreg(&lowered, values[in->a]);
      inst.src0 = mach_vreg(&lowered, values[in->c]);
      inst.byte_width = (in->flags & NYIR_INST_F_MEM_BYTE) != 0;
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
    } else if (in->op == NYIR_CALL) {
      inst.opcode = NYR_MACH_CALL;
      if (in->dst >= 0)
        inst.dst = mach_vreg(&lowered, values[in->dst]);
      inst.src0 = (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_SYMBOL,
                                       .as.symbol = in->symbol,
                                       .relocation = NYR_MACH_RELOC_CALL};
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
        inst.args_len = (size_t)arg_count;
        inst.args = ny_calloc_array(inst.args_len, sizeof(*inst.args));
        if (!inst.args)
          goto oom;
        if (in->arg_sizes) {
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
          memcpy(inst.arg_sizes, in->arg_sizes,
                 inst.args_len * sizeof(*inst.arg_sizes));
      }
      inst.call_is_extern = (in->flags & NYIR_INST_F_EXTERN) != 0;
      inst.call_has_sret = (in->flags & NYIR_INST_F_SRET) != 0;
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
        bool pow2_divisor =
            (caps & NY_NATIVE_CAP_MACH_SDIV_POW2) && in->op == NYIR_DIV_I64 &&
            has_constant_rhs && rhs_constant > 1 && rhs_constant <= INT32_MAX &&
            ((uint64_t)rhs_constant & ((uint64_t)rhs_constant - 1)) == 0;
        bool immediate_shift =
            bdef >= 0 && nyir->data[bdef].op == NYIR_CONST_I64 && shift_like;
        bool immediate_rhs = immediate_shift || pow2_divisor;
        if (immediate_rhs) {
          inst.src1 = (nyr_mach_operand_t){
              .kind = NYR_MACH_OPERAND_IMM,
              .as.imm = immediate_shift ? nyir->data[bdef].imm : rhs_constant};
        } else {
          inst.src1 = mach_vreg(&lowered, values[in->b]);
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
               in->op == NYIR_VEC8_OR_I64 || in->op == NYIR_VEC8_XOR_I64) {
      /*
       * Packed vector ALU: one machine op, element type on the vreg.
       */
      if (in->op == NYIR_VEC4_SUB_I64 || in->op == NYIR_VEC4_SUB_F64 ||
          in->op == NYIR_VEC8_SUB_F32)
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
      else if (in->op == NYIR_VEC4_SHL_I64)
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
               in->op == NYIR_VEC4_SET1_I64) {
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
  case NYR_MACH_BSWAP64:
  case NYR_MACH_BSWAP32:
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
      mach_schedule_inst_has_fixed_operand(inst) ||
      inst->source_pc == UINT32_MAX)
    return false;
  /*
   * Constant signed division is safe to move only when it cannot trap:
   * division by zero and INT64_MIN / -1 remain ordering barriers. The
   * register allocator models the fixed-register clobbers after scheduling.
   */
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

static bool mach_schedule_segment(nyr_mach_inst_t *insts, size_t count) {
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
  nyr_mach_inst_t *ordered = malloc(count * sizeof(*ordered));
  if (!edges || !indegree || !critical || !selected || !order || !src0_user ||
      !ordered) {
    free(edges);
    free(indegree);
    free(critical);
    free(selected);
    free(order);
    free(src0_user);
    free(ordered);
    return false;
  }

  /*
   * Preserve every value dependency and every write/write ordering.  The
   * original order is otherwise not a dependency, which is what lets the
   * ready-list choose independent work from later in the block.
   */
  for (size_t i = 0; i < count; ++i) {
    src0_user[i] = SIZE_MAX;
    for (size_t j = i + 1; j < count; ++j) {
      bool dependency = false;
      if (mach_is_value(&insts[i].dst)) {
        dependency = mach_schedule_inst_reads(&insts[j], &insts[i].dst) ||
                     mach_schedule_inst_writes(&insts[j], &insts[i].dst);
      }
      if (!dependency && mach_is_value(&insts[j].dst))
        dependency = mach_schedule_inst_reads(&insts[i], &insts[j].dst);
      if (dependency) {
        edges[i * count + j] = 1;
        ++indegree[j];
      }
    }
    if (!mach_is_value(&insts[i].dst))
      continue;
    size_t user = SIZE_MAX;
    size_t uses = 0;
    for (size_t j = i + 1; j < count; ++j) {
      size_t user_uses = mach_schedule_operand_uses(&insts[j], &insts[i].dst);
      if (user_uses) {
        uses += user_uses;
        user = j;
        if (uses > 1)
          break;
      }
    }
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

  for (size_t out = 0; out < count; ++out) {
    size_t best = SIZE_MAX;
    /*
     * Keep a single-use source-0 value live in %rax through its consumer.
     * Wait to issue that producer until the consumer's other inputs are ready,
     * then force the consumer into the immediately following slot.
     */
    for (size_t i = 0; i < count; ++i) {
      if (selected[i] || indegree[i] != 0 || src0_user[i] == SIZE_MAX)
        continue;
      size_t user = src0_user[i];
      if (indegree[user] == 1 &&
          (best == SIZE_MAX || critical[i] > critical[best] ||
           (critical[i] == critical[best] && i < best)))
        best = i;
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
      free(ordered);
      return false;
    }
    selected[best] = true;
    order[out] = best;
    ordered[out] = insts[best];
    size_t forced_user = src0_user[best];
    for (size_t j = best + 1; j < count; ++j)
      if (edges[best * count + j])
        --indegree[j];
    if (forced_user != SIZE_MAX && indegree[forced_user] == 0 &&
        out + 1 < count && !selected[forced_user]) {
      selected[forced_user] = true;
      order[out + 1] = forced_user;
      ordered[out + 1] = insts[forced_user];
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
    if (!mach_schedule_segment(insts + start, end - start))
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
      inst->call_has_sret || inst->call_clobbers != 0)
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
  case NYR_MACH_ROR:
  case NYR_MACH_ROR32:
  case NYR_MACH_BSWAP64:
  case NYR_MACH_BSWAP32:
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
    hash = mach_outline_mix(hash, inst->condition);
    hash = mach_outline_mix(hash, inst->byte_width);
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
        a->condition != b->condition || a->byte_width != b->byte_width)
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

bool nyr_mach_schedule_post_ra(nyr_mach_func_t *func) {
  if (!func || !func->insts || !func->blocks)
    return true;
  for (size_t block = 0; block < func->block_len; ++block)
    if (!mach_schedule_block(func, &func->blocks[block]))
      return false;
  return true;
}

static nyr_mach_operand_t mach_schedule_selftest_vreg(nyr_mach_reg_t reg) {
  return (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_VREG,
                              .reg_class = NYR_MACH_REGCLASS_GPR,
                              .as.reg = reg};
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
