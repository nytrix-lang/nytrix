#include "wire/pipe.h"
#include "base/common.h"
#include "base/loader.h"
#include "base/util.h"
#include "code/code.h"
#include "code/jit.h"
#include "code/llvm.h"
#include "parse/parser.h"
#include "repl/repl.h"
#include "wire/build.h"
#include "wire/cache.h"
#include <llvm-c/Analysis.h>
#include <llvm-c/DebugInfo.h>
#include <llvm-c/Error.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Linker.h>
#include <llvm-c/Support.h>
#include <llvm-c/Transforms/PassBuilder.h>
#include <llvm/Config/llvm-config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifndef _WIN32
#include <unistd.h>
#else
#include <io.h>
#endif

typedef struct ny_ir_stats_t {
  uint64_t funcs;
  uint64_t blocks;
  uint64_t insts;
  uint64_t allocas;
  uint64_t phis;
} ny_ir_stats_t;

typedef enum ny_opt_profile_kind_t {
  NY_OPT_PROFILE_DEFAULT = 0,
  NY_OPT_PROFILE_SPEED,
  NY_OPT_PROFILE_BALANCED,
  NY_OPT_PROFILE_COMPILE,
  NY_OPT_PROFILE_NONE,
  NY_OPT_PROFILE_SIZE,
  NY_OPT_PROFILE_CUSTOM,
} ny_opt_profile_kind_t;

static bool ny_env_enabled_strict(const char *name) {
  const char *v = getenv(name);
  if (!v || !*v)
    return false;
  return (strcmp(v, "1") == 0 || strcmp(v, "true") == 0 ||
          strcmp(v, "True") == 0 || strcmp(v, "yes") == 0 ||
          strcmp(v, "on") == 0 || strcmp(v, "y") == 0 || strcmp(v, "Y") == 0);
}

static bool ny_valid_native_artifact(const char *path) {
  if (!path || !*path)
    return false;
  struct stat st;
  if (stat(path, &st) != 0)
    return false;
  if (st.st_size <= 0)
    return false;
#ifndef _WIN32
  /* Ensure cached ELF artifacts are actually runnable. */
  if (access(path, X_OK) != 0)
    return false;
#endif
  FILE *f = fopen(path, "rb");
  if (!f)
    return false;
  unsigned char hdr[4] = {0, 0, 0, 0};
  size_t n = fread(hdr, 1, sizeof(hdr), f);
  fclose(f);
  if (n < 2)
    return false;
#ifdef _WIN32
  return hdr[0] == 'M' && hdr[1] == 'Z';
#else
  return n >= 4 && hdr[0] == 0x7f && hdr[1] == 'E' && hdr[2] == 'L' &&
         hdr[3] == 'F';
#endif
}

static void ny_collect_ir_stats(LLVMModuleRef module, ny_ir_stats_t *out) {
  if (!out) {
    return;
  }
  memset(out, 0, sizeof(*out));
  if (!module) {
    return;
  }
  for (LLVMValueRef fn = LLVMGetFirstFunction(module); fn;
       fn = LLVMGetNextFunction(fn)) {
    if (LLVMCountBasicBlocks(fn) == 0) {
      continue;
    }
    out->funcs++;
    for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(fn); bb;
         bb = LLVMGetNextBasicBlock(bb)) {
      out->blocks++;
      for (LLVMValueRef inst = LLVMGetFirstInstruction(bb); inst;
           inst = LLVMGetNextInstruction(inst)) {
        LLVMOpcode opc = LLVMGetInstructionOpcode(inst);
        out->insts++;
        if (opc == LLVMAlloca) {
          out->allocas++;
        } else if (opc == LLVMPHI) {
          out->phis++;
        }
      }
    }
  }
}

