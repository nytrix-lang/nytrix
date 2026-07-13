static void dump_debug_bundle(const ny_options *opt, const char *source,
                              LLVMModuleRef module) {
  if (!opt || !opt->dump_on_error)
    return;
  ny_ensure_dir_recursive(ny_dump_dir(opt));
  char src_path[4096];
  char ir_path[4096];
  char asm_path[4096];
  ny_dump_path(src_path, sizeof(src_path), opt, "last_source.ny");
  ny_dump_path(ir_path, sizeof(ir_path), opt, "last_ir.ll");
  ny_dump_path(asm_path, sizeof(asm_path), opt, "last_asm.s");
  if (source) {
    ny_write_file(src_path, source, strlen(source));
  }
  if (module) {
    LLVMModuleRef dump_mod = ny_prepare_ir_dump_module(opt, module);
    char *err = NULL;
    if (LLVMPrintModuleToFile(dump_mod ? dump_mod : module, ir_path, &err) !=
        0) {
      if (err) {
        NY_LOG_ERR("Failed to write IR dump: %s\n", err);
        LLVMDisposeMessage(err);
      }
    }
    ny_llvm_emit_file(dump_mod ? dump_mod : module, asm_path, LLVMAssemblyFile,
                      opt->opt_level);
    ny_write_ir_stats_file(opt, "last_stats.txt", dump_mod ? dump_mod : module);
    if (dump_mod)
      LLVMDisposeModule(dump_mod);
  }
  NY_LOG_ERR("Debug bundle saved under %s/\n", ny_dump_dir(opt));
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
  if (!opt || !opt->output_file || opt->run_jit || !opt->emit_only)
    return false;
  if (opt->stop_after != NY_STOP_AFTER_NONE || opt->emit_artifact_path ||
      opt->collect_errors || opt->emit_shapes)
    return false;
  if (opt->dump_diagnose)
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
  if (opt->emit_only && !opt->output_file && !opt->run_jit && !opt->run_aot)
    return false;
  if (ny_opt_profile_kind_from_name(opt->opt_profile) ==
          NY_OPT_PROFILE_COMPILE &&
      !ny_env_enabled("NYTRIX_COMPILE_PROFILE_CACHE"))
    return false;
  if (opt->stop_after != NY_STOP_AFTER_NONE || opt->emit_artifact_path ||
      opt->collect_errors || opt->emit_shapes)
    return false;
  if (opt->compiler_asserts > 0 || ny_compiler_asserts_enabled())
    return false;
  if (opt->output_file && !opt->run_jit &&
      !ny_env_enabled("NYTRIX_AOT_IR_CACHE"))
    return false;
  if (opt->run_jit && !opt->output_file &&
      !ny_env_enabled("NYTRIX_JIT_CACHE_RUN"))
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

static bool ny_use_name_is_project_std_module(const char *name) {
  if (!name)
    return false;
  return strcmp(name, "std.demo") == 0 || strncmp(name, "std.demo.", 9) == 0;
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
      "src/rt/init.c",      "src/rt/ast.c",      "src/rt/bigint.c",
      "src/rt/core.c",      "src/rt/ffi.c",      "src/rt/ffigates.c",
      "src/rt/gc.c",        "src/rt/math.c",     "src/rt/memory.c",
      "src/rt/os.c",        "src/rt/simmd.c",
      "src/rt/string.c",    "src/rt/shared.h",   "src/rt/runtime.h",
      "src/rt/defs.h",      "src/parse/ast.h",   "src/parse/json.h",
      "src/parse/parser.h", "src/parse/lexer.h", "src/code/types.h",
      "src/base/common.h",  "src/base/compat.h",
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
  h = ny_fnv1a64_cstr("aot-cache-v10", h);
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
    const unsigned opt_fields[] = {
        (unsigned)opt->opt_level,       (unsigned)opt->debug_symbols,
        (unsigned)opt->strip_override,  (unsigned)opt->std_mode,
        (unsigned)opt->no_std,          (unsigned)opt->opt_dce,
        (unsigned)opt->opt_internalize, (unsigned)opt->opt_loops,
        (unsigned)opt->opt_autotune,    (unsigned)opt->ownership,
        (unsigned)opt->ownership_strict, (unsigned)opt->borrow_check};
    h = ny_hash_u32v(h, opt_fields, sizeof(opt_fields) / sizeof(opt_fields[0]));
  }
  h = ny_fnv1a64_cstr(opt->opt_pipeline, h);
  h = ny_fnv1a64_cstr(ny_builder_choose_cc(), h);
  h = ny_hash_cstrv(h, (const char *const *)opt->link_dirs.data,
                    opt->link_dirs.len);
  h = ny_hash_cstrv(h, (const char *const *)opt->link_libs.data,
                    opt->link_libs.len);
  {
    const char *const host_envs[] = {
        "NYTRIX_HOST_CFLAGS",
        "NYTRIX_HOST_LDFLAGS",
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
    char *exe_path = ny_get_executable_path();
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
  h = ny_fnv1a64_cstr("std-cache-v10", h);
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
    const char *const envs[] = {"NYTRIX_HOST_TRIPLE", "NYTRIX_HOST_CFLAGS",
                                "NYTRIX_HOST_LDFLAGS", "NYTRIX_ARM_FLOAT_ABI",
                                "NYTRIX_ASSUME_INT"};
    h = ny_hash_envv(h, envs, sizeof(envs) / sizeof(envs[0]));
  }
  h = ny_fnv1a64_cstr(ny_src_root(), h);
  h = ny_fnv1a64_cstr(opt ? opt->argv0 : NULL, h);
  h = ny_hash64_u64(h,
                    (uint64_t)ny_file_mtime_or_zero(opt ? opt->argv0 : NULL));

  h = ny_fnv1a64_cstr(opt ? opt->input_file : NULL, h);
  h = ny_hash64_u64(
      h, (uint64_t)ny_file_mtime_or_zero(opt ? opt->input_file : NULL));
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

static bool ny_ir_is_std_symbol(const char *name);
static bool ny_is_llvm_special_global(const char *name);
static bool ny_ir_is_std_value(LLVMValueRef v);
static void ny_build_llvm_used(LLVMModuleRef module, const LLVMValueRef *values,
                               size_t count);

static bool ny_ir_is_string_global(const char *name) {
  return name && (strncmp(name, ".str.data.", 10) == 0 ||
                  strncmp(name, ".str.runtime.", 13) == 0);
}

static bool ny_std_bc_symbol_is_mixed_codegen_artifact(const char *name) {
  if (!name || !*name)
    return false;
  /* The stdlib cache is linked into unrelated user programs.  A script entry
   * can only come from the user half of the joined module; accepting it here
   * makes the first program that populates the cache run for every later
   * source with the same import set. */
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
  /* Every referenced function/global is also present in its module symbol
   * list, including declarations.  Checking those lists once proves that no
   * mixed-codegen artifact can be reached without recursively revisiting
   * every instruction and constant operand in the module. */
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

static void ny_preserve_std_values_for_dce(LLVMModuleRef module) {
  if (!module)
    return;
  VEC(LLVMValueRef) values;
  vec_init(&values);
  for (LLVMValueRef fn = LLVMGetFirstFunction(module); fn;
       fn = LLVMGetNextFunction(fn)) {
    if (ny_ir_is_std_value(fn) && LLVMCountBasicBlocks(fn) > 0)
      vec_push(&values, fn);
  }
  for (LLVMValueRef gv = LLVMGetFirstGlobal(module); gv;
       gv = LLVMGetNextGlobal(gv)) {
    if (LLVMIsDeclaration(gv))
      continue;
    if (ny_ir_is_std_value(gv))
      vec_push(&values, gv);
  }
  if (values.len)
    ny_build_llvm_used(module, values.data, values.len);
  vec_free(&values);
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
    if (lib && *lib)
      total += strlen(lib) + 1;
  }
  char *buf = malloc(total ? total : 1);
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
  /* Write the module to bitcode directly without LLVMCloneModule.  The old
   * path cloned the entire monolithic stdlib module (often > 10 GiB) just to
   * strip debug info, externalize non-std symbols, and run globaldce before
   * serialization.  That doubled peak RSS for no correctness benefit: the
   * load path already runs its own link-safety check, and the module will be
   * optimized after being linked back in.  Writing the original saves a full
   * module copy. */
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
  char **names;
  size_t len;
  size_t cap;
} ny_module_list;

typedef struct {
  char *name;
  char *bc_path;
#ifndef _WIN32
  pid_t pid;
#endif
  int exit_code;
} ny_module_job;

static void ny_module_list_add(ny_module_list *list, const char *name) {
  if (!list || !name || !*name)
    return;
  for (size_t i = 0; i < list->len; i++) {
    if (strcmp(list->names[i], name) == 0)
      return;
  }
  if (list->len == list->cap) {
    size_t nc = list->cap ? list->cap * 2 : 8;
    char **nn = realloc(list->names, nc * sizeof(char *));
    if (!nn)
      return;
    list->names = nn;
    list->cap = nc;
  }
  list->names[list->len++] = ny_strdup(name);
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
    free(list->names[i]);
  free(list->names);
  list->names = NULL;
  list->len = list->cap = 0;
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
  /* A module worker currently reparses and rebuilds the complete joined
   * source, multiplying LLVM memory by the worker count. It also cannot yet
   * preserve every cross-module lazy/capture dependency. Keep that experiment
   * behind the explicit modules mode; auto uses bounded outer/test parallelism
   * and the single shared compiler pipeline. */
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
  job->name = NULL;
  job->bc_path = NULL;
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

static bool ny_spawn_module_job(const ny_options *opt, const char *module_name,
                                const char *tmp_dir, ny_module_job *job) {
  if (!opt || !module_name || !tmp_dir || !job)
    return false;
  char *san = ny_sanitize_modname(module_name);
  if (!san)
    return false;
  static unsigned long long ny_mod_seq = 0;
  char bc_path[1024];
  unsigned long long seq = ++ny_mod_seq;
  snprintf(bc_path, sizeof(bc_path), "%s/ny_mod_%s_%ld_%llu.bc", tmp_dir, san,
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
  const char *argv[20];
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
  if (opt->opt_pipeline && *opt->opt_pipeline) {
    argv[idx++] = "-passes";
    argv[idx++] = opt->opt_pipeline;
  }
  if (opt->no_std)
    argv[idx++] = "--no-std";
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
  job->pid = pid;
  job->exit_code = -1;
  return true;
}

#endif

static bool ny_link_module_cache(LLVMContextRef ctx, LLVMModuleRef main_mod,
                                 const char *cache_path) {
  if (!ctx || !main_mod || !cache_path)
    return false;
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
    (void)unlink(cache_path);
    return false;
  }
  LLVMStripModuleDebugInfo(mod);

  char *verify_msg = NULL;
  if (LLVMVerifyModule(mod, LLVMReturnStatusAction, &verify_msg) != 0) {
    if (verify_msg)
      LLVMDisposeMessage(verify_msg);
    LLVMDisposeModule(mod);
    (void)unlink(cache_path);
    return false;
  }
  const char *bad_symbol = NULL;
  if (!ny_std_bc_module_is_link_safe(mod, &bad_symbol)) {
    if (bad_symbol && *bad_symbol)
      NY_LOG_WARN("Ignoring unsafe std cache %s: mixed codegen artifact %s\n",
                  cache_path, bad_symbol);
    LLVMDisposeModule(mod);
    (void)unlink(cache_path);
    return false;
  }
  if (LLVMLinkModules2(main_mod, mod) != 0) {
    LLVMDisposeModule(mod);
    (void)unlink(cache_path);
    return false;
  }
  verify_msg = NULL;
  if (LLVMVerifyModule(main_mod, LLVMReturnStatusAction, &verify_msg) != 0) {
    if (verify_msg) {
      NY_LOG_WARN("Linked module cache verification failed: %s\n",
                  verify_msg);
      LLVMDisposeMessage(verify_msg);
    }
    (void)unlink(cache_path);
    return false;
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
        while (t.kind != NY_T_EOF) {
          if (paren_depth == 0 && t.line != use_line)
            break;
          if (paren_depth == 0 && t.kind == NY_T_COMMA) {
            t = lexer_next(&lx);
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
        if (t.kind == NY_T_EOF ||
            (t.kind != NY_T_IDENT && t.kind != NY_T_STRING))
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
  if (ny_access("build/release/std.ny", R_OK) == 0) {
    snprintf(path, sizeof(path), "%s", "build/release/std.ny");
    return path;
  }
  if (ny_access("build/debug/std.ny", R_OK) == 0) {
    snprintf(path, sizeof(path), "%s", "build/debug/std.ny");
    return path;
  }
  if (ny_access("build/std.ny", R_OK) == 0) {
    snprintf(path, sizeof(path), "%s", "build/std.ny");
    return path;
  }
  char *exe_dir = ny_get_executable_dir();
  if (exe_dir) {
    snprintf(path, sizeof(path), "%s/std.ny", exe_dir);
    if (ny_access(path, R_OK) == 0)
      return path;
    snprintf(path, sizeof(path), "%s/../share/nytrix/std.ny", exe_dir);
    if (ny_access(path, R_OK) == 0)
      return path;
  }
  const char *root = ny_src_root();
  snprintf(path, sizeof(path), "%s/build/std.ny", root);
  if (ny_access(path, R_OK) == 0)
    return path;
  snprintf(path, sizeof(path), "%s/std.ny", root);
  if (ny_access(path, R_OK) == 0)
    return path;
  const char *common[] = {"/usr/share/nytrix/std.ny",
                          "/usr/local/share/nytrix/std.ny"};
  for (int i = 0; i < 2; i++)
    if (ny_access(common[i], R_OK) == 0)
      return common[i];
  return NULL;
}
