/*
 * Focused jump-threading regression: PHI edge updates and unknown retention.
 */
#include "opt.h"

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
  int cond = nyr_opt_emit_imm(f, NYIR_LOAD_LOCAL, 0);
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

static void build_live_out_phi(nyr_func_t *f) {
  nyr_opt_begin(f);
  int condition = nyr_opt_emit_imm(f, NYIR_LOAD_LOCAL, 0);
  int zero = nyr_opt_const(f, 0);
  int one = nyr_opt_const(f, 1);
  int from_false = nyr_opt_const(f, 42);
  int from_true = nyr_opt_const(f, 99);
  nyr_opt_br_if(f, condition, 11);
  nyr_opt_label(f, 10);
  nyr_opt_br(f, 20);
  nyr_opt_label(f, 11);
  nyr_opt_br(f, 20);
  nyr_opt_label(f, 20);
  phi2(f, 10, zero, 11, one);
  int branch = f->data[f->len - 1].dst;
  phi2(f, 10, from_false, 11, from_true);
  int value = f->data[f->len - 1].dst;
  nyr_opt_br_if(f, branch, 30);
  nyr_opt_label(f, 40);
  nyr_opt_ret(f, value);
  nyr_opt_label(f, 30);
  nyr_opt_ret(f, value);
}

int main(void) {
  nyr_func_t no_values;
  nyr_opt_begin(&no_values);
  nyr_opt_br(&no_values, 10);
  nyr_opt_label(&no_values, 10);
  nyr_opt_ret(&no_values, -1);
  if (!nyr_opt_verify(&no_values, "jump/no-values-before"))
    return 1;
  if (!nyr_jump_thread(&no_values) ||
      !nyr_opt_verify(&no_values, "jump/no-values-after") ||
      no_values.len != 3 || no_values.data[0].op != NYIR_BR ||
      no_values.data[1].op != NYIR_LABEL || no_values.data[2].op != NYIR_RET) {
    fprintf(stderr,
            "[jump-no-values] empty-value CFG was rejected or changed\n");
    return 1;
  }
  nyr_ir_func_free(&no_values);

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

  nyr_func_t live_out;
  build_live_out_phi(&live_out);
  if (!nyr_opt_verify(&live_out, "jump/live-out-phi-before"))
    return 1;
  int64_t inputs[] = {0, 1};
  int64_t expected[] = {42, 99};
  for (size_t i = 0; i < 2; ++i) {
    int64_t result = 0;
    if (!nyr_opt_eval_locals(&live_out, "jump/live-out-phi-before", &inputs[i],
                             1, &result) ||
        result != expected[i]) {
      fprintf(stderr, "[jump-live-out-phi] bad input result\n");
      return 1;
    }
  }
  if (!nyr_jump_thread(&live_out) ||
      !nyr_opt_verify(&live_out, "jump/live-out-phi-after"))
    return 1;
  for (size_t i = 0; i < 2; ++i) {
    int64_t result = 0;
    if (!nyr_opt_eval_locals(&live_out, "jump/live-out-phi-after", &inputs[i],
                             1, &result) ||
        result != expected[i]) {
      fprintf(stderr,
              "[jump-live-out-phi] threading changed input %zu result\n", i);
      nyr_dump(stderr, &live_out, "jump-live-out-phi-failure");
      return 1;
    }
  }
  nyr_ir_func_free(&live_out);
  return 0;
}
