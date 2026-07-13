#include "code/native/internal.h"
#include "base/common.h"
#include "base/time.h"
#include "base/util.h"
#include "wire/build.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Native emission orchestration: target dispatch, assembly output, internal
 * object selection, and the explicit external-assembler fallback. */
static bool ny_native_target_has(const ny_native_target_info_t *target,
                                 ny_native_target_cap_t cap) {
  return target && (target->caps & (unsigned)cap) != 0;
}

size_t ny_native_nir_local_count(const ny_nir_func_t *f) {
  int64_t max_slot = -1;
  for (size_t i = 0; f && i < f->len; ++i) {
    const ny_nir_inst_t *in = &f->data[i];
    if ((in->op == NY_NIR_LOAD_LOCAL || in->op == NY_NIR_STORE_LOCAL ||
         in->op == NYIR_ADDR_LOCAL) &&
        in->imm > max_slot)
      max_slot = in->imm;
  }
  return max_slot >= 0 ? (size_t)max_slot + 1 : 0;
}

bool ny_native_ensure_parent_dir_for_path(const char *path) {
  if (!path || !*path)
    return true;
  char tmp[4096];
  snprintf(tmp, sizeof(tmp), "%s", path);
  char *slash = strrchr(tmp, '/');
#ifdef _WIN32
  char *bslash = strrchr(tmp, '\\');
  if (!slash || (bslash && bslash > slash))
    slash = bslash;
#endif
  if (!slash || slash == tmp)
    return true;
  *slash = '\0';
  ny_ensure_dir_recursive(tmp);
  return true;
}

bool ny_native_emit_nir_func(ny_native_writer_t *w,
                                    const ny_native_target_info_t *target,
                                    const ny_nir_func_t *nir,
                                    const char *label, bool tag_return,
                                    char *err, size_t err_len) {
  if (!target)
    return false;
  switch (target->target) {
  case NY_NATIVE_TARGET_X86_64:
    return ny_native_x86_64_emit_nir(w, target, nir, label, tag_return, err,
                                     err_len);
  case NY_NATIVE_TARGET_AARCH64:
    return ny_native_aarch64_emit_nir(w, target, nir, label, tag_return, err,
                                      err_len);
  case NY_NATIVE_TARGET_X86:
    return ny_native_i386_emit_nir(w, target, nir, label, tag_return, err,
                                   err_len);
  case NY_NATIVE_TARGET_ARM:
    return ny_native_arm_emit_nir(w, target, nir, label, tag_return, err,
                                  err_len);
  case NY_NATIVE_TARGET_RISCV:
    return ny_native_riscv_emit_nir(w, target, nir, label, tag_return, err,
                                    err_len);
  case NY_NATIVE_TARGET_BPF:
    return ny_native_bpf_emit_nir(w, target, nir, label, tag_return, err,
                                  err_len);
  case NY_NATIVE_TARGET_MIPS:
    return ny_native_mips_emit_nir(w, target, nir, label, tag_return, err,
                                   err_len);
  case NY_NATIVE_TARGET_POWERPC:
    return ny_native_powerpc_emit_nir(w, target, nir, label, tag_return, err,
                                      err_len);
  case NY_NATIVE_TARGET_AVR:
    return ny_native_avr_emit_nir(w, target, nir, label, tag_return, err,
                                  err_len);
  case NY_NATIVE_TARGET_WASM:
    return ny_native_wasm_emit_nir(w, target, nir, label, tag_return, err,
                                   err_len);
  default:
    ny_native_set_err(err, err_len,
                      "native backend target '%s' has no NYIR emitter",
                      target->target_name ? target->target_name : "unknown");
    return false;
  }
}

