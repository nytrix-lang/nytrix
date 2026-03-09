#include "wire/cache.h"
#include "base/common.h"
#include "base/hash.h"
#include "base/loader.h"
#include "base/util.h"
#include <llvm-c/Analysis.h>
#include <llvm-c/BitReader.h>
#include <llvm-c/BitWriter.h>
#include <llvm-c/Core.h>
#include <llvm-c/DebugInfo.h>
#include <llvm-c/IRReader.h>
#include <llvm/Config/llvm-config.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
#include "base/dirent.h"
#include <direct.h>
#include <windows.h>
#define mkdir(p, m) _mkdir(p)
#define rmdir _rmdir
#else
#include <dirent.h>
#include <dlfcn.h>
#include <unistd.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static unsigned long ny_hash_string(const char *str) {
  unsigned long hash = 5381;
  int c;
  while ((c = *str++))
    hash = ((hash << 5) + hash) + c;
  return hash;
}

static uint64_t ny_stat_mtime_nsec(const struct stat *st) {
  if (!st)
    return 0;
#if defined(__APPLE__)
  return (uint64_t)st->st_mtimespec.tv_nsec;
#elif !defined(_WIN32)
  return (uint64_t)st->st_mtim.tv_nsec;
#else
  return 0;
#endif
}

static bool ny_write_text_file_atomic(const char *path, const char *content, size_t len);
static bool ny_cache_dir_ready(const char *path);

static uint64_t ny_cache_source_tree_fingerprint(const char *path) {
  if (!path || !*path)
    return 0;
  struct stat st;
  if (stat(path, &st) != 0)
    return 0;
  uint64_t h = NY_FNV1A64_OFFSET_BASIS;
  h = ny_fnv1a64_cstr(path, h);
  h = ny_hash64_u64(h, (uint64_t)st.st_mtime);
  h = ny_hash64_u64(h, ny_stat_mtime_nsec(&st));
  h = ny_hash64_u64(h, (uint64_t)st.st_size);
  if (!S_ISDIR(st.st_mode))
    return h;

  DIR *d = opendir(path);
  if (!d)
    return h;
  uint64_t acc = NY_FNV1A64_OFFSET_BASIS;
  uint64_t count = 0;
  struct dirent *ent;
  while ((ent = readdir(d)) != NULL) {
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
      continue;
    char child[PATH_MAX];
    snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
    struct stat cst;
    if (stat(child, &cst) != 0)
      continue;
    if (!S_ISDIR(cst.st_mode) && !S_ISREG(cst.st_mode))
      continue;
    uint64_t ch = ny_cache_source_tree_fingerprint(child);
    acc ^= ch + 0x9E3779B97F4A7C15ULL + (ch << 6) + (ch >> 2);
    count++;
  }
  closedir(d);
  h = ny_hash64_u64(h, count);
  h = ny_hash64_u64(h, acc);
  return h;
}

static uint64_t ny_cache_compiler_fingerprint_stamp_key(const char *root,
                                                       const char *exe_path) {
  uint64_t h = NY_FNV1A64_OFFSET_BASIS;
  h = ny_fnv1a64_cstr("compiler-source-stamp-v1", h);
  h = ny_fnv1a64_cstr(root ? root : "", h);
  h = ny_fnv1a64_cstr(exe_path ? exe_path : "", h);
  if (exe_path && *exe_path) {
    struct stat st;
    if (stat(exe_path, &st) == 0) {
      h = ny_hash64_u64(h, (uint64_t)st.st_mtime);
      h = ny_hash64_u64(h, ny_stat_mtime_nsec(&st));
      h = ny_hash64_u64(h, (uint64_t)st.st_size);
    }
  }
#ifdef LLVM_VERSION_STRING
  h = ny_fnv1a64_cstr(LLVM_VERSION_STRING, h);
#endif
#ifdef NYTRIX_VERSION_COMMIT
  h = ny_fnv1a64_cstr(NYTRIX_VERSION_COMMIT, h);
#endif
#ifdef NYTRIX_VERSION_DIRTY
  h = ny_hash64_u64(h, (uint64_t)NYTRIX_VERSION_DIRTY);
#endif
#ifdef NYTRIX_BUILD_HASH
  h = ny_fnv1a64_cstr(NYTRIX_BUILD_HASH, h);
#endif
  return h ? h : 1;
}

static bool ny_cache_compiler_fingerprint_stamp_path(char *out, size_t out_len,
                                                     uint64_t key) {
  if (!out || out_len == 0)
    return false;
  const char *cache_root = ny_cache_root_dir();
  if (!cache_root || !*cache_root)
    return false;
  char dir[PATH_MAX];
  snprintf(dir, sizeof(dir), "%s/compiler", cache_root);
  if (!ny_cache_dir_ready(dir))
    return false;
  snprintf(out, out_len, "%s/source_fp_%016llx.stamp", dir,
           (unsigned long long)key);
  return true;
}

