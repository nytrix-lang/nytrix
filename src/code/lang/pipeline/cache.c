/*
 * Pipeline cache helpers: debug-dump utilities for the compiler
 * pipeline that write source and IR on diagnostic/error conditions.
 */
static void dump_debug_bundle(const ny_options *opt, const char *user_source,
                              LLVMModuleRef module, const char *stage,
                              const char *reason) {
  if (!opt || !opt->dump_on_error)
    return;
  ny_ensure_dir_recursive(ny_dump_dir(opt));
  char src_path[4096];
  char ir_path[4096];
  char asm_path[4096];
  char stats_path[4096];
  char manifest_path[4096];
  ny_dump_path(src_path, sizeof(src_path), opt, "last_source.ny");
  ny_dump_path(ir_path, sizeof(ir_path), opt, "last_ir.ll");
  ny_dump_path(asm_path, sizeof(asm_path), opt, "last_asm.s");
  ny_dump_path(stats_path, sizeof(stats_path), opt, "last_stats.txt");
  ny_dump_path(manifest_path, sizeof(manifest_path), opt, "last_manifest.txt");

  /*
   * A failed parse can leave an old module from a previous run in build/debug.
   * Remove unavailable products so users cannot mistake stale IR for this run.
   */
  (void)remove(ir_path);
  (void)remove(asm_path);
  (void)remove(stats_path);
  (void)remove(manifest_path);
  bool source_ok = user_source && ny_write_file(src_path, user_source,
                                                strlen(user_source)) == 0;
  bool ir_ok = false;
  bool asm_ok = false;
  bool stats_ok = false;
  if (module) {
    LLVMModuleRef dump_mod = ny_prepare_ir_dump_module(opt, module);
    LLVMModuleRef artifact_module = dump_mod ? dump_mod : module;
    char *err = NULL;
    ir_ok = LLVMPrintModuleToFile(artifact_module, ir_path, &err) == 0;
    if (!ir_ok) {
      if (err) {
        NY_LOG_ERR("Failed to write IR dump: %s\n", err);
        LLVMDisposeMessage(err);
      }
    }
    asm_ok = ny_llvm_emit_file(artifact_module, asm_path, LLVMAssemblyFile,
                               opt->opt_level);
    ny_write_ir_stats_file(opt, "last_stats.txt", artifact_module);
    stats_ok = ny_access(stats_path, R_OK) == 0;
    if (dump_mod)
      LLVMDisposeModule(dump_mod);
  }
  char manifest[4096];
  int manifest_len = snprintf(
      manifest, sizeof(manifest),
      "input=%s\nstage=%s\nreason=%s\nbackend=%s\nopt_level=%d\n"
      "source=%s\nir=%s\nassembly=%s\nstats=%s\n",
      opt->input_file ? opt->input_file : "<inline>", stage ? stage : "unknown",
      reason ? reason : "unspecified",
      opt->native_backend_raw ? opt->native_backend_raw : "llvm",
      opt->opt_level, source_ok ? "written" : "failed-or-unavailable",
      ir_ok ? "written" : (module ? "failed" : "unavailable-before-codegen"),
      asm_ok ? "written" : (module ? "failed" : "unavailable-before-codegen"),
      stats_ok ? "written"
               : (module ? "failed" : "unavailable-before-codegen"));
  bool manifest_ok =
      manifest_len > 0 && (size_t)manifest_len < sizeof(manifest) &&
      ny_write_file(manifest_path, manifest, (size_t)manifest_len) == 0;
  if (ny_dbg_json_enabled()) {
    ny_dbg_heuristic_event(
        "DEBUG_ARTIFACTS", "%s path=%s manifest=%s",
        (source_ok || ir_ok || asm_ok || stats_ok || manifest_ok)
            ? "written"
            : "unavailable",
        ny_dump_dir(opt), manifest_ok ? "last_manifest.txt" : "unavailable");
    return;
  }
  if (source_ok || ir_ok || asm_ok || stats_ok || manifest_ok) {
    NY_LOG_ERR("Debug artifacts: %s/ (see last_manifest.txt)\n",
               ny_dump_dir(opt));
  } else {
    NY_LOG_ERR("Could not write debug artifacts under %s/\n", ny_dump_dir(opt));
  }
  {
    const size_t max_lines = 14;
    const char *paths[] = {ir_path, asm_path};
    const char *labels[] = {"IR snippet", "ASM snippet"};
    for (size_t i = 0; i < 2; i++) {
      char *content = ny_read_file(paths[i]);
      if (!content)
        continue;
      NY_LOG_ERR("--- %s (%s) ---\n", labels[i], paths[i]);
      size_t lines = 0;
      for (char *p = content; *p && lines < max_lines; p++) {
        fputc(*p, stderr);
        if (*p == '\n')
          lines++;
      }
      if (lines >= max_lines)
        NY_LOG_ERR("...\n");
      free(content);
    }
  }
}

#ifdef _WIN32
static const char *ny_windows_output_path(const char *raw, char *buf,
                                          size_t buflen) {
  if (!raw || !*raw)
    return raw;
  const char *base = strrchr(raw, '\\');
  const char *slash = strrchr(raw, '/');
  if (!base || (slash && slash > base))
    base = slash;
  base = base ? base + 1 : raw;
  const char *dot = strrchr(base, '.');
  if (dot && dot[1] != '\0')
    return raw;
  if (snprintf(buf, buflen, "%s.exe", raw) >= (int)buflen)
    return buf;
  return buf;
}
#endif

static LLVMCodeGenOptLevel ny_jit_codegen_opt_level(const ny_options *opt)
    __attribute__((unused));
static LLVMCodeGenOptLevel ny_jit_codegen_opt_level(const ny_options *opt) {
  const char *raw = getenv("NYTRIX_JIT_CODEGEN_OPT");
  if (!raw || !*raw)
    raw = getenv("NYTRIX_JIT_OPT_LEVEL");
  if (raw && *raw) {
    if (strcmp(raw, "0") == 0 || strcasecmp(raw, "o0") == 0 ||
        strcasecmp(raw, "none") == 0)
      return LLVMCodeGenLevelNone;
    if (strcmp(raw, "1") == 0 || strcasecmp(raw, "o1") == 0 ||
        strcasecmp(raw, "less") == 0)
      return LLVMCodeGenLevelLess;
    if (strcmp(raw, "2") == 0 || strcasecmp(raw, "o2") == 0 ||
        strcasecmp(raw, "default") == 0)
      return LLVMCodeGenLevelDefault;
    if (strcmp(raw, "3") == 0 || strcasecmp(raw, "o3") == 0 ||
        strcasecmp(raw, "aggressive") == 0)
      return LLVMCodeGenLevelAggressive;
  }
  ny_opt_profile_kind_t profile_kind = ny_opt_profile_kind_from_env();
  switch (profile_kind) {
  case NY_OPT_PROFILE_NONE:
  case NY_OPT_PROFILE_COMPILE:
    return LLVMCodeGenLevelNone;
  case NY_OPT_PROFILE_SIZE:
    return LLVMCodeGenLevelLess;
  case NY_OPT_PROFILE_PEAK:
  case NY_OPT_PROFILE_SPEED:
    return LLVMCodeGenLevelAggressive;
  case NY_OPT_PROFILE_BALANCED:
    return LLVMCodeGenLevelDefault;
  case NY_OPT_PROFILE_CUSTOM:
  case NY_OPT_PROFILE_DEFAULT:
  default:
    break;
  }
  int level = opt ? opt->opt_level : 2;
  if (level <= 0)
    return LLVMCodeGenLevelNone;
  if (level == 1)
    return LLVMCodeGenLevelLess;
  if (level >= 3)
    return LLVMCodeGenLevelAggressive;
  return LLVMCodeGenLevelDefault;
}

static bool ny_jit_triple_is_apple_arm64(const char *triple) {
  if (!triple || !*triple)
    return false;
  bool apple = strstr(triple, "apple") || strstr(triple, "darwin") ||
               strstr(triple, "macos");
  bool arm64 = strstr(triple, "arm64") || strstr(triple, "aarch64");
  return apple && arm64;
}

static bool ny_jit_module_is_apple_arm64(LLVMModuleRef module) {
  const char *triple = module ? LLVMGetTarget(module) : NULL;
  if (triple && *triple)
    return ny_jit_triple_is_apple_arm64(triple);
  const char *env_triple = getenv("NYTRIX_HOST_TRIPLE");
  if (env_triple && *env_triple)
    return ny_jit_triple_is_apple_arm64(env_triple);
  char *default_triple = LLVMGetDefaultTargetTriple();
  bool result = ny_jit_triple_is_apple_arm64(default_triple);
  if (default_triple)
    LLVMDisposeMessage(default_triple);
  return result;
}

static bool ny_env_has_value(const char *name) {
  const char *raw = getenv(name);
  return raw && *raw;
}

static int ny_clamp_llvm_opt_level(int level) {
  if (level < 0)
    return 0;
  if (level > 3)
    return 3;
  return level;
}

static LLVMCodeGenOptLevel
ny_jit_effective_codegen_opt_level(const ny_options *opt,
                                   LLVMModuleRef module) {
  if (ny_jit_module_is_apple_arm64(module) &&
      !ny_env_has_value("NYTRIX_JIT_CODEGEN_OPT") &&
      !ny_env_has_value("NYTRIX_JIT_OPT_LEVEL") &&
      !ny_env_has_value("NYTRIX_OPT_PROFILE")) {

    return LLVMCodeGenLevelNone;
  }
  return ny_jit_codegen_opt_level(opt);
}

static int ny_jit_effective_ir_opt_level(const ny_options *opt,
                                         LLVMModuleRef module, int fallback) {
  int level = ny_env_int("NYTRIX_JIT_IR_OPT_LEVEL", -1);
  if (level >= 0)
    return ny_clamp_llvm_opt_level(level);
  level = ny_env_int("NYTRIX_JIT_OPT_LEVEL", -1);
  if (level >= 0)
    return ny_clamp_llvm_opt_level(level);
  if (ny_jit_module_is_apple_arm64(module) &&
      !ny_env_has_value("NYTRIX_OPT_PROFILE") && !(opt && opt->opt_pipeline)) {

    return 0;
  }
  return ny_clamp_llvm_opt_level(fallback);
}

