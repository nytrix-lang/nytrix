#include "code/native/ir/internal.h"
#include "code/native/ir.h"
#include "base/compat.h"
#include "base/common.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Advanced optimizer seeds for the tiered-compiler roadmap.           */
/* Each pass is intentionally bounded, verifier-friendly, and gated by */
/* opt level. Full research-scale versions stay future work.           */
/* ------------------------------------------------------------------ */

/* Effect / alias summary: collapse per-instruction effects into one mask. */
bool nyir_effect_summary(const nyir_func_t *f, unsigned *out_mask,
                           size_t *out_reads, size_t *out_writes,
                           size_t *out_calls) {
  if (!f || !out_mask)
    return false;
  unsigned mask = 0;
  size_t reads = 0, writes = 0, calls = 0;
  for (size_t i = 0; i < f->len; ++i) {
    unsigned e = f->data[i].effects | nyir_inst_effects(&f->data[i]);
    mask |= e;
    if (e & (NYIR_EFFECT_READ_LOCAL | NYIR_EFFECT_READ_MEMORY))
      reads++;
    if (e & (NYIR_EFFECT_WRITE_LOCAL | NYIR_EFFECT_WRITE_MEMORY))
      writes++;
    if (e & NYIR_EFFECT_CALL)
      calls++;
  }
  *out_mask = mask;
  if (out_reads)
    *out_reads = reads;
  if (out_writes)
    *out_writes = writes;
  if (out_calls)
    *out_calls = calls;
  return true;
}

/* Null/alignment fact seed: after `x != 0` / `x > 0` compare+branch, mark the
 * compared value's range as excluding zero when the true edge is taken. We
 * only strengthen instruction-local range metadata on the compare result and
 * on proven non-zero AND masks (low-bit alignment). */
bool nyir_null_align_facts(nyir_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;
  nyir_value_fact_t *facts =
      calloc((size_t)f->next_value, sizeof(*facts));
  if (!facts || !nyir_analyze_values(f, facts, (size_t)f->next_value, NULL, 0)) {
    free(facts);
    return false;
  }
  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    /* (x & (align-1)) == 0 for power-of-two align → alignment fact on range of
     * the AND result is already 0..mask; nothing to rewrite. Strengthen known
     * non-null: if a value is proven > 0 by range, attach min=1 when missing. */
    if (in->dst >= 0 && in->dst < f->next_value) {
      const nyir_range_t *r = &facts[in->dst].range;
      if (r->has_min && r->min > 0 && !in->range.has_min) {
        in->range.has_min = true;
        in->range.min = r->min;
        if (r->has_max) {
          in->range.has_max = true;
          in->range.max = r->max;
        }
      }
    }
    /* Pointer-ish: load of local that was stored from a non-zero const. */
    if (in->op == NYIR_AND_I64 && in->b >= 0 && in->b < f->next_value &&
        facts[in->b].known_const) {
      int64_t mask = facts[in->b].const_value;
      if (mask > 0 && (mask & (mask + 1)) == 0) {
        /* mask is 2^n - 1: result is 0..mask (alignment residue). */
        in->range.has_min = true;
        in->range.has_max = true;
        in->range.min = 0;
        in->range.max = mask;
      }
    }
  }
  free(facts);
  return true;
}

/* Induction simplification seed: i = i + C with C const → keep as add (already
 * strength-reduced when C is power-of-two related). Detect `x - x` style and
 * `phi` self-add of constant step already handled by SCCP/peephole. Here we
 * fold `(i + c) - c` and `(i - c) + c` via local e-graph style peep. */
bool nyir_iv_simplify(nyir_func_t *f) {
  if (!f || f->len < 2)
    return true;
  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    if (in->dst < 0 || in->a < 0 || in->b < 0)
      continue;
    /* (x + c) - c  or  (x - c) + c → x when c is the same SSA const use. */
    if (in->op != NYIR_ADD_I64 && in->op != NYIR_SUB_I64)
      continue;
    /* Look for def of a as ADD/SUB with same const b. */
    for (size_t j = i; j > 0; --j) {
      const nyir_inst_t *def = &f->data[j - 1];
      if (def->dst != in->a)
        continue;
      if (def->op == NYIR_ADD_I64 && in->op == NYIR_SUB_I64 &&
          def->b == in->b && def->b >= 0) {
        *in = (nyir_inst_t){.op = NYIR_COPY, .dst = in->dst, .a = def->a,
                              .b = -1};
      } else if (def->op == NYIR_SUB_I64 && in->op == NYIR_ADD_I64 &&
                 def->b == in->b && def->b >= 0) {
        *in = (nyir_inst_t){.op = NYIR_COPY, .dst = in->dst, .a = def->a,
                              .b = -1};
      }
      break;
    }
  }
  return true;
}

/* Bounds-check elimination seed: `if ((x & m) < lim)` when m+1 is power of two
 * and m < lim, the compare is always true → const 1. */
bool nyir_bounds_check_elim(nyir_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;
  bool *known = calloc((size_t)f->next_value, sizeof(bool));
  int64_t *value = calloc((size_t)f->next_value, sizeof(int64_t));
  if (!known || !value) {
    free(known);
    free(value);
    return false;
  }
  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    if (in->op == NYIR_CONST_I64 && in->dst >= 0 &&
        in->dst < f->next_value) {
      known[in->dst] = true;
      value[in->dst] = in->imm;
    }
    if (in->op != NYIR_CMP_I64 || in->a < 0 || in->b < 0)
      continue;
    if (in->cmp != NYIR_CMP_LT && in->cmp != NYIR_CMP_LE)
      continue;
    if (!known[in->b])
      continue;
    int64_t lim = value[in->b];
    /* Find if a is AND with const mask. */
    for (size_t j = i; j > 0; --j) {
      const nyir_inst_t *def = &f->data[j - 1];
      if (def->dst != in->a)
        continue;
      if (def->op == NYIR_AND_I64 && def->b >= 0 && known[def->b]) {
        int64_t mask = value[def->b];
        if (mask >= 0 && lim > 0 &&
            (in->cmp == NYIR_CMP_LT ? mask < lim : mask <= lim)) {
          *in = (nyir_inst_t){.op = NYIR_CONST_I64,
                                .dst = in->dst,
                                .a = -1,
                                .b = -1,
                                .imm = 1,
                                .range = {.has_min = true,
                                          .has_max = true,
                                          .min = 1,
                                          .max = 1}};
        }
      }
      break;
    }
  }
  free(known);
  free(value);
  return true;
}

/* Local e-graph seed: reassociate pure (a+b)+c chains and commute consts to
 * the right on ADD/MUL for CSE friendliness. Bounded single forward pass. */
bool nyir_egraph_local(nyir_func_t *f) {
  if (!f)
    return true;
  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    if ((in->op != NYIR_ADD_I64 && in->op != NYIR_MUL_I64) || in->a < 0 ||
        in->b < 0)
      continue;
    /* Canonicalize: if a is const and b is not, swap (commute). */
    bool a_const = false, b_const = false;
    for (size_t j = i; j > 0; --j) {
      const nyir_inst_t *def = &f->data[j - 1];
      if (def->dst == in->a && def->op == NYIR_CONST_I64)
        a_const = true;
      if (def->dst == in->b && def->op == NYIR_CONST_I64)
        b_const = true;
      if (def->dst == in->a || def->dst == in->b) {
        if ((def->dst == in->a && !a_const) || (def->dst == in->b && !b_const)) {
          /* keep scanning until both defs found? simplified: break per match */
        }
      }
    }
    /* Simpler scan: */
    a_const = b_const = false;
    for (size_t j = 0; j < i; ++j) {
      if (f->data[j].op == NYIR_CONST_I64 && f->data[j].dst == in->a)
        a_const = true;
      if (f->data[j].op == NYIR_CONST_I64 && f->data[j].dst == in->b)
        b_const = true;
    }
    if (a_const && !b_const) {
      int t = in->a;
      in->a = in->b;
      in->b = t;
    }
  }
  return true;
}

