/*
 * CFG simplification: constant-branch folding, empty-block merging,
 * and trampoline branch threading (BR→LABEL→BR chains flattened,
 * 16-hop max). Runs to fixed point.
 *
 * References:
 *  Skiena — The Algorithm Design Manual, 3rd Ed., Ch.15 §15.1–15.3.
 *  Cooper & Torczon — Engineering a Compiler, 2nd Ed., Ch.9 §9.3.
 *  Cytron, Ferrante, Rosen, Wegman, Zadeck — "Efficiently Computing
 *   SSA Form and the Control Dependence Graph" (TOPLAS 1991).
 */
#include "code/ir/opt/util.h"
#include "code/ir/internal.h"
#include "base/compat.h"
#include "base/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * True when the block labelled `label` contains PHIs.  Valid NYIR places
 * PHIs contiguously between the LABEL and the first real instruction, so
 * scanning until the first non-NOP instruction suffices.  Unknown labels
 * report "has phi" so callers stay on the safe side.
 */
static bool cfg_simplify_block_has_phi(const nyir_func_t *f, int64_t label) {
  if (label < 0)
    return true;
  size_t k = 0;
  while (k < f->len && !(f->data[k].op == NYIR_LABEL && f->data[k].imm == label))
    ++k;
  if (k == f->len)
    return true;
  for (++k; k < f->len; ++k) {
    if (f->data[k].op == NYIR_NOP)
      continue;
    return f->data[k].op == NYIR_PHI;
  }
  return false;
}

bool nyir_cfg_simplify(nyir_func_t *f) {
  if (!f)
    return true;

  bool *known = NULL;
  int64_t *value = NULL;
  if (f->next_value > 0) {
    known = (bool *)calloc((size_t)f->next_value, sizeof(bool));
    value = (int64_t *)calloc((size_t)f->next_value, sizeof(int64_t));
    if (!known || !value) {
      free(known);
      free(value);
      return false;
    }
    if (!nir_collect_consts(f, known, value)) {
      free(known);
      free(value);
      return false;
    }
  }

  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    if (in->op == NYIR_BR_IF && in->a >= 0 && known && known[in->a] &&
        value[in->a] == 0 &&
        !cfg_simplify_block_has_phi(f, in->imm)) {
      /*
       * Removing a false edge keeps the lexical fallthrough edge. Its target
       * and PHIs are unchanged; the removed target must itself have no PHI.
       */
      *in = (nyir_inst_t){.op = NYIR_NOP, .dst = -1, .a = -1, .b = -1,
                          .c = -1, .d = -1, .e = -1, .f = -1};
    }
    if (in->op == NYIR_BR || in->op == NYIR_RET) {
      /*
       * Instructions after an unconditional terminator until the next
       * label are USUALLY unreachable in linear layout -- but exotic
       * predecessor shapes (unlabeled join regions fed by a conditional
       * fallthrough elsewhere) make that assumption unsafe for anything
       * observable. Only erase suffixes made purely of value computation;
       * any store/call/bounds-check keeps the whole suffix alive (the
       * dedicated DCE/ADE passes own real dead-store cleanup).
       */
      bool observable = false;
      size_t jend = i + 1;
      for (; jend < f->len && f->data[jend].op != NYIR_LABEL; ++jend) {
        nyir_op_t op = f->data[jend].op;
        if (op == NYIR_NOP || op == NYIR_BR || op == NYIR_BR_IF ||
            op == NYIR_RET)
          continue;
        if (op == NYIR_STORE_LOCAL || op == NYIR_STORE_I64 ||
            op == NYIR_CALL || op == NYIR_COPY_STRUCT ||
            op == NYIR_ALLOCA || op == NYIR_BOUNDS_CHECK ||
            op == NYIR_VEC4_STORE_F64 || op == NYIR_VEC8_STORE_F32 ||
            op == NYIR_VEC4_STORE_I64 || op == NYIR_VEC8_STORE_I64) {
          observable = true;
          break;
        }
      }
      if (!observable)
        for (size_t j = i + 1; j < jend; ++j)
          if (f->data[j].op != NYIR_NOP)
            (void)nyir_erase_instruction(f, j);
    }
  }

  /*
   * Trampoline branch threading: flatten chains of BR -> LABEL -> BR
   * so branch targets point directly to the final destination block.
   *
   * An edge retargeted past a trampoline changes the destination block's
   * predecessor set, which would leave PHI predecessor labels stale (the
   * same hazard that keeps nyir_jump_thread disabled).  Threading is
   * therefore skipped whenever the destination block contains PHIs.
   */
  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    if (in->op == NYIR_BR || in->op == NYIR_BR_IF) {
      int64_t target = in->imm;
      for (int hop = 0; hop < 16; ++hop) {
        int64_t next_target = -1;
        for (size_t k = 0; k < f->len; ++k) {
          if (f->data[k].op == NYIR_LABEL && f->data[k].imm == target) {
            size_t next = nir_next_non_nop(f, k + 1);
            if (next < f->len && f->data[next].op == NYIR_BR &&
                f->data[next].imm != target) {
              if (!cfg_simplify_block_has_phi(f, f->data[next].imm))
                next_target = f->data[next].imm;
            }
            break;
          }
        }
        if (next_target >= 0 && next_target != target)
          target = next_target;
        else
          break;
      }
      in->imm = target;
    }
  }

  free(known);
  free(value);
  return true;
}
