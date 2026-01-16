#include "base/common.h"
#include "rt/shared.h"
#include <errno.h>
#include <setjmp.h>

// Type Predicates
int64_t __is_int(int64_t v) { return is_int(v) ? 2 : 4; }
int64_t __is_ptr(int64_t v) { return is_ptr(v) ? 2 : 4; }
int64_t __is_flt(int64_t v) { return is_v_flt(v) ? 2 : 4; }
int64_t __is_str(int64_t v) { return is_v_str(v) ? 2 : 4; }

// Globals & Panic
static int64_t g_globals_ptr = 0;
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
    longjmp(*(g_panic_env_stack.data[g_panic_env_stack.len - 1]), 1);
  }
  fprintf(stderr, "Panic: %s\n", (char *)(uintptr_t)msg_ptr);
  exit(1);
  return 0;
}

// Args & Env
static int g_argc = 0;
static int g_envc = 0;
static char **g_argv = NULL;
static char **g_envp = NULL;

int64_t __set_args(int64_t argc, int64_t argv_ptr, int64_t envp_ptr) {
  g_argc = (int)argc;
  g_argv = calloc(g_argc + 1, sizeof(char *));
  char **old_argv = (char **)argv_ptr;
  for (int i = 0; i < g_argc; i++) {
    if (old_argv[i]) {
      size_t len = strlen(old_argv[i]);
      int64_t p = __malloc((int64_t)((len + 1) << 1 | 1));
      *(int64_t *)((char *)(uintptr_t)p - 8) = TAG_STR;
      *(int64_t *)((char *)(uintptr_t)p - 16) = ((int64_t)len << 1) | 1;
      __copy_mem((void *)(uintptr_t)p, old_argv[i], len + 1);
      g_argv[i] = (char *)(uintptr_t)p;
    } else {
      g_argv[i] = NULL;
    }
  }
  g_argv[g_argc] = NULL;
  char **old_envp = (char **)envp_ptr;
  int env_count = 0;
  if (old_envp) {
    while (old_envp[env_count])
      env_count++;
  }
  g_envc = env_count;
  g_envp = calloc(env_count + 1, sizeof(char *));
  for (int i = 0; i < env_count; i++) {
    size_t len = strlen(old_envp[i]);
    int64_t p = __malloc((int64_t)((len + 1) << 1 | 1));
    *(int64_t *)((char *)(uintptr_t)p - 8) = TAG_STR;
    *(int64_t *)((char *)(uintptr_t)p - 16) = ((int64_t)len << 1) | 1;
    __copy_mem((void *)(uintptr_t)p, old_envp[i], len + 1);
    g_envp[i] = (char *)(uintptr_t)p;
  }
  g_envp[env_count] = NULL;
  return 0;
}

void __cleanup_args(void) {
  if (g_argv) {
    free(g_argv);
    g_argv = NULL;
  }
  if (g_envp) {
    free(g_envp);
    g_envp = NULL;
  }
  g_argc = 0;
  g_envc = 0;
}

int64_t __argc(void) { return (int64_t)((g_argc << 1) | 1); }
int64_t __envc(void) { return (int64_t)((g_envc << 1) | 1); }
int64_t __envp(void) { return (int64_t)g_envp; }

int64_t __argv(int64_t i) {
  if (!is_int(i))
    return 0;
  int idx = (int)(i >> 1);
  if (idx < 0 || idx >= g_argc)
    return 0;
  const char *s = g_argv[idx];
  size_t len = strlen(s);
  int64_t res = __malloc(((int64_t)(len + 1) << 1) | 1);
  *(int64_t *)((char *)(uintptr_t)res - 8) = TAG_STR;
  *(int64_t *)((char *)(uintptr_t)res - 16) = ((int64_t)len << 1) | 1;
  __copy_mem((void *)(uintptr_t)res, s, len + 1);
  return res;
}

// Misc
int64_t __errno(void) { return errno; }

int64_t __kwarg(int64_t k, int64_t v) {
  int64_t res = __malloc(16);
  if (!res)
    return 0;
  *(int64_t *)(uintptr_t)((char *)res - 8) = 209; // Tag 104
  ((int64_t *)(uintptr_t)res)[0] = k;
  ((int64_t *)(uintptr_t)res)[1] = v;
  return res;
}
