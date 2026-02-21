#include "wire/cache.h"
#include "base/common.h"
#include "base/util.h"
#include <llvm-c/BitReader.h>
#include <llvm-c/BitWriter.h>
#include <llvm-c/Core.h>
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

// Simple hash (DJB2) for cache keys
static unsigned long ny_hash_string(const char *str) {
  unsigned long hash = 5381;
  int c;
  while ((c = *str++))
    hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
  return hash;
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
  const char *env = getenv("NYTRIX_JIT_CACHE");
  // Default to enabled unless explicitly disabled (off/0/false)
  if (env && (strcmp(env, "0") == 0 || strcmp(env, "off") == 0 ||
              strcmp(env, "false") == 0)) {
    return false;
  }
  return true;
}

char *ny_jit_cache_path(const char *source, const char *stdlib_path) {
  if (!source)
    return NULL;

  char *dir = ny_get_cache_dir();
  if (!dir)
    return NULL;

  ny_ensure_dir_recursive(dir);

  // Hash source content + stdlib mtime/path
  unsigned long src_hash = ny_hash_string(source);
  unsigned long std_hash = 0;

  if (stdlib_path) {
    struct stat st;
    if (stat(stdlib_path, &st) == 0) {
      std_hash = (unsigned long)st.st_mtime;
    }
    std_hash ^= ny_hash_string(stdlib_path);
  }

  static char path[1024];
  snprintf(path, sizeof(path), "%s/%lx_%lx.bc", dir, src_hash, std_hash);
  return strdup(path);
}

bool ny_jit_cache_load(const char *cache_path, LLVMContextRef ctx,
                       LLVMModuleRef *out_module) {
  if (!cache_path || !ctx || !out_module)
    return false;

  LLVMMemoryBufferRef buf = NULL;
  char *msg = NULL;

  if (LLVMCreateMemoryBufferWithContentsOfFile(cache_path, &buf, &msg) != 0) {
    if (msg)
      LLVMDisposeMessage(msg);
    return false;
  }

  if (LLVMParseBitcodeInContext(ctx, buf, out_module, &msg) != 0) {
    if (msg)
      LLVMDisposeMessage(msg);
    LLVMDisposeMemoryBuffer(buf);
    return false;
  }

  LLVMDisposeMemoryBuffer(buf);
  return true;
}

bool ny_jit_cache_save(const char *cache_path, LLVMModuleRef module) {
  if (!cache_path || !module)
    return false;
  return LLVMWriteBitcodeToFile(module, cache_path) == 0;
}
