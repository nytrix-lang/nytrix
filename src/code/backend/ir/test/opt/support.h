/*
 * Shared support for NYIR optimizer tests.
 *
 * Each test driver beside this header is a regression test for the
 * corresponding pass or subsystem in src/code/backend/ir/opt/. Drivers build
 * small NYIR functions by hand,
 * run the pass under test, and demand three things:
 *
 *   1. nyr_ir_verify passes before AND after the pass runs,
 *   2. nyr_ir_eval returns identical results before and after,
 *   3. the result matches a hand-computed golden value.
 *
 * Compiler tests build the functions through the same verifier and
 * evaluator as production code. Set OPTCHECK_DUMP=1
 * to print IR around each stage, which is usually the fastest way to
 * debug a mismatch.
 */
#ifndef NYR_IR_OPT_TEST_SUPPORT_H
#define NYR_IR_OPT_TEST_SUPPORT_H

#include <nyr/ir.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#define MAP_FAILED ((void *)(intptr_t)-1)
#else
#include <sys/mman.h>
#endif

static inline int nyr_opt_setenv(const char *name, const char *value) {
#ifdef _WIN32
  return _putenv_s(name, value);
#else
  return setenv(name, value, 1);
#endif
}

static inline int nyr_opt_unsetenv(const char *name) {
#ifdef _WIN32
  return _putenv_s(name, "");
#else
  return unsetenv(name);
#endif
}

static inline void *nyr_opt_execmem_alloc(size_t size) {
#ifdef _WIN32
  void *mapping =
      VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  return mapping ? mapping : MAP_FAILED;
#else
  return mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
              -1, 0);
#endif
}

static inline int nyr_opt_execmem_protect(void *mapping, size_t size) {
#ifdef _WIN32
  DWORD old_protection;
  if (VirtualProtect(mapping, size, PAGE_EXECUTE_READ, &old_protection) == 0 ||
      FlushInstructionCache(GetCurrentProcess(), mapping, size) == 0)
    return -1;
  return 0;
#else
  return mprotect(mapping, size, PROT_READ | PROT_EXEC);
#endif
}

static inline void nyr_opt_execmem_free(void *mapping, size_t size) {
  if (mapping == MAP_FAILED)
    return;
#ifdef _WIN32
  (void)size;
  (void)VirtualFree(mapping, 0, MEM_RELEASE);
#else
  (void)munmap(mapping, size);
#endif
}

/* IR builders: every operand slot is explicitly filled so accidental
 * zero-value operands cannot masquerade as valid value ids. */
static inline int nyr_opt_label(nyr_func_t *f, int64_t id) {
  return nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LABEL,
                                     .dst = -1,
                                     .a = -1,
                                     .b = -1,
                                     .c = -1,
                                     .d = -1,
                                     .e = -1,
                                     .f = -1,
                                     .imm = id});
}

static inline int nyr_opt_const(nyr_func_t *f, int64_t v) {
  return nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_CONST_I64,
                                     .dst = -1,
                                     .a = -1,
                                     .b = -1,
                                     .c = -1,
                                     .d = -1,
                                     .e = -1,
                                     .f = -1,
                                     .imm = v});
}

static inline int nyr_opt_const_f64(nyr_func_t *f, double v) {
  uint64_t bits;
  memcpy(&bits, &v, sizeof(bits));
  return nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_CONST_F64,
                                     .dst = -1,
                                     .a = -1,
                                     .b = -1,
                                     .c = -1,
                                     .d = -1,
                                     .e = -1,
                                     .f = -1,
                                     .imm = (int64_t)bits});
}

static inline int nyr_opt_select(nyr_func_t *f, int cond, int tval, int fval) {
  return nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_SELECT_I64,
                                     .dst = -1,
                                     .a = cond,
                                     .b = tval,
                                     .c = fval,
                                     .d = -1,
                                     .e = -1,
                                     .f = -1});
}

static inline int nyr_opt_binop(nyr_func_t *f, nyr_op_t op, int a, int b) {
  return nyr_ir_emit(f, (nyr_inst_t){.op = op,
                                     .dst = -1,
                                     .a = a,
                                     .b = b,
                                     .c = -1,
                                     .d = -1,
                                     .e = -1,
                                     .f = -1});
}

