#define _CRT_RAND_S
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
#include "wire/bundle.h"
#include "wire/cache.h"
#include <llvm-c/Analysis.h>
#include <llvm-c/BitReader.h>
#include <llvm-c/BitWriter.h>
#include <llvm-c/DebugInfo.h>
#include <llvm-c/Error.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/IRReader.h>
#include <llvm-c/Linker.h>
#include <llvm-c/Support.h>
#include <llvm-c/Transforms/PassBuilder.h>
#include <llvm/Config/llvm-config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <dlfcn.h>
#include <sys/wait.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#ifndef _WIN32
#include <unistd.h>
#else
#include <io.h>
#endif
#include <ctype.h>

#if defined(__GNUC__) || defined(__clang__)
#define NY_UNUSED_FUNC __attribute__((unused))
#else
#define NY_UNUSED_FUNC
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

static bool ny_valid_native_artifact(const char *path) {
  if (!path || !*path)
    return false;
  struct stat st;
  if (stat(path, &st) != 0)
    return false;
  if (st.st_size <= 0)
    return false;
#ifndef _WIN32
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

static NY_UNUSED_FUNC void ny_collect_ir_stats(LLVMModuleRef module,
                                               ny_ir_stats_t *out) {
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
                      LLVMAssemblyFile, opt->opt_level);
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
    return buf;
  return buf;
}
#endif

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