/* apply_rules / ny_isle_apply_nir live in isle.c (shared NYIR + machine form). */

/* Small pure-function inliner: replace CALL to a known callee with a copy of
 * its straight-line body when body is tiny and pure. */
static const nyir_inline_callee_t *nyir_inline_callees;
static size_t nyir_inline_callee_count;

void nyir_set_inline_callees(const nyir_inline_callee_t *callees,
                               size_t count) {
  nyir_inline_callees = callees;
  nyir_inline_callee_count = count;
}

static const nyir_func_t *nyir_find_inline_callee(const char *name) {
  if (!name)
    return NULL;
  for (size_t i = 0; i < nyir_inline_callee_count; ++i) {
    if (nyir_inline_callees[i].name &&
        strcmp(nyir_inline_callees[i].name, name) == 0)
      return nyir_inline_callees[i].func;
  }
  return NULL;
}

static bool nyir_func_is_inline_candidate(const nyir_func_t *f) {
  if (!f || f->len == 0 || f->len > 32)
    return false;
  if (f->len > 12 && !ny_native_profile_should_inline(f->len))
    return false;
  if (!nyir_is_pure_i64_straightline(f))
    return false;
  for (size_t i = 0; i < f->len; ++i) {
    if (f->data[i].op == NYIR_CALL || f->data[i].op == NYIR_PHI ||
        f->data[i].op == NYIR_BR || f->data[i].op == NYIR_BR_IF ||
        f->data[i].op == NYIR_LABEL)
      return false;
  }
  return true;
}

static bool nyir_is_i64_binop(nyir_op_t op) {
  return op == NYIR_ADD_I64 || op == NYIR_MUL_I64 || op == NYIR_SUB_I64 ||
         op == NYIR_AND_I64 || op == NYIR_OR_I64 || op == NYIR_XOR_I64 ||
         op == NYIR_SHL_I64 || op == NYIR_SAR_I64 || op == NYIR_DIV_I64 ||
         op == NYIR_MOD_I64;
}

/* Ensure room for `extra` more instructions at index `at` (shifts tail). */
static bool nyir_ensure_insert(nyir_func_t *f, size_t at, size_t extra) {
  if (!f || extra == 0)
    return true;
  if (f->len + extra > f->cap) {
    size_t nc = f->cap ? f->cap * 2 : 64;
    while (nc < f->len + extra)
      nc *= 2;
    nyir_inst_t *nd = realloc(f->data, nc * sizeof(*nd));
    if (!nd)
      return false;
    f->data = nd;
    f->cap = nc;
  }
  memmove(&f->data[at + extra], &f->data[at],
          (f->len - at) * sizeof(nyir_inst_t));
  f->len += extra;
  return true;
}

