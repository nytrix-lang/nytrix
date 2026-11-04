/*
 * Driver for nyr_cfg_simplify: CFG cleanup.
 *
 * Regression guard for the PHI-safety gate on trampoline threading:
 * the fixture's X block (label 30) is a branch-only trampoline sitting
 * between loop1's guard fallthrough and P2 — exactly the shape the
 * threading path wants to collapse.  Collapsing must never reroute an
 * edge into a block whose PHIs would gain a wrong predecessor label,
 * so both runs below (before and after fusion) must stay verifier-clean
 * and evaluate identically.
 */
#include "code/backend/ir/opt/loop.h"
#include "support.h"

static bool false_branch_preserves_debug(void) {
  static const char source[] = "cfg-simplify-origin.ny";
  nyr_func_t f;
  nyr_opt_begin(&f);
  int zero = nyr_opt_const(&f, 0);
  size_t branch_at = f.len;
  nyr_opt_br_if(&f, zero, 10);
  f.data[branch_at].debug =
      (nyr_debug_loc_t){.file = source, .line = 5, .column = 9};
  nyr_opt_br(&f, 20);
  nyr_opt_label(&f, 10);
  int taken = nyr_opt_const(&f, 17);
  nyr_opt_ret(&f, taken);
  nyr_opt_label(&f, 20);
  int fallthrough = nyr_opt_const(&f, 29);
  nyr_opt_ret(&f, fallthrough);

  bool ok = nyr_opt_verify(&f, "cfg/debug-before") &&
            nyr_opt_eval(&f, "cfg/debug-before") == 29 &&
            nyr_cfg_simplify(&f) && nyr_opt_verify(&f, "cfg/debug-after") &&
            nyr_opt_eval(&f, "cfg/debug-after") == 29 &&
            f.data[branch_at].op == NYIR_NOP &&
            f.data[branch_at].debug.file == source &&
            f.data[branch_at].debug.line == 5 &&
            f.data[branch_at].debug.column == 9;
  if (!ok)
    fprintf(stderr,
            "[cfg] false-branch rewrite lost debug location or result\n");
  nyr_ir_func_free(&f);
  return ok;
}

static bool sparse_labels_thread_without_linear_scans(void) {
  const int64_t first = 4096;
  nyr_func_t f;
  nyr_opt_begin(&f);
  size_t entry_branch = f.len;
  nyr_opt_br(&f, first);
  nyr_opt_label(&f, first);
  nyr_opt_br(&f, first + 1);
  nyr_opt_label(&f, first + 1);
  nyr_opt_br(&f, first + 2);
  nyr_opt_label(&f, first + 2);
  int result = nyr_opt_const(&f, 37);
  nyr_opt_ret(&f, result);

  bool ok = nyr_opt_verify(&f, "cfg/sparse-before") &&
            nyr_opt_eval(&f, "cfg/sparse-before") == 37 &&
            nyr_cfg_simplify(&f) && nyr_opt_verify(&f, "cfg/sparse-after") &&
            nyr_opt_eval(&f, "cfg/sparse-after") == 37 &&
            f.data[entry_branch].op == NYIR_BR &&
            f.data[entry_branch].imm == first + 2;
  if (!ok)
    fprintf(stderr,
            "[cfg] sparse-label lookup failed to thread branches safely\n");
  nyr_ir_func_free(&f);
  return ok;
}

static bool dominators_handle_loops_and_unreachable_blocks(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  nyr_opt_br(&f, 10);
  nyr_opt_label(&f, 10);
  int condition = nyr_opt_const(&f, 1);
  nyr_opt_br_if(&f, condition, 20);
  nyr_opt_br(&f, 15);
  nyr_opt_label(&f, 15);
  nyr_opt_br(&f, 10);
  nyr_opt_label(&f, 20);
  nyr_opt_ret(&f, condition);
  nyr_opt_label(&f, 99);
  nyr_opt_ret(&f, condition);

  nyr_cfg_t cfg = {0};
  size_t *rpo = NULL;
  size_t rpo_len = 0;
  bool ok = nyr_cfg_build(&f, &cfg) &&
            nyr_cfg_reverse_postorder(&cfg, &rpo, &rpo_len);
  size_t header = SIZE_MAX;
  size_t latch = SIZE_MAX;
  size_t exit = SIZE_MAX;
  size_t dead = SIZE_MAX;
  size_t entry = 0;
  size_t reachable_count = 0;
  if (ok) {
    for (size_t block = 0; block < cfg.block_count; ++block) {
      reachable_count += cfg.reachable[block];
      if (cfg.block_label[block] == 10)
        header = block;
      else if (cfg.block_label[block] == 15)
        latch = block;
      else if (cfg.block_label[block] == 20)
        exit = block;
      else if (cfg.block_label[block] == 99)
        dead = block;
    }
    bool saw_entry = false;
    for (size_t i = 0; i < rpo_len; ++i)
      saw_entry |= rpo[i] == entry;
    ok = header != SIZE_MAX && latch != SIZE_MAX && exit != SIZE_MAX &&
         dead != SIZE_MAX && saw_entry && rpo_len == reachable_count &&
         cfg.reachable[header] && cfg.reachable[latch] && cfg.reachable[exit] &&
         !cfg.reachable[dead] && nyr_cfg_dominates(&cfg, entry, header) &&
         nyr_cfg_dominates(&cfg, header, latch) &&
         nyr_cfg_dominates(&cfg, header, exit) &&
         !nyr_cfg_dominates(&cfg, latch, exit) &&
         nyr_cfg_dominates(&cfg, dead, dead) && cfg.idom[dead] == -1;
  }
  if (!ok)
    fprintf(stderr,
            "[cfg] dominators mishandled loop or unreachable block "
            "(blocks=%zu rpo=%zu entry=%zu header=%zu latch=%zu exit=%zu "
            "dead=%zu)\n",
            cfg.block_count, rpo_len, entry, header, latch, exit, dead);
  free(rpo);
  nyr_cfg_free(&cfg);
  nyr_ir_func_free(&f);
  return ok;
}

