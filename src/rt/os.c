#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "rt/shared.h"
#include <fcntl.h>
#include <stdlib.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#include <conio.h>
#include <direct.h>
#include <io.h>
#include <process.h>
#else
#include <dirent.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <spawn.h>
extern char **environ;
#elif !defined(_WIN32)
extern char **environ;
#endif
static int64_t __make_str(const char *s);

int64_t __call0(int64_t f);
int64_t __call1(int64_t f, int64_t a0);
int64_t __call2(int64_t f, int64_t a0, int64_t a1);
int64_t __call3(int64_t f, int64_t a0, int64_t a1, int64_t a2);
int64_t __call4(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3);
int64_t __call5(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                int64_t a4);
int64_t __call6(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                int64_t a4, int64_t a5);
int64_t __call7(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                int64_t a4, int64_t a5, int64_t a6);
int64_t __call8(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                int64_t a4, int64_t a5, int64_t a6, int64_t a7);
int64_t __call9(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                int64_t a4, int64_t a5, int64_t a6, int64_t a7, int64_t a8);
int64_t __call10(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                 int64_t a4, int64_t a5, int64_t a6, int64_t a7, int64_t a8,
                 int64_t a9);
int64_t __call11(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                 int64_t a4, int64_t a5, int64_t a6, int64_t a7, int64_t a8,
                 int64_t a9, int64_t a10);
int64_t __call12(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                 int64_t a4, int64_t a5, int64_t a6, int64_t a7, int64_t a8,
                 int64_t a9, int64_t a10, int64_t a11);
int64_t __call13(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                 int64_t a4, int64_t a5, int64_t a6, int64_t a7, int64_t a8,
                 int64_t a9, int64_t a10, int64_t a11, int64_t a12);
int64_t __call14(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                 int64_t a4, int64_t a5, int64_t a6, int64_t a7, int64_t a8,
                 int64_t a9, int64_t a10, int64_t a11, int64_t a12,
                 int64_t a13);
int64_t __call15(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                 int64_t a4, int64_t a5, int64_t a6, int64_t a7, int64_t a8,
                 int64_t a9, int64_t a10, int64_t a11, int64_t a12, int64_t a13,
                 int64_t a14);

static char **ny_native_argv(intptr_t rargv, bool *needs_free) {
  if (needs_free)
    *needs_free = false;
  if (!rargv)
    return NULL;
  int64_t *src = (int64_t *)(uintptr_t)rargv;
  size_t count = 0;
  while (src[count] != 0 && src[count] != 1)
    count++;
  char **dst = (char **)calloc(count + 1, sizeof(char *));
  if (!dst)
    return NULL;
  for (size_t i = 0; i < count; i++)
    dst[i] = (char *)(uintptr_t)rt_untag_v(src[i]);
  if (needs_free)
    *needs_free = true;
  return dst;
}

#ifndef _WIN32
static char **ny_native_envp(intptr_t renvp, bool *needs_free) {
  if (renvp == 0 || renvp == 1) {
    if (needs_free)
      *needs_free = false;
    return NULL;
  }
  int64_t *src = (int64_t *)(uintptr_t)renvp;
  size_t count = 0;
  while (src[count] != 0 && src[count] != 1)
    count++;
  char **dst = (char **)calloc(count + 1, sizeof(char *));
  if (!dst) {
    if (needs_free)
      *needs_free = false;
    return NULL;
  }
  for (size_t i = 0; i < count; i++)
    dst[i] = (char *)(uintptr_t)rt_untag_v(src[i]);
  dst[count] = NULL;
  if (needs_free)
    *needs_free = true;
  return dst;
}
#endif

// Syscall (inline asm on Linux x86_64 for zero overhead)
#if defined(__linux__) && defined(__x86_64__)
int64_t __syscall(int64_t n, int64_t a, int64_t b, int64_t c, int64_t d,
                  int64_t e, int64_t f) {
  // fprintf(stderr, "DEBUG: syscall(n=%ld, a=%ld, b=%ld, c=%ld)\n", (long)n,
  //         (long)a, (long)b, (long)c);
  long rn = (n & 1) ? (n >> 1) : n;
  /*
  fprintf(stdout, "DEBUG: syscall(n=%ld, a=%lx, b=%lx, c=%lx)\n", (long)rn,
          (long)a, (long)b, (long)c);
  fflush(stdout);
  */
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
  return (int64_t)(((uint64_t)_num << 1) | 1);
}
#else
int64_t __syscall(int64_t n, int64_t a, int64_t b, int64_t c, int64_t d,
                  int64_t e, int64_t f) {
#if defined(_WIN32) || !defined(__linux__)
  (void)n;
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  (void)e;
  (void)f;
  return (int64_t)-1;
#else
  long rn = (n & 1) ? (n >> 1) : n;
  long ra = (a & 1) ? (a >> 1) : a;
  long rb = (b & 1) ? (b >> 1) : b;
  long rc = (c & 1) ? (c >> 1) : c;
  long rd = (d & 1) ? (d >> 1) : d;
  long re = (e & 1) ? (e >> 1) : e;
  long rf = (f & 1) ? (f >> 1) : f;
  long res = syscall(rn, ra, rb, rc, rd, re, rf);
  return (int64_t)(((uint64_t)res << 1) | 1);
#endif
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
#ifdef _WIN32
  ssize_t r = _read((int)fd, (char *)((intptr_t)buf + (intptr_t)off),
                    (unsigned int)len);
  if (r < 0)
    r = -errno;
#else
  ssize_t r =
      read((int)fd, (char *)((intptr_t)buf + (intptr_t)off), (size_t)len);
  if (r < 0)
    r = -errno;
#endif
  return rt_tag_v((int64_t)r);
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
  char *ptr = (char *)((intptr_t)buf + (intptr_t)off);
#ifdef _WIN32
  ssize_t r = _write((int)fd, ptr, (unsigned int)len);
  if (r < 0)
    r = -errno;
#else
  ssize_t r = write((int)fd, ptr, (size_t)len);
  if (r < 0)
    r = -errno;
#endif
  return rt_tag_v((int64_t)r);
}

int64_t __open(int64_t path, int64_t flags, int64_t mode) {
  intptr_t rpath = (intptr_t)((path & 1) ? (path >> 1) : path);
  int64_t rflags = (flags & 1) ? (flags >> 1) : flags;
  int64_t rmode = (mode & 1) ? (mode >> 1) : mode;
#ifdef _WIN32
  int f = 0;
  if (rflags & 1)
    f |= _O_WRONLY;
  else if (rflags & 2)
    f |= _O_RDWR;
  else
    f |= _O_RDONLY;
  if (rflags & 64)
    f |= _O_CREAT;
  if (rflags & 128)
    f |= _O_EXCL;
  if (rflags & 512)
    f |= _O_TRUNC;
  if (rflags & 1024)
    f |= _O_APPEND;
  f |= _O_BINARY;
  int fd = _open((const char *)rpath, f, (int)rmode);
  if (fd < 0)
    return rt_tag_v((int64_t)-errno);
#else
  int fd = open((const char *)rpath, (int)rflags, (mode_t)rmode);
#endif
  return rt_tag_v((int64_t)fd);
}

int64_t __close(int64_t fd) {
  if (is_int(fd))
    fd >>= 1;
#ifdef _WIN32
  int r = _close((int)fd);
  if (r < 0)
    r = -errno;
#else
  int r = close((int)fd);
#endif
  return rt_tag_v((int64_t)r);
}

