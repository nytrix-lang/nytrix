/*
 * Shared harness for the optcheck driver suite.
 *
 * Each <opt>.c here is a standalone regression driver for one pass in
 * src/code/ir/opt/.  Drivers build small NYIR functions by hand,
 * run the pass under test, and demand three things:
 *
 *   1. nyir_verify passes before AND after the pass runs,
 *   2. nyir_eval returns identical results before and after,
 *   3. the result matches a hand-computed golden value.
 *
 * Build and run with the Makefile in this directory (`make run`, or
 * `make ASAN=1 run` for memory checking); binaries land in
 * build/driver/ and exit non-zero on any failure.  Set OPTCHECK_DUMP=1
 * to print IR around each stage, which is usually the fastest way to
 * debug a mismatch.
 */
#ifndef OPTCHECK_UTIL_H
#define OPTCHECK_UTIL_H

#include "code/ir/ir.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* IR builders: every operand slot is explicitly filled so accidental
 * zero-value operands cannot masquerade as valid value ids. */
static inline int ny_label(nyir_func_t *f, int64_t id) {
  return nyir_emit(f, (nyir_inst_t){.op = NYIR_LABEL, .dst = -1, .a = -1,
                                    .b = -1, .c = -1, .d = -1, .e = -1,
                                    .f = -1, .imm = id});
}

static inline int ny_const(nyir_func_t *f, int64_t v) {
  return nyir_emit(f, (nyir_inst_t){.op = NYIR_CONST_I64, .dst = -1, .a = -1,
                                    .b = -1, .c = -1, .d = -1, .e = -1,
                                    .f = -1, .imm = v});
}

static inline int ny_binop(nyir_func_t *f, nyir_op_t op, int a, int b) {
  return nyir_emit(f, (nyir_inst_t){.op = op, .dst = -1, .a = a, .b = b,
                                    .c = -1, .d = -1, .e = -1, .f = -1});
}

static inline int ny_cmp(nyir_func_t *f, int a, int b, nyir_cmp_t cmp) {
  return nyir_emit(f, (nyir_inst_t){.op = NYIR_CMP_I64, .dst = -1, .a = a,
                                    .b = b, .c = -1, .d = -1, .e = -1,
                                    .f = -1, .cmp = cmp});
}

static inline void ny_br(nyir_func_t *f, int64_t label) {
  nyir_emit(f, (nyir_inst_t){.op = NYIR_BR, .dst = -1, .a = -1, .b = -1,
                             .c = -1, .d = -1, .e = -1, .f = -1,
                             .imm = label});
}

static inline void ny_br_if(nyir_func_t *f, int cond, int64_t label) {
  nyir_emit(f, (nyir_inst_t){.op = NYIR_BR_IF, .dst = -1, .a = cond, .b = -1,
                             .c = -1, .d = -1, .e = -1, .f = -1,
                             .imm = label});
}

static inline void ny_ret(nyir_func_t *f, int value) {
  nyir_emit(f, (nyir_inst_t){.op = NYIR_RET, .dst = -1, .a = value, .b = -1,
                             .c = -1, .d = -1, .e = -1, .f = -1});
}

/* Two-entry PHI with explicit predecessor labels; entry blocks are
 * unlabeled and use -1. */
static inline void ny_phi(nyir_func_t *f, int64_t pred0, int value0, int64_t pred1,
                   int value1) {
  nyir_phi_incoming_t *inc = malloc(sizeof(*inc) * 2);
  if (!inc)
    exit(2);
  inc[0] = (nyir_phi_incoming_t){.predecessor_label = pred0, .value = value0};
  inc[1] = (nyir_phi_incoming_t){.predecessor_label = pred1, .value = value1};
  nyir_inst_t inst = {.op = NYIR_PHI, .dst = -1, .phi_incoming_len = 2};
  inst.phi_incoming = inc;
  nyir_emit(f, inst);
}

/* Reset a function for hand-built construction. */
static inline void ny_begin(nyir_func_t *f) {
  memset(f, 0, sizeof(*f));
  f->next_value = 0;
}

/* Verify helper: exits non-zero with a dump when the verifier objects. */
static inline bool ny_verify(nyir_func_t *f, const char *stage) {
  char err[256] = {0};
  if (nyir_verify(f, err, sizeof(err)))
    return true;
  fprintf(stderr, "[%s] verify failed: %s\n", stage, err);
  nyir_dump(stderr, f, stage);
  return false;
}

