/*
 * NYIR optimization pipeline: sequences passes in order, manages
 * per-pass verification, statistics, and early-stop diagnostics.
 *
 * O1 passes: const_fold, sccp, copy_prop, cse, peephole, cfg_simplify,
 *   dce, licm, strength_reduce, alias_store_sink, null_align_facts,
 *   bounds_check_elim, redundant_load_elim.
 *
 * O2 adds: loop_rotate, scev_lite, irce, loop_idiom, loop_last_overwrite,
 *   loop_interchange,
 *   loop_versioning, loop_predication, loop_unswitch, loop_vectorize,
 *   iv_elim, loop_unroll, inline_general, inline_small.
 *
 * O3 adds: slp_vectorize, unroll_jam, softpipe_rewrite,
 *   tbuf_private_object, sroa_scalar, dead_store_elim,
 *   adce, icp_profile, block_layout.
 *
 * NOP bloat: licm and alias_store_sink (the loop-phase NOP producers)
 * are followed by nyr_compact_if_sparse, which drains NYIR_NOPs
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
#include "base/common.h"
#include "base/compat.h"
#include "base/debug.h"
#include "base/util.h"
#include "code/backend/ir/internal.h"
#include "code/backend/ir/opt/loop.h"
#include "code/backend/ir/opt/util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *nyr_debug_pass_name(bool (*pass)(nyr_func_t *));
static void nyr_heu_noop_record(const char *pass_name, double ms);
static void nyr_heu_noop_flush(void);
static const char *nyr_disabled_pass = NULL;
static const char *nyr_stop_after_pass = NULL;
static bool nyr_pass_stopped = false;
static bool nyr_verify_each_pass = false;
static bool nyr_rollback_on_verify_fail = false;
static int nyr_pass_burst_count = 0;
static const char *nyr_pass_prev = "";
static const char *nyr_pass_prev2 = "";
static const char *nyr_pass_burst_first = NULL;

static void nyr_pass_burst_flush(void) {
  if (nyr_pass_burst_count >= 3)
    NY_DBG(NY_DBG_PASS,
           "pass burst compact↔phi_elim first=%s last=%s suppressed=%d",
           nyr_pass_burst_first ? nyr_pass_burst_first : "?", nyr_pass_prev,
           nyr_pass_burst_count - 2);
  nyr_pass_burst_count = 0;
  nyr_pass_burst_first = NULL;
  nyr_pass_prev = "";
  nyr_pass_prev2 = "";
}

/*
 * Value tracking across NYIR optimization passes.
 */
static int nyr_track_value = -1;
static bool nyr_track_enabled = false;
static char nyr_track_first_def[128];

void nyr_set_track_value(int value) {
  nyr_track_value = value;
  nyr_track_enabled = (value >= 0);
  nyr_track_first_def[0] = '\0';
}

bool nyr_get_track_value(int *out) {
  if (nyr_track_enabled) {
    *out = nyr_track_value;
    return true;
  }
  return false;
}

/*
 * Global compile budget: abort when total IR instructions exceed this cap.
 */
static size_t nyr_global_inst_cap = 0;
/*
 * Per-pass growth cap: abort a pass if it increases instruction count by more
 * than this factor.
 */
static double nyr_pass_growth_factor = 0.0;

/*
 * Set when any pass grew a function's IR by more than 10% during this
 * pipeline; gates the end-of-pipeline execution regression profile.
 */
#if !defined(NYR_NO_EVAL)
static bool nyr_pipeline_grew;
#endif

static nyr_scalar_class_t
nyr_pipeline_value_class(const nyr_func_t *f, const nyr_type_map_t *types,
                          const nyr_inst_t *const *defs, int value,
                          unsigned depth) {
  if (!f || value < 0 || value >= f->next_value || depth > 32)
    return NYIR_SCALAR_UNKNOWN;
  const nyr_inst_t *def = defs ? defs[value] : NULL;
  if (!def)
    return NYIR_SCALAR_UNKNOWN;
  if (def->scalar_class != NYIR_SCALAR_UNKNOWN)
    return def->scalar_class;
  if (def->op == NYIR_COPY)
    return nyr_pipeline_value_class(f, types, defs, def->a, depth + 1);
  if (def->op == NYIR_PHI && def->phi_incoming_len) {
    nyr_scalar_class_t joined = NYIR_SCALAR_UNKNOWN;
    for (size_t i = 0; i < def->phi_incoming_len; ++i) {
      nyr_scalar_class_t incoming = nyr_pipeline_value_class(
          f, types, defs, def->phi_incoming[i].value, depth + 1);
      if (incoming == NYIR_SCALAR_UNKNOWN ||
          (joined != NYIR_SCALAR_UNKNOWN && joined != incoming))
        return NYIR_SCALAR_UNKNOWN;
      joined = incoming;
    }
    return joined;
  }
  if (types && (size_t)value < types->value_count) {
    bool f32 = types->value_f32[value];
    bool f64 = types->value_f64[value];
    if (f32 != f64)
      return f64 ? NYIR_SCALAR_F64 : NYIR_SCALAR_F32;
  }
  return NYIR_SCALAR_UNKNOWN;
}

/*
 * Optimizer rewrites may synthesize local stores with a missing or stale
 * scalar class. Reconcile it with the producer's NYIR class before the
 * verifier treats it as an ABI contract. This is metadata only: it does not
 * insert a conversion or change the stored bits.
 */
static void nyr_stamp_local_store_classes(nyr_func_t *f) {
  if (!f)
    return;
  /*
   * This reconciliation only reads or writes LOAD_LOCAL and STORE_LOCAL.
   * Many optimized functions have no local slots at all (or only address
   * local storage), so avoid constructing a full value type map when there
   * is nothing this pass can repair.
   */
  bool has_local_value_access = false;
  int64_t max_local = -1;
  for (size_t i = 0; i < f->len; ++i) {
    nyr_op_t op = f->data[i].op;
    if ((op == NYIR_LOAD_LOCAL || op == NYIR_STORE_LOCAL ||
         op == NYIR_ADDR_LOCAL) &&
        f->data[i].imm > max_local)
      max_local = f->data[i].imm;
    if (op == NYIR_LOAD_LOCAL || op == NYIR_STORE_LOCAL)
      has_local_value_access = true;
  }
  if (!has_local_value_access)
    return;
  size_t local_count = max_local >= 0 ? (size_t)max_local + 1u : 0;
  nyr_type_map_t types = {0};
  if (!nyr_type_map_init(&types, f, local_count))
    return;
  size_t value_count = (size_t)f->next_value;
  const nyr_inst_t **defs =
      value_count ? ny_calloc_array(value_count, sizeof(*defs)) : NULL;
  if (value_count && !defs) {
    nyr_type_map_free(&types);
    return;
  }
  for (size_t i = 0; i < f->len; ++i) {
    int dst = f->data[i].dst;
    if (dst >= 0 && (size_t)dst < value_count)
      defs[dst] = &f->data[i];
  }
  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *in = &f->data[i];
    if (in->op != NYIR_STORE_LOCAL || in->a < 0 ||
        (size_t)in->a >= types.value_count)
      continue;
    nyr_scalar_class_t actual =
        nyr_pipeline_value_class(f, &types, defs, in->a, 0);
    /*
     * A concrete producer class is stronger than the type map's unioned
     * inference. In particular, a raw integer can share a coarse type root
     * with an f64 through optimizer-created copies; never let that inference
     * silently rewrite an explicit raw store into a floating ABI boundary.
     */
    if (actual != NYIR_SCALAR_UNKNOWN) {
      if (in->scalar_class != actual)
        in->scalar_class = actual;
    } else if (in->scalar_class == NYIR_SCALAR_UNKNOWN &&
               types.value_f64[in->a] && !types.value_f32[in->a]) {
      in->scalar_class = NYIR_SCALAR_F64;
    } else if (in->scalar_class == NYIR_SCALAR_UNKNOWN &&
               types.value_f32[in->a] && !types.value_f64[in->a]) {
      in->scalar_class = NYIR_SCALAR_F32;
    }
  }

  /*
   * PHI elimination and local promotion can leave a LOAD_LOCAL carrying the
   * class from the value that originally occupied its SSA id.  Local slots
   * are reused, so only repair a load when every explicit store to that slot
   * agrees on one concrete representation.  Mixed or partially unknown
   * slots remain untouched: guessing there could hide a real dynamic ABI
   * boundary.
   */
  nyr_scalar_class_t *slot_class =
      calloc(local_count ? local_count : 1, sizeof(*slot_class));
  bool *slot_seen = calloc(local_count ? local_count : 1, sizeof(*slot_seen));
  bool *slot_mixed = calloc(local_count ? local_count : 1, sizeof(*slot_mixed));
  if (slot_class && slot_seen && slot_mixed) {
    for (size_t i = 0; i < f->len; ++i) {
      nyr_inst_t *in = &f->data[i];
      if (in->op != NYIR_STORE_LOCAL || in->imm < 0 ||
          (uint64_t)in->imm >= local_count)
        continue;
      nyr_scalar_class_t cls = in->scalar_class;
      if (cls == NYIR_SCALAR_UNKNOWN && in->a >= 0 &&
          (size_t)in->a < types.value_count) {
        if (types.value_f64[in->a] && !types.value_f32[in->a])
          cls = NYIR_SCALAR_F64;
        else if (types.value_f32[in->a] && !types.value_f64[in->a])
          cls = NYIR_SCALAR_F32;
      }
      size_t slot = (size_t)in->imm;
      if (!slot_seen[slot]) {
        slot_seen[slot] = true;
        slot_class[slot] = cls;
      } else if (cls == NYIR_SCALAR_UNKNOWN || slot_class[slot] != cls) {
        slot_mixed[slot] = true;
      }
    }
    for (size_t i = 0; i < f->len; ++i) {
      nyr_inst_t *in = &f->data[i];
      if (in->op != NYIR_LOAD_LOCAL || in->imm < 0 ||
          (uint64_t)in->imm >= local_count)
        continue;
      size_t slot = (size_t)in->imm;
      if (slot_seen[slot] && !slot_mixed[slot] &&
          slot_class[slot] != NYIR_SCALAR_UNKNOWN &&
          in->scalar_class != slot_class[slot]) {
        in->scalar_class = slot_class[slot];
      }
    }
  }
  free(slot_class);
  free(slot_seen);
  free(slot_mixed);

  /*
   * For a load with a nearest reaching store, the store is stronger evidence
   * than an old class copied from a former occupant of the slot. Address-
   * taken slots remain conservative because intervening calls can mutate
   * them through an alias.
   */
  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *load = &f->data[i];
    if (load->op != NYIR_LOAD_LOCAL || load->imm < 0)
      continue;
    for (size_t j = i; j > 0; --j) {
      nyr_inst_t *prev = &f->data[j - 1];
      if (prev->op == NYIR_STORE_LOCAL && prev->imm == load->imm) {
        nyr_scalar_class_t cls = prev->scalar_class;
        if (cls == NYIR_SCALAR_UNKNOWN && prev->a >= 0 &&
            (size_t)prev->a < types.value_count) {
          if (types.value_f64[prev->a] && !types.value_f32[prev->a])
            cls = NYIR_SCALAR_F64;
          else if (types.value_f32[prev->a] && !types.value_f64[prev->a])
            cls = NYIR_SCALAR_F32;
        }
        if (cls != NYIR_SCALAR_UNKNOWN)
          load->scalar_class = cls;
        break;
      }
      if (prev->op == NYIR_LABEL || prev->op == NYIR_BR ||
          prev->op == NYIR_BR_IF || prev->op == NYIR_CALL ||
          (prev->op == NYIR_ADDR_LOCAL && prev->imm == load->imm))
        break;
    }
  }
  free(defs);
  nyr_type_map_free(&types);
}

void nyr_reconcile_local_scalar_classes(nyr_func_t *f) {
  nyr_stamp_local_store_classes(f);
}

bool nyr_loop_unswitch(nyr_func_t *f);
bool nyr_iv_elim(nyr_func_t *f);
bool nyr_gvn_pre(nyr_func_t *f);
bool nyr_loop_vectorize(nyr_func_t *f);
bool nyr_slp_vectorize(nyr_func_t *f);
bool nyr_memcombine(nyr_func_t *f);
bool nyr_alias_store_sink(nyr_func_t *f);
bool nyr_narrow(nyr_func_t *f);
bool nyr_phi_elim(nyr_func_t *f);

void nyr_set_pass_controls(const char *disable_pass, const char *stop_after) {
  nyr_disabled_pass = disable_pass && disable_pass[0] ? disable_pass : NULL;
  nyr_stop_after_pass = stop_after && stop_after[0] ? stop_after : NULL;
}

void nyr_set_verify_each_pass(bool enable) { nyr_verify_each_pass = enable; }

void nyr_set_rollback_on_verify_fail(bool enable) {
  nyr_rollback_on_verify_fail = enable;
}

