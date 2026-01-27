#include "base/common.h"
#include "rt/shared.h"
#include <errno.h>
#include <setjmp.h>

// Globals & Panic
int64_t g_globals_ptr = 1; // tagged (0)
typedef VEC(jmp_buf *) jmp_buf_vec;
static jmp_buf_vec g_panic_env_stack = {0};
static int64_t g_panic_value = 0;

int64_t __globals(void) { return g_globals_ptr; }
int64_t __set_globals(int64_t p) {
  g_globals_ptr = p;
  return p;
}

int64_t __set_panic_env(int64_t env_ptr) {
  vec_push(&g_panic_env_stack, (jmp_buf *)(uintptr_t)env_ptr);
  return 0;
}

int64_t __clear_panic_env(void) {
  if (g_panic_env_stack.len > 0) {
    g_panic_env_stack.len--;
  }
  return 0;
}

int64_t __jmpbuf_size(void) { return (int64_t)sizeof(jmp_buf); }
int64_t __get_panic_val(void) { return g_panic_value; }

int64_t __panic(int64_t msg_ptr) {
  if (g_panic_env_stack.len > 0) {
    g_panic_value = msg_ptr;
    jmp_buf *env = g_panic_env_stack.data[g_panic_env_stack.len - 1];
    longjmp(*env, 1);
  }
  // Make panic function robust
  if (is_int(msg_ptr)) {
    fprintf(stderr, "Panic: <integer value> %ld (raw)\n", msg_ptr);
  } else if (is_v_str(msg_ptr)) { // This implies is_heap_ptr(msg_ptr)
    const char *msg = (const char *)(uintptr_t)msg_ptr;
    size_t msg_len =
        __get_heap_size(msg_ptr); // We know it's a valid string heap object
    fprintf(stderr, "Panic: %.*s\n", (int)msg_len, msg);
  } else {
    // Fallback for non-int, non-string messages (e.g., potentially bad
    // pointers)
    fprintf(stderr, "Panic: <unknown type> %lx (raw)\n", msg_ptr);
  }
  exit(1);
}

// Args & Env
int64_t __argc_val = 1; // tagged 0
int64_t __envc_val = 1; // tagged 0
char **__argv_ptr = NULL;
char **__envp_ptr = NULL;

int64_t __set_args(int64_t argc, int64_t argv_ptr, int64_t envp_ptr) {
  __argc_val = (argc << 1) | 1;
  __argv_ptr = calloc(argc + 1, sizeof(char *));
  if (!__argv_ptr)
    return -1;
  char **old_argv = (char **)argv_ptr;
  for (int i = 0; i < argc; i++) {
    if (old_argv[i]) {
      size_t len = strlen(old_argv[i]);
      int64_t p = __malloc((int64_t)((len + 1) << 1 | 1));
      if (!p)
        return -1;
      *(int64_t *)((char *)(uintptr_t)p - 8) = TAG_STR;
      *(int64_t *)((char *)(uintptr_t)p - 16) = ((int64_t)len << 1) | 1;
      __copy_mem((void *)(uintptr_t)p, old_argv[i], len + 1);
      __argv_ptr[i] = (char *)(uintptr_t)p;
    } else {
      __argv_ptr[i] = NULL;
    }
  }
  char **old_envp = (char **)envp_ptr;
  int env_count = 0;
  if (old_envp) {
    while (old_envp[env_count])
      env_count++;
  }
  __envc_val = (env_count << 1) | 1;
  __envp_ptr = calloc(env_count + 1, sizeof(char *));
  if (!__envp_ptr)
    return -1;
  for (int i = 0; i < env_count; i++) {
    size_t len = strlen(old_envp[i]);
    int64_t p = __malloc((int64_t)((len + 1) << 1 | 1));
    if (!p)
      return -1;
    *(int64_t *)((char *)(uintptr_t)p - 8) = TAG_STR;
    *(int64_t *)((char *)(uintptr_t)p - 16) = ((int64_t)len << 1) | 1;
    __copy_mem((void *)(uintptr_t)p, old_envp[i], len + 1);
    __envp_ptr[i] = (char *)(uintptr_t)p;
  }
  return 0;
}

void __cleanup_args(void) {
  if (__argv_ptr) {
    free(__argv_ptr);
    __argv_ptr = NULL;
  }
  if (__envp_ptr) {
    free(__envp_ptr);
    __envp_ptr = NULL;
  }
  __argc_val = 1;
  __envc_val = 1;
}

int64_t __argc(void) { return __argc_val; }
int64_t __envc(void) { return __envc_val; }
int64_t __envp(void) { return (int64_t)__envp_ptr; }

int64_t __argv(int64_t i) {
  if (!is_int(i))
    return 0;
  int idx = (int)(i >> 1);
  if (idx < 0 || idx >= (__argc_val >> 1))
    return 0;
  const char *s = __argv_ptr[idx];
  size_t len = strlen(s);
  int64_t res = __malloc(((int64_t)(len + 1) << 1) | 1);
  *(int64_t *)((char *)(uintptr_t)res - 8) = TAG_STR;
  *(int64_t *)((char *)(uintptr_t)res - 16) = ((int64_t)len << 1) | 1;
  __copy_mem((void *)(uintptr_t)res, s, len + 1);
  return res;
}

// Misc
int64_t __errno_val = 1;
int64_t __errno(void) { return (int64_t)((errno << 1) | 1); }

void __copy_mem(void *dst, const void *src, size_t n) {
  char *d = (char *)dst;
  const char *s = (const char *)src;
  for (size_t i = 0; i < n; i++) {
    d[i] = s[i];
  }
}