bool nyir_inline_small(nyir_func_t *f) {
  if (!f || nyir_inline_callee_count == 0)
    return true;
  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    if (in->op != NYIR_CALL || !in->symbol)
      continue;
    const nyir_func_t *callee = nyir_find_inline_callee(in->symbol);
    if (!callee || !nyir_func_is_inline_candidate(callee))
      continue;

    int64_t ret_imm = 0;
    bool ret_const = false;
    int ret_src = -1;
    for (size_t k = 0; k < callee->len; ++k) {
      const nyir_inst_t *c = &callee->data[k];
      if (c->op != NYIR_RET || c->a < 0)
        continue;
      ret_src = c->a;
      for (size_t j = 0; j < k; ++j) {
        if (callee->data[j].op == NYIR_CONST_I64 &&
            callee->data[j].dst == c->a) {
          ret_imm = callee->data[j].imm;
          ret_const = true;
        }
      }
    }
    if (ret_const && in->dst >= 0) {
      *in = (nyir_inst_t){.op = NYIR_CONST_I64,
                            .dst = in->dst,
                            .imm = ret_imm,
                            .range = {.has_min = true,
                                      .has_max = true,
                                      .min = ret_imm,
                                      .max = ret_imm}};
      continue;
    }

    /* Single-expression callees: load.local 0; op; ret — specialize when
     * one arg and pure unary/binary on that arg only. */
    if (callee->len == 3 && in->a >= 0 && in->dst >= 0 &&
        callee->data[0].op == NYIR_LOAD_LOCAL && callee->data[0].imm == 0 &&
        callee->data[2].op == NYIR_RET &&
        callee->data[2].a == callee->data[1].dst) {
      const nyir_inst_t *op = &callee->data[1];
      if (nyir_is_i64_binop(op->op)) {
        /* Need const second operand in callee. */
        int64_t imm = 0;
        bool has_imm = false;
        if (op->a == callee->data[0].dst && op->b >= 0) {
          for (size_t j = 0; j < callee->len; ++j)
            if (callee->data[j].op == NYIR_CONST_I64 &&
                callee->data[j].dst == op->b) {
              imm = callee->data[j].imm;
              has_imm = true;
            }
        }
        if (has_imm) {
          /* Expand call to: const imm; op arg, imm → dst */
          if (!nyir_ensure_insert(f, i, 1))
            return false;
          int cdst = f->next_value++;
          int adst = in->dst;
          int arg = in->a;
          nyir_op_t oop = op->op;
          f->data[i] = (nyir_inst_t){.op = NYIR_CONST_I64,
                                        .dst = cdst,
                                        .imm = imm,
                                        .range = {.has_min = true,
                                                  .has_max = true,
                                                  .min = imm,
                                                  .max = imm}};
          f->data[i + 1] = (nyir_inst_t){
              .op = oop, .dst = adst, .a = arg, .b = cdst};
          i++;
          continue;
        }
      }
    }

    /* Two-arg body splice: load.local 0; load.local 1; binop; ret
     * → emit binop(call_arg0, call_arg1) at the call site. */
    if (callee->len == 4 && in->a >= 0 && in->b >= 0 && in->dst >= 0 &&
        callee->data[0].op == NYIR_LOAD_LOCAL && callee->data[0].imm == 0 &&
        callee->data[1].op == NYIR_LOAD_LOCAL && callee->data[1].imm == 1 &&
        nyir_is_i64_binop(callee->data[2].op) &&
        callee->data[2].a == callee->data[0].dst &&
        callee->data[2].b == callee->data[1].dst &&
        callee->data[3].op == NYIR_RET &&
        callee->data[3].a == callee->data[2].dst) {
      *in = (nyir_inst_t){.op = callee->data[2].op,
                            .dst = in->dst,
                            .a = in->a,
                            .b = in->b};
      continue;
    }

    /* Three-inst with const in middle: load0; const; binop; ret (len 4). */
    if (callee->len == 4 && in->a >= 0 && in->dst >= 0 &&
        callee->data[0].op == NYIR_LOAD_LOCAL && callee->data[0].imm == 0 &&
        callee->data[1].op == NYIR_CONST_I64 &&
        nyir_is_i64_binop(callee->data[2].op) &&
        callee->data[2].a == callee->data[0].dst &&
        callee->data[2].b == callee->data[1].dst &&
        callee->data[3].op == NYIR_RET &&
        callee->data[3].a == callee->data[2].dst) {
      if (!nyir_ensure_insert(f, i, 1))
        return false;
      int cdst = f->next_value++;
      int adst = in->dst;
      int arg = in->a;
      int64_t imm = callee->data[1].imm;
      nyir_op_t oop = callee->data[2].op;
      f->data[i] = (nyir_inst_t){.op = NYIR_CONST_I64,
                                    .dst = cdst,
                                    .imm = imm,
                                    .range = {.has_min = true,
                                              .has_max = true,
                                              .min = imm,
                                              .max = imm}};
      f->data[i + 1] =
          (nyir_inst_t){.op = oop, .dst = adst, .a = arg, .b = cdst};
      i++;
      continue;
    }

    /* General pure body splice (≤12 insts): map load.local p → arg, remap
     * temps, replace CALL with inlined stream ending in COPY to call dst. */
    if (callee->len <= 14 && callee->len >= 2 && in->dst >= 0) {
      int args[6] = {in->a, in->b, in->c, in->d, in->e, in->f};
      int arity = (int)in->imm;
      if (arity < 0) arity = 0;
      if (arity > 6) arity = 6;
      bool ok_s = true;
      int max_loc = -1;
      for (size_t k = 0; k < callee->len; ++k) {
        const nyir_inst_t *c = &callee->data[k];
        if (c->op == NYIR_NOP || c->op == NYIR_RET) continue;
        if (c->op == NYIR_LOAD_LOCAL) {
          if (c->imm > max_loc) max_loc = (int)c->imm;
          continue;
        }
        if (c->op == NYIR_STORE_LOCAL) { ok_s = false; break; }
        if (c->op == NYIR_CONST_I64 || c->op == NYIR_COPY ||
            nyir_is_i64_binop(c->op) || c->op == NYIR_CMP_I64)
          continue;
        ok_s = false;
        break;
      }
      if (ok_s && max_loc < arity) {
        size_t body_n = 0;
        for (size_t k = 0; k < callee->len; ++k)
          if (callee->data[k].op != NYIR_RET && callee->data[k].op != NYIR_NOP)
            body_n++;
        if (body_n > 0 && body_n <= 12) {
          int *vmap = NULL;
          if (callee->next_value > 0) {
            vmap = calloc((size_t)callee->next_value, sizeof(int));
            if (!vmap) return false;
            for (int v = 0; v < callee->next_value; ++v) vmap[v] = -1;
          }
          if (!nyir_ensure_insert(f, i, body_n)) {
            free(vmap);
            return false;
          }
          size_t w = i;
          int call_dst = in->dst;
          for (size_t k = 0; k < callee->len; ++k) {
            const nyir_inst_t *c = &callee->data[k];
            if (c->op == NYIR_NOP) continue;
            if (c->op == NYIR_RET) {
              int src = -1;
              if (c->a >= 0 && vmap && c->a < callee->next_value && vmap[c->a] >= 0)
                src = vmap[c->a];
              else if (c->a >= 0 && c->a < arity)
                src = args[c->a];
              if (src >= 0)
                f->data[w++] = (nyir_inst_t){.op = NYIR_COPY, .dst = call_dst, .a = src, .b = -1};
              continue;
            }
            if (c->op == NYIR_LOAD_LOCAL && c->imm >= 0 && (int)c->imm < arity) {
              int nd = f->next_value++;
              if (vmap && c->dst >= 0 && c->dst < callee->next_value)
                vmap[c->dst] = nd;
              f->data[w++] = (nyir_inst_t){.op = NYIR_COPY, .dst = nd, .a = args[c->imm], .b = -1};
              continue;
            }
            nyir_inst_t ni = *c;
            if (ni.dst >= 0) {
              int nd = f->next_value++;
              if (vmap && ni.dst < callee->next_value)
                vmap[ni.dst] = nd;
              ni.dst = nd;
            }
            if (ni.a >= 0 && vmap && ni.a < callee->next_value && vmap[ni.a] >= 0)
              ni.a = vmap[ni.a];
            if (ni.b >= 0 && vmap && ni.b < callee->next_value && vmap[ni.b] >= 0)
              ni.b = vmap[ni.b];
            f->data[w++] = ni;
          }
          for (size_t z = w; z <= i + body_n && z < f->len; ++z)
            f->data[z] = (nyir_inst_t){.op = NYIR_NOP, .dst = -1, .a = -1, .b = -1};
          free(vmap);
          i = w > 0 ? w - 1 : i;
          continue;
        }
      }
    }
    (void)ret_src;
  }
  return true;
}

