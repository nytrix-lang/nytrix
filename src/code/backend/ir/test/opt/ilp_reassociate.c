/*
 * Critical-path reassociation must preserve wrapping i64 results while moving
 * a loop-invariant addend into SELECT arms and reordering ADD/SUB chains so
 * the loop-carried value joins last. Straight-line code is left alone.
 */
#include "support.h"
#include <stdint.h>

static int64_t reference(int64_t depth, int64_t seed) {
  uint64_t x = (uint64_t)(seed + depth), acc = 1;
  for (int64_t i = 0; i < depth; ++i) {
    if ((x & 1) == 0)
      acc = acc * 3 + (uint64_t)i + (uint64_t)depth;
    else
      acc = acc * 5 - (uint64_t)i + (uint64_t)depth;
    x = (uint64_t)((int64_t)x / 2) + acc + (uint64_t)i;
  }
  return (int64_t)acc;
}

/*
 * The binary benchmark recurrence after if-conversion and select factoring:
 * acc' = select(even, acc*3 + i, acc*5 - i) + depth.
 */
static bool build(nyr_func_t *f, int *acc_next_out) {
  nyr_opt_begin(f);
  nyr_opt_label(f, 1);
  int depth = nyr_opt_emit_imm(f, NYIR_LOAD_LOCAL, 0);
  int seed = nyr_opt_emit_imm(f, NYIR_LOAD_LOCAL, 1);
  int zero = nyr_opt_const(f, 0);
  int one = nyr_opt_const(f, 1);
  int two = nyr_opt_const(f, 2);
  int three = nyr_opt_const(f, 3);
  int five = nyr_opt_const(f, 5);
  int x0 = nyr_opt_binop(f, NYIR_ADD_I64, seed, depth);
  nyr_opt_br(f, 2);
  nyr_opt_label(f, 2);
  size_t iv_phi = f->len;
  int iv = f->next_value;
  nyr_opt_phi(f, 1, zero, 3, zero);
  size_t acc_phi = f->len;
  int acc = f->next_value;
  nyr_opt_phi(f, 1, one, 3, one);
  size_t x_phi = f->len;
  int x = f->next_value;
  nyr_opt_phi(f, 1, x0, 3, x0);
  int more = nyr_opt_cmp(f, iv, depth, NYIR_CMP_LT);
  nyr_opt_br_if(f, more, 3);
  nyr_opt_br(f, 4);
  nyr_opt_label(f, 3);
  int bit = nyr_opt_binop(f, NYIR_AND_I64, x, one);
  int even = nyr_opt_cmp(f, bit, zero, NYIR_CMP_EQ);
  int a3 = nyr_opt_binop(f, NYIR_MUL_I64, acc, three);
  int t3 = nyr_opt_binop(f, NYIR_ADD_I64, a3, iv);
  int a5 = nyr_opt_binop(f, NYIR_MUL_I64, acc, five);
  int t5 = nyr_opt_binop(f, NYIR_SUB_I64, a5, iv);
  int selected = f->next_value;
  nyr_opt_emit_abc(f, NYIR_SELECT_I64, even, t3, t5);
  int acc_next = nyr_opt_binop(f, NYIR_ADD_I64, selected, depth);
  int half = nyr_opt_binop(f, NYIR_DIV_I64, x, two);
  int sum = nyr_opt_binop(f, NYIR_ADD_I64, half, acc_next);
  int x_next = nyr_opt_binop(f, NYIR_ADD_I64, sum, iv);
  int iv_next = nyr_opt_binop(f, NYIR_ADD_I64, iv, one);
  nyr_opt_br(f, 2);
  nyr_opt_label(f, 4);
  nyr_opt_ret(f, acc);
  f->data[iv_phi].phi_incoming[1].value = iv_next;
  f->data[acc_phi].phi_incoming[1].value = acc_next;
  f->data[x_phi].phi_incoming[1].value = x_next;
  *acc_next_out = acc_next;
  return nyr_opt_verify(f, "ilp-reassociate-before");
}