static bool ny_should_use_aot_cache(const ny_options *opt) {
  if (!opt || !opt->output_file || opt->run_jit ||
      (!opt->emit_only && !opt->run_aot))
    return false;
  /*
   * Lint modes (--warn-shadow, --warn-all) must run codegen so the
   * unused-import / shadowing diagnostics are emitted; a cached artifact
   * would silently skip them.
   */
  if (opt->warn_shadow || opt->warn_level >= 2)
    return false;
  if (opt->stop_after != NY_STOP_AFTER_NONE || opt->emit_artifact_path ||
      opt->collect_errors || opt->emit_shapes)
    return false;
  if (opt->dump_on_error || opt->dump_diagnose)
    return false;
  if (opt->trace_exec || opt->dump_docs || opt->dump_funcs ||
      opt->dump_symbols || opt->dump_stats || opt->emit_ir_pre_path ||
      opt->emit_asm_path || opt->callgraph || opt->native_tier_report ||
      opt->native_result_oracle || opt->native_dump_ir || opt->nyr_run ||
      opt->nyr_metadata_report || opt->emit_report_path ||
      opt->emit_refinement_interface_path || opt->emit_effect_interface_path ||
      opt->dbg_format_json || ny_env_enabled("NYTRIX_TRACE"))
    return false;
  if (opt->trace_exec || ny_env_enabled("NYTRIX_TRACE"))
    return false;
  if (!ny_env_enabled_default_on("NYTRIX_AOT_CACHE"))
    return false;
  if (opt->dump_ast || opt->expand || opt->dump_llvm || opt->dump_tokens ||
      opt->dump_docs || opt->dump_funcs || opt->dump_symbols ||
      opt->dump_stats || opt->emit_ir_path || opt->emit_asm_path)
    return false;
  return true;
}

static bool ny_should_use_jit_cache(const ny_options *opt) {
  if (!opt)
    return false;
  /*
   * Lint modes (--warn-shadow, --warn-all) must run codegen so the
   * unused-import / shadowing diagnostics are emitted; a cached artifact
   * would silently skip them.
   */
  if (opt->warn_shadow || opt->warn_level >= 2)
    return false;
  if (opt->emit_only && !opt->output_file && !opt->run_jit && !opt->run_aot)
    return false;
  if (opt->stop_after != NY_STOP_AFTER_NONE || opt->emit_artifact_path ||
      opt->collect_errors || opt->emit_shapes)
    return false;
  if (opt->dump_on_error || opt->dump_diagnose)
    return false;
  if (opt->trace_exec || opt->dump_docs || opt->dump_funcs ||
      opt->dump_symbols || opt->dump_stats || opt->emit_ir_pre_path ||
      opt->emit_asm_path || opt->callgraph || opt->native_tier_report ||
      opt->native_result_oracle || opt->native_dump_ir || opt->nyr_run ||
      opt->nyr_metadata_report || opt->emit_report_path ||
      opt->emit_refinement_interface_path || opt->emit_effect_interface_path ||
      opt->dbg_format_json || ny_env_enabled("NYTRIX_TRACE"))
    return false;
  /*
   * Default internal assertions run whenever compilation is needed. Only an
   * explicit assertion request requires repeating all compiler stages.
   */
  if (opt->compiler_asserts > 0 || ny_env_enabled("NYTRIX_COMPILER_ASSERTS"))
    return false;
  if (opt->output_file && !opt->run_jit &&
      !ny_env_enabled("NYTRIX_AOT_IR_CACHE"))
    return false;
  /*
   * File execution is the common edit-loop path. The cache manifest binds the
   * exact expanded source and semantic configuration, so reuse is safe by
   * default; NYTRIX_JIT_CACHE_RUN=0 remains an explicit cold-run escape.
   */
  if (opt->run_jit && !opt->output_file &&
      !ny_env_enabled_default_on("NYTRIX_JIT_CACHE_RUN"))
    return false;
  return ny_jit_cache_enabled();
}

static bool ny_should_write_compile_caches(const ny_options *opt) {
  if (!opt)
    return false;
  return true;
}

static bool ny_std_bc_cache_preverify_enabled(void) {
  return ny_env_enabled("NYTRIX_STD_BC_CACHE_VERIFY") ||
         ny_env_enabled("NYTRIX_CACHE_STRICT_VERIFY");
}

static bool ny_use_name_is_project_core_module(const char *name) {
  if (!name)
    return false;
  return strcmp(name, "core.demo") == 0 || strncmp(name, "core.demo.", 10) == 0;
}

static time_t ny_file_mtime_or_zero(const char *path) {
  if (!path || !*path)
    return 0;
  struct stat st;
  if (stat(path, &st) != 0)
    return 0;
  return st.st_mtime;
}

static uint64_t ny_file_cache_stamp(const char *path) {
  if (!path || !*path)
    return 0;
  struct stat st;
  if (stat(path, &st) != 0)
    return 0;
  uint64_t h = NY_FNV1A64_OFFSET_BASIS;
  const uint64_t vals[] = {
      (uint64_t)st.st_mtime,
      (uint64_t)st.st_size,
      ny_stat_mtime_nsec(&st),
  };
  return ny_hash_u64v(h, vals, sizeof(vals) / sizeof(vals[0]));
}

static time_t ny_runtime_latest_mtime(const char *root) {
  static char cached_root[4096];
  static time_t cached_latest = 0;
  static int cached_valid = 0;
  if (root && *root && cached_valid && strcmp(cached_root, root) == 0)
    return cached_latest;
  if (!root || !*root)
    return 0;
  static const char *const deps[] = {
      "src/code/lang/runtime/init.c",
      "src/code/lang/runtime/ast.c",
      "src/code/lang/runtime/bigint.c",
      "src/code/lang/runtime/core.c",
      "src/code/lang/runtime/ffi.c",
      "src/code/lang/runtime/ffigates.c",
      "src/code/lang/runtime/gc.c",
      "src/code/lang/runtime/math.c",
      "src/code/lang/runtime/memory.c",
      "src/code/lang/runtime/os.c",
      "src/code/lang/runtime/simmd.c",
      "src/code/lang/runtime/string.c",
      "src/code/lang/runtime/shared.h",
      "src/code/lang/runtime/runtime.h",
      "src/code/lang/runtime/defs.h",
      "src/code/lang/parse/ast.h",
      "src/code/lang/parse/json.h",
      "src/code/lang/parse/parser.h",
      "src/code/lang/parse/lexer.h",
      "src/code/lang/types/types.h",
      "src/base/common.h",
      "src/base/compat.h",
  };
  time_t latest = 0;
  char full[4096];
  for (size_t i = 0; i < sizeof(deps) / sizeof(deps[0]); ++i) {
    snprintf(full, sizeof(full), "%s/%s", root, deps[i]);
    time_t mt = ny_file_mtime_or_zero(full);
    if (mt > latest)
      latest = mt;
  }
  snprintf(cached_root, sizeof(cached_root), "%s", root);
  cached_latest = latest;
  cached_valid = 1;
  return latest;
}