bool nyir_loop_unroll(nyir_func_t *f) {
  /* Full unroll of tiny counted loops: i starts 0, i < C (const 1..4),
   * body has no nested CFG and ends with br back to the header label.
   * Duplicates the body (C-1) extra times before the latch and forces the
   * compare to fail after the last copy by rewriting the latch to jump to
   * the exit edge. Conservative: only pure scalar bodies. */
  if (!f || f->len < 8)
    return true;
  for (size_t i = 0; i < f->len; ++i)
    if (f->data[i].op == NYIR_PHI)
      return true;

  for (size_t bi = 0; bi < f->len; ++bi) {
    if (f->data[bi].op != NYIR_BR_IF)
      continue;
    int cond = f->data[bi].a;
    int64_t body_lab = f->data[bi].imm;
    if (cond < 0)
      continue;
    /* Find icmp.lt producing cond with const RHS trip. */
    int64_t trip = -1;
    int iv = -1;
    for (size_t j = 0; j < bi; ++j) {
      const nyir_inst_t *c = &f->data[j];
      if (c->op == NYIR_CMP_I64 && c->dst == cond && c->cmp == NYIR_CMP_LT) {
        iv = c->a;
        for (size_t k = 0; k < j; ++k)
          if (f->data[k].op == NYIR_CONST_I64 && f->data[k].dst == c->b) {
            trip = f->data[k].imm;
            break;
          }
        break;
      }
    }
    uint64_t loop_heat = ny_native_profile_loop_hot(bi);
    int64_t max_trip = loop_heat >= 100 ? 16 : loop_heat >= 16 ? 12 : 8;
    if (trip < 2 || trip > max_trip || iv < 0)
      continue;

    /* Body label region. */
    size_t body_start = 0, body_end = 0;
    bool have = false;
    for (size_t j = 0; j < f->len; ++j) {
      if (f->data[j].op == NYIR_LABEL && f->data[j].imm == body_lab) {
        body_start = j + 1;
        for (size_t k = body_start; k < f->len; ++k) {
          if (f->data[k].op == NYIR_BR && f->data[k].imm != body_lab) {
            /* unexpected */
          }
          if (f->data[k].op == NYIR_BR || f->data[k].op == NYIR_BR_IF ||
              f->data[k].op == NYIR_LABEL || f->data[k].op == NYIR_RET) {
            body_end = k;
            size_t body_limit = loop_heat >= 100 ? 32 : 16;
            have = body_end > body_start &&
                   (body_end - body_start) <= body_limit;
            break;
          }
        }
        break;
      }
    }
    if (!have)
      continue;
    /* Latch must be br back to a header (label before the br_if). */
    if (f->data[body_end].op != NYIR_BR)
      continue;
    bool pure = true;
    for (size_t k = body_start; k < body_end; ++k) {
      nyir_op_t op = f->data[k].op;
      /* Local loads/stores are safe across sequential copies: each copy keeps
       * the same local slot while the value map remaps its SSA operands.
       * Address-taken memory still needs alias-aware rematerialization. */
      if (op == NYIR_CALL || op == NYIR_PHI || op == NYIR_BR ||
          op == NYIR_BR_IF || op == NYIR_LABEL || op == NYIR_LOAD_I64 ||
          op == NYIR_STORE_I64 || op == NYIR_ADDR_LOCAL) {
        pure = false;
        break;
      }
    }
    if (!pure)
      continue;

    size_t body_len = body_end - body_start;
    size_t extra = (size_t)(trip - 1) * body_len;
    if (extra == 0)
      continue;
    if (f->len + extra > f->cap) {
      size_t nc = f->cap ? f->cap * 2 : 64;
      while (nc < f->len + extra)
        nc *= 2;
      nyir_inst_t *nd = realloc(f->data, nc * sizeof(*nd));
      if (!nd)
        return false;
      f->data = nd;
      f->cap = nc;
    }
    /* Insert (trip-1) body copies just before the latch BR with a full
     * mid-body value map so defs in the copy remap uses in later ops. */
    size_t insert_at = body_end;
    memmove(&f->data[insert_at + extra], &f->data[insert_at],
            (f->len - insert_at) * sizeof(nyir_inst_t));
    f->len += extra;
    for (int t = 1; t < (int)trip; ++t) {
      size_t base = insert_at + (size_t)(t - 1) * body_len;
      /* Map original value id → new value id for this copy only. */
      int *vmap = NULL;
      size_t vmap_n = (size_t)(f->next_value + (int)body_len + 8);
      if (vmap_n < 8)
        vmap_n = 8;
      vmap = malloc(vmap_n * sizeof(int));
      if (!vmap)
        return false;
      for (size_t m = 0; m < vmap_n; ++m)
        vmap[m] = -1;
      for (size_t k = 0; k < body_len; ++k) {
        nyir_inst_t in = f->data[body_start + k];
        int old_dst = in.dst;
        if (in.dst >= 0) {
          int nd = f->next_value++;
          if ((size_t)old_dst >= vmap_n) {
            size_t nn = (size_t)old_dst + 8;
            int *nv = realloc(vmap, nn * sizeof(int));
            if (!nv) {
              free(vmap);
              return false;
            }
            for (size_t m = vmap_n; m < nn; ++m)
              nv[m] = -1;
            vmap = nv;
            vmap_n = nn;
          }
          vmap[old_dst] = nd;
          in.dst = nd;
        }
        /* Remap operand uses that were defined earlier in this body copy. */
        if (in.a >= 0 && (size_t)in.a < vmap_n && vmap[in.a] >= 0)
          in.a = vmap[in.a];
        if (in.b >= 0 && (size_t)in.b < vmap_n && vmap[in.b] >= 0)
          in.b = vmap[in.b];
        if (in.c >= 0 && (size_t)in.c < vmap_n && vmap[in.c] >= 0)
          in.c = vmap[in.c];
        if (in.d >= 0 && (size_t)in.d < vmap_n && vmap[in.d] >= 0)
          in.d = vmap[in.d];
        f->data[base + k] = in;
      }
      free(vmap);
    }
    /* After unrolling copies, force exit: rewrite original br_if to br exit
     * by making condition always false — replace br_if with nop and keep
     * fallthrough; change latch br to point to fallthrough of br_if.
     * Safer: set trip const to 0 so first test fails after copies run...
     * Copies still sit inside the loop. Better: change latch to br exit_lab.
     * exit is the false edge of br_if = next label after br_if. */
    size_t br_if_at = bi;
    /* After insert, bi may still be valid if bi < body_end */
    if (br_if_at >= body_end)
      br_if_at = bi; /* br_if is before body, unshifted */
    int64_t exit_lab = -1;
    for (size_t j = br_if_at + 1; j < f->len; ++j) {
      if (f->data[j].op == NYIR_NOP)
        continue;
      if (f->data[j].op == NYIR_BR) {
        exit_lab = f->data[j].imm;
        break;
      }
      if (f->data[j].op == NYIR_LABEL) {
        exit_lab = f->data[j].imm;
        break;
      }
      break;
    }
    if (exit_lab >= 0) {
      /* Point latch at exit so after unrolled bodies we leave the loop. */
      size_t latch = body_end + extra;
      if (latch < f->len && f->data[latch].op == NYIR_BR)
        f->data[latch].imm = exit_lab;
      /* Disable original loop back by making br_if never take body again:
       * replace with br exit (skip body on re-entry). Header still runs once
       * then exits — but we already executed body trip times via copies
       * only if we entered body once. Structure:
       *   header; br_if body; exit_path; body; [copies]; latch->header
       * After: header; br_if body; exit; body; copies; latch->exit
       * First iter: enter body, run body+copies (=trip bodies), go exit. OK.
       */
    }
    return true; /* one loop only */
  }
  return true;
}

bool nyir_algebraic_combine(nyir_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;
  bool *known = calloc((size_t)f->next_value, sizeof(bool));
  int64_t *value = calloc((size_t)f->next_value, sizeof(int64_t));
  if (!known || !value) {
    free(known);
    free(value);
    return false;
  }
  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    if (in->op == NYIR_CONST_I64 && in->dst >= 0 &&
        (size_t)in->dst < (size_t)f->next_value) {
      known[in->dst] = true;
      value[in->dst] = in->imm;
    }
    /* (x + c1) + c2 → x + (c1+c2) */
    if (in->op == NYIR_ADD_I64 && in->a >= 0 && in->b >= 0 &&
        known[in->b]) {
      for (size_t j = 0; j < i; ++j) {
        nyir_inst_t *p = &f->data[j];
        if (p->op == NYIR_ADD_I64 && p->dst == in->a && p->b >= 0 &&
            known[p->b] && p->a >= 0) {
          int64_t sum = value[p->b] + value[in->b];
          int cdst = f->next_value++;
          if (f->len + 1 > f->cap) {
            size_t nc = f->cap ? f->cap * 2 : 64;
            nyir_inst_t *nd = realloc(f->data, nc * sizeof(*nd));
            if (!nd) {
              free(known);
              free(value);
              return false;
            }
            f->data = nd;
            f->cap = nc;
            in = &f->data[i];
            p = &f->data[j];
          }
          memmove(&f->data[i + 1], &f->data[i],
                  (f->len - i) * sizeof(nyir_inst_t));
          f->len++;
          f->data[i] =
              (nyir_inst_t){.op = NYIR_CONST_I64,
                              .dst = cdst,
                              .imm = sum,
                              .range = {.has_min = true,
                                        .has_max = true,
                                        .min = sum,
                                        .max = sum}};
          f->data[i + 1] = (nyir_inst_t){
              .op = NYIR_ADD_I64, .dst = in->dst, .a = p->a, .b = cdst};
          bool *new_known = realloc(known, (size_t)f->next_value * sizeof(bool));
          int64_t *new_value = realloc(value, (size_t)f->next_value * sizeof(int64_t));
          if (!new_known || !new_value) {
            free(new_known ? new_known : known);
            free(new_value ? new_value : value);
            return false;
          }
          known = new_known;
          value = new_value;
          known[cdst] = true;
          value[cdst] = sum;
          i++;
          break;
        }
      }
    }
    /* (x & m1) & m2 → x & (m1&m2) */
    if (in->op == NYIR_AND_I64 && in->a >= 0 && in->b >= 0 && known[in->b]) {
      for (size_t j = 0; j < i; ++j) {
        nyir_inst_t *p = &f->data[j];
        if (p->op == NYIR_AND_I64 && p->dst == in->a && p->b >= 0 &&
            known[p->b] && p->a >= 0) {
          int64_t m = value[p->b] & value[in->b];
          int cdst = f->next_value++;
          if (f->len + 1 > f->cap) {
            size_t nc = f->cap ? f->cap * 2 : 64;
            nyir_inst_t *nd = realloc(f->data, nc * sizeof(*nd));
            if (!nd) {
              free(known);
              free(value);
              return false;
            }
            f->data = nd;
            f->cap = nc;
            in = &f->data[i];
            p = &f->data[j];
          }
          memmove(&f->data[i + 1], &f->data[i],
                  (f->len - i) * sizeof(nyir_inst_t));
          f->len++;
          f->data[i] =
              (nyir_inst_t){.op = NYIR_CONST_I64,
                              .dst = cdst,
                              .imm = m,
                              .range = {.has_min = true,
                                        .has_max = true,
                                        .min = m,
                                        .max = m}};
          f->data[i + 1] = (nyir_inst_t){
              .op = NYIR_AND_I64, .dst = in->dst, .a = p->a, .b = cdst};
          bool *new_known = realloc(known, (size_t)f->next_value * sizeof(bool));
          int64_t *new_value = realloc(value, (size_t)f->next_value * sizeof(int64_t));
          if (!new_known || !new_value) {
            free(new_known ? new_known : known);
            free(new_value ? new_value : value);
            return false;
          }
          known = new_known;
          value = new_value;
          known[cdst] = true;
          value[cdst] = m;
          i++;
          break;
        }
      }
    }
  }
  free(known);
  free(value);
  return true;
}