void nyr_set_global_inst_cap(size_t cap) { nyr_global_inst_cap = cap; }

void nyr_set_pass_growth_factor(double factor) {
  nyr_pass_growth_factor = factor > 1.0 ? factor : 0.0;
}

static bool nyr_per_pass_oracle = false;
static nyr_per_pass_oracle_fn nyr_oracle_fn = NULL;
static void *nyr_oracle_userdata = NULL;
static int nyr_tv_trials = 0;

void nyr_set_per_pass_oracle(bool enable, nyr_per_pass_oracle_fn fn,
                              void *userdata) {
  nyr_per_pass_oracle = enable;
  nyr_oracle_fn = enable ? fn : NULL;
  nyr_oracle_userdata = enable ? userdata : NULL;
}

void nyr_set_tv_seed(int trials) { nyr_tv_trials = trials > 0 ? trials : 0; }

static bool nyr_oracle_checkpoint(const nyr_func_t *f,
                                   const char *pass_name) {
  if (!nyr_per_pass_oracle || !nyr_oracle_fn)
    return true;
  bool ok = nyr_oracle_fn(f, pass_name, nyr_oracle_userdata);
  if (!ok) {
    fprintf(stderr, "native NYIR: per-pass oracle failed after %s\n",
            pass_name ? pass_name : "pass");
  }
  return ok;
}

static bool nyr_tv_checkpoint(const nyr_func_t *before,
                               const nyr_func_t *after,
                               const char *pass_name) {
  if (nyr_tv_trials <= 0 || !before || !after)
    return true;
  char err[256] = {0};
  /*
   * Prefer SMT (Z3) for pure straight-line; multi-input covers CFG.
   */
  bool ok = nyr_tv_smt_equiv(before, after, err, sizeof(err));
  if (!ok && err[0] == '\0')
    ok = nyr_tv_equiv_straightline(before, after, nyr_tv_trials, err,
                                    sizeof(err));
  if (ok)
    return true;
  fprintf(stderr, "native NYIR: translation validation failed after %s: %s\n",
          pass_name ? pass_name : "pass", err[0] ? err : "mismatch");
  return false;
}

static bool nyr_verify_checkpoint(nyr_func_t *f, const char *pass_name) {
  if (!nyr_verify_each_pass)
    return true;
  char err[1024] = {0};
  if (nyr_ir_verify(f, err, sizeof(err)))
    return true;
  fprintf(stderr, "native NYIR: verification failed after %s: %s\n",
          pass_name ? pass_name : "pass", err[0] ? err : "unknown error");
  return false;
}

static bool nyr_pass_is_skipped(bool (*pass)(nyr_func_t *)) {
  if (nyr_pass_stopped)
    return true;
  const char *name = nyr_debug_pass_name(pass);
  return nyr_disabled_pass && strcmp(nyr_disabled_pass, name) == 0;
}

static void nyr_finish_pass(bool (*pass)(nyr_func_t *)) {
  const char *name = nyr_debug_pass_name(pass);
  if (nyr_stop_after_pass && strcmp(nyr_stop_after_pass, name) == 0)
    nyr_pass_stopped = true;
}

static bool nyr_debug_const_fold_candidate(nyr_op_t op) {
  switch (op) {
  case NYIR_ADD_I64:
  case NYIR_SUB_I64:
  case NYIR_MUL_I64:
  case NYIR_DIV_I64:
  case NYIR_MOD_I64:
  case NYIR_AND_I64:
  case NYIR_OR_I64:
  case NYIR_XOR_I64:
  case NYIR_SHL_I64:
  case NYIR_SAR_I64:
  case NYIR_ROR_I64:
  case NYIR_CMP_I64:
  case NYIR_ADD_F64:
  case NYIR_SUB_F64:
  case NYIR_MUL_F64:
  case NYIR_DIV_F64:
  case NYIR_CMP_F64:
  case NYIR_ADD_F32:
  case NYIR_SUB_F32:
  case NYIR_MUL_F32:
  case NYIR_DIV_F32:
  case NYIR_CMP_F32:
    return true;
  default:
    return false;
  }
}

static void nyr_debug_pattern_candidates(const nyr_func_t *f,
                                          const char *pass_name) {
  if (!f || !pass_name || !ny_dbg_is_enabled(NY_DBG_OPT_MISS))
    return;
  if (strcmp(pass_name, "const_fold") == 0 && f->next_value > 0) {
    nyr_value_fact_t *facts = calloc((size_t)f->next_value, sizeof(*facts));
    char err[128] = {0};
    if (facts && nyr_analyze_values(f, facts, (size_t)f->next_value, err,
                                     sizeof(err))) {
      unsigned shown = 0;
      for (size_t i = 0; i < f->len && shown < 8; ++i) {
        const nyr_inst_t *in = &f->data[i];
        if (!nyr_debug_const_fold_candidate(in->op) || in->dst < 0 ||
            in->a < 0 || in->b < 0 || in->dst >= f->next_value ||
            in->a >= f->next_value || in->b >= f->next_value ||
            !facts[in->a].known_const || !facts[in->b].known_const)
          continue;
        if ((in->op == NYIR_DIV_I64 || in->op == NYIR_MOD_I64) &&
            facts[in->b].const_value == 0)
          continue;
        ny_dbg_opt_missed(pass_name, "constant expression remains",
                          "both operands are known constants after the pass; "
                          "inspect overflow, scalar-class, and legality gates");
        ++shown;
      }
    }
    free(facts);
  } else if (strcmp(pass_name, "dead_store_elim") == 0) {
    for (size_t i = 0; i < f->len; ++i) {
      const nyr_inst_t *store = &f->data[i];
      if (store->op != NYIR_STORE_LOCAL)
        continue;
      for (size_t j = i + 1; j < f->len; ++j) {
        const nyr_inst_t *later = &f->data[j];
        if (later->op == NYIR_LOAD_LOCAL && later->imm == store->imm)
          break;
        if (later->op == NYIR_STORE_LOCAL && later->imm == store->imm) {
          ny_dbg_opt_missed(pass_name, "possible dead local store",
                            "same-slot store is overwritten before a visible "
                            "load in linear instruction order");
          return;
        }
        if (later->op == NYIR_BR || later->op == NYIR_BR_IF ||
            later->op == NYIR_RET || later->op == NYIR_CALL)
          break;
      }
    }
  } else if (strcmp(pass_name, "sroa_scalar") == 0) {
    for (size_t i = 0; i < f->len; ++i)
      if (f->data[i].op == NYIR_ALLOCA) {
        ny_dbg_opt_missed(pass_name, "aggregate allocation remains",
                          "inspect address escapes, aliasing, and field-use "
                          "constraints");
        return;
      }
  }
}