static void ny_build_aot_cache_path(const ny_options *opt, const char *source,
                                    const char *parse_name,
                                    const char *std_path,
                                    const char *output_path, char *out,
                                    size_t out_len) {
  if (!out || out_len == 0) {
    return;
  }
  out[0] = '\0';
  if (!opt || !source || !output_path)
    return;
  uint64_t h = NY_FNV1A64_OFFSET_BASIS;
  h = ny_fnv1a64_cstr("aot-cache-v14", h);
  h = ny_fnv1a64_cstr(VERSION, h);
#ifdef NYTRIX_VERSION_COMMIT
  h = ny_fnv1a64_cstr(NYTRIX_VERSION_COMMIT, h);
#endif
#ifdef NYTRIX_VERSION_DIRTY
  h = ny_hash64_u64(h, (uint64_t)NYTRIX_VERSION_DIRTY);
#endif
#ifdef NYTRIX_BUILD_HASH
  h = ny_fnv1a64_cstr(NYTRIX_BUILD_HASH, h);
#endif
#ifdef LLVM_VERSION_STRING
  h = ny_fnv1a64_cstr(LLVM_VERSION_STRING, h);
#endif
  h = ny_fnv1a64_cstr(source, h);
  h = ny_fnv1a64_cstr(parse_name ? parse_name : "<inline>", h);
  h = ny_fnv1a64_cstr(opt->opt_profile, h);
  {
    const unsigned opt_fields[] = {(unsigned)opt->opt_level,
                                   (unsigned)opt->debug_symbols,
                                   (unsigned)opt->strip_override,
                                   (unsigned)opt->std_mode,
                                   (unsigned)opt->no_std,
                                   (unsigned)opt->opt_dce,
                                   (unsigned)opt->opt_internalize,
                                   (unsigned)opt->opt_loops,
                                   (unsigned)opt->opt_autotune,
                                   (unsigned)opt->ownership,
                                   (unsigned)opt->ownership_strict,
                                   (unsigned)opt->borrow_check,
                                   (unsigned)opt->mode,
                                   (unsigned)opt->heap_policy,
                                   (unsigned)opt->safe_mode,
                                   (unsigned)opt->strict_types,
                                   (unsigned)opt->effect_require_known,
                                   (unsigned)opt->alias_require_known,
                                   (unsigned)opt->alias_require_no_escape,
                                   (unsigned)opt->std_builtin_ops,
                                   (unsigned)opt->compiler_asserts,
                                   (unsigned)opt->debug_locals,
                                   (unsigned)opt->dwarf_version,
                                   (unsigned)opt->dwarf_split_inlining,
                                   (unsigned)opt->dwarf_profile_info,
                                   (unsigned)opt->gprof,
                                   (unsigned)opt->native_backend,
                                   (unsigned)opt->native_abi,
                                   (unsigned)opt->native_only,
                                   (unsigned)opt->llvm_lto,
                                   (unsigned)opt->c_frontend};
    h = ny_hash_u32v(h, opt_fields, sizeof(opt_fields) / sizeof(opt_fields[0]));
  }
  h = ny_fnv1a64_cstr(opt->opt_pipeline, h);
  h = ny_fnv1a64_cstr(opt->type_solver_raw, h);
  h = ny_fnv1a64_cstr(opt->proof_solver_raw, h);
  h = ny_fnv1a64_cstr(opt->host_triple, h);
  h = ny_fnv1a64_cstr(opt->host_cflags, h);
  h = ny_fnv1a64_cstr(opt->host_ldflags, h);
  h = ny_fnv1a64_cstr(opt->sanitize, h);
  h = ny_fnv1a64_cstr(opt->arm_float_abi, h);
  h = ny_fnv1a64_cstr(ny_builder_choose_cc(), h);
  h = ny_hash_cstrv(h, (const char *const *)opt->link_dirs.data,
                    opt->link_dirs.len);
  h = ny_hash_cstrv(h, (const char *const *)opt->link_libs.data,
                    opt->link_libs.len);
  {
    const char *const host_envs[] = {
        "NYTRIX_COMPTIME_FUEL",
        "NYTRIX_COMPTIME_INTERP_ONLY",
        "NYTRIX_TEST_MODE",
        "NYTRIX_HOST_CFLAGS",
        "NYTRIX_HOST_LDFLAGS",
        "NYTRIX_TYPE_SOLVER",
        "NYTRIX_USER_NATIVE_ABI",
        "NYTRIX_ARM_FLOAT_ABI",
        "NYTRIX_NO_PIE",
        "NYTRIX_ASSUME_INT",
        "NYTRIX_COMPILER_ASSERTS",
        "NYTRIX_DEBUG_LOCALS",
        "NYTRIX_DWARF_VERSION",
        "NYTRIX_DWARF_SPLIT_INLINING",
        "NYTRIX_DWARF_PROFILE_INFO",
        "NYTRIX_OPT_PROFILE",
        "NYTRIX_INDEX_READ_PARITY",
        "NYTRIX_DISABLE_FAST_INDEX_READ",
        "NYTRIX_GUARDED_FAST_GET",
        "NYTRIX_TRUSTED_FAST_GET",
        "NYTRIX_GUARDED_FAST_SET",
        "NYTRIX_TRUSTED_FAST_SET",
        "NYTRIX_GUARDED_FAST_DICT_GET",
        "NYTRIX_TRUSTED_FAST_DICT_GET",
        "NYTRIX_MONO_TYPES",
        "NYTRIX_ENABLE_MONOMORPHIZATION",
        "NYTRIX_DISABLE_MONO_TYPES",
        "NYTRIX_DISABLE_MONOMORPHIZATION",
        "NYTRIX_MONO_IMPERATIVE",
        "NYTRIX_SIMPLE_RAW_INT_CALL_FAST",
        "NYTRIX_FAST_ALL_PROFILES",
        "NYTRIX_PROVEN_RAW_INT_EXPR_FAST",
        "NYTRIX_RAW_INT_EXPR_FAST",
        "NYTRIX_RAW_INT_SLOT_EXPR_FAST",
        "NYTRIX_RAW_INT_EXPR_FAST_OPS",
        "NYTRIX_RAW_INT_EXPR_ADDSUB_FAST",
        "NYTRIX_RAW_INT_EXPR_MUL_FAST",
        "NYTRIX_RAW_INT_HELPERS",
        "NYTRIX_UNTAGGED_INT_LIST_STORAGE",
        "NYTRIX_CONST_STRING_GLOBAL_INIT",
        "NYTRIX_PROVEN_INT_CAST_FAST",
        "NYTRIX_PROVEN_INT_BRANCH_EQ_FAST",
        "NYTRIX_PROVEN_INT_BRANCH_FAST",
        "NYTRIX_PROVEN_INT_MOD_FAST",
        "NYTRIX_PRINT_PROVEN_INT_FAST",
        "NYTRIX_PRINT_PROVEN_STR_FAST",
    };
    h = ny_hash_envv(h, host_envs, sizeof(host_envs) / sizeof(host_envs[0]));
  }
  h = ny_fnv1a64_cstr(std_path, h);
  {
    const char *exe_path = ny_get_executable_path();
    const uint64_t mtimes[] = {
        ny_file_cache_stamp(opt->argv0),
        ny_file_cache_stamp(exe_path),
        (uint64_t)ny_file_mtime_or_zero(std_path),
        (uint64_t)ny_runtime_latest_mtime(ny_src_root()),
    };
    h = ny_hash_u64v(h, mtimes, sizeof(mtimes) / sizeof(mtimes[0]));
  }
  const char *tmp = ny_get_temp_dir();
#ifdef _WIN32
  snprintf(out, out_len, "%s/ny_aot_cache_%016llx.exe", tmp,
           (unsigned long long)h);
#else
  snprintf(out, out_len, "%s/ny_aot_cache_%016llx", tmp, (unsigned long long)h);
#endif
}

static time_t ny_std_latest_mtime(void) {
  static int cached = 0;
  static time_t latest = 0;
  if (cached)
    return latest;
  latest = ny_std_latest_source_mtime();
  {
    const char *root = ny_src_root();
    if (root && *root) {
      char path[4096];
      snprintf(path, sizeof(path), "%s/src/base/loader.c", root);
      time_t mt = ny_file_mtime_or_zero(path);
      if (mt > latest)
        latest = mt;
      snprintf(path, sizeof(path), "%s/src/base/loader.h", root);
      mt = ny_file_mtime_or_zero(path);
      if (mt > latest)
        latest = mt;
    }
  }
  cached = 1;
  return latest;
}

static bool ny_std_path_is_generated_build_artifact(const char *path) {
  if (!path || !*path)
    return false;
  const char *root = ny_src_root();
  if (!root || !*root)
    return false;
  char prefix[4096];
  int n = snprintf(prefix, sizeof(prefix), "%s/build/", root);
  if (n > 0 && (size_t)n < sizeof(prefix) &&
      strncmp(path, prefix, (size_t)n) == 0)
    return true;
#ifdef _WIN32
  n = snprintf(prefix, sizeof(prefix), "%s\\build\\", root);
  if (n > 0 && (size_t)n < sizeof(prefix) &&
      strncmp(path, prefix, (size_t)n) == 0)
    return true;
#endif
  return false;
}

static int ny_write_file_atomic(const char *path, const char *content,
                                size_t len) {
  if (!path || !*path || !content)
    return -1;
  char tmp[4096];
#ifndef _WIN32
  snprintf(tmp, sizeof(tmp), "%s.XXXXXX", path);
  int fd = mkstemp(tmp);
  if (fd < 0)
    return -1;
  FILE *f = fdopen(fd, "wb");
  if (!f) {
    close(fd);
    unlink(tmp);
    return -1;
  }
#else
  int fd = -1;
  int retries = 100;
  while (retries > 0) {
    unsigned int r = 0;
    if (rand_s(&r) != 0) {
      r = (unsigned int)rand();
    }
    snprintf(tmp, sizeof(tmp), "%s.tmp.%u.%llu", path, r,
             (unsigned long long)ny_ticks_now());
    fd = _open(tmp, _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY,
               _S_IREAD | _S_IWRITE);
    if (fd >= 0) {
      break;
    }
    if (errno != EEXIST) {
      return -1;
    }
    retries--;
  }
  if (fd < 0) {
    return -1;
  }
  FILE *f = _fdopen(fd, "wb");
  if (!f) {
    _close(fd);
    (void)unlink(tmp);
    return -1;
  }
#endif
  size_t written = fwrite(content, 1, len, f);
  int close_rc = fclose(f);
  if (written != len || close_rc != 0) {
    (void)unlink(tmp);
    return -1;
  }
#ifdef _WIN32
  (void)unlink(path);
#endif
  if (rename(tmp, path) != 0) {
    (void)unlink(tmp);
    return -1;
  }
  return 0;
}

static bool ny_std_sources_available(void) {
  const char *root = ny_src_root();
  if (!root || !*root)
    return false;
  char std_src[4096];
  struct stat st;
  const char *cands[] = {"src/std", "std", "src/lib", "lib"};
  for (size_t i = 0; i < 4; i++) {
    snprintf(std_src, sizeof(std_src), "%s/%s", root, cands[i]);
    if (stat(std_src, &st) == 0 && S_ISDIR(st.st_mode))
      return true;
  }
  return false;
}

static uint64_t ny_build_std_cache_path(const ny_options *opt,
                                        const char *const *uses,
                                        size_t use_count, std_mode_t std_mode,
                                        const char *prebuilt_path, char *out,
                                        size_t out_len) {
  if (!out || out_len == 0)
    return 0;
  out[0] = '\0';
  uint64_t h = NY_FNV1A64_OFFSET_BASIS;
  h = ny_fnv1a64_cstr("std-cache-v14", h);
  h = ny_hash64_u64(h, (uint64_t)std_mode);
  if (opt) {
    const unsigned opt_fields[] = {
        (unsigned)opt->opt_level,       (unsigned)opt->opt_dce,
        (unsigned)opt->opt_internalize, (unsigned)opt->no_std,
        (unsigned)opt->debug_symbols,   (unsigned)opt->trace_exec,
        (unsigned)opt->ownership,       (unsigned)opt->ownership_strict,
        (unsigned)opt->borrow_check};
    h = ny_hash_u32v(h, opt_fields, sizeof(opt_fields) / sizeof(opt_fields[0]));
  }
  h = ny_hash_cstrv(h, uses, use_count);
  h = ny_fnv1a64_cstr(prebuilt_path, h);

  if (prebuilt_path && *prebuilt_path &&
      !ny_std_path_is_generated_build_artifact(prebuilt_path)) {
    struct stat pst;
    if (stat(prebuilt_path, &pst) == 0) {
      h = ny_hash64_u64(h, (uint64_t)pst.st_mtime);

      h = ny_hash64_u64(h, (uint64_t)pst.st_ctime);
      h = ny_hash64_u64(h, (uint64_t)pst.st_ino);
      h = ny_hash64_u64(h, (uint64_t)pst.st_dev);
      h = ny_hash64_u64(h, (uint64_t)pst.st_size);
    }
  }
  h = ny_hash64_u64(h, (uint64_t)ny_std_latest_mtime());
  h = ny_hash64_u64(h, ny_std_source_fingerprint());
  h = ny_fnv1a64_cstr(VERSION, h);
#ifdef NYTRIX_VERSION_COMMIT
  h = ny_fnv1a64_cstr(NYTRIX_VERSION_COMMIT, h);
#endif
#ifdef NYTRIX_VERSION_DIRTY
  h = ny_hash64_u64(h, (uint64_t)NYTRIX_VERSION_DIRTY);
#endif
  {
    const char *const envs[] = {
        "NYTRIX_COMPTIME_FUEL", "NYTRIX_COMPTIME_INTERP_ONLY",
        "NYTRIX_TEST_MODE",     "NYTRIX_HOST_TRIPLE",
        "NYTRIX_HOST_CFLAGS",   "NYTRIX_HOST_LDFLAGS",
        "NYTRIX_ARM_FLOAT_ABI", "NYTRIX_ASSUME_INT"};
    h = ny_hash_envv(h, envs, sizeof(envs) / sizeof(envs[0]));
  }
  h = ny_fnv1a64_cstr(ny_src_root(), h);
  h = ny_fnv1a64_cstr(opt ? opt->argv0 : NULL, h);
  h = ny_hash64_u64(h,
                    (uint64_t)ny_file_mtime_or_zero(opt ? opt->argv0 : NULL));

  /*
   * This cache is used only when the source has no local imports. Its
   * standard-library bundle depends on the requested std modules and build
   * configuration, not on the caller's source pathname or contents. Including
   * input_file here created a distinct std cache for every generated fixture
   * and forced every test worker to rebuild the same bundle.
   */
  char std_cache_dir[4096];
  snprintf(std_cache_dir, sizeof(std_cache_dir), "%s/std-src",
           ny_cache_root_dir());
  ny_ensure_dir_recursive(std_cache_dir);
  snprintf(out, out_len, "%s/ny_std_cache_%016llx.ny", std_cache_dir,
           (unsigned long long)h);
  return h;
}

