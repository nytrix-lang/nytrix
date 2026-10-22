/*
 * Driver for nyr_adce: aggressive dead code elimination.
 *
 * Regression guard for the PHI-blindness fix: liveness must propagate
 * through phi_incoming entries, including PHIs in unreachable blocks.
 * The input is fused two-loop IR (util.h fixture), which deliberately
 * contains dead definitions — the abandoned second comparison and its
 * body leftovers — next to moved PHIs whose backedge operands are the
 * only keepers of some values.  ADCE must reap exactly the dead defs
 * and leave evaluation untouched.
 */
#include "code/backend/ir/opt/loop.h"
#include "support.h"

static bool preserves_dead_arithmetic_trap(nyr_op_t op) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int one = nyr_opt_const(&f, 1);
  int divisor = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                            .dst = -1,
                                            .a = -1,
                                            .b = -1,
                                            .c = -1,
                                            .d = -1,
                                            .e = -1,
                                            .f = -1,
                                            .imm = 0});
  int quotient = nyr_opt_binop(&f, op, one, divisor);
  nyr_opt_ret(&f, nyr_opt_const(&f, 7));
  size_t div_index = 0;
  while (div_index < f.len && f.data[div_index].dst != quotient)
    ++div_index;
  if (quotient < 0 || div_index == f.len ||
      !(f.data[div_index].effects & NYIR_EFFECT_MAY_TRAP) ||
      !nyr_opt_verify(&f, "adce/trap-before")) {
    nyr_ir_func_free(&f);
    return false;
  }
  nyr_eval_result_t before = {0}, after = {0};
  int64_t locals[1] = {0};
  char err[256] = {0};
  bool before_ok = nyr_ir_eval(&f, locals, 1, 64, &before, err, sizeof(err));
  nyr_eval_result_free(&before);
  if (!nyr_adce(&f) || !nyr_opt_verify(&f, "adce/trap-after")) {
    nyr_ir_func_free(&f);
    return false;
  }
  err[0] = '\0';
  bool after_ok = nyr_ir_eval(&f, locals, 1, 64, &after, err, sizeof(err));
  nyr_eval_result_free(&after);
  bool has_trapping_op = false;
  for (size_t i = 0; i < f.len; ++i)
    has_trapping_op |= f.data[i].op == op;
  bool ok = !before_ok && !after_ok && has_trapping_op;
  if (!ok)
    fprintf(stderr,
            "[adce] dead trapping arithmetic changed behavior (op=%d before=%d "
            "after=%d retained=%d err=%s)\n",
            op, before_ok, after_ok, has_trapping_op, err);
  nyr_ir_func_free(&f);
  return ok;
}

static bool eliminates_only_known_pure_unused_calls(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int arg = nyr_opt_const(&f, 9);
  int result = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_CALL,
                                           .dst = -1,
                                           .a = arg,
                                           .b = -1,
                                           .c = -1,
                                           .d = -1,
                                           .e = -1,
                                           .f = -1,
                                           .imm = 1,
                                           .symbol = "rt_tag"});
  int decoded = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_CALL,
                                            .dst = -1,
                                            .a = result,
                                            .b = -1,
                                            .c = -1,
                                            .d = -1,
                                            .e = -1,
                                            .f = -1,
                                            .imm = 1,
                                            .symbol = "rt_any_to_i64"});
  int unknown = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_CALL,
                                            .dst = -1,
                                            .a = arg,
                                            .b = -1,
                                            .c = -1,
                                            .d = -1,
                                            .e = -1,
                                            .f = -1,
                                            .imm = 1,
                                            .symbol = "unknown_side_effect"});
  nyr_opt_ret(&f, arg);
  bool ok = result >= 0 && decoded >= 0 && unknown >= 0 &&
            nyr_call_effect_summary(&f.data[2]) ==
                (NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY) &&
            nyr_call_effect_summary(&f.data[1]) == NYIR_EFFECT_CALL &&
            nyr_opt_verify(&f, "adce/pure-call-before") && nyr_adce(&f) &&
            nyr_opt_verify(&f, "adce/pure-call-after");
  bool saw_tag = false, saw_decode = false, saw_unknown = false;
  for (size_t i = 0; i < f.len; ++i) {
    saw_tag |= f.data[i].op == NYIR_CALL && f.data[i].symbol &&
               strcmp(f.data[i].symbol, "rt_tag") == 0;
    saw_decode |= f.data[i].op == NYIR_CALL && f.data[i].symbol &&
                  strcmp(f.data[i].symbol, "rt_any_to_i64") == 0;
    saw_unknown |= f.data[i].op == NYIR_CALL && f.data[i].symbol &&
                   strcmp(f.data[i].symbol, "unknown_side_effect") == 0;
  }
  ok = ok && !saw_tag && !saw_decode && saw_unknown;
  if (!ok) {
    fprintf(stderr,
            "[adce] call cleanup mismatch: tag=%d decode=%d unknown=%d "
            "effects=%u/%u\n",
            saw_tag, saw_decode, saw_unknown,
            nyr_call_effect_summary(&f.data[1]),
            nyr_call_effect_summary(&f.data[2]));
    nyr_dump(stderr, &f, "adce/call-cleanup");
  }
  nyr_ir_func_free(&f);
  return ok;
}

int main(void) {
  nyr_func_t f;
  nyr_opt_build_two_loops(&f, 3, 3);
  if (!nyr_affine_nest(&f)) {
    fprintf(stderr, "[adce] fusion setup failed\n");
    return 1;
  }
  if (!nyr_opt_verify(&f, "adce/verify-before"))
    return 1;
  long long before = nyr_opt_eval(&f, "adce/before");
  nyr_adce(&f);
  if (!nyr_opt_verify(&f, "adce/verify-after"))
    return 1;
  long long after = nyr_opt_eval(&f, "adce/after");
  if (nyr_opt_want_dump())
    nyr_dump(stdout, &f, "after-adce");
  nyr_ir_func_free(&f);
  nyr_opt_check("adce", before, after, 209);
  return preserves_dead_arithmetic_trap(NYIR_DIV_I64) &&
                 preserves_dead_arithmetic_trap(NYIR_MOD_I64) &&
                 eliminates_only_known_pure_unused_calls()
             ? 0
             : 1;
}