/* Evaluate with a generous step budget; exits non-zero on eval failure. */
static inline long long ny_eval(nyir_func_t *f, const char *tag) {
  nyir_eval_result_t r = {0};
  int64_t locals[8] = {0};
  char err[256] = {0};
  if (!nyir_eval(f, locals, 8, 500, &r, err, sizeof(err))) {
    fprintf(stderr, "[eval-%s] failed: %s\n", tag, err);
    exit(1);
  }
  long long value = (long long)r.result;
  size_t steps = r.steps;
  nyir_eval_result_free(&r);
  if (getenv("OPTCHECK_STEPS"))
    printf("[%s] steps=%zu\n", tag, steps);
  return value;
}

static inline bool ny_want_dump(void) { return getenv("OPTCHECK_DUMP") != NULL; }

/* Final gate: before/after agreement plus the golden constant. */
static inline void ny_check(const char *test, long long before, long long after,
                     long long want) {
  if (before != want || after != want) {
    fprintf(stderr, "[%s] MISMATCH before=%lld after=%lld want=%lld\n", test,
            before, after, want);
    exit(1);
  }
  printf("[%s] ok result=%lld\n", test, want);
}

/*
 * Shared fixture: two adjacent counted loops in the canonical layout
 * ([entry][H1 guard fall->X][X br->P2][B1 br->H1][P2 br->H2]
 *  [H2 guard fall->X2][X2 ret][B2 br->H2][END]).
 *
 * With n == m the loops are fusion candidates for nyir_affine_nest;
 * passing different trip counts yields the not-fusable variant.  Both
 * accumulators start at 100 and X2 returns their sum, so n = m = 3
 * evaluates to (100+0+1+2) + (100+0+2+4) = 209.
 */
static inline void ny_build_two_loops(nyir_func_t *f, int64_t n, int64_t m) {
  ny_begin(f);
  int vn = ny_const(f, n);
  int vm = ny_const(f, m);
  int zero = ny_const(f, 0);
  int one = ny_const(f, 1);
  int acc0 = ny_const(f, 100);
  ny_br(f, 10);
  /* H1 */
  ny_label(f, 10);
  int iv1 = f->next_value;
  ny_phi(f, -1, zero, 0, zero); /* backedge patched below */
  int s1 = f->next_value;
  ny_phi(f, -1, acc0, 0, zero);
  int c1 = ny_cmp(f, iv1, vn, NYIR_CMP_LT);
  ny_br_if(f, c1, 11);
  /* X */
  ny_label(f, 30);
  ny_br(f, 40);
  /* B1 */
  ny_label(f, 11);
  int t1 = ny_binop(f, NYIR_ADD_I64, s1, iv1);
  int iv1n = ny_binop(f, NYIR_ADD_I64, iv1, one);
  ny_br(f, 10);
  /* P2 */
  ny_label(f, 40);
  int z2 = ny_const(f, 0);
  int a2 = ny_const(f, 100);
  ny_br(f, 20);
  /* H2 */
  ny_label(f, 20);
  int iv2 = f->next_value;
  ny_phi(f, 40, z2, 0, zero); /* backedge patched below */
  int s2 = f->next_value;
  ny_phi(f, 40, a2, 0, zero);
  int c2 = ny_cmp(f, iv2, vm, NYIR_CMP_LT);
  ny_br_if(f, c2, 21);
  /* X2 consumes header phis; body defs would not dominate this exit */
  ny_label(f, 50);
  int total = ny_binop(f, NYIR_ADD_I64, s1, s2);
  ny_ret(f, total);
  /* B2 */
  ny_label(f, 21);
  int jj = ny_binop(f, NYIR_ADD_I64, iv2, iv2);
  int t2 = ny_binop(f, NYIR_ADD_I64, s2, jj);
  int iv2n = ny_binop(f, NYIR_ADD_I64, iv2, one);
  ny_br(f, 20);
  /* END: unreachable but terminated */
  ny_label(f, 99);
  ny_ret(f, ny_const(f, 0));

  /* Backpatch phi backedges now that body values exist. */
  size_t phis = 0;
  for (size_t i = 0; i < f->len; ++i) {
    if (f->data[i].op != NYIR_PHI)
      continue;
    switch (phis++) {
    case 0:
      f->data[i].phi_incoming[1] = (nyir_phi_incoming_t){
          .predecessor_label = 11, .value = iv1n};
      break;
    case 1:
      f->data[i].phi_incoming[1] = (nyir_phi_incoming_t){
          .predecessor_label = 11, .value = t1};
      break;
    case 2:
      f->data[i].phi_incoming[1] = (nyir_phi_incoming_t){
          .predecessor_label = 21, .value = iv2n};
      break;
    case 3:
      f->data[i].phi_incoming[1] = (nyir_phi_incoming_t){
          .predecessor_label = 21, .value = t2};
      break;
    }
  }
}

#endif /* OPTCHECK_UTIL_H */