static bool timed_pass(nyr_func_t *f, bool (*pass)(nyr_func_t *),
                       double *out_ms) {
  if (nyr_pass_is_skipped(pass)) {
    if (out_ms)
      *out_ms = 0;
    nyr_finish_pass(pass);
    return true;
  }
  const char *name = nyr_debug_pass_name(pass);

  /*
   * Detect and suppress repetitive compact/phi_elim ping-pong pattern
   */
  bool is_compact = (strcmp(name, "compact") == 0);
  bool is_phi_elim = (strcmp(name, "phi_elim") == 0);

  /*
   * Detect compact <-> phi_elim ping-pong pattern
   */
  bool was_burst = nyr_pass_burst_count >= 3;
  if (is_phi_elim && strcmp(nyr_pass_prev, "compact") == 0) {
    /*
     * phi_elim after compact - potential ping-pong
     */
    if (strcmp(nyr_pass_prev2, "phi_elim") == 0) {
      /*
       * Pattern: phi_elim -> compact -> phi_elim
       */
      nyr_pass_burst_count++;
    } else {
      nyr_pass_burst_count = 1;
      nyr_pass_burst_first = name;
    }
  } else if (is_compact && strcmp(nyr_pass_prev, "phi_elim") == 0) {
    /*
     * compact after phi_elim - potential ping-pong
     */
    if (strcmp(nyr_pass_prev2, "compact") == 0) {
      /*
       * Pattern: compact -> phi_elim -> compact
       */
      nyr_pass_burst_count++;
    } else {
      nyr_pass_burst_count = 1;
      nyr_pass_burst_first = name;
    }
  } else {
    /*
     * Other passes reset the counter
     */
    if (was_burst)
      nyr_pass_burst_flush();
    else {
      nyr_pass_burst_count = 0;
      nyr_pass_burst_first = NULL;
    }
  }

  /*
   * Update history
   */
  nyr_pass_prev2 = nyr_pass_prev;
  nyr_pass_prev = name;

  bool suppress_repeated_log = nyr_pass_burst_count >= 3;
  if (nyr_pass_burst_count == 3) {
    NY_DBG(
        NY_DBG_PASS,
        "suppressing repeated compact/phi_elim diagnostics; passes continue");
  }

  /*
   * Declare all variables at function scope for C89 compatibility
   */
  nyr_func_t before = {0};
  char ir_phase[128];
  bool want_tv = nyr_tv_trials > 0;
  bool want_ir_diff = ny_dbg_is_enabled(NY_DBG_IR_DIFF);
  size_t before_insts = f ? f->len : 0;
  int before_values = f ? f->next_value : 0;
  size_t before_blocks = nyr_block_count(f);
  bool trace_heuristic = ny_dbg_is_enabled(NY_DBG_HEURISTIC);
  bool trace_opt_miss = ny_dbg_is_enabled(NY_DBG_OPT_MISS);
  bool trace_fingerprint = ny_trace_enabled("NY_TRACE_FINGERPRINT");
  bool trace_flaky = ny_trace_enabled("NY_TRACE_FLAKY_PASS");
  uint64_t before_fingerprint =
      (want_tv || nyr_per_pass_oracle || trace_heuristic || trace_opt_miss ||
       trace_fingerprint || trace_flaky)
          ? nyr_debug_fingerprint(f)
          : 0;
  bool ok = false;

  snprintf(ir_phase, sizeof(ir_phase), "%s input", name);
  NY_DBG_IR_FULL(f, ir_phase);
  if ((want_tv || want_ir_diff || trace_flaky) &&
      !nyr_ir_func_clone(f, &before)) {
    if (out_ms)
      *out_ms = 0;
    return false;
  }
  ny_tick_t t0_start = ny_ticks_now();
  ok = pass(f);
  snprintf(ir_phase, sizeof(ir_phase), "%s output", name);
  NY_DBG_IR_FULL(f, ir_phase);
  if (ok)
    nyr_refresh_metadata(f);
  if (ok)
    nyr_stamp_local_store_classes(f);
  if (ok && nyr_global_inst_cap && f->len > nyr_global_inst_cap) {
    NY_DBG(NY_DBG_PASS, "GLOBAL INST CAP EXCEEDED pass='%s' insts=%zu cap=%zu",
           name, f->len, nyr_global_inst_cap);
    ok = false;
  }
  if (ok && nyr_pass_growth_factor > 1.0 && before_insts > 0) {
    double growth = (double)f->len / (double)before_insts;
    if (growth > nyr_pass_growth_factor) {
      NY_DBG(NY_DBG_PASS,
             "PASS GROWTH EXCEEDED pass='%s' growth=%.2fx factor=%.2fx", name,
             growth, nyr_pass_growth_factor);
      ok = false;
    }
  }
  if (ok)
    ok = nyr_verify_checkpoint(f, name);
  /*
   * Flaky double-execution is only informative when the pass actually
   * rewrote IR; re-running every no-op cleanup pass doubles debug-pipeline
   * time to prove the same program twice.
   */
  if (ok && trace_flaky && before.data &&
      nyr_debug_fingerprint(f) != before_fingerprint) {
    nyr_func_t trial_a = {0}, trial_b = {0};
    bool cloned = nyr_ir_func_clone(&before, &trial_a) &&
                  nyr_ir_func_clone(&before, &trial_b);
    bool ran_a = cloned && pass(&trial_a);
    bool ran_b = cloned && pass(&trial_b);
    uint64_t fp_a = ran_a ? nyr_debug_fingerprint(&trial_a) : 0;
    uint64_t fp_b = ran_b ? nyr_debug_fingerprint(&trial_b) : 0;
    if (!cloned || !ran_a || !ran_b || fp_a != fp_b)
      NY_DBG(NY_DBG_PASS,
             "FLAKY PASS pass='%s' deterministic=%s baseline=%016llx "
             "run_a=%016llx run_b=%016llx",
             name, cloned && ran_a && ran_b && fp_a == fp_b ? "yes" : "no",
             (unsigned long long)before_fingerprint, (unsigned long long)fp_a,
             (unsigned long long)fp_b);
    else
      NY_DBG(NY_DBG_PASS,
             "FLAKY CHECK pass='%s' deterministic=yes fingerprint=%016llx",
             name, (unsigned long long)fp_a);
    nyr_ir_func_free(&trial_a);
    nyr_ir_func_free(&trial_b);
  }
  if (!ok) {
    NY_DBG_VERIFY(name, "verification failed");
    if (ny_trace_enabled("NY_TRACE_BISECT"))
      NY_DBG(NY_DBG_PASS,
             "NYIR BISECT first failing optimization boundary pass='%s' "
             "fingerprint_before=%016llx fingerprint_after=%016llx",
             name, (unsigned long long)before_fingerprint,
             (unsigned long long)nyr_debug_fingerprint(f));
    if (want_ir_diff && before.len) {
      NY_DBG_ROLLBACK(name);
    }
  }
  bool oracle_changed = (want_tv || nyr_per_pass_oracle) &&
                        nyr_debug_fingerprint(f) != before_fingerprint;
  /*
   * Translation validation is only needed when the pass changed semantic IR.
   * This fingerprint includes metadata and CFG details; rerunning Z3 on an
   * unchanged function made debug-enabled pipelines spend most of their time
   * proving the same program after no-op cleanup passes.
   */
  if (ok && want_tv && oracle_changed)
    ok = nyr_tv_checkpoint(&before, f, name);
  /*
   * The per-pass native oracle assembles, links, and runs the current IR.
   * An unchanged pass has no new execution behavior to validate.
   */
  if (ok && (!nyr_per_pass_oracle || oracle_changed))
    ok = nyr_oracle_checkpoint(f, name);
  if (!ok && ny_trace_enabled("NY_TRACE_PASS_FAIL"))
    ny_trace_line("PASS_FAIL", "nyir pass '%s' failed (insts=%zu values=%d)",
                  name, f->len, f->next_value);
  if (out_ms)
    *out_ms = ny_ticks_elapsed_ms(t0_start);
  double elapsed_ms = out_ms ? *out_ms : ny_ticks_elapsed_ms(t0_start);
  if (trace_heuristic || trace_opt_miss || trace_fingerprint) {
    uint64_t after_fingerprint = nyr_debug_fingerprint(f);
    bool changed = before_fingerprint != after_fingerprint;
    /*
     * Fingerprint records are only informative when IR moved; emitting one
     * per quiet pass (~80 per function) is the -vvvvvv flood.  Changed
     * passes still report, quiet ones fold into the NYIR_PASS no-op summary.
     */
    if (trace_fingerprint && (changed || !ok))
      NY_DBG(NY_DBG_PASS,
             "IR FINGERPRINT pass='%s' before=%016llx after=%016llx changed=%s",
             name, (unsigned long long)before_fingerprint,
             (unsigned long long)after_fingerprint, changed ? "yes" : "no");
    if (trace_heuristic) {
      if (!ok || changed) {
        nyr_heu_noop_flush();
        char dims[160] = {0};
        size_t dl = 0;
        if (before_insts != (f ? f->len : 0)) {
          int n = snprintf(dims + dl, sizeof(dims) - dl, " i%zu→%zu",
                           before_insts, f ? f->len : 0);
          if (n > 0)
            dl += (size_t)n;
        }
        int after_values = f ? f->next_value : 0;
        if (before_values != after_values) {
          int n = snprintf(dims + dl, sizeof(dims) - dl, " v%d→%d",
                           before_values, after_values);
          if (n > 0)
            dl += (size_t)n;
        }
        if (before_blocks != nyr_block_count(f)) {
          int n = snprintf(dims + dl, sizeof(dims) - dl, " b%zu→%zu",
                           before_blocks, nyr_block_count(f));
          if (n > 0)
            dl += (size_t)n;
        }
        ny_dbg_heuristic_event("NYIR_PASS", "%s: %s%s %.2fms", name,
                               !ok       ? "fail-verify"
                               : changed ? "rewrite"
                                         : "no-rewrite",
                               dims, elapsed_ms);
      } else if (!suppress_repeated_log) {
        /*
         * Quiet pass: fold into the per-function no-op summary instead of
         * one `no-rewrite 0.02ms` line per pass.
         */
        nyr_heu_noop_record(name, elapsed_ms);
      }
    } else if (trace_opt_miss) {
      if (!ok || changed) {
        nyr_heu_noop_flush();
        ny_trace_line("NYIR_PASS",
                      "%s: %s; IR %zu→%zu inst, %d→%d values, %.2f ms%s", name,
                      !ok       ? "failed validation"
                      : changed ? "rewrote IR"
                                : "IR unchanged",
                      before_insts, f ? f->len : 0, before_values,
                      f ? f->next_value : 0, elapsed_ms, "");
      } else {
        nyr_heu_noop_record(name, elapsed_ms);
      }
    }
#if !defined(NYR_NO_EVAL)
    if (trace_fingerprint && before_insts > 0 &&
        f->len * 10 > before_insts * 11) {
      nyr_pipeline_grew = true;
      NY_DBG(NY_DBG_PASS,
             "OPT REGRESSION CANDIDATE pass='%s' instruction_growth=%.1f%% "
             "runtime_speedup=unmeasured",
             name,
             100.0 * ((double)f->len - (double)before_insts) /
                 (double)before_insts);
    }
#endif
    if (ok && !changed && trace_opt_miss)
      nyr_debug_pattern_candidates(f, name);
    if (ok && trace_opt_miss && before_insts >= 10 &&
        f->len > before_insts + before_insts / 10)
      ny_dbg_opt_missed(name, "optimizer growth above 10%",
                        "IR grew substantially; compare runtime/profile data "
                        "before accepting the added code size");
  }
  /*
   * Value tracking output follows the value across pass boundaries.
   */
  if (!suppress_repeated_log && nyr_track_enabled && nyr_track_value >= 0 &&
      nyr_track_value < f->next_value) {
    const nyr_inst_t *def = NULL;
    bool existed_before = false;
    size_t use_count = 0;
    nyr_use_def_t tracked_uses = {0};
    bool have_uses = nyr_build_use_def(f, &tracked_uses) &&
                     (size_t)nyr_track_value < tracked_uses.value_count;
    if (have_uses)
      use_count = tracked_uses.offsets[nyr_track_value + 1] -
                  tracked_uses.offsets[nyr_track_value];
    for (size_t i = 0; i < f->len; ++i) {
      if (f->data[i].dst == nyr_track_value) {
        def = &f->data[i];
        break;
      }
    }
    if (nyr_track_value < before.next_value) {
      for (size_t i = 0; i < before.len; ++i)
        if (before.data[i].dst == nyr_track_value) {
          existed_before = true;
          break;
        }
    }
    if (def) {
      if (!nyr_track_first_def[0])
        snprintf(nyr_track_first_def, sizeof(nyr_track_first_def), "%s",
                 name);
      NY_DBG(NY_DBG_VALUE, "v%d DEF@%s %s@%s uses=%s%zu", nyr_track_value,
             nyr_track_first_def, have_uses && use_count ? "USE" : "LIVE",
             name, have_uses ? "" : "?", have_uses ? use_count : 0);
    } else {
      if (existed_before)
        NY_DBG(NY_DBG_VALUE, "v%d DEF@%s KILL@%s uses=%zu", nyr_track_value,
               nyr_track_first_def[0] ? nyr_track_first_def : "?", name,
               have_uses ? use_count : 0);
      else
        NY_DBG(NY_DBG_VALUE, "v%d absent@%s", nyr_track_value, name);
    }
    nyr_use_def_free(&tracked_uses);
  }
  if (ok) {
    if (!suppress_repeated_log && !trace_heuristic) {
      NY_DBG(NY_DBG_PASS, "pass='%s' insts=%zu->%zu values=%d->%d time=%.2fms",
             name, before_insts, f->len, before_values, f->next_value,
             elapsed_ms);
    }
  } else {
    NY_DBG(NY_DBG_PASS,
           "FAIL pass='%s' insts=%zu->%zu values=%d->%d time=%.2fms", name,
           before_insts, f->len, before_values, f->next_value, elapsed_ms);
  }
  if (ok && !suppress_repeated_log && want_ir_diff && before.len) {
    NY_DBG_IR(NY_DBG_IR_DIFF, f, name, &before, f);
  }
  if (ok)
    nyr_finish_pass(pass);
  nyr_ir_func_free(&before);
  return ok;
}
static bool nyr_cf_mem2reg_enabled = true;

void nyr_set_cf_mem2reg_enabled(bool enable) {
  nyr_cf_mem2reg_enabled = enable;
}

static bool nyr_preserve_phis = false;

void nyr_set_preserve_phis(bool preserve) { nyr_preserve_phis = preserve; }

/*
 * Pass execution tracker for deduplication
 */
typedef struct {
  const char *name;
  int count;
} pass_exec_t;

static pass_exec_t pass_history[64];
static int pass_history_len = 0;

static __attribute__((unused)) void record_pass_execution(const char *name,
                                                          size_t before_insts,
                                                          size_t after_insts) {
  (void)before_insts;
  (void)after_insts;
  if (pass_history_len < 64) {
    pass_history[pass_history_len++] = (pass_exec_t){name, 1};
  }
}

static __attribute__((unused)) bool is_repeating_pattern(const char *name) {
  if (pass_history_len >= 2) {
    const char *prev = pass_history[pass_history_len - 1].name;
    const char *prev2 = pass_history[pass_history_len - 2].name;
    /*
     * Detect compact -> phi_elim -> compact -> phi_elim pattern
     */
    if (strcmp(prev, "compact") == 0 && strcmp(prev2, "phi_elim") == 0 &&
        strcmp(name, "compact") == 0) {
      return true;
    }
    if (strcmp(prev, "phi_elim") == 0 && strcmp(prev2, "compact") == 0 &&
        strcmp(name, "phi_elim") == 0) {
      return true;
    }
  }
  return false;
}

bool nyr_get_preserve_phis(void) { return nyr_preserve_phis; }

typedef struct {
  bool (*pass)(nyr_func_t *);
  const char *name;
} nyr_pass_control_t;

static const nyr_pass_control_t nyr_pass_controls[] = {
    {nyr_const_fold, "const_fold"},
    {nyr_peephole, "peephole"},
    {nyr_copy_prop, "copy_prop"},
    {nyr_cse, "cse"},
    {nyr_strength_reduce, "strength_reduce"},
    {nyr_cfg_simplify, "cfg_simplify"},
    {nyr_phiopt, "phiopt"},
    {nyr_if_convert, "if_convert"},
    {nyr_licm, "licm"},
    {nyr_dce, "dce"},
    {nyr_dead_arg_elim, "dead_arg_elim"},
    {nyr_ipa_constprop, "ipa_constprop"},
    {nyr_dead_store_elim, "dead_store_elim"},
    {nyr_redundant_load_elim, "redundant_load_elim"},
    {nyr_jump_thread, "jump_thread"},
    {nyr_compact, "compact"},
    {nyr_compact_if_sparse, "compact_sparse"},
    {nyr_mem2reg, "mem2reg"},
    {nyr_devirtualize, "devirtualize"},
    {nyr_sccp, "sccp"},
    {nyr_null_align_facts, "null_align_facts"},
    {nyr_iv_simplify, "iv_simplify"},
    {nyr_bounds_check_elim, "bounds_check_elim"},
    {nyr_egraph_local, "egraph_local"},
    {nyr_egraph_rewrite, "egraph_saturate"},
    {nyr_apply_rules, "apply_rules"},
    {nyr_inline_small, "inline_small"},
    {nyr_algebraic_combine, "algebraic_combine"},
    {nyr_double_neg, "double_neg"},
    {nyr_reassoc_add, "reassoc_add"},
    {nyr_memory_ssa_forward, "memory_ssa_forward"},
    {nyr_escape_sroa, "escape_sroa"},
    {nyr_tbuf_scalar_len, "tbuf_scalar_len"},
    {nyr_tbuf_private_object, "tbuf_private_object"},
    {nyr_tbuf_promote_alloca, "tbuf_promote_alloca"},
    {nyr_sroa_scalar, "sroa_scalar"},
    {nyr_block_layout, "block_layout"},
    {nyr_points_to_sroa, "points_to_sroa"},
    {nyr_store_sink, "store_sink"},
    {nyr_aggregate_sroa, "aggregate_sroa"},
    {nyr_polyhedral_nest, "polyhedral_nest"},
    {nyr_affine_nest, "affine_nest"},
    {nyr_kernel_hint, "kernel_hint"},
    {nyr_icp_profile, "icp_profile"},
    {nyr_inline_general, "inline_general"},
    {nyr_loop_unroll, "loop_unroll"},
    {nyr_loop_peel, "loop_peel"},
    {nyr_unroll_jam, "unroll_jam"},
    {nyr_softpipe_rewrite, "softpipe_rewrite"},
    {nyr_scev_lite, "scev_lite"},
    {nyr_irce, "irce"},
    {nyr_loop_idiom, "loop_idiom"},
    {nyr_loop_memset, "loop_memset"},
    {nyr_loop_last_overwrite, "loop_last_overwrite"},
    {nyr_loop_rotate, "loop_rotate"},
    {nyr_loop_interchange, "loop_interchange"},
    {nyr_prefetch_insert, "prefetch_insert"},
    {nyr_polyhedral_tiling, "polyhedral_tiling"},
    {nyr_loop_versioning, "loop_versioning"},
    {nyr_loop_predication, "loop_predication"},
    {nyr_loop_unswitch, "loop_unswitch"},
    {nyr_loop_unroll_ssa, "loop_unroll_ssa"},
    {nyr_iv_elim, "iv_elim"},
    {nyr_gvn_pre, "gvn_pre"},
    {nyr_loop_vectorize, "loop_vectorize"},
    {nyr_slp_vectorize, "slp_vectorize"},
    {nyr_alias_store_sink, "alias_store_sink"},
    {nyr_narrow, "narrow"},
    {nyr_phi_elim, "phi_elim"},
    {nyr_adce, "adce"},
    {nyr_null_check_elim, "null_check_elim"},
    {nyr_overflow_check_elim, "overflow_check_elim"},
    {nyr_vrp, "vrp"},
    {nyr_cvp, "cvp"},
    {nyr_memcombine, "memcombine"},
    {nyr_alias_analysis, "alias_analysis"},
};