static inline int nyr_opt_cmp(nyr_func_t *f, int a, int b, nyr_cmp_t cmp) {
  return nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_CMP_I64,
                                     .dst = -1,
                                     .a = a,
                                     .b = b,
                                     .c = -1,
                                     .d = -1,
                                     .e = -1,
                                     .f = -1,
                                     .cmp = cmp});
}

static inline void nyr_opt_br(nyr_func_t *f, int64_t label) {
  nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_BR,
                              .dst = -1,
                              .a = -1,
                              .b = -1,
                              .c = -1,
                              .d = -1,
                              .e = -1,
                              .f = -1,
                              .imm = label});
}

static inline void nyr_opt_br_if(nyr_func_t *f, int cond, int64_t label) {
  nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_BR_IF,
                              .dst = -1,
                              .a = cond,
                              .b = -1,
                              .c = -1,
                              .d = -1,
                              .e = -1,
                              .f = -1,
                              .imm = label});
}

static inline void nyr_opt_ret(nyr_func_t *f, int value) {
  nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_RET,
                              .dst = -1,
                              .a = value,
                              .b = -1,
                              .c = -1,
                              .d = -1,
                              .e = -1,
                              .f = -1});
}

/* Two-entry PHI with explicit predecessor labels; entry blocks are
 * unlabeled and use -1. */
static inline void nyr_opt_phi(nyr_func_t *f, int64_t pred0, int value0,
                               int64_t pred1, int value1) {
  nyr_phi_incoming_t *inc = malloc(sizeof(*inc) * 2);
  if (!inc)
    exit(2);
  inc[0] = (nyr_phi_incoming_t){.predecessor_label = pred0, .value = value0};
  inc[1] = (nyr_phi_incoming_t){.predecessor_label = pred1, .value = value1};
  nyr_inst_t inst = {.op = NYIR_PHI, .dst = -1, .phi_incoming_len = 2};
  inst.phi_incoming = inc;
  nyr_ir_emit(f, inst);
}

/* Reset a function for hand-built construction. */
static inline void nyr_opt_begin(nyr_func_t *f) {
  memset(f, 0, sizeof(*f));
  f->next_value = 0;
}

/* Verify helper: exits non-zero with a dump when the verifier objects. */
static inline bool nyr_opt_verify(nyr_func_t *f, const char *stage) {
  char err[256] = {0};
  if (nyr_ir_verify(f, err, sizeof(err)))
    return true;
  fprintf(stderr, "[%s] verify failed: %s\n", stage, err);
  nyr_dump(stderr, f, stage);
  return false;
}

/* Evaluate with a generous step budget; exits non-zero on eval failure. */
static inline long long nyr_opt_eval(nyr_func_t *f, const char *tag) {
  nyr_eval_result_t r = {0};
  int64_t locals[8] = {0};
  char err[256] = {0};
  if (!nyr_ir_eval(f, locals, 8, 500, &r, err, sizeof(err))) {
    fprintf(stderr, "[eval-%s] failed: %s\n", tag, err);
    exit(1);
  }
  long long value = (long long)r.result;
  size_t steps = r.steps;
  nyr_eval_result_free(&r);
  if (getenv("OPTCHECK_STEPS"))
    printf("[%s] steps=%zu\n", tag, steps);
  return value;
}

/* Evaluate with caller-provided local slots for pointer-backed memory tests. */
static inline bool nyr_opt_eval_locals(nyr_func_t *f, const char *tag,
                                       int64_t *locals, size_t local_count,
                                       int64_t *value) {
  nyr_eval_result_t r = {0};
  char err[256] = {0};
  if (!nyr_ir_eval(f, locals, local_count, 5000, &r, err, sizeof(err))) {
    fprintf(stderr, "[eval-%s] failed: %s\n", tag, err);
    return false;
  }
  if (value)
    *value = r.result;
  nyr_eval_result_free(&r);
  return true;
}

static inline bool nyr_opt_eval_outcome(nyr_func_t *f, int64_t *locals,
                                        size_t local_count, size_t max_steps,
                                        int64_t *value, char *err,
                                        size_t err_len) {
  nyr_eval_result_t r = {0};
  bool ok = nyr_ir_eval(f, locals, local_count, max_steps, &r, err, err_len);
  if (ok && value)
    *value = r.result;
  nyr_eval_result_free(&r);
  return ok;
}