bool nyir_double_neg(nyir_func_t *f) {
  if (!f)
    return true;
  /* ~(~x) / -(-x) via xor -1 twice or sub 0 patterns: x ^ -1 twice → copy x */
  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    if (in->op != NYIR_XOR_I64 || in->a < 0 || in->b < 0)
      continue;
    /* find if b is const -1 and a is also xor with -1 */
    bool b_neg1 = false;
    for (size_t j = 0; j < i; ++j)
      if (f->data[j].op == NYIR_CONST_I64 && f->data[j].dst == in->b &&
          f->data[j].imm == -1)
        b_neg1 = true;
    if (!b_neg1)
      continue;
    for (size_t j = 0; j < i; ++j) {
      nyir_inst_t *p = &f->data[j];
      if (p->op == NYIR_XOR_I64 && p->dst == in->a && p->a >= 0 && p->b >= 0) {
        bool pb = false;
        for (size_t k = 0; k < j; ++k)
          if (f->data[k].op == NYIR_CONST_I64 && f->data[k].dst == p->b &&
              f->data[k].imm == -1)
            pb = true;
        if (pb) {
          *in = (nyir_inst_t){
              .op = NYIR_COPY, .dst = in->dst, .a = p->a, .b = -1};
          break;
        }
      }
    }
  }
  /* sub 0, x → neg; sub 0, (sub 0, x) → copy x */
  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    if (in->op != NYIR_SUB_I64 || in->a < 0 || in->b < 0)
      continue;
    bool a0 = false;
    for (size_t j = 0; j < i; ++j)
      if (f->data[j].op == NYIR_CONST_I64 && f->data[j].dst == in->a &&
          f->data[j].imm == 0)
        a0 = true;
    if (!a0)
      continue;
    for (size_t j = 0; j < i; ++j) {
      nyir_inst_t *p = &f->data[j];
      if (p->op == NYIR_SUB_I64 && p->dst == in->b && p->a >= 0 && p->b >= 0) {
        bool pa0 = false;
        for (size_t k = 0; k < j; ++k)
          if (f->data[k].op == NYIR_CONST_I64 && f->data[k].dst == p->a &&
              f->data[k].imm == 0)
            pa0 = true;
        if (pa0) {
          *in = (nyir_inst_t){
              .op = NYIR_COPY, .dst = in->dst, .a = p->b, .b = -1};
          break;
        }
      }
    }
  }
  return true;
}

bool nyir_reassoc_add(nyir_func_t *f) {
  /* (a + b) + c with c const, b const already handled in algebraic; here
   * reassoc (a + k1) + (b + k2) is out of scope. Reassoc a + (b + k) →
   * (a + b) + k when k const for better fold. */
  if (!f || f->next_value <= 0)
    return true;
  bool *known = calloc((size_t)f->next_value, sizeof(bool));
  int64_t *value = calloc((size_t)f->next_value, sizeof(int64_t));
  if (!known || !value) {
    free(known);
    free(value);
    return false;
  }
  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    if (in->op == NYIR_CONST_I64 && in->dst >= 0 &&
        (size_t)in->dst < (size_t)f->next_value) {
      known[in->dst] = true;
      value[in->dst] = in->imm;
    }
    if (in->op != NYIR_ADD_I64 || in->a < 0 || in->b < 0)
      continue;
    /* a + (b + k) where right is add with const */
    for (size_t j = 0; j < i; ++j) {
      nyir_inst_t *r = &f->data[j];
      if (r->op == NYIR_ADD_I64 && r->dst == in->b && r->a >= 0 && r->b >= 0 &&
          known[r->b]) {
        /* emit t = a + r.a; dst = t + k */
        int tdst = f->next_value++;
        if (f->len + 1 > f->cap) {
          size_t nc = f->cap ? f->cap * 2 : 64;
          nyir_inst_t *nd = realloc(f->data, nc * sizeof(*nd));
          if (!nd) {
            free(known);
            free(value);
            return false;
          }
          f->data = nd;
          f->cap = nc;
          in = &f->data[i];
          r = &f->data[j];
        }
        memmove(&f->data[i + 1], &f->data[i],
                (f->len - i) * sizeof(nyir_inst_t));
        f->len++;
        int adst = in->dst;
        int aa = in->a;
        f->data[i] =
            (nyir_inst_t){.op = NYIR_ADD_I64, .dst = tdst, .a = aa, .b = r->a};
        f->data[i + 1] = (nyir_inst_t){
            .op = NYIR_ADD_I64, .dst = adst, .a = tdst, .b = r->b};
        i++;
        break;
      }
    }
  }
  free(known);
  free(value);
  return true;
}

bool nyir_rewrite_fuel(nyir_func_t *f) {
  /* Fuel-bounded fixpoint of identity rules + algebraic (Cranelift-inspired). */
  if (!f)
    return true;
  const int fuel_max = 8;
  for (int fuel = 0; fuel < fuel_max; ++fuel) {
    size_t before = f->len;
    if (!nyir_apply_rules(f) || !nyir_double_neg(f) ||
        !nyir_algebraic_combine(f) || !nyir_reassoc_add(f))
      return false;
    if (f->len == before)
      break;
  }
  return true;
}