int64_t __ioctl(int64_t fd, int64_t req, int64_t arg) {
  if (is_int(fd))
    fd >>= 1;
  if (is_int(req))
    req >>= 1;
  if (is_int(arg))
    arg >>= 1;
#ifdef _WIN32
  int r = -1;
#else
  int r = ioctl((int)fd, (unsigned long)req, (void *)(uintptr_t)arg);
#endif
  return rt_tag_v((int64_t)r);
}

int64_t __clock_gettime(int64_t clk, int64_t ts_ptr) {
  if (is_int(clk))
    clk >>= 1;
  if (is_int(ts_ptr))
    ts_ptr >>= 1;
#ifdef _WIN32
  if (!ts_ptr)
    return (int64_t)-1;
  int64_t *t = (int64_t *)(uintptr_t)ts_ptr;
  if (clk == 0) {
    FILETIME ft;
    ULARGE_INTEGER uli;
    GetSystemTimeAsFileTime(&ft);
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    uint64_t t100 = uli.QuadPart;
    const uint64_t EPOCH = 116444736000000000ULL;
    if (t100 < EPOCH)
      t100 = EPOCH;
    t100 -= EPOCH;
    uint64_t sec = t100 / 10000000ULL;
    uint64_t nsec = (t100 % 10000000ULL) * 100ULL;
    t[0] = (int64_t)sec;
    t[1] = (int64_t)nsec;
  } else {
    LARGE_INTEGER freq, counter;
    if (!QueryPerformanceFrequency(&freq) ||
        !QueryPerformanceCounter(&counter) || freq.QuadPart == 0) {
      return (int64_t)-1;
    }
    uint64_t sec = (uint64_t)counter.QuadPart / (uint64_t)freq.QuadPart;
    uint64_t rem = (uint64_t)counter.QuadPart % (uint64_t)freq.QuadPart;
    uint64_t nsec = (uint64_t)((rem * 1000000000ULL) / (uint64_t)freq.QuadPart);
    t[0] = (int64_t)sec;
    t[1] = (int64_t)nsec;
  }
  return (int64_t)((0 << 1) | 1);
#else
  int r = clock_gettime((clockid_t)clk, (struct timespec *)(uintptr_t)ts_ptr);
  return rt_tag_v((int64_t)r);
#endif
}

int64_t __nanosleep(int64_t ts_ptr) {
  if (is_int(ts_ptr))
    ts_ptr >>= 1;
#ifdef _WIN32
  if (!ts_ptr)
    return (int64_t)-1;
  int64_t sec = ((int64_t *)(uintptr_t)ts_ptr)[0];
  int64_t nsec = ((int64_t *)(uintptr_t)ts_ptr)[1];
  int64_t ms = sec * 1000 + nsec / 1000000;
  if (ms < 0)
    ms = 0;
  Sleep((DWORD)ms);
  return (int64_t)((0 << 1) | 1);
#else
  int r = nanosleep((const struct timespec *)(uintptr_t)ts_ptr, NULL);
  return rt_tag_v((int64_t)r);
#endif
}

int64_t __getpid(void) {
#ifdef _WIN32
  int pid = _getpid();
#else
  int pid = getpid();
#endif
  return rt_tag_v((int64_t)pid);
}

int64_t __getppid(void) {
#ifdef _WIN32
  int ppid = 0;
#else
  int ppid = getppid();
#endif
  return rt_tag_v((int64_t)ppid);
}

int64_t __getuid(void) {
#ifdef _WIN32
  int uid = 0;
#else
  int uid = getuid();
#endif
  return rt_tag_v((int64_t)uid);
}

int64_t __getgid(void) {
#ifdef _WIN32
  int gid = 0;
#else
  int gid = getgid();
#endif
  return rt_tag_v((int64_t)gid);
}

int64_t __getcwd(int64_t buf, int64_t size) {
  if (is_int(buf))
    buf >>= 1;
  if (is_int(size))
    size >>= 1;
#ifdef _WIN32
  char *r = _getcwd((char *)(uintptr_t)buf, (int)size);
#else
  char *r = getcwd((char *)(uintptr_t)buf, (size_t)size);
#endif
  if (!r)
    return (int64_t)-1;
  int64_t len = (int64_t)strlen(r);
  return rt_tag_v((int64_t)len);
}

int64_t __access(int64_t path, int64_t mode) {
  intptr_t rpath = (intptr_t)((path & 1) ? (path >> 1) : path);
  int64_t rmode = (mode & 1) ? (mode >> 1) : mode;
#ifdef _WIN32
  int r = _access((const char *)rpath, (int)rmode);
#else
  int r = access((const char *)rpath, (int)rmode);
#endif
  return rt_tag_v((int64_t)r);
}

int64_t __unlink(int64_t path) {
  intptr_t rpath = (intptr_t)((path & 1) ? (path >> 1) : path);
#ifdef _WIN32
  int r = _unlink((const char *)rpath);
  if (r < 0)
    r = -errno;
#else
  int r = unlink((const char *)rpath);
#endif
  return rt_tag_v((int64_t)r);
}

int64_t __pipe(int64_t fds_ptr) {
  if (is_int(fds_ptr))
    fds_ptr >>= 1;
#ifdef _WIN32
  int r = -1;
  if (fds_ptr) {
    int fds[2];
    r = _pipe(fds, 4096, _O_BINARY);
    if (r == 0) {
      ((int32_t *)(uintptr_t)fds_ptr)[0] = fds[0];
      ((int32_t *)(uintptr_t)fds_ptr)[1] = fds[1];
    }
  }
#else
  int r = pipe((int *)(uintptr_t)fds_ptr);
#endif
  return rt_tag_v((int64_t)r);
}

int64_t __dup2(int64_t oldfd, int64_t newfd) {
  if (is_int(oldfd))
    oldfd >>= 1;
  if (is_int(newfd))
    newfd >>= 1;
#ifdef _WIN32
  int r = _dup2((int)oldfd, (int)newfd);
#else
  int r = dup2((int)oldfd, (int)newfd);
#endif
  return rt_tag_v((int64_t)r);
}

int64_t __fork(void) {
#ifdef _WIN32
  int r = -1;
#else
  int r = fork();
#endif
  return rt_tag_v((int64_t)r);
}

int64_t __wait4(int64_t pid, int64_t status_ptr, int64_t options) {
  if (is_int(pid))
    pid >>= 1;
  if (is_int(status_ptr))
    status_ptr >>= 1;
  if (is_int(options))
    options >>= 1;
#ifdef _WIN32
  int r = -1;
#else
  int r = waitpid((pid_t)pid, (int *)(uintptr_t)status_ptr, (int)options);
#endif
  return rt_tag_v((int64_t)r);
}

int64_t __exit(int64_t code) {
  if (is_int(code))
    code >>= 1;
#ifdef _WIN32
  ExitProcess((unsigned int)code);
#else
  _exit((int)code);
#endif
  return (int64_t)((0 << 1) | 1);
}

int64_t __enable_vt(void) {
#ifdef _WIN32
  int ok = 0;
  HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
  if (h != INVALID_HANDLE_VALUE) {
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode)) {
      mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
      if (SetConsoleMode(h, mode))
        ok = 1;
    }
  }
  h = GetStdHandle(STD_ERROR_HANDLE);
  if (h != INVALID_HANDLE_VALUE) {
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode)) {
      mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
      if (SetConsoleMode(h, mode))
        ok = 1;
    }
  }
  if (!ok)
    return (int64_t)-1;
  return (int64_t)((0 << 1) | 1);
#else
  return (int64_t)((0 << 1) | 1);
#endif
}

