/*
 * Assume-marked comparisons as SCEV facts.
 *
 * `NYIR_CMP_I64` with `NYIR_INST_F_ASSUME` means the compare is true at this
 * point without a branch. SCEV must consume it at the instruction level, not
 * just from block terminators, so an entry fact can make a symbolic loop
 * provably entered and IRCE can hoist an invariant check.
 *
 * The negative case clears the flag on the same IR: without the assumption
 * the ranges do not decide `init < limit`, so no symbolic trip exists and the
 * check stays in the body.
 */
#include "code/backend/passes/verify/loop.h"
#include "code/backend/passes/util.h"
#include "opt.h"

static int failures;

/*
 * entry: a = load 1; b = load 2; init = a & 15; limit = b & 15
 *        assume(init < limit)              <- flagged CMP, no branch
 * P:     br H
 * H:     iv = phi(P: init, B: next); if (iv < limit) br B else br X
 * B:     next = iv + 1
 *        bounds_check(locals[0], 0, len = load 3, cap = 64)
 *        br H
 * X:     ret iv
 */
static size_t build_loop(nyr_func_t *f, bool assume) {
  nyr_opt_begin(f);
  int a = nyr_opt_emit_imm(f, NYIR_LOAD_LOCAL, 1);
  int b = nyr_opt_emit_imm(f, NYIR_LOAD_LOCAL, 2);
  int mask = nyr_opt_const(f, 15);
  int init = nyr_opt_binop(f, NYIR_AND_I64, a, mask);
  int limit = nyr_opt_binop(f, NYIR_AND_I64, b, mask);
  int base = nyr_opt_emit_imm(f, NYIR_ADDR_LOCAL, 0);
  int offset = nyr_opt_const(f, 0);
  int length = nyr_opt_emit_imm(f, NYIR_LOAD_LOCAL, 3);
  int assumption = nyr_opt_cmp(f, init, limit, NYIR_CMP_LT);
  if (assume && !nyr_opt_mark_assume(f, assumption)) {
    fprintf(stderr, "[scev-assume] could not mark comparison\n");
    ++failures;
  }
  nyr_opt_br(f, 1);
  nyr_opt_label(f, 1);
  nyr_opt_br(f, 2);
  nyr_opt_label(f, 2);
  int iv = f->next_value;
  nyr_opt_phi(f, 1, init, 3, init);
  size_t phi = f->len - 1;
  int cond = nyr_opt_cmp(f, iv, limit, NYIR_CMP_LT);
  nyr_opt_br_if(f, cond, 3);
  nyr_opt_br(f, 4);
  nyr_opt_label(f, 3);
  int next = nyr_opt_binop(f, NYIR_ADD_I64, iv, nyr_opt_const(f, 1));
  f->data[phi].phi_incoming[1].value = next;
  nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_BOUNDS_CHECK,
                              .dst = -1,
                              .a = base,
                              .b = offset,
                              .c = length,
                              .d = -1,
                              .e = -1,
                              .f = -1,
                              .imm = 64});
  nyr_opt_br(f, 2);
  nyr_opt_label(f, 4);
  nyr_opt_ret(f, iv);
  return phi;
}

static const nyr_scev_loop_t *find_loop(const nyr_scev_info_t *info, int iv) {
  for (size_t i = 0; i < info->count; ++i)
    if (info->loops[i].iv == iv)
      return &info->loops[i];
  return NULL;
}

static bool check_inside(nyr_func_t *f, size_t header, size_t latch,
                         bool *inside, bool *found) {
  nyr_cfg_t cfg = {0};
  *inside = false;
  *found = false;
  if (!nyr_cfg_build(f, &cfg))
    return false;
  bool *members = calloc(cfg.block_count, sizeof(*members));
  if (!members) {
    nyr_cfg_free(&cfg);
    return false;
  }
  bool ok = nyr_cfg_natural_loop_blocks(&cfg, latch, header, members,
                                        cfg.block_count);
  for (size_t i = 0; i < f->len; ++i) {
    if (f->data[i].op != NYIR_BOUNDS_CHECK)
      continue;
    *found = true;
    size_t block = cfg.inst_block[i];
    *inside = ok && block < cfg.block_count && members[block];
    break;
  }
  free(members);
  nyr_cfg_free(&cfg);
  return true;
}

static bool run_case(const char *what, bool assume, bool want_known,
                     bool want_hoisted) {
  nyr_func_t f;
  size_t phi = build_loop(&f, assume);
  int iv = f.data[phi].dst;
  nyr_scev_info_t info = {0};
  bool ok = nyr_opt_verify(&f, what) && nyr_scev_analyze(&f, &info);
  const nyr_scev_loop_t *loop = ok ? find_loop(&info, iv) : NULL;
  ok = ok && loop && loop->trip_expr_known == want_known;
  ok = ok && loop->exit_value_known == want_known;
  ok = ok && nyr_irce(&f) && nyr_opt_verify(&f, "assume-irce");
  bool inside = false, found = false;
  ok = ok && check_inside(&f, loop->header_block, loop->latch_block, &inside,
                          &found);
  ok = ok && found && inside == !want_hoisted;
  if (!ok) {
    fprintf(stderr,
            "[scev-assume] %s: known=%d exit=%d found=%d inside=%d\n", what,
            loop ? (int)loop->trip_expr_known : -1,
            loop ? (int)loop->exit_value_known : -1, (int)found, (int)inside);
    nyr_dump(stderr, &f, what);
    ++failures;
  }
  nyr_scev_free(&info);
  nyr_ir_func_free(&f);
  return ok;
}

int main(void) {
  run_case("assume-entry", true, true, true);
  run_case("no-assume", false, false, false);
  if (failures) {
    fprintf(stderr, "[scev-assume] %d case(s) failed\n", failures);
    return 1;
  }
  printf("[scev-assume] assume-marked compare proves entry and enables hoist, "
         "unmarked compare does not\n");
  return 0;
}
