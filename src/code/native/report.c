/*
 * Native tier report: produces machine-readable tiering diagnostics
 * with hot/cold function lists and backend-recommendation summaries.
 */
#include "code/native/internal.h"
#include "code/native/object/internal.h"
#include "code/native/ir/opt/util.h"
#include "base/common.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/*
 * Tier fact collection, recommendation, and deterministic report output.
 */

static size_t ny_native_tier_inst_cost(const nyir_inst_t *in) {
  if (!in)
    return 0;
  switch (in->op) {
  case NYIR_NOP:
  case NYIR_LABEL:
    return 0;
  case NYIR_DIV_I64:
  case NYIR_MOD_I64:
    return 8;
  case NYIR_CALL:
    return 12;
  case NYIR_BR:
  case NYIR_BR_IF:
  case NYIR_RET:
    return 3;
  case NYIR_LOAD_LOCAL:
  case NYIR_STORE_LOCAL:
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
  size_t direct_calls;
  size_t indirect_calls;
  size_t runtime_calls;
  size_t dynamic_ops;
  size_t tag_checks;
  size_t box_unbox_conversions;
  size_t heap_allocations;
  size_t branches;
  size_t memory_ops;
  size_t bounds_checks;
  size_t divmod_ops;
  size_t control_ops;
  size_t effect_ops;
  size_t unknown_effect_ops;
  size_t io_effect_ops;
  size_t thread_effect_ops;
  size_t ffi_effect_ops;
  size_t fenv_effect_ops;
  size_t alias_unresolved_ops;
  size_t vectorize_attempted_loops;
  size_t vectorize_rejected_loops;
  size_t vectorized_loops;
} ny_native_tier_facts_t;

static bool ny_native_symbol_is_box_conversion(const char *symbol) {
  if (!symbol)
    return false;
  return strstr(symbol, "_any_to_") != NULL ||
         strcmp(symbol, "rt_bigint_from_i64_raw") == 0 ||
         strcmp(symbol, "rt_bigint_to_i64_raw") == 0 ||
         strcmp(symbol, "rt_native_bigfloat_from_value") == 0 ||
         strcmp(symbol, "rt_native_bigfloat_to_f64") == 0;
}

static void ny_native_json_string(FILE *out, const char *s) {
  fputc('"', out);
  if (s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
      switch (*p) {
      case '"': fputs("\\\"", out); break;
      case '\\': fputs("\\\\", out); break;
      case '\n': fputs("\\n", out); break;
      case '\r': fputs("\\r", out); break;
      case '\t': fputs("\\t", out); break;
      default:
        if (*p < 0x20) fprintf(out, "\\u%04x", (unsigned)*p);
        else fputc((int)*p, out);
        break;
      }
    }
  }
  fputc('"', out);
}

static void ny_native_print_reason(FILE *out, const char *function_name,
                                   size_t pc, const nyir_inst_t *in,
                                   const char *category, const char *reason) {
  if (!out || !in || !category || !reason) return;
  fputs("optimization_reason {\"function\":", out);
  ny_native_json_string(out, function_name ? function_name : "<fn>");
  fprintf(out, ",\"pc\":%zu,\"op\":", pc);
  ny_native_json_string(out, nyir_op_name(in->op));
  fputs(",\"symbol\":", out);
  if (in->symbol) ny_native_json_string(out, in->symbol); else fputs("null", out);
  fputs(",\"category\":", out); ny_native_json_string(out, category);
  fputs(",\"reason\":", out); ny_native_json_string(out, reason);
  fputs(",\"file\":", out);
  if (in->debug.file) ny_native_json_string(out, in->debug.file); else fputs("null", out);
  fprintf(out, ",\"line\":%u,\"column\":%u,\"block_heat\":%" PRIu64
               ",\"loop_heat\":%" PRIu64 "}\n",
          in->debug.line, in->debug.column, ny_native_profile_block_hot(pc),
          ny_native_profile_loop_hot(pc));
}