static bool ny_cache_read_compiler_fingerprint_stamp(const char *path,
                                                     uint64_t key,
                                                     uint64_t *out_hash) {
  if (!path || !*path || !out_hash)
    return false;
  size_t len = 0;
  char *raw = ny_read_file_raw(path, &len);
  if (!raw)
    return false;
  unsigned long long stored_key = 0;
  unsigned long long stored_hash = 0;
  bool ok = sscanf(raw, "%llx %llx", &stored_key, &stored_hash) == 2 &&
            (uint64_t)stored_key == key && stored_hash != 0;
  free(raw);
  if (!ok)
    return false;
  *out_hash = (uint64_t)stored_hash;
  return true;
}

static void ny_cache_write_compiler_fingerprint_stamp(const char *path,
                                                      uint64_t key,
                                                      uint64_t hash) {
  if (!path || !*path || !hash)
    return;
  char buf[80];
  int n = snprintf(buf, sizeof(buf), "%016llx %016llx\n",
                   (unsigned long long)key, (unsigned long long)hash);
  if (n > 0 && (size_t)n < sizeof(buf))
    (void)ny_write_text_file_atomic(path, buf, (size_t)n);
}

static uint64_t ny_cache_compiler_source_fingerprint(void) {
  static int cached = 0;
  static uint64_t cached_hash = 0;
  if (cached)
    return cached_hash;
  const char *root = ny_src_root();
  char *exe_path = ny_get_executable_path();
  const uint64_t stamp_key =
      ny_cache_compiler_fingerprint_stamp_key(root, exe_path);
  char stamp_path[PATH_MAX] = {0};
  if (ny_cache_compiler_fingerprint_stamp_path(stamp_path, sizeof(stamp_path),
                                               stamp_key) &&
      ny_cache_read_compiler_fingerprint_stamp(stamp_path, stamp_key,
                                               &cached_hash)) {
    cached = 1;
    return cached_hash;
  }
  uint64_t h = NY_FNV1A64_OFFSET_BASIS;
  h = ny_fnv1a64_cstr("compiler-source-v1", h);
  h = ny_fnv1a64_cstr(root ? root : "", h);
  if (root && *root) {
    const char *parts[] = {
        "CMakeLists.txt", "src/base", "src/code", "src/parse",
        "src/repl",       "src/rt",   "src/sema", "src/wire",
    };
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); ++i) {
      char path[PATH_MAX];
      snprintf(path, sizeof(path), "%s/%s", root, parts[i]);
      h = ny_hash64_u64(h, ny_cache_source_tree_fingerprint(path));
    }
  }
#ifdef NYTRIX_VERSION_COMMIT
  h = ny_fnv1a64_cstr(NYTRIX_VERSION_COMMIT, h);
#endif
#ifdef NYTRIX_VERSION_DIRTY
  h = ny_hash64_u64(h, (uint64_t)NYTRIX_VERSION_DIRTY);
#endif
#ifdef NYTRIX_BUILD_HASH
  h = ny_fnv1a64_cstr(NYTRIX_BUILD_HASH, h);
#endif
  cached_hash = h ? h : 1;
  if (stamp_path[0])
    ny_cache_write_compiler_fingerprint_stamp(stamp_path, stamp_key,
                                              cached_hash);
  cached = 1;
  return cached_hash;
}

static bool ny_write_text_file_atomic(const char *path, const char *content, size_t len) {
  if (!path || !*path || !content)
    return false;
#ifndef _WIN32
  char tmp_path[PATH_MAX];
  snprintf(tmp_path, sizeof(tmp_path), "%s.%ld.XXXXXX", path, (long)getpid());
  int fd = mkstemp(tmp_path);
  if (fd < 0)
    return false;
  FILE *f = fdopen(fd, "wb");
  if (!f) {
    close(fd);
    remove(tmp_path);
    return false;
  }
#else
  char tmp_path[1024];
  snprintf(tmp_path, sizeof(tmp_path), "%s.%lu.tmp", path, (unsigned long)GetCurrentProcessId());
  FILE *f = fopen(tmp_path, "wb");
  if (!f)
    return false;
#endif
  bool wrote = fwrite(content, 1, len, f) == len;
  bool closed = fclose(f) == 0;
  if (!wrote || !closed) {
    remove(tmp_path);
    return false;
  }
  if (rename(tmp_path, path) != 0) {
    remove(tmp_path);
    return false;
  }
  return true;
}

static bool ny_cache_dir_ready(const char *path) {
  if (!path || !*path)
    return false;
  ny_ensure_dir_recursive(path);
  char probe[1024];
  snprintf(probe, sizeof(probe), "%s/.nytrix_cache_probe_%ld", path, (long)getpid());
  FILE *f = fopen(probe, "wb");
  if (!f)
    return false;
  bool ok = fclose(f) == 0;
  remove(probe);
  return ok;
}

static bool ny_trace_cache_enabled(void) { return ny_env_enabled("NYTRIX_TRACE_CACHE"); }