static bool nyr_pass_control_valid(const char *name) {
  if (!name || !name[0] || strcmp(name, "scalar_cleanup") == 0)
    return true;
  for (size_t i = 0;
       i < sizeof(nyr_pass_controls) / sizeof(nyr_pass_controls[0]); ++i)
    if (strcmp(name, nyr_pass_controls[i].name) == 0)
      return true;
  return false;
}

static bool nyr_optimize_pipeline(nyr_func_t *f, nyr_opt_stats_t *stats,
                                   int opt_level, FILE *dump);

bool nyr_optimize_with_stats(nyr_func_t *f, nyr_opt_stats_t *stats,
                              int opt_level) {
  return nyr_optimize_pipeline(f, stats, opt_level, NULL);
}

bool nyr_ir_optimize(nyr_func_t *f, int opt_level) {
  /*
   * Execution-regression profiling needs the before/after counts, so the
   * no-stats variant supplies a scratch stats record when profiling is on.
   */
  if (getenv("NYTRIX_FINGERPRINT_PROFILE")) {
    nyr_opt_stats_t profile_stats;
    bool ok = nyr_optimize_with_stats(f, &profile_stats, opt_level);
    return ok;
  }
  return nyr_optimize_with_stats(f, NULL, opt_level);
}

/*
 * Pending no-op records for the -vvvv pass dump.  A pass that neither rewrote
 * IR nor was skipped prints no line of its own; each function's quiet passes
 * fold into one summary line (emitted when a changed pass reports, or at the
 * end of the function's pipeline), replacing ~80 `· <pass> · 0.00ms` lines
 * per function with a single count plus the distinct names.
 *
 * Consecutive passes sharing a `prefix_` (notably the `tbuf_` family) fold
 * further with brace expansion, so
 *   tbuf_private_object, tbuf_promote_alloca
 * renders as
 *   tbuf-{private-object,promote-alloca}
 * instead of two near-identical tokens.
 */
#define NYIR_NOOP_MAX_NAMES 96
#define NYIR_NOOP_MAX_SEQ 256
typedef struct {
  const char *names[NYIR_NOOP_MAX_NAMES];
  unsigned counts[NYIR_NOOP_MAX_NAMES];
  const char *seq[NYIR_NOOP_MAX_SEQ];
  size_t seq_len;
  double ms;
  size_t total;
  size_t distinct;
} nyr_noop_log_t;
static nyr_noop_log_t g_nyir_noop;
/*
 * Set when any pass grew a function's IR by more than 10% during this
 * pipeline; gates the end-of-pipeline execution regression profile.
 */

/*
 * Display form: internal `tbuf_private_object` renders as
 * `tbuf-private-object`.
 */
static void nyr_display_pass(char *out, size_t out_n, const char *pass_name) {
  if (!out || out_n == 0)
    return;
  size_t j = 0;
  for (size_t i = 0; pass_name && pass_name[i] && j + 1 < out_n; ++i) {
    char c = pass_name[i];
    if (c == '_')
      c = '-';
    out[j++] = c;
  }
  out[j] = '\0';
}

/*
 * Length of the shared `prefix_` (including the separator) for brace grouping.
 * Only `_` (and `-` for already-dashed names) count as separators; a bare
 * `cse`/`dce` has no prefix and never groups.  Returns 0 when there is no
 * shared prefix.
 *
 * Also covers the `compact` vs `compact_sparse` case where one name is an
 * exact prefix of the other: the shorter name's length is the grouping
 * prefix and the longer side renders with an empty-vs-suffix alternative
 * (`compact{,-sparse}`).
 */
static size_t nyr_common_prefix_len(const char *a, const char *b) {
  if (!a || !b)
    return 0;
  size_t i = 0;
  size_t last_sep = 0;
  while (a[i] && b[i] && a[i] == b[i]) {
    if (a[i] == '_' || a[i] == '-')
      last_sep = i + 1;
    i++;
  }
  /*
   * Exact-prefix case: `compact` vs `compact_sparse`.
   */
  if ((a[i] == '\0' && (b[i] == '_' || b[i] == '-')) ||
      (b[i] == '\0' && (a[i] == '_' || a[i] == '-')))
    return i;
  /*
   * Require a genuine shared prefix plus a non-empty suffix on both sides:
   * `tbuf_private_object` vs `tbuf_promote_alloca` shares `tbuf_` (5).
   * Single-letter or empty prefixes (`_` alone) do not group.
   */
  if (last_sep < 2)
    return 0;
  if (a[last_sep] == '\0' || b[last_sep] == '\0')
    return 0;
  return last_sep;
}

/*
 * Length of the shared `_suffix` (including the leading separator) for brace
 * grouping on the tail, e.g. `aggregate_sroa` vs `escape_sroa` share `_sroa`
 * (5) and render as `{aggregate,escape}-sroa`.  Likewise `dead_store_elim`
 * vs `null_check_elim` share `_elim`.  Returns 0 when there is no shared
 * suffix.  Requires a non-empty stem on both sides.
 */
static size_t nyr_common_suffix_len(const char *a, const char *b) {
  if (!a || !b)
    return 0;
  size_t alen = strlen(a);
  size_t blen = strlen(b);
  if (alen < 4 || blen < 4)
    return 0;
  size_t i = 0;
  while (i < alen && i < blen && a[alen - 1 - i] == b[blen - 1 - i])
    i++;
  if (i < 3)
    return 0;
  /*
   * The suffix must start at a separator in both names.
   */
  size_t a_start = alen - i;
  size_t b_start = blen - i;
  if (a_start == 0 || b_start == 0)
    return 0;
  if ((a[a_start] != '_' && a[a_start] != '-') ||
      (b[b_start] != '_' && b[b_start] != '-'))
    return 0;
  /*
   * Extend left while both sides stay equal (covers `_store_elim` etc).
   */
  while (a_start > 0 && b_start > 0 && a[a_start - 1] == b[b_start - 1]) {
    a_start--;
    b_start--;
    i++;
    if (a[a_start] == '_' || a[a_start] == '-')
      break;
  }
  if (a_start == 0 || b_start == 0)
    return 0;
  /*
   * Stems must be non-empty.
   */
  if (a_start < 2 || b_start < 2)
    return 0;
  return i;
}

/*
 * Family label for passes that belong together semantically but share no
 * affix (e.g. loop analyses `scev_lite`/`irce`/`licm`).  Returns NULL when
 * the pass has no family; used only as a last-resort grouping hint so
 * consecutive same-family quiet passes still fold into one token.
 */
static const char *nyr_pass_family(const char *pass) {
  if (!pass)
    return NULL;
  if (strncmp(pass, "loop_", 5) == 0 || strcmp(pass, "scev_lite") == 0 ||
      strcmp(pass, "irce") == 0 || strcmp(pass, "licm") == 0 ||
      strcmp(pass, "iv_elim") == 0 || strcmp(pass, "scev_lite") == 0)
    return "loop";
  if (strncmp(pass, "tbuf_", 5) == 0)
    return "tbuf";
  if (strstr(pass, "sroa") != NULL)
    return "sroa";
  if (strstr(pass, "vectorize") != NULL || strcmp(pass, "loop_unroll") == 0 ||
      strcmp(pass, "loop_unroll_ssa") == 0 || strcmp(pass, "loop_peel") == 0 ||
      strcmp(pass, "unroll_jam") == 0 || strcmp(pass, "softpipe_rewrite") == 0)
    return "vec";
  if (strcmp(pass, "cfg_simplify") == 0 || strcmp(pass, "jump_thread") == 0 ||
      strcmp(pass, "block_layout") == 0 || strcmp(pass, "phiopt") == 0 ||
      strcmp(pass, "if_convert") == 0 || strncmp(pass, "compact", 7) == 0)
    return "cfg";
  if (strncmp(pass, "inline_", 7) == 0 || strcmp(pass, "devirtualize") == 0 ||
      strcmp(pass, "ipa_constprop") == 0 || strcmp(pass, "dead_arg_elim") == 0)
    return "inline";
  if (strcmp(pass, "const_fold") == 0 || strcmp(pass, "copy_prop") == 0 ||
      strcmp(pass, "cse") == 0 || strcmp(pass, "peephole") == 0 ||
      strcmp(pass, "strength_reduce") == 0 || strcmp(pass, "narrow") == 0 ||
      strcmp(pass, "apply_rules") == 0 || strcmp(pass, "sccp") == 0 ||
      strcmp(pass, "gvn_pre") == 0 || strcmp(pass, "cse") == 0)
    return "scalar";
  if (strstr(pass, "elim") != NULL || strstr(pass, "mem") != NULL ||
      strstr(pass, "store") != NULL || strstr(pass, "load") != NULL ||
      strcmp(pass, "dce") == 0 || strcmp(pass, "adce") == 0 ||
      strcmp(pass, "memcombine") == 0)
    return "mem";
  return NULL;
}

/*
 * Format an ordered pass sequence into grouped tokens for every naming
 * family, not just `tbuf_`:
 * - consecutive identical passes collapse to `name×N`,
 * - consecutive runs sharing a `prefix_` collapse to `prefix-{suf1,suf2}`
 *   (e.g. `tbuf-{private-object,promote-alloca}`, `loop-{rotate,peel}`,
 *   `inline-{small,general}`, `compact{,-sparse}`),
 * - consecutive runs sharing a `_suffix` collapse to `{a,b}-suffix`
 *   (e.g. `{aggregate,escape,points-to}-sroa`, `{dead-store,null-check}-elim`),
 * - 3+ consecutive runs in one semantic family without a shared affix fold
 *   to bare `{a,b,c}`.
 * Suffix `×N` is included when a member repeated.  Writes at most max_tokens
 * tokens, reporting the hidden remainder via out_hidden.
 */
