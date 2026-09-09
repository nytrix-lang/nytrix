/*
 * NYIR optimization pipeline: sequences passes in order, manages
 * per-pass verification, statistics, and early-stop diagnostics.
 *
 * O1 passes: const_fold, sccp, copy_prop, cse, peephole, cfg_simplify,
 *   dce, licm, strength_reduce, alias_store_sink, null_align_facts,
 *   bounds_check_elim, redundant_load_elim.
 *
 * O2 adds: loop_rotate, scev_lite, irce, loop_idiom, loop_interchange,
 *   loop_versioning, loop_predication, loop_unswitch, loop_vectorize,
 *   iv_elim, loop_unroll, inline_general, inline_small.
 *
 * O3 adds: slp_vectorize, unroll_jam, softpipe_rewrite,
 *   tbuf_private_object, sroa_scalar, dead_store_elim,
 *   adce, icp_profile, block_layout.
 *
 * NOP bloat: licm and alias_store_sink (the loop-phase NOP producers)
 * are followed by nyir_compact_if_sparse, which drains NYIR_NOPs
 * immediately so the instruction array doesn't grow across loop passes;
 * other phase tails already compact.
 *
 * References:
 *  QBE (https://c9x.me/git/qbe.git, e786f06032fefa2e3790d6b1c9e31ed138f475a6)
 *   — compact, independently verifiable CFG/SSA pass boundaries. This
 *   pipeline is Nytrix-owned; no QBE source is incorporated.
 *  Cooper & Torczon — Engineering a Compiler, 2nd Ed., Ch.9
 *   (pass ordering, dataflow analysis structure).
 *  Muchnick — Advanced Compiler Design and Implementation, Ch.12
 *   (phase coupling, pass dependencies).
 *  LLVM — Pass Manager Documentation (llvm.org/docs/Passes.html)
 *   (canonical O2 pipeline order).
 */
#include "code/ir/opt/util.h"
#include "code/ir/opt/loop.h"
#include "code/ir/internal.h"
#include "base/compat.h"
#include "base/common.h"
#include "base/util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *nyir_debug_pass_name(bool (*pass)(nyir_func_t *));
static const char *nyir_disabled_pass = NULL;
static const char *nyir_stop_after_pass = NULL;
static bool nyir_pass_stopped = false;
static bool nyir_verify_each_pass = false;

bool nyir_loop_unswitch(nyir_func_t *f);
bool nyir_iv_elim(nyir_func_t *f);
bool nyir_gvn_pre(nyir_func_t *f);
bool nyir_loop_vectorize(nyir_func_t *f);
bool nyir_slp_vectorize(nyir_func_t *f);
bool nyir_memcombine(nyir_func_t *f);
bool nyir_alias_store_sink(nyir_func_t *f);
bool nyir_narrow(nyir_func_t *f);
bool nyir_phi_elim(nyir_func_t *f);

void nyir_set_pass_controls(const char *disable_pass, const char *stop_after) {
  nyir_disabled_pass = disable_pass && disable_pass[0] ? disable_pass : NULL;
  nyir_stop_after_pass = stop_after && stop_after[0] ? stop_after : NULL;
}

void nyir_set_verify_each_pass(bool enable) {
  nyir_verify_each_pass = enable;
}

static bool nyir_per_pass_oracle = false;
static nyir_per_pass_oracle_fn nyir_oracle_fn = NULL;
static void *nyir_oracle_userdata = NULL;
static int nyir_tv_trials = 0;

void nyir_set_per_pass_oracle(bool enable, nyir_per_pass_oracle_fn fn,
                                void *userdata) {
  nyir_per_pass_oracle = enable;
  nyir_oracle_fn = enable ? fn : NULL;
  nyir_oracle_userdata = enable ? userdata : NULL;
}

void nyir_set_tv_seed(int trials) {
  nyir_tv_trials = trials > 0 ? trials : 0;
}

static bool nyir_oracle_checkpoint(const nyir_func_t *f,
                                     const char *pass_name) {
  if (!nyir_per_pass_oracle || !nyir_oracle_fn)
    return true;
  bool ok = nyir_oracle_fn(f, pass_name, nyir_oracle_userdata);
  if (!ok) {
    fprintf(stderr, "native NYIR: per-pass oracle failed after %s\n",
            pass_name ? pass_name : "pass");
  }
  return ok;
}

static bool nyir_tv_checkpoint(const nyir_func_t *before,
                                 const nyir_func_t *after,
                                 const char *pass_name) {
  if (nyir_tv_trials <= 0 || !before || !after)
    return true;
  char err[256] = {0};
  /*
   * Prefer SMT (Z3) for pure straight-line; multi-input covers CFG.
   */
  bool ok = nyir_tv_smt_equiv(before, after, err, sizeof(err));
  if (!ok && err[0] == '\0')
    ok = nyir_tv_equiv_straightline(before, after, nyir_tv_trials, err,
                                      sizeof(err));
  if (ok)
    return true;
  fprintf(stderr, "native NYIR: translation validation failed after %s: %s\n",
          pass_name ? pass_name : "pass", err[0] ? err : "mismatch");
  return false;
}