bool ny_cache_path_is_ir(const char *cache_path) {
  if (!cache_path)
    return false;
  const char *ext = strrchr(cache_path, '.');
  if (!ext)
    return false;
  return strcmp(ext, ".ll") == 0 || strcmp(ext, ".ir") == 0 || strcmp(ext, ".llvm") == 0;
}

static bool ny_jit_cache_use_ir(void) {
  const char *env = ny_env_str_nonempty("NYTRIX_JIT_CACHE_FORMAT");
  if (!env || !*env)
    return false;
  if (strcmp(env, "bc") == 0 || strcmp(env, "bitcode") == 0)
    return false;
  return strcmp(env, "ir") == 0 || strcmp(env, "ll") == 0 || strcmp(env, "text") == 0 ||
         strcmp(env, "llvm") == 0;
}

static char *ny_get_cache_dir(void) {
  static char path[1024];
  const char *override = ny_env_str_nonempty("NYTRIX_CACHE_DIR");
  if (override && *override) {
    snprintf(path, sizeof(path), "%s", override);
    if (ny_cache_dir_ready(path))
      return path;
  }
  const char *src = ny_src_root();
  if (src && *src) {
    snprintf(path, sizeof(path), "%s/build/cache/nytrix/jit", src);
    if (ny_cache_dir_ready(path))
      return path;
  }
  const char *xdg = ny_env_str_nonempty("XDG_CACHE_HOME");
  if (xdg && *xdg) {
    snprintf(path, sizeof(path), "%s/nytrix/jit", xdg);
    if (ny_cache_dir_ready(path))
      return path;
  }
  const char *home = ny_env_str_nonempty("HOME");
  if (!home)
    home = ny_env_str_nonempty("USERPROFILE");
  if (home && *home) {
    snprintf(path, sizeof(path), "%s/.cache/nytrix/jit", home);
    if (ny_cache_dir_ready(path))
      return path;
  }
  snprintf(path, sizeof(path), "%s/nytrix/jit", ny_get_temp_dir());
  ny_cache_dir_ready(path);
  return path;
}

const char *ny_cache_root_dir(void) {
  static char path[1024];
  const char *override = ny_env_str_nonempty("NYTRIX_CACHE_DIR");
  if (override && *override) {
    snprintf(path, sizeof(path), "%s", override);
    return path;
  }
  const char *src = ny_src_root();
  if (src && *src) {
    snprintf(path, sizeof(path), "%s/build/cache/nytrix", src);
    return path;
  }
  const char *xdg = ny_env_str_nonempty("XDG_CACHE_HOME");
  if (xdg && *xdg) {
    snprintf(path, sizeof(path), "%s/nytrix", xdg);
    return path;
  }
  const char *home = ny_env_str_nonempty("HOME");
  if (!home)
    home = ny_env_str_nonempty("USERPROFILE");
  if (home && *home) {
    snprintf(path, sizeof(path), "%s/.cache/nytrix", home);
    return path;
  }
  snprintf(path, sizeof(path), "%s/nytrix", ny_get_temp_dir());
  return path;
}

static int ny_cache_remove_tree(const char *path) {
  struct stat st;
  if (!path || !*path || stat(path, &st) != 0)
    return 0;
  if (S_ISDIR(st.st_mode)) {
    DIR *d = opendir(path);
    if (!d)
      return -1;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
      if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
        continue;
      char child[PATH_MAX];
      snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
      if (ny_cache_remove_tree(child) != 0) {
        closedir(d);
        return -1;
      }
    }
    closedir(d);
    return rmdir(path);
  }
  return remove(path);
}

static int ny_cache_remove_prefix(const char *dir, const char *prefix) {
  if (!dir || !*dir || !prefix || !*prefix)
    return 0;
  DIR *d = opendir(dir);
  if (!d)
    return 0;
  size_t prefix_len = strlen(prefix);
  int rc = 0;
  struct dirent *ent;
  while ((ent = readdir(d)) != NULL) {
    if (strncmp(ent->d_name, prefix, prefix_len) != 0)
      continue;
    char child[PATH_MAX];
    snprintf(child, sizeof(child), "%s/%s", dir, ent->d_name);
    if (ny_cache_remove_tree(child) != 0)
      rc = -1;
  }
  closedir(d);
  return rc;
}

int ny_cache_clean(void) {
  int rc = 0;
  const char *root = ny_cache_root_dir();
  if (root && *root && ny_cache_remove_tree(root) != 0)
    rc = -1;

  const char *tmp = ny_get_temp_dir();
  if (tmp && *tmp) {
    if (ny_cache_remove_prefix(tmp, "ny_aot_cache_") != 0)
      rc = -1;
    if (ny_cache_remove_prefix(tmp, "ny_aot_run_") != 0)
      rc = -1;
    if (ny_cache_remove_prefix(tmp, "ny_std_cache_") != 0)
      rc = -1;
  }

  const char *src = ny_src_root();
  if (src && *src) {
    char tiny[PATH_MAX];
    snprintf(tiny, sizeof(tiny), "%s/build/cache/tiny-aot", src);
    if (ny_cache_remove_tree(tiny) != 0)
      rc = -1;
  }
  return rc;
}