static void append_use(char ***uses, size_t *len, size_t *cap,
                       const char *name) {
  for (size_t i = 0; i < *len; ++i) {
    if (strcmp((*uses)[i], name) == 0)
      return;
  }
  if (*len == *cap) {
    size_t new_cap = *cap ? (*cap * 2) : 8;
    if (new_cap <= *cap) /* overflow */
      return;
    char **tmp = realloc(*uses, new_cap * sizeof(char *));
    if (!tmp)
      return;
    *uses = tmp;
    *cap = new_cap;
  }
  (*uses)[(*len)++] = ny_strdup(name);
}

static bool ny_module_file_declares(const char *path, const char *module_name) {
  if (!path || !*path || !module_name || !*module_name ||
      ny_access(path, R_OK) != 0)
    return false;
  char *declared = ny_read_declared_module_name(path);
  bool ok = declared && strcmp(declared, module_name) == 0;
  free(declared);
  return ok;
}

static bool ny_check_child_module_path(const char *base, const char *leaf,
                                       const char *full_name) {
  char name[512];
  if (snprintf(name, sizeof(name), "%s.ny", leaf) >= (int)sizeof(name))
    return false;
  char path[4096];
  ny_join_path(path, sizeof(path), base, name);
  if (ny_module_file_declares(path, full_name))
    return true;

  char child_dir[4096];
  ny_join_path(child_dir, sizeof(child_dir), base, leaf);
  ny_join_path(path, sizeof(path), child_dir, "mod.ny");
  return ny_module_file_declares(path, full_name);
}

static bool ny_entry_child_module_exists(const char *entry_path,
                                         const char *full_name) {
  if (!entry_path || !*entry_path || entry_path[0] == '<' || !full_name ||
      !*full_name)
    return false;
  const char *leaf = strrchr(full_name, '.');
  leaf = leaf ? leaf + 1 : full_name;
  if (!leaf || !*leaf)
    return false;
  char path_copy[4096];
  if (snprintf(path_copy, sizeof(path_copy), "%s", entry_path) >=
      (int)sizeof(path_copy))
    return false;
  char *slash = strrchr(path_copy, '/');
  char *file = slash ? slash + 1 : path_copy;
  char stem[512];
  if (snprintf(stem, sizeof(stem), "%s", file) >= (int)sizeof(stem))
    return false;
  char *dot = strrchr(stem, '.');
  if (dot)
    *dot = '\0';
  if (slash) {
    if (slash == path_copy)
      slash[1] = '\0';
    else
      *slash = '\0';
  } else {
    snprintf(path_copy, sizeof(path_copy), ".");
  }

  if (ny_check_child_module_path(path_copy, leaf, full_name))
    return true;

  if (strcmp(stem, "mod") == 0)
    return false;
  char base[4096];
  ny_join_path(base, sizeof(base), path_copy, stem);
  return ny_check_child_module_path(base, leaf, full_name);
}

static token_t ny_collect_module_export_imports(lexer_t *lx, char ***uses,
                                                size_t *len, size_t *cap,
                                                const char *entry_path,
                                                const char *module_name,
                                                token_t t) {
  if (!lx || !module_name || !*module_name || t.kind != NY_T_LPAREN)
    return t;
  int paren_depth = 1;
  for (;;) {
    t = lexer_next(lx);
    if (t.kind == NY_T_EOF)
      return t;
    if (t.kind == NY_T_LPAREN) {
      paren_depth++;
      continue;
    }
    if (t.kind == NY_T_RPAREN) {
      paren_depth--;
      if (paren_depth == 0)
        return lexer_next(lx);
      continue;
    }
    if (paren_depth != 1 || t.kind != NY_T_IDENT)
      continue;
    char *leaf = dup_token_lexeme(t);
    if (!leaf)
      continue;
    size_t full_len = strlen(module_name) + 1 + strlen(leaf);
    char *full = malloc(full_len + 1);
    if (full) {
      snprintf(full, full_len + 1, "%s.%s", module_name, leaf);
      if (ny_entry_child_module_exists(entry_path, full))
        append_use(uses, len, cap, full);
      free(full);
    }
    free(leaf);
  }
}

static bool ny_is_llvm_special_global(const char *name);
static bool ny_ir_is_std_value(LLVMValueRef v);
static void ny_build_llvm_used(LLVMModuleRef module, const LLVMValueRef *values,
                               size_t count);

static bool ny_std_bc_symbol_is_mixed_codegen_artifact(const char *name) {
  if (!name || !*name)
    return false;
  /*
   * The stdlib cache is linked into unrelated user programs.  A script entry
   * can only come from the user half of the joined module; accepting it here
   * makes the first program that populates the cache run for every later
   * source with the same import set.
   */
  return strcmp(name, "_ny_top_entry") == 0 ||
         strncmp(name, "__ny_callable_adapter_", 22) == 0 ||
         strncmp(name, "__ny_callable_adapter_env_", 26) == 0;
}

static bool ny_std_bc_module_is_link_safe(LLVMModuleRef module,
                                          const char **bad_symbol) {
  if (bad_symbol)
    *bad_symbol = NULL;
  if (!module)
    return false;
  /*
   * Every referenced function/global is also present in its module symbol
   * list, including declarations.  Checking those lists once proves that no
   * mixed-codegen artifact can be reached without recursively revisiting
   * every instruction and constant operand in the module.
   */
  for (LLVMValueRef fn = LLVMGetFirstFunction(module); fn;
       fn = LLVMGetNextFunction(fn)) {
    const char *name = LLVMGetValueName(fn);
    if (ny_std_bc_symbol_is_mixed_codegen_artifact(name)) {
      if (bad_symbol)
        *bad_symbol = name;
      return false;
    }
  }
  for (LLVMValueRef gv = LLVMGetFirstGlobal(module); gv;
       gv = LLVMGetNextGlobal(gv)) {
    const char *name = LLVMGetValueName(gv);
    if (ny_std_bc_symbol_is_mixed_codegen_artifact(name)) {
      if (bad_symbol)
        *bad_symbol = name;
      return false;
    }
  }
  return true;
}

static void ny_drop_llvm_used_globals(LLVMModuleRef module) {
  if (!module)
    return;
  const char *names[] = {"llvm.used", "llvm.compiler.used"};
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    LLVMValueRef gv = LLVMGetNamedGlobal(module, names[i]);
    if (gv)
      LLVMDeleteGlobal(gv);
  }
}

static bool ny_std_bc_cache_links_path(const char *cache_path, char *out,
                                       size_t out_len) {
  if (!cache_path || !*cache_path || !out || out_len == 0)
    return false;
  int n = snprintf(out, out_len, "%s.libs", cache_path);
  return n > 0 && (size_t)n < out_len;
}

static bool ny_std_bc_cache_has_links(const char *cache_path) {
  char path[4096];
  return ny_std_bc_cache_links_path(cache_path, path, sizeof(path)) &&
         ny_access(path, R_OK) == 0;
}

static void ny_codegen_add_link_lib(codegen_t *cg, const char *lib) {
  if (!cg || !lib || !*lib)
    return;
  for (size_t i = 0; i < cg->links.len; ++i) {
    if (cg->links.data[i] && strcmp(cg->links.data[i], lib) == 0)
      return;
  }
  vec_push(&cg->links, ny_strdup(lib));
}

static bool ny_std_bc_cache_load_links(const char *cache_path, codegen_t *cg) {
  char path[4096];
  if (!ny_std_bc_cache_links_path(cache_path, path, sizeof(path)))
    return false;
  FILE *f = fopen(path, "r");
  if (!f)
    return false;
  char line[512];
  while (fgets(line, sizeof(line), f)) {
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' ||
                       line[len - 1] == ' ' || line[len - 1] == '\t'))
      line[--len] = '\0';
    char *p = line;
    while (*p == ' ' || *p == '\t')
      p++;
    if (*p)
      ny_codegen_add_link_lib(cg, p);
  }
  fclose(f);
  return true;
}

static bool ny_std_bc_cache_save_links(const char *cache_path,
                                       const codegen_t *cg) {
  char path[4096];
  if (!ny_std_bc_cache_links_path(cache_path, path, sizeof(path)) || !cg)
    return false;
  size_t total = 0;
  for (size_t i = 0; i < cg->links.len; ++i) {
    const char *lib = cg->links.data[i];
    if (lib && *lib) {
      size_t lib_len = strlen(lib);
      if (total > SIZE_MAX - lib_len - 1)
        return false;
      total += lib_len + 1;
    }
  }
  if (total == 0)
    return true;
  char *buf = malloc(total);
  if (!buf)
    return false;
  size_t off = 0;
  for (size_t i = 0; i < cg->links.len; ++i) {
    const char *lib = cg->links.data[i];
    if (!lib || !*lib)
      continue;
    size_t len = strlen(lib);
    memcpy(buf + off, lib, len);
    off += len;
    buf[off++] = '\n';
  }
  bool ok = ny_write_file_atomic(path, buf, off) == 0;
  free(buf);
  return ok;
}