static void ny_native_print_optimization_reasons(FILE *out, const char *name,
                                                  const nyir_func_t *f) {
  if (!out || !f) return;
  for (size_t pc = 0; pc < f->len; ++pc) {
    const nyir_inst_t *in = &f->data[pc];
    unsigned effects = nyir_effective_effects(in);
    if (in->op == NYIR_CALL && in->symbol &&
        (strcmp(in->symbol, "rt_native_has_tag") == 0 ||
         strstr(in->symbol, "_any_to_") != NULL))
      ny_native_print_reason(out, name, pc, in, "dynamic",
          "runtime representation/type evidence was not discharged before lowering");
    if (in->op == NYIR_CALL && ny_native_symbol_is_box_conversion(in->symbol))
      ny_native_print_reason(out, name, pc, in, "boxing",
          "a representation or ABI boundary still requires a box/unbox conversion");
    if ((effects & NYIR_EFFECT_ALLOCATION) != 0 && in->op != NYIR_ALLOCA)
      ny_native_print_reason(out, name, pc, in, "allocation",
          "this site still performs a heap allocation after native optimization");
    if (in->op == NYIR_CALL && (!in->symbol || !in->symbol[0]))
      ny_native_print_reason(out, name, pc, in, "devirtualization",
          "the call site has no proven stable direct target");
    if (effects & NYIR_EFFECT_UNKNOWN_SIDE_EFFECT)
      ny_native_print_reason(out, name, pc, in, "effect",
          "unknown external effects conservatively block motion and value reuse");
    if (in->op == NYIR_LOAD_I64 || in->op == NYIR_STORE_I64 ||
        in->op == NYIR_COPY_STRUCT || in->op == NYIR_VEC4_LOAD_F64 ||
        in->op == NYIR_VEC4_STORE_F64 || in->op == NYIR_VEC8_LOAD_F32 ||
        in->op == NYIR_VEC8_STORE_F32 || in->op == NYIR_VEC4_LOAD_I64 ||
        in->op == NYIR_VEC4_STORE_I64 || in->op == NYIR_VEC8_LOAD_I64 ||
        in->op == NYIR_VEC8_STORE_I64 ||
        (in->op == NYIR_CALL &&
         (effects & (NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_WRITE_MEMORY |
                     NYIR_EFFECT_UNKNOWN_SIDE_EFFECT | NYIR_EFFECT_FFI))))
      ny_native_print_reason(out, name, pc, in, "alias",
          "pointer provenance/noalias evidence is not precise enough to prove independence");
  }
}

static void ny_native_print_escape_reasons(FILE *out, const char *name,
                                           const nyir_func_t *f) {
  if (!out || !f)
    return;
  size_t slot_count = nyir_local_slot_count(f);
  if (!slot_count)
    return;
  nyir_local_escape_info_t *info = calloc(slot_count, sizeof(*info));
  if (!info)
    return;
  if (!nyir_analyze_local_escapes(f, info, slot_count)) {
    free(info);
    return;
  }
  for (size_t slot = 0; slot < slot_count; ++slot) {
    const nyir_local_escape_info_t *e = &info[slot];
    if (!e->escapes)
      continue;
    const char *reason = e->thread_escape ? "thread"
                         : e->ffi_escape ? "ffi"
                         : e->unknown_call_escape ? "unknown-call"
                         : e->returned ? "return"
                         : e->stored_to_memory ? "stored-to-memory"
                         : e->passed_to_call ? "call-capture"
                         : "address-taken";
    size_t pc = e->first_escape_pc;
    const nyir_inst_t *in = pc < f->len ? &f->data[pc] : NULL;
    fputs("escape_reason {\"function\":", out);
    ny_native_json_string(out, name ? name : "<fn>");
    fprintf(out, ",\"slot\":%zu,\"pc\":%zu,\"reason\":", slot, pc);
    ny_native_json_string(out, reason);
    fprintf(out,
            ",\"address_taken\":%s,\"passed_to_call\":%s,\"returned\":%s"
            ",\"stored_to_memory\":%s,\"ffi\":%s,\"thread\":%s"
            ",\"unknown_call\":%s,\"file\":",
            e->address_taken ? "true" : "false",
            e->passed_to_call ? "true" : "false",
            e->returned ? "true" : "false",
            e->stored_to_memory ? "true" : "false",
            e->ffi_escape ? "true" : "false",
            e->thread_escape ? "true" : "false",
            e->unknown_call_escape ? "true" : "false");
    if (in && in->debug.file) ny_native_json_string(out, in->debug.file);
    else fputs("null", out);
    fprintf(out, ",\"line\":%u,\"column\":%u}\n",
            in ? in->debug.line : 0, in ? in->debug.column : 0);
  }
  free(info);
}