static bool nyir_verify_checkpoint(nyir_func_t *f, const char *pass_name) {
  if (!nyir_verify_each_pass)
    return true;
  char err[256] = {0};
  if (nyir_verify(f, err, sizeof(err)))
    return true;
  fprintf(stderr, "native NYIR: verification failed after %s: %s\n",
          pass_name ? pass_name : "pass", err[0] ? err : "unknown error");
  return false;
}

static bool nyir_pass_is_skipped(bool (*pass)(nyir_func_t *)) {
  if (nyir_pass_stopped)
    return true;
  const char *name = nyir_debug_pass_name(pass);
  return nyir_disabled_pass && strcmp(nyir_disabled_pass, name) == 0;
}

static void nyir_finish_pass(bool (*pass)(nyir_func_t *)) {
  const char *name = nyir_debug_pass_name(pass);
  if (nyir_stop_after_pass && strcmp(nyir_stop_after_pass, name) == 0)
    nyir_pass_stopped = true;
}

static bool timed_pass(nyir_func_t *f, bool (*pass)(nyir_func_t *),
                       double *out_ms) {
  if (nyir_pass_is_skipped(pass)) {
    if (out_ms)
      *out_ms = 0;
    /*
     * A stop_after target that is also disabled must still end the
     * pipeline here; otherwise the marker never sets and every later
     * pass silently runs.
     */
    nyir_finish_pass(pass);
    return true;
  }
  const char *name = nyir_debug_pass_name(pass);
  nyir_func_t before = {0};
  bool want_tv = nyir_tv_trials > 0;
  if (want_tv && !nyir_func_clone(f, &before)) {
    if (out_ms)
      *out_ms = 0;
    return false;
  }
  ny_tick_t t0 = ny_ticks_now();
  bool ok = pass(f);
  if (ok)
    nyir_refresh_metadata(f);
  if (ok)
    ok = nyir_verify_checkpoint(f, name);
  if (ok)
    ok = nyir_tv_checkpoint(&before, f, name);
  if (ok)
    ok = nyir_oracle_checkpoint(f, name);
  if (!ok && ny_trace_enabled("NY_TRACE_PASS_FAIL"))
    ny_trace_line("PASS_FAIL", "nyir pass '%s' failed (insts=%zu values=%d)",
                  name, f->len, f->next_value);
  if (out_ms)
    *out_ms = ny_ticks_elapsed_ms(t0);
  if (ok)
    nyir_finish_pass(pass);
  nyir_func_free(&before);
  return ok;
}

static bool nyir_cf_mem2reg_enabled = true;

void nyir_set_cf_mem2reg_enabled(bool enable) {
  nyir_cf_mem2reg_enabled = enable;
}

static bool nyir_preserve_phis = false;

void nyir_set_preserve_phis(bool preserve) {
  nyir_preserve_phis = preserve;
}

bool nyir_get_preserve_phis(void) {
  return nyir_preserve_phis;
}

typedef struct {
  bool (*pass)(nyir_func_t *);
  const char *name;
} nyir_pass_control_t;