static bool ny_save_std_bc_cache_from_module(LLVMModuleRef module,
                                             const char *cache_path) {
  if (!module || !cache_path || !*cache_path)
    return false;
  /*
   * Write the module to bitcode directly without LLVMCloneModule.  The old
   * path cloned the entire monolithic stdlib module (often > 10 GiB) just to
   * strip debug info, externalize non-std symbols, and run globaldce before
   * serialization.  That doubled peak RSS for no correctness benefit: the
   * load path already runs its own link-safety check, and the module will be
   * optimized after being linked back in.  Writing the original saves a full
   * module copy.
   */
  const char *bad_symbol = NULL;
  if (!ny_std_bc_module_is_link_safe(module, &bad_symbol)) {
    if (verbose_enabled >= 2 && bad_symbol && *bad_symbol)
      NY_LOG_INFO("skipping stdlib bitcode cache: mixed codegen artifact %s\n",
                  bad_symbol);
    (void)unlink(cache_path);
    return false;
  }
  bool ok = ny_jit_cache_save(cache_path, module);
  return ok;
}

typedef struct {
  struct ny_module_cache_entry *entries;
  size_t len;
  size_t cap;
} ny_module_list;

typedef struct ny_module_cache_entry {
  char *name;
  uint64_t source_fingerprint;
} ny_module_cache_entry;

typedef struct {
  char *name;
  char *bc_path;
  char *cache_path;
  bool persistent;
#ifndef _WIN32
  pid_t pid;
#endif
  int exit_code;
} ny_module_job;

static void ny_module_list_add(ny_module_list *list, const char *name) {
  if (!list || !name || !*name)
    return;
  for (size_t i = 0; i < list->len; i++) {
    if (strcmp(list->entries[i].name, name) == 0)
      return;
  }
  if (list->len == list->cap) {
    size_t nc = list->cap ? list->cap * 2 : 8;
    ny_module_cache_entry *nn =
        realloc(list->entries, nc * sizeof(ny_module_cache_entry));
    if (!nn)
      return;
    list->entries = nn;
    list->cap = nc;
  }
  list->entries[list->len++] = (ny_module_cache_entry){.name = ny_strdup(name)};
}

static NY_UNUSED_FUNC void ny_collect_top_modules(const program_t *prog,
                                                  ny_module_list *out) {
  if (!prog || !out)
    return;
  for (size_t i = 0; i < prog->body.len; i++) {
    stmt_t *s = prog->body.data[i];
    if (s && s->kind == NY_S_MODULE && s->as.module.name) {
      ny_module_list_add(out, s->as.module.name);
    }
  }
}

static NY_UNUSED_FUNC void ny_free_module_list(ny_module_list *list) {
  if (!list)
    return;
  for (size_t i = 0; i < list->len; i++)
    free(list->entries[i].name);
  free(list->entries);
  list->entries = NULL;
  list->len = list->cap = 0;
}

static long ny_module_list_index(const ny_module_list *list, const char *name) {
  if (!list || !name)
    return -1;
  for (size_t i = 0; i < list->len; ++i) {
    if (list->entries[i].name && strcmp(list->entries[i].name, name) == 0)
      return (long)i;
  }
  return -1;
}

static long ny_module_list_index_for_use(const ny_module_list *list,
                                         const char *raw_name) {
  long exact = ny_module_list_index(list, raw_name);
  if (exact >= 0 || !list || !raw_name)
    return exact;
  /*
   * The parser preserves a quoted relative import such as "./provider.ny",
   * while the loader assigns the declared module name ("provider"). Map that
   * source spelling back to the unit declaration before building the cache
   * dependency graph.
   */
  const char *leaf = strrchr(raw_name, '/');
  leaf = leaf ? leaf + 1 : raw_name;
  size_t leaf_len = strlen(leaf);
  if (leaf_len > 3 && strcmp(leaf + leaf_len - 3, ".ny") == 0)
    leaf_len -= 3;
  if (leaf_len == 0 || leaf_len >= 512)
    return -1;
  char normalized[512];
  memcpy(normalized, leaf, leaf_len);
  normalized[leaf_len] = '\0';
  for (size_t i = 0; i < list->len; ++i) {
    const char *candidate = list->entries[i].name;
    if (!candidate)
      continue;
    const char *candidate_leaf = strrchr(candidate, '.');
    candidate_leaf = candidate_leaf ? candidate_leaf + 1 : candidate;
    if (strcmp(candidate, normalized) == 0 ||
        strcmp(candidate_leaf, normalized) == 0)
      return (long)i;
  }
  return -1;
}

static const stmt_t *ny_module_stmt(const program_t *prog, const char *name) {
  if (!prog || !name)
    return NULL;
  for (size_t i = 0; i < prog->body.len; ++i) {
    const stmt_t *s = prog->body.data[i];
    if (s && s->kind == NY_S_MODULE && s->as.module.name &&
        strcmp(s->as.module.name, name) == 0)
      return s;
  }
  return NULL;
}

static uint64_t ny_module_file_fingerprint(const stmt_t *mod,
                                           const program_t *prog) {
  uint64_t h = NY_FNV1A64_OFFSET_BASIS;
  const char *path = mod && mod->tok.filename ? mod->tok.filename : NULL;
  h = ny_fnv1a64_cstr(path, h);
  char *contents = path ? ny_read_file(path) : NULL;
  if (contents) {
    h = ny_fnv1a64_cstr(contents, h);
    free(contents);
  } else if (prog && prog->raw_src) {
    /*
     * Synthetic/generated modules have no independently readable file. Keep
     * them conservative: their enclosing parsed source owns the identity.
     */
    h = ny_fnv1a64_cstr(prog->raw_src, h);
  }
  return h;
}

static void ny_module_mark_dependency_closure(const program_t *prog,
                                              const ny_module_list *list,
                                              size_t idx, bool *marked) {
  if (!prog || !list || !marked || idx >= list->len || marked[idx])
    return;
  marked[idx] = true;
  const stmt_t *mod = ny_module_stmt(prog, list->entries[idx].name);
  if (!mod)
    return;
  for (size_t i = 0; i < mod->as.module.body.len; ++i) {
    const stmt_t *child = mod->as.module.body.data[i];
    if (!child || child->kind != NY_S_USE || !child->as.use.module)
      continue;
    long dep = ny_module_list_index_for_use(list, child->as.use.module);
    if (dep >= 0)
      ny_module_mark_dependency_closure(prog, list, (size_t)dep, marked);
  }
}

static void ny_module_list_fingerprint(const program_t *prog,
                                       ny_module_list *list) {
  if (!prog || !list)
    return;
  uint64_t *own = calloc(list->len ? list->len : 1, sizeof(uint64_t));
  bool *marked = calloc(list->len ? list->len : 1, sizeof(bool));
  if (!own || !marked) {
    free(own);
    free(marked);
    return;
  }
  for (size_t i = 0; i < list->len; ++i)
    own[i] = ny_module_file_fingerprint(
        ny_module_stmt(prog, list->entries[i].name), prog);
  for (size_t i = 0; i < list->len; ++i) {
    memset(marked, 0, list->len * sizeof(bool));
    ny_module_mark_dependency_closure(prog, list, i, marked);
    uint64_t h =
        ny_fnv1a64_cstr("module-dependency-v1", NY_FNV1A64_OFFSET_BASIS);
    for (size_t j = 0; j < list->len; ++j) {
      if (!marked[j])
        continue;
      h = ny_fnv1a64_cstr(list->entries[j].name, h);
      h = ny_hash64_u64(h, own[j]);
    }
    list->entries[i].source_fingerprint = h;
  }
  free(own);
  free(marked);
}

static int ny_parallel_default_jobs(void) {
  long ncpu = ny_cpu_count();
  if (ncpu > 0 && ncpu < 1024)
    return (int)ncpu;
  return 4;
}

static NY_UNUSED_FUNC int ny_parallel_module_jobs(const ny_options *opt,
                                                  size_t total) {
  if (!opt)
    return 1;
  if (opt->thread_count > 0)
    return opt->thread_count;
  int jobs = ny_parallel_default_jobs();
  if (opt->parallel_mode && strcmp(opt->parallel_mode, "auto") == 0 && jobs > 8)
    jobs = 8;
  if (jobs < 1)
    jobs = 1;
  if ((size_t)jobs > total)
    jobs = (int)total;
  if (jobs < 1)
    jobs = 1;
  return jobs;
}

static NY_UNUSED_FUNC bool ny_parallel_modules_enabled(const ny_options *opt) {
  if (!opt || !opt->parallel_mode)
    return false;

  if (opt->emit_only && !opt->output_file && !opt->run_jit)
    return false;
  bool explicit_modules = strcmp(opt->parallel_mode, "modules") == 0;
  /*
   * A module worker currently reparses and rebuilds the complete joined
   * source, multiplying LLVM memory by the worker count. It also cannot yet
   * preserve every cross-module lazy/capture dependency. Keep that experiment
   * behind the explicit modules mode; auto uses bounded outer/test parallelism
   * and the single shared compiler pipeline.
   */
  if (!explicit_modules)
    return false;
  if (getenv("NYTRIX_PARALLEL_DISABLE"))
    return false;
  if (opt->run_jit && !ny_env_enabled("NYTRIX_PARALLEL_JIT"))
    return false;
  if (!opt->input_file)
    return false;
  return true;
}

#ifndef _WIN32
static void ny_module_job_free(ny_module_job *job) {
  if (!job)
    return;
  free(job->name);
  free(job->bc_path);
  free(job->cache_path);
  job->name = NULL;
  job->bc_path = NULL;
  job->cache_path = NULL;
}