#ifdef _WIN32
static int __tty_mode_saved = 0;
static DWORD __tty_mode_prev = 0;
#else
static int __tty_mode_saved = 0;
static struct termios __tty_mode_prev;
#endif

int64_t __tty_raw(int64_t enable) {
  if (is_int(enable))
    enable >>= 1;
#ifdef _WIN32
  HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
  if (hIn == INVALID_HANDLE_VALUE)
    return rt_tag_v((int64_t)-1);
  if (enable) {
    DWORD mode = 0;
    if (!GetConsoleMode(hIn, &mode))
      return rt_tag_v((int64_t)-1);
    if (!__tty_mode_saved) {
      __tty_mode_prev = mode;
      __tty_mode_saved = 1;
    }
    mode &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
    if (!SetConsoleMode(hIn, mode))
      return rt_tag_v((int64_t)-1);
    return rt_tag_v((int64_t)0);
  }
  if (__tty_mode_saved && !SetConsoleMode(hIn, __tty_mode_prev))
    return rt_tag_v((int64_t)-1);
  __tty_mode_saved = 0;
  return rt_tag_v((int64_t)0);
#else
  if (enable) {
    if (!isatty(STDIN_FILENO))
      return rt_tag_v((int64_t)-1);
    struct termios t;
    if (tcgetattr(STDIN_FILENO, &t) != 0)
      return rt_tag_v((int64_t)-errno);
    if (!__tty_mode_saved) {
      __tty_mode_prev = t;
      __tty_mode_saved = 1;
    }
    t.c_lflag &= (tcflag_t) ~(ICANON | ECHO);
#ifdef IEXTEN
    t.c_lflag &= (tcflag_t)~IEXTEN;
#endif
#ifdef IXON
    t.c_iflag &= (tcflag_t)~IXON;
#endif
#ifdef ICRNL
    t.c_iflag &= (tcflag_t)~ICRNL;
#endif
#ifdef BRKINT
    t.c_iflag &= (tcflag_t)~BRKINT;
#endif
#ifdef INPCK
    t.c_iflag &= (tcflag_t)~INPCK;
#endif
#ifdef ISTRIP
    t.c_iflag &= (tcflag_t)~ISTRIP;
#endif
#ifdef OPOST
    t.c_oflag &= (tcflag_t)~OPOST;
#endif
#ifdef CS8
    t.c_cflag |= CS8;
#endif
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &t) != 0)
      return rt_tag_v((int64_t)-errno);
    return rt_tag_v((int64_t)0);
  }
  if (__tty_mode_saved &&
      tcsetattr(STDIN_FILENO, TCSANOW, &__tty_mode_prev) != 0)
    return rt_tag_v((int64_t)-errno);
  __tty_mode_saved = 0;
  return rt_tag_v((int64_t)0);
#endif
}

int64_t __tty_pending(void) {
#ifdef _WIN32
  return rt_tag_v((int64_t)(_kbhit() ? 1 : 0));
#else
  int n = 0;
  if (ioctl(STDIN_FILENO, FIONREAD, &n) < 0)
    return rt_tag_v((int64_t)0);
  if (n < 0)
    n = 0;
  return rt_tag_v((int64_t)n);
#endif
}

int64_t __tty_size(int64_t out_ptr) {
  if (is_int(out_ptr))
    out_ptr >>= 1;
  if (!out_ptr)
    return rt_tag_v((int64_t)-1);
#ifdef _WIN32
  HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
  if (h == INVALID_HANDLE_VALUE)
    h = GetStdHandle(STD_ERROR_HANDLE);
  if (h == INVALID_HANDLE_VALUE)
    return rt_tag_v((int64_t)-1);
  CONSOLE_SCREEN_BUFFER_INFO info;
  if (!GetConsoleScreenBufferInfo(h, &info))
    return rt_tag_v((int64_t)-1);
  int cols = (int)(info.srWindow.Right - info.srWindow.Left + 1);
  int rows = (int)(info.srWindow.Bottom - info.srWindow.Top + 1);
  if (cols <= 0)
    cols = (int)info.dwSize.X;
  if (rows <= 0)
    rows = (int)info.dwSize.Y;
  if (cols <= 0 || rows <= 0)
    return rt_tag_v((int64_t)-1);
  ((int32_t *)(uintptr_t)out_ptr)[0] = (int32_t)cols;
  ((int32_t *)(uintptr_t)out_ptr)[1] = (int32_t)rows;
  return rt_tag_v((int64_t)0);
#else
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0) {
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) != 0)
      return rt_tag_v((int64_t)-1);
  }
  int cols = (int)ws.ws_col;
  int rows = (int)ws.ws_row;
  if (cols <= 0 || rows <= 0)
    return rt_tag_v((int64_t)-1);
  ((int32_t *)(uintptr_t)out_ptr)[0] = (int32_t)cols;
  ((int32_t *)(uintptr_t)out_ptr)[1] = (int32_t)rows;
  return rt_tag_v((int64_t)0);
#endif
}

int64_t __is_dir(int64_t path) {
  intptr_t rpath = (intptr_t)((path & 1) ? (path >> 1) : path);
#ifdef _WIN32
  DWORD attr = GetFileAttributesA((const char *)rpath);
  if (attr == INVALID_FILE_ATTRIBUTES)
    return (int64_t)((0 << 1) | 1);
  return (int64_t)(((attr & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0) << 1 | 1);
#else
  struct stat st;
  if (stat((const char *)rpath, &st) != 0)
    return (int64_t)((0 << 1) | 1);
  return (int64_t)(((S_ISDIR(st.st_mode) ? 1 : 0) << 1) | 1);
#endif
}

#ifdef _WIN32
typedef struct ny_dir {
  HANDLE h;
  WIN32_FIND_DATAA data;
  int first;
} ny_dir;
#endif

int64_t __dir_open(int64_t path) {
  intptr_t rpath = (intptr_t)((path & 1) ? (path >> 1) : path);
#ifdef _WIN32
  ny_dir *d = (ny_dir *)malloc(sizeof(ny_dir));
  if (!d)
    return 0;
  d->first = 1;
  char pattern[MAX_PATH];
  size_t len = strlen((const char *)rpath);
  if (len > 0 && (((const char *)rpath)[len - 1] == '\\' ||
                  ((const char *)rpath)[len - 1] == '/')) {
    snprintf(pattern, sizeof(pattern), "%s*", (const char *)rpath);
  } else {
    snprintf(pattern, sizeof(pattern), "%s\\*", (const char *)rpath);
  }
  d->h = FindFirstFileA(pattern, &d->data);
  if (d->h == INVALID_HANDLE_VALUE) {
    free(d);
    return 0;
  }
  return (int64_t)(uintptr_t)d;
#else
  DIR *dir = opendir((const char *)rpath);
  if (!dir)
    return 0;
  return (int64_t)(uintptr_t)dir;
#endif
}

int64_t __dir_read(int64_t handle) {
  if (is_int(handle))
    handle >>= 1;
#ifdef _WIN32
  ny_dir *d = (ny_dir *)(uintptr_t)handle;
  if (!d)
    return 0;
  WIN32_FIND_DATAA *data = &d->data;
  while (1) {
    if (d->first) {
      d->first = 0;
    } else {
      if (!FindNextFileA(d->h, data))
        return 0;
    }
    const char *name = data->cFileName;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
      continue;
    return __make_str(name);
  }
#else
  DIR *dir = (DIR *)(uintptr_t)handle;
  if (!dir)
    return 0;
  struct dirent *ent;
  while ((ent = readdir(dir)) != NULL) {
    const char *name = ent->d_name;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
      continue;
    return __make_str(name);
  }
  return 0;
#endif
}

int64_t __dir_close(int64_t handle) {
  if (is_int(handle))
    handle >>= 1;
#ifdef _WIN32
  ny_dir *d = (ny_dir *)(uintptr_t)handle;
  if (!d)
    return (int64_t)-1;
  FindClose(d->h);
  free(d);
  return (int64_t)((0 << 1) | 1);
#else
  DIR *dir = (DIR *)(uintptr_t)handle;
  if (!dir)
    return (int64_t)-1;
  closedir(dir);
  return (int64_t)((0 << 1) | 1);
#endif
}

// Sockets (portable wrappers)
#ifdef _WIN32
static int __ws_init_done = 0;
static void __ws_init(void) {
  if (__ws_init_done)
    return;
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0) {
    __ws_init_done = 1;
  }
}
#endif