static const nyir_pass_control_t nyir_pass_controls[] = {
    {nyir_const_fold, "const_fold"},
    {nyir_peephole, "peephole"},
    {nyir_copy_prop, "copy_prop"},
    {nyir_cse, "cse"},
    {nyir_strength_reduce, "strength_reduce"},
    {nyir_cfg_simplify, "cfg_simplify"},
    {nyir_phiopt, "phiopt"},
    {nyir_if_convert, "if_convert"},
    {nyir_licm, "licm"},
    {nyir_dce, "dce"},
    {nyir_dead_arg_elim, "dead_arg_elim"},
    {nyir_ipa_constprop, "ipa_constprop"},
    {nyir_dead_store_elim, "dead_store_elim"},
    {nyir_redundant_load_elim, "redundant_load_elim"},
    {nyir_jump_thread, "jump_thread"},
    {nyir_compact, "compact"},
    {nyir_compact_if_sparse, "compact_sparse"},
    {nyir_mem2reg, "mem2reg"},
    {nyir_sccp, "sccp"},
    {nyir_null_align_facts, "null_align_facts"},
    {nyir_iv_simplify, "iv_simplify"},
    {nyir_bounds_check_elim, "bounds_check_elim"},
    {nyir_egraph_local, "egraph_local"},
    {nyir_egraph_rewrite, "egraph_saturate"},
    {nyir_apply_rules, "apply_rules"},
    {nyir_inline_small, "inline_small"},
    {nyir_algebraic_combine, "algebraic_combine"},
    {nyir_double_neg, "double_neg"},
    {nyir_reassoc_add, "reassoc_add"},
    {nyir_memory_ssa_forward, "memory_ssa_forward"},
    {nyir_escape_sroa, "escape_sroa"},
    {nyir_tbuf_scalar_len, "tbuf_scalar_len"},
    {nyir_tbuf_private_object, "tbuf_private_object"},
    {nyir_sroa_scalar, "sroa_scalar"},
    {nyir_block_layout, "block_layout"},
    {nyir_points_to_sroa, "points_to_sroa"},
    {nyir_store_sink, "store_sink"},
    {nyir_aggregate_sroa, "aggregate_sroa"},
    {nyir_polyhedral_nest, "polyhedral_nest"},
    {nyir_affine_nest, "affine_nest"},
    {nyir_kernel_hint, "kernel_hint"},
    {nyir_icp_profile, "icp_profile"},
    {nyir_inline_general, "inline_general"},
    {nyir_loop_unroll, "loop_unroll"},
    {nyir_loop_peel, "loop_peel"},
    {nyir_unroll_jam, "unroll_jam"},
    {nyir_softpipe_rewrite, "softpipe_rewrite"},
    {nyir_scev_lite, "scev_lite"},
    {nyir_irce, "irce"},
    {nyir_loop_idiom, "loop_idiom"},
    {nyir_loop_rotate, "loop_rotate"},
    {nyir_loop_interchange, "loop_interchange"},
    {nyir_prefetch_insert, "prefetch_insert"},
    {nyir_polyhedral_tiling, "polyhedral_tiling"},
    {nyir_loop_versioning, "loop_versioning"},
    {nyir_loop_predication, "loop_predication"},
    {nyir_loop_unswitch, "loop_unswitch"},
    {nyir_iv_elim, "iv_elim"},
    {nyir_gvn_pre, "gvn_pre"},
    {nyir_loop_vectorize, "loop_vectorize"},
    {nyir_slp_vectorize, "slp_vectorize"},
    {nyir_alias_store_sink, "alias_store_sink"},
    {nyir_narrow, "narrow"},
    {nyir_phi_elim, "phi_elim"},
    {nyir_adce, "adce"},
    {nyir_null_check_elim, "null_check_elim"},
    {nyir_overflow_check_elim, "overflow_check_elim"},
    {nyir_vrp, "vrp"},
    {nyir_cvp, "cvp"},
    {nyir_memcombine, "memcombine"},
    {nyir_alias_analysis, "alias_analysis"},
};

static bool nyir_pass_control_valid(const char *name) {
  if (!name || !name[0] || strcmp(name, "scalar_cleanup") == 0)
    return true;
  for (size_t i = 0; i < sizeof(nyir_pass_controls) /
                              sizeof(nyir_pass_controls[0]); ++i)
    if (strcmp(name, nyir_pass_controls[i].name) == 0)
      return true;
  return false;
}

static bool nyir_optimize_pipeline(nyir_func_t *f, nyir_opt_stats_t *stats, int opt_level, FILE *dump);

bool nyir_optimize_with_stats(nyir_func_t *f, nyir_opt_stats_t *stats,
                                int opt_level) {
  return nyir_optimize_pipeline(f, stats, opt_level, NULL);
}

bool nyir_optimize(nyir_func_t *f, int opt_level) {
  return nyir_optimize_with_stats(f, NULL, opt_level);
}

static const char *nyir_debug_pass_name(bool (*pass)(nyir_func_t *)) {
  for (size_t i = 0; i < sizeof(nyir_pass_controls) /
                              sizeof(nyir_pass_controls[0]); ++i)
    if (pass == nyir_pass_controls[i].pass)
      return nyir_pass_controls[i].name;
  return "unknown";
}

