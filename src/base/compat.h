#pragma once

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif
#ifdef __APPLE__
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#include <BaseTsd.h>
#include <direct.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <unistd.h>
#ifdef __APPLE__
#include <mach/mach_time.h>
#endif
#endif

#ifndef _WIN32
extern char *realpath(const char *path, char *resolved_path);
extern int setenv(const char *name, const char *value, int overwrite);
extern int unsetenv(const char *name);
#ifdef __APPLE__
extern int sysctlbyname(const char *name, void *oldp, size_t *oldlenp, void *newp,
                        size_t newlen);
#endif
#endif

#ifdef _WIN32
typedef SSIZE_T ssize_t;
#endif

#ifndef STDIN_FILENO
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifdef _WIN32
#ifndef strcasecmp
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#endif

#ifndef strdup
#define strdup _strdup
#endif

#ifndef setlinebuf
#define setlinebuf(stream) setvbuf((stream), NULL, _IONBF, 0)
#endif

#ifndef R_OK
#define R_OK 4
#endif
#ifndef W_OK
#define W_OK 2
#endif
#ifndef X_OK
#define X_OK 1
#endif
#ifndef F_OK
#define F_OK 0
#endif
#endif

typedef uint64_t ny_tick_t;

static inline int ny_setenv(const char *name, const char *value, int overwrite) {
  if (!name || !*name)
    return -1;
#ifdef _WIN32
  if (!overwrite && getenv(name))
    return 0;
  return _putenv_s(name, value ? value : "");
#else
  return setenv(name, value ? value : "", overwrite);
#endif
}

static inline int ny_unsetenv(const char *name) {
  if (!name || !*name)
    return -1;
#ifdef _WIN32
  return _putenv_s(name, "");
#else
  return unsetenv(name);
#endif
}

static inline int ny_access(const char *path, int mode) {
  if (!path)
    return -1;
#ifdef _WIN32
  int win_mode = mode;
  if (win_mode & X_OK)
    win_mode = (win_mode & ~X_OK) | F_OK;
  return _access(path, win_mode);
#else
  return access(path, mode);
#endif
}

static inline char *ny_realpath(const char *path, char *resolved) {
  if (!path)
    return NULL;
#ifdef _WIN32
  return _fullpath(resolved, path, PATH_MAX);
#else
  return realpath(path, resolved);
#endif
}

static inline const void *ny_memmem(const void *haystack, size_t haystack_len, const void *needle,
                                    size_t needle_len) {
  if (!haystack || !needle)
    return NULL;
  if (needle_len == 0)
    return haystack;
  if (needle_len > haystack_len)
    return NULL;
  const unsigned char *h = (const unsigned char *)haystack;
  const unsigned char *n = (const unsigned char *)needle;
  const unsigned char *cur = h;
  size_t remaining = haystack_len;
  while (remaining >= needle_len) {
    const void *hit = memchr(cur, n[0], remaining - needle_len + 1);
    if (!hit)
      return NULL;
    cur = (const unsigned char *)hit;
    if (memcmp(cur, n, needle_len) == 0)
      return cur;
    remaining = haystack_len - (size_t)((cur + 1) - h);
    cur++;
  }
  return NULL;
}

static inline long ny_cpu_count(void) {
#ifdef _WIN32
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  return si.dwNumberOfProcessors > 0 ? (long)si.dwNumberOfProcessors : 1;
#elif defined(__APPLE__)
  int ncpu = 0;
  size_t len = sizeof(ncpu);
  if (sysctlbyname("hw.logicalcpu", &ncpu, &len, NULL, 0) == 0 && ncpu > 0)
    return (long)ncpu;
  return 1;
#elif defined(_SC_NPROCESSORS_ONLN)
  long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
  return ncpu > 0 ? ncpu : 1;
#else
  return 1;
#endif
}

static inline long ny_page_size(void) {
#ifdef _WIN32
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  return si.dwPageSize > 0 ? (long)si.dwPageSize : 4096;
#elif defined(_SC_PAGESIZE)
  long ps = sysconf(_SC_PAGESIZE);
  return ps > 0 ? ps : 4096;
#else
  return 4096;
#endif
}

static inline ny_tick_t ny_ticks_now(void) {
#ifdef _WIN32
  LARGE_INTEGER counter;
  QueryPerformanceCounter(&counter);
  return (ny_tick_t)counter.QuadPart;
#elif defined(__APPLE__)
  return (ny_tick_t)mach_absolute_time();
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (ny_tick_t)ts.tv_sec * 1000000000ull + (ny_tick_t)ts.tv_nsec;
#endif
}

static inline double ny_ticks_elapsed_sec(ny_tick_t start) {
  ny_tick_t end = ny_ticks_now();
#ifdef _WIN32
  static LARGE_INTEGER freq;
  if (freq.QuadPart == 0)
    QueryPerformanceFrequency(&freq);
  if (freq.QuadPart <= 0)
    return 0.0;
  return (double)(end - start) / (double)freq.QuadPart;
#elif defined(__APPLE__)
  static mach_timebase_info_data_t info;
  if (info.denom == 0)
    mach_timebase_info(&info);
  uint64_t delta = end - start;
  double nanos = (double)delta * (double)info.numer / (double)info.denom;
  return nanos / 1000000000.0;
#else
  return (double)(end - start) / 1000000000.0;
#endif
}

static inline double ny_ticks_delta_sec(ny_tick_t start, ny_tick_t end) {
#ifdef _WIN32
  static LARGE_INTEGER freq;
  if (freq.QuadPart == 0)
    QueryPerformanceFrequency(&freq);
  if (freq.QuadPart <= 0)
    return 0.0;
  return (double)(end - start) / (double)freq.QuadPart;
#elif defined(__APPLE__)
  static mach_timebase_info_data_t info;
  if (info.denom == 0)
    mach_timebase_info(&info);
  uint64_t delta = end - start;
  double nanos = (double)delta * (double)info.numer / (double)info.denom;
  return nanos / 1000000000.0;
#else
  return (double)(end - start) / 1000000000.0;
#endif
}

static inline double ny_ticks_elapsed_ms(ny_tick_t start) {
  return ny_ticks_elapsed_sec(start) * 1000.0;
}

static inline double ny_ticks_delta_ms(ny_tick_t start, ny_tick_t end) {
  return ny_ticks_delta_sec(start, end) * 1000.0;
}

#ifdef _WIN32
#define isatty _isatty
#define fileno _fileno
#define unlink _unlink
#define chdir _chdir
#define getcwd _getcwd
#define read _read
#define write _write
#define fsync _commit
#define getpid _getpid

#ifndef S_ISDIR
#define S_ISDIR(mode) (((mode) & _S_IFMT) == _S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(mode) (((mode) & _S_IFMT) == _S_IFREG)
#endif
#endif
