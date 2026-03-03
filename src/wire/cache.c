#include "wire/cache.h"
#include "base/common.h"
#include "base/util.h"
#include <llvm-c/Analysis.h>
#include <llvm-c/BitReader.h>
#include <llvm-c/BitWriter.h>
#include <llvm-c/Core.h>
#include <llvm-c/IRReader.h>
#include <llvm/Config/llvm-config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define mkdir(p, m) _mkdir(p)
#else
#include <unistd.h>
#endif

static unsigned long ny_hash_string(const char *str) {
  unsigned long hash = 5381;
  int c;
  while ((c = *str++))
    hash = ((hash << 5) + hash) + c;
  return hash;
}

static bool ny_write_text_file_atomic(const char *path, const char *content,
                                      size_t len) {
  if (!path || !*path || !content)
    return false;
  char tmp_path[1024];
  snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
  FILE *f = fopen(tmp_path, "wb");
  if (!f)
    return false;
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

static bool ny_cache_path_is_ir(const char *cache_path) {
  if (!cache_path)
    return false;
  const char *ext = strrchr(cache_path, '.');
  if (!ext)
    return false;
  return strcmp(ext, ".ll") == 0 || strcmp(ext, ".ir") == 0 ||
         strcmp(ext, ".llvm") == 0;
}

static bool ny_jit_cache_use_ir(void) {
  const char *env = getenv("NYTRIX_JIT_CACHE_FORMAT");
  if (!env || !*env) // Fallback to bitcode (bc) for speed
    return false;
  return strcmp(env, "ir") == 0 || strcmp(env, "ll") == 0 ||
         strcmp(env, "text") == 0 || strcmp(env, "llvm") == 0;
}

static char *ny_get_cache_dir(void) {
  const char *home = getenv("HOME");
  if (!home)
    home = getenv("USERPROFILE");
  if (!home)
    return NULL;
  static char path[1024];
  snprintf(path, sizeof(path), "%s/.cache/nytrix/jit", home);
  return path;
}

static void ny_ensure_dir_recursive(const char *path) {
  char tmp[1024];
  snprintf(tmp, sizeof(tmp), "%s", path);
  size_t len = strlen(tmp);
  if (tmp[len - 1] == '/')
    tmp[len - 1] = 0;
  for (char *p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = 0;
      ny_ensure_dir(tmp);
      *p = '/';
    }
  }
  ny_ensure_dir(tmp);
}

bool ny_jit_cache_enabled(void) {
  return ny_env_enabled_default_on("NYTRIX_JIT_CACHE");
}

enum { NY_JIT_CACHE_VERSION = 6 };

char *ny_jit_cache_path(const char *source, const char *stdlib_path,
                        unsigned long std_src_hash, int opt_level, int opt_dce,
                        int opt_internalize, bool debug_symbols,
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
    } else {
      struct stat st;
      if (stat(stdlib_path, &st) == 0) {
        std_hash = (unsigned long)st.st_mtime;
      }
    }
    std_hash ^= ny_hash_string(stdlib_path);
  }
  if (std_latest_mtime) {
    std_hash ^= std_latest_mtime;
  }
#ifdef LLVM_VERSION_STRING
  std_hash ^= ny_hash_string(LLVM_VERSION_STRING);
#endif
  std_hash ^= (unsigned long)opt_level;
  std_hash ^= (unsigned long)opt_dce;
  std_hash ^= (unsigned long)opt_internalize;
  if (debug_symbols)
    std_hash ^= 0xDEADBEEF;
  std_hash ^= (unsigned long)NY_JIT_CACHE_VERSION;
  static char path[1024];
  const char *ext = ny_jit_cache_use_ir() ? "ll" : "bc";
  snprintf(path, sizeof(path), "%s/%lx_%lx.%s", dir, src_hash, std_hash, ext);
  return strdup(path);
}

static bool ny_jit_cache_verify_enabled(void) {
  return ny_env_enabled("NYTRIX_JIT_CACHE_VERIFY");
}

bool ny_jit_cache_load(const char *cache_path, LLVMContextRef ctx,
                       LLVMModuleRef *out_module) {
  if (!cache_path || !ctx || !out_module)
    return false;
  LLVMMemoryBufferRef buf = NULL;
  if (access(cache_path, R_OK) != 0)
    return false;
  if (LLVMCreateMemoryBufferWithContentsOfFile(cache_path, &buf, NULL) != 0) {
    remove(cache_path);
    return false;
  }
  bool parsed = false;
  bool buf_owned_by_module = false;
  if (ny_cache_path_is_ir(cache_path)) {
    parsed = LLVMParseIRInContext(ctx, buf, out_module, NULL) == 0;
    buf_owned_by_module = parsed;
  } else {
    parsed = LLVMParseBitcodeInContext2(ctx, buf, out_module) == 0;
  }
  if (!parsed || !buf_owned_by_module) {
    LLVMDisposeMemoryBuffer(buf);
  }
  if (!parsed) {
    remove(cache_path);
    return false;
  }
  /* Validate cached module only when explicitly requested */
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
  if (ny_cache_path_is_ir(cache_path)) {
    char *ir = LLVMPrintModuleToString(module);
    if (!ir)
      return false;
    bool ok = ny_write_text_file_atomic(cache_path, ir, strlen(ir));
    LLVMDisposeMessage(ir);
    return ok;
  }
  char tmp_path[1024];
  snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", cache_path);
  if (LLVMWriteBitcodeToFile(module, tmp_path) != 0)
    return false;
  if (rename(tmp_path, cache_path) != 0) {
    remove(tmp_path);
    return false;
  }
  return true;
}

bool ny_jit_cache_load_ir(const char *cache_path, LLVMContextRef ctx,
                          LLVMModuleRef *out_module) {
  return ny_jit_cache_load(cache_path, ctx, out_module);
}

bool ny_jit_cache_save_ir(const char *cache_path, LLVMModuleRef module) {
  return ny_jit_cache_save(cache_path, module);
}