bool ny_native_emit_asm_entry(const program_t *prog, const ny_options *opt,
                              const char *path, const char *entry_name,
                              bool tag_return, char *err, size_t err_len) {
  ny_native_target_info_t target;
  if (!ny_native_target_info_init(&target, opt)) {
    ny_native_set_err(err, err_len, "native backend is not enabled");
    return false;
  }
  bool target_has_ast_fallback =
      ny_native_target_has(&target, NY_NATIVE_CAP_AST_FALLBACK);
  if (!ny_native_target_has(&target, NY_NATIVE_CAP_NIR_ASM)) {
    ny_native_set_err(err, err_len,
                      "native backend target '%s' is registered (abi=%s object=%s ptr=%zub) but no emitter is implemented yet",
                      target.target_name, target.abi_name,
                      target.object_format, target.pointer_bits);
    return false;
  }
  if (!opt->native_dump_ir_path && !opt->nyir_dump_bin_path &&
      !ny_native_dump_ir_for_program(prog, opt, err, err_len))
    return false;

  /* Try the NYIR-first codegen path: build, optimize, verify, emit from IR. */
  ny_nir_func_t rt_main_nir = {0};
  ny_nir_func_t func_nirs[64];
  size_t func_count = 0;
  char nir_err[512] = {0};
  bool nir_ok = ny_native_build_nir(prog, opt, &rt_main_nir, func_nirs,
                                     &func_count, 64, nir_err, sizeof(nir_err));

  ny_native_writer_t w = {0};
  bool ok = false;

  if (nir_ok && rt_main_nir.len > 0) {
    /* Emit header comment. */
    if (!ny_native_printf(&w, "# Nytrix native %s backend output (NYIR path)\n",
                          target.target_name))
      nir_ok = false;
    if (nir_ok &&
        !ny_native_printf(&w,
                          "# target=%s abi=%s object=%s ptr=%zub red_zone=%s shadow_space=%zu\n",
                          target.target_name, target.abi_name,
                          target.object_format, target.pointer_bits,
                          target.red_zone ? "yes" : "no",
                          target.shadow_space_bytes))
      nir_ok = false;
    ny_native_tier_plan_t tier = {0};
    if (nir_ok && ny_native_tier_plan_init(&tier, &target, opt) &&
        !ny_native_printf(&w,
                          "# tier budget=%zu hot=%zu cold=%zu cache=%u vm=%s ast_fallback=%s\n",
                          tier.compile_budget, tier.hot_threshold,
                          tier.cold_threshold, tier.cache_score,
                          tier.prefer_nir_vm ? "yes" : "no",
                          tier.prefer_ast_fallback ? "yes" : "no"))
      nir_ok = false;

    /* Emit user functions (raw return, no tagging). */
    if (nir_ok) {
      for (size_t i = 0; i < func_count; ++i) {
        const char *fn_name = NULL;
        /* Find the function name from the program AST. */
        for (size_t j = 0; j < prog->body.len; ++j) {
          const stmt_t *s = prog->body.data[j];
          if (s && s->kind == NY_S_FUNC && s->as.fn.name) {
            /* Match by order — the NYIR builder processes them in the same
             * order as the program body. */
            if (fn_name == NULL) {
              /* Count functions before position j that are FUNC. */
              size_t func_idx = 0;
              for (size_t k = 0; k < j; ++k) {
                if (prog->body.data[k] && prog->body.data[k]->kind == NY_S_FUNC)
                  func_idx++;
              }
              if (func_idx == i) {
                fn_name = s->as.fn.name;
                break;
              }
            }
          }
        }
        if (!fn_name)
          fn_name = "unknown_fn";
        /* Build the native label: ny_fn_<name>. */
        char label[256];
        snprintf(label, sizeof(label), "ny_fn_%s", fn_name);
        bool emitted = ny_native_emit_nir_func(&w, &target, &func_nirs[i],
                                               label, false, err, err_len);
        if (!emitted) {
          nir_ok = false;
          break;
        }
      }
    }

    /* Emit the top-level entry. */
    if (nir_ok) {
      const char *top_name = entry_name && entry_name[0] ? entry_name : "rt_main";
      bool emitted = ny_native_emit_nir_func(&w, &target, &rt_main_nir,
                                             top_name, tag_return, err,
                                             err_len);
      if (!emitted)
        nir_ok = false;
    }

    if (nir_ok)
      ok = ny_write_file(path, w.data ? w.data : "", w.len) == 0;

    if (verbose_enabled >= 1 && nir_ok) {
      fprintf(stderr, "native asm: %zu functions + %s (%zu NYIR insts total)"
                      " -> %s (%zu bytes)\n",
              func_count, entry_name && entry_name[0] ? entry_name : "rt_main",
              rt_main_nir.len, path ? path : "(stdout)", w.len);
    }

    /* Clean up NYIR. */
    ny_nir_func_free(&rt_main_nir);
    for (size_t i = 0; i < func_count; ++i)
      ny_nir_func_free(&func_nirs[i]);
  }

  if (!nir_ok || !ok) {
    /* NYIR path failed or produced no output; only x86-64 has AST fallback. */
    ny_nir_func_free(&rt_main_nir);
    for (size_t i = 0; i < func_count; ++i)
      ny_nir_func_free(&func_nirs[i]);
    free(w.data);
    w = (ny_native_writer_t){0};
    if (!target_has_ast_fallback) {
      if (err && err_len > 0 && err[0] == '\0')
        ny_native_set_err(err, err_len,
                          "native %s backend requires NYIR-supported input",
                          target.target_name);
      return false;
    }
    if (entry_name && entry_name[0] && strcmp(entry_name, "rt_main") != 0) {
      if (err && err_len > 0 && err[0] == '\0')
        ny_native_set_err(err, err_len,
                          "native NYIR path failed before executable entry emission");
      return false;
    }
    if (verbose_enabled >= 1)
      fprintf(stderr, "native asm: NYIR path failed, falling back to AST\n");
    char fallback_err[512] = {0};
    ok = ny_native_x86_64_emit_rt_main(&w, &target, prog, fallback_err,
                                       sizeof(fallback_err));
    if (ok)
      ok = ny_write_file(path, w.data ? w.data : "", w.len) == 0;
    if (!ok && err && err_len > 0 && err[0] == '\0')
      ny_native_set_err(err, err_len, "%s",
                        fallback_err[0] ? fallback_err
                                        : "native AST fallback failed");
    free(w.data);
    return ok;
  }

  if (!ok && err && err_len > 0 && err[0] == '\0')
    ny_native_set_err(err, err_len, "failed to write native assembly to %s: %s",
                      path ? path : "(null)", strerror(errno));
  free(w.data);
  return ok;
}

