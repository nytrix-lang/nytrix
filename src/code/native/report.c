#include "code/native/internal.h"
#include "base/common.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* Tier fact collection, recommendation, and deterministic report output. */

static size_t ny_native_tier_inst_cost(const ny_nir_inst_t *in) {
  if (!in)
    return 0;
  switch (in->op) {
  case NY_NIR_NOP:
  case NY_NIR_LABEL:
    return 0;
  case NY_NIR_DIV_I64:
  case NY_NIR_MOD_I64:
    return 8;
  case NY_NIR_CALL:
    return 12;
  case NY_NIR_BR:
  case NY_NIR_BR_IF:
  case NY_NIR_RET:
    return 3;
  case NY_NIR_LOAD_LOCAL:
  case NY_NIR_STORE_LOCAL:
    return 2;
  default:
    return 1;
  }
}

typedef struct {
  size_t insts;
  int values;
  size_t cost;
  size_t calls;
  size_t branches;
  size_t memory_ops;
  size_t divmod_ops;
  size_t control_ops;
  size_t effect_ops;
} ny_native_tier_facts_t;

static void ny_native_tier_facts_add(ny_native_tier_facts_t *facts,
                                     const ny_nir_func_t *f) {
  if (!facts || !f)
    return;
  facts->insts += f->len;
  if (f->next_value > 0)
    facts->values += f->next_value;
  for (size_t i = 0; i < f->len; ++i) {
    const ny_nir_inst_t *in = &f->data[i];
    facts->cost += ny_native_tier_inst_cost(in);
    if (in->op == NY_NIR_CALL)
      facts->calls++;
    else if (in->op == NY_NIR_BR || in->op == NY_NIR_BR_IF)
      facts->branches++;
    else if (in->op == NY_NIR_LOAD_LOCAL || in->op == NY_NIR_STORE_LOCAL)
      facts->memory_ops++;
    if (in->op == NY_NIR_DIV_I64 || in->op == NY_NIR_MOD_I64)
      facts->divmod_ops++;
    if ((in->effects & (unsigned)NY_NIR_EFFECT_CONTROL) != 0)
      facts->control_ops++;
    if (in->effects != 0)
      facts->effect_ops++;
  }
}

static const char *ny_native_tier_recommendation(
    const ny_native_tier_plan_t *plan, const ny_native_target_info_t *target,
    const ny_native_tier_facts_t *facts) {
  if (!plan || !target || !facts)
    return "unavailable";
  bool has_vm = (target->caps & (unsigned)NY_NATIVE_CAP_NIR_VM) != 0;
  bool has_asm = (target->caps & (unsigned)NY_NATIVE_CAP_NIR_ASM) != 0;
  bool has_obj = (target->caps & ((unsigned)NY_NATIVE_CAP_ELF_OBJECT |
                                  (unsigned)NY_NATIVE_CAP_COFF_OBJECT |
                                  (unsigned)NY_NATIVE_CAP_MACHO_OBJECT)) != 0;
  if (facts->cost <= plan->cold_threshold && has_vm)
    return "nyir-vm-cold";
  if (plan->prefer_nir_vm && has_vm && facts->cost <= plan->compile_budget)
    return "nyir-vm-preferred";
  if (has_obj && plan->cache_score >= 50 && facts->cost >= plan->hot_threshold)
    return "native-object-cache";
  if (has_asm)
    return "native-asm";
  if (plan->prefer_ast_fallback)
    return "ast-fallback";
  return has_vm ? "nyir-vm" : "unsupported";
}

static const char *ny_native_tier_recommendation_with_profile(
    const ny_native_tier_plan_t *plan, const ny_native_target_info_t *target,
    const ny_native_tier_facts_t *facts,
    const ny_nir_eval_result_t *profile) {
  if (!profile || profile->steps == 0)
    return ny_native_tier_recommendation(plan, target, facts);
  bool has_obj =
      target && (target->caps & ((unsigned)NY_NATIVE_CAP_ELF_OBJECT |
                                 (unsigned)NY_NATIVE_CAP_COFF_OBJECT |
                                 (unsigned)NY_NATIVE_CAP_MACHO_OBJECT)) != 0;
  bool has_asm = target && (target->caps & (unsigned)NY_NATIVE_CAP_NIR_ASM) != 0;
  bool has_vm = target && (target->caps & (unsigned)NY_NATIVE_CAP_NIR_VM) != 0;
  if (plan && has_obj && plan->cache_score >= 50 &&
      profile->steps >= plan->hot_threshold)
    return "native-object-cache-profile";
  if (plan && plan->prefer_nir_vm && has_vm &&
      profile->steps <= plan->cold_threshold)
    return "nyir-vm-profile-cold";
  if (has_asm)
    return "native-asm-profile";
  return has_vm ? "nyir-vm-profile" : "unsupported";
}

