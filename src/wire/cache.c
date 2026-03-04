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
#include <dlfcn.h>
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

bool ny_jit_cache_enabled(void) {
  return ny_env_enabled_default_on("NYTRIX_JIT_CACHE");
}

enum { NY_JIT_CACHE_VERSION = 14 };

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
  if (ny_env_enabled("NYTRIX_FAST_MODE"))
    std_hash ^= 0xCAFEBABE;
  if (ny_env_enabled("NYTRIX_FAST_INT_BINOPS"))
    std_hash ^= 0x12345678;
  if (ny_env_enabled("NYTRIX_FAST_FLOAT_BINOPS"))
    std_hash ^= 0x87654321;
  if (ny_env_enabled("NYTRIX_ASSUME_INT"))
    std_hash ^= 0xA5A5A5A5;

  unsigned long compiler_hash = 0;
  char *exe_path = ny_get_executable_path();
  if (exe_path) {
    struct stat st;
    if (stat(exe_path, &st) == 0) {
      compiler_hash = (unsigned long)st.st_mtime;
#ifdef NYTRIX_BUILD_HASH
      compiler_hash ^= (unsigned long)ny_hash_string(NYTRIX_BUILD_HASH);
#endif
    }
  }

  std_hash ^= compiler_hash;
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
  if (LLVMWriteBitcodeToFile(module, tmp_path) == 0) {
    if (rename(tmp_path, cache_path) == 0)
      return true;
    remove(tmp_path);
  }
  /* Direct write failed — try IR round-trip to work around LLVM bitcode
   * writer inconsistencies ('Invalid record' errors). */
  char *ir = LLVMPrintModuleToString(module);
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

bool ny_jit_cache_load_ir(const char *cache_path, LLVMContextRef ctx,
                          LLVMModuleRef *out_module) {
  return ny_jit_cache_load(cache_path, ctx, out_module);
}

bool ny_jit_cache_save_ir(const char *cache_path, LLVMModuleRef module) {
  return ny_jit_cache_save(cache_path, module);
}

#ifndef _WIN32
static bool ny_jit_cache_use_native(void) {
  return ny_env_enabled_default_on("NYTRIX_JIT_NATIVE_CACHE");
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

bool ny_jit_native_cache_load(const char *so_path, void **out_handle,
                              void (**out_entry)(void)) {
  if (!so_path || !out_handle || !out_entry)
    return false;
  if (access(so_path, R_OK) != 0)
    return false;
  dlopen(NULL, RTLD_NOW | RTLD_GLOBAL);
  ny_jit_native_load_libs(so_path);
  void *h = dlopen(so_path, RTLD_LAZY | RTLD_GLOBAL);
  if (!h) {
    if (getenv("NYTRIX_VERBOSE") || getenv("NYTRIX_DEBUG"))
      fprintf(stderr, "[cache] native load failed: %s\n", dlerror());
    return false;
  }
  void (*entry)(void) = (void (*)(void))dlsym(h, "__script_top");
  if (!entry) {
    dlclose(h);
    return false;
  }
  *out_handle = h;
  *out_entry = entry;
  return true;
}

bool ny_jit_native_cache_save(const char *so_path, LLVMModuleRef module,
                              int opt_level, const char *const *link_libs,
                              size_t link_count) {
  if (!so_path || !module)
    return false;
  char obj_path[1024];
  snprintf(obj_path, sizeof(obj_path), "%s.o", so_path);
  extern bool ny_llvm_emit_object(LLVMModuleRef module, const char *path,
                                  int opt_level);
  if (!ny_llvm_emit_object(module, obj_path, opt_level)) {
    remove(obj_path);
    return false;
  }
  char tmp_so[1024];
  snprintf(tmp_so, sizeof(tmp_so), "%s.tmp", so_path);
  char cmd[4096];
  snprintf(cmd, sizeof(cmd),
           "ld -shared --allow-shlib-undefined -o %s %s 2>/dev/null", tmp_so,
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