bool nyir_escape_sroa(nyir_func_t *f) {
  if (!f || f->len == 0)
    return true;
  size_t n = nyir_max_local(f);
  if (!n)
    return true;
  bool *escaped = calloc(n, 1);
  bool *loaded = calloc(n, 1);
  bool *stored = calloc(n, 1);
  if (!escaped || !loaded || !stored) {
    free(escaped); free(loaded); free(stored);
    return false;
  }
  for (size_t i = 0; i < f->len; ++i) {
    const nyir_inst_t *in = &f->data[i];
    if (in->imm < 0 || (size_t)in->imm >= n)
      continue;
    if (in->op == NYIR_ADDR_LOCAL)
      escaped[in->imm] = true;
    else if (in->op == NYIR_LOAD_LOCAL)
      loaded[in->imm] = true;
    else if (in->op == NYIR_STORE_LOCAL)
      stored[in->imm] = true;
  }
  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    if (in->op == NYIR_STORE_LOCAL && in->imm >= 0 &&
        (size_t)in->imm < n && !escaped[in->imm] && !loaded[in->imm]) {
      /* Dead store to never-loaded local. */
      *in = (nyir_inst_t){.op = NYIR_NOP, .dst = -1, .a = -1, .b = -1};
    }
  }
  free(escaped); free(loaded); free(stored);
  return true;
}

/* Apply NyP1 profile: boost inline candidate size when edge density is high. */
static uint64_t ny_profile_edges;
static uint64_t ny_profile_steps;

void ny_native_profile_set_runtime(uint64_t edges, uint64_t steps) {
  ny_profile_edges = edges;
  ny_profile_steps = steps;
}

bool ny_native_profile_should_inline(size_t callee_insts) {
  /* Hot profiles allow larger callees (up to 32 insts). NyP-style density. */
  size_t lim = 12;
  if (ny_profile_steps > 10000 && ny_profile_edges > 100)
    lim = 32;
  else if (ny_profile_steps > 1000)
    lim = 20;
  return callee_insts > 0 && callee_insts <= lim;
}

/* Dense NyP-style ICP: count monomorphic direct CALL targets in the function.
 * When a single symbol dominates (>= 2 sites, or unique among multi-call),
 * boost profile so inline_small admits that callee's body. */
bool nyir_icp_profile(nyir_func_t *f) {
  if (!f || f->len == 0)
    return true;
  /* Collect up to 16 distinct direct call symbols and counts. */
  const char *syms[16];
  unsigned counts[16];
  size_t nsym = 0;
  size_t total_calls = 0;
  for (size_t i = 0; i < f->len; ++i) {
    if (f->data[i].op != NYIR_CALL || !f->data[i].symbol)
      continue;
    total_calls++;
    const char *s = f->data[i].symbol;
    size_t j;
    for (j = 0; j < nsym; ++j)
      if (strcmp(syms[j], s) == 0) {
        counts[j]++;
        break;
      }
    if (j == nsym && nsym < 16) {
      syms[nsym] = s;
      counts[nsym] = 1;
      nsym++;
    }
  }
  if (total_calls == 0)
    return true;
  /* Monomorphic: one target, or one target has >= 2/3 of direct calls. */
  unsigned best = 0;
  for (size_t j = 0; j < nsym; ++j)
    if (counts[j] > best)
      best = counts[j];
  bool mono = (nsym == 1 && total_calls >= 1) ||
              (best * 3 >= (unsigned)total_calls * 2 && best >= 2);
  if (mono) {
    uint64_t steps = ny_profile_steps < 2000 ? 2000 : ny_profile_steps;
    uint64_t edges = ny_profile_edges < 200 ? 200 : ny_profile_edges + best * 10;
    ny_native_profile_set_runtime(edges, steps);
  } else if (ny_profile_steps >= 100 && ny_profile_edges < 200) {
    ny_native_profile_set_runtime(ny_profile_edges + 100, ny_profile_steps);
  }
  return true;
}

uint64_t ny_native_profile_edges(void) { return ny_profile_edges; }
uint64_t ny_native_profile_steps(void) { return ny_profile_steps; }

/* Scalar replacement for non-escaped locals without address-taken: within each
 * straight-line region (between labels/branches/calls), promote store→load to
 * copy when the local is not escaped. Complements CFG MemorySSA joins. */
bool nyir_sroa_scalar(nyir_func_t *f) {
  if (!f || f->len == 0)
    return true;
  size_t n = nyir_max_local(f);
  if (!n)
    return true;
  bool *escaped = calloc(n, 1);
  int *def = malloc(n * sizeof(int));
  if (!escaped || !def) {
    free(escaped);
    free(def);
    return false;
  }
  for (size_t i = 0; i < f->len; ++i)
    if (f->data[i].op == NYIR_ADDR_LOCAL && f->data[i].imm >= 0 &&
        (size_t)f->data[i].imm < n)
      escaped[f->data[i].imm] = true;
  for (size_t i = 0; i < n; ++i)
    def[i] = -1;
  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    if (in->op == NYIR_LABEL || in->op == NYIR_BR || in->op == NYIR_BR_IF ||
        in->op == NYIR_CALL) {
      for (size_t l = 0; l < n; ++l)
        def[l] = -1;
      continue;
    }
    if (in->op == NYIR_STORE_LOCAL && in->imm >= 0 && (size_t)in->imm < n &&
        !escaped[in->imm] && in->a >= 0)
      def[in->imm] = in->a;
    else if (in->op == NYIR_LOAD_LOCAL && in->imm >= 0 &&
             (size_t)in->imm < n && !escaped[in->imm] && def[in->imm] >= 0 &&
             in->dst >= 0) {
      *in = (nyir_inst_t){
          .op = NYIR_COPY, .dst = in->dst, .a = def[in->imm], .b = -1};
    }
  }
  free(escaped);
  free(def);
  return true;
}

/* Hot/cold layout: invert br_if when the cold arm is a tiny ret/exit and the
 * true arm is larger so fallthrough hits the hot body. Also prefer the larger
 * arm as fallthrough when profile heat suggests inlining-scale bodies
 * (NyP-style edge preference without full ICP). */
static bool nyir_invert_br_if_at(nyir_func_t *f, size_t i) {
  if (!f || i + 1 >= f->len || f->data[i].op != NYIR_BR_IF ||
      f->data[i + 1].op != NYIR_BR)
    return false;
  int cond = f->data[i].a;
  int64_t lt = f->data[i].imm, lf = f->data[i + 1].imm;
  for (size_t j = 0; j < i; ++j) {
    if (f->data[j].op == NYIR_CMP_I64 && f->data[j].dst == cond) {
      nyir_cmp_t c = f->data[j].cmp;
      nyir_cmp_t inv = c;
      switch (c) {
      case NYIR_CMP_EQ:
        inv = NYIR_CMP_NE;
        break;
      case NYIR_CMP_NE:
        inv = NYIR_CMP_EQ;
        break;
      case NYIR_CMP_LT:
        inv = NYIR_CMP_GE;
        break;
      case NYIR_CMP_LE:
        inv = NYIR_CMP_GT;
        break;
      case NYIR_CMP_GT:
        inv = NYIR_CMP_LE;
        break;
      case NYIR_CMP_GE:
        inv = NYIR_CMP_LT;
        break;
      default:
        inv = c;
        break;
      }
      if (inv == c)
        return false;
      f->data[j].cmp = inv;
      f->data[i].imm = lf;
      f->data[i + 1].imm = lt;
      return true;
    }
  }
  return false;
}

/* Aggregate/field SROA: (1) dead-member store kill; (2) within straight-line
 * regions promote non-escaped store→load to COPY (field-scalar); (3) kill
 * stores that are overwritten before any load of that member. */
