#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "rt/shared.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <time.h>
#ifdef _WIN32
#include <conio.h>
#include <direct.h>
#include <io.h>
#include <process.h>
#include <windows.h>
#else
#include <dirent.h>
#include <pthread.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#endif
#ifdef __linux__
#include <pty.h>
#include <utmp.h>
#endif
#ifdef __APPLE__
#include <util.h>
#endif
#ifdef __APPLE__
#include <spawn.h>
extern char **environ;
#elif !defined(_WIN32)
extern char **environ;
#endif
#ifdef __APPLE__
extern int openpty(int *amaster, int *aslave, char *name, struct termios *termp,
                   struct winsize *winp);
#endif
int64_t rt_call0(int64_t f);
int64_t rt_call1(int64_t f, int64_t a0);
int64_t rt_call2(int64_t f, int64_t a0, int64_t a1);
int64_t rt_call3(int64_t f, int64_t a0, int64_t a1, int64_t a2);
int64_t rt_call4(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3);
int64_t rt_call5(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4);
int64_t rt_call6(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5);
int64_t rt_call7(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5,
                 int64_t a6);
int64_t rt_call8(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5,
                 int64_t a6, int64_t a7);
int64_t rt_call9(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5,
                 int64_t a6, int64_t a7, int64_t a8);
int64_t rt_call10(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5,
                  int64_t a6, int64_t a7, int64_t a8, int64_t a9);
int64_t rt_call11(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5,
                  int64_t a6, int64_t a7, int64_t a8, int64_t a9, int64_t a10);
int64_t rt_call12(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5,
                  int64_t a6, int64_t a7, int64_t a8, int64_t a9, int64_t a10, int64_t a11);
int64_t rt_call13(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5,
                  int64_t a6, int64_t a7, int64_t a8, int64_t a9, int64_t a10, int64_t a11,
                  int64_t a12);
int64_t rt_call14(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5,
                  int64_t a6, int64_t a7, int64_t a8, int64_t a9, int64_t a10, int64_t a11,
                  int64_t a12, int64_t a13);
int64_t rt_call15(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5,
                  int64_t a6, int64_t a7, int64_t a8, int64_t a9, int64_t a10, int64_t a11,
                  int64_t a12, int64_t a13, int64_t a14);
int64_t rt_tty_install_cleanup(void);

static char **ny_native_argv(intptr_t rargv, bool *needs_free) {
  if (needs_free)
    *needs_free = false;
  if (!rargv)
    return NULL;

  int64_t tag = *(int64_t *)((char *)rargv - 8);
  char **dst = NULL;
  size_t count = 0;

  if (tag == TAG_LIST || tag == TAG_TUPLE) {
    count = rt_untag_v(*(int64_t *)rargv);
    dst = (char **)calloc(count + 1, sizeof(char *));
    if (!dst)
      return NULL;
    int64_t *src = (int64_t *)((char *)rargv + 16);
    for (size_t i = 0; i < count; i++) {
      dst[i] = (char *)(uintptr_t)rt_untag_v(src[i]);
    }
  } else {
    // Fallback for raw NULL-terminated array
    int64_t *src = (int64_t *)(uintptr_t)rargv;
    while (src[count] != 0 && src[count] != 1)
      count++;
    dst = (char **)calloc(count + 1, sizeof(char *));
    if (!dst)
      return NULL;
    for (size_t i = 0; i < count; i++)
      dst[i] = (char *)(uintptr_t)rt_untag_v(src[i]);
  }

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
  return ny_native_argv(renvp, needs_free);
}
#endif

#if defined(__linux__) && defined(rt_x86_64__)
int64_t rt_syscall(int64_t n, int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, int64_t f) {
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
  register long _num rt_asm__("rax") = rn;
  register long _arg1 rt_asm__("rdi") = ra;
  register long _arg2 rt_asm__("rsi") = rb;
  register long _arg3 rt_asm__("rdx") = rc;
  register long _arg4 rt_asm__("r10") = rd;
  register long _arg5 rt_asm__("r8") = re;
  register long _arg6 rt_asm__("r9") = rf;
  rt_asm__ rt_volatile__("syscall\n" : "+r"(_num) : "r"(_arg1), "r"(_arg2), "r"(_arg3), "r"(_arg4),
                         "r"(_arg5), "r"(_arg6) : "rcx", "r11", "memory");
  return (int64_t)(((uint64_t)_num << 1) | 1);
}
#else
int64_t rt_syscall(int64_t n, int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, int64_t f) {
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

#ifdef _WIN32
static int64_t rt_write_console_utf8_fd(int fd, const char *ptr, size_t len) {
  if (!ptr)
    return -1;
  if (len == 0)
    return 0;
  intptr_t raw = _get_osfhandle(fd);
  if (raw == -1)
    return -1;
  HANDLE h = (HANDLE)raw;
  DWORD mode = 0;
  if (!GetConsoleMode(h, &mode))
    return -1;
  SetConsoleOutputCP(CP_UTF8);
  int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, ptr, (int)len,
                                 NULL, 0);
  if (wlen <= 0)
    return -1;
  WCHAR *wide = (WCHAR *)malloc(sizeof(WCHAR) * (size_t)wlen);
  if (!wide)
    return -1;
  int got = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, ptr, (int)len,
                                wide, wlen);
  if (got != wlen) {
    free(wide);
    return -1;
  }

  size_t extra = 0;
  for (int i = 0; i < wlen; ++i) {
    if (wide[i] == L'\n' && (i == 0 || wide[i - 1] != L'\r'))
      extra++;
  }
  WCHAR *out = wide;
  size_t out_len = (size_t)wlen;
  if (extra > 0) {
    out = (WCHAR *)malloc(sizeof(WCHAR) * ((size_t)wlen + extra));
    if (!out) {
      free(wide);
      return -1;
    }
    size_t j = 0;
    for (int i = 0; i < wlen; ++i) {
      if (wide[i] == L'\n' && (i == 0 || wide[i - 1] != L'\r'))
        out[j++] = L'\r';
      out[j++] = wide[i];
    }
    out_len = j;
  }

  size_t done = 0;
  while (done < out_len) {
    DWORD chunk = (DWORD)((out_len - done) > 32768 ? 32768 : (out_len - done));
    DWORD written = 0;
    if (!WriteConsoleW(h, out + done, chunk, &written, NULL) || written == 0) {
      if (out != wide)
        free(out);
      free(wide);
      return -1;
    }
    done += written;
  }
  if (out != wide)
    free(out);
  free(wide);
  return (int64_t)len;
}

int64_t rt_write_stdout_console(const char *ptr, size_t len) {
  return rt_write_console_utf8_fd(1, ptr, len);
}
#endif

int64_t rt_read_off(int64_t fd, int64_t buf, int64_t len, int64_t off) {
  if (is_int(fd))
    fd >>= 1;
  if (is_int(len))
    len >>= 1;
  if (is_int(off))
    off >>= 1;
  if (!rt_check_oob("sys_read", buf, off, (size_t)len))
    return -1LL;
  char *ptr = (char *)((intptr_t)rt_untag_v(buf) + (intptr_t)off);
#ifdef _WIN32
  ssize_t r = _read((int)fd, ptr, (unsigned int)len);
  if (r < 0)
    r = -errno;
#else
  ssize_t r = read((int)fd, ptr, (size_t)len);
  if (r < 0)
    r = -errno;
#endif
  return rt_tag_v((int64_t)r);
}

int64_t rt_write_off(int64_t fd, int64_t buf, int64_t len, int64_t off) {
  if (is_int(fd))
    fd >>= 1;
  if (is_int(len))
    len >>= 1;
  if (is_int(off))
    off >>= 1;
  if (fd == 1) {
    extern int64_t rt_print_flush(void);
    rt_print_flush();
  }
  if (!rt_check_oob("sys_write", buf, off, (size_t)len))
    return -1LL;
  char *ptr = (char *)((intptr_t)rt_untag_v(buf) + (intptr_t)off);
#ifdef _WIN32
  int64_t cr = (fd == 1 || fd == 2)
                   ? rt_write_console_utf8_fd((int)fd, ptr, (size_t)len)
                   : -1;
  ssize_t r = cr >= 0 ? (ssize_t)cr : _write((int)fd, ptr, (unsigned int)len);
  if (r < 0)
    r = -errno;
#else
  ssize_t r = write((int)fd, ptr, (size_t)len);
  if (r < 0)
    r = -errno;
#endif
  return rt_tag_v((int64_t)r);
}