#ifdef _WIN32
static char *__build_cmdline(char **argv) {
  size_t cap = 1024;
  size_t len = 0;
  char *buf = (char *)malloc(cap);
  if (!buf)
    return NULL;
  buf[0] = 0;
  for (size_t i = 0; argv && argv[i]; ++i) {
    const char *s = argv[i];
    int need_q = 0;
    for (const char *p = s; *p; ++p) {
      if (*p == ' ' || *p == '\t' || *p == '"') {
        need_q = 1;
        break;
      }
    }
    size_t extra = strlen(s) + 3;
    if (len + extra + 2 >= cap) {
      cap = (cap + extra + 1024) * 2;
      buf = (char *)realloc(buf, cap);
      if (!buf)
        return NULL;
    }
    if (len > 0)
      buf[len++] = ' ';
    if (need_q)
      buf[len++] = '"';
    for (const char *p = s; *p; ++p) {
      if (*p == '"')
        buf[len++] = '\\';
      buf[len++] = *p;
    }
    if (need_q)
      buf[len++] = '"';
  }
  buf[len] = 0;
  return buf;
}

static HANDLE __dup_inheritable(HANDLE src) {
  if (!src || src == INVALID_HANDLE_VALUE)
    return NULL;
  HANDLE out = NULL;
  if (!DuplicateHandle(GetCurrentProcess(), src, GetCurrentProcess(), &out, 0,
                       TRUE, DUPLICATE_SAME_ACCESS))
    return NULL;
  return out;
}