static bool timed_pass_verified(nyir_func_t *f, bool (*pass)(nyir_func_t *),
                                 double *out_ms, FILE *dump,
                                 bool *ok) {
  const char *pass_name = nyir_debug_pass_name(pass);
  char tag[64];
  nyir_pass_tag(tag, sizeof(tag), pass_name);
  if (nyir_pass_is_skipped(pass)) {
    if (out_ms)
      *out_ms = 0;
    if (dump)
      fprintf(dump, "· %s  skip\n", tag);
    return true;
  }
  size_t before_insts = f ? f->len : 0;
  int before_values = f ? f->next_value : 0;
  size_t before_blocks = nyir_block_count(f);
  uint64_t before = nyir_debug_fingerprint(f);
  nyir_func_t tv_before = {0};
  nyir_func_t dump_before = {0};
  bool want_tv = nyir_tv_trials > 0;
  bool want_dump_diff = dump != NULL;
  if (want_tv && !nyir_func_clone(f, &tv_before)) {
    *ok = false;
    if (out_ms)
      *out_ms = 0;
    return false;
  }
  if (want_dump_diff && !nyir_func_clone(f, &dump_before)) {
    nyir_func_free(&tv_before);
    *ok = false;
    if (out_ms)
      *out_ms = 0;
    return false;
  }
  ny_tick_t t0 = ny_ticks_now();
  *ok = pass(f);
  if (*ok)
    nyir_refresh_metadata(f);
  if (out_ms)
    *out_ms = ny_ticks_elapsed_ms(t0);
  if (*ok) {
    char vbuf[256] = {0};
    if (!nyir_verify(f, vbuf, sizeof(vbuf))) {
      /*
       * A folding pass may make a formerly reachable successor dead. That is
       * a normal transient CFG state, not a verifier escape hatch: normalize
       * it before the checkpoint, then require the strict verifier to pass.
       */
      if (strstr(vbuf, "unreachable CFG block") && nyir_cfg_simplify(f)) {
        nyir_refresh_metadata(f);
        memset(vbuf, 0, sizeof(vbuf));
      }
      if (!nyir_verify(f, vbuf, sizeof(vbuf))) {
        if (dump)
          fprintf(dump, "· %s  VERIFY FAIL: %s\n", tag, vbuf);
        *ok = false;
      }
    }
  }
  if (*ok)
    *ok = nyir_tv_checkpoint(&tv_before, f, pass_name);
  if (*ok)
    *ok = nyir_oracle_checkpoint(f, pass_name);
  nyir_func_free(&tv_before);
  if (dump) {
    size_t after_blocks = nyir_block_count(f);
    uint64_t after = nyir_debug_fingerprint(f);
    double ms = out_ms ? *out_ms : ny_ticks_elapsed_ms(t0);
    bool changed = before != after;
    /*
     * One compact status line: · <cfg-simplify>  Δ i25 b6→5  0.0ms
     */
    fprintf(dump, "· %s  ", tag);
    if (!changed) {
      fprintf(dump, "·  %.2fms\n", ms);
    } else {
      fprintf(dump, "Δ");
      if (before_insts != (f ? f->len : 0))
        fprintf(dump, " i%zu→%zu", before_insts, f ? f->len : 0);
      int after_values = f ? f->next_value : 0;
      if (before_values != after_values)
        fprintf(dump, " v%d→%d", before_values, after_values);
      if (before_blocks != after_blocks)
        fprintf(dump, " b%zu→%zu", before_blocks, after_blocks);
      if (before_insts == (f ? f->len : 0) && before_values == after_values &&
          before_blocks == after_blocks)
        fprintf(dump, " fp");
      fprintf(dump, "  %.2fms\n", ms);
      /*
       * Compact per-line diff instead of full IR re-dump.
       */
      nyir_dump_diff(dump, &dump_before, f, tag);
    }
  }
  nyir_func_free(&dump_before);
  if (*ok)
    nyir_finish_pass(pass);
  return *ok;
}

static void nyir_stats_append(nyir_opt_stats_t *stats,
                                const char *name, double ms,
                                size_t before_insts, size_t after_insts,
                                int before_values, int after_values,
                                size_t before_blocks, size_t after_blocks) {
  if (!stats)
    return;
  stats->total_time_ms += ms;
  if (stats->pass_count >= NYIR_MAX_PASS_STATS)
    return;
  stats->passes[stats->pass_count++] = (nyir_pass_stat_t){
      .name = name, .time_ms = ms,
      .before_insts = before_insts, .after_insts = after_insts,
      .before_values = before_values, .after_values = after_values,
      .before_blocks = before_blocks, .after_blocks = after_blocks};
}

/*
 * Shared O0–O3 pipeline. dump!=NULL enables verified+compact-diff mode.
 */
static bool nyir_run_pass(nyir_func_t *f, bool (*pass)(nyir_func_t *),
                            nyir_opt_stats_t *stats, FILE *dump, bool *ok) {
  bool skipped = nyir_pass_is_skipped(pass);
  size_t before = f ? f->len : 0;
  int before_values = f ? f->next_value : 0;
  size_t before_blocks = nyir_block_count(f);
  double ms = 0;
  bool r = dump ? timed_pass_verified(f, pass, &ms, dump, ok)
                : timed_pass(f, pass, &ms);
  if (!r)
    *ok = false;
  if (!skipped)
    nyir_stats_append(stats, nyir_debug_pass_name(pass), ms, before,
                        f ? f->len : 0, before_values,
                        f ? f->next_value : 0, before_blocks,
                        nyir_block_count(f));
  return r;
}