int64_t rt_save_tga_rgba(int64_t path, int64_t data, int64_t width, int64_t height,
                         int64_t channels) {
  const char *path_p = (const char *)(uintptr_t)(is_int(path) ? rt_untag_v(path) : path);
  const uint8_t *src = (const uint8_t *)(uintptr_t)(is_int(data) ? rt_untag_v(data) : data);
  int64_t w64 = is_int(width) ? rt_untag_v(width) : width;
  int64_t h64 = is_int(height) ? rt_untag_v(height) : height;
  int64_t ch64 = is_int(channels) ? rt_untag_v(channels) : channels;
  if (!path_p || !src || w64 <= 0 || h64 <= 0 || w64 > 65535 || h64 > 65535 || ch64 <= 0)
    return rt_tag_v(-22);
  size_t w = (size_t)w64;
  size_t h = (size_t)h64;
  size_t ch = (size_t)ch64;
  if (ch > 4)
    ch = 4;
  if (w > SIZE_MAX / h || w * h > SIZE_MAX / 4 || w * ch > SIZE_MAX / h)
    return rt_tag_v(-75);
  size_t src_size = w * h * ch;
  if (!rt_check_oob("save_tga_rgba", data, 0, src_size))
    return rt_tag_v(-14);

  FILE *fp = fopen(path_p, "wb");
  if (!fp)
    return rt_tag_v((int64_t)-errno);

  uint8_t header[18] = {0};
  header[2] = 2; /* uncompressed true-color */
  header[12] = (uint8_t)(w & 255u);
  header[13] = (uint8_t)((w >> 8) & 255u);
  header[14] = (uint8_t)(h & 255u);
  header[15] = (uint8_t)((h >> 8) & 255u);
  header[16] = 32;
  header[17] = 40; /* top-left origin + 8 alpha bits */
  if (fwrite(header, 1, sizeof(header), fp) != sizeof(header)) {
    int e = errno ? errno : EIO;
    fclose(fp);
    return rt_tag_v((int64_t)-e);
  }

  size_t row_bytes = w * 4u;
  uint8_t *row = (uint8_t *)malloc(row_bytes);
  if (!row) {
    fclose(fp);
    return rt_tag_v(-12);
  }

  for (size_t y = 0; y < h; ++y) {
    const uint8_t *srow = src + y * w * ch;
    if (ch == 4) {
      for (size_t x = 0; x < w; ++x) {
        const uint8_t *s = srow + x * 4u;
        uint8_t *d = row + x * 4u;
        d[0] = s[2];
        d[1] = s[1];
        d[2] = s[0];
        d[3] = s[3];
      }
    } else if (ch == 3) {
      for (size_t x = 0; x < w; ++x) {
        const uint8_t *s = srow + x * 3u;
        uint8_t *d = row + x * 4u;
        d[0] = s[2];
        d[1] = s[1];
        d[2] = s[0];
        d[3] = 255u;
      }
    } else if (ch == 2) {
      for (size_t x = 0; x < w; ++x) {
        const uint8_t *s = srow + x * 2u;
        uint8_t *d = row + x * 4u;
        d[0] = s[0];
        d[1] = s[0];
        d[2] = s[0];
        d[3] = s[1];
      }
    } else {
      for (size_t x = 0; x < w; ++x) {
        uint8_t v = srow[x];
        uint8_t *d = row + x * 4u;
        d[0] = v;
        d[1] = v;
        d[2] = v;
        d[3] = 255u;
      }
    }
    if (fwrite(row, 1, row_bytes, fp) != row_bytes) {
      int e = errno ? errno : EIO;
      free(row);
      fclose(fp);
      return rt_tag_v((int64_t)-e);
    }
  }
  free(row);
  if (fclose(fp) != 0)
    return rt_tag_v((int64_t)-(errno ? errno : EIO));
  return rt_tag_v((int64_t)(18u + w * h * 4u));
}