bool nyir_aggregate_sroa(nyir_func_t *f) {
  if (!f || f->len == 0)
    return true;
  size_t n = nyir_max_local(f);
  if (!n)
    return true;
  bool *loaded = calloc(n, 1);
  bool *escaped = calloc(n, 1);
  int *last_store = malloc(n * sizeof(int));
  int *field_def = malloc(n * sizeof(int));
  if (!loaded || !escaped || !last_store || !field_def) {
    free(loaded);
    free(escaped);
    free(last_store);
    free(field_def);
    return false;
  }
  for (size_t i = 0; i < n; ++i) {
    last_store[i] = -1;
    field_def[i] = -1;
  }
  for (size_t i = 0; i < f->len; ++i) {
    if (f->data[i].op == NYIR_LOAD_LOCAL && f->data[i].imm >= 0 &&
        (size_t)f->data[i].imm < n)
      loaded[f->data[i].imm] = true;
    if (f->data[i].op == NYIR_ADDR_LOCAL && f->data[i].imm >= 0 &&
        (size_t)f->data[i].imm < n)
      escaped[f->data[i].imm] = true;
  }
  /* Global dead-member stores. */
  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    if (in->op == NYIR_STORE_LOCAL && in->imm >= 0 && (size_t)in->imm < n &&
        !escaped[in->imm] && !loaded[in->imm])
      *in = (nyir_inst_t){.op = NYIR_NOP, .dst = -1, .a = -1, .b = -1};
  }
  /* Straight-line field promote + kill overwritten stores. */
  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    if (in->op == NYIR_LABEL || in->op == NYIR_BR || in->op == NYIR_BR_IF ||
        in->op == NYIR_CALL || in->op == NYIR_RET) {
      for (size_t l = 0; l < n; ++l) {
        last_store[l] = -1;
        field_def[l] = -1;
      }
      continue;
    }
    if (in->op == NYIR_STORE_LOCAL && in->imm >= 0 && (size_t)in->imm < n &&
        !escaped[in->imm]) {
      if (last_store[in->imm] >= 0) {
        size_t prev = (size_t)last_store[in->imm];
        f->data[prev] =
            (nyir_inst_t){.op = NYIR_NOP, .dst = -1, .a = -1, .b = -1};
      }
      last_store[in->imm] = (int)i;
      field_def[in->imm] = in->a;
    } else if (in->op == NYIR_LOAD_LOCAL && in->imm >= 0 &&
               (size_t)in->imm < n && !escaped[in->imm] &&
               field_def[in->imm] >= 0 && in->dst >= 0) {
      *in = (nyir_inst_t){
          .op = NYIR_COPY, .dst = in->dst, .a = field_def[in->imm], .b = -1};
      last_store[in->imm] = -1;
    }
  }
  free(loaded);
  free(escaped);
  free(last_store);
  free(field_def);
  return true;
}

/* Dead store after store to same non-escaped local with no intervening load. */
bool nyir_store_sink(nyir_func_t *f) {
  if (!f || f->len == 0)
    return true;
  size_t n = nyir_max_local(f);
  if (!n)
    return true;
  int *last_store = malloc(n * sizeof(int));
  bool *escaped = calloc(n, 1);
  if (!last_store || !escaped) {
    free(last_store);
    free(escaped);
    return false;
  }
  for (size_t i = 0; i < n; ++i)
    last_store[i] = -1;
  for (size_t i = 0; i < f->len; ++i)
    if (f->data[i].op == NYIR_ADDR_LOCAL && f->data[i].imm >= 0 &&
        (size_t)f->data[i].imm < n)
      escaped[f->data[i].imm] = true;
  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    if (in->op == NYIR_LABEL || in->op == NYIR_BR || in->op == NYIR_BR_IF ||
        in->op == NYIR_CALL || in->op == NYIR_RET) {
      for (size_t l = 0; l < n; ++l)
        last_store[l] = -1;
      continue;
    }
    if (in->op == NYIR_LOAD_LOCAL && in->imm >= 0 && (size_t)in->imm < n)
      last_store[in->imm] = -1;
    else if (in->op == NYIR_STORE_LOCAL && in->imm >= 0 &&
             (size_t)in->imm < n && !escaped[in->imm]) {
      if (last_store[in->imm] >= 0) {
        size_t prev = (size_t)last_store[in->imm];
        f->data[prev] =
            (nyir_inst_t){.op = NYIR_NOP, .dst = -1, .a = -1, .b = -1};
      }
      last_store[in->imm] = (int)i;
    }
  }
  free(last_store);
  free(escaped);
  return true;
}

/* Local points-to seed: ADDR_LOCAL v → local L. When LOAD_I64 of that pointer
 * and L is never passed to CALL, rewrite to LOAD_LOCAL. Complements SROA. */
bool nyir_points_to_sroa(nyir_func_t *f) {
  if (!f || f->len == 0 || f->next_value <= 0)
    return true;
  size_t nv = (size_t)f->next_value;
  int *pt = malloc(nv * sizeof(int));
  bool *escaped = NULL;
  if (!pt)
    return false;
  for (size_t i = 0; i < nv; ++i)
    pt[i] = -1;
  for (size_t i = 0; i < f->len; ++i) {
    const nyir_inst_t *in = &f->data[i];
    if (in->op == NYIR_ADDR_LOCAL && in->dst >= 0 && in->imm >= 0) {
      pt[in->dst] = (int)in->imm;
    } else if (in->op == NYIR_COPY && in->dst >= 0 && in->a >= 0 &&
               (size_t)in->a < nv && pt[in->a] >= 0) {
      pt[in->dst] = pt[in->a];
    }
  }
  size_t max_local = nyir_max_local(f);
  if (max_local == 0) {
    free(pt);
    return true;
  }
  escaped = calloc(max_local, 1);
  if (!escaped) {
    free(pt);
    return false;
  }
  for (size_t i = 0; i < f->len; ++i) {
    const nyir_inst_t *in = &f->data[i];
    if (in->op != NYIR_CALL)
      continue;
    int args[16];
    int argc = 0;
    if (!nyir_call_args(in, f->next_value, args, 16, &argc, NULL, 0))
      continue;
    for (int a = 0; a < argc; ++a) {
      if (args[a] >= 0 && (size_t)args[a] < nv && pt[args[a]] >= 0)
        escaped[pt[args[a]]] = true;
    }
  }
  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    if (in->op == NYIR_LOAD_I64 && in->a >= 0 && (size_t)in->a < nv &&
        pt[in->a] >= 0 && !escaped[pt[in->a]] && in->dst >= 0) {
      *in = (nyir_inst_t){.op = NYIR_LOAD_LOCAL,
                            .dst = in->dst,
                            .a = -1,
                            .b = -1,
                            .imm = pt[in->a]};
    } else if (in->op == NYIR_STORE_I64 && in->a >= 0 && (size_t)in->a < nv &&
               pt[in->a] >= 0 && !escaped[pt[in->a]] && in->c >= 0) {
      /* store *addr, val (a=addr, c=val) → store.local */
      *in = (nyir_inst_t){.op = NYIR_STORE_LOCAL,
                            .dst = -1,
                            .a = in->c,
                            .b = -1,
                            .c = -1,
                            .imm = pt[in->a]};
    }
  }
  free(pt);
  free(escaped);
  return true;
}