static char *ny_sanitize_modname(const char *name) {
  if (!name)
    return ny_strdup("mod");
  size_t n = strlen(name);
  char *out = malloc(n + 1);
  if (!out)
    return NULL;
  for (size_t i = 0; i < n; i++) {
    char c = name[i];
    out[i] = (c == '.') ? '_' : c;
  }
  out[n] = '\0';
  return out;
}

static bool ny_module_cache_path(const ny_options *opt, const char *module_name,
                                 uint64_t source_fingerprint, char *out,
                                 size_t out_len) {
  if (!opt || !module_name || !out || out_len == 0)
    return false;
  uint64_t h = NY_FNV1A64_OFFSET_BASIS;
  h = ny_fnv1a64_cstr("module-bitcode-cache-v4", h);
  h = ny_hash64_u64(h, ny_cache_compiler_source_fingerprint());
  h = ny_fnv1a64_cstr(module_name, h);
  h = ny_hash64_u64(h, source_fingerprint);
  h = ny_hash64_u64(h, ny_file_cache_stamp(opt->input_file));
  h = ny_hash64_u64(h, (uint64_t)opt->opt_level);
  h = ny_hash64_u64(h, (uint64_t)opt->no_std);
  h = ny_hash64_u64(h, (uint64_t)opt->debug_symbols);
  {
    const unsigned config[] = {
        (unsigned)opt->std_mode,
        (unsigned)opt->std_mode_explicit,
        (unsigned)opt->opt_size,
        (unsigned)opt->opt_dce,
        (unsigned)opt->opt_internalize,
        (unsigned)opt->opt_loops,
        (unsigned)opt->opt_autotune,
        (unsigned)opt->safe_mode,
        (unsigned)opt->strict_types,
        (unsigned)opt->strict_types_explicit,
        (unsigned)opt->ownership,
        (unsigned)opt->ownership_strict,
        (unsigned)opt->borrow_check,
        (unsigned)opt->effect_require_known,
        (unsigned)opt->alias_require_known,
        (unsigned)opt->alias_require_no_escape,
        (unsigned)opt->heap_policy,
        (unsigned)opt->runtime_mode,
        (unsigned)opt->std_builtin_ops,
        (unsigned)opt->compiler_asserts,
        (unsigned)opt->native_backend,
        (unsigned)opt->native_abi,
        (unsigned)opt->native_tier,
        (unsigned)opt->native_only,
        (unsigned)opt->native_enable_cf_mem2reg,
        (unsigned)opt->llvm_lto,
        (unsigned)opt->c_frontend,
        (unsigned)opt->fast_math,
        (unsigned)opt->gpu_fast_math,
    };
    h = ny_hash_u32v(h, config, sizeof(config) / sizeof(config[0]));
  }
  h = ny_fnv1a64_cstr(opt->opt_profile, h);
  h = ny_fnv1a64_cstr(opt->type_solver_raw, h);
  h = ny_fnv1a64_cstr(opt->proof_solver_raw, h);
  h = ny_fnv1a64_cstr(opt->runtime_mode_raw, h);
  h = ny_fnv1a64_cstr(opt->native_backend_raw, h);
  h = ny_fnv1a64_cstr(opt->native_abi_raw, h);
  h = ny_fnv1a64_cstr(opt->native_tier_raw, h);
  h = ny_fnv1a64_cstr(opt->host_triple, h);
  h = ny_fnv1a64_cstr(opt->c_frontend_raw, h);
  h = ny_fnv1a64_cstr(opt->arm_float_abi, h);
  h = ny_fnv1a64_cstr(opt->sanitize, h);
  h = ny_fnv1a64_cstr(opt->host_cflags, h);
  h = ny_fnv1a64_cstr(opt->host_ldflags, h);
  h = ny_fnv1a64_cstr(opt->std_path, h);
  h = ny_fnv1a64_cstr(opt->std_bc_path, h);
  h = ny_fnv1a64_cstr(opt->opt_pipeline, h);
  h = ny_hash_cstrv(h, (const char *const *)opt->link_dirs.data,
                    opt->link_dirs.len);
  h = ny_hash_cstrv(h, (const char *const *)opt->link_libs.data,
                    opt->link_libs.len);
  {
    const char *const semantic_envs[] = {"NYTRIX_COMPTIME_FUEL",
                                         "NYTRIX_COMPTIME_INTERP_ONLY",
                                         "NYTRIX_TEST_MODE",
                                         "NYTRIX_TYPE_SOLVER",
                                         "NYTRIX_PROOF_SOLVER",
                                         "NYTRIX_USER_NATIVE_ABI",
                                         "NYTRIX_ARM_FLOAT_ABI",
                                         "NYTRIX_ASSUME_INT",
                                         "NYTRIX_BIGFLOAT_PRECISION",
                                         "NYTRIX_MONO_TYPES",
                                         "NYTRIX_ENABLE_MONOMORPHIZATION",
                                         "NYTRIX_DISABLE_MONO_TYPES",
                                         "NYTRIX_DISABLE_MONOMORPHIZATION",
                                         "NYTRIX_MONO_IMPERATIVE",
                                         "NYTRIX_MONO_STDLIB",
                                         "NYTRIX_MONO_MAX_COST",
                                         "NYTRIX_MONO_MAX_GLOBAL",
                                         "NYTRIX_MONO_MAX_PER_FN",
                                         "NYTRIX_OPT_PROFILE",
                                         "NYTRIX_FAST_MODE",
                                         "NYTRIX_FAST_INT_BINOPS",
                                         "NYTRIX_FAST_FLOAT_BINOPS",
                                         "NYTRIX_FAST_ALL_PROFILES",
                                         "NYTRIX_ENABLE_OPTIMIZE",
                                         "NYTRIX_ENABLE_TYPEINFER",
                                         "NYTRIX_DISABLE_TYPEINFER",
                                         "NYTRIX_ENABLE_CONST_FOLD",
                                         "NYTRIX_ENABLE_TAIL_CALL",
                                         "NYTRIX_ENABLE_INLINE",
                                         "NYTRIX_AUTO_VECTORIZE_LOOPS",
                                         "NYTRIX_AUTO_MEMO",
                                         "NYTRIX_AUTO_MEMO_IMPURE",
                                         "NYTRIX_DYNAMIC_GLOBAL_CALLS",
                                         "NYTRIX_UNSAFE_ARITH",
                                         "NYTRIX_UNSAFE_FIXNUM",
                                         "NYTRIX_UNSAFE_FAST_MEM",
                                         "NYTRIX_ZERO_INIT",
                                         "NYTRIX_OWNERSHIP",
                                         "NYTRIX_OWNERSHIP_STRICT",
                                         "NYTRIX_OWNERSHIP_CLEANUP",
                                         "NYTRIX_HEAP_POLICY",
                                         "NYTRIX_EFFECT_ASYNC_LOWERING",
                                         "NYTRIX_EFFECT_FORBID",
                                         "NYTRIX_EFFECT_POLICY_INCLUDE_STD",
                                         "NYTRIX_EFFECT_REQUIRE_KNOWN",
                                         "NYTRIX_EFFECT_REQUIRE_PURE",
                                         "NYTRIX_ALIAS_REQUIRE_KNOWN",
                                         "NYTRIX_ALIAS_REQUIRE_NO_ESCAPE",
                                         "NYTRIX_ALIAS_POLICY_INCLUDE_STD",
                                         "NYTRIX_STDBC_IMPORT_FFI",
                                         "NYTRIX_OPT_ENABLE",
                                         "NYTRIX_FAST_COMPILE",
                                         "NYTRIX_FAST_COMPILER",
                                         "NYTRIX_SIMPLE_RAW_INT_CALL_FAST",
                                         "NYTRIX_PROVEN_RAW_INT_EXPR_FAST",
                                         "NYTRIX_RAW_INT_EXPR_FAST",
                                         "NYTRIX_RAW_INT_SLOT_EXPR_FAST",
                                         "NYTRIX_RAW_INT_EXPR_FAST_OPS",
                                         "NYTRIX_RAW_INT_EXPR_ADDSUB_FAST",
                                         "NYTRIX_RAW_INT_EXPR_MUL_FAST",
                                         "NYTRIX_RAW_INT_HELPERS",
                                         "NYTRIX_UNTAGGED_INT_LIST_STORAGE",
                                         "NYTRIX_CONST_STRING_GLOBAL_INIT",
                                         "NYTRIX_PROVEN_INT_CAST_FAST",
                                         "NYTRIX_PROVEN_INT_BRANCH_EQ_FAST",
                                         "NYTRIX_PROVEN_INT_BRANCH_FAST",
                                         "NYTRIX_PROVEN_INT_MOD_FAST",
                                         "NYTRIX_PRINT_PROVEN_INT_FAST",
                                         "NYTRIX_PRINT_PROVEN_STR_FAST"};
    h = ny_hash_envv(h, semantic_envs,
                     sizeof(semantic_envs) / sizeof(semantic_envs[0]));
  }
  char dir[4096];
  int dn = snprintf(dir, sizeof(dir), "%s/modules", ny_cache_root_dir());
  if (dn <= 0 || (size_t)dn >= sizeof(dir))
    return false;
  ny_ensure_dir_recursive(dir);
  int n = snprintf(out, out_len, "%s/%016llx.bc", dir, (unsigned long long)h);
  if (ny_env_enabled("NYTRIX_TRACE_CACHE"))
    fprintf(stderr, "module cache key: %s=%016llx\n", module_name,
            (unsigned long long)h);
  return n > 0 && (size_t)n < out_len;
}