int64_t rt_open(int64_t path, int64_t flags, int64_t mode) {
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

int64_t rt_close(int64_t fd) {
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

int64_t rt_ioctl(int64_t fd, int64_t req, int64_t arg) {
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

int64_t rt_clock_gettime(int64_t clk, int64_t ts_ptr) {
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
    if (!QueryPerformanceFrequency(&freq) || !QueryPerformanceCounter(&counter) ||
        freq.QuadPart == 0) {
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

int64_t rt_nanosleep(int64_t ts_ptr) {
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

int64_t rt_time_seconds(void) {
#ifdef _WIN32
  FILETIME ft;
  ULARGE_INTEGER uli;
  GetSystemTimeAsFileTime(&ft);
  uli.LowPart = ft.dwLowDateTime;
  uli.HighPart = ft.dwHighDateTime;
  uint64_t t100 = uli.QuadPart;
  const uint64_t EPOCH = 116444736000000000ULL;
  if (t100 < EPOCH)
    t100 = EPOCH;
  return rt_tag_v((int64_t)((t100 - EPOCH) / 10000000ULL));
#else
  struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
    return rt_tag_v(0);
  return rt_tag_v((int64_t)ts.tv_sec);
#endif
}

int64_t rt_time_milliseconds(void) {
#ifdef _WIN32
  FILETIME ft;
  ULARGE_INTEGER uli;
  GetSystemTimeAsFileTime(&ft);
  uli.LowPart = ft.dwLowDateTime;
  uli.HighPart = ft.dwHighDateTime;
  uint64_t t100 = uli.QuadPart;
  const uint64_t EPOCH = 116444736000000000ULL;
  if (t100 < EPOCH)
    t100 = EPOCH;
  return rt_tag_v((int64_t)((t100 - EPOCH) / 10000ULL));
#else
  struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
    return rt_tag_v(0);
  return rt_tag_v((int64_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000));
#endif
}

int64_t rt_ticks_ns(void) {
#ifdef _WIN32
  LARGE_INTEGER freq, counter;
  if (!QueryPerformanceFrequency(&freq) || !QueryPerformanceCounter(&counter) ||
      freq.QuadPart == 0) {
    return rt_tag_v(0);
  }
  uint64_t sec = (uint64_t)counter.QuadPart / (uint64_t)freq.QuadPart;
  uint64_t rem = (uint64_t)counter.QuadPart % (uint64_t)freq.QuadPart;
  uint64_t nsec = (rem * 1000000000ULL) / (uint64_t)freq.QuadPart;
  uint64_t total = sec * 1000000000ULL + nsec;
#else
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    return rt_tag_v(0);
  uint64_t total = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
  if (total > (uint64_t)INT64_MAX / 2u)
    total = (uint64_t)INT64_MAX / 2u;
  return rt_tag_v((int64_t)total);
}

int64_t rt_msleep_ms(int64_t ms) {
  if (is_int(ms))
    ms >>= 1;
  if (ms < 0)
    ms = 0;
#ifdef _WIN32
  Sleep((DWORD)ms);
#else
  struct timespec req;
  req.tv_sec = (time_t)(ms / 1000);
  req.tv_nsec = (long)((ms % 1000) * 1000000);
  while (nanosleep(&req, &req) != 0 && errno == EINTR) {
  }
#endif
  return rt_tag_v(0);
}

int64_t rt_getpid(void) {
#ifdef _WIN32
  int pid = _getpid();
#else
  int pid = getpid();
#endif
  return rt_tag_v((int64_t)pid);
}

int64_t rt_getppid(void) {
#ifdef _WIN32
  int ppid = 0;
#else
  int ppid = getppid();
#endif
  return rt_tag_v((int64_t)ppid);
}

int64_t rt_getuid(void) {
#ifdef _WIN32
  int uid = 0;
#else
  int uid = getuid();
#endif
  return rt_tag_v((int64_t)uid);
}

int64_t rt_getgid(void) {
#ifdef _WIN32
  int gid = 0;
#else
  int gid = getgid();
#endif
  return rt_tag_v((int64_t)gid);
}

int64_t rt_getcwd(int64_t buf, int64_t size) {
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

int64_t rt_access(int64_t path, int64_t mode) {
  intptr_t rpath = (intptr_t)((path & 1) ? (path >> 1) : path);
  int64_t rmode = (mode & 1) ? (mode >> 1) : mode;
  int r = ny_access((const char *)rpath, (int)rmode);
  return rt_tag_v((int64_t)r);
}

int64_t rt_unlink(int64_t path) {
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

int64_t rt_rename(int64_t old_path, int64_t new_path) {
  intptr_t rold = (intptr_t)((old_path & 1) ? (old_path >> 1) : old_path);
  intptr_t rnew = (intptr_t)((new_path & 1) ? (new_path >> 1) : new_path);
#ifdef _WIN32
  int r = MoveFileExA((const char *)rold, (const char *)rnew, MOVEFILE_REPLACE_EXISTING) ? 0 : -((int)GetLastError());
#else
  int r = rename((const char *)rold, (const char *)rnew);
  if (r < 0)
    r = -errno;
#endif
  return rt_tag_v((int64_t)r);
}

int64_t rt_pipe(int64_t fds_ptr) {
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

int64_t rt_dup2(int64_t oldfd, int64_t newfd) {
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

int64_t rt_fork(void) {
#ifdef _WIN32
  int r = -1;
#else
  int r = fork();
#endif
  return rt_tag_v((int64_t)r);
}

int64_t rt_wait4(int64_t pid, int64_t status_ptr, int64_t options) {
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

extern int64_t rt_print_flush(void);

int64_t rt_exit(int64_t code) {
  if (is_int(code))
    code >>= 1;
  rt_print_flush();
#ifdef _WIN32
  ExitProcess((unsigned int)code);
#else
  _exit((int)code);
#endif
  return (int64_t)((0 << 1) | 1);
}

int64_t rt_enable_vt(void) {
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
static int rt_tty_mode_saved = 0;
static DWORD rt_tty_mode_prev = 0;
#else
static int rt_tty_mode_saved = 0;
static struct termios rt_tty_mode_prev;
#endif

int64_t rt_openpty(int64_t fds_ptr) {
  intptr_t ptr = (intptr_t)rt_untag_v(fds_ptr);
#if !defined(_WIN32)
  int m, s;
  int r = openpty(&m, &s, NULL, NULL, NULL);
  if (r == 0) {
    if (ptr) {
      ((int32_t *)ptr)[0] = m;
      ((int32_t *)ptr)[1] = s;
    }
  } else {
    r = -errno;
  }
  return rt_tag_v((int64_t)r);
#else
  (void)ptr;
  return rt_tag_v((int64_t)-1);
#endif
}

int64_t rt_setsid(void) {
#ifdef _WIN32
  return rt_tag_v((int64_t)-1);
#else
  return rt_tag_v((int64_t)setsid());
#endif
}

int64_t rt_tty_raw(int64_t enable) {
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
    if (!rt_tty_mode_saved) {
      rt_tty_mode_prev = mode;
      rt_tty_mode_saved = 1;
    }
    mode &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
    if (!SetConsoleMode(hIn, mode))
      return rt_tag_v((int64_t)-1);
    (void)rt_tty_install_cleanup();
    return rt_tag_v((int64_t)0);
  }
  if (rt_tty_mode_saved && !SetConsoleMode(hIn, rt_tty_mode_prev))
    return rt_tag_v((int64_t)-1);
  rt_tty_mode_saved = 0;
  return rt_tag_v((int64_t)0);
#else
  if (enable) {
    if (!isatty(STDIN_FILENO))
      return rt_tag_v((int64_t)-1);
    struct termios t;
    if (tcgetattr(STDIN_FILENO, &t) != 0)
      return rt_tag_v((int64_t)-errno);
    if (!rt_tty_mode_saved) {
      rt_tty_mode_prev = t;
      rt_tty_mode_saved = 1;
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
    (void)rt_tty_install_cleanup();
    return rt_tag_v((int64_t)0);
  }
  if (rt_tty_mode_saved && tcsetattr(STDIN_FILENO, TCSANOW, &rt_tty_mode_prev) != 0)
    return rt_tag_v((int64_t)-errno);
  rt_tty_mode_saved = 0;
  return rt_tag_v((int64_t)0);
#endif
}

int64_t rt_tty_sane_fd(int64_t fd) {
  if (is_int(fd))
    fd >>= 1;
#ifdef _WIN32
  (void)fd;
  return rt_tag_v((int64_t)-1);
#else
  if (fd < 0 || !isatty((int)fd))
    return rt_tag_v((int64_t)-1);
  struct termios t;
  if (tcgetattr((int)fd, &t) != 0)
    return rt_tag_v((int64_t)-errno);
  t.c_lflag |= (tcflag_t)(ICANON | ECHO | ISIG);
#ifdef IEXTEN
  t.c_lflag |= (tcflag_t)IEXTEN;
#endif
#ifdef ICRNL
  t.c_iflag |= (tcflag_t)ICRNL;
#endif
#ifdef IXON
  t.c_iflag |= (tcflag_t)IXON;
#endif
#ifdef IUTF8
  t.c_iflag |= (tcflag_t)IUTF8;
#endif
#ifdef OPOST
  t.c_oflag |= (tcflag_t)OPOST;
#endif
#ifdef ONLCR
  t.c_oflag |= (tcflag_t)ONLCR;
#endif
  if (tcsetattr((int)fd, TCSANOW, &t) != 0)
    return rt_tag_v((int64_t)-errno);
  return rt_tag_v((int64_t)0);
#endif
}

/*
 * Terminal-safe signal handler for SIGINT / SIGTERM
 * Restores the saved termios, exits alt-screen, resets attrs, shows cursor.
 * Called automatically when set_raw_mode() activates raw mode.
  */
#ifndef _WIN32
#include <signal.h>
static volatile sig_atomic_t rt_tty_sig_installed = 0;
static struct sigaction rt_tty_old_sigint;
static struct sigaction rt_tty_old_sigterm;
static struct sigaction rt_tty_old_sigquit;
static struct sigaction rt_tty_old_sighup;

static struct sigaction *rt_tty_old_sigaction(int sig) {
  if (sig == SIGINT)
    return &rt_tty_old_sigint;
  if (sig == SIGTERM)
    return &rt_tty_old_sigterm;
#ifdef SIGQUIT
  if (sig == SIGQUIT)
    return &rt_tty_old_sigquit;
#endif
#ifdef SIGHUP
  if (sig == SIGHUP)
    return &rt_tty_old_sighup;
#endif
  return NULL;
}

static void rt_tty_sig_restore(int sig) {
  /* Restore termios */
  if (rt_tty_mode_saved) {
    tcsetattr(STDIN_FILENO, TCSANOW, &rt_tty_mode_prev);
    rt_tty_mode_saved = 0;
  }
  /* Exit alt-screen, reset all SGR attrs, show cursor, enable wrap */
  static const char cleanup[] = "\033[0m"     /* reset SGR */
                                "\033[?25h"   /* show cursor */
                                "\033[?7h"    /* enable wrap */
                                "\033[?1049l" /* leave alt screen */
                                "\033[?2004l" /* disable bracketed paste */
                                "\033[?1000l" /* disable mouse tracking */
                                "\033[?1002l" /* disable mouse drag tracking */
                                "\033[?1003l" /* disable any-event mouse tracking */
                                "\033[?1006l" /* disable sgr mouse mode */
                                "\033[0m"     /* reset SGR again on main screen */
                                "\033[?25h"   /* show cursor on main screen */
                                "\033[?7h"    /* enable wrap on main screen */
                                "\033[2K\r";  /* clear current line and return */
  write(STDOUT_FILENO, cleanup, sizeof(cleanup) - 1);
  fsync(STDOUT_FILENO);
  /* Re-raise with original handler */
  struct sigaction *old = rt_tty_old_sigaction(sig);
  if (old)
    sigaction(sig, old, NULL);
  else
    signal(sig, SIG_DFL);
  raise(sig);
}

int64_t rt_tty_install_cleanup(void) {
  if (rt_tty_sig_installed)
    return 0;
  struct sigaction sa;
  unsigned char *_p_sa = (unsigned char *)&sa;
  for (size_t _i_sa = 0; _i_sa < sizeof(sa); _i_sa++)
    _p_sa[_i_sa] = 0;
  sa.sa_handler = rt_tty_sig_restore;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESETHAND;
  sigaction(SIGINT, &sa, &rt_tty_old_sigint);
  sigaction(SIGTERM, &sa, &rt_tty_old_sigterm);
#ifdef SIGQUIT
  sigaction(SIGQUIT, &sa, &rt_tty_old_sigquit);
#endif
#ifdef SIGHUP
  sigaction(SIGHUP, &sa, &rt_tty_old_sighup);
#endif
  rt_tty_sig_installed = 1;
  return 0;
}
#else
int64_t rt_tty_install_cleanup(void) { return 0; }
#endif

int64_t rt_tty_pending(void) {
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

int64_t rt_tty_size(int64_t out_ptr) {
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

int64_t rt_is_dir(int64_t path) {
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

int64_t rt_dir_open(int64_t path) {
  intptr_t rpath = (intptr_t)((path & 1) ? (path >> 1) : path);
#ifdef _WIN32
  ny_dir *d = (ny_dir *)malloc(sizeof(ny_dir));
  if (!d)
    return 0;
  d->first = 1;
  char pattern[MAX_PATH];
  size_t len = strlen((const char *)rpath);
  if (len > 0 &&
      (((const char *)rpath)[len - 1] == '\\' || ((const char *)rpath)[len - 1] == '/')) {
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

int64_t rt_dir_read(int64_t handle) {
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
    return rt_alloc_string(name);
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
    return rt_alloc_string(name);
  }
  return 0;
#endif
}

int64_t rt_dir_close(int64_t handle) {
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

#ifdef _WIN32
static int rt_ws_init_done = 0;
static void rt_ws_init(void) {
  if (rt_ws_init_done)
    return;
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0) {
    rt_ws_init_done = 1;
  }
}
#endif

#ifdef _WIN32
static char *rt_build_cmdline(char **argv) {
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

static HANDLE rt_dup_inheritable(HANDLE src) {
  if (!src || src == INVALID_HANDLE_VALUE)
    return NULL;
  HANDLE out = NULL;
  if (!DuplicateHandle(GetCurrentProcess(), src, GetCurrentProcess(), &out, 0, TRUE,
                       DUPLICATE_SAME_ACCESS))
    return NULL;
  return out;
}

static HANDLE rt_open_nul_handle(DWORD access) {
  SECURITY_ATTRIBUTES sa;
  ZeroMemory(&sa, sizeof(sa));
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  return CreateFileA("NUL", access, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING,
                     FILE_ATTRIBUTE_NORMAL, NULL);
}

typedef struct rt_proc_handle_node {
  DWORD pid;
  HANDLE h;
  struct rt_proc_handle_node *next;
} rt_proc_handle_node;

static rt_proc_handle_node *rt_proc_handles = NULL;

static void rt_proc_handle_put(DWORD pid, HANDLE h) {
  if (!pid || !h || h == INVALID_HANDLE_VALUE) {
    if (h && h != INVALID_HANDLE_VALUE)
      CloseHandle(h);
    return;
  }
  rt_proc_handle_node *n = (rt_proc_handle_node *)malloc(sizeof(rt_proc_handle_node));
  if (!n) {
    CloseHandle(h);
    return;
  }
  n->pid = pid;
  n->h = h;
  n->next = rt_proc_handles;
  rt_proc_handles = n;
}

static HANDLE rt_proc_handle_take(DWORD pid) {
  rt_proc_handle_node *prev = NULL;
  rt_proc_handle_node *cur = rt_proc_handles;
  while (cur) {
    if (cur->pid == pid) {
      HANDLE h = cur->h;
      if (prev) {
        prev->next = cur->next;
      } else {
        rt_proc_handles = cur->next;
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

int64_t rt_socket(int64_t domain, int64_t type, int64_t protocol) {
  if (is_int(domain))
    domain >>= 1;
  if (is_int(type))
    type >>= 1;
  if (is_int(protocol))
    protocol >>= 1;
#ifdef _WIN32
  rt_ws_init();
  SOCKET s = socket((int)domain, (int)type, (int)protocol);
  if (s == INVALID_SOCKET)
    return (int64_t)-1;
  return rt_tag_v((int64_t)s);
#else
  int fd = socket((int)domain, (int)type, (int)protocol);
  return rt_tag_v((int64_t)fd);
#endif
}

int64_t rt_connect(int64_t fd, int64_t addr, int64_t addrlen) {
  if (is_int(fd))
    fd >>= 1;
  if (is_int(addr))
    addr >>= 1;
  if (is_int(addrlen))
    addrlen >>= 1;
#ifdef _WIN32
  int r = connect((SOCKET)fd, (const struct sockaddr *)(uintptr_t)addr, (int)addrlen);
#else
  int r = connect((int)fd, (const struct sockaddr *)(uintptr_t)addr, (socklen_t)addrlen);
#endif
  return rt_tag_v((int64_t)r);
}

int64_t rt_bind(int64_t fd, int64_t addr, int64_t addrlen) {
  if (is_int(fd))
    fd >>= 1;
  if (is_int(addr))
    addr >>= 1;
  if (is_int(addrlen))
    addrlen >>= 1;
#ifdef _WIN32
  int r = bind((SOCKET)fd, (const struct sockaddr *)(uintptr_t)addr, (int)addrlen);
#else
  int r = bind((int)fd, (const struct sockaddr *)(uintptr_t)addr, (socklen_t)addrlen);
#endif
  return rt_tag_v((int64_t)r);
}

int64_t rt_listen(int64_t fd, int64_t backlog) {
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

int64_t rt_accept(int64_t fd, int64_t addr, int64_t addrlen) {
  if (is_int(fd))
    fd >>= 1;
  if (is_int(addr))
    addr >>= 1;
  if (is_int(addrlen))
    addrlen >>= 1;
#ifdef _WIN32
  SOCKET s = accept((SOCKET)fd, (struct sockaddr *)(uintptr_t)addr, (int *)(uintptr_t)addrlen);
  if (s == INVALID_SOCKET)
    return (int64_t)-1;
  return rt_tag_v((int64_t)s);
#else
  int s = accept((int)fd, (struct sockaddr *)(uintptr_t)addr, (socklen_t *)(uintptr_t)addrlen);
  return rt_tag_v((int64_t)s);
#endif
}

int64_t rt_sendto(int64_t fd, int64_t buf, int64_t len, int64_t flags, int64_t addr,
                  int64_t addrlen) {
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
  ssize_t r = sendto((int)fd, (const void *)(uintptr_t)buf, (size_t)len, (int)flags,
                     (const struct sockaddr *)(uintptr_t)addr, (socklen_t)addrlen);
#endif
  return rt_tag_v((int64_t)r);
}

int64_t rt_recvfrom(int64_t fd, int64_t buf, int64_t len, int64_t flags, int64_t addr,
                    int64_t addrlen) {
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

int64_t rt_setsockopt(int64_t fd, int64_t level, int64_t optname, int64_t optval, int64_t optlen) {
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
  int r = setsockopt((SOCKET)fd, (int)level, (int)optname, (const char *)(uintptr_t)optval,
                     (int)optlen);
#else
  int r = setsockopt((int)fd, (int)level, (int)optname, (const void *)(uintptr_t)optval,
                     (socklen_t)optlen);
#endif
  return rt_tag_v((int64_t)r);
}

int64_t rt_recv(int64_t fd, int64_t buf, int64_t len, int64_t flags) {
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

int64_t rt_send(int64_t fd, int64_t buf, int64_t len, int64_t flags) {
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
  ssize_t r = send((int)fd, (const void *)(uintptr_t)buf, (size_t)len, (int)flags);
#endif
  return rt_tag_v((int64_t)r);
}

int64_t rt_closesocket(int64_t fd) {
  if (is_int(fd))
    fd >>= 1;
#ifdef _WIN32
  int r = closesocket((SOCKET)fd);
#else
  int r = close((int)fd);
#endif
  return rt_tag_v((int64_t)r);
}

int64_t rt_spawn_wait(int64_t path, int64_t argv) {
  intptr_t rpath = (intptr_t)((path & 1) ? (path >> 1) : path);
  intptr_t rargv = (intptr_t)((argv & 1) ? (argv >> 1) : argv);
#ifdef _WIN32
  bool av_free = false;
  char **av = ny_native_argv(rargv, &av_free);
  char *cmd = rt_build_cmdline(av);
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
  child_stdin = rt_dup_inheritable(GetStdHandle(STD_INPUT_HANDLE));
  if (!child_stdin || child_stdin == INVALID_HANDLE_VALUE)
    child_stdin = rt_open_nul_handle(GENERIC_READ);
  child_stdout = rt_dup_inheritable(GetStdHandle(STD_OUTPUT_HANDLE));
  if (!child_stdout || child_stdout == INVALID_HANDLE_VALUE)
    child_stdout = rt_open_nul_handle(GENERIC_WRITE);
  child_stderr = rt_dup_inheritable(GetStdHandle(STD_ERROR_HANDLE));
  if (!child_stderr || child_stderr == INVALID_HANDLE_VALUE)
    child_stderr = rt_open_nul_handle(GENERIC_WRITE);
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
      ok = CreateProcessA(app, cmd_try, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    }
    free(cmd_try);
  }
  if (!ok) {
    ZeroMemory(&pi, sizeof(pi));
    cmd_try = (char *)malloc(cmd_len + 1);
    if (cmd_try) {
      memcpy(cmd_try, cmd, cmd_len + 1);
      ok = CreateProcessA(NULL, cmd_try, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
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
  int r = posix_spawn(&pid, (const char *)rpath, NULL, NULL, (char *const *)av, environ);
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

int64_t rt_spawn_pipe(int64_t path, int64_t argv, int64_t fds_ptr) {
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
  SetHandleInformation(hIn, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
  SetHandleInformation(hOut, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
  SetHandleInformation((HANDLE)_get_osfhandle(in_fds[1]), HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation((HANDLE)_get_osfhandle(out_fds[0]), HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOA si;
  PROCESS_INFORMATION pi;
  ZeroMemory(&si, sizeof(si));
  ZeroMemory(&pi, sizeof(pi));
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = hIn;
  si.hStdOutput = hOut;
  si.hStdError = hOut;
  bool av_free = false;
  char **av = ny_native_argv(rargv, &av_free);
  char *cmd = rt_build_cmdline(av);
  if (!cmd) {
    if (av_free)
      free(av);
    _close(in_fds[0]);
    _close(in_fds[1]);
    _close(out_fds[0]);
    _close(out_fds[1]);
    return (int64_t)-1;
  }
  BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
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
  rt_proc_handle_put(pi.dwProcessId, pi.hProcess);
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
  int r = posix_spawn(&pid, (const char *)rpath, &actions, NULL, (char *const *)av, environ);
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

int64_t rt_wait_process(int64_t pid) {
  if (is_int(pid))
    pid >>= 1;
#ifdef _WIN32
  if (pid <= 0)
    return (int64_t)-1;
  HANDLE h = rt_proc_handle_take((DWORD)pid);
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

int64_t rt_execve(int64_t path, int64_t argv, int64_t envp) {
  intptr_t rpath = (intptr_t)rt_untag_v(path);
  intptr_t rargv = (intptr_t)rt_untag_v(argv);
  intptr_t renvp = (intptr_t)rt_untag_v(envp);
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
  int64_t res = execve((const char *)rpath, (char *const *)av, (char *const *)ev);
  if (res < 0) {
    perror("execve");
  }
  if (av_free)
    free(av);
  if (env_free)
    free(ev);
#endif
  return (int64_t)(((uint64_t)res << 1) | 1);
}

static int64_t rt_thread_call_dispatch(int64_t fn, int64_t argc, const int64_t *argv) {
  switch (argc) {
  case 0:
    return rt_call0(fn);
  case 1:
    return rt_call1(fn, argv[0]);
  case 2:
    return rt_call2(fn, argv[0], argv[1]);
  case 3:
    return rt_call3(fn, argv[0], argv[1], argv[2]);
  case 4:
    return rt_call4(fn, argv[0], argv[1], argv[2], argv[3]);
  case 5:
    return rt_call5(fn, argv[0], argv[1], argv[2], argv[3], argv[4]);
  case 6:
    return rt_call6(fn, argv[0], argv[1], argv[2], argv[3], argv[4], argv[5]);
  case 7:
    return rt_call7(fn, argv[0], argv[1], argv[2], argv[3], argv[4], argv[5], argv[6]);
  case 8:
    return rt_call8(fn, argv[0], argv[1], argv[2], argv[3], argv[4], argv[5], argv[6], argv[7]);
  case 9:
    return rt_call9(fn, argv[0], argv[1], argv[2], argv[3], argv[4], argv[5], argv[6], argv[7],
                    argv[8]);
  case 10:
    return rt_call10(fn, argv[0], argv[1], argv[2], argv[3], argv[4], argv[5], argv[6], argv[7],
                     argv[8], argv[9]);
  case 11:
    return rt_call11(fn, argv[0], argv[1], argv[2], argv[3], argv[4], argv[5], argv[6], argv[7],
                     argv[8], argv[9], argv[10]);
  case 12:
    return rt_call12(fn, argv[0], argv[1], argv[2], argv[3], argv[4], argv[5], argv[6], argv[7],
                     argv[8], argv[9], argv[10], argv[11]);
  case 13:
    return rt_call13(fn, argv[0], argv[1], argv[2], argv[3], argv[4], argv[5], argv[6], argv[7],
                     argv[8], argv[9], argv[10], argv[11], argv[12]);
  case 14:
    return rt_call14(fn, argv[0], argv[1], argv[2], argv[3], argv[4], argv[5], argv[6], argv[7],
                     argv[8], argv[9], argv[10], argv[11], argv[12], argv[13]);
  case 15:
    return rt_call15(fn, argv[0], argv[1], argv[2], argv[3], argv[4], argv[5], argv[6], argv[7],
                     argv[8], argv[9], argv[10], argv[11], argv[12], argv[13], argv[14]);
  default:
    return 0;
  }
}

static bool rt_thread_prepare_call_args(int64_t argc, int64_t argv_ptr, int64_t *argc_raw_out,
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
    memcpy(argv_copy, (const void *)(uintptr_t)src_ptr, (size_t)argc_raw * sizeof(int64_t));
  }
  *argc_raw_out = argc_raw;
  *argv_copy_out = argv_copy;
  return true;
}

typedef enum rt_async_state {
  RT_ASYNC_READY = 0,
  RT_ASYNC_RUNNING = 1,
  RT_ASYNC_WAITING = 2,
  RT_ASYNC_DONE = 3,
  RT_ASYNC_FAILED = 4,
  RT_ASYNC_CANCELLED = 5,
} rt_async_state;

typedef enum rt_async_kind {
  RT_ASYNC_CALL = 0,
  RT_ASYNC_TIMER = 1,
  RT_ASYNC_WAIT_FD = 2,
  RT_ASYNC_RECV = 3,
  RT_ASYNC_SEND = 4,
  RT_ASYNC_ACCEPT = 5,
  RT_ASYNC_CONNECT = 6,
  RT_ASYNC_READ_SOCKET = 7,
  RT_ASYNC_WRITE_ALL = 8,
  RT_ASYNC_READ_UNTIL = 9,
} rt_async_kind;

#define RT_ASYNC_MAGIC 0x4e595441534b3031ULL
#define RT_ASYNC_EV_READ 1
#define RT_ASYNC_EV_WRITE 2

typedef struct rt_async_task {
  uint64_t magic;
  rt_async_state state;
  rt_async_kind kind;
  int64_t result;
  int64_t fn;
  int64_t argc;
  int64_t *argv;
  int64_t fd;
  int64_t flags;
  int64_t events;
  int64_t timeout_ms;
  int64_t deadline_ms;
  int64_t buf;
  int64_t len;
  int64_t off;
  int64_t data;
  int connect_started;
  int old_flags;
  unsigned char addr[128];
  int64_t addrlen;
  char *heap_buf;
  int64_t heap_len;
  int64_t heap_cap;
  char *needle_buf;
  int64_t needle_len;
  struct rt_async_task *next;
  struct rt_async_task *all_next;
} rt_async_task;

static rt_async_task *g_async_ready_head = NULL;
static rt_async_task *g_async_ready_tail = NULL;
static rt_async_task *g_async_all = NULL;

static void rt_async_complete(rt_async_task *t, int64_t result);

static int64_t rt_async_raw(int64_t v) {
  if (NY_NATIVE_IS(v))
    return (int64_t)(uintptr_t)NY_NATIVE_DECODE(v);
  return is_int(v) ? (v >> 1) : v;
}

static int64_t rt_async_now_ms(void) {
  struct timespec ts;
#if defined(_WIN32)
  timespec_get(&ts, TIME_UTC);
#else
  clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
  return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}

static void rt_async_ready_push(rt_async_task *t) {
  if (!t || t->state == RT_ASYNC_DONE || t->state == RT_ASYNC_FAILED ||
      t->state == RT_ASYNC_CANCELLED)
    return;
  t->next = NULL;
  if (g_async_ready_tail)
    g_async_ready_tail->next = t;
  else
    g_async_ready_head = t;
  g_async_ready_tail = t;
  t->state = RT_ASYNC_READY;
}

static rt_async_task *rt_async_ready_pop(void) {
  rt_async_task *t = g_async_ready_head;
  if (!t)
    return NULL;
  g_async_ready_head = t->next;
  if (!g_async_ready_head)
    g_async_ready_tail = NULL;
  t->next = NULL;
  return t;
}

static void rt_async_all_add(rt_async_task *t) {
  if (!t)
    return;
  t->all_next = g_async_all;
  g_async_all = t;
}

static void rt_async_all_remove(rt_async_task *t) {
  if (!t)
    return;
  rt_async_task **pp = &g_async_all;
  while (*pp) {
    if (*pp == t) {
      *pp = t->all_next;
      t->all_next = NULL;
      return;
    }
    pp = &(*pp)->all_next;
  }
}

static rt_async_task *rt_async_find_task(int64_t handle) {
  uintptr_t raw = (uintptr_t)rt_async_raw(handle);
  if (!raw)
    return NULL;
  for (rt_async_task *t = g_async_all; t; t = t->all_next) {
    if ((uintptr_t)t == raw && t->magic == RT_ASYNC_MAGIC)
      return t;
  }
  return NULL;
}

static void rt_async_task_free(rt_async_task *t) {
  if (!t)
    return;
  if (t->argv)
    free(t->argv);
  if (t->heap_buf)
    free(t->heap_buf);
  if (t->needle_buf)
    free(t->needle_buf);
  t->magic = 0;
  free(t);
}

static int64_t rt_async_find_bytes(const char *hay, int64_t hay_len, const char *needle,
                                   int64_t needle_len) {
  if (needle_len <= 0)
    return 0;
  if (!hay || !needle || hay_len < needle_len)
    return -1;
  for (int64_t i = 0; i <= hay_len - needle_len; ++i) {
    if (memcmp(hay + i, needle, (size_t)needle_len) == 0)
      return i;
  }
  return -1;
}

static bool rt_async_read_until_finish(rt_async_task *t, int64_t out_len) {
  if (!t)
    return false;
  if (out_len < 0)
    out_len = 0;
  if (out_len > t->heap_len)
    out_len = t->heap_len;
  int64_t s = rt_alloc_string_len(t->heap_buf ? t->heap_buf : "", (size_t)out_len);
  rt_async_complete(t, s ? s : rt_alloc_string_len("", 0));
  return true;
}

static bool rt_async_read_until_append(rt_async_task *t, const char *src, int64_t n) {
  if (!t || n <= 0)
    return true;
  if (t->heap_len + n > t->heap_cap) {
    int64_t next_cap = t->heap_cap > 0 ? t->heap_cap : 256;
    while (next_cap < t->heap_len + n)
      next_cap *= 2;
    if (next_cap > t->len)
      next_cap = t->len;
    if (next_cap < t->heap_len + n)
      return false;
    char *next = (char *)realloc(t->heap_buf, (size_t)next_cap);
    if (!next)
      return false;
    t->heap_buf = next;
    t->heap_cap = next_cap;
  }
  memcpy(t->heap_buf + t->heap_len, src, (size_t)n);
  t->heap_len += n;
  return true;
}

static void rt_async_complete(rt_async_task *t, int64_t result) {
  if (!t)
    return;
  t->result = result;
  t->state = RT_ASYNC_DONE;
}

static rt_async_task *rt_async_task_alloc(rt_async_kind kind) {
  rt_async_task *t = (rt_async_task *)calloc(1, sizeof(rt_async_task));
  if (!t)
    return NULL;
  t->magic = RT_ASYNC_MAGIC;
  t->kind = kind;
  t->state = RT_ASYNC_WAITING;
  t->result = 0;
  t->timeout_ms = -1;
  t->deadline_ms = -1;
  rt_async_all_add(t);
  return t;
}

static int rt_async_fd_ready(int64_t fd, int64_t events, int timeout_ms) {
  int rfd = (int)fd;
  if (rfd < 0)
    return -1;
#ifdef _WIN32
  fd_set rfds;
  fd_set wfds;
  FD_ZERO(&rfds);
  FD_ZERO(&wfds);
  if (events & RT_ASYNC_EV_READ)
    FD_SET((SOCKET)rfd, &rfds);
  if (events & RT_ASYNC_EV_WRITE)
    FD_SET((SOCKET)rfd, &wfds);
  struct timeval tv;
  tv.tv_sec = timeout_ms < 0 ? 0 : timeout_ms / 1000;
  tv.tv_usec = timeout_ms < 0 ? 0 : (timeout_ms % 1000) * 1000;
  int rc = select(0, &rfds, &wfds, NULL, timeout_ms < 0 ? NULL : &tv);
  if (rc <= 0)
    return rc;
  return 1;
#else
  struct pollfd pfd;
  memset(&pfd, 0, sizeof(pfd));
  pfd.fd = rfd;
  if (events & RT_ASYNC_EV_READ)
    pfd.events |= POLLIN;
  if (events & RT_ASYNC_EV_WRITE)
    pfd.events |= POLLOUT;
  int rc;
  do {
    rc = poll(&pfd, 1, timeout_ms);
  } while (rc < 0 && errno == EINTR);
  if (rc <= 0)
    return rc;
  if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
    return 1;
  return (pfd.revents & pfd.events) ? 1 : 0;
#endif
}

static void rt_async_close_fd(int64_t fd) {
#ifdef _WIN32
  closesocket((SOCKET)fd);
#else
  close((int)fd);
#endif
}

static int rt_async_set_nonblock(int64_t fd, int enabled, int *old_flags) {
#ifdef _WIN32
  u_long mode = enabled ? 1u : 0u;
  (void)old_flags;
  return ioctlsocket((SOCKET)fd, FIONBIO, &mode);
#else
  int flags = fcntl((int)fd, F_GETFL, 0);
  if (flags < 0)
    return -1;
  if (old_flags)
    *old_flags = flags;
  int next = enabled ? (flags | O_NONBLOCK) : (old_flags ? *old_flags : (flags & ~O_NONBLOCK));
  return fcntl((int)fd, F_SETFL, next);
#endif
}

static void rt_async_restore_blocking(int64_t fd, int old_flags) {
#ifdef _WIN32
  u_long mode = 0u;
  (void)old_flags;
  ioctlsocket((SOCKET)fd, FIONBIO, &mode);
#else
  if (old_flags >= 0)
    fcntl((int)fd, F_SETFL, old_flags);
#endif
}

static bool rt_async_would_block(void) {
#ifdef _WIN32
  int e = WSAGetLastError();
  return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEALREADY;
#else
  return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS || errno == EALREADY;
#endif
}

static int rt_async_socket_error(int64_t fd) {
  int err = 0;
#ifdef _WIN32
  int len = sizeof(err);
  if (getsockopt((SOCKET)fd, SOL_SOCKET, SO_ERROR, (char *)&err, &len) != 0)
    return WSAGetLastError();
#else
  socklen_t len = sizeof(err);
  if (getsockopt((int)fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0)
    return errno ? errno : -1;
#endif
  return err;
}

static bool rt_async_deadline_expired(rt_async_task *t, int64_t now) {
  return t && t->deadline_ms >= 0 && now >= t->deadline_ms;
}

static bool rt_async_progress_task(rt_async_task *t, int block, int wait_ms) {
  if (!t || t->magic != RT_ASYNC_MAGIC)
    return false;
  if (t->state == RT_ASYNC_RUNNING)
    return false;
  if (t->state == RT_ASYNC_DONE || t->state == RT_ASYNC_FAILED || t->state == RT_ASYNC_CANCELLED)
    return true;
  int64_t now = rt_async_now_ms();
  if (rt_async_deadline_expired(t, now) && t->kind != RT_ASYNC_TIMER) {
    rt_async_complete(t, rt_tag_v(-1));
    return true;
  }
  switch (t->kind) {
  case RT_ASYNC_CALL:
    t->state = RT_ASYNC_RUNNING;
    t->result = rt_thread_call_dispatch(t->fn, t->argc, t->argv);
    t->state = RT_ASYNC_DONE;
    return true;
  case RT_ASYNC_TIMER:
    if (t->deadline_ms <= now) {
      rt_async_complete(t, rt_tag_v(0));
      return true;
    }
    if (block) {
      int64_t delta = t->deadline_ms - now;
      if (delta > 0) {
#ifdef _WIN32
        Sleep((DWORD)delta);
#else
        struct timespec req;
        req.tv_sec = delta / 1000;
        req.tv_nsec = (delta % 1000) * 1000000;
        while (nanosleep(&req, &req) != 0 && errno == EINTR) {
        }
#endif
      }
      rt_async_complete(t, rt_tag_v(0));
      return true;
    }
    return false;
  case RT_ASYNC_WAIT_FD: {
    int timeout = block ? wait_ms : 0;
    int ready = rt_async_fd_ready(t->fd, t->events, timeout);
    if (ready > 0) {
      rt_async_complete(t, rt_tag_v(0));
      return true;
    }
    if (ready < 0) {
      rt_async_complete(t, rt_tag_v(-1));
      return true;
    }
    return false;
  }
  case RT_ASYNC_ACCEPT: {
    int ready = rt_async_fd_ready(t->fd, RT_ASYNC_EV_READ, block ? wait_ms : 0);
    if (ready <= 0)
      return false;
#ifdef _WIN32
    SOCKET s = accept((SOCKET)t->fd, NULL, NULL);
    if (s == INVALID_SOCKET) {
      if (rt_async_would_block())
        return false;
      rt_async_complete(t, rt_tag_v(-1));
    } else {
      rt_async_complete(t, rt_tag_v((int64_t)s));
    }
#else
    int s = accept((int)t->fd, NULL, NULL);
    if (s < 0) {
      if (rt_async_would_block())
        return false;
      rt_async_complete(t, rt_tag_v(-1));
    } else {
      rt_async_complete(t, rt_tag_v((int64_t)s));
    }
#endif
    return true;
  }
  case RT_ASYNC_CONNECT: {
    if (!t->connect_started) {
      t->old_flags = -1;
      rt_async_set_nonblock(t->fd, 1, &t->old_flags);
#ifdef _WIN32
      int rc = connect((SOCKET)t->fd, (const struct sockaddr *)t->addr, (int)t->addrlen);
#else
      int rc = connect((int)t->fd, (const struct sockaddr *)t->addr, (socklen_t)t->addrlen);
#endif
      t->connect_started = 1;
      if (rc == 0) {
        rt_async_restore_blocking(t->fd, t->old_flags);
        rt_async_complete(t, rt_tag_v(t->fd));
        return true;
      }
      if (!rt_async_would_block()) {
        rt_async_restore_blocking(t->fd, t->old_flags);
        rt_async_close_fd(t->fd);
        rt_async_complete(t, rt_tag_v(-1));
        return true;
      }
    }
    int ready = rt_async_fd_ready(t->fd, RT_ASYNC_EV_WRITE, block ? wait_ms : 0);
    if (ready <= 0)
      return false;
    int err = rt_async_socket_error(t->fd);
    rt_async_restore_blocking(t->fd, t->old_flags);
    if (err == 0) {
      rt_async_complete(t, rt_tag_v(t->fd));
    } else {
      rt_async_close_fd(t->fd);
      rt_async_complete(t, rt_tag_v(-1));
    }
    return true;
  }
  case RT_ASYNC_RECV: {
    int ready = rt_async_fd_ready(t->fd, RT_ASYNC_EV_READ, block ? wait_ms : 0);
    if (ready < 0) {
      rt_async_complete(t, rt_tag_v(-1));
      return true;
    }
    if (ready == 0)
      return false;
#ifdef _WIN32
    int r = recv((SOCKET)t->fd, (char *)(uintptr_t)t->buf, (int)t->len, (int)t->flags);
#else
    ssize_t r = recv((int)t->fd, (void *)(uintptr_t)t->buf, (size_t)t->len, (int)t->flags);
#endif
    if (r < 0 && rt_async_would_block())
      return false;
    rt_async_complete(t, rt_tag_v((int64_t)r));
    return true;
  }
  case RT_ASYNC_SEND:
  case RT_ASYNC_WRITE_ALL: {
    int ready = rt_async_fd_ready(t->fd, RT_ASYNC_EV_WRITE, block ? wait_ms : 0);
    if (ready < 0) {
      rt_async_complete(t, rt_tag_v(-1));
      return true;
    }
    if (ready == 0)
      return false;
    const char *base = (const char *)(uintptr_t)t->buf;
    int64_t remaining = t->len - t->off;
    if (remaining <= 0) {
      rt_async_complete(t, rt_tag_v(t->off));
      return true;
    }
#ifdef _WIN32
    int r = send((SOCKET)t->fd, base + t->off, (int)remaining, (int)t->flags);
#else
    ssize_t r = send((int)t->fd, base + t->off, (size_t)remaining, (int)t->flags);
#endif
    if (r < 0 && rt_async_would_block())
      return false;
    if (r <= 0) {
      rt_async_complete(t, rt_tag_v(t->off > 0 ? t->off : -1));
      return true;
    }
    t->off += r;
    if (t->kind == RT_ASYNC_SEND || t->off >= t->len) {
      rt_async_complete(t, rt_tag_v(t->kind == RT_ASYNC_SEND ? (int64_t)r : t->off));
      return true;
    }
    return false;
  }
  case RT_ASYNC_READ_SOCKET: {
    int ready = rt_async_fd_ready(t->fd, RT_ASYNC_EV_READ, block ? wait_ms : 0);
    if (ready <= 0)
      return false;
    int64_t max_len = t->len;
    if (max_len <= 0)
      max_len = 1;
    if (max_len > 1048576)
      max_len = 1048576;
    char *buf = (char *)malloc((size_t)max_len);
    if (!buf) {
      rt_async_complete(t, rt_alloc_string_len("", 0));
      return true;
    }
#ifdef _WIN32
    int r = recv((SOCKET)t->fd, buf, (int)max_len, 0);
#else
    ssize_t r = recv((int)t->fd, buf, (size_t)max_len, 0);
#endif
    if (r < 0 && rt_async_would_block()) {
      free(buf);
      return false;
    }
    if (r <= 0) {
      free(buf);
      rt_async_complete(t, rt_alloc_string_len("", 0));
      return true;
    }
    int64_t s = rt_alloc_string_len(buf, (size_t)r);
    free(buf);
    rt_async_complete(t, s ? s : rt_alloc_string_len("", 0));
    return true;
  }
  case RT_ASYNC_READ_UNTIL: {
    if (t->needle_len == 0)
      return rt_async_read_until_finish(t, 0);
    int64_t at = rt_async_find_bytes(t->heap_buf, t->heap_len, t->needle_buf, t->needle_len);
    if (at >= 0)
      return rt_async_read_until_finish(t, at + t->needle_len);
    if (t->heap_len >= t->len)
      return rt_async_read_until_finish(t, t->heap_len);
    int ready = rt_async_fd_ready(t->fd, RT_ASYNC_EV_READ, block ? wait_ms : 0);
    if (ready <= 0)
      return false;
    char tmp[4096];
    int64_t want = t->len - t->heap_len;
    if (want > (int64_t)sizeof(tmp))
      want = (int64_t)sizeof(tmp);
#ifdef _WIN32
    int r = recv((SOCKET)t->fd, tmp, (int)want, 0);
#else
    ssize_t r = recv((int)t->fd, tmp, (size_t)want, 0);
#endif
    if (r < 0 && rt_async_would_block())
      return false;
    if (r <= 0)
      return rt_async_read_until_finish(t, t->heap_len);
    if (!rt_async_read_until_append(t, tmp, (int64_t)r))
      return rt_async_read_until_finish(t, t->heap_len);
    at = rt_async_find_bytes(t->heap_buf, t->heap_len, t->needle_buf, t->needle_len);
    if (at >= 0)
      return rt_async_read_until_finish(t, at + t->needle_len);
    if (t->heap_len >= t->len)
      return rt_async_read_until_finish(t, t->heap_len);
    return false;
  }
  default:
    rt_async_complete(t, rt_tag_v(-1));
    return true;
  }
}

static int rt_async_compute_wait_ms(void) {
  int wait_ms = 10;
  int64_t now = rt_async_now_ms();
  for (rt_async_task *t = g_async_all; t; t = t->all_next) {
    if (t->state == RT_ASYNC_RUNNING || t->state == RT_ASYNC_DONE || t->state == RT_ASYNC_FAILED ||
        t->state == RT_ASYNC_CANCELLED)
      continue;
    if (t->kind == RT_ASYNC_TIMER && t->deadline_ms >= 0) {
      int64_t delta = t->deadline_ms - now;
      if (delta < 0)
        return 0;
      if (delta < wait_ms)
        wait_ms = (int)delta;
    } else if (t->deadline_ms >= 0) {
      int64_t delta = t->deadline_ms - now;
      if (delta < 0)
        return 0;
      if (delta < wait_ms)
        wait_ms = (int)delta;
    }
  }
  if (wait_ms < 0)
    wait_ms = 0;
  return wait_ms;
}

static int rt_async_scheduler_step(int block) {
  rt_async_task *ready = rt_async_ready_pop();
  if (ready) {
    rt_async_progress_task(ready, 0, 0);
    return 1;
  }
  for (rt_async_task *t = g_async_all; t; t = t->all_next) {
    if (t->state == RT_ASYNC_RUNNING || t->state == RT_ASYNC_DONE || t->state == RT_ASYNC_FAILED ||
        t->state == RT_ASYNC_CANCELLED)
      continue;
    if (rt_async_progress_task(t, 0, 0))
      return 1;
  }
  if (!block)
    return 0;
  int wait_ms = rt_async_compute_wait_ms();
  for (rt_async_task *t = g_async_all; t; t = t->all_next) {
    if (t->state == RT_ASYNC_RUNNING || t->state == RT_ASYNC_DONE || t->state == RT_ASYNC_FAILED ||
        t->state == RT_ASYNC_CANCELLED)
      continue;
    if (rt_async_progress_task(t, 1, wait_ms))
      return 1;
  }
  if (wait_ms > 0) {
#ifdef _WIN32
    Sleep((DWORD)wait_ms);
#else
    struct timespec req;
    req.tv_sec = wait_ms / 1000;
    req.tv_nsec = (wait_ms % 1000) * 1000000;
    while (nanosleep(&req, &req) != 0 && errno == EINTR) {
    }
#endif
  }
  return 0;
}

int64_t rt_async_task_new(int64_t fn, int64_t argc, int64_t argv_ptr) {
  int64_t argc_raw = 0;
  int64_t *argv_copy = NULL;
  if (!rt_thread_prepare_call_args(argc, argv_ptr, &argc_raw, &argv_copy))
    return 0;
  rt_async_task *t = rt_async_task_alloc(RT_ASYNC_CALL);
  if (!t) {
    free(argv_copy);
    return 0;
  }
  t->fn = fn;
  t->argc = argc_raw;
  t->argv = argv_copy;
  rt_async_ready_push(t);
  return (int64_t)(uintptr_t)t;
}

int64_t rt_async_value(int64_t value) {
  rt_async_task *t = rt_async_task_alloc(RT_ASYNC_TIMER);
  if (!t)
    return 0;
  rt_async_complete(t, value);
  return (int64_t)(uintptr_t)t;
}

int64_t rt_async_await_blocking(int64_t handle) {
  if (!handle)
    return 0;
  rt_async_task *t = rt_async_find_task(handle);
  if (!t)
    return handle;
  while (t->state != RT_ASYNC_DONE && t->state != RT_ASYNC_FAILED &&
         t->state != RT_ASYNC_CANCELLED) {
    rt_async_scheduler_step(1);
  }
  int64_t result = t->result;
  rt_async_all_remove(t);
  rt_async_task_free(t);
  return result;
}

int64_t rt_async_run(int64_t handle) { return rt_async_await_blocking(handle); }

int64_t rt_async_yield(void) {
  rt_async_task *t = rt_async_task_alloc(RT_ASYNC_TIMER);
  if (!t)
    return 0;
  t->deadline_ms = rt_async_now_ms();
  return (int64_t)(uintptr_t)t;
}

int64_t rt_async_sleep_ms(int64_t ms) {
  int64_t raw = rt_async_raw(ms);
  if (raw < 0)
    raw = 0;
  rt_async_task *t = rt_async_task_alloc(RT_ASYNC_TIMER);
  if (!t)
    return 0;
  t->deadline_ms = rt_async_now_ms() + raw;
  return (int64_t)(uintptr_t)t;
}

int64_t rt_async_wait_fd(int64_t fd, int64_t events, int64_t timeout_ms) {
  rt_async_task *t = rt_async_task_alloc(RT_ASYNC_WAIT_FD);
  if (!t)
    return 0;
  t->fd = rt_async_raw(fd);
  t->events = rt_async_raw(events);
  t->timeout_ms = rt_async_raw(timeout_ms);
  if (t->timeout_ms >= 0)
    t->deadline_ms = rt_async_now_ms() + t->timeout_ms;
  return (int64_t)(uintptr_t)t;
}

int64_t rt_async_recv(int64_t fd, int64_t buf, int64_t len, int64_t flags) {
  rt_async_task *t = rt_async_task_alloc(RT_ASYNC_RECV);
  if (!t)
    return 0;
  t->fd = rt_async_raw(fd);
  t->buf = rt_async_raw(buf);
  t->len = rt_async_raw(len);
  t->flags = rt_async_raw(flags);
  return (int64_t)(uintptr_t)t;
}

int64_t rt_async_send(int64_t fd, int64_t buf, int64_t len, int64_t flags) {
  rt_async_task *t = rt_async_task_alloc(RT_ASYNC_SEND);
  if (!t)
    return 0;
  t->fd = rt_async_raw(fd);
  t->buf = rt_async_raw(buf);
  t->len = rt_async_raw(len);
  t->flags = rt_async_raw(flags);
  return (int64_t)(uintptr_t)t;
}

int64_t rt_async_accept(int64_t fd) {
  rt_async_task *t = rt_async_task_alloc(RT_ASYNC_ACCEPT);
  if (!t)
    return 0;
  t->fd = rt_async_raw(fd);
  return (int64_t)(uintptr_t)t;
}

int64_t rt_async_connect(int64_t fd, int64_t addr, int64_t addrlen) {
  int64_t raw_len = rt_async_raw(addrlen);
  int64_t raw_addr = rt_async_raw(addr);
  if (raw_len <= 0 || raw_len > 128 || !raw_addr)
    return 0;
  rt_async_task *t = rt_async_task_alloc(RT_ASYNC_CONNECT);
  if (!t)
    return 0;
  t->fd = rt_async_raw(fd);
  t->addrlen = raw_len;
  memcpy(t->addr, (const void *)(uintptr_t)raw_addr, (size_t)raw_len);
  t->old_flags = -1;
  return (int64_t)(uintptr_t)t;
}

int64_t rt_async_read_socket(int64_t fd, int64_t max_len) {
  rt_async_task *t = rt_async_task_alloc(RT_ASYNC_READ_SOCKET);
  if (!t)
    return 0;
  t->fd = rt_async_raw(fd);
  t->len = rt_async_raw(max_len);
  return (int64_t)(uintptr_t)t;
}

int64_t rt_async_write_socket_part(int64_t fd, int64_t data, int64_t off, int64_t size) {
  int64_t raw_data = rt_async_raw(data);
  int64_t raw_off = rt_async_raw(off);
  int64_t raw_size = rt_async_raw(size);
  if (!raw_data || raw_off < 0)
    return 0;
  size_t slen = rt_tagged_str_len(data);
  if ((size_t)raw_off > slen)
    raw_off = (int64_t)slen;
  if (raw_size < 0 || (size_t)(raw_off + raw_size) > slen)
    raw_size = (int64_t)slen - raw_off;
  rt_async_task *t = rt_async_task_alloc(RT_ASYNC_SEND);
  if (!t)
    return 0;
  t->fd = rt_async_raw(fd);
  t->buf = raw_data + raw_off;
  t->len = raw_size;
  t->flags = 0;
  return (int64_t)(uintptr_t)t;
}

int64_t rt_async_write_socket_all(int64_t fd, int64_t data) {
  int64_t raw_data = rt_async_raw(data);
  if (!raw_data)
    return 0;
  rt_async_task *t = rt_async_task_alloc(RT_ASYNC_WRITE_ALL);
  if (!t)
    return 0;
  t->fd = rt_async_raw(fd);
  t->buf = raw_data;
  t->len = (int64_t)rt_tagged_str_len(data);
  t->flags = 0;
  return (int64_t)(uintptr_t)t;
}

int64_t rt_async_read_socket_until(int64_t fd, int64_t needle, int64_t max_bytes) {
  int64_t raw_needle = rt_async_raw(needle);
  if (!raw_needle)
    return rt_async_value(rt_alloc_string_len("", 0));
  int64_t max_len = rt_async_raw(max_bytes);
  if (max_len <= 0)
    max_len = 65536;
  if (max_len > 1048576)
    max_len = 1048576;
  size_t nlen = rt_tagged_str_len(needle);
  rt_async_task *t = rt_async_task_alloc(RT_ASYNC_READ_UNTIL);
  if (!t)
    return 0;
  t->fd = rt_async_raw(fd);
  t->len = max_len;
  t->needle_len = (int64_t)nlen;
  if (nlen > 0) {
    t->needle_buf = (char *)malloc(nlen);
    if (!t->needle_buf) {
      rt_async_complete(t, rt_alloc_string_len("", 0));
      return (int64_t)(uintptr_t)t;
    }
    memcpy(t->needle_buf, (const void *)(uintptr_t)raw_needle, nlen);
  }
  int64_t initial_cap = max_len < 256 ? max_len : 256;
  if (initial_cap < 1)
    initial_cap = 1;
  t->heap_buf = (char *)malloc((size_t)initial_cap);
  if (!t->heap_buf) {
    rt_async_complete(t, rt_alloc_string_len("", 0));
    return (int64_t)(uintptr_t)t;
  }
  t->heap_cap = initial_cap;
  return (int64_t)(uintptr_t)t;
}

int64_t rt_async_state_of(int64_t handle) {
  if (!handle)
    return rt_tag_v(-1);
  rt_async_task *t = rt_async_find_task(handle);
  if (!t)
    return rt_tag_v(-1);
  return rt_tag_v((int64_t)t->state);
}

#ifdef _WIN32
typedef struct rt_thread_state {
  HANDLE h;
  int64_t ret;
} rt_thread_state;

typedef struct rt_thread_arg {
  int64_t fn;
  int64_t arg;
  int64_t argc;
  int64_t *argv;
  rt_thread_state *st;
} rt_thread_arg;

static DWORD WINAPI rt_thread_trampoline(LPVOID p) {
  rt_thread_arg *ta = (rt_thread_arg *)p;
  rt_thread_state *st = ta->st;
  int64_t fn = ta->fn;
  int64_t arg = ta->arg;
  int64_t ret = 0;
  if (ta->argc >= 0) {
    ret = rt_thread_call_dispatch(fn, ta->argc, ta->argv);
  } else {
    if (NY_NATIVE_IS(fn)) {
      int64_t (*f)(int64_t) = (int64_t (*)(int64_t))NY_NATIVE_DECODE(fn);
      ret = rt_tag_v(f(rt_untag_v(arg)));
    } else if (is_heap_ptr(fn) && *(int64_t *)(rt_untag_v(fn) - 8) == TAG_CLOSURE) {
      int64_t base = rt_untag_v(fn);
      int64_t code = *(int64_t *)base;
      int64_t env = *(int64_t *)(base + 8);
      ret = ((int64_t (*)(int64_t, int64_t))code)(env, arg);
    } else {
      ret = ((int64_t (*)(int64_t))fn)(arg);
    }
  }
  if (st)
    st->ret = ret;
  if (ta->argv)
    free(ta->argv);
  free(ta);
  return 0;
}

int64_t rt_thread_spawn(int64_t fn, int64_t arg) {
  rt_thread_state *st = (rt_thread_state *)malloc(sizeof(rt_thread_state));
  if (!st)
    return -1;
  st->ret = 0;
  rt_thread_arg *ta = (rt_thread_arg *)malloc(sizeof(rt_thread_arg));
  if (!ta) {
    free(st);
    return -1;
  }
  ta->fn = fn;
  ta->arg = arg;
  ta->argc = -1;
  ta->argv = NULL;
  ta->st = st;
  HANDLE h = CreateThread(NULL, 0, rt_thread_trampoline, ta, 0, NULL);
  if (!h) {
    free(ta);
    free(st);
    return -1;
  }
  st->h = h;
  return (int64_t)(uintptr_t)st;
}

int64_t rt_thread_spawn_call(int64_t fn, int64_t argc, int64_t argv_ptr) {
  int64_t argc_raw = 0;
  int64_t *argv_copy = NULL;
  if (!rt_thread_prepare_call_args(argc, argv_ptr, &argc_raw, &argv_copy))
    return -1;
  rt_thread_state *st = (rt_thread_state *)malloc(sizeof(rt_thread_state));
  if (!st) {
    if (argv_copy)
      free(argv_copy);
    return -1;
  }
  st->ret = 0;
  rt_thread_arg *ta = (rt_thread_arg *)malloc(sizeof(rt_thread_arg));
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
  HANDLE h = CreateThread(NULL, 0, rt_thread_trampoline, ta, 0, NULL);
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

int64_t rt_thread_launch_call(int64_t fn, int64_t argc, int64_t argv_ptr) {
  int64_t argc_raw = 0;
  int64_t *argv_copy = NULL;
  if (!rt_thread_prepare_call_args(argc, argv_ptr, &argc_raw, &argv_copy))
    return rt_tag_v(-1);
  rt_thread_arg *ta = (rt_thread_arg *)malloc(sizeof(rt_thread_arg));
  if (!ta) {
    if (argv_copy)
      free(argv_copy);
    return rt_tag_v(-1);
  }
  ta->fn = fn;
  ta->arg = 0;
  ta->argc = argc_raw;
  ta->argv = argv_copy;
  ta->st = NULL;
  HANDLE h = CreateThread(NULL, 0, rt_thread_trampoline, ta, 0, NULL);
  if (!h) {
    free(ta);
    if (argv_copy)
      free(argv_copy);
    return rt_tag_v(-1);
  }
  CloseHandle(h);
  return rt_tag_v(0);
}

int64_t rt_thread_join(int64_t tid) {
  if (!tid)
    return -1;
  rt_thread_state *st = (rt_thread_state *)(uintptr_t)tid;
  WaitForSingleObject(st->h, INFINITE);
  CloseHandle(st->h);
  int64_t res = st->ret;
  free(st);
  return res;
}

int64_t rt_mutex_new(void) {
  HANDLE h = CreateMutexA(NULL, FALSE, NULL);
  return (int64_t)(uintptr_t)h;
}

int64_t rt_mutex_lock64(int64_t m) {
  if (!m)
    return -1;
  DWORD r = WaitForSingleObject((HANDLE)(uintptr_t)m, INFINITE);
  return (r == WAIT_OBJECT_0) ? 0 : -1;
}

int64_t rt_mutex_unlock64(int64_t m) {
  if (!m)
    return -1;
  return ReleaseMutex((HANDLE)(uintptr_t)m) ? 0 : -1;
}

int64_t rt_mutex_free(int64_t m) {
  if (!m)
    return 0;
  CloseHandle((HANDLE)(uintptr_t)m);
  return 0;
}
#else
typedef struct rt_thread_arg {
  int64_t fn;
  int64_t arg;
  int64_t argc;
  int64_t *argv;
} rt_thread_arg;

static void *rt_thread_trampoline(void *p) {
  rt_thread_arg *ta = (rt_thread_arg *)p;
  int64_t fn = ta->fn;
  int64_t arg = ta->arg;
  int64_t res = 0;
  if (ta->argc >= 0) {
    res = rt_thread_call_dispatch(fn, ta->argc, ta->argv);
  } else {
    if (NY_NATIVE_IS(fn)) {
      int64_t (*f)(int64_t) = (int64_t (*)(int64_t))NY_NATIVE_DECODE(fn);
      res = rt_tag_v(f(rt_untag_v(arg)));
    } else if (is_heap_ptr(fn) && *(int64_t *)(rt_untag_v(fn) - 8) == TAG_CLOSURE) {
      int64_t base = rt_untag_v(fn);
      int64_t code = *(int64_t *)base;
      int64_t env = *(int64_t *)(base + 8);
      res = ((int64_t (*)(int64_t, int64_t))code)(env, arg);
    } else {
      res = ((int64_t (*)(int64_t))fn)(arg);
    }
  }
  if (ta->argv)
    free(ta->argv);
  free(ta);
  return (void *)(uintptr_t)res;
}

int64_t rt_thread_spawn(int64_t fn, int64_t arg) {
  pthread_t tid;
  rt_thread_arg *ta = malloc(sizeof(rt_thread_arg));
  if (!ta)
    return -1;
  ta->fn = fn;
  ta->arg = arg;
  ta->argc = -1;
  ta->argv = NULL;
  int r = pthread_create(&tid, NULL, rt_thread_trampoline, ta);
  if (r != 0) {
    free(ta);
    return -r;
  }
  return (int64_t)tid;
}

int64_t rt_thread_spawn_call(int64_t fn, int64_t argc, int64_t argv_ptr) {
  int64_t argc_raw = 0;
  int64_t *argv_copy = NULL;
  if (!rt_thread_prepare_call_args(argc, argv_ptr, &argc_raw, &argv_copy))
    return -1;
  pthread_t tid;
  rt_thread_arg *ta = malloc(sizeof(rt_thread_arg));
  if (!ta) {
    if (argv_copy)
      free(argv_copy);
    return -1;
  }
  ta->fn = fn;
  ta->arg = 0;
  ta->argc = argc_raw;
  ta->argv = argv_copy;
  int r = pthread_create(&tid, NULL, rt_thread_trampoline, ta);
  if (r != 0) {
    if (argv_copy)
      free(argv_copy);
    free(ta);
    return -r;
  }
  return (int64_t)tid;
}

int64_t rt_thread_launch_call(int64_t fn, int64_t argc, int64_t argv_ptr) {
  int64_t argc_raw = 0;
  int64_t *argv_copy = NULL;
  if (!rt_thread_prepare_call_args(argc, argv_ptr, &argc_raw, &argv_copy))
    return rt_tag_v(-1);
  pthread_t tid;
  rt_thread_arg *ta = malloc(sizeof(rt_thread_arg));
  if (!ta) {
    if (argv_copy)
      free(argv_copy);
    return rt_tag_v(-1);
  }
  ta->fn = fn;
  ta->arg = 0;
  ta->argc = argc_raw;
  ta->argv = argv_copy;
  int r = pthread_create(&tid, NULL, rt_thread_trampoline, ta);
  if (r != 0) {
    if (argv_copy)
      free(argv_copy);
    free(ta);
    return rt_tag_v(-r);
  }
  r = pthread_detach(tid);
  if (r != 0)
    return rt_tag_v(-r);
  return rt_tag_v(0);
}

int64_t rt_thread_join(int64_t tid) {
  void *ret = NULL;
  int r = pthread_join((pthread_t)tid, &ret);
  if (r != 0)
    return -r;
  return (int64_t)(uintptr_t)ret;
}

int64_t rt_mutex_new(void) {
  pthread_mutex_t *m = calloc(1, sizeof(pthread_mutex_t));
  if (!m)
    return 0;
  if (pthread_mutex_init(m, NULL) != 0) {
    free(m);
    return 0;
  }
  return (int64_t)(uintptr_t)m;
}

int64_t rt_mutex_lock64(int64_t m) {
  if (!m)
    return -1;
  return pthread_mutex_lock((pthread_mutex_t *)(uintptr_t)m);
}

int64_t rt_mutex_unlock64(int64_t m) {
  if (!m)
    return -1;
  return pthread_mutex_unlock((pthread_mutex_t *)(uintptr_t)m);
}

int64_t rt_mutex_free(int64_t m) {
  if (!m)
    return 0;
  pthread_mutex_destroy((pthread_mutex_t *)(uintptr_t)m);
  free((void *)(uintptr_t)m);
  return 0;
}
#endif

int64_t rt_os_name(void) {
  static int64_t cached = 0;
  if (cached)
    return cached;
#if defined(__linux__)
  const char *s = "linux";
#elif defined(__APPLE__)
  const char *s = "macos";
#elif defined(rt_FreeBSD__)
  const char *s = "freebsd";
#elif defined(_WIN32)
  const char *s = "windows";
#else
  const char *s = "unknown";
#endif
  cached = rt_alloc_string(s);
  return cached;
}

int64_t rt_arch_name(void) {
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
  cached = rt_alloc_string(s);
  return cached;
}

int64_t rt_main(void) {
  return (getenv("NYTRIX_TEST_MODE") != NULL) ? NY_IMM_TRUE : NY_IMM_FALSE;
}