static void nyr_format_group_token(char *token, size_t token_n,
                                    const char **run_names,
                                    const size_t *run_counts, size_t start,
                                    size_t end, size_t prefix,
                                    size_t suffix_len, const char *suffix_str,
                                    int kind) {
  /*
   * kind: 1=prefix, 2=suffix, 3=family-bare
   */
  token[0] = '\0';
  if (kind == 1) {
    char disp_prefix[128] = {0};
    if (prefix == 0) {
      disp_prefix[0] = '\0';
    } else {
      char raw_prefix[128] = {0};
      size_t plen =
          prefix < sizeof(raw_prefix) - 1 ? prefix : sizeof(raw_prefix) - 1;
      memcpy(raw_prefix, run_names[start], plen);
      raw_prefix[plen] = '\0';
      nyr_display_pass(disp_prefix, sizeof(disp_prefix), raw_prefix);
    }
    size_t pos = 0;
    pos += snprintf(token + pos, token_n - pos, "%s{", disp_prefix);
    for (size_t k = start; k <= end && pos < token_n - 1; ++k) {
      char disp[128] = {0};
      nyr_display_pass(disp, sizeof(disp), run_names[k] + prefix);
      if (k > start)
        pos += snprintf(token + pos, token_n - pos, ",");
      if (disp[0] == '\0') {
        /*
         * Exact-prefix member (`compact` in `compact{,-sparse}`).
         */
        if (run_counts[k] > 1)
          pos += snprintf(token + pos, token_n - pos, "∅×%zu", run_counts[k]);
        else
          pos += snprintf(token + pos, token_n - pos, "∅");
      } else if (run_counts[k] > 1)
        pos +=
            snprintf(token + pos, token_n - pos, "%s×%zu", disp, run_counts[k]);
      else
        pos += snprintf(token + pos, token_n - pos, "%s", disp);
    }
    if (pos < token_n - 1)
      snprintf(token + pos, token_n - pos, "}");
  } else if (kind == 2) {
    char disp_suffix[128] = {0};
    nyr_display_pass(disp_suffix, sizeof(disp_suffix), suffix_str);
    size_t pos = 0;
    pos += snprintf(token + pos, token_n - pos, "{");
    for (size_t k = start; k <= end && pos < token_n - 1; ++k) {
      size_t rlen = strlen(run_names[k]);
      char raw_stem[128] = {0};
      size_t stem_len = rlen > suffix_len ? rlen - suffix_len : 0;
      if (stem_len >= sizeof(raw_stem))
        stem_len = sizeof(raw_stem) - 1;
      memcpy(raw_stem, run_names[k], stem_len);
      raw_stem[stem_len] = '\0';
      char disp[128] = {0};
      nyr_display_pass(disp, sizeof(disp), raw_stem);
      if (k > start)
        pos += snprintf(token + pos, token_n - pos, ",");
      if (run_counts[k] > 1)
        pos +=
            snprintf(token + pos, token_n - pos, "%s×%zu", disp, run_counts[k]);
      else
        pos += snprintf(token + pos, token_n - pos, "%s", disp);
    }
    if (pos < token_n - 1)
      snprintf(token + pos, token_n - pos, "}%s", disp_suffix);
  } else {
    size_t pos = 0;
    pos += snprintf(token + pos, token_n - pos, "{");
    for (size_t k = start; k <= end && pos < token_n - 1; ++k) {
      char disp[128] = {0};
      nyr_display_pass(disp, sizeof(disp), run_names[k]);
      if (k > start)
        pos += snprintf(token + pos, token_n - pos, ",");
      if (run_counts[k] > 1)
        pos +=
            snprintf(token + pos, token_n - pos, "%s×%zu", disp, run_counts[k]);
      else
        pos += snprintf(token + pos, token_n - pos, "%s", disp);
    }
    if (pos < token_n - 1)
      snprintf(token + pos, token_n - pos, "}");
  }
}

static void nyr_format_pass_seq(char *out, size_t out_n, const char **seq,
                                 size_t seq_len, size_t max_tokens,
                                 size_t *out_tokens, size_t *out_hidden) {
  if (out_tokens)
    *out_tokens = 0;
  if (out_hidden)
    *out_hidden = 0;
  if (!out || out_n == 0)
    return;
  out[0] = '\0';
  if (!seq || seq_len == 0)
    return;
  /*
   * Compress into consecutive identical runs first.
   */
  const char *run_names[NYIR_NOOP_MAX_SEQ];
  size_t run_counts[NYIR_NOOP_MAX_SEQ];
  size_t run_len = 0;
  for (size_t i = 0; i < seq_len; ++i) {
    if (run_len > 0 && strcmp(run_names[run_len - 1], seq[i]) == 0) {
      run_counts[run_len - 1]++;
    } else if (run_len < NYIR_NOOP_MAX_SEQ) {
      run_names[run_len] = seq[i];
      run_counts[run_len] = 1;
      run_len++;
    }
  }
  /*
   * Precompute per-token group extents so hidden-count matches rendering.
   */
  size_t token_run_consumed[NYIR_NOOP_MAX_SEQ] = {0};
  size_t token_kind[NYIR_NOOP_MAX_SEQ] = {0};
  size_t token_affix[NYIR_NOOP_MAX_SEQ] = {0};
  size_t n_tokens_total = 0;
  {
    size_t i = 0;
    while (i < run_len && n_tokens_total < NYIR_NOOP_MAX_SEQ) {
      size_t group_end = i;
      size_t prefix = 0;
      size_t suffix_len = 0;
      int kind = 0;
      if (i + 1 < run_len) {
        prefix = nyr_common_prefix_len(run_names[i], run_names[i + 1]);
        if (prefix) {
          size_t ge = i + 1;
          while (ge + 1 < run_len &&
                 nyr_common_prefix_len(run_names[ge], run_names[ge + 1]) ==
                     prefix &&
                 strncmp(run_names[i], run_names[ge + 1], prefix) == 0)
            ge++;
          if (ge - i >= 1) {
            bool okg = true;
            for (size_t k = i + 1; k <= ge; ++k)
              if (strncmp(run_names[i], run_names[k], prefix) != 0)
                okg = false;
            if (okg) {
              group_end = ge;
              kind = 1;
            }
          }
        }
        if (!kind) {
          suffix_len = nyr_common_suffix_len(run_names[i], run_names[i + 1]);
          if (suffix_len) {
            size_t alen = strlen(run_names[i]);
            const char *suf = run_names[i] + alen - suffix_len;
            size_t ge = i + 1;
            while (ge + 1 < run_len) {
              size_t sl =
                  nyr_common_suffix_len(run_names[ge], run_names[ge + 1]);
              if (sl != suffix_len)
                break;
              size_t blen = strlen(run_names[ge + 1]);
              if (blen < suffix_len ||
                  strcmp(run_names[ge + 1] + blen - suffix_len, suf) != 0)
                break;
              ge++;
            }
            if (ge - i >= 1) {
              bool okg = true;
              for (size_t k = i; k <= ge; ++k) {
                size_t kl = strlen(run_names[k]);
                if (kl < suffix_len ||
                    strcmp(run_names[k] + kl - suffix_len, suf) != 0)
                  okg = false;
              }
              if (okg) {
                group_end = ge;
                kind = 2;
              } else {
                suffix_len = 0;
              }
            } else {
              suffix_len = 0;
            }
          }
        }
        if (!kind) {
          const char *fam = nyr_pass_family(run_names[i]);
          if (fam) {
            size_t ge = i + 1;
            while (ge + 1 < run_len) {
              const char *f2 = nyr_pass_family(run_names[ge + 1]);
              if (!f2 || strcmp(fam, f2) != 0)
                break;
              /*
               * Family grouping must not hide an affix group inside.
               */
              if (nyr_common_prefix_len(run_names[ge], run_names[ge + 1]) ||
                  nyr_common_suffix_len(run_names[ge], run_names[ge + 1]))
                break;
              ge++;
            }
            /*
             * Need 3+ for a bare-family group to be worthwhile.
             */
            if (ge - i >= 2) {
              group_end = ge;
              kind = 3;
            }
          }
        }
      }
      token_run_consumed[n_tokens_total] = group_end - i + 1;
      token_kind[n_tokens_total] = (size_t)kind;
      token_affix[n_tokens_total] = kind == 1   ? prefix
                                    : kind == 2 ? suffix_len
                                                : 0;
      n_tokens_total++;
      i = group_end + 1;
    }
  }
  size_t used = 0;
  size_t tokens = 0;
  size_t run_idx = 0;
  for (size_t t = 0; t < n_tokens_total && tokens < max_tokens; ++t) {
    size_t start = run_idx;
    size_t end = run_idx + token_run_consumed[t] - 1;
    char token[512] = {0};
    if (token_kind[t] == 0) {
      char disp[128] = {0};
      nyr_display_pass(disp, sizeof(disp), run_names[start]);
      if (run_counts[start] > 1)
        snprintf(token, sizeof(token), "%s×%zu", disp, run_counts[start]);
      else
        snprintf(token, sizeof(token), "%s", disp);
    } else if (token_kind[t] == 1) {
      nyr_format_group_token(token, sizeof(token), run_names, run_counts,
                              start, end, token_affix[t], 0, NULL, 1);
    } else if (token_kind[t] == 2) {
      size_t alen = strlen(run_names[start]);
      const char *suf = run_names[start] + alen - token_affix[t];
      nyr_format_group_token(token, sizeof(token), run_names, run_counts,
                              start, end, 0, token_affix[t], suf, 2);
    } else {
      nyr_format_group_token(token, sizeof(token), run_names, run_counts,
                              start, end, 0, 0, NULL, 3);
    }
    size_t need = strlen(token) + (tokens ? 2 : 1) + 1;
    if (used + need >= out_n)
      break;
    used += (size_t)snprintf(out + used, out_n - used, "%s%s",
                             tokens ? ", " : " ", token);
    tokens++;
    run_idx += token_run_consumed[t];
  }
  if (out_tokens)
    *out_tokens = tokens;
  if (out_hidden) {
    size_t rendered_runs = 0;
    for (size_t t = 0; t < tokens && t < n_tokens_total; ++t)
      rendered_runs += token_run_consumed[t];
    *out_hidden = run_len > rendered_runs ? run_len - rendered_runs : 0;
  }
}

static void nyr_noop_record(const char *pass_name, double ms) {
  g_nyir_noop.total++;
  g_nyir_noop.ms += ms;
  if (g_nyir_noop.seq_len < NYIR_NOOP_MAX_SEQ)
    g_nyir_noop.seq[g_nyir_noop.seq_len++] = pass_name;
  for (size_t i = 0; i < g_nyir_noop.distinct && i < NYIR_NOOP_MAX_NAMES; ++i) {
    if (strcmp(g_nyir_noop.names[i], pass_name) == 0) {
      g_nyir_noop.counts[i]++;
      return;
    }
  }
  if (g_nyir_noop.distinct < NYIR_NOOP_MAX_NAMES) {
    g_nyir_noop.names[g_nyir_noop.distinct] = pass_name;
    g_nyir_noop.counts[g_nyir_noop.distinct] = 1;
    g_nyir_noop.distinct++;
  }
}

static void nyr_noop_flush(FILE *dump) {
  if (!dump || g_nyir_noop.total == 0)
    return;
  double avg =
      g_nyir_noop.total ? g_nyir_noop.ms / (double)g_nyir_noop.total : 0.0;
  fprintf(dump,
          "· %zu no-op pass%s (%.2fms total, ~%.2fms avg):", g_nyir_noop.total,
          g_nyir_noop.total == 1 ? "" : "es", g_nyir_noop.ms, avg);
  char grouped[2048] = {0};
  size_t hidden = 0;
  if (g_nyir_noop.seq_len > 0) {
    size_t tokens = 0;
    nyr_format_pass_seq(grouped, sizeof(grouped), g_nyir_noop.seq,
                         g_nyir_noop.seq_len, 12, &tokens, &hidden);
    fputs(grouped, dump);
  } else {
    size_t shown = 0;
    for (size_t i = 0; i < g_nyir_noop.distinct && shown < 12; ++i) {
      char disp[128] = {0};
      nyr_display_pass(disp, sizeof(disp), g_nyir_noop.names[i]);
      fprintf(dump, "%s %s×%u", shown ? "," : "", disp, g_nyir_noop.counts[i]);
      shown++;
    }
    if (g_nyir_noop.distinct > shown)
      hidden = g_nyir_noop.distinct - shown;
  }
  if (hidden)
    fprintf(dump, ", +%zu more", hidden);
  fprintf(dump, "\n");
  memset(&g_nyir_noop, 0, sizeof(g_nyir_noop));
}

/*
 * Pending NYIR_PASS heuristic no-op records for the non-dump (-vvvvvv without
 * --nyir-verify) path.  Consecutive `no-rewrite` events carry no per-pass
 * information beyond the name, so they fold into one summary event per
 * rewrite boundary instead of ~70 lines per function.
 */
#define NYIR_HEU_NOOP_MAX_SEQ 256
typedef struct {
  const char *seq[NYIR_HEU_NOOP_MAX_SEQ];
  size_t len;
  size_t total;
  double ms;
} nyr_heu_noop_t;
static nyr_heu_noop_t g_nyir_heu_noop;

static void nyr_heu_noop_record(const char *pass_name, double ms) {
  g_nyir_heu_noop.total++;
  g_nyir_heu_noop.ms += ms;
  if (g_nyir_heu_noop.len < NYIR_HEU_NOOP_MAX_SEQ)
    g_nyir_heu_noop.seq[g_nyir_heu_noop.len++] = pass_name;
}

static void nyr_heu_noop_flush(void) {
  if (g_nyir_heu_noop.total == 0)
    return;
  if (!ny_dbg_is_enabled(NY_DBG_HEURISTIC)) {
    memset(&g_nyir_heu_noop, 0, sizeof(g_nyir_heu_noop));
    return;
  }
  char grouped[2048] = {0};
  size_t hidden = 0;
  size_t tokens = 0;
  nyr_format_pass_seq(grouped, sizeof(grouped), g_nyir_heu_noop.seq,
                       g_nyir_heu_noop.len, 14, &tokens, &hidden);
  char detail[2304] = {0};
  if (hidden)
    snprintf(detail, sizeof(detail), "no-op×%zu (%.2fms total):%s, +%zu more",
             g_nyir_heu_noop.total, g_nyir_heu_noop.ms, grouped, hidden);
  else
    snprintf(detail, sizeof(detail), "no-op×%zu (%.2fms total):%s",
             g_nyir_heu_noop.total, g_nyir_heu_noop.ms, grouped);
  ny_dbg_heuristic_event("NYIR_PASS", "%s", detail);
  memset(&g_nyir_heu_noop, 0, sizeof(g_nyir_heu_noop));
}