static bool ny_spawn_module_job(const ny_options *opt, const char *module_name,
                                uint64_t source_fingerprint,
                                const char *tmp_dir, ny_module_job *job) {
  if (!opt || !module_name || !tmp_dir || !job)
    return false;
  char cache_path[4096];
  if (!ny_module_cache_path(opt, module_name, source_fingerprint, cache_path,
                            sizeof(cache_path)))
    return false;
  if (ny_access(cache_path, R_OK) == 0) {
    job->name = ny_strdup(module_name);
    job->bc_path = ny_strdup(cache_path);
    job->persistent = true;
    job->exit_code = 0;
    if (ny_env_enabled("NYTRIX_TRACE_CACHE"))
      fprintf(stderr, "module cache hit: %s\n", module_name);
    return job->name && job->bc_path;
  }
  char *san = ny_sanitize_modname(module_name);
  if (!san)
    return false;
  static unsigned long long ny_mod_seq = 0;
  char bc_path[1024];
  unsigned long long seq = ++ny_mod_seq;
  /*
   * Keep the unpublished file beside its final name. rename(2) is then
   * atomic even when the system temp directory and the cache use different
   * filesystems.
   */
  snprintf(bc_path, sizeof(bc_path), "%s.tmp.%s.%ld.%llu", cache_path, san,
           (long)getpid(), (unsigned long long)seq);
  free(san);
  char emit_bc_arg[1100];
  char emit_mod_arg[1100];
  snprintf(emit_bc_arg, sizeof(emit_bc_arg), "--emit-bc=%s", bc_path);
  snprintf(emit_mod_arg, sizeof(emit_mod_arg), "--emit-module=%s", module_name);

  char std_path_arg[1100];
  const char *std_path = NULL;
  if (opt->std_path) {
    snprintf(std_path_arg, sizeof(std_path_arg), "--std-path=%s",
             opt->std_path);
    std_path = std_path_arg;
  }

  char opt_arg[16];
  char type_solver_arg[128];
  char proof_solver_arg[128];
  char std_bc_arg[1100];
  char heap_arg[32];
  const char *argv[48];
  int idx = 0;
  argv[idx++] = opt->argv0 ? opt->argv0 : "ny";
  if (opt->opt_level > 0) {
    snprintf(opt_arg, sizeof(opt_arg), "-O%d", opt->opt_level);
    argv[idx++] = opt_arg;
  } else {
    argv[idx++] = "-O0";
  }
  argv[idx++] = "-emit-only";
  argv[idx++] = emit_bc_arg;
  argv[idx++] = emit_mod_arg;
  argv[idx++] = "--parallel=off";
  if (opt->safe_mode)
    argv[idx++] = "--safe-mode";
  if (opt->strict_types_explicit)
    argv[idx++] = opt->strict_types ? "--strict-types" : "--no-strict-types";
  argv[idx++] = opt->effect_require_known ? "--effect-require-known"
                                          : "--no-effect-require-known";
  argv[idx++] = opt->alias_require_known ? "--alias-require-known"
                                         : "--no-alias-require-known";
  if (opt->alias_require_no_escape)
    argv[idx++] = "--alias-require-no-escape";
  if (opt->ownership)
    argv[idx++] = "--ownership";
  if (opt->ownership_strict)
    argv[idx++] = "--ownership-strict";
  if (opt->borrow_check)
    argv[idx++] = "--borrow-check";
  if (opt->type_solver_raw && *opt->type_solver_raw) {
    snprintf(type_solver_arg, sizeof(type_solver_arg), "--type-solver=%s",
             opt->type_solver_raw);
    argv[idx++] = type_solver_arg;
  }
  if (opt->proof_solver_raw && *opt->proof_solver_raw) {
    snprintf(proof_solver_arg, sizeof(proof_solver_arg), "--proof-solver=%s",
             opt->proof_solver_raw);
    argv[idx++] = proof_solver_arg;
  }
  if (opt->opt_pipeline && *opt->opt_pipeline) {
    argv[idx++] = "-passes";
    argv[idx++] = opt->opt_pipeline;
  }
  if (opt->no_std)
    argv[idx++] = "-no-std";
  else if (opt->std_mode_explicit && opt->std_mode == STD_MODE_FULL)
    argv[idx++] = "--full-mod";
  else if (opt->std_mode_explicit && opt->std_mode == STD_MODE_NONE)
    argv[idx++] = "-no-std";
  else if (opt->std_mode_explicit && opt->std_mode == STD_MODE_BC &&
           opt->std_bc_path) {
    snprintf(std_bc_arg, sizeof(std_bc_arg), "--std-bc=%s", opt->std_bc_path);
    argv[idx++] = std_bc_arg;
  } else if (opt->std_mode_explicit && opt->std_mode == STD_MODE_MINIMAL) {
    argv[idx++] = "-std";
    argv[idx++] = "minimal";
  }
  if (opt->heap_policy_explicit) {
    snprintf(heap_arg, sizeof(heap_arg), "--heap=%s",
             ny_heap_policy_name(opt->heap_policy));
    argv[idx++] = heap_arg;
  }
  if (std_path)
    argv[idx++] = std_path;
  argv[idx++] = opt->input_file;
  argv[idx++] = NULL;

  pid_t pid = fork();
  if (pid < 0)
    return false;
  if (pid == 0) {
    ny_setenv("NYTRIX_PARALLEL_DISABLE", "1", 1);
    ny_setenv("NYTRIX_WORKER", "1", 1);
    if (opt->opt_level > 0 || (opt->opt_pipeline && *opt->opt_pipeline))
      ny_setenv("NYTRIX_WORKER_OPT", "1", 1);
    execvp(argv[0], (char *const *)argv);
    _exit(1);
  }
  job->name = ny_strdup(module_name);
  job->bc_path = ny_strdup(bc_path);
  job->cache_path = ny_strdup(cache_path);
  job->pid = pid;
  job->exit_code = -1;
  return job->name && job->bc_path && job->cache_path;
}

static bool ny_module_job_publish_cache(ny_module_job *job) {
  if (!job || job->persistent || !job->bc_path || !job->cache_path)
    return job && job->persistent;
  if (rename(job->bc_path, job->cache_path) != 0) {
    if (ny_env_enabled("NYTRIX_TRACE_CACHE"))
      fprintf(stderr, "module cache publish failed: %s: %s\n",
              job->name ? job->name : "<module>", strerror(errno));
    return false;
  }
  free(job->bc_path);
  job->bc_path = job->cache_path;
  job->cache_path = NULL;
  job->persistent = true;
  if (ny_env_enabled("NYTRIX_TRACE_CACHE"))
    fprintf(stderr, "module cache saved: %s\n",
            job->name ? job->name : "<module>");
  return true;
}

#endif

static bool ny_link_module_cache(LLVMContextRef ctx, LLVMModuleRef main_mod,
                                 const char *cache_path,
                                 bool remove_invalid_cache, bool std_cache) {
  if (!ctx || !main_mod || !cache_path)
    return false;
  bool trace_timing = ny_env_enabled("NYTRIX_TRACE_CACHE_TIMING");
  ny_tick_t t_start = trace_timing ? ny_ticks_now() : 0;
  LLVMMemoryBufferRef buf = NULL;
  char *msg = NULL;
  if (LLVMCreateMemoryBufferWithContentsOfFile(cache_path, &buf, &msg) != 0) {
    if (msg)
      LLVMDisposeMessage(msg);
    return false;
  }
  LLVMModuleRef mod = NULL;
  bool parsed = false;
  bool buf_owned_by_module = false;
  if (ny_cache_path_is_ir(cache_path)) {
    parsed = (LLVMParseIRInContext(ctx, buf, &mod, &msg) == 0);
    buf_owned_by_module = parsed;
  } else {
    parsed = (LLVMParseBitcodeInContext2(ctx, buf, &mod) == 0);
    buf_owned_by_module = parsed;
  }
  if (!buf_owned_by_module)
    LLVMDisposeMemoryBuffer(buf);
  if (!parsed && msg) {
    LLVMDisposeMessage(msg);
    msg = NULL;
  }
  if (!parsed) {
    if (remove_invalid_cache)
      (void)unlink(cache_path);
    return false;
  }
  ny_tick_t t_parsed = trace_timing ? ny_ticks_now() : 0;
  LLVMStripModuleDebugInfo(mod);

  char *verify_msg = NULL;
  if (LLVMVerifyModule(mod, LLVMReturnStatusAction, &verify_msg) != 0) {
    if (verify_msg)
      LLVMDisposeMessage(verify_msg);
    LLVMDisposeModule(mod);
    if (remove_invalid_cache)
      (void)unlink(cache_path);
    return false;
  }
  const char *bad_symbol = NULL;
  if (!ny_std_bc_module_is_link_safe(mod, &bad_symbol)) {
    if (bad_symbol && *bad_symbol)
      NY_LOG_WARN("Ignoring unsafe std cache %s: mixed codegen artifact %s\n",
                  cache_path, bad_symbol);
    LLVMDisposeModule(mod);
    if (remove_invalid_cache)
      (void)unlink(cache_path);
    return false;
  }
  ny_tick_t t_verified = trace_timing ? ny_ticks_now() : 0;
  /*
   * The worker compiles the joined source so it can resolve cross-module
   * references. As a result, a function body can also already be present in
   * the entry module. A cached duplicate must remain valid until the linker
   * remaps its users to the entry definition; deleting it leaves dangling
   * references in cached callers. Unreferenced core exports are discardable,
   * while their address-taken uses and global initializers remain roots.
   */
  for (LLVMValueRef fn = LLVMGetFirstFunction(mod); fn;) {
    LLVMValueRef next = LLVMGetNextFunction(fn);
    const char *name = LLVMGetValueName(fn);
    LLVMValueRef entry_fn = name ? LLVMGetNamedFunction(main_mod, name) : NULL;
    if (entry_fn && LLVMGetFirstBasicBlock(entry_fn) &&
        LLVMGetFirstBasicBlock(fn))
      LLVMSetLinkage(fn, LLVMAvailableExternallyLinkage);
    else if (std_cache && !LLVMIsDeclaration(fn) &&
             LLVMGetLinkage(fn) == LLVMExternalLinkage)
      LLVMSetLinkage(fn, LLVMLinkOnceODRLinkage);
    fn = next;
  }
  if (std_cache) {
    for (LLVMValueRef gv = LLVMGetFirstGlobal(mod); gv;
         gv = LLVMGetNextGlobal(gv)) {
      if (!LLVMIsDeclaration(gv) && LLVMGetLinkage(gv) == LLVMExternalLinkage)
        LLVMSetLinkage(gv, LLVMLinkOnceODRLinkage);
    }
  }
  /*
   * The shared core.bc is built once for the host triple. Cross targets
   * (e.g. coff64 with --host-triple x86_64-w64-windows-gnu) would otherwise
   * link a linux-triple module into a windows-triple module. LLVM warns
   * about the layout/triple mismatch and, with -g, the post-link verify can
   * fail on intermediate DIBuilder state (invalid DILexicalBlockFile scope).
   * Normalize the cached module to the main module before linking.
   */
  {
    const char *main_triple = LLVMGetTarget(main_mod);
    const char *main_layout = LLVMGetDataLayoutStr(main_mod);
    if (main_triple && *main_triple)
      LLVMSetTarget(mod, main_triple);
    if (main_layout && *main_layout)
      LLVMSetDataLayout(mod, main_layout);
  }
  if (LLVMLinkModules2(main_mod, mod) != 0) {
    /*
     * LLVMLinkModules2 consumes the parsed module, including on failure. A
     * consumer-specific link failure does not make the shared cache invalid;
     * other modules and parallel test workers may still use it.
     */
    return false;
  }
  if (std_cache) {
    ny_drop_llvm_used_globals(main_mod);
    LLVMPassBuilderOptionsRef passes = LLVMCreatePassBuilderOptions();
    if (!passes)
      return false;
    LLVMErrorRef error = LLVMRunPasses(main_mod, "globaldce", NULL, passes);
    LLVMDisposePassBuilderOptions(passes);
    if (error) {
      char *message = LLVMGetErrorMessage(error);
      NY_LOG_ERR("Core cache reachability pruning failed: %s",
                 message ? message : "<unknown>");
      if (message)
        LLVMDisposeErrorMessage(message);
      return false;
    }
  }
  verify_msg = NULL;
  if (LLVMVerifyModule(main_mod, LLVMReturnStatusAction, &verify_msg) != 0) {
    if (verify_msg) {
      NY_LOG_WARN("Linked module cache verification failed: %s\n", verify_msg);
      LLVMDisposeMessage(verify_msg);
    }
    /*
     * The cache module was valid before linking; only this combined module
     * failed verification, so preserve the reusable input cache.
     */
    return false;
  }
  if (trace_timing) {
    ny_tick_t t_done = ny_ticks_now();
    fprintf(stderr,
            "[cache-timing] %s read/parse=%.3f verify=%.3f link/final-verify="
            "%.3f total=%.3f s\n",
            cache_path, ny_ticks_delta_sec(t_start, t_parsed),
            ny_ticks_delta_sec(t_parsed, t_verified),
            ny_ticks_delta_sec(t_verified, t_done),
            ny_ticks_delta_sec(t_start, t_done));
  }
  return true;
}