static bool dominators_use_rpo_for_large_reverse_chains(void) {
  const int64_t last_label = 63;
  nyr_func_t f;
  nyr_opt_begin(&f);
  int result = nyr_opt_const(&f, 42);
  nyr_opt_br(&f, last_label);
  for (int64_t label = 1; label <= last_label; ++label) {
    nyr_opt_label(&f, label);
    if (label == 1)
      nyr_opt_ret(&f, result);
    else
      nyr_opt_br(&f, label - 1);
  }

  nyr_cfg_t cfg = {0};
  size_t *rpo = NULL;
  size_t rpo_len = 0;
  bool ok = nyr_cfg_build(&f, &cfg) &&
            nyr_cfg_reverse_postorder(&cfg, &rpo, &rpo_len) &&
            cfg.block_count == 64 && rpo_len == 64;
  if (ok) {
    for (size_t order = 0; order < rpo_len; ++order) {
      size_t expected = order == 0 ? 0 : (size_t)(64 - order);
      if (rpo[order] != expected) {
        fprintf(stderr, "[cfg] rpo[%zu]=%zu expected=%zu\n", order, rpo[order],
                expected);
        ok = false;
        break;
      }
    }
    for (size_t block = 0; ok && block < cfg.block_count; ++block) {
      if (!cfg.reachable[block] || !nyr_cfg_dominates(&cfg, 0, block)) {
        fprintf(stderr, "[cfg] block %zu is unreachable or not dominated\n",
                block);
        ok = false;
      }
    }
    for (int64_t label = 1; ok && label <= last_label; ++label) {
      int expected_idom = (int)(label == last_label ? 0 : label + 1);
      if (cfg.idom[label] != expected_idom) {
        fprintf(stderr, "[cfg] idom[%lld]=%d expected=%d\n", (long long)label,
                cfg.idom[label], expected_idom);
        ok = false;
      }
    }
  }
  if (!ok)
    fprintf(stderr, "[cfg] reverse-chain dominance RPO was not preserved\n");
  free(rpo);
  nyr_cfg_free(&cfg);
  nyr_ir_func_free(&f);
  return ok;
}

int main(void) {
  if (!false_branch_preserves_debug() ||
      !sparse_labels_thread_without_linear_scans() ||
      !dominators_handle_loops_and_unreachable_blocks() ||
      !dominators_use_rpo_for_large_reverse_chains())
    return 1;
  /*
   * Before fusion: trampolines exist, headers still have PHIs.
   */
  nyr_func_t f;
  nyr_opt_build_two_loops(&f, 3, 3);
  if (!nyr_opt_verify(&f, "cfg/verify-before"))
    return 1;
  long long before = nyr_opt_eval(&f, "cfg/before");
  nyr_cfg_simplify(&f);
  if (!nyr_opt_verify(&f, "cfg/verify-mid"))
    return 1;
  long long mid = nyr_opt_eval(&f, "cfg/mid");

  /*
   * After fusion: moved PHIs make the merged header extra sensitive to
   * predecessor-label rewrites.
   */
  if (!nyr_affine_nest(&f)) {
    fprintf(stderr, "[cfg] fusion setup failed\n");
    return 1;
  }
  nyr_cfg_simplify(&f);
  if (!nyr_opt_verify(&f, "cfg/verify-after"))
    return 1;
  long long after = nyr_opt_eval(&f, "cfg/after");
  if (nyr_opt_want_dump())
    nyr_dump(stdout, &f, "after-cfg");
  nyr_ir_func_free(&f);
  nyr_opt_check("cfg", before, mid, 209);
  nyr_opt_check("cfg-fused", before, after, 209);
  return 0;
}