static void dump_debug_bundle(const ny_options *opt, const char *source,
                              LLVMModuleRef module) {
  if (!opt || !opt->dump_on_error)
    return;
  ny_ensure_dir("build");
  ny_ensure_dir("build/debug");
  if (source) {
    ny_write_file("build/debug/last_source.ny", source, strlen(source));
  }
  if (module) {
    LLVMModuleRef dump_mod = LLVMCloneModule(module);
    if (dump_mod)
      LLVMStripModuleDebugInfo(dump_mod);
    char *err = NULL;
    if (LLVMPrintModuleToFile(dump_mod ? dump_mod : module,
                              "build/debug/last_ir.ll", &err) != 0) {
      if (err) {
        NY_LOG_ERR("Failed to write IR dump: %s\n", err);
        LLVMDisposeMessage(err);
      }
    }
    ny_llvm_emit_file(dump_mod ? dump_mod : module, "build/debug/last_asm.s",
                      LLVMAssemblyFile);
    if (dump_mod)
      LLVMDisposeModule(dump_mod);
  }
  NY_LOG_ERR("Debug bundle saved under build/debug/\n");
  {
    const size_t max_lines = 14;
    const char *paths[] = {"build/debug/last_ir.ll", "build/debug/last_asm.s"};
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
    return raw;
  return buf;
}
#endif

static bool ny_env_enabled_default_on(const char *name) {
  const char *v = getenv(name);
  if (!v || !*v)
    return true;
  return ny_env_is_truthy(v);
}

static ny_opt_profile_kind_t
ny_opt_profile_kind_from_name(const char *profile_name) {
  if (!profile_name || !*profile_name)
    return NY_OPT_PROFILE_DEFAULT;
  if (strcasecmp(profile_name, "default") == 0)
    return NY_OPT_PROFILE_DEFAULT;
  if (strcasecmp(profile_name, "speed") == 0)
    return NY_OPT_PROFILE_SPEED;
  if (strcasecmp(profile_name, "balanced") == 0)
    return NY_OPT_PROFILE_BALANCED;
  if (strcasecmp(profile_name, "compile") == 0)
    return NY_OPT_PROFILE_COMPILE;
  if (strcasecmp(profile_name, "none") == 0)
    return NY_OPT_PROFILE_NONE;
  if (strcasecmp(profile_name, "size") == 0)
    return NY_OPT_PROFILE_SIZE;
  return NY_OPT_PROFILE_CUSTOM;
}

static const char *ny_opt_profile_name(ny_opt_profile_kind_t kind,
                                       const char *custom_name) {
  switch (kind) {
  case NY_OPT_PROFILE_SPEED:
    return "speed";
  case NY_OPT_PROFILE_BALANCED:
    return "balanced";
  case NY_OPT_PROFILE_COMPILE:
    return "compile";
  case NY_OPT_PROFILE_NONE:
    return "none";
  case NY_OPT_PROFILE_SIZE:
    return "size";
  case NY_OPT_PROFILE_CUSTOM:
    return (custom_name && *custom_name) ? custom_name : "default";
  case NY_OPT_PROFILE_DEFAULT:
  default:
    return "default";
  }
}

static int ny_env_int(const char *name, int fallback) {
  const char *v = getenv(name);
  if (!v || !*v)
    return fallback;
  char *end = NULL;
  long n = strtol(v, &end, 10);
  if (!end || *end != '\0')
    return fallback;
  if (n < -1)
    return -1;
  if (n > 100000)
    return 100000;
  return (int)n;
}

static int ny_guided_inline_threshold(ny_opt_profile_kind_t profile_kind,
                                      int eff_opt_level) {
  int explicit_thr = ny_env_int("NYTRIX_INLINE_THRESHOLD", -2);
  if (explicit_thr >= -1)
    return explicit_thr;

  const char *guided = getenv("NYTRIX_GUIDED_INLINE");
  if (guided && *guided) {
    if (strcasecmp(guided, "off") == 0)
      return -1;
    if (strcasecmp(guided, "conservative") == 0)
      return 85;
    if (strcasecmp(guided, "balanced") == 0)
      return 175;
    if (strcasecmp(guided, "aggressive") == 0)
      return 325;
  }

  switch (profile_kind) {
  case NY_OPT_PROFILE_SPEED:
    return 325;
  case NY_OPT_PROFILE_BALANCED:
    return 200;
  case NY_OPT_PROFILE_SIZE:
    return 75;
  case NY_OPT_PROFILE_COMPILE:
  case NY_OPT_PROFILE_NONE:
    return 25;
  default:
    break;
  }

  if (eff_opt_level >= 3)
    return 275;
  if (eff_opt_level >= 2)
    return 200;
  if (eff_opt_level == 1)
    return 125;
  return 50;
}

static bool ny_should_use_aot_cache(const ny_options *opt) {
  if (!opt || !opt->output_file || opt->run_jit || !opt->emit_only)
    return false;
  if (!ny_env_enabled_default_on("NYTRIX_AOT_CACHE"))
    return false;
  if (opt->do_timing || opt->dump_ast || opt->dump_llvm || opt->dump_tokens ||
      opt->dump_docs || opt->dump_funcs || opt->dump_symbols ||
      opt->dump_stats || opt->emit_ir_path || opt->emit_asm_path)
    return false;
  return true;
}

static time_t ny_file_mtime_or_zero(const char *path) {
  if (!path || !*path)
    return 0;
  struct stat st;
  if (stat(path, &st) != 0)
    return 0;
  return st.st_mtime;
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
      "src/rt/init.c",    "src/rt/ast.c",       "src/rt/core.c",
      "src/rt/ffi.c",     "src/rt/math.c",      "src/rt/memory.c",
      "src/rt/os.c",      "src/rt/string.c",    "src/rt/shared.h",
      "src/rt/runtime.h", "src/rt/defs.h",      "src/ast/ast.h",
      "src/ast/json.h",   "src/parse/parser.h", "src/lex/lexer.h",
      "src/code/types.h", "src/base/common.h",  "src/base/compat.h",
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

static uint64_t ny_hash_mix_u32v(uint64_t h, const unsigned *vals, size_t len) {
  if (!vals)
    return h;
  for (size_t i = 0; i < len; ++i)
    h = ny_hash64_u64(h, (uint64_t)vals[i]);
  return h;
}

static uint64_t ny_hash_mix_u64v(uint64_t h, const uint64_t *vals, size_t len) {
  if (!vals)
    return h;
  for (size_t i = 0; i < len; ++i)
    h = ny_hash64_u64(h, vals[i]);
  return h;
}

static uint64_t ny_hash_mix_cstrv(uint64_t h, const char *const *vals,
                                  size_t len) {
  if (!vals)
    return h;
  for (size_t i = 0; i < len; ++i)
    h = ny_fnv1a64_cstr(vals[i], h);
  return h;
}

static uint64_t ny_hash_mix_envv(uint64_t h, const char *const *env_names,
                                 size_t len) {
  if (!env_names)
    return h;
  for (size_t i = 0; i < len; ++i)
    h = ny_fnv1a64_cstr(getenv(env_names[i]), h);
  return h;
}

static void ny_build_aot_cache_path(const ny_options *opt, const char *source,
                                    const char *parse_name,
                                    const char *std_bundle_path,
                                    const char *output_path, char *out,
                                    size_t out_len) {
  if (!out || out_len == 0) {
    return;
  }
  out[0] = '\0';
  if (!opt || !source || !output_path)
    return;
  uint64_t h = NY_FNV1A64_OFFSET_BASIS;
  h = ny_fnv1a64_cstr("aot-cache-v2", h);
  h = ny_fnv1a64_cstr(source, h);
  h = ny_fnv1a64_cstr(parse_name ? parse_name : "<inline>", h);
  {
    const unsigned opt_fields[] = {
        (unsigned)opt->opt_level, (unsigned)opt->debug_symbols,
        (unsigned)opt->strip_override, (unsigned)opt->std_mode,
        (unsigned)opt->no_std};
    h = ny_hash_mix_u32v(h, opt_fields,
                         sizeof(opt_fields) / sizeof(opt_fields[0]));
  }
  h = ny_fnv1a64_cstr(opt->opt_pipeline, h);
  h = ny_fnv1a64_cstr(ny_builder_choose_cc(), h);
  h = ny_hash_mix_cstrv(h, (const char *const *)opt->link_dirs.data,
                        opt->link_dirs.len);
  h = ny_hash_mix_cstrv(h, (const char *const *)opt->link_libs.data,
                        opt->link_libs.len);
  {
    const char *const host_envs[] = {"NYTRIX_HOST_CFLAGS",
                                     "NYTRIX_HOST_LDFLAGS"};
    h = ny_hash_mix_envv(h, host_envs,
                         sizeof(host_envs) / sizeof(host_envs[0]));
  }
  h = ny_fnv1a64_cstr(std_bundle_path, h);
  {
    const uint64_t mtimes[] = {
        (uint64_t)ny_file_mtime_or_zero(opt->argv0),
        (uint64_t)ny_file_mtime_or_zero(std_bundle_path),
        (uint64_t)ny_runtime_latest_mtime(ny_src_root()),
    };
    h = ny_hash_mix_u64v(h, mtimes, sizeof(mtimes) / sizeof(mtimes[0]));
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

  latest = 0;
  size_t count = ny_std_module_count();
  for (size_t i = 0; i < count; ++i) {
    const char *p = ny_std_module_path(i);
    time_t mt = ny_file_mtime_or_zero(p);
    if (mt > latest)
      latest = mt;
  }

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

static int ny_write_file_atomic(const char *path, const char *content,
                                size_t len) {
  if (!path || !*path || !content)
    return -1;

  char tmp[4096];
  snprintf(tmp, sizeof(tmp), "%s.tmp.%ld.%lu", path, (long)getpid(),
           (unsigned long)clock());

  FILE *f = fopen(tmp, "wb");
  if (!f)
    return -1;

  size_t written = fwrite(content, 1, len, f);
  int close_rc = fclose(f);
  if (written != len || close_rc != 0) {
    (void)unlink(tmp);
    return -1;
  }

#ifdef _WIN32
  /* Replace destination on Windows where rename() cannot overwrite in place. */
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
  snprintf(std_src, sizeof(std_src), "%s/src/std", root);
  if (stat(std_src, &st) == 0 && S_ISDIR(st.st_mode))
    return true;
  snprintf(std_src, sizeof(std_src), "%s/std", root);
  if (stat(std_src, &st) == 0 && S_ISDIR(st.st_mode))
    return true;
  return false;
}

static void ny_build_std_cache_path(const ny_options *opt,
                                    const char *const *uses, size_t use_count,
                                    std_mode_t std_mode,
                                    const char *prebuilt_path, char *out,
                                    size_t out_len) {
  if (!out || out_len == 0)
    return;
  out[0] = '\0';

  uint64_t h = NY_FNV1A64_OFFSET_BASIS;
  h = ny_fnv1a64_cstr("std-cache-v3", h);
  h = ny_hash64_u64(h, (uint64_t)std_mode);
  h = ny_hash_mix_cstrv(h, uses, use_count);
  h = ny_fnv1a64_cstr(prebuilt_path, h);
  h = ny_hash64_u64(h, (uint64_t)ny_std_latest_mtime());

  {
    const char *const envs[] = {"NYTRIX_HOST_TRIPLE", "NYTRIX_HOST_CFLAGS",
                                "NYTRIX_HOST_LDFLAGS", "NYTRIX_ARM_FLOAT_ABI"};
    h = ny_hash_mix_envv(h, envs, sizeof(envs) / sizeof(envs[0]));
  }

  h = ny_fnv1a64_cstr(ny_src_root(), h);
  h = ny_fnv1a64_cstr(opt ? opt->argv0 : NULL, h);
  h = ny_hash64_u64(h,
                    (uint64_t)ny_file_mtime_or_zero(opt ? opt->argv0 : NULL));

  const char *tmp = ny_get_temp_dir();
  snprintf(out, out_len, "%s/ny_std_cache_%016llx.ny", tmp,
           (unsigned long long)h);
}
static char *dup_string_token(token_t t) {
  if (t.len < 2)
    return NULL;
  size_t head = 1, tail = 1;
  if (t.len >= 6 && t.lexeme[0] == t.lexeme[1] && t.lexeme[1] == t.lexeme[2]) {
    head = 3;
    tail = 3;
  }
  if (t.len < head + tail)
    return NULL;
  size_t out_len = t.len - head - tail;
  char *out = malloc(out_len + 1);
  if (!out)
    return NULL;
  memcpy(out, t.lexeme + head, out_len);
  out[out_len] = '\0';
  return out;
}

static char *parse_use_name(lexer_t *lx, token_t *entry_tok,
                            token_t *out_last_tok) {
  token_t t = *entry_tok;
  if (t.kind == NY_T_STRING) {
    char *name = dup_string_token(t);
    if (out_last_tok)
      *out_last_tok = lexer_next(lx);
    return name;
  }
  if (t.kind != NY_T_IDENT)
    return NULL;
  size_t cap = 64, len = 0;
  char *buf = malloc(cap);
  if (!buf)
    return NULL;
  memcpy(buf, t.lexeme, t.len);
  len += t.len;
  for (;;) {
    token_t tok = lexer_next(lx);
    if (tok.kind == NY_T_DOT) {
      token_t id = lexer_next(lx);
      if (id.kind != NY_T_IDENT) {
        free(buf);
        return NULL;
      }
      if (len + 1 + id.len + 1 > cap) {
        cap = (len + 1 + id.len + 1) * 2;
        char *nb = realloc(buf, cap);
        if (!nb) {
          free(buf);
          return NULL;
        }
        buf = nb;
      }
      buf[len++] = '.';
      memcpy(buf + len, id.lexeme, id.len);
      len += id.len;
    } else {
      if (out_last_tok)
        *out_last_tok = tok;
      break;
    }
  }
  buf[len] = '\0';
  return buf;
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

static char **collect_use_modules(const char *src, size_t *out_count) {
  lexer_t lx;
  lexer_init(&lx, src, "<collect_use>");
  int depth = 0;
  char **uses = NULL;
  size_t len = 0, cap = 0;
  token_t t = lexer_next(&lx);
  for (;;) {
    if (t.kind == NY_T_EOF)
      break;
    if (t.kind == NY_T_LBRACE) {
      depth++;
      t = lexer_next(&lx);
    } else if (t.kind == NY_T_RBRACE) {
      if (depth > 0)
        depth--;
      t = lexer_next(&lx);
    } else if (t.kind == NY_T_USE && depth == 0) {
      t = lexer_next(&lx);
      token_t next_tok;
      char *name = parse_use_name(&lx, &t, &next_tok);
      if (name) {
        append_use(&uses, &len, &cap, name);
        free(name);
      }
      t = next_tok;
    } else {
      t = lexer_next(&lx);
    }
  }
  if (out_count)
    *out_count = len;
  return uses;
}

static const char *resolve_std_bundle(const char *compile_time_path) {
  const char *env = getenv("NYTRIX_STD_PREBUILT");
  if (env && *env && access(env, R_OK) == 0)
    return env;
  env = getenv("NYTRIX_BUILD_STD_PATH");
  if (env && *env && access(env, R_OK) == 0)
    return env;

  static char path[4096];
  // 0. Check relative to current directory
  if (access("build/release/std.ny", R_OK) == 0) {
    strcpy(path, "build/release/std.ny");
    return path;
  }
  if (access("build/debug/std.ny", R_OK) == 0) {
    strcpy(path, "build/debug/std.ny");
    return path;
  }
  if (access("build/std.ny", R_OK) == 0) {
    strcpy(path, "build/std.ny");
    return path;
  }
  // 1. Check relative to binary
  char *exe_dir = ny_get_executable_dir();
  if (exe_dir) {
    snprintf(path, sizeof(path), "%s/std.ny", exe_dir);
    if (access(path, R_OK) == 0)
      return path;
    snprintf(path, sizeof(path), "%s/../share/nytrix/std.ny", exe_dir);
    if (access(path, R_OK) == 0)
      return path;
  }

  // 2. Check source root
  const char *root = ny_src_root();
  snprintf(path, sizeof(path), "%s/build/std.ny", root);
  if (access(path, R_OK) == 0)
    return path;
  snprintf(path, sizeof(path), "%s/std.ny", root);
  if (access(path, R_OK) == 0)
    return path;

  // 3. Fallback to compile-time path
  if (compile_time_path && access(compile_time_path, R_OK) == 0)
    return compile_time_path;

  // 4. Hardcoded common paths
  const char *common[] = {"/usr/share/nytrix/std_bundle.ny",
                          "/usr/local/share/nytrix/std_bundle.ny"};
  for (int i = 0; i < 2; i++)
    if (access(common[i], R_OK) == 0)
      return common[i];

  return NULL;
}

static void ensure_aot_entry(codegen_t *cg, LLVMValueRef script_fn) {
  if (!cg || !cg->module || !script_fn)
    return;
  if (LLVMGetNamedFunction(cg->module, "main"))
    return;
  // Generate: int main(int argc, char **argv, char **envp) {
  //   __set_args((int64_t)argc, (int64_t)argv, (int64_t)envp);
  //   return (int)script_fn();
  // }
  LLVMTypeRef i32 = LLVMInt32TypeInContext(cg->ctx);
  LLVMTypeRef i64 = LLVMInt64TypeInContext(cg->ctx);
  LLVMTypeRef ptr = LLVMPointerType(LLVMInt8TypeInContext(cg->ctx), 0);
  LLVMTypeRef ptrptr = LLVMPointerType(ptr, 0);

  LLVMTypeRef main_ty =
      LLVMFunctionType(i32, (LLVMTypeRef[]){i32, ptrptr, ptrptr}, 3, 0);
  LLVMValueRef main_fn = LLVMAddFunction(cg->module, "main", main_ty);
  LLVMBasicBlockRef entry =
      LLVMAppendBasicBlockInContext(cg->ctx, main_fn, "entry");
  LLVMBuilderRef builder = LLVMCreateBuilderInContext(cg->ctx);
  LLVMPositionBuilderAtEnd(builder, entry);

  LLVMValueRef argc = LLVMGetParam(main_fn, 0);
  LLVMValueRef argv = LLVMGetParam(main_fn, 1);
  LLVMValueRef envp = LLVMGetParam(main_fn, 2);

  LLVMValueRef argc_i64 = LLVMBuildSExt(builder, argc, i64, "");
  LLVMValueRef argv_i64 = LLVMBuildPtrToInt(builder, argv, i64, "");
  LLVMValueRef envp_i64 = LLVMBuildPtrToInt(builder, envp, i64, "");

  // Call __set_args
  LLVMValueRef set_args_fn = LLVMGetNamedFunction(cg->module, "__set_args");
  if (!set_args_fn) {
    // Look it up from builtin defs/internal declarations if possible, or
    // declare it
    LLVMTypeRef set_args_ty =
        LLVMFunctionType(i64, (LLVMTypeRef[]){i64, i64, i64}, 3, 0);
    set_args_fn = LLVMAddFunction(cg->module, "__set_args", set_args_ty);
  }
  LLVMBuildCall2(builder, LLVMGlobalGetValueType(set_args_fn), set_args_fn,
                 (LLVMValueRef[]){argc_i64, argv_i64, envp_i64}, 3, "");

  // Call script
  LLVMValueRef res_raw = LLVMBuildCall2(
      builder, LLVMGlobalGetValueType(script_fn), script_fn, NULL, 0, "");
  // Call __runtime_cleanup to release runtime-owned allocations before exit.
  LLVMValueRef cleanup_fn =
      LLVMGetNamedFunction(cg->module, "__runtime_cleanup");
  if (!cleanup_fn) {
    LLVMTypeRef cleanup_ty = LLVMFunctionType(i64, NULL, 0, 0);
    cleanup_fn = LLVMAddFunction(cg->module, "__runtime_cleanup", cleanup_ty);
  }
  LLVMBuildCall2(builder, LLVMGlobalGetValueType(cleanup_fn), cleanup_fn, NULL,
                 0, "");
  // result is tagged int64. convert to exit code (int32)
  LLVMValueRef res_int =
      LLVMBuildAShr(builder, res_raw, LLVMConstInt(i64, 1, 0), "");
  LLVMValueRef res_i32 = LLVMBuildTrunc(builder, res_int, i32, "");

  LLVMBuildRet(builder, res_i32);
  LLVMDisposeBuilder(builder);
}

static void maybe_log_phase_time(bool enabled, const char *label,
                                 clock_t start_time) {
  if (!enabled)
    return;
  fprintf(stderr, "%-12s %.4fs\n", label,
          (double)(clock() - start_time) / CLOCKS_PER_SEC);
}

static bool handle_non_compile_modes(const ny_options *opt, int *exit_code) {
  if (opt->mode == NY_MODE_VERSION) {
    printf("Nytrix v0.1.5\n");
    *exit_code = 0;
    return true;
  }
  if (opt->mode == NY_MODE_HELP) {
    if (opt->help_env)
      ny_options_usage_env(opt->argv0 ? opt->argv0 : "ny");
    else
      ny_options_usage(opt->argv0 ? opt->argv0 : "ny");
    *exit_code = 0;
    return true;
  }
  if (opt->mode == NY_MODE_REPL) {
    LLVMLinkInMCJIT();
    ny_llvm_init_native();
    LLVMLoadLibraryPermanently(NULL);
    std_mode_t repl_std_mode = opt->no_std ? STD_MODE_NONE : opt->std_mode;
    ny_repl_set_std_mode(repl_std_mode);
    ny_repl_set_plain(opt->repl_plain ? 1 : 0);
    /*
     * Piped stdin (`-repl < file`) is used heavily by the test runner.
     * Treat it as batch REPL to skip interactive-only setup overhead.
     */
    int repl_batch = 0;
#ifdef _WIN32
    repl_batch = (_isatty(_fileno(stdin)) == 0);
#else
    repl_batch = (isatty(STDIN_FILENO) == 0);
#endif
    ny_repl_run(opt->opt_level, opt->opt_pipeline, opt->command_string,
                repl_batch);
    *exit_code = 0;
    return true;
  }
  return false;
}

static char *load_user_source(const ny_options *opt) {
  if (opt->command_string)
    return ny_strdup(opt->command_string);
  if (opt->input_file)
    return ny_read_file(opt->input_file);
  return ny_strdup("fn main() { return 0\n }");
}

static bool verify_module_if_needed(const ny_options *opt,
                                    LLVMModuleRef module) {
  if (!opt->verify_module)
    return true;
  char *err = NULL;
  if (LLVMVerifyModule(module, LLVMPrintMessageAction, &err)) {
    NY_LOG_ERR("Verification failed: %s\n", err);
    LLVMDisposeMessage(err);
    return false;
  }
  return true;
}

static void ny_dump_ir_if_requested(LLVMModuleRef module, const char *path,
                                    const char *stage) {
  if (!module || !path || !*path)
    return;
  char *err = NULL;
  if (LLVMPrintModuleToFile(module, path, &err) != 0) {
    NY_LOG_WARN("failed to write %s IR to %s: %s\n", stage ? stage : "module",
                path, err ? err : "<unknown>");
    if (err)
      LLVMDisposeMessage(err);
    return;
  }
  if (err)
    LLVMDisposeMessage(err);
}

static bool ny_ir_is_std_symbol(const char *name) {
  if (!name || !*name)
    return false;
  return (strncmp(name, "std.", 4) == 0 || strncmp(name, "lib.", 4) == 0 ||
          strncmp(name, "src.std.", 8) == 0 ||
          strncmp(name, "src.lib.", 8) == 0);
}

static void ny_ir_externalize_std_definitions(LLVMModuleRef module) {
  if (!module)
    return;

  for (LLVMValueRef fn = LLVMGetFirstFunction(module); fn;) {
    LLVMValueRef next_fn = LLVMGetNextFunction(fn);
    const char *name = LLVMGetValueName(fn);
    if (ny_ir_is_std_symbol(name) && LLVMCountBasicBlocks(fn) > 0) {
      for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(fn); bb;) {
        LLVMBasicBlockRef next_bb = LLVMGetNextBasicBlock(bb);
        LLVMDeleteBasicBlock(bb);
        bb = next_bb;
      }
      LLVMSetLinkage(fn, LLVMExternalLinkage);
      LLVMSetVisibility(fn, LLVMDefaultVisibility);
    }
    fn = next_fn;
  }

  for (LLVMValueRef gv = LLVMGetFirstGlobal(module); gv;) {
    LLVMValueRef next_gv = LLVMGetNextGlobal(gv);
    const char *name = LLVMGetValueName(gv);
    if (ny_ir_is_std_symbol(name) && !LLVMIsDeclaration(gv)) {
      LLVMSetInitializer(gv, NULL);
      LLVMSetLinkage(gv, LLVMExternalLinkage);
      LLVMSetVisibility(gv, LLVMDefaultVisibility);
      LLVMSetGlobalConstant(gv, false);
    }
    gv = next_gv;
  }

  LLVMPassBuilderOptionsRef popt = LLVMCreatePassBuilderOptions();
  if (popt) {
    LLVMErrorRef perr = LLVMRunPasses(module, "globaldce", NULL, popt);
    if (perr) {
      char *msg = LLVMGetErrorMessage(perr);
      NY_LOG_WARN("IR std-prune pass failed: %s\n", msg ? msg : "<unknown>");
      if (msg)
        LLVMDisposeErrorMessage(msg);
    }
    LLVMDisposePassBuilderOptions(popt);
  }
}

static LLVMModuleRef ny_prepare_ir_dump_module(const ny_options *opt,
                                               LLVMModuleRef module) {
  if (!module)
    return NULL;
  LLVMModuleRef dump_mod = LLVMCloneModule(module);
  if (!dump_mod)
    return NULL;
  LLVMStripModuleDebugInfo(dump_mod);
  if (!opt || !opt->ir_include_std)
    ny_ir_externalize_std_definitions(dump_mod);
  return dump_mod;
}

static void ny_log_opt_skipped_if_diag(bool opt_diag, const char *profile_name,
                                       int eff_opt_level,
                                       const ny_ir_stats_t *stats) {
  if (!opt_diag || !stats)
    return;
  fprintf(stderr, "[opt] profile=%s O=%d passes=skipped\n", profile_name,
          eff_opt_level);
  fprintf(stderr, "[opt] ir: fn=%llu bb=%llu inst=%llu alloca=%llu phi=%llu\n",
          (unsigned long long)stats->funcs, (unsigned long long)stats->blocks,
          (unsigned long long)stats->insts, (unsigned long long)stats->allocas,
          (unsigned long long)stats->phis);
}

static void run_optimization_if_needed(const ny_options *opt,
                                       LLVMModuleRef module) {
  int eff_opt_level = opt->opt_level;
  const char *opt_profile = getenv("NYTRIX_OPT_PROFILE");
  const char *emit_pre_ir = getenv("NYTRIX_EMIT_IR_PREOPT_PATH");
  const char *emit_post_ir = getenv("NYTRIX_EMIT_IR_POSTOPT_PATH");
  const char *profile_name = "default";
  bool opt_diag = ny_env_enabled_strict("NYTRIX_OPT_DIAG");
  ny_ir_stats_t before_stats;
  ny_ir_stats_t after_stats;
  memset(&before_stats, 0, sizeof(before_stats));
  memset(&after_stats, 0, sizeof(after_stats));
  ny_opt_profile_kind_t profile_kind =
      ny_opt_profile_kind_from_name(opt_profile);
  bool profile_size = false;
  bool profile_compile = false;
  bool profile_none = false;
  bool profile_balanced = false;
  bool profile_speed = false;
  profile_name = ny_opt_profile_name(profile_kind, opt_profile);
  switch (profile_kind) {
  case NY_OPT_PROFILE_SPEED:
    profile_speed = true;
    eff_opt_level = 3;
    break;
  case NY_OPT_PROFILE_BALANCED:
    profile_balanced = true;
    eff_opt_level = 2;
    break;
  case NY_OPT_PROFILE_COMPILE:
    profile_compile = true;
    eff_opt_level = 0;
    break;
  case NY_OPT_PROFILE_NONE:
    profile_none = true;
    eff_opt_level = 0;
    break;
  case NY_OPT_PROFILE_SIZE:
    profile_size = true;
    eff_opt_level = 2;
    break;
  default:
    break;
  }

  // Auto-tune JIT optimization level based on IR complexity for better defaults
  bool autotune_enabled =
      (opt->opt_autotune == 1) ||
      (opt->opt_autotune == -1 && !ny_env_enabled("NYTRIX_JIT_NO_AUTOTUNE"));
  if (opt->opt_autotune == 0)
    autotune_enabled = false;

  if (autotune_enabled && opt->run_jit && opt->mode != NY_MODE_REPL &&
      !opt_profile &&
      opt->opt_level == 2) { // Only auto-tune if using default O2
    ny_ir_stats_t size_check;
    ny_collect_ir_stats(module, &size_check);
    // For small scripts (< 1000 instructions), use O1 for faster compilation
    // This improves startup time with minimal runtime cost
    if (size_check.insts < 1000 && !ny_env_enabled("NYTRIX_JIT_NO_AUTOTUNE")) {
      eff_opt_level = 1;
      if (opt_diag) {
        fprintf(stderr,
                "[opt] auto-tune: JIT small workload, using O1 "
                "(inst=%llu)\n",
                (unsigned long long)size_check.insts);
      }
    }
  }

  if (opt_diag) {
    ny_collect_ir_stats(module, &before_stats);
  }
  ny_dump_ir_if_requested(module, emit_pre_ir, "pre-opt");
  /* "none" is the explicit no-optimization profile. */
  if (!opt->opt_pipeline && profile_none) {
    ny_dump_ir_if_requested(module, emit_post_ir, "post-opt");
    ny_log_opt_skipped_if_diag(opt_diag, profile_name, eff_opt_level,
                               &before_stats);
    return;
  }
  const char *passes = opt->opt_pipeline;
  char buf[96];
  if (!passes) {
    bool with_attributor = !ny_env_enabled("NYTRIX_NO_ATTRIBUTOR");
    if (profile_compile)
      with_attributor = false;

    const char *core_pipeline = NULL;
    char core_buf[32];
    if (profile_size) {
      core_pipeline = "default<Os>";
    } else if (profile_compile) {
      core_pipeline = "default<O1>";
    } else if (profile_balanced) {
      core_pipeline = "default<O2>";
    } else if (profile_speed) {
      core_pipeline = "default<O3>";
    } else {
      snprintf(core_buf, sizeof(core_buf), "default<O%d>", eff_opt_level);
      core_pipeline = core_buf;
    }

    if (with_attributor) {
      snprintf(buf, sizeof(buf), "%s,attributor-cgscc", core_pipeline);
    } else {
      snprintf(buf, sizeof(buf), "%s", core_pipeline);
    }
    passes = buf;
  }
  if (opt_diag) {
    fprintf(stderr, "[opt] profile=%s O=%d passes=%s\n", profile_name,
            eff_opt_level, passes);
  }
  NY_LOG_V3("Running passes: %s\n", passes);
  LLVMPassBuilderOptionsRef popt = LLVMCreatePassBuilderOptions();
  int inline_thr = ny_guided_inline_threshold(profile_kind, eff_opt_level);
#if defined(LLVM_VERSION_MAJOR) && (LLVM_VERSION_MAJOR >= 17)
  if (inline_thr >= 0) {
    LLVMPassBuilderOptionsSetInlinerThreshold(popt, inline_thr);
  }
#endif
  bool enable_loop_opts =
      (opt->opt_loops == 1) ||
      (opt->opt_loops == -1 && ny_env_enabled_default_on("NYTRIX_OPT_LOOPS"));
  if (opt->opt_loops == 0)
    enable_loop_opts = false;
  if (enable_loop_opts && !profile_compile && !profile_size) {
    LLVMPassBuilderOptionsSetLoopInterleaving(popt, 1);
    LLVMPassBuilderOptionsSetLoopVectorization(popt, 1);
    LLVMPassBuilderOptionsSetSLPVectorization(popt, 1);
    LLVMPassBuilderOptionsSetLoopUnrolling(popt, 1);
  }
  if (profile_compile) {
    LLVMPassBuilderOptionsSetLoopInterleaving(popt, 0);
    LLVMPassBuilderOptionsSetLoopVectorization(popt, 0);
    LLVMPassBuilderOptionsSetSLPVectorization(popt, 0);
    LLVMPassBuilderOptionsSetLoopUnrolling(popt, 0);
  } else if (profile_size) {
    LLVMPassBuilderOptionsSetLoopVectorization(popt, 0);
    LLVMPassBuilderOptionsSetSLPVectorization(popt, 0);
  }
  if (opt_diag && inline_thr >= 0) {
#if defined(LLVM_VERSION_MAJOR) && (LLVM_VERSION_MAJOR >= 17)
    fprintf(stderr, "[opt] inline-threshold=%d\n", inline_thr);
#else
    fprintf(stderr,
            "[opt] inline-threshold=%d (ignored: requires LLVM >= 17)\n",
            inline_thr);
#endif
  }
  LLVMErrorRef perr = LLVMRunPasses(module, passes, NULL, popt);
  if (perr) {
    char *msg = LLVMGetErrorMessage(perr);
    NY_LOG_WARN("LLVM pass pipeline error: %s\n", msg ? msg : "<unknown>");
    if (msg)
      LLVMDisposeErrorMessage(msg);
  } else if (ny_env_enabled_strict("NYTRIX_OPT_EXTRA_CSE")) {
    const char *extra = "function(early-cse,gvn,instcombine,simplifycfg)";
    if (opt_diag) {
      fprintf(stderr, "[opt] extra-passes=%s\n", extra);
    }
    LLVMErrorRef xerr = LLVMRunPasses(module, extra, NULL, popt);
    if (xerr) {
      char *msg = LLVMGetErrorMessage(xerr);
      NY_LOG_WARN("LLVM extra CSE pipeline error: %s\n",
                  msg ? msg : "<unknown>");
      if (msg)
        LLVMDisposeErrorMessage(msg);
    }
  }
  LLVMDisposePassBuilderOptions(popt);
  ny_dump_ir_if_requested(module, emit_post_ir, "post-opt");
  if (opt_diag) {
    ny_collect_ir_stats(module, &after_stats);
    fprintf(stderr,
            "[opt] ir: fn=%llu bb=%llu inst=%llu->%llu alloca=%llu->%llu "
            "phi=%llu->%llu\n",
            (unsigned long long)before_stats.funcs,
            (unsigned long long)before_stats.blocks,
            (unsigned long long)before_stats.insts,
            (unsigned long long)after_stats.insts,
            (unsigned long long)before_stats.allocas,
            (unsigned long long)after_stats.allocas,
            (unsigned long long)before_stats.phis,
            (unsigned long long)after_stats.phis);
  }
}

static bool ny_is_llvm_special_global(const char *name) {
  return name && strncmp(name, "llvm.", 5) == 0;
}

static bool ny_should_preserve_aot_symbol(const char *name) {
  if (!name || !*name)
    return false;
  return strcmp(name, "main") == 0 || strcmp(name, "__script_top") == 0;
}

static bool ny_is_externalish_linkage(LLVMLinkage lk) {
  switch (lk) {
  case LLVMExternalLinkage:
  case LLVMAvailableExternallyLinkage:
  case LLVMLinkOnceAnyLinkage:
  case LLVMLinkOnceODRLinkage:
  case LLVMWeakAnyLinkage:
  case LLVMWeakODRLinkage:
  case LLVMAppendingLinkage:
  case LLVMExternalWeakLinkage:
  case LLVMCommonLinkage:
    return true;
  default:
    return false;
  }
}

static void run_dead_strip_if_needed(const ny_options *opt,
                                     LLVMModuleRef module) {
  if (!opt || !module)
    return;
  bool is_aot = (opt->output_file != NULL);
  bool is_jit = opt->run_jit && (opt->mode != NY_MODE_REPL);

  if (!is_aot && !is_jit)
    return;

  // Check CLI flags first, then env vars, then smart defaults
  bool dce_enabled;
  if (is_aot) {
    dce_enabled =
        (opt->opt_dce == 1) ||
        (opt->opt_dce == -1 && ny_env_enabled_default_on("NYTRIX_AOT_DCE"));
  } else {
    dce_enabled =
        (opt->opt_dce == 1) ||
        (opt->opt_dce == -1 && ny_env_enabled_default_on("NYTRIX_JIT_DCE"));
  }

  if (opt->opt_dce == 0) // Explicitly disabled via CLI
    dce_enabled = false;

  if (!dce_enabled)
    return;

  bool internalize_enabled;
  if (is_aot) {
    internalize_enabled = (opt->opt_internalize == 1) ||
                          (opt->opt_internalize == -1 &&
                           ny_env_enabled_default_on("NYTRIX_AOT_INTERNALIZE"));
  } else {
    internalize_enabled = (opt->opt_internalize == 1) ||
                          (opt->opt_internalize == -1 &&
                           ny_env_enabled_default_on("NYTRIX_JIT_INTERNALIZE"));
  }

  if (opt->opt_internalize == 0) // Explicitly disabled via CLI
    internalize_enabled = false;

  if (internalize_enabled) {
    size_t internalized_fns = 0;
    size_t internalized_globals = 0;

    for (LLVMValueRef fn = LLVMGetFirstFunction(module); fn;
         fn = LLVMGetNextFunction(fn)) {
      if (LLVMCountBasicBlocks(fn) == 0)
        continue; // declaration only
      size_t name_len = 0;
      const char *name = LLVMGetValueName2(fn, &name_len);
      if (!name || name_len == 0)
        continue;
      if (ny_should_preserve_aot_symbol(name))
        continue;
      LLVMLinkage lk = LLVMGetLinkage(fn);
      if (!ny_is_externalish_linkage(lk))
        continue;
      LLVMSetLinkage(fn, LLVMInternalLinkage);
      internalized_fns++;
    }

    for (LLVMValueRef gv = LLVMGetFirstGlobal(module); gv;
         gv = LLVMGetNextGlobal(gv)) {
      if (LLVMIsDeclaration(gv))
        continue;
      size_t name_len = 0;
      const char *name = LLVMGetValueName2(gv, &name_len);
      if (!name || name_len == 0)
        continue;
      if (ny_is_llvm_special_global(name))
        continue;
      if (ny_should_preserve_aot_symbol(name))
        continue;
      LLVMLinkage lk = LLVMGetLinkage(gv);
      if (!ny_is_externalish_linkage(lk))
        continue;
      LLVMSetLinkage(gv, LLVMInternalLinkage);
      internalized_globals++;
    }

    if (verbose_enabled >= 2) {
      NY_LOG_V2("%s internalize: functions=%zu globals=%zu\n",
                is_aot ? "AOT" : "JIT", internalized_fns, internalized_globals);
    }
  }

  const char *pipeline =
      getenv(is_aot ? "NYTRIX_AOT_DCE_PIPELINE" : "NYTRIX_JIT_DCE_PIPELINE");
  if (!pipeline || !*pipeline)
    pipeline = "globaldce";

  LLVMPassBuilderOptionsRef popt = LLVMCreatePassBuilderOptions();
  if (!popt)
    return;

  LLVMErrorRef perr = LLVMRunPasses(module, pipeline, NULL, popt);
  if (perr) {
    char *msg = LLVMGetErrorMessage(perr);
    NY_LOG_WARN("AOT dead-strip pipeline '%s' failed: %s\n", pipeline,
                msg ? msg : "<unknown>");
    if (msg)
      LLVMDisposeErrorMessage(msg);

    if (strcmp(pipeline, "globaldce") != 0) {
      LLVMErrorRef ferr = LLVMRunPasses(module, "globaldce", NULL, popt);
      if (ferr) {
        char *fmsg = LLVMGetErrorMessage(ferr);
        NY_LOG_WARN("%s dead-strip fallback 'globaldce' failed: %s\n",
                    is_aot ? "AOT" : "JIT", fmsg ? fmsg : "<unknown>");
        if (fmsg)
          LLVMDisposeErrorMessage(fmsg);
      } else if (verbose_enabled >= 2) {
        NY_LOG_V2("%s dead-strip passes: globaldce\n", is_aot ? "AOT" : "JIT");
      }
    }
  } else if (verbose_enabled >= 2) {
    NY_LOG_V2("%s dead-strip passes: %s\n", is_aot ? "AOT" : "JIT", pipeline);
  }

  LLVMDisposePassBuilderOptions(popt);
}

int ny_pipeline_run(ny_options *opt) {
  int exit_code = 0;
  if (handle_non_compile_modes(opt, &exit_code))
    return exit_code;

  verbose_enabled = opt->verbose;
  clock_t t_start = 0;
  if (opt->do_timing)
    t_start = clock();

  char *user_src = NULL;
  char *std_src = NULL;
  char *source = NULL;
  char **uses = NULL;
  size_t use_count = 0;
  arena_t *arena = NULL;
  char aot_cache_path[4096] = {0};
  char std_cache_path[4096] = {0};
  program_t prog = {0};
  codegen_t cg;
  memset(&cg, 0, sizeof(cg));
  const char *parse_name = opt->input_file ? opt->input_file : "<inline>";
  const char *output_path = opt->output_file;
  LLVMValueRef script_fn = NULL;
#ifdef _WIN32
  char output_win[4096];
  if (output_path)
    output_path =
        ny_windows_output_path(output_path, output_win, sizeof(output_win));
#endif

  user_src = load_user_source(opt);
  if (!user_src) {
    if (opt->input_file)
      NY_LOG_ERR("Failed to read file '%s'\n", opt->input_file);
    else
      NY_LOG_ERR("Failed to allocate source input\n");
    return 1;
  }
  maybe_log_phase_time(opt->do_timing, "Read file:", t_start);

  clock_t t_scan = clock();
  uses = collect_use_modules(user_src, &use_count);

  maybe_log_phase_time(opt->do_timing, "Scan imports:", t_scan);

  std_mode_t std_mode = opt->std_mode;

  const char *prebuilt_path = resolve_std_bundle(
      opt->std_path
          ? opt->std_path
          : (NYTRIX_STD_PATH ? NYTRIX_STD_PATH : "build/std_bundle.ny"));

  if (opt->no_std) {
    std_mode = STD_MODE_NONE;
  }

  clock_t t_std = clock();
  bool has_local = false;
  for (size_t i = 0; i < use_count; i++) {
    if (strncmp(uses[i], "std.", 4) != 0 && strncmp(uses[i], "lib.", 4) != 0) {
      has_local = true;
      break;
    }
  }
  bool std_sources_ok = ny_std_sources_available();
  bool prebuilt_ok = prebuilt_path && access(prebuilt_path, R_OK) == 0;
  bool prebuilt_preferred =
      (std_mode == STD_MODE_FULL || std_mode == STD_MODE_DEFAULT) && !has_local;
  bool prebuilt_required = !std_sources_ok;

  if (std_mode != STD_MODE_NONE && prebuilt_ok &&
      (prebuilt_preferred || prebuilt_required)) {
    if (verbose_enabled)
      NY_LOG_INFO("Using prebuilt std bundle: %s\n", prebuilt_path);
    std_src = ny_read_file(prebuilt_path);
    if (!std_src && verbose_enabled) {
      NY_LOG_WARN("Failed to read prebuilt std bundle: %s (falling back)\n",
                  prebuilt_path);
    }
  }
  if (std_mode != STD_MODE_NONE && !std_src) {
    bool use_std_cache =
        !has_local && ny_env_enabled_default_on("NYTRIX_STD_CACHE");
    if (use_std_cache) {
      ny_build_std_cache_path(opt, (const char *const *)uses, use_count,
                              std_mode, prebuilt_path, std_cache_path,
                              sizeof(std_cache_path));
      if (std_cache_path[0] != '\0' && access(std_cache_path, R_OK) == 0) {
        std_src = ny_read_file(std_cache_path);
        if (std_src && std_src[0] == '\0') {
          free(std_src);
          std_src = NULL;
          (void)unlink(std_cache_path);
        }
        if (std_src && verbose_enabled >= 2)
          NY_LOG_INFO("Using std cache: %s\n", std_cache_path);
      }
    }
    if (!std_src) {
      std_src = ny_build_std_bundle((const char **)uses, use_count, std_mode,
                                    opt->verbose, opt->input_file);
      if (std_src && use_std_cache && std_cache_path[0] != '\0')
        (void)ny_write_file_atomic(std_cache_path, std_src, strlen(std_src));
    }
  }

  if (std_mode != STD_MODE_NONE && !std_src) {
    NY_LOG_ERR("Could not load standard library bundle or source files.\n");
    NY_LOG_ERR("Checked paths: %s and %s/std\n",
               prebuilt_path ? prebuilt_path : "NULL", ny_src_root());
    if (user_src)
      free(user_src);
    if (uses)
      ny_str_list_free(uses, use_count);
    return 1;
  }
  maybe_log_phase_time(opt->do_timing, "Stdlib load:", t_std);

  // 4. Construct final source with std + user
  size_t slen = std_src ? strlen(std_src) : 0;
  size_t ulen = strlen(user_src);
  source = malloc(slen + ulen + 2);
  char *ptr = source;
  if (std_src) {
    memcpy(ptr, std_src, slen);
    ptr += slen;
    *ptr++ = '\n';
  }
  memcpy(ptr, user_src, ulen + 1);

  if (ny_should_use_aot_cache(opt)) {
    ny_build_aot_cache_path(opt, source, parse_name, prebuilt_path, output_path,
                            aot_cache_path, sizeof(aot_cache_path));
    if (aot_cache_path[0] != '\0' && access(aot_cache_path, R_OK) == 0 &&
        strcmp(aot_cache_path, output_path) != 0) {
      if (ny_valid_native_artifact(aot_cache_path)) {
        if (ny_copy_file(aot_cache_path, output_path) == 0 &&
            ny_valid_native_artifact(output_path)) {
          NY_LOG_V2("AOT cache hit: %s\n", aot_cache_path);
#ifdef _WIN32
          NY_LOG_SUCCESS("Saved EXE: %s\n", output_path);
#else
          NY_LOG_SUCCESS("Saved ELF: %s\n", output_path);
#endif
          goto exit_success;
        }
      } else {
        (void)unlink(aot_cache_path);
      }
    }
  }

  if (opt->dump_tokens) {
    lexer_t lx;
    lexer_init(&lx, source, parse_name);
    for (;;) {
      token_t t = lexer_next(&lx);
      printf("%d:%d kind=%d lexeme='%.*s'\n", t.line, t.col, t.kind, (int)t.len,
             t.lexeme);
      if (t.kind == NY_T_EOF)
        break;
    }
    goto exit_success;
  }

  // Check JIT cache for instant compilation
  char *jit_cache_file = NULL;
  bool loaded_from_cache = false;
  if ((opt->run_jit || opt->output_file) && opt->mode != NY_MODE_REPL &&
      ny_jit_cache_enabled() && !opt->dump_ast && !opt->dump_llvm &&
      !opt->emit_ir_path) {
    jit_cache_file = ny_jit_cache_path(source, prebuilt_path);
    // fprintf(stderr, "[DEBUG] JIT path: %s\n", jit_cache_file ? jit_cache_file
    // : "NULL");
    if (jit_cache_file) {
      codegen_init(&cg, NULL, NULL, "nytrix");
      if (ny_jit_cache_load(jit_cache_file, cg.ctx, &cg.module)) {
        if (opt->verbose)
          fprintf(stderr, "JIT cache hit: %s\n", jit_cache_file);
        loaded_from_cache = true;
        script_fn = LLVMGetNamedFunction(cg.module, "__script_top");
        if (!script_fn) {
          if (opt->verbose)
            fprintf(stderr, "JIT cache corrupt (missing entry): %s\n",
                    jit_cache_file);
          LLVMDisposeModule(cg.module);
          cg.module = NULL;
          loaded_from_cache = false;
        } else {
          goto skip_compilation;
        }
      } else {
        if (opt->verbose)
          fprintf(stderr, "JIT cache miss: %s\n", jit_cache_file);
        codegen_dispose(&cg);
      }
    }
  } else {
    // if (opt->verbose) fprintf(stderr, "[DEBUG] JIT caching disabled\n");
  }

  clock_t t_parse = clock();
  parser_t parser;
  arena = (arena_t *)malloc(sizeof(arena_t));
  memset(arena, 0, sizeof(arena_t));
  parser_init_with_arena(&parser, source, std_src ? "<stdlib>" : parse_name,
                         arena);
  if (std_src) {
    parser.lex.split_pos = slen + 1;
    parser.lex.split_filename = parse_name;
  }
  prog = parse_program(&parser);
  maybe_log_phase_time(opt->do_timing, "Parsing:", t_parse);

  if (parser.had_error) {
    NY_LOG_ERR("Compilation failed: %d errors\n", parser.error_count);
    dump_debug_bundle(opt, source, NULL);
    exit_code = 1;
    goto exit_success;
  }

  if (opt->dump_ast) {
    for (size_t i = 0; i < prog.body.len; i++) {
      stmt_t *s = prog.body.data[i];
      printf("  [%zu] Kind=%d\n", i, s->kind);
    }
  }

  clock_t t_codegen = clock();
  NY_LOG_V2("Initializing codegen_t for module 'nytrix'\n");

  codegen_init(&cg, &prog, arena, "nytrix");

  LLVMModuleRef cached_std_mod = NULL;
  char *std_cache_ptr = NULL;
  if (ny_jit_cache_enabled() && std_src) {
    std_cache_ptr = ny_jit_cache_path(std_src, "stdlib_only");
    if (ny_jit_cache_load(std_cache_ptr, cg.ctx, &cached_std_mod)) {
      if (opt->verbose)
        fprintf(stderr, "Stdlib cache hit: %s\n", std_cache_ptr);
      cg.skip_stdlib = true;
    }
  }
  cg.debug_symbols = opt->debug_symbols;
  cg.debug_opt_level = opt->opt_level;
  cg.debug_opt_pipeline = opt->opt_pipeline;
  cg.trace_exec = opt->trace_exec;
  if (cg.debug_symbols)
    codegen_debug_init(&cg, parse_name);
  cg.source_string = source;
  cg.prog_owned = false; // prog is on stack
  NY_LOG_V2("Emitting IR...\n");
  codegen_emit(&cg);

  if (cached_std_mod) {
    if (LLVMLinkModules2(cg.module, cached_std_mod)) {
      NY_LOG_ERR("Failed to link cached stdlib\n");
      exit_code = 1;
      goto exit_success;
    }
    cached_std_mod = NULL;
  }
  if (std_cache_ptr)
    free(std_cache_ptr);
  if (cg.had_error) {
    NY_LOG_ERR("Codegen failed\n");
    dump_debug_bundle(opt, source, cg.module);
    exit_code = 1;
    goto exit_success;
  }
  NY_LOG_V2("Emitting script entry point...\n");
  script_fn = codegen_emit_script(&cg, "__script_top");
  if (cg.had_error) {
    NY_LOG_ERR("Codegen script entry failed\n");
    dump_debug_bundle(opt, source, cg.module);
    exit_code = 1;
    goto exit_success;
  }
  codegen_debug_finalize(&cg);
  maybe_log_phase_time(opt->do_timing, "Codegen:", t_codegen);

  if (opt->dump_llvm) {
    LLVMModuleRef dump_mod = ny_prepare_ir_dump_module(opt, cg.module);
    LLVMDumpModule(dump_mod ? dump_mod : cg.module);
    if (dump_mod)
      LLVMDisposeModule(dump_mod);
  }

  clock_t t_ver = clock();
  if (!verify_module_if_needed(opt, cg.module)) {
    dump_debug_bundle(opt, source, cg.module);
    exit_code = 1;
    goto exit_success;
  }
  if (opt->do_timing && opt->verify_module)
    fprintf(stderr, "Verify:       %.4fs\n",
            (double)(clock() - t_ver) / CLOCKS_PER_SEC);

  clock_t t_opt = clock();
  run_dead_strip_if_needed(opt, cg.module);
  run_optimization_if_needed(opt, cg.module);
  if (opt->do_timing && (opt->opt_level > 0 || opt->opt_pipeline))
    fprintf(stderr, "Optimization: %.4fs\n",
            (double)(clock() - t_opt) / CLOCKS_PER_SEC);

  if (jit_cache_file && !loaded_from_cache) {
    if (ny_jit_cache_save(jit_cache_file, cg.module)) {
      if (opt->verbose)
        fprintf(stderr, "JIT cache saved: %s\n", jit_cache_file);
    } else {
      if (opt->verbose)
        fprintf(stderr, "Failed to save JIT cache: %s\n", jit_cache_file);
    }
  }

skip_compilation:
  if (jit_cache_file)
    free(jit_cache_file);

  ny_llvm_apply_host_attrs(cg.module);

  if (opt->emit_ir_path) {
    LLVMModuleRef dump_mod = ny_prepare_ir_dump_module(opt, cg.module);
    char *err = NULL;
    if (LLVMPrintModuleToFile(dump_mod ? dump_mod : cg.module,
                              opt->emit_ir_path, &err) != 0) {
      NY_LOG_ERR("Failed to write IR to %s\n", opt->emit_ir_path);
      if (err) {
        NY_LOG_ERR("%s\n", err);
        LLVMDisposeMessage(err);
      }
      exit_code = 1;
      goto exit_success;
    }
    if (dump_mod)
      LLVMDisposeModule(dump_mod);
  }

  if (opt->emit_asm_path) {
    if (!ny_llvm_emit_file(cg.module, opt->emit_asm_path, LLVMAssemblyFile)) {
      NY_LOG_ERR("Failed to write assembly to %s\n", opt->emit_asm_path);
      exit_code = 1;
      goto exit_success;
    }
  }

  if (opt->output_file) {
    char obj[4096];
    char obj_name[64];
    snprintf(obj_name, sizeof(obj_name), "ny_tmp_%d.o", getpid());
    ny_join_path(obj, sizeof(obj), ny_get_temp_dir(), obj_name);
    ensure_aot_entry(&cg, script_fn);
    if (ny_llvm_emit_object(cg.module, obj)) {
      const char *cc = ny_builder_choose_cc();
      char rto[4096];
      char rto_name[64];
      snprintf(rto_name, sizeof(rto_name), "ny_rt_%d.o", getpid());
      ny_join_path(rto, sizeof(rto), ny_get_temp_dir(), rto_name);
      NY_LOG_V2("Compiling runtime to %s using %s (debug=%d)...\n", rto, cc,
                opt->debug_symbols);
      if (!ny_builder_compile_runtime(cc, rto, NULL, opt->debug_symbols,
                                      opt->gprof == 1)) {
        unlink(obj);
        dump_debug_bundle(opt, source, cg.module);
        exit_code = 1;
        goto exit_success;
      }
      bool link_strip = opt->strip_override == 1 ||
                        (opt->strip_override == -1 && !opt->debug_symbols);
      NY_LOG_V2("Linking executable %s (strip=%d, debug=%d)...\n", output_path,
                link_strip, opt->debug_symbols);
      if (!ny_builder_link(
              cc, obj, rto, NULL, NULL, 0,
              (const char *const *)opt->link_dirs.data, opt->link_dirs.len,
              (const char *const *)opt->link_libs.data, opt->link_libs.len,
              output_path, link_strip, opt->debug_symbols, opt->gprof == 1)) {
        unlink(obj);
        unlink(rto);
        dump_debug_bundle(opt, source, cg.module);
        exit_code = 1;
        goto exit_success;
      }
      unlink(obj);
      unlink(rto);
      if (aot_cache_path[0] != '\0' &&
          strcmp(aot_cache_path, output_path) != 0 &&
          ny_valid_native_artifact(output_path)) {
        (void)ny_copy_file(output_path, aot_cache_path);
      }
#ifdef _WIN32
      NY_LOG_SUCCESS("Saved EXE: %s\n", output_path);
#else
      NY_LOG_SUCCESS("Saved ELF: %s\n", output_path);
#endif
    } else {
      NY_LOG_ERR("Failed to emit object file\n");
      dump_debug_bundle(opt, source, cg.module);
      exit_code = 1;
      goto exit_success;
    }
  }

  if (opt->run_jit) {
    clock_t t_jit = clock();
    LLVMLinkInMCJIT();
    ny_llvm_init_native();
    LLVMExecutionEngineRef ee;
    char *err = NULL;
    struct LLVMMCJITCompilerOptions jopt;
    LLVMInitializeMCJITCompilerOptions(&jopt, sizeof(jopt));
    // Large code model hurts JIT compile+exec speed on normal workloads.
    // Keep default unless explicitly overridden for debugging edge cases.
    jopt.CodeModel = LLVMCodeModelJITDefault;
    {
      const char *cm = getenv("NYTRIX_JIT_CODE_MODEL");
      if (cm && *cm) {
        if (strcmp(cm, "large") == 0)
          jopt.CodeModel = LLVMCodeModelLarge;
        else if (strcmp(cm, "medium") == 0)
          jopt.CodeModel = LLVMCodeModelMedium;
        else if (strcmp(cm, "small") == 0)
          jopt.CodeModel = LLVMCodeModelSmall;
      }
    }
    {
      if (ny_env_enabled("NYTRIX_JIT_FAST_ISEL"))
        jopt.EnableFastISel = 1;
    }
    LLVMModuleRef jmod = cg.module;
    if (LLVMCreateMCJITCompilerForModule(&ee, jmod, &jopt, sizeof(jopt),
                                         &err)) {
      NY_LOG_ERR("JIT failed: %s\n", err);
      dump_debug_bundle(opt, source, jmod);
      exit_code = 1;
      goto exit_success;
    }
    // Execution engine now owns the module
    cg.module = NULL;

    {
      if (ny_env_enabled("NYTRIX_JIT_MAP_STRINGS")) {
        for (size_t i = 0; i < cg.interns.len; i++) {
          if (cg.interns.data[i].gv) {
            LLVMAddGlobalMapping(
                ee, cg.interns.data[i].gv,
                (void *)((char *)cg.interns.data[i].data - 64));
          }
          if (cg.interns.data[i].val &&
              cg.interns.data[i].val != cg.interns.data[i].gv) {
            LLVMAddGlobalMapping(ee, cg.interns.data[i].val,
                                 &cg.interns.data[i].data);
          }
        }
      }
    }

    register_jit_symbols(ee, jmod, &cg);
    maybe_log_phase_time(opt->do_timing, "JIT Init:", t_jit);

    clock_t t_exec = clock();
    // Execution
    uint64_t saddr = LLVMGetFunctionAddress(ee, "__script_top");
    if (saddr) {
      if (verbose_enabled >= 3)
        fprintf(stderr, "TRACE: Executing script...\n");
      ((void (*)(void))saddr)();
      if (verbose_enabled >= 3)
        fprintf(stderr, "TRACE: Script finished.\n");
    } else {
      if (verbose_enabled >= 3)
        fprintf(stderr, "TRACE: __script_top NOT FOUND\n");
    }

    // execution.
    maybe_log_phase_time(opt->do_timing, "JIT Exec:", t_exec);

    LLVMDisposeExecutionEngine(ee);
  }

exit_success:
  // Cleanup allocated memory
  if (user_src)
    free(user_src);
  if (std_src)
    free(std_src);
  if (source)
    free(source);
  if (uses)
    ny_str_list_free(uses, use_count);
  codegen_dispose(&cg);
  program_free(&prog, arena);

  maybe_log_phase_time(opt->do_timing, "Total time:", t_start);
  return exit_code;
}
