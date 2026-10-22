/*
 * Focused jump-threading regression: PHI edge updates and unknown retention.
 */
#include "support.h"

static void phi2(nyr_func_t *f, int64_t p0, int v0, int64_t p1, int v1) {
  nyr_phi_incoming_t *in = malloc(2 * sizeof(*in));
  if (!in)
    exit(2);
  in[0] = (nyr_phi_incoming_t){.predecessor_label = p0, .value = v0};
  in[1] = (nyr_phi_incoming_t){.predecessor_label = p1, .value = v1};
  nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_PHI,
                             .dst = -1,
                             .a = -1,
                             .b = -1,
                             .c = -1,
                             .d = -1,
                             .e = -1,
                             .f = -1,
                             .phi_incoming = in,
                             .phi_incoming_len = 2});
}

static void build_positive(nyr_func_t *f) {
  nyr_opt_begin(f);
  int one = nyr_opt_const(f, 1);
  int zero = nyr_opt_const(f, 0);
  nyr_opt_br(f, 10);
  nyr_opt_label(f, 10);
  int cond = 0;
  phi2(f, -1, one, 20, zero);
  cond = f->data[f->len - 1].dst;
  nyr_opt_br_if(f, cond, 30);
  nyr_opt_label(f, 40);
  nyr_opt_br(f, 70);
  nyr_opt_label(f, 70);
  nyr_opt_br(f, 30);
  nyr_opt_label(f, 30);
  int result = 0;
  phi2(f, 10, one, 70, zero);
  result = f->data[f->len - 1].dst;
  nyr_opt_ret(f, result);
  nyr_opt_label(f, 20);
  nyr_opt_br(f, 10);
}

static int64_t first_branch_target(const nyr_func_t *f) {
  for (size_t i = 0; i < f->len; ++i)
    if (f->data[i].op == NYIR_BR_IF)
      return f->data[i].imm;
  return -1;
}

static void build_unknown(nyr_func_t *f, int64_t *branch_target) {
  nyr_opt_begin(f);
  int cond = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                        .dst = -1,
                                        .a = -1,
                                        .b = -1,
                                        .c = -1,
                                        .d = -1,
                                        .e = -1,
                                        .f = -1,
                                        .imm = 0});
  nyr_opt_br(f, 10);
  nyr_opt_label(f, 10);
  nyr_opt_br_if(f, cond, 30);
  *branch_target = f->data[f->len - 1].imm;
  nyr_opt_label(f, 40);
  int value = nyr_opt_const(f, 4);
  nyr_opt_ret(f, value);
  nyr_opt_label(f, 30);
  value = nyr_opt_const(f, 9);
  nyr_opt_ret(f, value);
}

int main(void) {
  nyr_func_t positive;
  build_positive(&positive);
  if (!nyr_opt_verify(&positive, "jump/positive-before"))
    return 1;
  long long before = nyr_opt_eval(&positive, "jump/positive-before");
  nyr_jump_thread(&positive);
  if (!nyr_opt_verify(&positive, "jump/positive-after"))
    return 1;
  long long after = nyr_opt_eval(&positive, "jump/positive-after");
  nyr_opt_check("jump-positive", before, after, 1);
  nyr_ir_func_free(&positive);

  nyr_func_t unknown;
  int64_t target = -1;
  build_unknown(&unknown, &target);
  if (!nyr_opt_verify(&unknown, "jump/unknown-before"))
    return 1;
  long long unknown_before = nyr_opt_eval(&unknown, "jump/unknown-before");
  nyr_jump_thread(&unknown);
  if (!nyr_opt_verify(&unknown, "jump/unknown-after"))
    return 1;
  long long unknown_after = nyr_opt_eval(&unknown, "jump/unknown-after");
  int64_t after_target = first_branch_target(&unknown);
  if (unknown_before != unknown_after || target != 30 || after_target != 30) {
    fprintf(stderr, "[jump-unknown] branch was not retained\n");
    return 1;
  }
  printf("[jump-unknown] ok result=%lld\n", unknown_after);
  nyr_ir_func_free(&unknown);
  return 0;
}