static const char *nyr_debug_pass_name(bool (*pass)(nyr_func_t *)) {
  for (size_t i = 0;
       i < sizeof(nyr_pass_controls) / sizeof(nyr_pass_controls[0]); ++i)
    if (pass == nyr_pass_controls[i].pass)
      return nyr_pass_controls[i].name;
  return "unknown";
}

static bool timed_pass_verified(nyr_func_t *f, bool (*pass)(nyr_func_t *),
                                double *out_ms, FILE *dump, bool *ok) {
  const char *pass_name = nyr_debug_pass_name(pass);
  char tag[64];
  char ir_phase[128];
  nyr_pass_tag(tag, sizeof(tag), pass_name);
  if (nyr_pass_is_skipped(pass)) {
    if (out_ms)
      *out_ms = 0;
    if (dump)
      nyr_noop_record(pass_name, 0.0);
    return true;
  }
  size_t before_insts = f ? f->len : 0;
  int before_values = f ? f->next_value : 0;
  size_t before_blocks = nyr_block_count(f);
  uint64_t before = nyr_debug_fingerprint(f);
  nyr_func_t rollback = {0};
  nyr_func_t tv_before = {0};
  nyr_func_t dump_before = {0};
  bool want_tv = nyr_tv_trials > 0;
  bool want_dump_diff = dump != NULL;
  bool want_rollback = nyr_rollback_on_verify_fail && nyr_verify_each_pass;
  if (want_rollback && !nyr_ir_func_clone(f, &rollback)) {
    *ok = false;
    if (out_ms)
      *out_ms = 0;
    return false;
  }
  if (want_tv && !nyr_ir_func_clone(f, &tv_before)) {
    nyr_ir_func_free(&rollback);
    *ok = false;
    if (out_ms)
      *out_ms = 0;
    return false;
  }
  if (want_dump_diff && !nyr_ir_func_clone(f, &dump_before)) {
    nyr_ir_func_free(&rollback);
    nyr_ir_func_free(&tv_before);
    *ok = false;
    if (out_ms)
      *out_ms = 0;
    return false;
  }
  ny_tick_t t0 = ny_ticks_now();
  snprintf(ir_phase, sizeof(ir_phase), "%s input", pass_name);
  NY_DBG_IR_FULL(f, ir_phase);
  *ok = pass(f);
  snprintf(ir_phase, sizeof(ir_phase), "%s output", pass_name);
  NY_DBG_IR_FULL(f, ir_phase);
  if (*ok)
    nyr_refresh_metadata(f);
  if (out_ms)
    *out_ms = ny_ticks_elapsed_ms(t0);
  if (*ok) {
    char vbuf[256] = {0};
    if (!nyr_ir_verify(f, vbuf, sizeof(vbuf))) {
      if (dump) {
        nyr_noop_flush(dump);
        fprintf(dump, "· %s  VERIFY FAIL: %s\n", tag, vbuf);
      }
      *ok = false;
    }
  }
  if (!*ok && want_rollback) {
    if (dump) {
      nyr_noop_flush(dump);
      fprintf(dump, "· %s  ROLLBACK\n", tag);
    }
    nyr_func_replace(f, &rollback);
    nyr_refresh_metadata(f);
  }
  bool oracle_changed = nyr_debug_fingerprint(f) != before;
  if (*ok && want_tv && oracle_changed)
    *ok = nyr_tv_checkpoint(&tv_before, f, pass_name);
  if (*ok && (!nyr_per_pass_oracle || oracle_changed))
    *ok = nyr_oracle_checkpoint(f, pass_name);
  nyr_ir_func_free(&tv_before);
  if (dump) {
    size_t after_blocks = nyr_block_count(f);
    uint64_t after = nyr_debug_fingerprint(f);
    double ms = out_ms ? *out_ms : ny_ticks_elapsed_ms(t0);
    bool changed = before != after;
    if (ny_dbg_is_enabled(NY_DBG_HEURISTIC)) {
      if (!*ok || changed) {
        /*
         * Compact record: verdict plus only the dimensions that moved.
         * Flush any folded no-ops first so order stays causal.
         */
        nyr_heu_noop_flush();
        char dims[160] = {0};
        size_t dl = 0;
        if (before_insts != (f ? f->len : 0)) {
          int n = snprintf(dims + dl, sizeof(dims) - dl, " i%zu→%zu",
                           before_insts, f ? f->len : 0);
          if (n > 0)
            dl += (size_t)n;
        }
        int after_values = f ? f->next_value : 0;
        if (before_values != after_values) {
          int n = snprintf(dims + dl, sizeof(dims) - dl, " v%d→%d",
                           before_values, after_values);
          if (n > 0)
            dl += (size_t)n;
        }
        if (before_blocks != after_blocks) {
          int n = snprintf(dims + dl, sizeof(dims) - dl, " b%zu→%zu",
                           before_blocks, after_blocks);
          if (n > 0)
            dl += (size_t)n;
        }
        ny_dbg_heuristic_event("NYIR_PASS", "%s: %s%s %.2fms", pass_name,
                               !*ok      ? "reject"
                               : changed ? "rewrite"
                                         : "no-rewrite",
                               dims, ms);
      } else {
        nyr_heu_noop_record(pass_name, ms);
      }
    }
    if (!changed) {
      /*
       * Zero-information record: hold it and fold it into the per-function
       * no-op summary instead of printing a full `· <pass> · 0.00ms` line
       * for every quiet pass (~80 lines per function at -vvvv).
       */
      nyr_noop_record(pass_name, ms);
    } else {
      nyr_noop_flush(dump);
      fprintf(dump, "· %s  Δ", tag);
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
      nyr_dump_diff(dump, &dump_before, f, tag);
    }
  }
  nyr_ir_func_free(&dump_before);
  nyr_ir_func_free(&rollback);
  if (*ok)
    nyr_finish_pass(pass);
  return *ok;
}

static void nyr_stats_append(nyr_opt_stats_t *stats, const char *name,
                              double ms, size_t before_insts,
                              size_t after_insts, int before_values,
                              int after_values, size_t before_blocks,
                              size_t after_blocks) {
  if (!stats)
    return;
  stats->total_time_ms += ms;
  if (stats->pass_count >= NYIR_MAX_PASS_STATS)
    return;
  stats->passes[stats->pass_count++] =
      (nyr_pass_stat_t){.name = name,
                         .time_ms = ms,
                         .before_insts = before_insts,
                         .after_insts = after_insts,
                         .before_values = before_values,
                         .after_values = after_values,
                         .before_blocks = before_blocks,
                         .after_blocks = after_blocks};
}

/*
 * Shared O0–O3 pipeline. dump!=NULL enables verified+compact-diff mode.
 */
static bool nyr_run_pass(nyr_func_t *f, bool (*pass)(nyr_func_t *),
                          nyr_opt_stats_t *stats, FILE *dump, bool *ok) {
  bool skipped = nyr_pass_is_skipped(pass);
  size_t before = f ? f->len : 0;
  int before_values = f ? f->next_value : 0;
  size_t before_blocks = nyr_block_count(f);
  if (skipped && ny_dbg_is_enabled(NY_DBG_HEURISTIC))
    ny_dbg_heuristic_event("NYIR_PASS", "%s: skip", nyr_debug_pass_name(pass));
  double ms = 0;
  bool r = dump ? timed_pass_verified(f, pass, &ms, dump, ok)
                : timed_pass(f, pass, &ms);
  if (!r)
    *ok = false;
  if (!skipped)
    nyr_stats_append(stats, nyr_debug_pass_name(pass), ms, before,
                      f ? f->len : 0, before_values, f ? f->next_value : 0,
                      before_blocks, nyr_block_count(f));
  return r;
}

static bool nyr_run_seq(nyr_func_t *f, bool (*const *passes)(nyr_func_t *),
                         int n, nyr_opt_stats_t *stats, FILE *dump, bool *ok) {
  for (int i = 0; i < n && *ok; ++i)
    if (!nyr_run_pass(f, passes[i], stats, dump, ok))
      return false;
  return *ok;
}

static bool nyr_run_finish_scalar(nyr_func_t *f, nyr_opt_stats_t *stats,
                                   FILE *dump, bool *ok) {
  if (!*ok || nyr_pass_stopped)
    return *ok;
  if (nyr_disabled_pass && strcmp(nyr_disabled_pass, "scalar_cleanup") == 0) {
    if (dump)
      fprintf(dump, "· <scalar-cleanup>  skip\n");
    return true;
  }

  bool has_cf = nyr_has_control_flow(f);
  bool has_locals = nyr_has_local_mem(f);
  if (f->len <= 2 && !has_locals && !has_cf) {
    nyr_run_pass(f, nyr_compact, stats, dump, ok);
  } else {
    static bool (*const base[])(nyr_func_t *) = {nyr_const_fold,
                                                  nyr_strength_reduce,
                                                  nyr_narrow,
                                                  nyr_peephole,
                                                  nyr_apply_rules,
                                                  nyr_copy_prop,
                                                  nyr_redundant_load_elim,
                                                  nyr_bounds_check_elim,
                                                  nyr_cse,
                                                  nyr_tbuf_scalar_len,
                                                  nyr_tbuf_private_object,
                                                  nyr_tbuf_promote_alloca};
    nyr_run_seq(f, base, 12, stats, dump, ok);
    if (*ok && has_locals) {
      static bool (*const memory[])(nyr_func_t *) = {
          nyr_memory_ssa_forward,  nyr_points_to_sroa,
          nyr_store_sink,          nyr_aggregate_sroa,
          nyr_escape_sroa,         nyr_tbuf_private_object,
          nyr_tbuf_promote_alloca, nyr_sroa_scalar,
          nyr_dead_store_elim,     nyr_dce};
      nyr_run_seq(f, memory, 10, stats, dump, ok);
    }
    if (*ok && has_cf) {
      static bool (*const control[])(nyr_func_t *) = {nyr_polyhedral_nest,
                                                       nyr_block_layout};
      nyr_run_seq(f, control, 2, stats, dump, ok);
    }
    if (*ok) {
      static bool (*const tail[])(nyr_func_t *) = {
          nyr_kernel_hint, nyr_icp_profile, nyr_cfg_simplify, nyr_dce,
          nyr_const_fold,  nyr_dce,         nyr_compact};
      nyr_run_seq(f, tail, 7, stats, dump, ok);
    }
  }
  if (*ok && nyr_stop_after_pass &&
      strcmp(nyr_stop_after_pass, "scalar_cleanup") == 0)
    nyr_pass_stopped = true;
  if (dump)
    nyr_noop_flush(dump);
  return *ok;
}