static bool ny_verify_bitcode(LLVMContextRef ctx, const char *bc_path) {
  if (!ctx || !bc_path)
    return false;
  LLVMMemoryBufferRef buf = NULL;
  char *msg = NULL;
  if (LLVMCreateMemoryBufferWithContentsOfFile(bc_path, &buf, &msg) != 0) {
    if (msg)
      LLVMDisposeMessage(msg);
    return false;
  }
  LLVMModuleRef mod = NULL;
  bool ok = (LLVMParseBitcodeInContext2(ctx, buf, &mod) == 0);
  if (!ok || !mod) {
    LLVMDisposeMemoryBuffer(buf);
    return false;
  }
  char *verify_msg = NULL;
  if (LLVMVerifyModule(mod, LLVMReturnStatusAction, &verify_msg) != 0) {
    if (verify_msg)
      LLVMDisposeMessage(verify_msg);
    LLVMDisposeModule(mod);
    return false;
  }
  LLVMDisposeModule(mod);
  return true;
}

static bool ny_reemit_bitcode_via_ir(LLVMModuleRef module,
                                     const char *bc_path) {
  if (!module || !bc_path)
    return false;
  char *ir = LLVMPrintModuleToString(module);
  if (!ir)
    return false;
  size_t ir_len = strlen(ir);
  LLVMMemoryBufferRef buf =
      LLVMCreateMemoryBufferWithMemoryRangeCopy(ir, ir_len, "nytrix_ir");
  LLVMDisposeMessage(ir);
  if (!buf)
    return false;
  LLVMContextRef fresh_ctx = LLVMContextCreate();
  LLVMModuleRef parsed = NULL;
  char *msg = NULL;
  if (!fresh_ctx || LLVMParseIRInContext(fresh_ctx, buf, &parsed, &msg) != 0) {
    if (msg)
      LLVMDisposeMessage(msg);
    LLVMDisposeMemoryBuffer(buf);
    if (fresh_ctx)
      LLVMContextDispose(fresh_ctx);
    return false;
  }
  bool ok = (LLVMWriteBitcodeToFile(parsed, bc_path) == 0);
  LLVMDisposeModule(parsed);
  if (fresh_ctx)
    LLVMContextDispose(fresh_ctx);
  return ok;
}

static char **ny_collect_import_names(const char *src, const char *entry_path,
                                      size_t *out_count) {
  lexer_t lx;
  lexer_init(&lx, src, "<collect_use>");
  lx.quiet = true;
  int depth = 0;
  int module_depths[128];
  int module_depth_count = 0;
  bool pending_module_brace = false;
  char **uses = NULL;
  size_t len = 0, cap = 0;
  token_t t = lexer_next(&lx);
  for (;;) {
    if (t.kind == NY_T_EOF)
      break;
    if (t.kind == NY_T_MODULE) {
      pending_module_brace = true;
      token_t mod_tok = lexer_next(&lx);
      token_t next_tok = mod_tok;
      char *module_name = NULL;
      if (mod_tok.kind == NY_T_IDENT || mod_tok.kind == NY_T_NUMBER)
        module_name = parse_use_name(&lx, &mod_tok, &next_tok);
      if (module_name && next_tok.kind == NY_T_LPAREN)
        next_tok = ny_collect_module_export_imports(
            &lx, &uses, &len, &cap, entry_path, module_name, next_tok);
      free(module_name);
      t = next_tok;
    } else if (t.kind == NY_T_LBRACE) {
      depth++;
      if (pending_module_brace) {
        if (module_depth_count <
            (int)(sizeof(module_depths) / sizeof(module_depths[0]))) {
          module_depths[module_depth_count++] = depth;
        }
        pending_module_brace = false;
      }
      t = lexer_next(&lx);
    } else if (t.kind == NY_T_RBRACE) {
      if (module_depth_count > 0 &&
          depth == module_depths[module_depth_count - 1]) {
        module_depth_count--;
      }
      if (depth > 0)
        depth--;
      pending_module_brace = false;
      t = lexer_next(&lx);
    } else if (t.kind == NY_T_USE && (depth == 0 || module_depth_count > 0)) {
      t = lexer_next(&lx);
      int use_line = t.line;
      for (;;) {
        token_t next_tok;
        char *name = parse_use_name(&lx, &t, &next_tok);
        if (name) {
          append_use(&uses, &len, &cap, name);
          free(name);
        }
        t = next_tok;
        int paren_depth = 0;
        /*
         * Only a trailing comma continues the use list (possibly onto later
         * lines).  A bare newline ends it: without this, the first ident
         * of the next line (e.g. print in `use core\nprint(...)`) was
         * collected as a bogus module seed.
         */
        bool comma_cont = false;
        while (t.kind != NY_T_EOF) {
          if (paren_depth == 0 && t.line != use_line)
            break;
          if (paren_depth == 0 && t.kind == NY_T_COMMA) {
            t = lexer_next(&lx);
            comma_cont = true;
            use_line = t.line;
            break;
          }
          if (paren_depth == 0 &&
              (t.kind == NY_T_USE || t.kind == NY_T_MODULE ||
               t.kind == NY_T_FN || t.kind == NY_T_STRUCT ||
               t.kind == NY_T_ENUM || t.kind == NY_T_EXTERN ||
               t.kind == NY_T_COMPTIME || t.kind == NY_T_DEF ||
               t.kind == NY_T_MUT || t.kind == NY_T_DEL ||
               t.kind == NY_T_RBRACE)) {
            break;
          }
          if (t.kind == NY_T_LPAREN)
            paren_depth++;
          else if (t.kind == NY_T_RPAREN && paren_depth > 0)
            paren_depth--;
          t = lexer_next(&lx);
        }
        if (t.kind == NY_T_EOF || !comma_cont)
          break;
      }
    } else {
      t = lexer_next(&lx);
    }
  }
  if (out_count)
    *out_count = len;
  return uses;
}

static const char *resolve_std_path(const char *compile_time_path) {
  const char *env = getenv("NYTRIX_STD_PREBUILT");
  if (env && *env && ny_access(env, R_OK) == 0)
    return env;
  env = getenv("NYTRIX_BUILD_STD_PATH");
  if (env && *env && ny_access(env, R_OK) == 0)
    return env;
  if (compile_time_path && ny_access(compile_time_path, R_OK) == 0)
    return compile_time_path;
  static char path[4096];
  if (ny_access("build/release/core.ny", R_OK) == 0) {
    snprintf(path, sizeof(path), "%s", "build/release/core.ny");
    return path;
  }
  if (ny_access("build/debug/core.ny", R_OK) == 0) {
    snprintf(path, sizeof(path), "%s", "build/debug/core.ny");
    return path;
  }
  if (ny_access("build/core.ny", R_OK) == 0) {
    snprintf(path, sizeof(path), "%s", "build/core.ny");
    return path;
  }
  char *exe_dir = ny_get_executable_dir();
  if (exe_dir) {
    snprintf(path, sizeof(path), "%s/core.ny", exe_dir);
    if (ny_access(path, R_OK) == 0)
      return path;
#ifdef NYTRIX_INSTALL_DATADIR
    snprintf(path, sizeof(path), "%s/../%s/nytrix/core.ny", exe_dir,
             NYTRIX_INSTALL_DATADIR);
#else
    snprintf(path, sizeof(path), "%s/../share/nytrix/core.ny", exe_dir);
#endif
    if (ny_access(path, R_OK) == 0)
      return path;
  }
  const char *root = ny_src_root();
  snprintf(path, sizeof(path), "%s/build/core.ny", root);
  if (ny_access(path, R_OK) == 0)
    return path;
  snprintf(path, sizeof(path), "%s/core.ny", root);
  if (ny_access(path, R_OK) == 0)
    return path;
  const char *common[] = {"/usr/share/nytrix/core.ny",
                          "/usr/local/share/nytrix/core.ny"};
  for (int i = 0; i < 2; i++)
    if (ny_access(common[i], R_OK) == 0)
      return common[i];
  return NULL;
}