static bool nyir_run_seq(nyir_func_t *f,
                           bool (*const *passes)(nyir_func_t *), int n,
                           nyir_opt_stats_t *stats, FILE *dump, bool *ok) {
  for (int i = 0; i < n && *ok; ++i)
    if (!nyir_run_pass(f, passes[i], stats, dump, ok))
      return false;
  return *ok;
}

static bool nyir_run_finish_scalar(nyir_func_t *f,
                                      nyir_opt_stats_t *stats, FILE *dump,
                                      bool *ok) {
  if (!*ok || nyir_pass_stopped)
    return *ok;
  if (nyir_disabled_pass &&
      strcmp(nyir_disabled_pass, "scalar_cleanup") == 0) {
    if (dump)
      fprintf(dump, "· <scalar-cleanup>  skip\n");
    return true;
  }

  bool has_cf = nyir_has_control_flow(f);
  bool has_locals = nyir_has_local_mem(f);
  if (f->len <= 2 && !has_locals && !has_cf) {
    nyir_run_pass(f, nyir_compact, stats, dump, ok);
  } else {
    static bool (*const base[])(nyir_func_t *) = {
        nyir_const_fold, nyir_strength_reduce, nyir_narrow, nyir_peephole, nyir_apply_rules,
        nyir_copy_prop, nyir_redundant_load_elim, nyir_bounds_check_elim, nyir_cse,
        nyir_tbuf_scalar_len, nyir_tbuf_private_object};
    nyir_run_seq(f, base, 11, stats, dump, ok);
    if (*ok && has_locals) {
      static bool (*const memory[])(nyir_func_t *) = {
          nyir_memory_ssa_forward, nyir_points_to_sroa,
          nyir_store_sink, nyir_aggregate_sroa, nyir_escape_sroa,
          nyir_tbuf_private_object, nyir_sroa_scalar,
          nyir_dead_store_elim, nyir_dce};
      nyir_run_seq(f, memory, 9, stats, dump, ok);
    }
    if (*ok && has_cf) {
      static bool (*const control[])(nyir_func_t *) = {
          nyir_polyhedral_nest, nyir_block_layout};
      nyir_run_seq(f, control, 2, stats, dump, ok);
    }
    if (*ok) {
      static bool (*const tail[])(nyir_func_t *) = {
          nyir_kernel_hint, nyir_icp_profile, nyir_cfg_simplify,
          nyir_dce, nyir_const_fold, nyir_dce, nyir_compact};
      nyir_run_seq(f, tail, 7, stats, dump, ok);
    }
  }
  if (*ok && nyir_stop_after_pass &&
      strcmp(nyir_stop_after_pass, "scalar_cleanup") == 0)
    nyir_pass_stopped = true;
  return *ok;
}