static bool nyr_optimize_pipeline(nyr_func_t *f, nyr_opt_stats_t *stats,
                                   int opt_level, FILE *dump) {
  /*
   * Read fast-math mode from env config each pipeline invocation.
   */
  nyr_set_fast_math(ny_env_enabled("NYTRIX_FAST_MATH"));
  nyr_pass_stopped = false;
  if (!nyr_pass_control_valid(nyr_disabled_pass) ||
      !nyr_pass_control_valid(nyr_stop_after_pass)) {
    const char *bad = !nyr_pass_control_valid(nyr_disabled_pass)
                          ? nyr_disabled_pass
                          : nyr_stop_after_pass;
    fprintf(stderr, "native NYIR: unknown optimizer pass '%s'\n", bad);
    return false;
  }
  if (stats) {
    memset(stats, 0, sizeof(*stats));
    nyr_collect_stats(f, &stats->before_insts, &stats->before_values,
                       stats->before_ops, NYIR_OP_COUNT);
  }
  /*
   * Opt-regression execution profiling: keep the pre-optimization snapshot
   * so a >10% size growth can be validated by actually executing before and
   * after in the debug VM before the word "regression" is used.
   */
  static nyr_func_t profile_before;
  static bool profile_before_valid = false;
#if !defined(NYR_NO_EVAL)
  nyr_pipeline_grew = false;
#endif
  bool profile_this = getenv("NYTRIX_FINGERPRINT_PROFILE") && f && f->len > 0;
  if (profile_this) {
    nyr_ir_func_free(&profile_before);
    profile_before_valid = nyr_ir_func_clone(f, &profile_before);
    if (!profile_before_valid)
      profile_this = false;
  }
  if (dump && f && f->len > 0)
    nyr_dump(dump, f, "<before>");

  bool ok = true;

  if (opt_level <= 0) {
    static bool (*const p0[])(nyr_func_t *) = {nyr_compact};
    nyr_run_seq(f, p0, 1, stats, dump, &ok);
  } else if (opt_level == 1) {
    static bool (*const p1[])(nyr_func_t *) = {
        nyr_devirtualize, nyr_const_fold,   nyr_peephole,   nyr_copy_prop,
        nyr_const_fold,   nyr_cfg_simplify, nyr_if_convert, nyr_dce,
        nyr_cfg_simplify, nyr_compact};
    nyr_run_seq(f, p1, 10, stats, dump, &ok);
    if (ok)
      nyr_run_pass(f, nyr_inline_small, stats, dump, &ok);
    if (ok)
      nyr_run_finish_scalar(f, stats, dump, &ok);
  } else if (opt_level == 2) {
    static bool (*const p2[])(nyr_func_t *) = {nyr_devirtualize,
                                                nyr_const_fold,
                                                nyr_peephole,
                                                nyr_copy_prop,
                                                nyr_cse,
                                                nyr_cfg_simplify,
                                                nyr_if_convert,
                                                nyr_dce,
                                                nyr_dead_store_elim,
                                                nyr_cfg_simplify,
                                                nyr_compact_if_sparse};
    nyr_run_seq(f, p2, 11, stats, dump, &ok);
    if (ok && !nyr_pass_stopped &&
        (!nyr_has_control_flow(f) || nyr_cf_mem2reg_enabled)) {
      static bool (*const prom[])(nyr_func_t *) = {
          nyr_mem2reg, nyr_sccp, nyr_dce, nyr_dead_store_elim,
          nyr_compact_if_sparse};
      nyr_run_seq(f, prom, 5, stats, dump, &ok);
    }
    if (ok && !nyr_pass_stopped)
      nyr_run_pass(f, nyr_inline_general, stats, dump, &ok);
    if (ok && !nyr_pass_stopped)
      nyr_run_pass(f, nyr_inline_small, stats, dump, &ok);
    /*
     * Wrapper inlining can expose fixed-size typed-buffer allocations whose
     * source bounds checks are still present at this point.  Promote those
     * scratch objects before loop/BCE passes erase the checks that prove all
     * derived payload addresses stay within the allocation.  Re-running the
     * pass later remains useful for allocations exposed by other transforms.
     *
     * NOTE: a second general-inline pass here was tried and reverted — it
     * miscompiled control-flow callees (inline/cmov-sort/native-oracle
     * FAILs). The clone path notes prior incorrect-value incidents across
     * cloned CFG paths; keep a single general pass.
     */
    if (ok && !nyr_pass_stopped)
      nyr_run_pass(f, nyr_tbuf_promote_alloca, stats, dump, &ok);
    if (ok && !nyr_pass_stopped) {
      static bool (*const post_inline1[])(nyr_func_t *) = {
          nyr_const_fold, nyr_sccp, nyr_copy_prop};
      static bool (*const post_inline2[])(nyr_func_t *) = {
          nyr_cfg_simplify, nyr_if_convert, nyr_dce};
      /*
       * Inlining exposes constants and dead branch structure recursively.
       */
      uint64_t fp1 = nyr_debug_fingerprint(f);
      nyr_run_seq(f, post_inline1, 3, stats, dump, &ok);
      if (ok && nyr_debug_fingerprint(f) != fp1) {
        nyr_run_seq(f, post_inline1, 3, stats, dump, &ok);
      }
      if (ok)
        nyr_run_seq(f, post_inline2, 3, stats, dump, &ok);
    }
    /*
     * Straight-line functions (e.g. the 500-diamond DCE bench) have no
     * loops: skip the loop-only passes entirely. Each still pays O(n)
     * CFG/SCEV rebuilds and bails with no change.
     */
    if (ok && !nyr_pass_stopped && nyr_has_loop(f)) {
      static bool (*const loop_opts[])(nyr_func_t *) = {
          nyr_loop_rotate,
          nyr_scev_lite,
          nyr_irce,
          nyr_loop_idiom,
          nyr_loop_memset,
          nyr_loop_last_overwrite,
          nyr_cfg_simplify,
          nyr_dce,
          nyr_licm,
          nyr_cse,
          nyr_scev_lite,
          nyr_irce,
          nyr_alias_store_sink};
      /*
       * LICM/CSE can expose a simpler loop guard and can replace repeated
       * managed-buffer length helpers with one dominating SSA value.  Refresh
       * SCEV and IRCE once after those motions so O2 actually consumes the
       * newly exposed affine/bounds facts before vectorization.
       */
      nyr_run_seq(f, loop_opts, 13, stats, dump, &ok);
    }
    if (ok && !nyr_pass_stopped && nyr_has_loop(f))
      nyr_run_pass(f, nyr_loop_unroll_ssa, stats, dump, &ok);
    /*
     * O2 is the default benchmark tier, so it must not skip the existing
     * vector pipeline entirely.  Keep the canonical induction/address form
     * through vectorization, then let IV elimination and SLP clean up the
     * remaining scalar regions/tails.
     */
    if (ok && !nyr_pass_stopped && nyr_has_loop(f))
      nyr_run_pass(f, nyr_loop_vectorize, stats, dump, &ok);
    if (ok && !nyr_pass_stopped && nyr_has_loop(f))
      nyr_run_pass(f, nyr_iv_elim, stats, dump, &ok);
    if (ok && !nyr_pass_stopped && nyr_has_loop(f))
      nyr_run_pass(f, nyr_loop_peel, stats, dump, &ok);
    if (ok && !nyr_pass_stopped && nyr_has_loop(f))
      nyr_run_pass(f, nyr_unroll_jam, stats, dump, &ok);
    if (ok && !nyr_pass_stopped && nyr_has_loop(f))
      nyr_run_pass(f, nyr_softpipe_rewrite, stats, dump, &ok);
    if (ok && !nyr_pass_stopped && nyr_has_loop(f))
      nyr_run_pass(f, nyr_loop_unroll, stats, dump, &ok);
    if (ok && !nyr_pass_stopped) {
      static bool (*const pre_slp[])(nyr_func_t *) = {nyr_redundant_load_elim,
                                                       nyr_memory_ssa_forward,
                                                       nyr_tbuf_private_object,
                                                       nyr_tbuf_promote_alloca,
                                                       nyr_sroa_scalar,
                                                       nyr_dead_store_elim,
                                                       nyr_dce};
      /*
       * SLP sees better trees after private/local memory has been forwarded
       * into SSA.  Keep this immediately before SLP so later scalar cleanup
       * does not become the first point where load/store chains disappear.
       */
      nyr_run_seq(f, pre_slp, 7, stats, dump, &ok);
      if (ok && !nyr_pass_stopped)
        nyr_run_pass(f, nyr_memcombine, stats, dump, &ok);
    }
    if (ok && !nyr_pass_stopped)
      nyr_run_pass(f, nyr_slp_vectorize, stats, dump, &ok);
    if (ok && !nyr_pass_stopped)
      nyr_run_finish_scalar(f, stats, dump, &ok);
  } else {
    /*
     * O3: build SSA once, keep PHIs through scalar/loop optimization, and
     * destroy them only at machine lowering.
     */
    static bool (*const head[])(nyr_func_t *) = {
        nyr_devirtualize, nyr_const_fold,   nyr_cfg_simplify,
        nyr_dce,          nyr_cfg_simplify, nyr_if_convert};
    nyr_run_seq(f, head, 6, stats, dump, &ok);

    bool has_cf = nyr_has_control_flow(f);
    if (ok && nyr_has_local_mem(f) && !nyr_pass_stopped &&
        (!has_cf || nyr_cf_mem2reg_enabled)) {
      static bool (*const early[])(nyr_func_t *) = {nyr_mem2reg,
                                                     nyr_sccp,
                                                     nyr_cfg_simplify,
                                                     nyr_dce,
                                                     nyr_alias_analysis,
                                                     nyr_compact_if_sparse,
                                                     nyr_redundant_load_elim,
                                                     nyr_bounds_check_elim};
      nyr_run_seq(f, early, 8, stats, dump, &ok);
    }
    if (ok && !nyr_pass_stopped)
      nyr_run_pass(f, nyr_inline_general, stats, dump, &ok);
    if (ok && !nyr_pass_stopped)
      nyr_run_pass(f, nyr_inline_small, stats, dump, &ok);
    /*
     * See the O2 note above: stack promotion must consume source bounds
     * evidence before BCE/IRCE intentionally erase it. (Second general
     * pass reverted here too — same miscompile as O2.)
     */
    if (ok && !nyr_pass_stopped)
      nyr_run_pass(f, nyr_tbuf_promote_alloca, stats, dump, &ok);
    if (ok && f->len > 2) {
      static bool (*const mid_before_egraph[])(nyr_func_t *) = {
          nyr_const_fold, nyr_sccp, nyr_copy_prop, nyr_cse, nyr_peephole};
      nyr_run_seq(f, mid_before_egraph, 5, stats, dump, &ok);
      /*
       * E-graph extraction can change values in loop-carried scalar state after
       * memory-to-SSA promotion. Keep it on straight-line functions, where its
       * local dominance model is sufficient, and leave loop functions to the
       * dedicated loop analyses and scalar cleanup below.
       */
      if (ok && !nyr_pass_stopped && !nyr_has_loop(f))
        nyr_run_pass(f, nyr_egraph_rewrite, stats, dump, &ok);
      static bool (*const mid_after_egraph[])(nyr_func_t *) = {
          nyr_cfg_simplify,
          nyr_if_convert,
          nyr_dce,
          nyr_null_check_elim,
          nyr_overflow_check_elim,
          nyr_vrp,
          nyr_cvp};
      nyr_run_seq(f, mid_after_egraph, 7, stats, dump, &ok);
    }
    /*
     * Loop-only O3 passes: skip entirely for straight-line functions
     * (e.g. the 500-diamond DCE bench). Each still pays CFG/SCEV rebuilds
     * and bails with no change. Mirrors the O2 has_loop guard.
     */
    if (ok && !nyr_pass_stopped && nyr_has_loop(f)) {
      static bool (*const loop_analysis[])(nyr_func_t *) = {
          nyr_loop_rotate,
          nyr_scev_lite,
          nyr_loop_last_overwrite,
          nyr_cfg_simplify,
          nyr_dce,
          nyr_null_align_facts,
          nyr_bounds_check_elim,
          nyr_irce,
          nyr_loop_idiom,
          nyr_loop_memset,
          nyr_loop_interchange,
          nyr_loop_versioning,
          nyr_loop_predication,
          nyr_affine_nest,
          nyr_licm,
          nyr_cse};
      nyr_run_seq(f, loop_analysis, 16, stats, dump, &ok);
      /*
       * Polyhedral tiling: annotate hot loops with tile-size hints.
       */
      if (ok && !nyr_pass_stopped)
        nyr_run_pass(f, nyr_polyhedral_tiling, stats, dump, &ok);
      /*
       * Drain NOPs licm left so the array doesn't bloat before further loop
       * passes; sparse-gated so it's a no-op when nothing was removed.
       */
      if (ok && !nyr_pass_stopped)
        nyr_run_pass(f, nyr_compact_if_sparse, stats, dump, &ok);
      if (ok && !nyr_pass_stopped)
        nyr_run_pass(f, nyr_loop_unswitch, stats, dump, &ok);
      if (ok && !nyr_pass_stopped)
        nyr_run_pass(f, nyr_loop_unroll_ssa, stats, dump, &ok);
      /*
       * Late O3 vectorization runs by default again: the historical PBKDF2
       * checksum corruption was isolated to two x64 machine-encoder defects
       * (inverted VEX R/B extension bits in vextracti128, and SIB-LEA
       * folding under vector load/store consumers), both fixed with the
       * `vec8_reduce_loop` executable regression. Strict f64 reductions are
       * eligible when the vector reducer preserves lane order; remaining
       * legality and profitability checks stay in the vectorizer.
       */
      if (ok && !nyr_pass_stopped)
        nyr_run_pass(f, nyr_loop_vectorize, stats, dump, &ok);
      /*
       * Prefetch insertion after loop structure is finalized.
       */
      if (ok && !nyr_pass_stopped)
        nyr_run_pass(f, nyr_prefetch_insert, stats, dump, &ok);
      if (ok && !nyr_pass_stopped)
        nyr_run_pass(f, nyr_iv_elim, stats, dump, &ok);
      if (ok && !nyr_pass_stopped)
        nyr_run_pass(f, nyr_cse, stats, dump, &ok);
      if (ok && !nyr_pass_stopped)
        nyr_run_pass(f, nyr_loop_peel, stats, dump, &ok);
      if (ok && !nyr_pass_stopped)
        nyr_run_pass(f, nyr_unroll_jam, stats, dump, &ok);
      if (ok && !nyr_pass_stopped)
        nyr_run_pass(f, nyr_softpipe_rewrite, stats, dump, &ok);
      if (ok && !nyr_pass_stopped)
        nyr_run_pass(f, nyr_loop_unroll, stats, dump, &ok);
    }
    /*
     * SLP + scalar cleanup run regardless of loops: SLP vectorizes
     * straight-line code too.
     */
    if (ok && !nyr_pass_stopped) {
      if (ok && !nyr_pass_stopped) {
        static bool (*const pre_slp[])(nyr_func_t *) = {
            nyr_redundant_load_elim,
            nyr_memory_ssa_forward,
            nyr_tbuf_private_object,
            nyr_tbuf_promote_alloca,
            nyr_sroa_scalar,
            nyr_dead_store_elim,
            nyr_dce,
            nyr_alias_store_sink};
        nyr_run_seq(f, pre_slp, 8, stats, dump, &ok);
        if (ok && !nyr_pass_stopped)
          nyr_run_pass(f, nyr_memcombine, stats, dump, &ok);
      }
      if (ok && !nyr_pass_stopped)
        nyr_run_pass(f, nyr_slp_vectorize, stats, dump, &ok);
      if (ok && !nyr_pass_stopped)
        nyr_run_pass(f, nyr_compact_if_sparse, stats, dump, &ok);
    }
    if (ok && nyr_has_control_flow(f))
      nyr_run_pass(f, nyr_jump_thread, stats, dump, &ok);
    if (ok)
      nyr_run_pass(f, nyr_icp_profile, stats, dump, &ok);
    if (ok)
      nyr_run_pass(f, nyr_block_layout, stats, dump, &ok);
    if (ok) {
      static bool (*const cfg[])(nyr_func_t *) = {
          nyr_cfg_simplify, nyr_phiopt,    nyr_if_convert,
          nyr_peephole,     nyr_copy_prop, nyr_dce};
      nyr_run_seq(f, cfg, 6, stats, dump, &ok);
    }
    if (ok && !nyr_pass_stopped) {
      static bool (*const gvn_cleanup[])(nyr_func_t *) = {
          nyr_copy_prop, nyr_gvn_pre, nyr_dce, nyr_compact_if_sparse};
      nyr_run_seq(f, gvn_cleanup, 4, stats, dump, &ok);
    }
    if (ok && !nyr_pass_stopped)
      nyr_run_pass(f, nyr_ipa_constprop, stats, dump, &ok);
    if (ok && !nyr_pass_stopped)
      nyr_run_pass(f, nyr_adce, stats, dump, &ok);
    if (ok && !nyr_pass_stopped)
      nyr_run_pass(f, nyr_dead_arg_elim, stats, dump, &ok);
    if (ok && !nyr_pass_stopped)
      nyr_run_finish_scalar(f, stats, dump, &ok);
  }

  /*
   * Machine-form x86-64/AArch64 lowering resolves SSA PHIs with
   * predecessor-edge parallel copies. Other direct-NYIR encoders still need
   * the local-memory representation.
   */
  if (ok && !nyr_pass_stopped && !nyr_preserve_phis)
    nyr_run_pass(f, nyr_phi_elim, stats, dump, &ok);

  /*
   * Final gate: verify the optimized function unconditionally. With
   * verify_each_pass defaulting off, the production path otherwise ships
   * whatever the last pass left behind with no semantic net at all.
   */
  if (ok) {
    nyr_stamp_local_store_classes(f);
    char err[1024] = {0};
    if (!nyr_ir_verify(f, err, sizeof(err))) {
      fprintf(stderr,
              "native NYIR: verification failed after optimization pipeline: "
              "%s\n",
              err[0] ? err : "unknown error");
      ok = false;
    }
  }

  if (stats)
    nyr_collect_stats(f, &stats->after_insts, &stats->after_values,
                       stats->after_ops, NYIR_OP_COUNT);
  if (ok)
    nyr_refresh_metadata(f);

  if (dump && stats && stats->total_time_ms > 0.001) {
    typedef struct {
      const char *name;
      size_t count;
      double time_ms;
      size_t before_insts;
      size_t after_insts;
      size_t max_delta;
    } nyr_agg_t;
    nyr_agg_t agg[NYIR_MAX_PASS_STATS] = {{0}};
    /*
     * Aggregate the per-pass rows by pass name before printing: the pipeline
     * repeats passes many times (cfg-simplify runs nine times), so raw rows
     * repeat too. A row prints individually only when it moved instructions
     * or consumed measurable time; everything else folds into one quiet
     * summary line.
     */
    fprintf(dump, "nyir pass timing (ms | instruction Δ | change):\n");
    size_t agg_count = 0;
    size_t quiet_rows = 0;
    double quiet_ms = 0.0;
    const char *quiet_seq[NYIR_MAX_PASS_STATS] = {0};
    size_t quiet_seq_len = 0;
    for (size_t i = 0; i < stats->pass_count; ++i) {
      const nyr_pass_stat_t *r = &stats->passes[i];
      const char *name = r->name ? r->name : "unknown";
      size_t delta = r->before_insts > r->after_insts
                         ? r->before_insts - r->after_insts
                         : r->after_insts - r->before_insts;
      bool informative = delta != 0 || r->time_ms >= 0.005;
      if (!informative) {
        quiet_rows++;
        quiet_ms += r->time_ms;
        if (quiet_seq_len < NYIR_MAX_PASS_STATS)
          quiet_seq[quiet_seq_len++] = name;
        continue;
      }
      /*
       * Find or create this name's aggregate row.
       */
      size_t slot = agg_count;
      for (size_t j = 0; j < agg_count; ++j) {
        if (strcmp(agg[j].name, name) == 0) {
          slot = j;
          break;
        }
      }
      if (slot == agg_count && agg_count < NYIR_MAX_PASS_STATS) {
        agg[agg_count++] = (nyr_agg_t){.name = name,
                                        .count = 0,
                                        .time_ms = 0.0,
                                        .before_insts = 0,
                                        .after_insts = 0,
                                        .max_delta = 0};
      }
      if (slot >= NYIR_MAX_PASS_STATS)
        continue;
      if (agg[slot].count == 0)
        agg[slot].before_insts = r->before_insts;
      agg[slot].count++;
      agg[slot].time_ms += r->time_ms;
      agg[slot].after_insts = r->after_insts;
      size_t d = agg[slot].before_insts > agg[slot].after_insts
                     ? agg[slot].before_insts - agg[slot].after_insts
                     : agg[slot].after_insts - agg[slot].before_insts;
      if (d > agg[slot].max_delta)
        agg[slot].max_delta = d;
    }
    size_t max_delta = 0;
    for (size_t i = 0; i < agg_count; ++i)
      if (agg[i].max_delta > max_delta)
        max_delta = agg[i].max_delta;
    for (size_t i = 0; i < agg_count; ++i) {
      size_t delta = agg[i].max_delta;
      char spark[9] = "--------";
      if (max_delta && delta) {
        size_t bars = (delta * 8 + max_delta - 1) / max_delta;
        if (bars > 8)
          bars = 8;
        memset(spark, '#', bars);
        memset(spark + bars, '-', 8 - bars);
      }
      char label[32];
      snprintf(label, sizeof(label), "%s%s", agg[i].name,
               agg[i].count > 1 ? "*" : "");
      fprintf(dump, "  %-22.*s %8.2f  %c%-6zu  %s\n", (int)sizeof(label) - 1,
              label, agg[i].time_ms,
              agg[i].after_insts < agg[i].before_insts   ? '-'
              : agg[i].after_insts > agg[i].before_insts ? '+'
                                                         : '=',
              delta, spark);
    }
    if (quiet_rows) {
      char quiet_grouped[1024] = {0};
      size_t hidden = 0;
      size_t qtokens = 0;
      nyr_format_pass_seq(quiet_grouped, sizeof(quiet_grouped), quiet_seq,
                           quiet_seq_len, 10, &qtokens, &hidden);
      /*
       * Truncate to one line; the formatter already caps tokens.
       */
      quiet_grouped[sizeof(quiet_grouped) - 1] = '\0';
      if (hidden)
        fprintf(dump, "  (quiet: %zu row%s, %.2fms:%s, +%zu more)\n",
                quiet_rows, quiet_rows == 1 ? "" : "s", quiet_ms, quiet_grouped,
                hidden);
      else
        fprintf(dump, "  (quiet: %zu row%s, %.2fms:%s)\n", quiet_rows,
                quiet_rows == 1 ? "" : "s", quiet_ms, quiet_grouped);
    }
    fprintf(dump, "  total                  %8.2f ms\n", stats->total_time_ms);
  } else if (verbose_enabled >= 1 && stats && stats->total_time_ms > 0.001) {
    bool grew = stats->after_insts > stats->before_insts;
    size_t delta = grew ? stats->after_insts - stats->before_insts
                        : stats->before_insts - stats->after_insts;
    double pct =
        stats->before_insts > 0 && delta > 0
            ? (grew ? 1.0 : -1.0) * 100.0 * (double)delta / stats->before_insts
            : 0.0;
    if (ny_dbg_json_enabled())
      ny_dbg_heuristic_event("NYIR_OPT_STATS",
                             "level=O%d instructions=%zu->%zu delta=%zu "
                             "direction=%s percent=%+.1f time_ms=%.2f",
                             opt_level, stats->before_insts, stats->after_insts,
                             delta,
                             delta == 0 ? "flat"
                             : grew     ? "growth"
                                        : "shrink",
                             pct, stats->total_time_ms);
    else if (!ny_dbg_is_enabled(NY_DBG_HEURISTIC) && verbose_enabled >= 2)
      NY_LOG_V2(
          "NYIR optimize O%d: %zu→%zu instructions (%s, %+.1f%%) in %.2f ms\n",
          opt_level, stats->before_insts, stats->after_insts,
          delta == 0 ? "flat"
          : grew     ? "growth"
                     : "shrink",
          pct, stats->total_time_ms);
    else if (!ny_dbg_is_enabled(NY_DBG_HEURISTIC))
      fprintf(stderr,
              "nyir opt(O%d): %zu->%zu insts (%c%zu, %+.1f%%) in %.2fms\n",
              opt_level, stats->before_insts, stats->after_insts,
              delta == 0 ? '='
              : grew     ? '+'
                         : '-',
              delta, pct, stats->total_time_ms);
  }
#if !defined(NYR_NO_EVAL)
  /*
   * Execute before/after profiling: only when the run grew >10% and both
   * snapshots execute in the debug VM.  The comparison times the same fixed
   * number of VM steps of work on each snapshot and reports the measured
   * execution delta — IR size alone is explicitly not a regression verdict.
   */
  bool pipeline_net_growth = stats && stats->before_insts > 0 &&
                             stats->after_insts * 10 > stats->before_insts * 11;
  if (profile_this && profile_before_valid &&
      (nyr_pipeline_grew || pipeline_net_growth)) {
    const int runs = 200;
    double before_ms = 0.0, after_ms = 0.0;
    bool before_ok = true, after_ok = true;
    nyr_eval_result_t rb, ra;
    char verr[192];
    for (int r = 0; r < runs && before_ok; ++r) {
      ny_tick_t t0 = ny_ticks_now();
      before_ok =
          nyr_ir_eval(&profile_before, NULL, 0, 4000000, &rb, verr, sizeof(verr));
      before_ms += ny_ticks_elapsed_ms(t0);
    }
    for (int r = 0; r < runs && after_ok; ++r) {
      ny_tick_t t0 = ny_ticks_now();
      after_ok = nyr_ir_eval(f, NULL, 0, 4000000, &ra, verr, sizeof(verr));
      after_ms += ny_ticks_elapsed_ms(t0);
    }
    if (before_ok && after_ok && before_ms > 0.0) {
      double ratio = after_ms / before_ms;
      char verdict[24];
      snprintf(verdict, sizeof(verdict), "%s",
               ratio > 1.10   ? "REGRESSION"
               : ratio < 0.90 ? "IMPROVED"
                              : "NEUTRAL");
      char pdetail[192];
      snprintf(pdetail, sizeof(pdetail),
               "VM execution before=%.3fms after=%.3fms (%d runs, "
               "ratio=%.2f); IR size %zu→%zu",
               before_ms, after_ms, runs, ratio, stats->before_insts,
               stats->after_insts);
      ny_trace_line("opt-regression", "%s: %s", verdict, pdetail);
      ny_dbg_opt_missed("opt-regression profile", verdict, pdetail);
    } else {
      ny_trace_line("opt-regression",
                    "UNMEASURED: debug VM could not "
                    "execute the snapshot (runtime calls unsupported); size "
                    "growth stays a candidate, not a verdict");
      ny_dbg_opt_missed("opt-regression profile", "UNMEASURED",
                        "debug VM could not execute the snapshot "
                        "(runtime calls unsupported); size growth stays a "
                        "candidate, not a verdict");
    }
  }
#endif
  if (profile_this)
    nyr_ir_func_free(&profile_before);
  nyr_heu_noop_flush();
  nyr_pass_burst_flush();
  return ok;
}

bool nyr_optimize_debug(nyr_func_t *f, FILE *dump, nyr_opt_stats_t *stats,
                         int opt_level) {
  return nyr_optimize_pipeline(f, stats, opt_level, dump);
}
