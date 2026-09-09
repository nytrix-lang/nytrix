/*
 * Focused jump-threading regression: PHI edge updates and unknown retention.
 */
#include "util.h"

static void phi2(nyir_func_t *f, int64_t p0, int v0, int64_t p1, int v1) {
  nyir_phi_incoming_t *in = malloc(2 * sizeof(*in));
  if (!in)
    exit(2);
  in[0] = (nyir_phi_incoming_t){.predecessor_label = p0, .value = v0};
  in[1] = (nyir_phi_incoming_t){.predecessor_label = p1, .value = v1};
  nyir_emit(f, (nyir_inst_t){.op = NYIR_PHI, .dst = -1, .a = -1,
                              .b = -1, .c = -1, .d = -1, .e = -1, .f = -1,
                              .phi_incoming = in, .phi_incoming_len = 2});
}

static void build_positive(nyir_func_t *f) {
  ny_begin(f);
  int one = ny_const(f, 1);
  int zero = ny_const(f, 0);
  ny_br(f, 10);
  ny_label(f, 10);
  int cond = 0;
  phi2(f, -1, one, 20, zero);
  cond = f->data[f->len - 1].dst;
  ny_br_if(f, cond, 30);
  ny_label(f, 40);
  ny_br(f, 70);
  ny_label(f, 70);
  ny_br(f, 30);
  ny_label(f, 30);
  int result = 0;
  phi2(f, 10, one, 70, zero);
  result = f->data[f->len - 1].dst;
  ny_ret(f, result);
  ny_label(f, 20);
  ny_br(f, 10);
}

static int64_t first_branch_target(const nyir_func_t *f) {
  for (size_t i = 0; i < f->len; ++i)
    if (f->data[i].op == NYIR_BR_IF)
      return f->data[i].imm;
  return -1;
}


static void build_unknown(nyir_func_t *f, int64_t *branch_target) {
  ny_begin(f);
  int cond = nyir_emit(f, (nyir_inst_t){.op = NYIR_LOAD_LOCAL, .dst = -1,
                                        .a = -1, .b = -1, .c = -1, .d = -1,
                                        .e = -1, .f = -1, .imm = 0});
  ny_br(f, 10);
  ny_label(f, 10);
  ny_br_if(f, cond, 30);
  *branch_target = f->data[f->len - 1].imm;
  ny_label(f, 40);
  int value = ny_const(f, 4);
  ny_ret(f, value);
  ny_label(f, 30);
  value = ny_const(f, 9);
  ny_ret(f, value);
}

int main(void) {
  nyir_func_t positive;
  build_positive(&positive);
  if (!ny_verify(&positive, "jump/positive-before"))
    return 1;
  long long before = ny_eval(&positive, "jump/positive-before");
  nyir_jump_thread(&positive);
  if (!ny_verify(&positive, "jump/positive-after"))
    return 1;
  long long after = ny_eval(&positive, "jump/positive-after");
  ny_check("jump-positive", before, after, 1);
  nyir_func_free(&positive);

  nyir_func_t unknown;
  int64_t target = -1;
  build_unknown(&unknown, &target);
  if (!ny_verify(&unknown, "jump/unknown-before"))
    return 1;
  long long unknown_before = ny_eval(&unknown, "jump/unknown-before");
  nyir_jump_thread(&unknown);
  if (!ny_verify(&unknown, "jump/unknown-after"))
    return 1;
  long long unknown_after = ny_eval(&unknown, "jump/unknown-after");
  int64_t after_target = first_branch_target(&unknown);
  if (unknown_before != unknown_after || target != 30 || after_target != 30) {
    fprintf(stderr, "[jump-unknown] branch was not retained\n");
    return 1;
  }
  printf("[jump-unknown] ok result=%lld\n", unknown_after);
  nyir_func_free(&unknown);
  return 0;
}