bool ny_jit_cache_enabled(void) { return ny_env_enabled_default_on("NYTRIX_JIT_CACHE"); }

/* Bump these whenever cached bitcode assumptions change.  The test tree
 * layout is part of the source/cache identity because embedded paths can flow
 * into generated modules and stale bitcode should not survive a layout move. */
enum { NY_JIT_CACHE_VERSION = 25 };
enum { NY_STD_BC_CACHE_VERSION = 14 };

static bool ny_cache_strict_file_id_enabled(void) {
  return ny_env_enabled("NYTRIX_CACHE_STRICT_FILE_ID");
}

static bool ny_std_bc_path_is_generated_build_artifact(const char *path) {
  if (!path || !*path)
    return false;
  const char *root = ny_src_root();
  if (!root || !*root)
    return false;
  char prefix[PATH_MAX];
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

char *ny_jit_cache_path(const char *source, const char *stdlib_path, unsigned long std_src_hash,
                        int opt_level, int opt_dce, int opt_internalize, bool debug_symbols,
                        unsigned long std_latest_mtime) {
  if (!source)
    return NULL;
  char *dir = ny_get_cache_dir();
  if (!dir)
    return NULL;
  ny_ensure_dir_recursive(dir);
  unsigned long src_hash = ny_hash_string(source);
  unsigned long std_hash = 0;
  if (stdlib_path) {
    if (std_src_hash) {
      std_hash = std_src_hash;
    } else if (ny_env_enabled("NYTRIX_JIT_CACHE_STRICT_MTIME")) {
      struct stat st;
      if (stat(stdlib_path, &st) == 0) {
        std_hash = (unsigned long)st.st_mtime;
      }
    }
    std_hash ^= ny_hash_string(stdlib_path);
  }
  /* The JIT source hash already covers the generated std.ny text.  Hashing
     mtimes here churns large native-cache keys when generated std artifacts are
     refreshed without content changes, so keep strict mtime invalidation opt-in. */
  if (std_latest_mtime && ny_env_enabled("NYTRIX_JIT_CACHE_STRICT_MTIME")) {
    std_hash ^= std_latest_mtime;
  }
#ifdef LLVM_VERSION_STRING
  std_hash ^= ny_hash_string(LLVM_VERSION_STRING);
#endif
  std_hash ^= ny_hash_string(VERSION);
#ifdef NYTRIX_VERSION_COMMIT
  std_hash ^= ny_hash_string(NYTRIX_VERSION_COMMIT);
#endif
#ifdef NYTRIX_VERSION_DIRTY
  std_hash ^= (unsigned long)NYTRIX_VERSION_DIRTY;
#endif
  std_hash ^= (unsigned long)opt_level;
  std_hash ^= (unsigned long)opt_dce;
  std_hash ^= (unsigned long)opt_internalize;
  if (debug_symbols)
    std_hash ^= 0xDEADBEEF;
  if (ny_env_enabled("NYTRIX_FAST_MODE"))
    std_hash ^= 0xCAFEBABE;
  if (ny_env_enabled("NYTRIX_FAST_INT_BINOPS"))
    std_hash ^= 0x12345678;
  if (ny_env_enabled("NYTRIX_FAST_FLOAT_BINOPS"))
    std_hash ^= 0x87654321;
  if (ny_env_enabled("NYTRIX_ASSUME_INT"))
    std_hash ^= 0xA5A5A5A5;
  if (ny_env_enabled("NYTRIX_TRACE"))
    std_hash ^= 0x71726163655ULL;
  {
    const char *const envs[] = {"NYTRIX_COMPILER_ASSERTS",
                                "NYTRIX_DEBUG_LOCALS",
                                "NYTRIX_DWARF_VERSION",
                                "NYTRIX_DWARF_SPLIT_INLINING",
                                "NYTRIX_DWARF_PROFILE_INFO",
                                "NYTRIX_HOST_TRIPLE",
                                "NYTRIX_HOST_CFLAGS",
                                "NYTRIX_HOST_LDFLAGS",
                                "NYTRIX_ARM_FLOAT_ABI",
                                "NYTRIX_OWNERSHIP",
                                "NYTRIX_OWNERSHIP_STRICT",
                                "NYTRIX_OPT_PROFILE",
                                "NYTRIX_MONO_TYPES",
                                "NYTRIX_ENABLE_MONOMORPHIZATION",
                                "NYTRIX_DISABLE_MONO_TYPES",
                                "NYTRIX_DISABLE_MONOMORPHIZATION",
                                "NYTRIX_MONO_IMPERATIVE",
                                "NYTRIX_SIMPLE_RAW_INT_CALL_FAST",
                                "NYTRIX_FAST_ALL_PROFILES",
                                "NYTRIX_PROVEN_RAW_INT_EXPR_FAST",
                                "NYTRIX_RAW_INT_EXPR_FAST",
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
    std_hash ^= (unsigned long)ny_hash_envv(NY_FNV1A64_OFFSET_BASIS, envs,
                                            sizeof(envs) / sizeof(envs[0]));
  }

  unsigned long compiler_hash = 0;
  char *exe_path = ny_get_executable_path();
  if (exe_path) {
    struct stat st;
    if (stat(exe_path, &st) == 0) {
      uint64_t ch = NY_FNV1A64_OFFSET_BASIS;
      ch = ny_hash64_u64(ch, (uint64_t)st.st_mtime);
      ch = ny_hash64_u64(ch, (uint64_t)st.st_size);
      ch = ny_hash64_u64(ch, ny_stat_mtime_nsec(&st));
#ifdef NYTRIX_VERSION_COMMIT
      ch = ny_hash64_u64(ch, (uint64_t)ny_hash_string(NYTRIX_VERSION_COMMIT));
#endif
#ifdef NYTRIX_VERSION_DIRTY
      ch = ny_hash64_u64(ch, (uint64_t)NYTRIX_VERSION_DIRTY);
#endif
#ifdef NYTRIX_BUILD_HASH
      ch = ny_hash64_u64(ch, (uint64_t)ny_hash_string(NYTRIX_BUILD_HASH));
#endif
      compiler_hash = (unsigned long)ch;
    }
  }

  std_hash ^= compiler_hash;
  std_hash ^= (unsigned long)NY_JIT_CACHE_VERSION;
  static char path[1024];
  const char *ext = ny_jit_cache_use_ir() ? "ll" : "bc";
  snprintf(path, sizeof(path), "%s/%lx_%lx.%s", dir, src_hash, std_hash, ext);
  return strdup(path);
}

char *ny_std_bc_cache_path(const char *stdlib_path, const char *const *uses, size_t use_count,
                           int std_mode, bool debug_symbols, unsigned long std_latest_mtime,
                           const char *argv0) {
  char dir[1024];
  const char *override = ny_env_str_nonempty("NYTRIX_CACHE_DIR");
  if (override && *override) {
    snprintf(dir, sizeof(dir), "%s/std", override);
  } else {
    const char *src = ny_src_root();
    const char *xdg = ny_env_str_nonempty("XDG_CACHE_HOME");
    if (src && *src) {
      snprintf(dir, sizeof(dir), "%s/build/cache/nytrix/std", src);
    } else if (xdg && *xdg) {
      snprintf(dir, sizeof(dir), "%s/nytrix/std", xdg);
    } else {
      const char *home = ny_env_str_nonempty("HOME");
      if (!home)
        home = ny_env_str_nonempty("USERPROFILE");
      if (home && *home)
        snprintf(dir, sizeof(dir), "%s/.cache/nytrix/std", home);
      else
        snprintf(dir, sizeof(dir), "%s/nytrix/std", ny_get_temp_dir());
    }
  }
  ny_cache_dir_ready(dir);

  uint64_t h = NY_FNV1A64_OFFSET_BASIS;
  h = ny_fnv1a64_cstr("std-bc-cache-v3", h);
  h = ny_hash64_u64(h, (uint64_t)(unsigned)std_mode);
  h = ny_hash_cstrv(h, uses, use_count);
  h = ny_fnv1a64_cstr(stdlib_path, h);
  h = ny_hash64_u64(h, (uint64_t)std_latest_mtime);
  uint64_t source_fingerprint = ny_std_source_fingerprint();
  h = ny_hash64_u64(h, source_fingerprint);
  bool strict_file_id = ny_cache_strict_file_id_enabled();
  uint64_t stdlib_mtime = 0;
  uint64_t stdlib_mtime_nsec = 0;
  uint64_t stdlib_ctime = 0;
  uint64_t stdlib_ino = 0;
  uint64_t stdlib_dev = 0;
  uint64_t stdlib_size = 0;
  if (stdlib_path && *stdlib_path &&
      !ny_std_bc_path_is_generated_build_artifact(stdlib_path)) {
    struct stat st;
    if (stat(stdlib_path, &st) == 0) {
      stdlib_mtime = (uint64_t)st.st_mtime;
      stdlib_mtime_nsec = ny_stat_mtime_nsec(&st);
      stdlib_ctime = (uint64_t)st.st_ctime;
      stdlib_ino = (uint64_t)st.st_ino;
      stdlib_dev = (uint64_t)st.st_dev;
      stdlib_size = (uint64_t)st.st_size;
      h = ny_hash64_u64(h, stdlib_mtime);
      h = ny_hash64_u64(h, stdlib_mtime_nsec);
      h = ny_hash64_u64(h, stdlib_size);
      if (strict_file_id) {
        h = ny_hash64_u64(h, stdlib_ctime);
        h = ny_hash64_u64(h, stdlib_ino);
        h = ny_hash64_u64(h, stdlib_dev);
      }
    }
  }
  uint64_t compiler_source_fingerprint = ny_cache_compiler_source_fingerprint();
  uint64_t argv0_mtime = 0;
  uint64_t argv0_mtime_nsec = 0;
  uint64_t argv0_ctime = 0;
  uint64_t argv0_size = 0;
  char *exe_path = ny_get_executable_path();
  const char *compiler_path = exe_path && *exe_path ? exe_path : argv0;
  bool strict_compiler_id = ny_env_enabled("NYTRIX_STDBC_CACHE_STRICT_COMPILER");
  if (strict_compiler_id && compiler_path) {
    struct stat st;
    if (stat(compiler_path, &st) == 0) {
      argv0_mtime = (uint64_t)st.st_mtime;
      argv0_mtime_nsec = ny_stat_mtime_nsec(&st);
      argv0_ctime = (uint64_t)st.st_ctime;
      argv0_size = (uint64_t)st.st_size;
    }
  }
#ifdef LLVM_VERSION_STRING
  h = ny_fnv1a64_cstr(LLVM_VERSION_STRING, h);
#endif
  h = ny_fnv1a64_cstr(VERSION, h);
#ifdef NYTRIX_VERSION_COMMIT
  h = ny_fnv1a64_cstr(NYTRIX_VERSION_COMMIT, h);
#endif
#ifdef NYTRIX_VERSION_DIRTY
  h = ny_hash64_u64(h, (uint64_t)NYTRIX_VERSION_DIRTY);
#endif
  if (debug_symbols)
    h = ny_hash64_u64(h, 0xD3B6D3B6ULL);
  if (ny_env_enabled("NYTRIX_TRACE"))
    h = ny_hash64_u64(h, 0x71726163655ULL);
  h = ny_hash64_u64(h, compiler_source_fingerprint);
  if (strict_compiler_id) {
    h = ny_hash64_u64(h, argv0_mtime);
    h = ny_hash64_u64(h, argv0_mtime_nsec);
    h = ny_hash64_u64(h, argv0_size);
    if (strict_file_id)
      h = ny_hash64_u64(h, argv0_ctime);
  }
#ifdef NYTRIX_BUILD_HASH
  h = ny_hash64_u64(h, (uint64_t)ny_hash_string(NYTRIX_BUILD_HASH));
#endif
  {
    const char *const envs[] = {"NYTRIX_ASSUME_INT",
                                "NYTRIX_FAST_INT_BINOPS",
                                "NYTRIX_FAST_FLOAT_BINOPS",
                                "NYTRIX_FAST_MODE",
                                "NYTRIX_HOST_TRIPLE",
                                "NYTRIX_HOST_CFLAGS",
                                "NYTRIX_ARM_FLOAT_ABI",
                                "NYTRIX_COMPILER_ASSERTS",
                                "NYTRIX_DEBUG_LOCALS",
                                "NYTRIX_DWARF_VERSION",
                                "NYTRIX_DWARF_SPLIT_INLINING",
                                "NYTRIX_DWARF_PROFILE_INFO",
                                "NYTRIX_OPT_PROFILE",
                                "NYTRIX_MONO_TYPES",
                                "NYTRIX_ENABLE_MONOMORPHIZATION",
                                "NYTRIX_DISABLE_MONO_TYPES",
                                "NYTRIX_DISABLE_MONOMORPHIZATION",
                                "NYTRIX_MONO_IMPERATIVE",
                                "NYTRIX_SIMPLE_RAW_INT_CALL_FAST",
                                "NYTRIX_FAST_ALL_PROFILES",
                                "NYTRIX_PROVEN_RAW_INT_EXPR_FAST",
                                "NYTRIX_RAW_INT_EXPR_FAST",
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
    h = ny_hash_envv(h, envs, sizeof(envs) / sizeof(envs[0]));
  }
  h = ny_hash64_u64(h, (uint64_t)NY_STD_BC_CACHE_VERSION);

  char path[1024];
  snprintf(path, sizeof(path), "%s/ny_std_%016llx.bc", dir, (unsigned long long)h);
  if (ny_trace_cache_enabled()) {
    fprintf(stderr,
            "[cache] std-bc path=%s std_mode=%d uses=%zu std_latest=%lu "
            "fingerprint=%016llx stdlib=%s stdlib_stat=%llu/%llu/%llu/%llu/%llu/%llu "
            "compiler=%s compiler_source=%016llx compiler_stat=%llu/%llu/%llu/%llu "
            "strict_compiler=%d strict_file_id=%d debug=%d\n",
            path, std_mode, use_count, std_latest_mtime,
            (unsigned long long)source_fingerprint,
            stdlib_path ? stdlib_path : "<null>",
            (unsigned long long)stdlib_mtime,
            (unsigned long long)stdlib_mtime_nsec,
            (unsigned long long)stdlib_ctime,
            (unsigned long long)stdlib_ino,
            (unsigned long long)stdlib_dev,
            (unsigned long long)stdlib_size,
            compiler_path ? compiler_path : "<null>",
            (unsigned long long)compiler_source_fingerprint,
            (unsigned long long)argv0_mtime,
            (unsigned long long)argv0_mtime_nsec,
            (unsigned long long)argv0_ctime,
            (unsigned long long)argv0_size, strict_compiler_id ? 1 : 0,
            strict_file_id ? 1 : 0, debug_symbols ? 1 : 0);
  }
  return strdup(path);
}

static bool ny_jit_cache_verify_enabled(void) { return ny_env_enabled("NYTRIX_JIT_CACHE_VERIFY"); }

bool ny_jit_cache_load(const char *cache_path, LLVMContextRef ctx, LLVMModuleRef *out_module) {
  if (!cache_path || !ctx || !out_module)
    return false;
  LLVMMemoryBufferRef buf = NULL;
  if (ny_access(cache_path, R_OK) != 0)
    return false;
  if (LLVMCreateMemoryBufferWithContentsOfFile(cache_path, &buf, NULL) != 0) {
    remove(cache_path);
    return false;
  }
  bool parsed = false;
  bool buf_owned_by_module = false;
  if (ny_cache_path_is_ir(cache_path)) {
    char *err_msg = NULL;
    parsed = LLVMParseIRInContext(ctx, buf, out_module, &err_msg) == 0 && *out_module != NULL;
    if (err_msg)
      LLVMDisposeMessage(err_msg);
    buf_owned_by_module = parsed;
  } else {
    parsed = LLVMParseBitcodeInContext2(ctx, buf, out_module) == 0 && *out_module != NULL;
  }
  if (!parsed || !buf_owned_by_module) {
    LLVMDisposeMemoryBuffer(buf);
  }
  if (!parsed) {
    remove(cache_path);
    return false;
  }
  if (ny_trace_cache_enabled()) {
    fprintf(stderr, "[cache] jit hit: %s\n", cache_path);
  }
  if (ny_jit_cache_verify_enabled()) {
    char *vmsg = NULL;
    if (LLVMVerifyModule(*out_module, LLVMReturnStatusAction, &vmsg) != 0) {
      if (vmsg)
        LLVMDisposeMessage(vmsg);
      LLVMDisposeModule(*out_module);
      *out_module = NULL;
      remove(cache_path);
      return false;
    }
  }
  return true;
}

bool ny_jit_cache_save(const char *cache_path, LLVMModuleRef module) {
  if (!cache_path || !module)
    return false;

  LLVMModuleRef save_mod = LLVMCloneModule(module);
  if (!save_mod)
    return false;

  if (ny_cache_path_is_ir(cache_path)) {
    char *ir = LLVMPrintModuleToString(save_mod);
    LLVMDisposeModule(save_mod);
    if (!ir)
      return false;
    bool ok = ny_write_text_file_atomic(cache_path, ir, strlen(ir));
    LLVMDisposeMessage(ir);
    return ok;
  }

  /* Use PID-unique temp file to avoid race with parallel processes */
  char tmp_path[1024];
  pid_t pid = getpid();
  snprintf(tmp_path, sizeof(tmp_path), "%s.%d.tmp", cache_path, (int)pid);
  if (LLVMWriteBitcodeToFile(save_mod, tmp_path) == 0) {
    if (rename(tmp_path, cache_path) == 0) {
      LLVMDisposeModule(save_mod);
      return true;
    }
    remove(tmp_path);
  }

  char *ir = LLVMPrintModuleToString(save_mod);
  LLVMDisposeModule(save_mod);
  if (!ir)
    return false;
  LLVMContextRef tmp_ctx = LLVMContextCreate();
  if (!tmp_ctx) {
    LLVMDisposeMessage(ir);
    return false;
  }
  LLVMModuleRef tmp_mod = NULL;
  char *err_msg = NULL;
  LLVMMemoryBufferRef ir_buf =
      LLVMCreateMemoryBufferWithMemoryRangeCopy(ir, strlen(ir), "ny_cache_tmp");
  LLVMDisposeMessage(ir);
  bool ok = false;
  if (ir_buf) {
    if (LLVMParseIRInContext(tmp_ctx, ir_buf, &tmp_mod, &err_msg) == 0) {
      snprintf(tmp_path, sizeof(tmp_path), "%s.%d.tmp", cache_path, (int)pid);
      if (LLVMWriteBitcodeToFile(tmp_mod, tmp_path) == 0) {
        if (rename(tmp_path, cache_path) == 0)
          ok = true;
        else
          remove(tmp_path);
      }
      LLVMDisposeModule(tmp_mod);
    } else {
      if (err_msg)
        LLVMDisposeMessage(err_msg);
      LLVMDisposeMemoryBuffer(ir_buf);
    }
  }
  LLVMContextDispose(tmp_ctx);
  return ok;
}

bool ny_jit_cache_load_ir(const char *cache_path, LLVMContextRef ctx, LLVMModuleRef *out_module) {
  return ny_jit_cache_load(cache_path, ctx, out_module);
}

bool ny_jit_cache_save_ir(const char *cache_path, LLVMModuleRef module) {
  return ny_jit_cache_save(cache_path, module);
}

#ifndef _WIN32
static bool ny_jit_cache_use_native(void) {
  /* Native .so JIT cache is fast, but it bypasses parts of the normal MCJIT
     setup path and has proven unsafe for large UI/native-library scripts.
     Keep the stable bitcode cache on by default and require an explicit opt-in
     for the native shared-object cache while that path is hardened. */
  return ny_env_enabled("NYTRIX_JIT_NATIVE_CACHE");
}

char *ny_jit_native_cache_path(const char *bc_path) {
  if (!bc_path)
    return NULL;
  size_t len = strlen(bc_path);
  char *path = malloc(len + 4);
  if (!path)
    return NULL;
  memcpy(path, bc_path, len);
  const char *ext = strrchr(bc_path, '.');
  if (ext) {
    size_t base = (size_t)(ext - bc_path);
    snprintf(path + base, len + 4 - base, ".so");
  } else {
    snprintf(path + len, 4, ".so");
  }
  return path;
}

static void ny_jit_native_load_libs(const char *so_path) {
  char libs_path[1024];
  snprintf(libs_path, sizeof(libs_path), "%s.libs", so_path);
  FILE *f = fopen(libs_path, "r");
  if (!f)
    return;
  extern void *ny_jit_load_library(const char *path);
  char line[256];
  while (fgets(line, sizeof(line), f)) {
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
      line[--len] = '\0';
    if (len > 0)
      ny_jit_load_library(line);
  }
  fclose(f);
}

bool ny_jit_native_cache_load(const char *so_path, void **out_handle, void (**out_entry)(void)) {
  if (!so_path || !out_handle || !out_entry)
    return false;
  if (ny_access(so_path, R_OK) != 0)
    return false;
  dlopen(NULL, RTLD_NOW | RTLD_GLOBAL);
  ny_jit_native_load_libs(so_path);
  void *h = dlopen(so_path, RTLD_LAZY | RTLD_GLOBAL);
  if (!h) {
    if (ny_env_enabled("NYTRIX_VERBOSE") || ny_env_enabled("NYTRIX_DEBUG"))
      fprintf(stderr, "[cache] native load failed: %s\n", dlerror());
    return false;
  }
  void (*entry)(void) = (void (*)(void))dlsym(h, "_ny_top_entry");
  if (!entry) {
    dlclose(h);
    return false;
  }
  *out_handle = h;
  *out_entry = entry;
  return true;
}

bool ny_jit_native_cache_save(const char *so_path, LLVMModuleRef module, int opt_level,
                              const char *const *link_libs, size_t link_count) {
  if (!so_path || !module)
    return false;
  char obj_path[1024];
  snprintf(obj_path, sizeof(obj_path), "%s.o", so_path);
  extern bool ny_llvm_emit_object(LLVMModuleRef module, const char *path, int opt_level);
  LLVMModuleRef emit_mod = LLVMCloneModule(module);
  if (!emit_mod)
    return false;
  LLVMStripModuleDebugInfo(emit_mod);
  bool ok = ny_llvm_emit_object(emit_mod, obj_path, opt_level);
  LLVMDisposeModule(emit_mod);
  if (!ok) {
    remove(obj_path);
    return false;
  }
  char tmp_so[1024];
  snprintf(tmp_so, sizeof(tmp_so), "%s.tmp", so_path);
  char cmd[4096];
  if (ny_env_enabled("NYTRIX_VERBOSE") || ny_env_enabled("NYTRIX_DEBUG"))
    snprintf(cmd, sizeof(cmd), "ld -shared --allow-shlib-undefined -o %s %s", tmp_so, obj_path);
  else
    snprintf(cmd, sizeof(cmd), "ld -shared --allow-shlib-undefined -o %s %s 2>/dev/null", tmp_so,
             obj_path);
  int rc = system(cmd);
  remove(obj_path);
  if (rc != 0) {
    remove(tmp_so);
    return false;
  }
  if (rename(tmp_so, so_path) != 0) {
    remove(tmp_so);
    return false;
  }
  if (link_libs && link_count > 0) {
    char libs_path[1024];
    snprintf(libs_path, sizeof(libs_path), "%s.libs", so_path);
    FILE *f = fopen(libs_path, "w");
    if (f) {
      for (size_t i = 0; i < link_count; i++) {
        if (link_libs[i])
          fprintf(f, "%s\n", link_libs[i]);
      }
      fclose(f);
    }
  }
  return true;
}

bool ny_jit_native_cache_enabled(void) { return ny_jit_cache_use_native(); }
#endif