static HANDLE __open_nul_handle(DWORD access) {
  SECURITY_ATTRIBUTES sa;
  ZeroMemory(&sa, sizeof(sa));
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  return CreateFileA("NUL", access, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
}

typedef struct __proc_handle_node {
  DWORD pid;
  HANDLE h;
  struct __proc_handle_node *next;
} __proc_handle_node;

static __proc_handle_node *__proc_handles = NULL;

static void __proc_handle_put(DWORD pid, HANDLE h) {
  if (!pid || !h || h == INVALID_HANDLE_VALUE) {
    if (h && h != INVALID_HANDLE_VALUE)
      CloseHandle(h);
    return;
  }
  __proc_handle_node *n =
      (__proc_handle_node *)malloc(sizeof(__proc_handle_node));
  if (!n) {
    CloseHandle(h);
    return;
  }
  n->pid = pid;
  n->h = h;
  n->next = __proc_handles;
  __proc_handles = n;
}

static HANDLE __proc_handle_take(DWORD pid) {
  __proc_handle_node *prev = NULL;
  __proc_handle_node *cur = __proc_handles;
  while (cur) {
    if (cur->pid == pid) {
      HANDLE h = cur->h;
      if (prev) {
        prev->next = cur->next;
      } else {
        __proc_handles = cur->next;
      }
      free(cur);
      return h;
    }
    prev = cur;
    cur = cur->next;
  }
  return NULL;
}
#endif

int64_t __socket(int64_t domain, int64_t type, int64_t protocol) {
  if (is_int(domain))
    domain >>= 1;
  if (is_int(type))
    type >>= 1;
  if (is_int(protocol))
    protocol >>= 1;
#ifdef _WIN32
  __ws_init();
  SOCKET s = socket((int)domain, (int)type, (int)protocol);
  if (s == INVALID_SOCKET)
    return (int64_t)-1;
  return rt_tag_v((int64_t)s);
#else
  int fd = socket((int)domain, (int)type, (int)protocol);
  return rt_tag_v((int64_t)fd);
#endif
}

int64_t __connect(int64_t fd, int64_t addr, int64_t addrlen) {
  if (is_int(fd))
    fd >>= 1;
  if (is_int(addr))
    addr >>= 1;
  if (is_int(addrlen))
    addrlen >>= 1;
#ifdef _WIN32
  int r = connect((SOCKET)fd, (const struct sockaddr *)(uintptr_t)addr,
                  (int)addrlen);
#else
  int r = connect((int)fd, (const struct sockaddr *)(uintptr_t)addr,
                  (socklen_t)addrlen);
#endif
  return rt_tag_v((int64_t)r);
}

int64_t __bind(int64_t fd, int64_t addr, int64_t addrlen) {
  if (is_int(fd))
    fd >>= 1;
  if (is_int(addr))
    addr >>= 1;
  if (is_int(addrlen))
    addrlen >>= 1;
#ifdef _WIN32
  int r =
      bind((SOCKET)fd, (const struct sockaddr *)(uintptr_t)addr, (int)addrlen);
#else
  int r = bind((int)fd, (const struct sockaddr *)(uintptr_t)addr,
               (socklen_t)addrlen);
#endif
  return rt_tag_v((int64_t)r);
}

int64_t __listen(int64_t fd, int64_t backlog) {
  if (is_int(fd))
    fd >>= 1;
  if (is_int(backlog))
    backlog >>= 1;
#ifdef _WIN32
  int r = listen((SOCKET)fd, (int)backlog);
#else
  int r = listen((int)fd, (int)backlog);
#endif
  return rt_tag_v((int64_t)r);
}

int64_t __accept(int64_t fd, int64_t addr, int64_t addrlen) {
  if (is_int(fd))
    fd >>= 1;
  if (is_int(addr))
    addr >>= 1;
  if (is_int(addrlen))
    addrlen >>= 1;
#ifdef _WIN32
  SOCKET s = accept((SOCKET)fd, (struct sockaddr *)(uintptr_t)addr,
                    (int *)(uintptr_t)addrlen);
  if (s == INVALID_SOCKET)
    return (int64_t)-1;
  return rt_tag_v((int64_t)s);
#else
  int s = accept((int)fd, (struct sockaddr *)(uintptr_t)addr,
                 (socklen_t *)(uintptr_t)addrlen);
  return rt_tag_v((int64_t)s);
#endif
}

int64_t __sendto(int64_t fd, int64_t buf, int64_t len, int64_t flags,
                 int64_t addr, int64_t addrlen) {
  if (is_int(fd))
    fd >>= 1;
  if (is_int(buf))
    buf >>= 1;
  if (is_int(len))
    len >>= 1;
  if (is_int(flags))
    flags >>= 1;
  if (is_int(addr))
    addr >>= 1;
  if (is_int(addrlen))
    addrlen >>= 1;
#ifdef _WIN32
  int r = sendto((SOCKET)fd, (const char *)(uintptr_t)buf, (int)len, (int)flags,
                 (const struct sockaddr *)(uintptr_t)addr, (int)addrlen);
#else
  ssize_t r =
      sendto((int)fd, (const void *)(uintptr_t)buf, (size_t)len, (int)flags,
             (const struct sockaddr *)(uintptr_t)addr, (socklen_t)addrlen);
#endif
  return rt_tag_v((int64_t)r);
}

int64_t __recvfrom(int64_t fd, int64_t buf, int64_t len, int64_t flags,
                   int64_t addr, int64_t addrlen) {
  if (is_int(fd))
    fd >>= 1;
  if (is_int(buf))
    buf >>= 1;
  if (is_int(len))
    len >>= 1;
  if (is_int(flags))
    flags >>= 1;
  if (is_int(addr))
    addr >>= 1;
  if (is_int(addrlen))
    addrlen >>= 1;
#ifdef _WIN32
  int alen = (int)addrlen;
  int r = recvfrom((SOCKET)fd, (char *)(uintptr_t)buf, (int)len, (int)flags,
                   (struct sockaddr *)(uintptr_t)addr, &alen);
#else
  socklen_t alen = (socklen_t)addrlen;
  ssize_t r = recvfrom((int)fd, (void *)(uintptr_t)buf, (size_t)len, (int)flags,
                       (struct sockaddr *)(uintptr_t)addr, &alen);
#endif
  return rt_tag_v((int64_t)r);
}

int64_t __setsockopt(int64_t fd, int64_t level, int64_t optname, int64_t optval,
                     int64_t optlen) {
  if (is_int(fd))
    fd >>= 1;
  if (is_int(level))
    level >>= 1;
  if (is_int(optname))
    optname >>= 1;
  if (is_int(optval))
    optval >>= 1;
  if (is_int(optlen))
    optlen >>= 1;
#ifdef _WIN32
  int r = setsockopt((SOCKET)fd, (int)level, (int)optname,
                     (const char *)(uintptr_t)optval, (int)optlen);
#else
  int r = setsockopt((int)fd, (int)level, (int)optname,
                     (const void *)(uintptr_t)optval, (socklen_t)optlen);
#endif
  return rt_tag_v((int64_t)r);
}

int64_t __recv(int64_t fd, int64_t buf, int64_t len, int64_t flags) {
  if (is_int(fd))
    fd >>= 1;
  if (is_int(buf))
    buf >>= 1;
  if (is_int(len))
    len >>= 1;
  if (is_int(flags))
    flags >>= 1;
#ifdef _WIN32
  int r = recv((SOCKET)fd, (char *)(uintptr_t)buf, (int)len, (int)flags);
#else
  ssize_t r = recv((int)fd, (void *)(uintptr_t)buf, (size_t)len, (int)flags);
#endif
  return rt_tag_v((int64_t)r);
}

int64_t __send(int64_t fd, int64_t buf, int64_t len, int64_t flags) {
  if (is_int(fd))
    fd >>= 1;
  if (is_int(buf))
    buf >>= 1;
  if (is_int(len))
    len >>= 1;
  if (is_int(flags))
    flags >>= 1;
#ifdef _WIN32
  int r = send((SOCKET)fd, (const char *)(uintptr_t)buf, (int)len, (int)flags);
#else
  ssize_t r =
      send((int)fd, (const void *)(uintptr_t)buf, (size_t)len, (int)flags);
#endif
  return rt_tag_v((int64_t)r);
}

int64_t __closesocket(int64_t fd) {
  if (is_int(fd))
    fd >>= 1;
#ifdef _WIN32
  int r = closesocket((SOCKET)fd);
#else
  int r = close((int)fd);
#endif
  return rt_tag_v((int64_t)r);
}

int64_t __spawn_wait(int64_t path, int64_t argv) {
  intptr_t rpath = (intptr_t)((path & 1) ? (path >> 1) : path);
  intptr_t rargv = (intptr_t)((argv & 1) ? (argv >> 1) : argv);
#ifdef _WIN32
  bool av_free = false;
  char **av = ny_native_argv(rargv, &av_free);
  char *cmd = __build_cmdline(av);
  if (!cmd) {
    if (av_free)
      free(av);
    return (int64_t)-1;
  }
  STARTUPINFOA si;
  PROCESS_INFORMATION pi;
  HANDLE child_stdin = NULL;
  HANDLE child_stdout = NULL;
  HANDLE child_stderr = NULL;
  ZeroMemory(&si, sizeof(si));
  ZeroMemory(&pi, sizeof(pi));
  si.cb = sizeof(si);
  child_stdin = __dup_inheritable(GetStdHandle(STD_INPUT_HANDLE));
  if (!child_stdin || child_stdin == INVALID_HANDLE_VALUE)
    child_stdin = __open_nul_handle(GENERIC_READ);
  child_stdout = __dup_inheritable(GetStdHandle(STD_OUTPUT_HANDLE));
  if (!child_stdout || child_stdout == INVALID_HANDLE_VALUE)
    child_stdout = __open_nul_handle(GENERIC_WRITE);
  child_stderr = __dup_inheritable(GetStdHandle(STD_ERROR_HANDLE));
  if (!child_stderr || child_stderr == INVALID_HANDLE_VALUE)
    child_stderr = __open_nul_handle(GENERIC_WRITE);

  if (!child_stdin || child_stdin == INVALID_HANDLE_VALUE || !child_stdout ||
      child_stdout == INVALID_HANDLE_VALUE || !child_stderr ||
      child_stderr == INVALID_HANDLE_VALUE) {
    if (child_stdin && child_stdin != INVALID_HANDLE_VALUE)
      CloseHandle(child_stdin);
    if (child_stdout && child_stdout != INVALID_HANDLE_VALUE)
      CloseHandle(child_stdout);
    if (child_stderr && child_stderr != INVALID_HANDLE_VALUE)
      CloseHandle(child_stderr);
    free(cmd);
    if (av_free)
      free(av);
    return (int64_t)-1;
  }
  si.dwFlags |= STARTF_USESTDHANDLES;
  si.hStdInput = child_stdin;
  si.hStdOutput = child_stdout;
  si.hStdError = child_stderr;

  const char *app = NULL;
  if (rpath)
    app = (const char *)rpath;
  BOOL ok = FALSE;
  size_t cmd_len = strlen(cmd);
  char *cmd_try = (char *)malloc(cmd_len + 1);
  if (cmd_try) {
    memcpy(cmd_try, cmd, cmd_len + 1);
    if (app) {
      ok = CreateProcessA(app, cmd_try, NULL, NULL, TRUE, 0, NULL, NULL, &si,
                          &pi);
    }
    free(cmd_try);
  }
  if (!ok) {
    ZeroMemory(&pi, sizeof(pi));
    cmd_try = (char *)malloc(cmd_len + 1);
    if (cmd_try) {
      memcpy(cmd_try, cmd, cmd_len + 1);
      ok = CreateProcessA(NULL, cmd_try, NULL, NULL, TRUE, 0, NULL, NULL, &si,
                          &pi);
      free(cmd_try);
    }
  }
  if (child_stdin && child_stdin != INVALID_HANDLE_VALUE)
    CloseHandle(child_stdin);
  if (child_stdout && child_stdout != INVALID_HANDLE_VALUE)
    CloseHandle(child_stdout);
  if (child_stderr && child_stderr != INVALID_HANDLE_VALUE)
    CloseHandle(child_stderr);
  free(cmd);
  if (av_free)
    free(av);
  if (!ok)
    return (int64_t)-1;
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 0;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return rt_tag_v((int64_t)code);
#elif defined(__APPLE__)
  bool av_free = false;
  char **av = ny_native_argv(rargv, &av_free);
  pid_t pid = 0;
  int r = posix_spawn(&pid, (const char *)rpath, NULL, NULL, (char *const *)av,
                      environ);
  if (av_free)
    free(av);
  if (r != 0)
    return (int64_t)-1;
  int status = 0;
  if (waitpid(pid, &status, 0) < 0)
    return (int64_t)-1;
  if (WIFEXITED(status)) {
    int code = WEXITSTATUS(status);
    return rt_tag_v((int64_t)code);
  }
  if (WIFSIGNALED(status)) {
    int code = 128 + WTERMSIG(status);
    return rt_tag_v((int64_t)code);
  }
  return (int64_t)-1;
#else
  bool av_free = false;
  char **av = ny_native_argv(rargv, &av_free);
  pid_t pid = fork();
  if (pid == 0) {
    execve((const char *)rpath, (char *const *)av, environ);
    _exit(127);
  }
  if (pid < 0) {
    if (av_free)
      free(av);
    return (int64_t)-1;
  }
  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    if (av_free)
      free(av);
    return (int64_t)-1;
  }
  if (av_free)
    free(av);
  if (WIFEXITED(status)) {
    int code = WEXITSTATUS(status);
    return rt_tag_v((int64_t)code);
  }
  if (WIFSIGNALED(status)) {
    int code = 128 + WTERMSIG(status);
    return rt_tag_v((int64_t)code);
  }
  return (int64_t)-1;
#endif
}