static bool nyir_optimize_pipeline(nyir_func_t *f, nyir_opt_stats_t *stats,
                                     int opt_level, FILE *dump) {
  /*
   * Read fast-math mode from env config each pipeline invocation.
   */
  ny_native_set_fast_math(ny_env_enabled("NYTRIX_FAST_MATH"));
  nyir_pass_stopped = false;
  if (!nyir_pass_control_valid(nyir_disabled_pass) ||
      !nyir_pass_control_valid(nyir_stop_after_pass)) {
    const char *bad = !nyir_pass_control_valid(nyir_disabled_pass)
                          ? nyir_disabled_pass
                          : nyir_stop_after_pass;
    fprintf(stderr, "native NYIR: unknown optimizer pass '%s'\n", bad);
    return false;
  }
  if (stats) {
    memset(stats, 0, sizeof(*stats));
    nyir_collect_stats(f, &stats->before_insts, &stats->before_values,
                         stats->before_ops, NYIR_OP_COUNT);
  }
  if (dump && f && f->len > 0)
    nyir_dump(dump, f, "<before>");

  bool ok = true;

  if (opt_level <= 0) {
    static bool (*const p0[])(nyir_func_t *) = {nyir_compact};
    nyir_run_seq(f, p0, 1, stats, dump, &ok);
  } else if (opt_level == 1) {
    static bool (*const p1[])(nyir_func_t *) = {
        nyir_const_fold, nyir_peephole, nyir_copy_prop, nyir_const_fold,
        nyir_cfg_simplify, nyir_if_convert, nyir_dce, nyir_cfg_simplify,
        nyir_compact};
    nyir_run_seq(f, p1, 9, stats, dump, &ok);
    if (ok)
      nyir_run_pass(f, nyir_inline_small, stats, dump, &ok);
    if (ok)
      nyir_run_finish_scalar(f, stats, dump, &ok);
  } else if (opt_level == 2) {
    static bool (*const p2[])(nyir_func_t *) = {
        nyir_const_fold, nyir_peephole, nyir_copy_prop, nyir_cse,
        nyir_cfg_simplify, nyir_if_convert, nyir_dce, nyir_dead_store_elim,
        nyir_cfg_simplify, nyir_compact_if_sparse};
    nyir_run_seq(f, p2, 10, stats, dump, &ok);
    if (ok && !nyir_pass_stopped &&
        (!nyir_has_control_flow(f) || nyir_cf_mem2reg_enabled)) {
      static bool (*const prom[])(nyir_func_t *) = {
          nyir_mem2reg, nyir_sccp, nyir_dce,
          nyir_dead_store_elim, nyir_compact_if_sparse};
      nyir_run_seq(f, prom, 5, stats, dump, &ok);
    }
    if (ok && !nyir_pass_stopped)
      nyir_run_pass(f, nyir_inline_general, stats, dump, &ok);
    if (ok && !nyir_pass_stopped)
      nyir_run_pass(f, nyir_inline_small, stats, dump, &ok);
    if (ok && !nyir_pass_stopped) {
      static bool (*const post_inline1[])(nyir_func_t *) = {
          nyir_const_fold, nyir_sccp, nyir_copy_prop};
      static bool (*const post_inline2[])(nyir_func_t *) = {
          nyir_cfg_simplify, nyir_if_convert, nyir_dce};
      /*
       * Inlining exposes constants and dead branch structure recursively.
       */
      uint64_t fp1 = nyir_debug_fingerprint(f);
      nyir_run_seq(f, post_inline1, 3, stats, dump, &ok);
      if (ok && nyir_debug_fingerprint(f) != fp1) {
        nyir_run_seq(f, post_inline1, 3, stats, dump, &ok);
      }
      if (ok)
        nyir_run_seq(f, post_inline2, 3, stats, dump, &ok);
    }
    if (ok && !nyir_pass_stopped) {
      static bool (*const loop_opts[])(nyir_func_t *) = {
          nyir_loop_rotate, nyir_scev_lite, nyir_irce,
          nyir_loop_idiom, nyir_licm, nyir_cse,
          nyir_scev_lite, nyir_irce, nyir_alias_store_sink};
      /*
       * LICM/CSE can expose a simpler loop guard and can replace repeated
       * managed-buffer length helpers with one dominating SSA value.  Refresh
       * SCEV and IRCE once after those motions so O2 actually consumes the
       * newly exposed affine/bounds facts before vectorization.
       */
      nyir_run_seq(f, loop_opts, 9, stats, dump, &ok);
    }
    /*
     * O2 is the default benchmark tier, so it must not skip the existing
     * vector pipeline entirely.  Keep the canonical induction/address form
     * through vectorization, then let IV elimination and SLP clean up the
     * remaining scalar regions/tails.
     */
    if (ok && !nyir_pass_stopped)
      nyir_run_pass(f, nyir_loop_vectorize, stats, dump, &ok);
    if (ok && !nyir_pass_stopped)
      nyir_run_pass(f, nyir_iv_elim, stats, dump, &ok);
    if (ok && !nyir_pass_stopped)
      nyir_run_pass(f, nyir_loop_peel, stats, dump, &ok);
    if (ok && !nyir_pass_stopped)
      nyir_run_pass(f, nyir_unroll_jam, stats, dump, &ok);
    if (ok && !nyir_pass_stopped)
      nyir_run_pass(f, nyir_softpipe_rewrite, stats, dump, &ok);
    if (ok && !nyir_pass_stopped)
      nyir_run_pass(f, nyir_loop_unroll, stats, dump, &ok);
    if (ok && !nyir_pass_stopped) {
      static bool (*const pre_slp[])(nyir_func_t *) = {
          nyir_redundant_load_elim, nyir_memory_ssa_forward,
          nyir_tbuf_private_object, nyir_sroa_scalar,
          nyir_dead_store_elim, nyir_dce};
      /*
       * SLP sees better trees after private/local memory has been forwarded
       * into SSA.  Keep this immediately before SLP so later scalar cleanup
       * does not become the first point where load/store chains disappear.
       */
      nyir_run_seq(f, pre_slp, 6, stats, dump, &ok);
      if (ok && !nyir_pass_stopped)
        nyir_run_pass(f, nyir_memcombine, stats, dump, &ok);
    }
    if (ok && !nyir_pass_stopped)
      nyir_run_pass(f, nyir_slp_vectorize, stats, dump, &ok);
    if (ok && !nyir_pass_stopped)
      nyir_run_finish_scalar(f, stats, dump, &ok);
  } else {
    /*
     * O3: build SSA once, keep PHIs through scalar/loop optimization, and
     * destroy them only at machine lowering.
     */
    static bool (*const head[])(nyir_func_t *) = {
        nyir_const_fold, nyir_cfg_simplify, nyir_dce, nyir_cfg_simplify,
        nyir_if_convert};
    nyir_run_seq(f, head, 5, stats, dump, &ok);

    /*
     * Run loop vectorization BEFORE mem2reg (in early passes) so that
     * memory accesses are still present for vectorization.
     */
    if (ok && !nyir_pass_stopped)
      nyir_run_pass(f, nyir_loop_vectorize, stats, dump, &ok);

    bool has_cf = nyir_has_control_flow(f);
    if (ok && nyir_has_local_mem(f) && !nyir_pass_stopped &&
        (!has_cf || nyir_cf_mem2reg_enabled)) {
      static bool (*const early[])(nyir_func_t *) = {
          nyir_mem2reg, nyir_sccp, nyir_cfg_simplify,
          nyir_dce, nyir_alias_analysis, nyir_compact_if_sparse,
          nyir_redundant_load_elim, nyir_bounds_check_elim};
      nyir_run_seq(f, early, 8, stats, dump, &ok);
    }
    if (ok && !nyir_pass_stopped)
      nyir_run_pass(f, nyir_inline_general, stats, dump, &ok);
    if (ok && !nyir_pass_stopped)
      nyir_run_pass(f, nyir_inline_small, stats, dump, &ok);
    if (ok && f->len > 2) {
      static bool (*const mid[])(nyir_func_t *) = {
          nyir_const_fold, nyir_sccp, nyir_copy_prop, nyir_cse,
          nyir_peephole, nyir_egraph_rewrite, nyir_cfg_simplify, nyir_if_convert, nyir_dce,
          nyir_null_check_elim, nyir_overflow_check_elim,
          nyir_vrp, nyir_cvp};
      nyir_run_seq(f, mid, 13, stats, dump, &ok);
    }
    if (ok && !nyir_pass_stopped) {
      static bool (*const loop_analysis[])(nyir_func_t *) = {
          nyir_loop_rotate, nyir_scev_lite, nyir_null_align_facts,
          nyir_bounds_check_elim, nyir_irce, nyir_loop_idiom, nyir_loop_interchange,
          nyir_loop_versioning, nyir_loop_predication, nyir_affine_nest};
      nyir_run_seq(f, loop_analysis, 10, stats, dump, &ok);
      /*
       * Polyhedral tiling: annotate hot loops with tile-size hints.
       */
      if (ok && !nyir_pass_stopped)
        nyir_run_pass(f, nyir_polyhedral_tiling, stats, dump, &ok);
      /*
       * Drain NOPs licm left so the array doesn't bloat before further loop
       * passes; sparse-gated so it's a no-op when nothing was removed.
       */
      if (ok && !nyir_pass_stopped)
        nyir_run_pass(f, nyir_compact_if_sparse, stats, dump, &ok);
      if (ok && !nyir_pass_stopped)
        nyir_run_pass(f, nyir_loop_unswitch, stats, dump, &ok);
      /*
       * Prefetch insertion after loop structure is finalized.
       */
      if (ok && !nyir_pass_stopped)
        nyir_run_pass(f, nyir_prefetch_insert, stats, dump, &ok);
      /*
       * Vectorization needs the canonical source IV and affine address
       * expressions.  Run it before IV elimination introduces secondary
       * recurrence PHIs; scalar IV elimination can then clean up the
       * remaining tail loops.
       */
      if (ok && !nyir_pass_stopped)
        nyir_run_pass(f, nyir_loop_vectorize, stats, dump, &ok);
      if (ok && !nyir_pass_stopped)
        nyir_run_pass(f, nyir_iv_elim, stats, dump, &ok);
      if (ok && !nyir_pass_stopped)
        nyir_run_pass(f, nyir_cse, stats, dump, &ok);
      if (ok && !nyir_pass_stopped)
        nyir_run_pass(f, nyir_loop_peel, stats, dump, &ok);
      if (ok && !nyir_pass_stopped)
        nyir_run_pass(f, nyir_unroll_jam, stats, dump, &ok);
      if (ok && !nyir_pass_stopped)
        nyir_run_pass(f, nyir_softpipe_rewrite, stats, dump, &ok);
      if (ok && !nyir_pass_stopped)
        nyir_run_pass(f, nyir_loop_unroll, stats, dump, &ok);
      if (ok && !nyir_pass_stopped) {
        static bool (*const pre_slp[])(nyir_func_t *) = {
            nyir_redundant_load_elim, nyir_memory_ssa_forward,
            nyir_tbuf_private_object, nyir_sroa_scalar,
            nyir_dead_store_elim, nyir_dce, nyir_alias_store_sink};
        nyir_run_seq(f, pre_slp, 7, stats, dump, &ok);
        if (ok && !nyir_pass_stopped)
          nyir_run_pass(f, nyir_memcombine, stats, dump, &ok);
      }
      if (ok && !nyir_pass_stopped)
        nyir_run_pass(f, nyir_slp_vectorize, stats, dump, &ok);
      if (ok && !nyir_pass_stopped)
        nyir_run_pass(f, nyir_compact_if_sparse, stats, dump, &ok);
    }
    if (ok && nyir_has_control_flow(f))
      nyir_run_pass(f, nyir_jump_thread, stats, dump, &ok);
    if (ok)
      nyir_run_pass(f, nyir_icp_profile, stats, dump, &ok);
    if (ok)
      nyir_run_pass(f, nyir_block_layout, stats, dump, &ok);
    if (ok) {
      static bool (*const cfg[])(nyir_func_t *) = {nyir_cfg_simplify,
                                                     nyir_phiopt, nyir_if_convert,
                                                     nyir_dce};
      nyir_run_seq(f, cfg, 4, stats, dump, &ok);
    }
    if (ok && !nyir_pass_stopped)
      nyir_run_pass(f, nyir_ipa_constprop, stats, dump, &ok);
    if (ok && !nyir_pass_stopped)
      nyir_run_pass(f, nyir_adce, stats, dump, &ok);
    if (ok && !nyir_pass_stopped)
      nyir_run_pass(f, nyir_dead_arg_elim, stats, dump, &ok);
    if (ok && !nyir_pass_stopped)
      nyir_run_finish_scalar(f, stats, dump, &ok);
  }

  /*
   * Machine-form x86-64/AArch64 lowering resolves SSA PHIs with
   * predecessor-edge parallel copies. Other direct-NYIR encoders still need
   * the local-memory representation.
   */
  if (ok && !nyir_pass_stopped && !nyir_preserve_phis)
    nyir_run_pass(f, nyir_phi_elim, stats, dump, &ok);

  /*
   * Final gate: verify the optimized function unconditionally. With
   * verify_each_pass defaulting off, the production path otherwise ships
   * whatever the last pass left behind with no semantic net at all.
   */
  if (ok) {
    char err[256] = {0};
    if (!nyir_verify(f, err, sizeof(err))) {
      fprintf(stderr,
              "native NYIR: verification failed after optimization pipeline: "
              "%s\n",
              err[0] ? err : "unknown error");
      ok = false;
    }
  }

  if (stats)
    nyir_collect_stats(f, &stats->after_insts, &stats->after_values,
                         stats->after_ops, NYIR_OP_COUNT);
  if (ok)
    nyir_refresh_metadata(f);

  if (dump && stats && stats->total_time_ms > 0.001) {
    fprintf(dump, "nyir timing:");
    for (size_t i = 0; i < stats->pass_count; ++i) {
      const nyir_pass_stat_t *r = &stats->passes[i];
      char tag[64];
      nyir_pass_tag(tag, sizeof(tag), r->name ? r->name : "unknown");
      size_t len = strlen(tag);
      if (len >= 2 && tag[0] == '<' && tag[len - 1] == '>')
        fprintf(dump, " %.*s=%.2fms", (int)(len - 2), tag + 1, r->time_ms);
      else
        fprintf(dump, " %s=%.2fms", tag, r->time_ms);
      if (r->before_insts != r->after_insts ||
          r->before_values != r->after_values ||
          r->before_blocks != r->after_blocks)
        fprintf(dump, "[i%zu→%zu v%d→%d b%zu→%zu]",
                r->before_insts, r->after_insts,
                r->before_values, r->after_values,
                r->before_blocks, r->after_blocks);
    }
    fprintf(dump, " total=%.2fms\n", stats->total_time_ms);
  } else if (verbose_enabled >= 1 && stats && stats->total_time_ms > 0.001) {
    bool grew = stats->after_insts > stats->before_insts;
    size_t delta = grew ? stats->after_insts - stats->before_insts
                        : stats->before_insts - stats->after_insts;
    double pct = stats->before_insts > 0
                     ? (grew ? 1.0 : -1.0) * 100.0 * (double)delta /
                           stats->before_insts
                     : 0.0;
    fprintf(stderr,
            "nyir opt(O%d): %zu->%zu insts (%c%zu, %+.1f%%) in %.2fms\n",
            opt_level, stats->before_insts, stats->after_insts,
            grew ? '+' : '-', delta, pct, stats->total_time_ms);
  }
  return ok;
}

bool nyir_optimize_debug(nyir_func_t *f, FILE *dump,
                           nyir_opt_stats_t *stats, int opt_level) {
  if (!dump)
    dump = stderr;
  return nyir_optimize_pipeline(f, stats, opt_level, dump);
}