static void ny_native_tier_facts_add(ny_native_tier_facts_t *facts,
                                     const nyir_func_t *f) {
  if (!facts || !f)
    return;
  facts->insts += f->len;
  facts->vectorize_attempted_loops += f->vectorize_attempted_loops;
  facts->vectorize_rejected_loops += f->vectorize_rejected_loops;
  facts->vectorized_loops += f->vectorized_loops;
  if (f->next_value > 0)
    facts->values += f->next_value;
  for (size_t i = 0; i < f->len; ++i) {
    const nyir_inst_t *in = &f->data[i];
    unsigned effects = nyir_effective_effects(in);
    facts->cost += ny_native_tier_inst_cost(in);
    if (in->op == NYIR_CALL) {
      facts->calls++;
      if (in->symbol && in->symbol[0])
        facts->direct_calls++;
      else
        facts->indirect_calls++;
      if (in->symbol && strncmp(in->symbol, "rt_", 3) == 0)
        facts->runtime_calls++;
      if (in->symbol && strcmp(in->symbol, "rt_native_has_tag") == 0) {
        facts->tag_checks++;
        facts->dynamic_ops++;
      }
      if (in->symbol && strstr(in->symbol, "_any_to_") != NULL)
        facts->dynamic_ops++;
      if (ny_native_symbol_is_box_conversion(in->symbol))
        facts->box_unbox_conversions++;
    } else if (in->op == NYIR_BR || in->op == NYIR_BR_IF) {
      facts->branches++;
    } else if (in->op == NYIR_LOAD_LOCAL || in->op == NYIR_STORE_LOCAL) {
      facts->memory_ops++;
    } else if (in->op == NYIR_BOUNDS_CHECK) {
      facts->bounds_checks++;
    }
    if (in->op == NYIR_DIV_I64 || in->op == NYIR_MOD_I64)
      facts->divmod_ops++;
    if ((effects & (unsigned)NYIR_EFFECT_CONTROL) != 0)
      facts->control_ops++;
    if (effects != 0)
      facts->effect_ops++;
    if ((effects & (unsigned)NYIR_EFFECT_ALLOCATION) != 0 &&
        in->op != NYIR_ALLOCA)
      facts->heap_allocations++;
    if ((effects & (unsigned)NYIR_EFFECT_UNKNOWN_SIDE_EFFECT) != 0)
      facts->unknown_effect_ops++;
    if ((effects & (unsigned)NYIR_EFFECT_IO) != 0)
      facts->io_effect_ops++;
    if ((effects & (unsigned)NYIR_EFFECT_THREAD) != 0)
      facts->thread_effect_ops++;
    if ((effects & (unsigned)NYIR_EFFECT_FFI) != 0)
      facts->ffi_effect_ops++;
    if ((effects & (unsigned)NYIR_EFFECT_FENV) != 0)
      facts->fenv_effect_ops++;
    /*
     * Local-slot traffic has explicit storage identity. Raw pointer memory
     * traffic and unknown calls do not currently carry auditable provenance/
     * noalias evidence in NYIR, so report those sites as unresolved alias
     * boundaries rather than silently folding them into a generic effect count.
     */
    if (in->op == NYIR_LOAD_I64 || in->op == NYIR_STORE_I64 ||
        in->op == NYIR_COPY_STRUCT || in->op == NYIR_VEC4_LOAD_F64 ||
        in->op == NYIR_VEC4_STORE_F64 || in->op == NYIR_VEC8_LOAD_F32 ||
        in->op == NYIR_VEC8_STORE_F32 || in->op == NYIR_VEC4_LOAD_I64 ||
        in->op == NYIR_VEC4_STORE_I64 || in->op == NYIR_VEC8_LOAD_I64 ||
        in->op == NYIR_VEC8_STORE_I64 ||
        (in->op == NYIR_CALL &&
         (effects & ((unsigned)NYIR_EFFECT_READ_MEMORY |
                     (unsigned)NYIR_EFFECT_WRITE_MEMORY |
                     (unsigned)NYIR_EFFECT_UNKNOWN_SIDE_EFFECT)) != 0))
      facts->alias_unresolved_ops++;
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
    const nyir_eval_result_t *profile) {
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

static void ny_native_print_function_regalloc(
    FILE *out, const char *name, const ny_native_regalloc_metrics_t *gpr,
    const ny_native_regalloc_metrics_t *fpr,
    const ny_native_regalloc_metrics_t *vector) {
  if (!out || !gpr || !fpr || !vector)
    return;
  fprintf(out,
          "function_regalloc name=%s "
          "gpr_segments=%zu gpr_colored=%zu gpr_spilled=%zu gpr_reloads=%zu "
          "gpr_peak_live=%zu gpr_hot_loop_segments=%zu "
          "gpr_hot_loop_spilled=%zu gpr_hot_loop_reloads=%zu "
          "gpr_hot_loop_peak_live=%zu fpr_segments=%zu fpr_colored=%zu "
          "fpr_spilled=%zu fpr_reloads=%zu fpr_peak_live=%zu "
          "fpr_hot_loop_segments=%zu fpr_hot_loop_spilled=%zu "
          "fpr_hot_loop_reloads=%zu fpr_hot_loop_peak_live=%zu "
          "vector_segments=%zu vector_colored=%zu vector_spilled=%zu "
          "vector_reloads=%zu vector_peak_live=%zu "
          "vector_hot_loop_segments=%zu vector_hot_loop_spilled=%zu "
          "vector_hot_loop_reloads=%zu vector_hot_loop_peak_live=%zu\n",
          name ? name : "<fn>", gpr->segments, gpr->colored, gpr->spilled,
          gpr->reloads, gpr->peak_live, gpr->hot_loop_segments,
          gpr->hot_loop_spilled, gpr->hot_loop_reloads,
          gpr->hot_loop_peak_live, fpr->segments, fpr->colored, fpr->spilled,
          fpr->reloads, fpr->peak_live, fpr->hot_loop_segments,
          fpr->hot_loop_spilled, fpr->hot_loop_reloads,
          fpr->hot_loop_peak_live, vector->segments, vector->colored,
          vector->spilled, vector->reloads, vector->peak_live,
          vector->hot_loop_segments, vector->hot_loop_spilled,
          vector->hot_loop_reloads, vector->hot_loop_peak_live);
}

static void ny_native_print_static_island(
    FILE *out, const char *name, const ny_native_tier_facts_t *facts,
    bool regalloc_valid, const ny_native_regalloc_metrics_t *gpr,
    const ny_native_regalloc_metrics_t *fpr,
    const ny_native_regalloc_metrics_t *vector) {
  if (!out || !facts)
    return;
  size_t spills = 0, reloads = 0, hot_spills = 0, hot_reloads = 0;
  if (regalloc_valid && gpr && fpr && vector) {
    spills = gpr->spilled + fpr->spilled + vector->spilled;
    reloads = gpr->reloads + fpr->reloads + vector->reloads;
    hot_spills = gpr->hot_loop_spilled + fpr->hot_loop_spilled +
                 vector->hot_loop_spilled;
    hot_reloads = gpr->hot_loop_reloads + fpr->hot_loop_reloads +
                  vector->hot_loop_reloads;
  }
  fprintf(out,
          "static_island name=%s dynamic_ops=%zu tag_checks=%zu box_unbox=%zu "
          "heap_allocations=%zu runtime_calls=%zu bounds_checks=%zu "
          "direct_calls=%zu indirect_calls=%zu unknown_effects=%zu "
          "alias_unresolved=%zu "
          "vector_attempted=%zu vector_rejected=%zu vectorized=%zu "
          "regalloc_known=%s spills=%zu reloads=%zu "
          "hot_loop_spills=%zu hot_loop_reloads=%zu\n",
          name ? name : "<fn>", facts->dynamic_ops, facts->tag_checks,
          facts->box_unbox_conversions, facts->heap_allocations,
          facts->runtime_calls, facts->bounds_checks, facts->direct_calls,
          facts->indirect_calls, facts->unknown_effect_ops,
          facts->alias_unresolved_ops, facts->vectorize_attempted_loops,
          facts->vectorize_rejected_loops,
          facts->vectorized_loops, regalloc_valid ? "yes" : "no", spills,
          reloads, hot_spills, hot_reloads);
}

/*
 * Encode one finalized machine function through the target-owned encoder so
 * opt-in tier reports can attribute actual emitted bytes, not an IR-cost proxy.
 * This work is intentionally report-only and never runs on the normal compile
 * fast path.
 */
static bool ny_native_machine_code_size(const ny_native_target_info_t *target,
                                        const ny_mach_func_t *mach,
                                        const char *symbol,
                                        size_t *out_bytes) {
  if (out_bytes)
    *out_bytes = 0;
  if (!target || !mach || !out_bytes)
    return false;
  if (target->target != NY_NATIVE_TARGET_X86_64 &&
      target->target != NY_NATIVE_TARGET_AARCH64)
    return false;

  ny_obj_buf_t code = {0};
  ny_x64_obj_symbol_def_t *defs = calloc(NY_NATIVE_MAX_DEFS, sizeof(*defs));
  ny_x64_obj_reloc_t *relocs = calloc(NY_X64_OBJ_MAX_RELOCS, sizeof(*relocs));
  if (!defs || !relocs) {
    free(defs);
    free(relocs);
    return false;
  }
  size_t def_count = 0, reloc_count = 0;
  char local_err[256] = {0};
  const char *name = symbol && symbol[0] ? symbol : "tier_probe";
  bool ok = false;
  if (target->target == NY_NATIVE_TARGET_X86_64) {
    ok = ny_x64_mach_build_bundle(mach, NULL, NULL, 0, target, name, false,
                                  &code, defs, &def_count, relocs,
                                  &reloc_count, local_err, sizeof(local_err));
  } else {
    ok = ny_a64_mach_build_bundle(mach, NULL, NULL, 0, target, name, false,
                                  &code, defs, &def_count, relocs,
                                  &reloc_count, local_err, sizeof(local_err));
  }
  if (ok)
    *out_bytes = code.len;
  ny_obj_free(&code);
  free(defs);
  free(relocs);
  return ok;
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
  NY_CAP("nyir-asm", NY_NATIVE_CAP_NIR_ASM);
  NY_CAP("ast-fallback", NY_NATIVE_CAP_AST_FALLBACK);
  NY_CAP("asm-object", NY_NATIVE_CAP_ASM_OBJECT);
  NY_CAP("nyir-vm", NY_NATIVE_CAP_NIR_VM);
  NY_CAP("elf-object", NY_NATIVE_CAP_ELF_OBJECT);
  NY_CAP("coff-object", NY_NATIVE_CAP_COFF_OBJECT);
  NY_CAP("macho-object", NY_NATIVE_CAP_MACHO_OBJECT);
  NY_CAP("live-jit", NY_NATIVE_CAP_LIVE_JIT);
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

  nyir_func_t rt_main = {0};
  nyir_func_t funcs[128];
  const char *func_names[128];
  memset(funcs, 0, sizeof(funcs));
  memset(func_names, 0, sizeof(func_names));
  size_t func_count = 0;
  char local_err[512] = {0};
  bool built = ny_native_build_nir(prog, opt, &rt_main, funcs, &func_count,
                                   func_names, 128, local_err,
                                   sizeof(local_err));
  if (!built) {
    ny_native_set_err(err, err_len, "native tier report: %s",
                      local_err[0] ? local_err : "failed to build NYIR");
    return false;
  }

  nyir_eval_result_t vm_profile = {0};
  bool vm_profile_used = false;
  if (opt->nyir_run_profile && rt_main.len) {
    char profile_err[512] = {0};
    if (ny_native_collect_vm_profile(&rt_main, funcs, func_names, func_count,
                                     opt, &vm_profile, profile_err,
                                     sizeof(profile_err))) {
      vm_profile_used = true;
    } else if (verbose_enabled) {
      fprintf(stderr, "native tier report: VM profile unavailable: %s\n",
              profile_err[0] ? profile_err : NY_NATIVE_UNKNOWN_ERR);
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

  ny_native_regalloc_metrics_t rt_gpr = {0}, rt_fpr = {0}, rt_vector = {0};
  ny_native_regalloc_metrics_t fn_gpr[128] = {{0}};
  ny_native_regalloc_metrics_t fn_fpr[128] = {{0}};
  ny_native_regalloc_metrics_t fn_vector[128] = {{0}};
  bool rt_regalloc_valid = false;
  bool fn_regalloc_valid[128] = {0};
  size_t rt_code_bytes = 0;
  size_t fn_code_bytes[128] = {0};
  bool rt_code_bytes_valid = false;
  bool fn_code_bytes_valid[128] = {0};

  size_t machine_ready = 0;
  size_t machine_total = rt_main.len ? 1 : 0;
  size_t machine_i64 = 0, machine_f32 = 0, machine_f64 = 0;
  size_t machine_ptr = 0, machine_vector = 0, machine_flags = 0;
  char machine_skip[256] = {0};
  if (rt_main.len) {
    ny_mach_func_t machine = {0};
    char machine_err[256] = {0};
    if (ny_mach_lower_nir(&rt_main, &machine, target.caps, machine_err, sizeof(machine_err))) {
      ++machine_ready;
      if (target.target == NY_NATIVE_TARGET_X86_64)
        rt_regalloc_valid = ny_native_x64_regalloc_metrics(
            &machine, &rt_gpr, &rt_fpr, &rt_vector);
      else if (target.target == NY_NATIVE_TARGET_AARCH64)
        rt_regalloc_valid = ny_native_a64_regalloc_metrics(
            &machine, &rt_gpr, &rt_fpr, &rt_vector);
      rt_code_bytes_valid = ny_native_machine_code_size(
          &target, &machine, "rt_main", &rt_code_bytes);
      for (size_t i = 0; i < machine.vreg_len; ++i) {
        switch (machine.vreg_types[i]) {
        case NY_MACH_TYPE_I64: ++machine_i64; break;
        case NY_MACH_TYPE_F32: ++machine_f32; break;
        case NY_MACH_TYPE_F64: ++machine_f64; break;
        case NY_MACH_TYPE_PTR: ++machine_ptr; break;
        case NY_MACH_TYPE_V128_I64:
        case NY_MACH_TYPE_V128_F64:
        case NY_MACH_TYPE_V128_F32: ++machine_vector; break;
        case NY_MACH_TYPE_FLAGS: ++machine_flags; break;
        default: break;
        }
      }
    } else if (!machine_skip[0])
      snprintf(machine_skip, sizeof(machine_skip), "%s",
               machine_err[0] ? machine_err : "unsupported NYIR shape");
    ny_mach_func_free(&machine);
  }
  for (size_t i = 0; i < func_count; ++i) {
    ++machine_total;
    ny_mach_func_t machine = {0};
    char machine_err[256] = {0};
    if (ny_mach_lower_nir(&funcs[i], &machine, target.caps, machine_err, sizeof(machine_err))) {
      ++machine_ready;
      if (target.target == NY_NATIVE_TARGET_X86_64 && i < 128)
        fn_regalloc_valid[i] = ny_native_x64_regalloc_metrics(
            &machine, &fn_gpr[i], &fn_fpr[i], &fn_vector[i]);
      else if (target.target == NY_NATIVE_TARGET_AARCH64 && i < 128)
        fn_regalloc_valid[i] = ny_native_a64_regalloc_metrics(
            &machine, &fn_gpr[i], &fn_fpr[i], &fn_vector[i]);
      if (i < 128)
        fn_code_bytes_valid[i] = ny_native_machine_code_size(
            &target, &machine,
            (func_names[i] && func_names[i][0]) ? func_names[i] : "tier_fn",
            &fn_code_bytes[i]);
      for (size_t v = 0; v < machine.vreg_len; ++v) {
        switch (machine.vreg_types[v]) {
        case NY_MACH_TYPE_I64: ++machine_i64; break;
        case NY_MACH_TYPE_F32: ++machine_f32; break;
        case NY_MACH_TYPE_F64: ++machine_f64; break;
        case NY_MACH_TYPE_PTR: ++machine_ptr; break;
        case NY_MACH_TYPE_V128_I64:
        case NY_MACH_TYPE_V128_F64:
        case NY_MACH_TYPE_V128_F32: ++machine_vector; break;
        case NY_MACH_TYPE_FLAGS: ++machine_flags; break;
        default: break;
        }
      }
    } else if (!machine_skip[0])
      snprintf(machine_skip, sizeof(machine_skip), "%s",
               machine_err[0] ? machine_err : "unsupported NYIR shape");
    ny_mach_func_free(&machine);
  }

  FILE *out = stderr;
  if (opt->native_tier_report_path && opt->native_tier_report_path[0]) {
    ny_native_ensure_parent_dir_for_path(opt->native_tier_report_path);
    out = fopen(opt->native_tier_report_path, "wb");
    if (!out) {
      ny_native_set_err(err, err_len,
                        "native tier report: failed to open %s: %s",
                        opt->native_tier_report_path, strerror(errno));
      nyir_eval_result_free(&vm_profile);
      for (size_t i = 0; i < func_count; ++i)
        nyir_func_free(&funcs[i]);
      nyir_func_free(&rt_main);
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
          "plan budget=%zu hot=%zu cold=%zu cache=%u prefer_vm=%s ast_fallback=%s "
          "requested_tier=%s resolved_tier=%s\n",
          plan.compile_budget, plan.hot_threshold, plan.cold_threshold,
          plan.cache_score, plan.prefer_nir_vm ? "yes" : "no",
          plan.prefer_ast_fallback ? "yes" : "no",
          plan.requested_tier ? plan.requested_tier : "auto",
          plan.resolved_tier ? plan.resolved_tier : "baseline");
  fprintf(out,
          "facts functions=%zu insts=%zu values=%d cost=%zu calls=%zu "
          "runtime_calls=%zu dynamic_ops=%zu tag_checks=%zu box_unbox=%zu "
          "heap_allocations=%zu direct_calls=%zu indirect_calls=%zu "
          "branches=%zu locals=%zu bounds_checks=%zu "
          "divmod=%zu control=%zu effects=%zu unknown_effects=%zu "
          "io_effects=%zu thread_effects=%zu ffi_effects=%zu fenv_effects=%zu "
          "alias_unresolved=%zu "
          "vector_attempted=%zu vector_rejected=%zu vectorized=%zu\n",
          func_count + (rt_main.len ? 1u : 0u), facts.insts, facts.values,
          facts.cost, facts.calls, facts.runtime_calls, facts.dynamic_ops,
          facts.tag_checks, facts.box_unbox_conversions, facts.heap_allocations,
          facts.direct_calls, facts.indirect_calls, facts.branches,
          facts.memory_ops, facts.bounds_checks, facts.divmod_ops,
          facts.control_ops, facts.effect_ops, facts.unknown_effect_ops,
          facts.io_effect_ops, facts.thread_effect_ops, facts.ffi_effect_ops,
          facts.fenv_effect_ops, facts.alias_unresolved_ops, facts.vectorize_attempted_loops,
          facts.vectorize_rejected_loops,
          facts.vectorized_loops);
  fprintf(out,
          "handoffs entries=%zu returns=%zu calls=%zu branches=%zu labels=%zu "
          "deopt_safe=%zu\n",
          handoffs.entry_points, handoffs.return_points, handoffs.call_points,
          handoffs.branch_points, handoffs.label_points,
          handoffs.deopt_safe_points);
  size_t machine_code_bytes = rt_code_bytes_valid ? rt_code_bytes : 0;
  size_t machine_code_functions = rt_code_bytes_valid ? 1 : 0;
  size_t specialization_code_bytes = 0;
  size_t specialization_code_functions = 0;
  size_t specialization_max_function_bytes = 0;
  for (size_t i = 0; i < func_count && i < 128; ++i) {
    if (!fn_code_bytes_valid[i])
      continue;
    machine_code_bytes += fn_code_bytes[i];
    ++machine_code_functions;
    if (func_names[i] && strstr(func_names[i], "__ny_mono_")) {
      specialization_code_bytes += fn_code_bytes[i];
      if (fn_code_bytes[i] > specialization_max_function_bytes)
        specialization_max_function_bytes = fn_code_bytes[i];
      ++specialization_code_functions;
    }
  }
  fprintf(out,
          "machine_lowering ready=%zu total=%zu code_bytes=%zu code_functions=%zu "
          "specialization_code_bytes=%zu specialization_code_functions=%zu "
          "specialization_max_function_bytes=%zu "
          "types=i64:%zu,f32:%zu,f64:%zu,ptr:%zu,v128:%zu,flags:%zu%s%s\n",
          machine_ready, machine_total, machine_code_bytes,
          machine_code_functions, specialization_code_bytes,
          specialization_code_functions, specialization_max_function_bytes,
          machine_i64, machine_f32, machine_f64,
          machine_ptr, machine_vector, machine_flags,
          machine_skip[0] ? " first_skip=" : "",
          machine_skip[0] ? machine_skip : "");
  {
    size_t supported = 0, total = 0;
    unsigned long long mach_ok = 0, nir_fb = 0;
    unsigned long long ra_segments = 0, ra_colored = 0, ra_spilled = 0;
    unsigned long long ra_reloads = 0, ra_peak_live = 0;
    unsigned long long fp_segments = 0, fp_colored = 0, fp_spilled = 0;
    unsigned long long fp_reloads = 0, fp_peak_live = 0;
    unsigned long long vec_segments = 0, vec_colored = 0, vec_spilled = 0;
    unsigned long long vec_reloads = 0, vec_peak_live = 0;
    char fallback_detail[512] = {0};
    ny_mach_opcode_coverage(&supported, &total);
    ny_native_mach_encode_stats(&mach_ok, &nir_fb);
    ny_native_mach_encode_fallback_detail(fallback_detail,
                                          sizeof(fallback_detail));
    ny_native_mach_regalloc_stats(&ra_segments, &ra_colored, &ra_spilled,
                                  &ra_reloads, &ra_peak_live);
    ny_native_mach_fpr_stats(&fp_segments, &fp_colored, &fp_spilled,
                             &fp_reloads, &fp_peak_live);
    ny_native_mach_vector_stats(&vec_segments, &vec_colored, &vec_spilled,
                                &vec_reloads, &vec_peak_live);
    fprintf(out, "mach_opcode_coverage supported=%zu total=%zu fallback=%zu\n",
            supported, total, total - supported);
    fprintf(out,
            "mach_encode mach_functions=%llu nir_fallback_functions=%llu "
            "first_fallback=%s\n",
            mach_ok, nir_fb, fallback_detail);
    fprintf(out,
            "mach_regalloc segments=%llu colored=%llu spilled=%llu "
            "reloads=%llu peak_live=%llu\n",
            ra_segments, ra_colored, ra_spilled, ra_reloads, ra_peak_live);
    fprintf(out,
            "mach_fpr_regalloc segments=%llu colored=%llu spilled=%llu "
            "reloads=%llu peak_live=%llu\n",
            fp_segments, fp_colored, fp_spilled, fp_reloads, fp_peak_live);
    fprintf(out,
            "mach_vector_regalloc segments=%llu colored=%llu spilled=%llu "
            "reloads=%llu peak_live=%llu\n",
            vec_segments, vec_colored, vec_spilled, vec_reloads,
            vec_peak_live);
  }
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
            "runtime_calls=%zu dynamic_ops=%zu tag_checks=%zu box_unbox=%zu "
            "heap_allocations=%zu direct_calls=%zu indirect_calls=%zu "
            "branches=%zu locals=%zu bounds_checks=%zu "
            "divmod=%zu control=%zu effects=%zu unknown_effects=%zu "
            "io_effects=%zu thread_effects=%zu ffi_effects=%zu fenv_effects=%zu "
            "alias_unresolved=%zu vector_attempted=%zu vector_rejected=%zu "
            "vectorized=%zu "
            "handoffs=%zu deopt_safe=%zu recommend=%s\n",
            rt_facts.insts, rt_facts.values, rt_facts.cost, rt_facts.calls,
            rt_facts.runtime_calls, rt_facts.dynamic_ops, rt_facts.tag_checks,
            rt_facts.box_unbox_conversions, rt_facts.heap_allocations,
            rt_facts.direct_calls, rt_facts.indirect_calls, rt_facts.branches,
            rt_facts.memory_ops, rt_facts.bounds_checks, rt_facts.divmod_ops,
            rt_facts.control_ops, rt_facts.effect_ops, rt_facts.unknown_effect_ops,
            rt_facts.io_effect_ops, rt_facts.thread_effect_ops,
            rt_facts.ffi_effect_ops, rt_facts.fenv_effect_ops,
            rt_facts.alias_unresolved_ops, rt_facts.vectorize_attempted_loops,
            rt_facts.vectorize_rejected_loops, rt_facts.vectorized_loops,
            rt_handoffs.entry_points + rt_handoffs.return_points +
                rt_handoffs.call_points + rt_handoffs.branch_points +
                rt_handoffs.label_points,
            rt_handoffs.deopt_safe_points,
            ny_native_tier_recommendation_with_profile(
                &plan, &target, &rt_facts,
                vm_profile_used ? &vm_profile : NULL));
    if (rt_code_bytes_valid)
      fprintf(out, "function_code name=rt_main bytes=%zu\n", rt_code_bytes);
    if (rt_regalloc_valid)
      ny_native_print_function_regalloc(out, "rt_main", &rt_gpr, &rt_fpr,
                                        &rt_vector);
    ny_native_print_static_island(out, "rt_main", &rt_facts,
                                  rt_regalloc_valid, &rt_gpr, &rt_fpr,
                                  &rt_vector);
    ny_native_print_optimization_reasons(out, "rt_main", &rt_main);
    ny_native_print_escape_reasons(out, "rt_main", &rt_main);
  }

  for (size_t func_index = 0; func_index < func_count; ++func_index) {
    const char *name = func_names[func_index] && func_names[func_index][0]
                           ? func_names[func_index]
                           : "<fn>";
    ny_native_tier_facts_t fn_facts = {0};
    ny_native_tier_facts_add(&fn_facts, &funcs[func_index]);
    ny_native_handoff_summary_t fn_handoffs = {0};
    ny_native_handoff_summary(&funcs[func_index], &fn_handoffs);
    fprintf(out,
            "function name=%s insts=%zu values=%d cost=%zu calls=%zu "
            "runtime_calls=%zu dynamic_ops=%zu tag_checks=%zu box_unbox=%zu "
            "heap_allocations=%zu direct_calls=%zu indirect_calls=%zu "
            "branches=%zu locals=%zu bounds_checks=%zu "
            "divmod=%zu control=%zu effects=%zu unknown_effects=%zu "
            "io_effects=%zu thread_effects=%zu ffi_effects=%zu fenv_effects=%zu "
            "alias_unresolved=%zu vector_attempted=%zu vector_rejected=%zu "
            "vectorized=%zu "
            "handoffs=%zu deopt_safe=%zu recommend=%s\n",
            name, fn_facts.insts, fn_facts.values, fn_facts.cost,
            fn_facts.calls, fn_facts.runtime_calls, fn_facts.dynamic_ops,
            fn_facts.tag_checks, fn_facts.box_unbox_conversions,
            fn_facts.heap_allocations, fn_facts.direct_calls,
            fn_facts.indirect_calls, fn_facts.branches, fn_facts.memory_ops,
            fn_facts.bounds_checks, fn_facts.divmod_ops, fn_facts.control_ops,
            fn_facts.effect_ops, fn_facts.unknown_effect_ops,
            fn_facts.io_effect_ops, fn_facts.thread_effect_ops,
            fn_facts.ffi_effect_ops, fn_facts.fenv_effect_ops,
            fn_facts.alias_unresolved_ops, fn_facts.vectorize_attempted_loops,
            fn_facts.vectorize_rejected_loops,
            fn_facts.vectorized_loops,
            fn_handoffs.entry_points + fn_handoffs.return_points +
                fn_handoffs.call_points + fn_handoffs.branch_points +
                fn_handoffs.label_points,
            fn_handoffs.deopt_safe_points,
            ny_native_tier_recommendation(&plan, &target, &fn_facts));
    if (func_index < 128 && fn_code_bytes_valid[func_index]) {
      fprintf(out, "function_code name=%s bytes=%zu\n", name,
              fn_code_bytes[func_index]);
      if (strstr(name, "__ny_mono_"))
        fprintf(out, "specialization_code name=%s bytes=%zu\n", name,
                fn_code_bytes[func_index]);
    }
    if (func_index < 128 && fn_regalloc_valid[func_index])
      ny_native_print_function_regalloc(
          out, name, &fn_gpr[func_index], &fn_fpr[func_index],
          &fn_vector[func_index]);
    ny_native_print_static_island(
        out, name, &fn_facts, func_index < 128 && fn_regalloc_valid[func_index],
        func_index < 128 ? &fn_gpr[func_index] : NULL,
        func_index < 128 ? &fn_fpr[func_index] : NULL,
        func_index < 128 ? &fn_vector[func_index] : NULL);
    ny_native_print_optimization_reasons(out, name, &funcs[func_index]);
    ny_native_print_escape_reasons(out, name, &funcs[func_index]);
  }

  if (out != stderr)
    fclose(out);
  nyir_eval_result_free(&vm_profile);
  for (size_t i = 0; i < func_count; ++i)
    nyir_func_free(&funcs[i]);
  nyir_func_free(&rt_main);
  if (err && err_len > 0)
    err[0] = '\0';
  return true;
}