/* Polyhedral-style nest analysis: counted outer/inner trip, IV ranges, and
 * dependence-free tiling hint (when both trips constant and body is pure
 * affine add/mul on IVs — mark outer IV range so unroll can fire). */
bool nyir_polyhedral_nest(nyir_func_t *f) {
  if (!f || f->len < 12)
    return true;
  for (size_t i = 0; i < f->len; ++i) {
    if (f->data[i].op != NYIR_CMP_I64 || f->data[i].cmp != NYIR_CMP_LT)
      continue;
    int iv = f->data[i].a;
    int lim = f->data[i].b;
    if (iv < 0 || lim < 0)
      continue;
    int64_t trip = -1;
    for (size_t j = 0; j < i; ++j)
      if (f->data[j].op == NYIR_CONST_I64 && f->data[j].dst == lim) {
        trip = f->data[j].imm;
        break;
      }
    if (trip < 1 || trip > 1000000)
      continue;
    f->data[i].range.has_min = true;
    f->data[i].range.has_max = true;
    f->data[i].range.min = 0;
    f->data[i].range.max = 1;
    for (size_t j = 0; j < i; ++j) {
      if (f->data[j].dst != iv)
        continue;
      if (f->data[j].op == NYIR_LOAD_LOCAL || f->data[j].op == NYIR_COPY ||
          f->data[j].op == NYIR_ADD_I64 || f->data[j].op == NYIR_PHI) {
        f->data[j].range.has_min = true;
        f->data[j].range.has_max = true;
        f->data[j].range.min = 0;
        f->data[j].range.max = trip - 1;
      }
    }
    int64_t trip2 = -1;
    for (size_t k = i + 1; k < f->len && k < i + 128; ++k) {
      if (f->data[k].op != NYIR_CMP_I64 || f->data[k].cmp != NYIR_CMP_LT)
        continue;
      int lim2 = f->data[k].b;
      if (lim2 < 0)
        continue;
      for (size_t j = 0; j < k; ++j)
        if (f->data[j].op == NYIR_CONST_I64 && f->data[j].dst == lim2) {
          trip2 = f->data[j].imm;
          break;
        }
      if (trip2 >= 1 && trip2 <= 10000) {
        f->data[k].range.has_min = true;
        f->data[k].range.has_max = true;
        f->data[k].range.min = 0;
        f->data[k].range.max = 1;
        break;
      }
    }
    /* Tiling / unroll hint: small constant trip or nest product ≤ 64 →
     * tag lim const with range so consumers see exact trip; boost profile
     * when product is tile-friendly (power-of-two-ish small). */
    if (trip >= 2 && trip <= 8) {
      for (size_t j = 0; j < i; ++j)
        if (f->data[j].op == NYIR_CONST_I64 && f->data[j].dst == lim) {
          f->data[j].range.has_min = true;
          f->data[j].range.has_max = true;
          f->data[j].range.min = trip;
          f->data[j].range.max = trip;
        }
    }
    if (trip2 >= 2 && trip >= 2 && trip * trip2 <= 64) {
      if (ny_profile_steps < 3000)
        ny_native_profile_set_runtime(ny_profile_edges + 20, 3000);
    }
  }
  return true;
}

/* @kernel autotune: CALL targets matching *kernel* → larger inline budget,
 * profile boost proportional to arg-count (proxy for work item), and
 * polyhedral-friendly step count so nest ranges fire before inline. */
bool nyir_kernel_hint(nyir_func_t *f) {
  if (!f)
    return true;
  size_t kernel_calls = 0;
  size_t kernel_args = 0;
  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    if (in->op != NYIR_CALL || !in->symbol)
      continue;
    if (strstr(in->symbol, "kernel") || strstr(in->symbol, "Kernel") ||
        strstr(in->symbol, "KERNEL")) {
      kernel_calls++;
      int args[16];
      int argc = 0;
      if (nyir_call_args(in, f->next_value, args, 16, &argc, NULL, 0))
        kernel_args += (size_t)(argc > 0 ? argc : 1);
      else
        kernel_args += 1;
    }
  }
  if (kernel_calls == 0)
    return true;
  /* Target pick: more args / more sites → hotter kernel → denser budget. */
  uint64_t steps = 5000 + kernel_calls * 500 + kernel_args * 100;
  if (steps > 20000)
    steps = 20000;
  uint64_t edges = ny_profile_edges + 50 + kernel_calls * 25;
  if (ny_profile_steps < steps)
    ny_native_profile_set_runtime(edges, steps);
  return true;
}

bool nyir_block_layout(nyir_func_t *f) {
  if (!f || f->len < 4)
    return true;
  for (size_t i = 0; i + 1 < f->len; ++i) {
    if (f->data[i].op != NYIR_BR_IF || f->data[i + 1].op != NYIR_BR)
      continue;
    int64_t lt = f->data[i].imm, lf = f->data[i + 1].imm;
    size_t t_sz = 0, f_sz = 0;
    bool f_is_ret = false, t_is_ret = false;
    for (size_t j = 0; j < f->len; ++j) {
      if (f->data[j].op == NYIR_LABEL && f->data[j].imm == lt) {
        for (size_t k = j + 1; k < f->len && f->data[k].op != NYIR_LABEL; ++k) {
          t_sz++;
          if (f->data[k].op == NYIR_RET)
            t_is_ret = true;
        }
      }
      if (f->data[j].op == NYIR_LABEL && f->data[j].imm == lf) {
        for (size_t k = j + 1; k < f->len && f->data[k].op != NYIR_LABEL; ++k) {
          f_sz++;
          if (f->data[k].op == NYIR_RET)
            f_is_ret = true;
        }
      }
    }
    size_t t_pc = SIZE_MAX, f_pc = SIZE_MAX;
    for (size_t j = 0; j < f->len; ++j) {
      if (f->data[j].op != NYIR_LABEL)
        continue;
      if (f->data[j].imm == lt)
        t_pc = j;
      if (f->data[j].imm == lf)
        f_pc = j;
    }
    uint64_t t_heat = t_pc != SIZE_MAX
                          ? ny_native_profile_edge_hot(i, t_pc)
                          : 0;
    uint64_t f_heat = f_pc != SIZE_MAX
                          ? ny_native_profile_edge_hot(i + 1, f_pc)
                          : 0;
    if (!f_heat)
      f_heat = ny_native_profile_edge_hot(i, i + 1);
    if (t_heat || f_heat) {
      if (t_heat > f_heat)
        (void)nyir_invert_br_if_at(f, i);
      continue;
    }

    /* Cold exit on false arm: invert so hot true falls through after invert
     * (true becomes the old false which was cold — wait: we want fallthrough
     * = hot. After invert, fallthrough is the old true. So we invert when
     * fallthrough (false arm) is cold ret and true is hot. That makes old true
     * become fallthrough. Correct. */
    if (f_is_ret && f_sz <= 3 && t_sz > f_sz) {
      (void)nyir_invert_br_if_at(f, i);
      continue;
    }
    /* Profile-guided-ish: when true is tiny ret and false is large, leave as-is
     * (fallthrough already hot). When both non-ret and true is much larger
     * than false, invert so large arm falls through after invert... actually
     * fallthrough is false arm; invert swaps so old true becomes fallthrough. */
    if (!f_is_ret && !t_is_ret && t_sz >= f_sz * 2 && t_sz >= 4 &&
        ny_native_profile_should_inline(t_sz)) {
      (void)nyir_invert_br_if_at(f, i);
    }
  }
  return true;
}
