#include "rt/shared.h"
#include <pthread.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>

// Syscall (inline asm on x86_64 for zero overhead)
#ifdef __x86_64__
int64_t __syscall(int64_t n, int64_t a, int64_t b, int64_t c, int64_t d,
                  int64_t e, int64_t f) {
  long rn = (n & 1) ? (n >> 1) : n;
  long ra = a;
  long rb = b;
  long rc = c;
  long rd = (d & 1) ? (d >> 1) : d;
  long re = (e & 1) ? (e >> 1) : e;
  long rf = (f & 1) ? (f >> 1) : f;
  if (rn != 59) {
    ra = (a & 1) ? (a >> 1) : a;
    rb = (b & 1) ? (b >> 1) : b;
    rc = (c & 1) ? (c >> 1) : c;
  }
  register long _num __asm__("rax") = rn;
  register long _arg1 __asm__("rdi") = ra;
  register long _arg2 __asm__("rsi") = rb;
  register long _arg3 __asm__("rdx") = rc;
  register long _arg4 __asm__("r10") = rd;
  register long _arg5 __asm__("r8") = re;
  register long _arg6 __asm__("r9") = rf;
  __asm__ __volatile__("syscall\n"
                       : "+r"(_num)
                       : "r"(_arg1), "r"(_arg2), "r"(_arg3), "r"(_arg4),
                         "r"(_arg5), "r"(_arg6)
                       : "rcx", "r11", "memory");
  return (int64_t)((_num << 1) | 1);
}
#else
int64_t __syscall(int64_t n, int64_t a, int64_t b, int64_t c, int64_t d,
                  int64_t e, int64_t f) {
  int64_t raw_n = (n & 1) ? (n >> 1) : n;
  int64_t raw_a = a;
  int64_t raw_b = b;
  int64_t raw_c = c;
  int64_t raw_d = (d & 1) ? (d >> 1) : d;
  int64_t raw_e = (e & 1) ? (e >> 1) : e;
  int64_t raw_f = (f & 1) ? (f >> 1) : f;
  if (raw_n != 59) {
    raw_a = (a & 1) ? (a >> 1) : a;
    raw_b = (b & 1) ? (b >> 1) : b;
    raw_c = (c & 1) ? (c >> 1) : c;
  }
  int64_t res = syscall(raw_n, raw_a, raw_b, raw_c, raw_d, raw_e, raw_f);
  return (res << 1) | 1;
}
#endif

int64_t __sys_read_off(int64_t fd, int64_t buf, int64_t len, int64_t off) {
  if (is_int(fd))
    fd >>= 1;
  if (is_int(len))
    len >>= 1;
  if (is_int(off))
    off >>= 1;
  if (!__check_oob("sys_read", buf, off, (size_t)len))
    return -1LL;
  ssize_t r =
      read((int)fd, (char *)((intptr_t)buf + (intptr_t)off), (size_t)len);
  return (int64_t)((r << 1) | 1);
}

int64_t __sys_write_off(int64_t fd, int64_t buf, int64_t len, int64_t off) {
  if (is_int(fd))
    fd >>= 1;
  if (is_int(len))
    len >>= 1;
  if (is_int(off))
    off >>= 1;
  if (!__check_oob("sys_write", buf, off, (size_t)len))
    return -1LL;
  ssize_t r =
      write((int)fd, (char *)((intptr_t)buf + (intptr_t)off), (size_t)len);
  return (int64_t)((r << 1) | 1);
}

int64_t __execve(int64_t path, int64_t argv, int64_t envp) {
  long rpath = (path & 1) ? (path >> 1) : path;
  long rargv = (argv & 1) ? (argv >> 1) : argv;
  long renvp = (envp & 1) ? (envp >> 1) : envp;
  long res = syscall(SYS_execve, (const char *)rpath, (char *const *)rargv,
                     (char *const *)renvp);
  return (int64_t)((res << 1) | 1);
}

// Threads
typedef struct __thread_arg {
  int64_t fn;
  int64_t arg;
} __thread_arg;

static void *__thread_trampoline(void *p) {
  __thread_arg *ta = (__thread_arg *)p;
  int64_t fn = ta->fn;
  int64_t arg = ta->arg;
  free(ta);
  int64_t (*f)(int64_t) = (void *)(uintptr_t)__mask_ptr(fn);
  int64_t res = f(arg);
  return (void *)(uintptr_t)res;
}

int64_t __thread_spawn(int64_t fn, int64_t arg) {
  pthread_t tid;
  __thread_arg *ta = malloc(sizeof(__thread_arg));
  if (!ta)
    return -1;
  ta->fn = fn;
  ta->arg = arg;
  int r = pthread_create(&tid, NULL, __thread_trampoline, ta);
  if (r != 0) {
    free(ta);
    return -r;
  }
  return (int64_t)tid;
}

int64_t __thread_join(int64_t tid) {
  void *ret = NULL;
  int r = pthread_join((pthread_t)tid, &ret);
  if (r != 0)
    return -r;
  return (int64_t)(uintptr_t)ret;
}

int64_t __mutex_new(void) {
  pthread_mutex_t *m = calloc(1, sizeof(pthread_mutex_t));
  if (!m)
    return 0;
  if (pthread_mutex_init(m, NULL) != 0) {
    free(m);
    return 0;
  }
  return (int64_t)(uintptr_t)m;
}

int64_t __mutex_lock64(int64_t m) {
  if (!m)
    return -1;
  return pthread_mutex_lock((pthread_mutex_t *)(uintptr_t)m);
}

int64_t __mutex_unlock64(int64_t m) {
  if (!m)
    return -1;
  return pthread_mutex_unlock((pthread_mutex_t *)(uintptr_t)m);
}

int64_t __mutex_free(int64_t m) {
  if (!m)
    return 0;
  pthread_mutex_destroy((pthread_mutex_t *)(uintptr_t)m);
  free((void *)(uintptr_t)m);
  return 0;
}

// Sockets
int64_t __recv(int64_t sockfd, int64_t buf, int64_t len, int64_t flags) {
  if (is_int(sockfd))
    sockfd >>= 1;
  if (is_int(len))
    len >>= 1;
  if (is_int(flags))
    flags >>= 1;
  if (!buf)
    return -1;
  ssize_t res =
      recv((int)sockfd, (void *)(uintptr_t)buf, (size_t)len, (int)flags);
  return (int64_t)((res << 1) | 1);
}