static inline bool nyr_opt_want_dump(void) {
  return getenv("OPTCHECK_DUMP") != NULL;
}

/* Final gate: before/after agreement plus the golden constant. */
static inline void nyr_opt_check(const char *test, long long before,
                                 long long after, long long want) {
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
 * With n == m the loops are fusion candidates for nyr_affine_nest;
 * passing different trip counts yields the not-fusable variant.  Both
 * accumulators start at 100 and X2 returns their sum, so n = m = 3
 * evaluates to (100+0+1+2) + (100+0+2+4) = 209.
 */
static inline void nyr_opt_build_two_loops(nyr_func_t *f, int64_t n,
                                           int64_t m) {
  nyr_opt_begin(f);
  int vn = nyr_opt_const(f, n);
  int vm = nyr_opt_const(f, m);
  int zero = nyr_opt_const(f, 0);
  int one = nyr_opt_const(f, 1);
  int acc0 = nyr_opt_const(f, 100);
  nyr_opt_br(f, 10);
  /* H1 */
  nyr_opt_label(f, 10);
  int iv1 = f->next_value;
  nyr_opt_phi(f, -1, zero, 0, zero); /* backedge patched below */
  int s1 = f->next_value;
  nyr_opt_phi(f, -1, acc0, 0, zero);
  int c1 = nyr_opt_cmp(f, iv1, vn, NYIR_CMP_LT);
  nyr_opt_br_if(f, c1, 11);
  /* X */
  nyr_opt_label(f, 30);
  nyr_opt_br(f, 40);
  /* B1 */
  nyr_opt_label(f, 11);
  int t1 = nyr_opt_binop(f, NYIR_ADD_I64, s1, iv1);
  int iv1n = nyr_opt_binop(f, NYIR_ADD_I64, iv1, one);
  nyr_opt_br(f, 10);
  /* P2 */
  nyr_opt_label(f, 40);
  int z2 = nyr_opt_const(f, 0);
  int a2 = nyr_opt_const(f, 100);
  nyr_opt_br(f, 20);
  /* H2 */
  nyr_opt_label(f, 20);
  int iv2 = f->next_value;
  nyr_opt_phi(f, 40, z2, 0, zero); /* backedge patched below */
  int s2 = f->next_value;
  nyr_opt_phi(f, 40, a2, 0, zero);
  int c2 = nyr_opt_cmp(f, iv2, vm, NYIR_CMP_LT);
  nyr_opt_br_if(f, c2, 21);
  /* X2 consumes header phis; body defs would not dominate this exit */
  nyr_opt_label(f, 50);
  int total = nyr_opt_binop(f, NYIR_ADD_I64, s1, s2);
  nyr_opt_ret(f, total);
  /* B2 */
  nyr_opt_label(f, 21);
  int jj = nyr_opt_binop(f, NYIR_ADD_I64, iv2, iv2);
  int t2 = nyr_opt_binop(f, NYIR_ADD_I64, s2, jj);
  int iv2n = nyr_opt_binop(f, NYIR_ADD_I64, iv2, one);
  nyr_opt_br(f, 20);
  /* END: unreachable but terminated */
  nyr_opt_label(f, 99);
  nyr_opt_ret(f, nyr_opt_const(f, 0));

  /* Backpatch phi backedges now that body values exist. */
  size_t phis = 0;
  for (size_t i = 0; i < f->len; ++i) {
    if (f->data[i].op != NYIR_PHI)
      continue;
    switch (phis++) {
    case 0:
      f->data[i].phi_incoming[1] =
          (nyr_phi_incoming_t){.predecessor_label = 11, .value = iv1n};
      break;
    case 1:
      f->data[i].phi_incoming[1] =
          (nyr_phi_incoming_t){.predecessor_label = 11, .value = t1};
      break;
    case 2:
      f->data[i].phi_incoming[1] =
          (nyr_phi_incoming_t){.predecessor_label = 21, .value = iv2n};
      break;
    case 3:
      f->data[i].phi_incoming[1] =
          (nyr_phi_incoming_t){.predecessor_label = 21, .value = t2};
      break;
    }
  }
}

#endif /* NYR_IR_OPT_TEST_SUPPORT_H */