static const nyr_inst_t *def_of(const nyr_func_t *f, int value) {
  for (size_t i = 0; i < f->len; ++i)
    if (f->data[i].op != NYIR_NOP && f->data[i].dst == value)
      return &f->data[i];
  return NULL;
}

static bool run_values(nyr_func_t *f, const char *tag) {
  static const int64_t depths[] = {0, 1, 2, 4, 7, 18};
  static const int64_t seeds[] = {0, 1, -1, -7, 12345, INT64_MAX - 20,
                                  INT64_MIN + 3};
  for (size_t d = 0; d < sizeof(depths) / sizeof(depths[0]); ++d)
    for (size_t s = 0; s < sizeof(seeds) / sizeof(seeds[0]); ++s) {
      int64_t locals[8] = {depths[d], seeds[s]};
      int64_t got = 0;
      if (!nyr_opt_eval_locals(f, tag, locals, 8, &got))
        return false;
      int64_t want = reference(depths[d], seeds[s]);
      if (got != want) {
        fprintf(stderr,
                "[ilp-reassociate] %s depth=%lld seed=%lld got=%lld "
                "want=%lld\n",
                tag, (long long)depths[d], (long long)seeds[s],
                (long long)got, (long long)want);
        return false;
      }
    }
  return true;
}

static bool check_loop(void) {
  nyr_func_t f;
  int acc_next = -1;
  bool ok = build(&f, &acc_next) && run_values(&f, "before") &&
            nyr_ilp_reassociate(&f) &&
            nyr_opt_verify(&f, "ilp-reassociate-after") &&
            run_values(&f, "after");
  /*
   * The loop-carried accumulator is now produced by the SELECT itself, and
   * each arm carries the invariant addend.
   */
  const nyr_inst_t *acc_def = ok ? def_of(&f, acc_next) : NULL;
  const nyr_inst_t *yes = acc_def ? def_of(&f, acc_def->b) : NULL;
  const nyr_inst_t *no = acc_def ? def_of(&f, acc_def->c) : NULL;
  ok = ok && acc_def && acc_def->op == NYIR_SELECT_I64 && yes && no &&
       (yes->op == NYIR_ADD_I64 || yes->op == NYIR_SUB_I64) &&
       (no->op == NYIR_ADD_I64 || no->op == NYIR_SUB_I64);
  if (!ok) {
    fprintf(stderr, "[ilp-reassociate] loop recurrence was not rewritten\n");
    nyr_dump(stderr, &f, "ilp-reassociate");
  }
  nyr_ir_func_free(&f);
  return ok;
}

/*
 * Without a loop nothing is on a recurrence; the pass must not grow code.
 */
static bool check_straight_line(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int a = nyr_opt_const(&f, 6);
  int b = nyr_opt_const(&f, 9);
  int k = nyr_opt_const(&f, 4);
  int c = nyr_opt_cmp(&f, a, b, NYIR_CMP_LT);
  int s = f.next_value;
  nyr_opt_emit_abc(&f, NYIR_SELECT_I64, c, a, b);
  int r = nyr_opt_binop(&f, NYIR_ADD_I64, s, k);
  nyr_opt_ret(&f, r);
  size_t before = f.len;
  bool ok = nyr_opt_verify(&f, "ilp-straight-before") &&
            nyr_ilp_reassociate(&f) && f.len == before &&
            nyr_opt_eval(&f, "ilp-straight") == 10;
  if (!ok)
    fprintf(stderr, "[ilp-reassociate] straight-line code changed\n");
  nyr_ir_func_free(&f);
  return ok;
}

int main(void) {
  bool ok = check_loop() && check_straight_line();
  if (ok)
    puts("[ilp-reassociate] ok");
  return ok ? 0 : 1;
}