static NY_UNUSED_FUNC const char *
ny_opt_profile_name(ny_opt_profile_kind_t kind, const char *custom_name) {
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

static NY_UNUSED_FUNC int
ny_guided_inline_threshold(ny_opt_profile_kind_t profile_kind,
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

static LLVMCodeGenOptLevel ny_jit_codegen_opt_level(const ny_options *opt)
    __attribute__((unused));
static LLVMCodeGenOptLevel ny_jit_codegen_opt_level(const ny_options *opt) {
  const char *raw = getenv("NYTRIX_JIT_CODEGEN_OPT");
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
  ny_opt_profile_kind_t profile_kind =
      ny_opt_profile_kind_from_name(getenv("NYTRIX_OPT_PROFILE"));
  switch (profile_kind) {
  case NY_OPT_PROFILE_NONE:
  case NY_OPT_PROFILE_COMPILE:
    return LLVMCodeGenLevelNone;
  case NY_OPT_PROFILE_SIZE:
    return LLVMCodeGenLevelLess;
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
        (unsigned)opt->opt_level,       (unsigned)opt->debug_symbols,
        (unsigned)opt->strip_override,  (unsigned)opt->std_mode,
        (unsigned)opt->no_std,          (unsigned)opt->opt_dce,
        (unsigned)opt->opt_internalize, (unsigned)opt->opt_loops,
        (unsigned)opt->opt_autotune};
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
    const char *const host_envs[] = {
        "NYTRIX_HOST_CFLAGS", "NYTRIX_HOST_LDFLAGS", "NYTRIX_ASSUME_INT"};
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
    snprintf(tmp, sizeof(tmp), "%s.tmp.%u.%lu", path, r,
             (unsigned long)clock());
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

static void ny_build_std_cache_path(const ny_options *opt,
                                    const char *const *uses, size_t use_count,
                                    std_mode_t std_mode,
                                    const char *prebuilt_path, char *out,
                                    size_t out_len) {
  if (!out || out_len == 0)
    return;
  out[0] = '\0';
  uint64_t h = NY_FNV1A64_OFFSET_BASIS;
  h = ny_fnv1a64_cstr("std-cache-v4", h);
  h = ny_hash64_u64(h, (uint64_t)std_mode);
  if (opt) {
    const unsigned opt_fields[] = {
        (unsigned)opt->opt_level, (unsigned)opt->opt_dce,
        (unsigned)opt->opt_internalize, (unsigned)opt->no_std,
        (unsigned)opt->debug_symbols};
    h = ny_hash_mix_u32v(h, opt_fields,
                         sizeof(opt_fields) / sizeof(opt_fields[0]));
  }
  h = ny_hash_mix_cstrv(h, uses, use_count);
  h = ny_fnv1a64_cstr(prebuilt_path, h);
  h = ny_hash64_u64(h, (uint64_t)ny_std_latest_mtime());
  {
    const char *const envs[] = {"NYTRIX_HOST_TRIPLE", "NYTRIX_HOST_CFLAGS",
                                "NYTRIX_HOST_LDFLAGS", "NYTRIX_ARM_FLOAT_ABI",
                                "NYTRIX_ASSUME_INT"};
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
#ifndef _WIN32
  long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
  if (ncpu > 0 && ncpu < 1024)
    return (int)ncpu;
#endif
  return 4;
}

static NY_UNUSED_FUNC int ny_parallel_module_jobs(const ny_options *opt,
                                                  size_t total) {
  if (!opt)
    return 1;
  if (opt->thread_count > 0)
    return opt->thread_count;
  int jobs = ny_parallel_default_jobs();
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
  if (strcmp(opt->parallel_mode, "modules") != 0)
    return false;
  if (getenv("NYTRIX_PARALLEL_DISABLE"))
    return false;
  if (!opt->input_file)
    return false;
  if (opt->run_jit)
    return false; // Disable for JIT to avoid linking issues
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
    setenv("NYTRIX_PARALLEL_DISABLE", "1", 1);
    setenv("NYTRIX_WORKER", "1", 1);
    if (opt->opt_level > 0 || (opt->opt_pipeline && *opt->opt_pipeline))
      setenv("NYTRIX_WORKER_OPT", "1", 1);
    execvp(argv[0], (char *const *)argv);
    _exit(1);
  }
  job->name = ny_strdup(module_name);
  job->bc_path = ny_strdup(bc_path);
  job->pid = pid;
  job->exit_code = -1;
  return true;
}

static NY_UNUSED_FUNC bool ny_wait_module_jobs(ny_module_job *jobs,
                                               size_t job_count) {
  bool ok = true;
  for (size_t i = 0; i < job_count; i++) {
    int status = 0;
    if (waitpid(jobs[i].pid, &status, 0) < 0) {
      ok = false;
      jobs[i].exit_code = 1;
      continue;
    }
    if (WIFEXITED(status)) {
      jobs[i].exit_code = WEXITSTATUS(status);
      if (jobs[i].exit_code != 0)
        ok = false;
    } else {
      jobs[i].exit_code = 1;
      ok = false;
    }
  }
  return ok;
}
#endif

/* ny_cache_path_is_ir() moved to cache.c - use from there */
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
    parsed = (LLVMGetBitcodeModuleInContext2(ctx, buf, &mod) == 0);
  }
  if (!parsed && msg) {
    LLVMDisposeMessage(msg);
    msg = NULL;
  }
  if (!parsed || !buf_owned_by_module) {
    LLVMDisposeMemoryBuffer(buf);
  }
  if (!parsed) {
    return false;
  }
  if (LLVMLinkModules2(main_mod, mod) != 0) {
    LLVMDisposeModule(mod);
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
  bool ok = (LLVMGetBitcodeModuleInContext2(ctx, buf, &mod) == 0);
  if (mod)
    LLVMDisposeModule(mod);
  LLVMDisposeMemoryBuffer(buf);
  return ok;
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
  bool buf_owned_by_module = false;
  if (!fresh_ctx || LLVMParseIRInContext(fresh_ctx, buf, &parsed, &msg) != 0) {
    if (msg)
      LLVMDisposeMessage(msg);
    LLVMDisposeMemoryBuffer(buf);
    if (fresh_ctx)
      LLVMContextDispose(fresh_ctx);
    return false;
  }
  buf_owned_by_module = true;
  if (!buf_owned_by_module)
    LLVMDisposeMemoryBuffer(buf);
  bool ok = (LLVMWriteBitcodeToFile(parsed, bc_path) == 0);
  LLVMDisposeModule(parsed);
  if (fresh_ctx)
    LLVMContextDispose(fresh_ctx);
  return ok;
}

static char **ny_collect_import_names(const char *src, size_t *out_count) {
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
  char *exe_dir = ny_get_executable_dir();
  if (exe_dir) {
    snprintf(path, sizeof(path), "%s/std.ny", exe_dir);
    if (access(path, R_OK) == 0)
      return path;
    snprintf(path, sizeof(path), "%s/../share/nytrix/std.ny", exe_dir);
    if (access(path, R_OK) == 0)
      return path;
  }
  const char *root = ny_src_root();
  snprintf(path, sizeof(path), "%s/build/std.ny", root);
  if (access(path, R_OK) == 0)
    return path;
  snprintf(path, sizeof(path), "%s/std.ny", root);
  if (access(path, R_OK) == 0)
    return path;
  if (compile_time_path && access(compile_time_path, R_OK) == 0)
    return compile_time_path;
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
  LLVMValueRef set_args_fn = LLVMGetNamedFunction(cg->module, "rt_set_args");
  if (!set_args_fn) {
    LLVMTypeRef set_args_ty =
        LLVMFunctionType(i64, (LLVMTypeRef[]){i64, i64, i64}, 3, 0);
    set_args_fn = LLVMAddFunction(cg->module, "rt_set_args", set_args_ty);
  }
  LLVMBuildCall2(builder, LLVMGlobalGetValueType(set_args_fn), set_args_fn,
                 (LLVMValueRef[]){argc_i64, argv_i64, envp_i64}, 3, "");
  LLVMValueRef res_raw = LLVMBuildCall2(
      builder, LLVMGlobalGetValueType(script_fn), script_fn, NULL, 0, "");
  LLVMValueRef cleanup_fn =
      LLVMGetNamedFunction(cg->module, "rt_runtime_cleanup");
  if (!cleanup_fn) {
    LLVMTypeRef cleanup_ty = LLVMFunctionType(i64, NULL, 0, 0);
    cleanup_fn = LLVMAddFunction(cg->module, "rt_runtime_cleanup", cleanup_ty);
  }
  LLVMBuildCall2(builder, LLVMGlobalGetValueType(cleanup_fn), cleanup_fn, NULL,
                 0, "");
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
  if (opt->mode == NY_MODE_BUNDLE) {
    *exit_code = ny_bundle_save(opt);
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
    int repl_batch = 0;
#ifdef _WIN32
    repl_batch = (_isatty(_fileno(stdin)) == 0);
#else
    repl_batch = (isatty(STDIN_FILENO) == 0);
#endif
    std_mode_t repl_std_mode = opt->no_std ? STD_MODE_NONE : opt->std_mode;
    if (repl_batch && !opt->std_mode_explicit) {
      const char *env_std = getenv("NYTRIX_REPL_STD");
      const char *env_no_std = getenv("NYTRIX_REPL_NO_STD");
      if ((!env_std || !*env_std) && (!env_no_std || !*env_no_std)) {
        repl_std_mode = STD_MODE_NONE;
      }
    }
    ny_repl_set_std_mode(repl_std_mode);
    ny_repl_set_plain(opt->repl_plain ? 1 : 0);
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

static NY_UNUSED_FUNC void ny_dump_ir_if_requested(LLVMModuleRef module,
                                                   const char *path,
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

static void ny_ir_externalize_std_definitions(const ny_options *opt,
                                              LLVMModuleRef module) {
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
    bool enable_dce = true;
    if (opt && opt->opt_level == 0)
      enable_dce = false;
    if (enable_dce) {
      LLVMErrorRef perr = LLVMRunPasses(module, "globaldce", NULL, popt);
      if (perr) {
        char *msg = LLVMGetErrorMessage(perr);
        NY_LOG_WARN("IR std-prune pass failed: %s\n", msg ? msg : "<unknown>");
        if (msg)
          LLVMDisposeErrorMessage(msg);
      }
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
    ny_ir_externalize_std_definitions(opt, dump_mod);
  return dump_mod;
}

static NY_UNUSED_FUNC void
ny_log_opt_skipped_if_diag(bool opt_diag, const char *profile_name,
                           int eff_opt_level, const ny_ir_stats_t *stats) {
  if (!opt_diag || !stats)
    return;
  fprintf(stderr, "[opt] profile=%s O=%d passes=skipped\n", profile_name,
          eff_opt_level);
  fprintf(stderr, "[opt] ir: fn=%llu bb=%llu inst=%llu alloca=%llu phi=%llu\n",
          (unsigned long long)stats->funcs, (unsigned long long)stats->blocks,
          (unsigned long long)stats->insts, (unsigned long long)stats->allocas,
          (unsigned long long)stats->phis);
}

static bool ny_is_llvm_special_global(const char *name) {
  return name && strncmp(name, "llvm.", 5) == 0;
}

static bool ny_should_preserve_symbol(const codegen_t *cg, const char *name,
                                      bool is_jit) {
  if (!name || !*name)
    return false;
  if (strcmp(name, "main") == 0 || strcmp(name, "_ny_top_entry") == 0)
    return true;
  if (strncmp(name, "__std_init", 10) == 0)
    return true;
  if (name[0] == '.')
    return true;
  if (is_jit)
    return false;

  const char *dot = strchr(name, '.');
  if (dot) {
    if (!cg)
      return true;
    size_t mod_len = (size_t)(dot - name);
    for (size_t i = 0; i < cg->link_allowed_modules.len; i++) {
      const char *use_name = cg->link_allowed_modules.data[i];
      if (!use_name)
        continue;
      if (strncmp(name, use_name, mod_len) == 0 && use_name[mod_len] == '\0') {
        return true;
      }
    }
    return false;
  }
  return false;
}

static bool ny_should_preserve_aot_symbol(const codegen_t *cg,
                                          const char *name) {
  return ny_should_preserve_symbol(cg, name, false);
}

static bool ny_should_preserve_jit_symbol(const codegen_t *cg,
                                          const char *name) {
  return ny_should_preserve_symbol(cg, name, true);
}

static void ny_build_llvm_used(LLVMModuleRef module, const LLVMValueRef *values,
                               size_t count) {
  if (!module || !values || count == 0)
    return;
  LLVMTypeRef i8ptr =
      LLVMPointerType(LLVMInt8TypeInContext(LLVMGetModuleContext(module)), 0);
  VEC(LLVMValueRef) entries;
  vec_init(&entries);

  LLVMValueRef used = LLVMGetNamedGlobal(module, "llvm.used");
  if (used) {
    LLVMValueRef init = LLVMGetInitializer(used);
    if (init && LLVMIsAConstantArray(init)) {
      unsigned n = LLVMGetNumOperands(init);
      for (unsigned i = 0; i < n; i++) {
        LLVMValueRef op = LLVMGetOperand(init, i);
        if (op)
          vec_push(&entries, op);
      }
    }
  }

  for (size_t i = 0; i < count; i++) {
    LLVMValueRef v = values[i];
    if (!v)
      continue;
    LLVMValueRef cast = LLVMConstBitCast(v, i8ptr);
    vec_push(&entries, cast);
  }

  if (entries.len == 0) {
    vec_free(&entries);
    return;
  }

  LLVMTypeRef arr_ty = LLVMArrayType(i8ptr, (unsigned)entries.len);
  LLVMValueRef arr = LLVMConstArray(i8ptr, entries.data, (unsigned)entries.len);
  if (!used) {
    used = LLVMAddGlobal(module, arr_ty, "llvm.used");
  }
  LLVMSetLinkage(used, LLVMAppendingLinkage);
  LLVMSetSection(used, "llvm.metadata");
  LLVMSetInitializer(used, arr);
  vec_free(&entries);
}

static void ny_prepare_internalize(LLVMModuleRef module, const ny_options *opt,
                                   const codegen_t *cg, bool is_jit) {
  if (!module || !opt)
    return;
  VEC(LLVMValueRef) preserve;
  vec_init(&preserve);
  for (LLVMValueRef fn = LLVMGetFirstFunction(module); fn;
       fn = LLVMGetNextFunction(fn)) {
    if (LLVMIsDeclaration(fn))
      continue;
    size_t name_len = 0;
    const char *name = LLVMGetValueName2(fn, &name_len);
    if (!name || name_len == 0)
      continue;
    if (is_jit ? ny_should_preserve_jit_symbol(cg, name)
               : ny_should_preserve_aot_symbol(cg, name)) {
      vec_push(&preserve, fn);
    }
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
    if (is_jit ? ny_should_preserve_jit_symbol(cg, name)
               : ny_should_preserve_aot_symbol(cg, name)) {
      vec_push(&preserve, gv);
    }
  }
  if (preserve.len > 0)
    ny_build_llvm_used(module, preserve.data, preserve.len);
  vec_free(&preserve);
}

static void run_dead_strip_if_needed(const ny_options *opt, codegen_t *cg,
                                     LLVMModuleRef module) {
  if (!opt || !module)
    return;
  bool is_aot = (opt->output_file != NULL);
  bool is_jit = opt->run_jit && (opt->mode != NY_MODE_REPL);
  if (!is_aot && !is_jit)
    return;

  bool dce_enabled = false;
  if (is_aot || is_jit) {
    dce_enabled = (opt->opt_dce != 0) && !ny_env_enabled("NYTRIX_JIT_NO_DCE");
  }

  if (!dce_enabled)
    return;

  bool internalize_enabled = false;
  if (is_aot || is_jit) {
    internalize_enabled =
        (opt->opt_internalize != 0) && !ny_env_enabled("NYTRIX_JIT_NO_DCE");
  }

  if (internalize_enabled) {
    ny_prepare_internalize(module, opt, cg, is_jit);
    if (verbose_enabled >= 1)
      NY_LOG_INFO("%s internalize: enabled via llvm.used\n",
                  is_aot ? "AOT" : "JIT");
  } else if (verbose_enabled >= 1) {
    NY_LOG_INFO("%s internalize: DISABLED (opt->opt_internalize=%d)\n",
                is_jit ? "JIT" : "AOT", opt->opt_internalize);
  }
  const char *pipeline = NULL;
  if (internalize_enabled) {
    pipeline = getenv(is_aot ? "NYTRIX_AOT_INTERNALIZE_PIPELINE"
                             : "NYTRIX_JIT_INTERNALIZE_PIPELINE");
    if (!pipeline || !*pipeline)
      pipeline = "internalize,globaldce";
  } else {
    pipeline =
        getenv(is_aot ? "NYTRIX_AOT_DCE_PIPELINE" : "NYTRIX_JIT_DCE_PIPELINE");
    if (!pipeline || !*pipeline)
      pipeline = "globaldce";
  }
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
  if (getenv("NYTRIX_WORKER") && !getenv("NYTRIX_WORKER_OPT")) {
    opt->opt_level = 0;
    opt->opt_pipeline = NULL;
    opt->opt_dce = 0;
  }

  /* Low-overhead compiler mode for development iteration. */
  bool low_overhead = ny_env_enabled("NYTRIX_FAST_COMPILE");
  bool fast_compiler = low_overhead || ny_env_enabled("NYTRIX_FAST_COMPILER");
  if (low_overhead || fast_compiler) {
    opt->opt_level = low_overhead ? 0 : 1;
    opt->opt_pipeline = NULL;
    opt->opt_loops = 0;
    opt->opt_autotune = 0;
    opt->verify_module = false;
    opt->debug_symbols = false;
    /* Enable JIT caching for faster subsequent runs */
    setenv("NYTRIX_JIT_CACHE", "1", 0);
    setenv("NYTRIX_JIT_OPT_LEVEL", low_overhead ? "0" : "1", 0);
    setenv("NYTRIX_JIT_FAST_ISEL", "1", 0);
  }
  verbose_enabled = opt->verbose;
  clock_t t_start = 0;
  clock_t t0 = 0;
  if (opt->do_timing) {
    t_start = clock();
    t0 = clock();
  }
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
  char aot_run_path[4096] = {0};
  bool aot_run_temp = false;
  bool loaded_from_cache = false;
  char *jit_cache_file = NULL;
#ifndef _WIN32
  char *native_cache_file = NULL;
  void *native_cache_handle = NULL;
  void (*native_cache_entry)(void) = NULL;
#endif
#ifdef _WIN32
  if (opt->run_aot && (!output_path || !*output_path)) {
    snprintf(aot_run_path, sizeof(aot_run_path), "%s/ny_aot_run_%d.exe",
             ny_get_temp_dir(), (int)getpid());
    opt->output_file = aot_run_path;
    output_path = opt->output_file;
    opt->emit_only = true;
    opt->run_jit = false;
    aot_run_temp = true;
  }
#else
  if (opt->run_aot && (!output_path || !*output_path)) {
    snprintf(aot_run_path, sizeof(aot_run_path), "%s/ny_aot_run_%d",
             ny_get_temp_dir(), (int)getpid());
    opt->output_file = aot_run_path;
    output_path = opt->output_file;
    opt->emit_only = true;
    opt->run_jit = false;
    aot_run_temp = true;
  }
#endif
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
  maybe_log_phase_time(opt->do_timing, "Read file:", t0);
  t0 = clock();
  uses = ny_collect_import_names(user_src, &use_count);
  maybe_log_phase_time(opt->do_timing, "Scan imports:", t0);
  t0 = clock();
  std_mode_t std_mode = opt->std_mode;
  const char *prebuilt_path = resolve_std_bundle(
      opt->std_path
          ? opt->std_path
          : (NYTRIX_STD_PATH ? NYTRIX_STD_PATH : "build/std_bundle.ny"));
  if (opt->no_std) {
    std_mode = STD_MODE_NONE;
  }
  if (std_mode == STD_MODE_DEFAULT && use_count == 0) {
    std_mode = STD_MODE_NONE;
  }
  clock_t t_std = clock();
  bool has_local = false;
  for (size_t i = 0; i < use_count; i++) {
    const char *u = uses[i];
    bool is_std = (strcmp(u, "std") == 0 || strncmp(u, "std.", 4) == 0);
    bool is_lib = (strcmp(u, "lib") == 0 || strncmp(u, "lib.", 4) == 0);
    if (!is_std && !is_lib) {
      has_local = true;
      break;
    }
  }
  bool std_sources_ok = ny_std_sources_available();
  bool prebuilt_ok = prebuilt_path && access(prebuilt_path, R_OK) == 0;
  bool prefer_prebuilt = ny_env_enabled("NYTRIX_STD_PREFER_PREBUILT");
  bool prebuilt_preferred =
      (std_mode == STD_MODE_FULL ||
       (prefer_prebuilt && std_mode == STD_MODE_DEFAULT)) &&
      !has_local;
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
  size_t slen = std_src ? strlen(std_src) : 0;
  size_t ulen = strlen(user_src);
  if (slen > (SIZE_MAX - ulen - 2)) {
    NY_LOG_ERR("Source code too large to concatenate\n");
    if (user_src)
      free(user_src);
    if (std_src)
      free(std_src);
    if (uses)
      ny_str_list_free(uses, use_count);
    return 1;
  }
  size_t line_directive_len = 0;
  char line_buf[1024];
  if (parse_name && parse_name[0] != '<') {
    line_directive_len =
        snprintf(line_buf, sizeof(line_buf), "#line 1 \"%s\"\n", parse_name);
  }

  source = malloc(slen + ulen + line_directive_len + 4);
  char *ptr = source;
  if (std_src) {
    memcpy(ptr, std_src, slen);
    ptr += slen;
    if (ptr > source && ptr[-1] != '\n')
      *ptr++ = '\n';
  }

  if (line_directive_len > 0) {
    memcpy(ptr, line_buf, line_directive_len);
    ptr += line_directive_len;
  }

  memcpy(ptr, user_src, ulen + 1);
#ifndef _WIN32
  if (opt->run_jit && ny_jit_cache_enabled() && ny_jit_native_cache_enabled() &&
      !opt->dump_ast && !opt->dump_llvm && !opt->emit_ir_path &&
      !opt->emit_bc_path && !opt->dump_tokens) {
    char *early_bc = ny_jit_cache_path(source, prebuilt_path, 0, opt->opt_level,
                                       opt->opt_dce, opt->opt_internalize,
                                       opt->debug_symbols,
                                       (unsigned long)ny_std_latest_mtime());
    if (early_bc) {
      char *early_so = ny_jit_native_cache_path(early_bc);
      if (early_so) {
        if (ny_jit_native_cache_load(early_so, &native_cache_handle,
                                     &native_cache_entry)) {
          if (opt->verbose)
            fprintf(stderr, "JIT native cache hit (early): %s\n", early_so);
          loaded_from_cache = true;
          free(early_so);
          free(early_bc);
          goto skip_compilation;
        }
        free(early_so);
      }
      free(early_bc);
    }
  }
#endif
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
          if (opt->run_aot) {
            const char *argv_exec[] = {output_path, NULL};
            int rc = ny_exec_spawn(argv_exec);
            if (rc != 0)
              exit_code = rc;
            if (aot_run_temp)
              (void)unlink(output_path);
          }
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

  clock_t t_codegen = clock();
  NY_LOG_V2("Initializing codegen_t for module 'nytrix'\n");
  codegen_init(&cg, &prog, arena, "nytrix");
  codegen_collect_links(&cg, &prog);
  /* Debug symbols only for AOT output, not for JIT execution */
  cg.debug_symbols = opt->debug_symbols && (opt->output_file != NULL);
  cg.debug_opt_level = opt->opt_level;
  if ((opt->run_jit || opt->output_file ||
       (opt->emit_only && !opt->output_file && !opt->run_jit)) &&
      opt->mode != NY_MODE_REPL && ny_jit_cache_enabled() && !opt->dump_ast &&
      !opt->dump_llvm && !opt->emit_ir_path && !opt->emit_bc_path) {
    jit_cache_file = ny_jit_cache_path(source, prebuilt_path, 0, opt->opt_level,
                                       opt->opt_dce, opt->opt_internalize,
                                       opt->debug_symbols,
                                       (unsigned long)ny_std_latest_mtime());
#ifndef _WIN32
    if (jit_cache_file && opt->run_jit && ny_jit_native_cache_enabled()) {
      native_cache_file = ny_jit_native_cache_path(jit_cache_file);
      if (native_cache_file &&
          ny_jit_native_cache_load(native_cache_file, &native_cache_handle,
                                   &native_cache_entry)) {
        if (opt->verbose)
          fprintf(stderr, "JIT native cache hit: %s\n", native_cache_file);
        loaded_from_cache = true;
        goto skip_compilation;
      }
    }
#endif
    if (jit_cache_file) {
      LLVMModuleRef cached_mod = NULL;
      if (ny_jit_cache_load(jit_cache_file, cg.ctx, &cached_mod)) {
        if (opt->verbose)
          fprintf(stderr, "JIT cache hit: %s\n", jit_cache_file);
        loaded_from_cache = true;
        LLVMDisposeModule(cg.module);
        cg.module = cached_mod;
        script_fn = LLVMGetNamedFunction(cg.module, "_ny_top_entry");
        if (!script_fn) {
          if (opt->verbose)
            fprintf(stderr, "JIT cache corrupt (missing entry): %s\n",
                    jit_cache_file);
          loaded_from_cache = false;
          cg.module = LLVMModuleCreateWithNameInContext("nytrix", cg.ctx);
          ny_llvm_prepare_module(cg.module, 3);
        } else {
          codegen_repopulate_interns(&cg);
          goto skip_compilation;
        }
      }
    }
  }

  if (opt->dump_ast) {
    for (size_t i = 0; i < prog.body.len; i++) {
      stmt_t *s = prog.body.data[i];
      printf("  [%zu] Kind=%d\n", i, s->kind);
    }
  }
  bool parallel_modules = false;
#ifndef _WIN32
  ny_module_list mods = {0};
  if (!fast_compiler && ny_parallel_modules_enabled(opt)) {
    ny_collect_top_modules(&prog, &mods);
    if (mods.len > 0)
      parallel_modules = true;
  }
#endif

  const char *std_bc_cache = NULL;
  bool use_std_bc_cache = false;
  if (opt->std_bc_path && access(opt->std_bc_path, R_OK) == 0) {
    cg.skip_stdlib = true;
    use_std_bc_cache = true;
    std_bc_cache = opt->std_bc_path;
  } else {
    const char *env_bc = getenv("NYTRIX_STD_BC_CACHE");
    if (!opt->no_std && env_bc && *env_bc && access(env_bc, R_OK) == 0) {
      cg.skip_stdlib = true;
      use_std_bc_cache = true;
      std_bc_cache = env_bc;
    }
  }
  if (use_std_bc_cache) {
    NY_LOG_INFO("linking stdlib bitcode cache: %s\n", std_bc_cache);
  }
  if (opt->emit_module) {
    cg.emit_module_name = opt->emit_module;
    cg.emit_module_decls_only = true;
    cg.emit_script = false;
  }
#ifndef _WIN32
  if (!opt->emit_module && parallel_modules) {
    cg.emit_module_name = "";
    cg.emit_module_decls_only = true;
  }
#endif
  if (cg.debug_symbols)
    codegen_debug_init(&cg, parse_name);
  cg.source_string = source;
  cg.prog_owned = false;
  NY_LOG_V2("Preparing codegen (analysis & links)...\n");
  codegen_prepare(&cg);
  if (cg.had_error) {
    NY_LOG_ERR("Codegen prepare failed\n");
    exit_code = 1;
    goto exit_success;
  }
#ifndef _WIN32
  ny_module_job *mod_jobs = NULL;
  size_t mod_job_count = 0;
  clock_t t_parallel = 0;
  size_t mods_len = 0;
  if (!fast_compiler && ny_parallel_modules_enabled(opt)) {
    if (opt->do_timing)
      t_parallel = clock();
    if (parallel_modules && mods.len > 0) {
      mods_len = mods.len;
      mod_jobs = calloc(mods.len, sizeof(ny_module_job));
      if (mod_jobs) {
        mod_job_count = mods.len;
        int max_jobs = ny_parallel_module_jobs(opt, mods.len);
        size_t started = 0;
        size_t finished = 0;
        size_t running = 0;
        const char *tmp_dir = ny_get_temp_dir();
        while (finished < mods.len) {
          while (started < mods.len && running < (size_t)max_jobs) {
            if (!ny_spawn_module_job(opt, mods.names[started], tmp_dir,
                                     &mod_jobs[started])) {
              parallel_modules = false;
              break;
            }
            started++;
            running++;
          }
          if (!parallel_modules)
            break;
          int status = 0;
          pid_t pid = wait(&status);
          if (pid < 0) {
            parallel_modules = false;
            break;
          }
          for (size_t i = 0; i < started; i++) {
            if (mod_jobs[i].pid == pid) {
              if (WIFEXITED(status))
                mod_jobs[i].exit_code = WEXITSTATUS(status);
              else
                mod_jobs[i].exit_code = 1;
              if (mod_jobs[i].exit_code != 0)
                parallel_modules = false;
              break;
            }
          }
          running--;
          finished++;
        }
        if (!parallel_modules) {
          for (size_t i = 0; i < started; i++) {
            if (mod_jobs[i].pid > 0) {
              int st = 0;
              (void)waitpid(mod_jobs[i].pid, &st, 0);
            }
          }
        }
      } else {
        parallel_modules = false;
      }
    } else {
      parallel_modules = false;
    }
  }
  ny_free_module_list(&mods);
  if (mods_len > 0 && !parallel_modules) {
    NY_LOG_ERR("Parallel module build failed\n");
    exit_code = 1;
    goto exit_success;
  }
  if (!parallel_modules && mod_jobs) {
    for (size_t i = 0; i < mod_job_count; i++) {
      if (mod_jobs[i].bc_path)
        (void)unlink(mod_jobs[i].bc_path);
      ny_module_job_free(&mod_jobs[i]);
    }
    free(mod_jobs);
    mod_jobs = NULL;
    mod_job_count = 0;
  }
#endif
  NY_LOG_V2("Emitting IR...\n");
  codegen_emit(&cg);
  if (cg.had_error) {
    NY_LOG_ERR("Codegen failed\n");
    dump_debug_bundle(opt, source, cg.module);
    exit_code = 1;
    goto exit_success;
  }
  if (use_std_bc_cache) {
    ny_ir_externalize_std_definitions(opt, cg.module);
    if (!ny_link_module_cache(cg.ctx, cg.module, std_bc_cache)) {
      NY_LOG_ERR("Failed to link std cache: %s\n", std_bc_cache);
      exit_code = 1;
      goto exit_success;
    }
  }
  if (cg.emit_script) {
    NY_LOG_V2("Emitting script entry point...\n");
    script_fn = codegen_emit_script(&cg, opt->entry_name ? opt->entry_name
                                                         : "_ny_top_entry");
    if (cg.had_error) {
      NY_LOG_ERR("Codegen script entry failed\n");
      dump_debug_bundle(opt, source, cg.module);
      exit_code = 1;
      goto exit_success;
    }
  }
#ifndef _WIN32
  if (parallel_modules && mod_jobs) {
    for (size_t i = 0; i < mod_job_count; i++) {
      if (!mod_jobs[i].bc_path)
        continue;
      if (!ny_link_module_cache(cg.ctx, cg.module, mod_jobs[i].bc_path)) {
        NY_LOG_ERR("Failed to link module cache for %s\n",
                   mod_jobs[i].name ? mod_jobs[i].name : "<module>");
        exit_code = 1;
        goto exit_success;
      }
      (void)unlink(mod_jobs[i].bc_path);
      ny_module_job_free(&mod_jobs[i]);
    }
    free(mod_jobs);
    mod_jobs = NULL;
    mod_job_count = 0;
    if (opt->do_timing && t_parallel)
      fprintf(stderr, "Parallel modules: %.4fs\n",
              (double)(clock() - t_parallel) / CLOCKS_PER_SEC);
  }
#endif
  codegen_debug_finalize(&cg);
  maybe_log_phase_time(opt->do_timing, "Codegen:", t_codegen);

  if (opt->dump_llvm) {
    LLVMModuleRef dump_mod = ny_prepare_ir_dump_module(opt, cg.module);
    LLVMDumpModule(dump_mod ? dump_mod : cg.module);
    if (dump_mod)
      LLVMDisposeModule(dump_mod);
  }
  if (ny_env_enabled("NY_IR_DUMP")) {
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
  ny_llvm_apply_host_attrs(cg.module);
  run_dead_strip_if_needed(opt, &cg, cg.module);
  if (opt->emit_ir_pre_path) {
    LLVMModuleRef dump_mod = ny_prepare_ir_dump_module(opt, cg.module);
    ny_dump_ir_if_requested(dump_mod ? dump_mod : cg.module,
                            opt->emit_ir_pre_path, "pre-opt");
    if (dump_mod)
      LLVMDisposeModule(dump_mod);
  }

  if (!fast_compiler) {
    int eff_opt = opt->opt_level;
    if (parallel_modules && !ny_env_enabled("NYTRIX_PARALLEL_OPT_LINK")) {
      eff_opt = 0;
    }
    if (opt->run_jit) {
      int jit_level = ny_env_int("NYTRIX_JIT_OPT_LEVEL", -1);
      if (jit_level < 0)
        jit_level = 1;
      if (jit_level > eff_opt)
        eff_opt = jit_level;
    }
    /* Finalize debug info BEFORE optimization */
    if (cg.di_builder) {
      codegen_debug_finalize(&cg);
    }
    ny_llvm_optimize_module(cg.module, eff_opt, opt->opt_loops,
                            opt->opt_pipeline);
    if (opt->do_timing && (opt->opt_level > 0 || opt->opt_pipeline))
      fprintf(stderr, "Optimization: %.4fs\n",
              (double)(clock() - t_opt) / CLOCKS_PER_SEC);
  }
  if (jit_cache_file && !loaded_from_cache) {
    if (native_cache_file && opt->run_jit) {
      clock_t t_native = clock();
      if (ny_jit_native_cache_save(native_cache_file, cg.module, opt->opt_level,
                                   (const char *const *)cg.links.data,
                                   cg.links.len)) {
        if (opt->verbose)
          fprintf(stderr, "JIT native cache saved: %s\n", native_cache_file);
      }
      maybe_log_phase_time(opt->do_timing, "Native Cache:", t_native);
    }
    if (ny_jit_cache_save(jit_cache_file, cg.module)) {
      if (opt->verbose)
        fprintf(stderr, "JIT cache saved: %s\n", jit_cache_file);
    }
  }
skip_compilation:
  if (jit_cache_file)
    free(jit_cache_file);
  if (loaded_from_cache && cg.module)
    codegen_prepare(&cg);

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
  if (opt->emit_bc_path) {
    bool wrote = ny_reemit_bitcode_via_ir(cg.module, opt->emit_bc_path);
    if (wrote && !ny_verify_bitcode(cg.ctx, opt->emit_bc_path)) {
      wrote = false;
    }
    if (!wrote) {
      NY_LOG_ERR("Failed to write bitcode to %s\n", opt->emit_bc_path);
      exit_code = 1;
      goto exit_success;
    }
  }
  if (opt->emit_asm_path) {
    if (!ny_llvm_emit_file(cg.module, opt->emit_asm_path, LLVMAssemblyFile,
                           opt->opt_level)) {
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
    bool is_obj_only =
        (output_path && strlen(output_path) > 2 &&
         strcmp(output_path + strlen(output_path) - 2, ".o") == 0);
    if (is_obj_only) {
      if (ny_llvm_emit_object(cg.module, output_path, opt->opt_level)) {
        NY_LOG_SUCCESS("Saved object: %s\n", output_path);
        if (opt->run_aot) {
          NY_LOG_ERR("Cannot run AOT from object file\n");
          exit_code = 1;
          goto exit_success;
        }
      } else {
        NY_LOG_ERR("Failed to emit object file\n");
        exit_code = 1;
        goto exit_success;
      }
    } else if (ny_llvm_emit_object(cg.module, obj, opt->opt_level)) {
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
      if (output_path && *output_path) {
        char out_dir[1024];
        snprintf(out_dir, sizeof(out_dir), "%s", output_path);
        char *slash = strrchr(out_dir, '/');
        if (slash && slash != out_dir) {
          *slash = '\0';
          ny_ensure_dir_recursive(out_dir);
        }
      }
      NY_LOG_V2("Linking executable %s (strip=%d, debug=%d)...\n", output_path,
                link_strip, opt->debug_symbols);
      /* Merge #link directives from codegen into link_libs */
      VEC(char *) merged_libs;
      vec_init(&merged_libs);
      for (size_t li = 0; li < opt->link_libs.len; li++) {
        const char *lib = opt->link_libs.data[li];
        /* Convert libXXX.so -> -lXXX for ELF linker */
        if (lib && strncmp(lib, "lib", 3) == 0) {
          const char *base = lib + 3;
          size_t blen = strlen(base);
          const char *dot = strstr(base, ".so");
          if (dot && dot > base)
            blen = (size_t)(dot - base);
          if (blen > 0 && blen < 256) {
            char buf[260];
            snprintf(buf, sizeof(buf), "-l%.*s", (int)blen, base);
            vec_push(&merged_libs, ny_strdup(buf));
            continue;
          }
        }
        vec_push(&merged_libs, ny_strdup(lib));
      }
      for (size_t li = 0; li < cg.links.len; li++) {
        const char *name = cg.links.data[li];
        if (!name)
          continue;
        /* Convert libXXX.so[.N] -> XXX for -lXXX format */
        const char *lib_name = name;
        size_t lib_len = strlen(name);
        if (strncmp(name, "lib", 3) == 0) {
          lib_name = name + 3;
          lib_len = strlen(lib_name);
          const char *dot = strstr(lib_name, ".so");
          if (dot && dot > lib_name)
            lib_len = (size_t)(dot - lib_name);
        }
        /* Check for duplicates */
        bool dup = false;
        for (size_t lj = 0; lj < merged_libs.len; lj++) {
          const char *e = merged_libs.data[lj];
          if (e && e[0] == '-' && e[1] == 'l' &&
              strncmp(e + 2, lib_name, lib_len) == 0 &&
              e[2 + lib_len] == '\0') {
            dup = true;
            break;
          }
        }
        if (!dup) {
          char buf[260];
          snprintf(buf, sizeof(buf), "-l%.*s", (int)lib_len, lib_name);
          vec_push(&merged_libs, ny_strdup(buf));
        }
      }
      if (!ny_builder_link(
              cc, obj, rto, NULL, NULL, 0,
              (const char *const *)opt->link_dirs.data, opt->link_dirs.len,
              (const char *const *)merged_libs.data, merged_libs.len,
              output_path, link_strip, opt->debug_symbols, opt->gprof == 1)) {
        unlink(obj);
        unlink(rto);
        dump_debug_bundle(opt, source, cg.module);
        exit_code = 1;
        for (size_t li = 0; li < merged_libs.len; li++)
          free(merged_libs.data[li]);
        vec_free(&merged_libs);
        goto exit_success;
      }
      for (size_t li = 0; li < merged_libs.len; li++)
        free(merged_libs.data[li]);
      vec_free(&merged_libs);
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
    if (opt->run_aot) {
      const char *argv_exec[] = {output_path, NULL};
      int rc = ny_exec_spawn(argv_exec);
      if (rc != 0)
        exit_code = rc;
      if (aot_run_temp)
        (void)unlink(output_path);
    }
  }
  if (opt->run_jit) {
#ifndef _WIN32
    if (native_cache_entry) {
      clock_t t_run = clock();
      native_cache_entry();
      extern void rt_print_flush(void);
      rt_print_flush();
      maybe_log_phase_time(opt->do_timing, "JIT Run:", t_run);
    } else
#endif
    {
      clock_t t_jit = clock();
      LLVMLinkInMCJIT();
      ny_llvm_init_native();
      LLVMExecutionEngineRef ee;
      char *err = NULL;
      LLVMModuleRef jmod = cg.module;
      /* Strip DWARF debug info before handing the module to MCJIT.
         DwarfDebug::finishEntityDefinitions can crash during MCJIT code
         emission with certain metadata patterns. MCJIT doesn't use DWARF for
         execution. */
      if (opt->debug_symbols)
        LLVMStripModuleDebugInfo(jmod);
      struct LLVMMCJITCompilerOptions jopt;
      ny_jit_init_options(&jopt, jmod);
      jopt.OptLevel = (unsigned)ny_jit_codegen_opt_level(opt);
      {
        if (ny_env_enabled_default_on("NYTRIX_JIT_FAST_ISEL"))
          jopt.EnableFastISel = 1;
      }
      if (LLVMCreateMCJITCompilerForModule(&ee, jmod, &jopt, sizeof(jopt),
                                           &err)) {
        NY_LOG_ERR("JIT failed: %s\n", err);
        dump_debug_bundle(opt, source, jmod);
        exit_code = 1;
        goto exit_success;
      }
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
      ny_jit_map_unresolved_symbols(ee, jmod, NULL);
      ny_jit_write_perf_map(ee, jmod);
      maybe_log_phase_time(opt->do_timing, "JIT Init:", t_jit);
      clock_t t_exec = clock();
      uint64_t saddr = LLVMGetFunctionAddress(ee, "_ny_top_entry");
      maybe_log_phase_time(opt->do_timing, "JIT Compile:", t_exec);
      clock_t t_run = clock();
      if (saddr) {
        if (verbose_enabled >= 3)
          fprintf(stderr, "TRACE: Executing script...\n");
        ((void (*)(void))saddr)();
        extern void rt_print_flush(void);
        rt_print_flush();
        if (verbose_enabled >= 3)
          fprintf(stderr, "TRACE: Script finished.\n");
      } else {
        if (verbose_enabled >= 3)
          fprintf(stderr, "TRACE: __script_top NOT FOUND\n");
      }
      maybe_log_phase_time(opt->do_timing, "JIT Run:", t_run);
      LLVMDisposeExecutionEngine(ee);
    }
  }
exit_success:
#ifndef _WIN32
  if (native_cache_handle)
    dlclose(native_cache_handle);
  if (native_cache_file)
    free(native_cache_file);
#endif
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