int64_t __spawn_pipe(int64_t path, int64_t argv, int64_t fds_ptr) {
  (void)path;
  intptr_t rargv = (intptr_t)((argv & 1) ? (argv >> 1) : argv);
  if (is_int(fds_ptr))
    fds_ptr >>= 1;
#ifndef _WIN32
  intptr_t rpath = (intptr_t)((path & 1) ? (path >> 1) : path);
#endif
#ifdef _WIN32
  int in_fds[2];
  int out_fds[2];
  if (_pipe(in_fds, 4096, _O_BINARY) != 0)
    return (int64_t)-1;
  if (_pipe(out_fds, 4096, _O_BINARY) != 0) {
    _close(in_fds[0]);
    _close(in_fds[1]);
    return (int64_t)-1;
  }
  HANDLE hIn = (HANDLE)_get_osfhandle(in_fds[0]);
  HANDLE hOut = (HANDLE)_get_osfhandle(out_fds[1]);
  SetHandleInformation((HANDLE)_get_osfhandle(in_fds[1]), HANDLE_FLAG_INHERIT,
                       0);
  SetHandleInformation((HANDLE)_get_osfhandle(out_fds[0]), HANDLE_FLAG_INHERIT,
                       0);
  STARTUPINFOA si;
  PROCESS_INFORMATION pi;
  ZeroMemory(&si, sizeof(si));
  ZeroMemory(&pi, sizeof(pi));
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = hIn;
  si.hStdOutput = hOut;
  si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  bool av_free = false;
  char **av = ny_native_argv(rargv, &av_free);
  char *cmd = __build_cmdline(av);
  if (!cmd) {
    if (av_free)
      free(av);
    _close(in_fds[0]);
    _close(in_fds[1]);
    _close(out_fds[0]);
    _close(out_fds[1]);
    return (int64_t)-1;
  }
  BOOL ok =
      CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
  free(cmd);
  if (av_free)
    free(av);
  if (!ok) {
    _close(in_fds[0]);
    _close(in_fds[1]);
    _close(out_fds[0]);
    _close(out_fds[1]);
    return (int64_t)-1;
  }
  CloseHandle(pi.hThread);
  __proc_handle_put(pi.dwProcessId, pi.hProcess);
  _close(in_fds[0]);
  _close(out_fds[1]);
  ((int32_t *)(uintptr_t)fds_ptr)[0] = in_fds[1];
  ((int32_t *)(uintptr_t)fds_ptr)[1] = out_fds[0];
  return rt_tag_v((int64_t)pi.dwProcessId);
#elif defined(__APPLE__)
  bool av_free = false;
  char **av = ny_native_argv(rargv, &av_free);
  int in_fds[2];
  int out_fds[2];
  if (pipe(in_fds) != 0)
    return (int64_t)-1;
  if (pipe(out_fds) != 0) {
    close(in_fds[0]);
    close(in_fds[1]);
    return (int64_t)-1;
  }
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, in_fds[0], 0);
  posix_spawn_file_actions_adddup2(&actions, out_fds[1], 1);
  posix_spawn_file_actions_addclose(&actions, in_fds[1]);
  posix_spawn_file_actions_addclose(&actions, out_fds[0]);
  pid_t pid = 0;
  int r = posix_spawn(&pid, (const char *)rpath, &actions, NULL,
                      (char *const *)av, environ);
  posix_spawn_file_actions_destroy(&actions);
  if (av_free)
    free(av);
  if (r != 0) {
    close(in_fds[0]);
    close(in_fds[1]);
    close(out_fds[0]);
    close(out_fds[1]);
    return (int64_t)-1;
  }
  close(in_fds[0]);
  close(out_fds[1]);
  ((int32_t *)(uintptr_t)fds_ptr)[0] = in_fds[1];
  ((int32_t *)(uintptr_t)fds_ptr)[1] = out_fds[0];
  return rt_tag_v((int64_t)pid);
#else
  bool av_free = false;
  char **av = ny_native_argv(rargv, &av_free);
  int in_fds[2];
  int out_fds[2];
  if (pipe(in_fds) != 0)
    return (int64_t)-1;
  if (pipe(out_fds) != 0) {
    close(in_fds[0]);
    close(in_fds[1]);
    return (int64_t)-1;
  }
  pid_t pid = fork();
  if (pid == 0) {
    dup2(in_fds[0], STDIN_FILENO);
    dup2(out_fds[1], STDOUT_FILENO);
    close(in_fds[0]);
    close(in_fds[1]);
    close(out_fds[0]);
    close(out_fds[1]);
    execve((const char *)rpath, (char *const *)av, environ);
    _exit(127);
  }
  if (pid < 0) {
    if (av_free)
      free(av);
    close(in_fds[0]);
    close(in_fds[1]);
    close(out_fds[0]);
    close(out_fds[1]);
    return (int64_t)-1;
  }
  if (av_free)
    free(av);
  close(in_fds[0]);
  close(out_fds[1]);
  ((int32_t *)(uintptr_t)fds_ptr)[0] = in_fds[1];
  ((int32_t *)(uintptr_t)fds_ptr)[1] = out_fds[0];
  return rt_tag_v((int64_t)pid);
#endif
}

int64_t __wait_process(int64_t pid) {
  if (is_int(pid))
    pid >>= 1;
#ifdef _WIN32
  if (pid <= 0)
    return (int64_t)-1;
  HANDLE h = __proc_handle_take((DWORD)pid);
  if (!h) {
    h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_INFORMATION, FALSE, (DWORD)pid);
  }
  if (!h)
    return (int64_t)-1;
  DWORD wr = WaitForSingleObject(h, INFINITE);
  if (wr == WAIT_FAILED) {
    CloseHandle(h);
    return (int64_t)-1;
  }
  DWORD code = 0;
  if (!GetExitCodeProcess(h, &code)) {
    CloseHandle(h);
    return (int64_t)-1;
  }
  CloseHandle(h);
  return rt_tag_v((int64_t)code);
#else
  (void)pid;
  return (int64_t)-1;
#endif
}