static void ny_native_print_caps(FILE *out, unsigned caps) {
  bool first = true;
#define NY_CAP(name, bit)                                                        \
  do {                                                                           \
    if ((caps & (unsigned)(bit)) != 0) {                                         \
      fprintf(out, "%s%s", first ? "" : ",", name);                              \
      first = false;                                                             \
    }                                                                            \
  } while (0)
  NY_CAP("nir-asm", NY_NATIVE_CAP_NIR_ASM);
  NY_CAP("ast-fallback", NY_NATIVE_CAP_AST_FALLBACK);
  NY_CAP("asm-object", NY_NATIVE_CAP_ASM_OBJECT);
  NY_CAP("nir-vm", NY_NATIVE_CAP_NIR_VM);
  NY_CAP("elf-object", NY_NATIVE_CAP_ELF_OBJECT);
  NY_CAP("coff-object", NY_NATIVE_CAP_COFF_OBJECT);
  NY_CAP("macho-object", NY_NATIVE_CAP_MACHO_OBJECT);
#undef NY_CAP
  if (first)
    fputs("none", out);
}

bool ny_native_write_tier_report_for_program(const program_t *prog,
                                             const ny_options *opt, char *err,
                                             size_t err_len) {
  if (!opt || !opt->native_tier_report)
    return true;
  ny_native_target_info_t target = {0};
  if (!ny_native_target_info_init(&target, opt)) {
    ny_native_set_err(err, err_len,
                      "native tier report unavailable for selected backend");
    return false;
  }
  ny_native_tier_plan_t plan = {0};
  if (!ny_native_tier_plan_init(&plan, &target, opt)) {
    ny_native_set_err(err, err_len, "native tier report: failed to build plan");
    return false;
  }

  ny_nir_func_t rt_main = {0};
  ny_nir_func_t funcs[128];
  const char *func_names[128];
  memset(funcs, 0, sizeof(funcs));
  memset(func_names, 0, sizeof(func_names));
  size_t func_count = 0;
  char local_err[512] = {0};
  bool built = ny_native_build_nir(prog, opt, &rt_main, funcs, &func_count,
                                   128, local_err, sizeof(local_err));
  if (!built) {
    ny_native_set_err(err, err_len, "native tier report: %s",
                      local_err[0] ? local_err : "failed to build NYIR");
    return false;
  }
  size_t name_index = 0;
  for (size_t i = 0; prog && i < prog->body.len && name_index < func_count; ++i) {
    const stmt_t *stmt = prog->body.data[i];
    if (!stmt || stmt->kind != NY_S_FUNC)
      continue;
    func_names[name_index++] = stmt->as.fn.name ? stmt->as.fn.name : "<fn>";
  }

  ny_nir_eval_result_t vm_profile = {0};
  bool vm_profile_used = false;
  if (opt->nyir_run_profile && rt_main.len) {
    char profile_err[512] = {0};
    if (ny_native_collect_vm_profile(&rt_main, funcs, func_names, func_count,
                                     opt, &vm_profile, profile_err,
                                     sizeof(profile_err))) {
      vm_profile_used = true;
    } else if (verbose_enabled) {
      fprintf(stderr, "native tier report: VM profile unavailable: %s\n",
              profile_err[0] ? profile_err : "unknown error");
    }
  }

  ny_native_tier_facts_t facts = {0};
  ny_native_handoff_summary_t handoffs = {0};
  ny_native_tier_facts_add(&facts, &rt_main);
  ny_native_handoff_summary_t local_handoff = {0};
  if (ny_native_handoff_summary(&rt_main, &local_handoff)) {
    handoffs.entry_points += local_handoff.entry_points;
    handoffs.return_points += local_handoff.return_points;
    handoffs.call_points += local_handoff.call_points;
    handoffs.branch_points += local_handoff.branch_points;
    handoffs.label_points += local_handoff.label_points;
    handoffs.deopt_safe_points += local_handoff.deopt_safe_points;
  }
  for (size_t i = 0; i < func_count; ++i) {
    ny_native_tier_facts_add(&facts, &funcs[i]);
    memset(&local_handoff, 0, sizeof(local_handoff));
    if (ny_native_handoff_summary(&funcs[i], &local_handoff)) {
      handoffs.entry_points += local_handoff.entry_points;
      handoffs.return_points += local_handoff.return_points;
      handoffs.call_points += local_handoff.call_points;
      handoffs.branch_points += local_handoff.branch_points;
      handoffs.label_points += local_handoff.label_points;
      handoffs.deopt_safe_points += local_handoff.deopt_safe_points;
    }
  }

  FILE *out = stderr;
  if (opt->native_tier_report_path && opt->native_tier_report_path[0]) {
    ny_native_ensure_parent_dir_for_path(opt->native_tier_report_path);
    out = fopen(opt->native_tier_report_path, "wb");
    if (!out) {
      ny_native_set_err(err, err_len,
                        "native tier report: failed to open %s: %s",
                        opt->native_tier_report_path, strerror(errno));
      for (size_t i = 0; i < func_count; ++i)
        ny_nir_func_free(&funcs[i]);
      ny_nir_func_free(&rt_main);
      return false;
    }
  }

  fprintf(out, "native tier report target=%s abi=%s object=%s ptr=%zub\n",
          target.target_name ? target.target_name : "unknown",
          target.abi_name ? target.abi_name : "unknown",
          target.object_format ? target.object_format : "unknown",
          target.pointer_bits);
  fprintf(out, "caps=");
  ny_native_print_caps(out, target.caps);
  fputc('\n', out);
  fprintf(out,
          "plan budget=%zu hot=%zu cold=%zu cache=%u prefer_vm=%s ast_fallback=%s\n",
          plan.compile_budget, plan.hot_threshold, plan.cold_threshold,
          plan.cache_score, plan.prefer_nir_vm ? "yes" : "no",
          plan.prefer_ast_fallback ? "yes" : "no");
  fprintf(out,
          "facts functions=%zu insts=%zu values=%d cost=%zu calls=%zu "
          "branches=%zu locals=%zu divmod=%zu control=%zu effects=%zu\n",
          func_count + (rt_main.len ? 1u : 0u), facts.insts, facts.values,
          facts.cost, facts.calls, facts.branches, facts.memory_ops,
          facts.divmod_ops, facts.control_ops, facts.effect_ops);
  fprintf(out,
          "handoffs entries=%zu returns=%zu calls=%zu branches=%zu labels=%zu "
          "deopt_safe=%zu\n",
          handoffs.entry_points, handoffs.return_points, handoffs.call_points,
          handoffs.branch_points, handoffs.label_points,
          handoffs.deopt_safe_points);
  fprintf(out,
          "vm_profile used=%s returned=%s result=%" PRId64
          " steps=%zu calls=%zu branches_taken=%zu branches_not_taken=%zu "
          "max_pc=%zu max_value=%zu max_local=%zu\n",
          vm_profile_used ? "yes" : "no",
          vm_profile.returned ? "yes" : "no", vm_profile.result,
          vm_profile.steps, vm_profile.call_count, vm_profile.branch_taken,
          vm_profile.branch_not_taken, vm_profile.max_pc,
          vm_profile.max_value_index, vm_profile.max_local_index);
  fprintf(out, "recommend=%s\n",
          ny_native_tier_recommendation_with_profile(
              &plan, &target, &facts, vm_profile_used ? &vm_profile : NULL));

  ny_native_tier_facts_t rt_facts = {0};
  if (rt_main.len) {
    ny_native_tier_facts_add(&rt_facts, &rt_main);
    ny_native_handoff_summary_t rt_handoffs = {0};
    ny_native_handoff_summary(&rt_main, &rt_handoffs);
    fprintf(out,
            "function name=rt_main insts=%zu values=%d cost=%zu calls=%zu "
            "branches=%zu locals=%zu divmod=%zu control=%zu effects=%zu "
            "handoffs=%zu deopt_safe=%zu recommend=%s\n",
            rt_facts.insts, rt_facts.values, rt_facts.cost, rt_facts.calls,
            rt_facts.branches, rt_facts.memory_ops, rt_facts.divmod_ops,
            rt_facts.control_ops, rt_facts.effect_ops,
            rt_handoffs.entry_points + rt_handoffs.return_points +
                rt_handoffs.call_points + rt_handoffs.branch_points +
                rt_handoffs.label_points,
            rt_handoffs.deopt_safe_points,
            ny_native_tier_recommendation_with_profile(
                &plan, &target, &rt_facts,
                vm_profile_used ? &vm_profile : NULL));
  }

  size_t func_index = 0;
  for (size_t i = 0; prog && i < prog->body.len && func_index < func_count; ++i) {
    const stmt_t *stmt = prog->body.data[i];
    if (!stmt || stmt->kind != NY_S_FUNC)
      continue;
    ny_native_tier_facts_t fn_facts = {0};
    ny_native_tier_facts_add(&fn_facts, &funcs[func_index]);
    ny_native_handoff_summary_t fn_handoffs = {0};
    ny_native_handoff_summary(&funcs[func_index], &fn_handoffs);
    fprintf(out,
            "function name=%s insts=%zu values=%d cost=%zu calls=%zu "
            "branches=%zu locals=%zu divmod=%zu control=%zu effects=%zu "
            "handoffs=%zu deopt_safe=%zu recommend=%s\n",
            stmt->as.fn.name ? stmt->as.fn.name : "<fn>", fn_facts.insts,
            fn_facts.values, fn_facts.cost, fn_facts.calls,
            fn_facts.branches, fn_facts.memory_ops, fn_facts.divmod_ops,
            fn_facts.control_ops, fn_facts.effect_ops,
            fn_handoffs.entry_points + fn_handoffs.return_points +
                fn_handoffs.call_points + fn_handoffs.branch_points +
                fn_handoffs.label_points,
            fn_handoffs.deopt_safe_points,
            ny_native_tier_recommendation(&plan, &target, &fn_facts));
    func_index++;
  }

  if (out != stderr)
    fclose(out);
  for (size_t i = 0; i < func_count; ++i)
    ny_nir_func_free(&funcs[i]);
  ny_nir_func_free(&rt_main);
  if (err && err_len > 0)
    err[0] = '\0';
  return true;
}
