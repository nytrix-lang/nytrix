#include "code/native/ir/opt/util.h"
#include "code/native/ir/internal.h"
#include "base/compat.h"
#include "base/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* LICM: Loop-Invariant Code Motion (single-pass structured approach) */
/*                                                                    */
/* Nytrix while loops generate:                                       */
/*   LABEL(head) ... BR_IF(cond, end) ... body ... BR(head) LABEL(end)*/
/*                                                                    */
/* We identify only CFG-proven natural backedges. The structured loop */
/* body is [head_label..BR].                                          */
/* Instructions in the loop body whose inputs are all defined outside */
/* the loop are hoisted before the head label.                        */
/*                                                                    */
/* This is a simplified LICM for structured loops only. It handles    */
/* nested loops correctly by processing inner loops first.            */
/* ------------------------------------------------------------------ */

bool nyir_licm(nyir_func_t *f) {
  if (!f || f->len < 4 || f->next_value <= 0)
    return true;
  nyir_cfg_t cfg = {0};
  if (!nyir_cfg_build(f, &cfg))
    return false;

  /* Find all loops: for each BR that targets a label at an earlier
   * position, record (head_label, body_start_idx, back_edge_idx). */
  typedef struct {
    int head_label;     /* label imm */
    size_t head_idx;    /* index of the LABEL instruction */
    size_t body_start;  /* index after the label */
    size_t back_edge;   /* index of the BR instruction */
    size_t end_idx;     /* index of the label after the back-edge BR */
  } loop_t;
  loop_t *loops = NULL;
  size_t loop_count = 0;
  size_t loop_cap = 0;

  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    if (in->op != NYIR_BR || in->imm < 0)
      continue;
    int target = in->imm;
    /* Find the LABEL with this imm at or before i. */
    for (size_t j = i; j > 0; --j) {
      if (f->data[j - 1].op == NYIR_LABEL && f->data[j - 1].imm == target) {
        size_t latch = cfg.inst_block[i];
        size_t header = cfg.inst_block[j - 1];
        if (!nyir_cfg_is_backedge(&cfg, latch, header))
          break;
        if (loop_count == loop_cap) {
          size_t cap = loop_cap ? loop_cap * 2 : 16;
          if (cap < loop_cap || cap > SIZE_MAX / sizeof(*loops)) {
            free(loops);
            nyir_cfg_free(&cfg);
            return false;
          }
          loop_t *grown = realloc(loops, cap * sizeof(*loops));
          if (!grown) {
            free(loops);
            nyir_cfg_free(&cfg);
            return false;
          }
          loops = grown;
          loop_cap = cap;
        }
        loops[loop_count].head_label = target;
        loops[loop_count].head_idx = j - 1;
        loops[loop_count].body_start = j;
        loops[loop_count].back_edge = i;
        /* Find the end label (first label after the BR). */
        loops[loop_count].end_idx = i + 1;
        for (size_t k = i + 1; k < f->len; ++k) {
          if (f->data[k].op == NYIR_LABEL) {
            loops[loop_count].end_idx = k;
            break;
          }
        }
        loop_count++;
        break;
      }
    }
  }

  if (loop_count == 0) {
    free(loops);
    nyir_cfg_free(&cfg);
    return true;
  }

  /* Sort loops by body_start descending (innermost first). */
  for (size_t i = 0; i + 1 < loop_count; ++i) {
    for (size_t j = i + 1; j < loop_count; ++j) {
      /* A nested loop starts later in the instruction stream. Hoist its
       * invariants first, then adjust the enclosing loop's saved indices. */
      if (loops[j].body_start > loops[i].body_start) {
        loop_t tmp = loops[i];
        loops[i] = loops[j];
        loops[j] = tmp;
      }
    }
  }

  /* For each loop, find invariant instructions and hoist them. */
  for (size_t li = 0; li < loop_count; ++li) {
    loop_t *lp = &loops[li];
    size_t body_end = lp->back_edge;

    /* Collect instructions to hoist (in reverse order so inserts don't
     * invalidate earlier indices). */
    typedef struct {
      size_t orig_idx;
      nyir_inst_t inst;
    } hoist_t;
    hoist_t *to_hoist = NULL;
    size_t hoist_count = 0;
    size_t hoist_cap = 0;

    for (size_t i = lp->body_start; i <= body_end; ++i) {
      nyir_inst_t *in = &f->data[i];
      if (in->op == NYIR_LABEL || in->op == NYIR_PHI ||
          in->op == NYIR_BR || in->op == NYIR_BR_IF ||
          in->op == NYIR_RET ||
          in->op == NYIR_CALL || in->op == NYIR_STORE_I64 ||
          in->op == NYIR_ADDR_LOCAL || in->op == NYIR_STORE_I64 ||
          in->op == NYIR_STORE_LOCAL)
        continue;
      if (in->dst < 0)
        continue;
      /* Check if all inputs are defined outside the loop. */
      bool all_outside = true;
      int inputs[] = {in->a, in->b, in->c, in->d, in->e, in->f};
      for (int k = 0; k < 6; ++k) {
        if (inputs[k] < 0)
          continue;
        /* Is this input defined inside the loop body? */
        for (size_t j = lp->body_start; j <= body_end; ++j) {
          if (f->data[j].dst == inputs[k]) {
            all_outside = false;
            break;
          }
        }
        if (!all_outside)
          break;
      }
      if (!all_outside)
        continue;
      /* Don't hoist LOAD_LOCAL — it's cheap and can be eliminated
       * by redundant_load_elim instead. */
      if (in->op == NYIR_LOAD_LOCAL)
        continue;
      if (hoist_count == hoist_cap) {
        size_t cap = hoist_cap ? hoist_cap * 2 : 16;
        if (cap < hoist_cap || cap > SIZE_MAX / sizeof(*to_hoist)) {
          free(to_hoist);
          free(loops);
          nyir_cfg_free(&cfg);
          return false;
        }
        hoist_t *grown = realloc(to_hoist, cap * sizeof(*to_hoist));
        if (!grown) {
          free(to_hoist);
          free(loops);
          nyir_cfg_free(&cfg);
          return false;
        }
        to_hoist = grown;
        hoist_cap = cap;
      }
      to_hoist[hoist_count++] = (hoist_t){i, *in};
    }

    if (hoist_count == 0) {
      free(to_hoist);
      continue;
    }
    /* Mark all originals before moving the instruction array. Repeated
     * insertions used stale saved indices after the first memmove, which could
     * discard unrelated definitions and leave invalid NYIR at O3. Hoisting as
     * one contiguous batch is both linear and keeps source ordering intact. */
    for (size_t h = 0; h < hoist_count; ++h)
      f->data[to_hoist[h].orig_idx] =
          (nyir_inst_t){.op = NYIR_NOP, .dst = -1, .a = -1, .b = -1};
    size_t insert_pos = lp->head_idx;
    if (hoist_count > SIZE_MAX - f->len) {
      free(to_hoist);
      free(loops);
      nyir_cfg_free(&cfg);
      return false;
    }
    size_t need = f->len + (size_t)hoist_count;
    if (need > f->cap) {
      size_t new_cap = f->cap ? f->cap : 64;
      while (new_cap < need) {
        if (new_cap > SIZE_MAX / 2) {
          free(to_hoist);
          free(loops);
          nyir_cfg_free(&cfg);
          return false;
        }
        new_cap *= 2;
      }
      if (new_cap > SIZE_MAX / sizeof(*f->data)) {
        free(to_hoist);
        free(loops);
        nyir_cfg_free(&cfg);
        return false;
      }
      nyir_inst_t *p = realloc(f->data, new_cap * sizeof(*p));
      if (!p) {
        free(to_hoist);
        free(loops);
        nyir_cfg_free(&cfg);
        return false;
      }
      f->data = p;
      f->cap = new_cap;
    }
    memmove(&f->data[insert_pos + (size_t)hoist_count],
            &f->data[insert_pos],
            (f->len - insert_pos) * sizeof(*f->data));
    for (size_t h = 0; h < hoist_count; ++h)
      f->data[insert_pos + (size_t)h] = to_hoist[h].inst;
    f->len = need;
    free(to_hoist);
    for (size_t k = li; k < loop_count; ++k) {
      if (loops[k].head_idx >= insert_pos) loops[k].head_idx += (size_t)hoist_count;
      if (loops[k].body_start >= insert_pos) loops[k].body_start += (size_t)hoist_count;
      if (loops[k].back_edge >= insert_pos) loops[k].back_edge += (size_t)hoist_count;
    }
  }
  free(loops);
  nyir_cfg_free(&cfg);
  return true;
}