int64_t __execve(int64_t path, int64_t argv, int64_t envp) {
  intptr_t rpath = (intptr_t)((path & 1) ? (path >> 1) : path);
  intptr_t rargv = (intptr_t)((argv & 1) ? (argv >> 1) : argv);
  intptr_t renvp = (intptr_t)((envp & 1) ? (envp >> 1) : envp);
#ifdef _WIN32
  (void)rpath;
  (void)rargv;
  (void)renvp;
  int64_t res = -1;
#else
  bool av_free = false;
  bool env_free = false;
  char **av = ny_native_argv(rargv, &av_free);
  char **ev = renvp ? ny_native_envp(renvp, &env_free) : environ;
  int64_t res =
      execve((const char *)rpath, (char *const *)av, (char *const *)ev);
  if (av_free)
    free(av);
  if (env_free)
    free(ev);
#endif
  return (int64_t)(((uint64_t)res << 1) | 1);
}

static int64_t __thread_call_dispatch(int64_t fn, int64_t argc,
                                      const int64_t *argv) {
  switch (argc) {
  case 0:
    return __call0(fn);
  case 1:
    return __call1(fn, argv[0]);
  case 2:
    return __call2(fn, argv[0], argv[1]);
  case 3:
    return __call3(fn, argv[0], argv[1], argv[2]);
  case 4:
    return __call4(fn, argv[0], argv[1], argv[2], argv[3]);
  case 5:
    return __call5(fn, argv[0], argv[1], argv[2], argv[3], argv[4]);
  case 6:
    return __call6(fn, argv[0], argv[1], argv[2], argv[3], argv[4], argv[5]);
  case 7:
    return __call7(fn, argv[0], argv[1], argv[2], argv[3], argv[4], argv[5],
                   argv[6]);
  case 8:
    return __call8(fn, argv[0], argv[1], argv[2], argv[3], argv[4], argv[5],
                   argv[6], argv[7]);
  case 9:
    return __call9(fn, argv[0], argv[1], argv[2], argv[3], argv[4], argv[5],
                   argv[6], argv[7], argv[8]);
  case 10:
    return __call10(fn, argv[0], argv[1], argv[2], argv[3], argv[4], argv[5],
                    argv[6], argv[7], argv[8], argv[9]);
  case 11:
    return __call11(fn, argv[0], argv[1], argv[2], argv[3], argv[4], argv[5],
                    argv[6], argv[7], argv[8], argv[9], argv[10]);
  case 12:
    return __call12(fn, argv[0], argv[1], argv[2], argv[3], argv[4], argv[5],
                    argv[6], argv[7], argv[8], argv[9], argv[10], argv[11]);
  case 13:
    return __call13(fn, argv[0], argv[1], argv[2], argv[3], argv[4], argv[5],
                    argv[6], argv[7], argv[8], argv[9], argv[10], argv[11],
                    argv[12]);
  case 14:
    return __call14(fn, argv[0], argv[1], argv[2], argv[3], argv[4], argv[5],
                    argv[6], argv[7], argv[8], argv[9], argv[10], argv[11],
                    argv[12], argv[13]);
  case 15:
    return __call15(fn, argv[0], argv[1], argv[2], argv[3], argv[4], argv[5],
                    argv[6], argv[7], argv[8], argv[9], argv[10], argv[11],
                    argv[12], argv[13], argv[14]);
  default:
    return 0;
  }
}

static bool __thread_prepare_call_args(int64_t argc, int64_t argv_ptr,
                                       int64_t *argc_raw_out,
                                       int64_t **argv_copy_out) {
  if (!argc_raw_out || !argv_copy_out)
    return false;
  int64_t argc_raw = is_int(argc) ? (argc >> 1) : argc;
  if (argc_raw < 0 || argc_raw > 15)
    return false;
  int64_t *argv_copy = NULL;
  if (argc_raw > 0) {
    int64_t src_ptr = is_int(argv_ptr) ? (argv_ptr >> 1) : argv_ptr;
    if (!src_ptr)
      return false;
    argv_copy = (int64_t *)malloc((size_t)argc_raw * sizeof(int64_t));
    if (!argv_copy)
      return false;
    memcpy(argv_copy, (const void *)(uintptr_t)src_ptr,
           (size_t)argc_raw * sizeof(int64_t));
  }
  *argc_raw_out = argc_raw;
  *argv_copy_out = argv_copy;
  return true;
}

#ifdef _WIN32
typedef struct __thread_state {
  HANDLE h;
  int64_t ret;
} __thread_state;

typedef struct __thread_arg {
  int64_t fn;
  int64_t arg;
  int64_t argc;
  int64_t *argv;
  __thread_state *st;
} __thread_arg;

static DWORD WINAPI __thread_trampoline(LPVOID p) {
  __thread_arg *ta = (__thread_arg *)p;
  __thread_state *st = ta->st;
  int64_t fn = ta->fn;
  int64_t arg = ta->arg;
  int64_t ret = 0;
  if (ta->argc >= 0) {
    ret = __thread_call_dispatch(fn, ta->argc, ta->argv);
  } else {
    if (NY_NATIVE_IS(fn)) {
      int64_t (*f)(int64_t) = (int64_t (*)(int64_t))NY_NATIVE_DECODE(fn);
      ret = rt_tag_v(f(rt_untag_v(arg)));
    } else if (is_heap_ptr(fn) && *(int64_t *)((uintptr_t)fn - 8) == 105) {
      int64_t code = *(int64_t *)fn;
      int64_t env = *(int64_t *)(fn + 8);
      ret = ((int64_t (*)(int64_t, int64_t))__mask_ptr(code))(env, arg);
    } else {
      ret = ((int64_t (*)(int64_t))__mask_ptr(fn))(arg);
    }
  }
  if (st)
    st->ret = ret;
  if (ta->argv)
    free(ta->argv);
  free(ta);
  return 0;
}

int64_t __thread_spawn(int64_t fn, int64_t arg) {
  __thread_state *st = (__thread_state *)malloc(sizeof(__thread_state));
  if (!st)
    return -1;
  st->ret = 0;
  __thread_arg *ta = (__thread_arg *)malloc(sizeof(__thread_arg));
  if (!ta) {
    free(st);
    return -1;
  }
  ta->fn = fn;
  ta->arg = arg;
  ta->argc = -1;
  ta->argv = NULL;
  ta->st = st;
  HANDLE h = CreateThread(NULL, 0, __thread_trampoline, ta, 0, NULL);
  if (!h) {
    free(ta);
    free(st);
    return -1;
  }
  st->h = h;
  return (int64_t)(uintptr_t)st;
}

int64_t __thread_spawn_call(int64_t fn, int64_t argc, int64_t argv_ptr) {
  int64_t argc_raw = 0;
  int64_t *argv_copy = NULL;
  if (!__thread_prepare_call_args(argc, argv_ptr, &argc_raw, &argv_copy))
    return -1;
  __thread_state *st = (__thread_state *)malloc(sizeof(__thread_state));
  if (!st) {
    if (argv_copy)
      free(argv_copy);
    return -1;
  }
  st->ret = 0;
  __thread_arg *ta = (__thread_arg *)malloc(sizeof(__thread_arg));
  if (!ta) {
    free(st);
    if (argv_copy)
      free(argv_copy);
    return -1;
  }
  ta->fn = fn;
  ta->arg = 0;
  ta->argc = argc_raw;
  ta->argv = argv_copy;
  ta->st = st;
  HANDLE h = CreateThread(NULL, 0, __thread_trampoline, ta, 0, NULL);
  if (!h) {
    free(ta);
    free(st);
    if (argv_copy)
      free(argv_copy);
    return -1;
  }
  st->h = h;
  return (int64_t)(uintptr_t)st;
}