bool ny_native_emit_asm(const program_t *prog, const ny_options *opt,
                        const char *path, char *err, size_t err_len) {
  return ny_native_emit_asm_entry(prog, opt, path, "rt_main", true, err,
                                  err_len);
}

bool ny_native_emit_object(const program_t *prog, const ny_options *opt,
                           const char *path, const char *entry_name,
                           bool tag_return, char *err, size_t err_len) {
  if (!prog || !opt || !path || !*path) {
    ny_native_set_err(err, err_len,
                      "native object emission: missing input or output path");
    return false;
  }
  ny_native_target_info_t target;
  if (!ny_native_target_info_init(&target, opt)) {
    ny_native_set_err(err, err_len, "native object emission: backend disabled");
    return false;
  }
  if (!ny_native_target_has(&target, NY_NATIVE_CAP_ASM_OBJECT)) {
    ny_native_set_err(err, err_len,
                      "native object emission for target '%s' is not enabled yet; use --emit-asm for assembly output",
                      target.target_name ? target.target_name : "unknown");
    return false;
  }
  if (ny_native_target_has(&target, NY_NATIVE_CAP_ELF_OBJECT) ||
      ny_native_target_has(&target, NY_NATIVE_CAP_COFF_OBJECT) ||
      ny_native_target_has(&target, NY_NATIVE_CAP_MACHO_OBJECT)) {
    ny_nir_func_t rt_main_nir = {0};
    ny_nir_func_t func_nirs[64] = {{0}};
    size_t func_count = 0;
    char nir_err[512] = {0};
    if (ny_native_build_nir(prog, opt, &rt_main_nir, func_nirs, &func_count,
                            64, nir_err, sizeof(nir_err)) &&
        rt_main_nir.len > 0) {
      const char *obj_symbol =
          entry_name && entry_name[0] ? entry_name : "rt_main";
      const char *func_names[64] = {0};
      size_t name_count = 0;
      for (size_t i = 0; prog && i < prog->body.len && name_count < func_count; ++i) {
        const stmt_t *s = prog->body.data[i];
        if (s && s->kind == NY_S_FUNC)
          func_names[name_count++] = s->as.fn.name ? s->as.fn.name : "unknown_fn";
      }
      char obj_err[512] = {0};
      bool obj_ok = false;
      if (ny_native_target_has(&target, NY_NATIVE_CAP_ELF_OBJECT)) {
        if (target.target == NY_NATIVE_TARGET_X86) {
          obj_ok = ny_native_emit_elf32_i386_object_from_nirs(
              &rt_main_nir, func_nirs, func_names, func_count, &target, path,
              obj_symbol, tag_return, obj_err, sizeof(obj_err));
        } else if (target.target == NY_NATIVE_TARGET_AARCH64) {
          obj_ok = ny_native_emit_elf64_aarch64_object_from_nirs(
              &rt_main_nir, func_nirs, func_names, func_count, &target, path,
              obj_symbol, tag_return, obj_err, sizeof(obj_err));
        } else {
          obj_ok = ny_native_emit_elf64_object_from_nirs(
              &rt_main_nir, func_nirs, func_names, func_count, &target, path,
              obj_symbol, tag_return, obj_err, sizeof(obj_err));
        }
      } else if (ny_native_target_has(&target, NY_NATIVE_CAP_COFF_OBJECT)) {
        obj_ok = ny_native_emit_coff_x64_object_from_nirs(
            &rt_main_nir, func_nirs, func_names, func_count, &target, path,
            obj_symbol, tag_return, obj_err, sizeof(obj_err));
      } else if (ny_native_target_has(&target, NY_NATIVE_CAP_MACHO_OBJECT)) {
        obj_ok = ny_native_emit_macho_x64_object_from_nirs(
            &rt_main_nir, func_nirs, func_names, func_count, &target, path,
            obj_symbol, tag_return, obj_err, sizeof(obj_err));
      }
      if (obj_ok) {
        for (size_t i = 0; i < func_count; ++i)
          ny_nir_func_free(&func_nirs[i]);
        ny_nir_func_free(&rt_main_nir);
        if (err && err_len > 0)
          err[0] = '\0';
        return true;
      }
      if (target.target == NY_NATIVE_TARGET_X86 &&
          ny_native_target_has(&target, NY_NATIVE_CAP_ELF_OBJECT)) {
        ny_native_set_err(err, err_len, "%s",
                          obj_err[0] ? obj_err
                                     : "i386 ELF object writer failed");
        for (size_t i = 0; i < func_count; ++i)
          ny_nir_func_free(&func_nirs[i]);
        ny_nir_func_free(&rt_main_nir);
        return false;
      }
    }
    for (size_t i = 0; i < func_count; ++i)
      ny_nir_func_free(&func_nirs[i]);
    ny_nir_func_free(&rt_main_nir);
    if (err && err_len > 0)
      err[0] = '\0';
  }
  char asm_path[4096];
  char asm_name[96];
  snprintf(asm_name, sizeof(asm_name), "ny_native_%ld_%llu.s", (long)getpid(),
           (unsigned long long)ny_ticks_now());
  ny_join_path(asm_path, sizeof(asm_path), ny_get_temp_dir(), asm_name);
  if (!ny_native_emit_asm_entry(prog, opt, asm_path, entry_name, tag_return,
                                err, err_len))
    return false;
  ny_native_ensure_parent_dir_for_path(path);
  const char *cc = ny_builder_choose_cc();
  const char *argv[] = {cc, "-c", asm_path, "-o", path, NULL};
  int rc = ny_exec_spawn(argv);
  unlink(asm_path);
  if (rc != 0) {
    ny_native_set_err(err, err_len,
                      "native object emission: assembler failed with rc=%d", rc);
    return false;
  }
  struct stat st;
  if (stat(path, &st) != 0 || st.st_size <= 0) {
    ny_native_set_err(err, err_len,
                      "native object emission: assembler produced no object");
    return false;
  }
  if (err && err_len > 0)
    err[0] = '\0';
  return true;
}
