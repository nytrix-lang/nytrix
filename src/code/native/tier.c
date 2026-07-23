#include "code/native/native.h"

#include <string.h>

/* Tier defaults and NYIR handoff accounting are independent of lowering,
 * execution, and report formatting. */

bool ny_native_tier_plan_init(ny_native_tier_plan_t *plan,
                              const ny_native_target_info_t *target,
                              const ny_options *opt) {
  if (!plan)
    return false;
  memset(plan, 0, sizeof(*plan));
  plan->backend_name = target && target->target_name ? target->target_name : "unknown";
  plan->requested_tier = opt && opt->native_tier_raw ? opt->native_tier_raw : "auto";
  /* This planner is used only after a native target has been selected.
   * Experimental tier requests (including `llvm`) must not claim a different
   * executable path until that target actually has one. LLVM selection lives
   * in the top-level backend choice, not in a native-target tier report. */
  plan->resolved_tier = "baseline";
  /* Honour explicit tier requests as resolved labels even while the executable
   * path remains the shared NYIR→Machine IR→encode pipeline. Stencil/fast/opt
   * select budgets; llvm remains a top-level backend choice. */
  if (opt && opt->native_tier_raw && opt->native_tier_raw[0]) {
    if (strcmp(opt->native_tier_raw, "stencil") == 0)
      plan->resolved_tier = "stencil";
    else if (strcmp(opt->native_tier_raw, "fast") == 0)
      plan->resolved_tier = "fast";
    else if (strcmp(opt->native_tier_raw, "opt") == 0)
      plan->resolved_tier = "opt";
    else if (strcmp(opt->native_tier_raw, "baseline") == 0)
      plan->resolved_tier = "baseline";
  }
  ny_opt_profile_kind_t profile =
      ny_opt_profile_kind_from_name(opt && opt->opt_profile ? opt->opt_profile
                                                            : NULL);
  switch (profile) {
  case NY_OPT_PROFILE_PEAK:
    plan->compile_budget = 1000000;
    plan->hot_threshold = 64;
    plan->cold_threshold = 2;
    plan->cache_score = 100;
    if (!opt || !opt->native_tier_raw || !opt->native_tier_raw[0] ||
        strcmp(opt->native_tier_raw, "auto") == 0)
      plan->resolved_tier = "opt";
    break;
  case NY_OPT_PROFILE_SPEED:
    plan->compile_budget = 500000;
    plan->hot_threshold = 32;
    plan->cold_threshold = 2;
    plan->cache_score = 80;
    break;
  case NY_OPT_PROFILE_COMPILE:
  case NY_OPT_PROFILE_NONE:
    plan->compile_budget = 25000;
    plan->hot_threshold = 8;
    plan->cold_threshold = 1;
    plan->cache_score = 20;
    plan->prefer_nir_vm = true;
    break;
  case NY_OPT_PROFILE_SIZE:
    plan->compile_budget = 75000;
    plan->hot_threshold = 16;
    plan->cold_threshold = 1;
    plan->cache_score = 50;
    break;
  case NY_OPT_PROFILE_BALANCED:
  case NY_OPT_PROFILE_CUSTOM:
  case NY_OPT_PROFILE_DEFAULT:
  default:
    plan->compile_budget = 150000;
    plan->hot_threshold = 16;
    plan->cold_threshold = 1;
    plan->cache_score = 60;
    break;
  }
  if (opt) {
    if (opt->native_tier_budget >= 0)
      plan->compile_budget = (size_t)opt->native_tier_budget;
    if (opt->native_hot_threshold >= 0)
      plan->hot_threshold = (size_t)opt->native_hot_threshold;
    if (opt->native_cold_threshold >= 0)
      plan->cold_threshold = (size_t)opt->native_cold_threshold;
    if (opt->native_cache_score >= 0)
      plan->cache_score = (unsigned)opt->native_cache_score;
    if (opt->native_prefer_vm)
      plan->prefer_nir_vm = true;
    if (opt->native_prefer_asm)
      plan->prefer_nir_vm = false;
  }
  plan->prefer_ast_fallback =
      target && (target->caps & (unsigned)NY_NATIVE_CAP_AST_FALLBACK) != 0 &&
      !plan->prefer_nir_vm;
  return true;
}

bool ny_native_handoff_summary(const nyir_func_t *nyir,
                               ny_native_handoff_summary_t *summary) {
  if (!nyir || !summary)
    return false;
  memset(summary, 0, sizeof(*summary));
  if (nyir->len == 0)
    return true;
  summary->entry_points = 1;
  summary->deopt_safe_points = 1;
  for (size_t i = 0; i < nyir->len; ++i) {
    const nyir_inst_t *in = &nyir->data[i];
    switch (in->op) {
    case NYIR_RET:
      summary->return_points++;
      summary->deopt_safe_points++;
      break;
    case NYIR_CALL:
      summary->call_points++;
      summary->deopt_safe_points++;
      break;
    case NYIR_BR:
    case NYIR_BR_IF:
      summary->branch_points++;
      summary->deopt_safe_points++;
      break;
    case NYIR_LABEL:
      summary->label_points++;
      summary->deopt_safe_points++;
      break;
    default:
      break;
    }
  }
  return true;
}