int64_t __thread_launch_call(int64_t fn, int64_t argc, int64_t argv_ptr) {
  int64_t argc_raw = 0;
  int64_t *argv_copy = NULL;
  if (!__thread_prepare_call_args(argc, argv_ptr, &argc_raw, &argv_copy))
    return -1;
  __thread_arg *ta = (__thread_arg *)malloc(sizeof(__thread_arg));
  if (!ta) {
    if (argv_copy)
      free(argv_copy);
    return -1;
  }
  ta->fn = fn;
  ta->arg = 0;
  ta->argc = argc_raw;
  ta->argv = argv_copy;
  ta->st = NULL;
  HANDLE h = CreateThread(NULL, 0, __thread_trampoline, ta, 0, NULL);
  if (!h) {
    free(ta);
    if (argv_copy)
      free(argv_copy);
    return -1;
  }
  CloseHandle(h);
  return 0;
}

int64_t __thread_join(int64_t tid) {
  if (!tid)
    return -1;
  __thread_state *st = (__thread_state *)(uintptr_t)tid;
  WaitForSingleObject(st->h, INFINITE);
  CloseHandle(st->h);
  int64_t res = st->ret;
  free(st);
  return res;
}

int64_t __mutex_new(void) {
  HANDLE h = CreateMutexA(NULL, FALSE, NULL);
  return (int64_t)(uintptr_t)h;
}

int64_t __mutex_lock64(int64_t m) {
  if (!m)
    return -1;
  DWORD r = WaitForSingleObject((HANDLE)(uintptr_t)m, INFINITE);
  return (r == WAIT_OBJECT_0) ? 0 : -1;
}

int64_t __mutex_unlock64(int64_t m) {
  if (!m)
    return -1;
  return ReleaseMutex((HANDLE)(uintptr_t)m) ? 0 : -1;
}

int64_t __mutex_free(int64_t m) {
  if (!m)
    return 0;
  CloseHandle((HANDLE)(uintptr_t)m);
  return 0;
}
#else
typedef struct __thread_arg {
  int64_t fn;
  int64_t arg;
  int64_t argc;
  int64_t *argv;
} __thread_arg;

static void *__thread_trampoline(void *p) {
  __thread_arg *ta = (__thread_arg *)p;
  int64_t fn = ta->fn;
  int64_t arg = ta->arg;
  int64_t res = 0;
  if (ta->argc >= 0) {
    res = __thread_call_dispatch(fn, ta->argc, ta->argv);
  } else {
    if (NY_NATIVE_IS(fn)) {
      int64_t (*f)(int64_t) = (int64_t (*)(int64_t))NY_NATIVE_DECODE(fn);
      res = rt_tag_v(f(rt_untag_v(arg)));
    } else if (is_heap_ptr(fn) && *(int64_t *)((uintptr_t)fn - 8) == 105) {
      int64_t code = *(int64_t *)fn;
      int64_t env = *(int64_t *)(fn + 8);
      res = ((int64_t (*)(int64_t, int64_t))__mask_ptr(code))(env, arg);
    } else {
      res = ((int64_t (*)(int64_t))__mask_ptr(fn))(arg);
    }
  }
  if (ta->argv)
    free(ta->argv);
  free(ta);
  return (void *)(uintptr_t)res;
}

int64_t __thread_spawn(int64_t fn, int64_t arg) {
  pthread_t tid;
  __thread_arg *ta = malloc(sizeof(__thread_arg));
  if (!ta)
    return -1;
  ta->fn = fn;
  ta->arg = arg;
  ta->argc = -1;
  ta->argv = NULL;
  int r = pthread_create(&tid, NULL, __thread_trampoline, ta);
  if (r != 0) {
    free(ta);
    return -r;
  }
  return (int64_t)tid;
}

int64_t __thread_spawn_call(int64_t fn, int64_t argc, int64_t argv_ptr) {
  int64_t argc_raw = 0;
  int64_t *argv_copy = NULL;
  if (!__thread_prepare_call_args(argc, argv_ptr, &argc_raw, &argv_copy))
    return -1;
  pthread_t tid;
  __thread_arg *ta = malloc(sizeof(__thread_arg));
  if (!ta) {
    if (argv_copy)
      free(argv_copy);
    return -1;
  }
  ta->fn = fn;
  ta->arg = 0;
  ta->argc = argc_raw;
  ta->argv = argv_copy;
  int r = pthread_create(&tid, NULL, __thread_trampoline, ta);
  if (r != 0) {
    if (argv_copy)
      free(argv_copy);
    free(ta);
    return -r;
  }
  return (int64_t)tid;
}

int64_t __thread_launch_call(int64_t fn, int64_t argc, int64_t argv_ptr) {
  int64_t argc_raw = 0;
  int64_t *argv_copy = NULL;
  if (!__thread_prepare_call_args(argc, argv_ptr, &argc_raw, &argv_copy))
    return -1;
  pthread_t tid;
  __thread_arg *ta = malloc(sizeof(__thread_arg));
  if (!ta) {
    if (argv_copy)
      free(argv_copy);
    return -1;
  }
  ta->fn = fn;
  ta->arg = 0;
  ta->argc = argc_raw;
  ta->argv = argv_copy;
  int r = pthread_create(&tid, NULL, __thread_trampoline, ta);
  if (r != 0) {
    if (argv_copy)
      free(argv_copy);
    free(ta);
    return -r;
  }
  r = pthread_detach(tid);
  if (r != 0)
    return -r;
  return 0;
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
#endif

int64_t __os_name(void) {
  static int64_t cached = 0;
  if (cached)
    return cached;
#if defined(__linux__)
  const char *s = "linux";
#elif defined(__APPLE__)
  const char *s = "macos";
#elif defined(__FreeBSD__)
  const char *s = "freebsd";
#elif defined(_WIN32)
  const char *s = "windows";
#else
  const char *s = "unknown";
#endif
  size_t len = strlen(s);
  int64_t res = __malloc(((int64_t)len + 1) << 1 | 1);
  if (!res)
    return 0;
  *(int64_t *)(uintptr_t)((char *)res - 8) = TAG_STR;
  *(int64_t *)(uintptr_t)((char *)res - 16) = ((int64_t)len << 1) | 1;
  strcpy((char *)(uintptr_t)res, s);
  cached = res;
  return res;
}

int64_t __arch_name(void) {
  static int64_t cached = 0;
  if (cached)
    return cached;
#if defined(__x86_64__) || defined(_M_X64)
  const char *s = "x86_64";
#elif defined(__i386__) || defined(_M_IX86)
  const char *s = "x86";
#elif defined(__aarch64__) || defined(_M_ARM64)
  const char *s = "aarch64";
#elif defined(__arm__) || defined(_M_ARM)
  const char *s = "arm";
#elif defined(__riscv)
  const char *s = "riscv";
#else
  const char *s = "unknown";
#endif
  size_t len = strlen(s);
  int64_t res = __malloc(((int64_t)len + 1) << 1 | 1);
  if (!res)
    return 0;
  *(int64_t *)(uintptr_t)((char *)res - 8) = TAG_STR;
  *(int64_t *)(uintptr_t)((char *)res - 16) = ((int64_t)len << 1) | 1;
  strcpy((char *)(uintptr_t)res, s);
  cached = res;
  return res;
}

static int64_t __make_str(const char *s) {
  size_t len = strlen(s);
  int64_t res = __malloc(((int64_t)len + 1) << 1 | 1);
  if (!res)
    return 0;
  *(int64_t *)(uintptr_t)((char *)res - 8) = TAG_STR;
  *(int64_t *)(uintptr_t)((char *)res - 16) = ((int64_t)len << 1) | 1;
  strcpy((char *)(uintptr_t)res, s);
  return res;
}
