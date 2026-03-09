#pragma once

#include "base/compat.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static inline int nyt_is_file(const char *path) {
  struct stat st;
  return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static inline int nyt_ends_with(const char *s, const char *suf) {
  size_t n = strlen(s), m = strlen(suf);
  return n >= m && memcmp(s + (n - m), suf, m) == 0;
}

static inline int nyt_env_truthy(const char *name) {
  const char *v = getenv(name);
  if (!v || !*v)
    return 0;
  return strcmp(v, "0") != 0 && strcasecmp(v, "false") != 0 && strcasecmp(v, "off") != 0 &&
         strcasecmp(v, "no") != 0;
}

static inline void nyt_path_copy(char *out, size_t out_sz, const char *s) {
  if (!out || out_sz == 0)
    return;
  size_t n = s ? strlen(s) : 0;
  if (n >= out_sz)
    n = out_sz - 1;
  if (n > 0)
    memcpy(out, s, n);
  out[n] = '\0';
}

static inline void nyt_path_join(char *out, size_t out_sz, const char *a, const char *b) {
  if (!out || out_sz == 0)
    return;
  if (!a || !*a) {
    nyt_path_copy(out, out_sz, b ? b : "");
    return;
  }
  if (!b || !*b) {
    nyt_path_copy(out, out_sz, a);
    return;
  }
  size_t al = strlen(a);
  size_t bl = strlen(b);
  int slash = a[al - 1] != '/';
  size_t n = 0;
  if (al >= out_sz)
    al = out_sz - 1;
  memcpy(out, a, al);
  n = al;
  if (slash && n + 1 < out_sz)
    out[n++] = '/';
  size_t room = (n < out_sz) ? out_sz - n - 1 : 0;
  if (bl > room)
    bl = room;
  if (bl > 0)
    memcpy(out + n, b, bl);
  out[n + bl] = '\0';
}

static inline int nyt_ensure_repo_root_by_marker(char *out, size_t out_sz, const char *marker_file) {
  const char *env = getenv("NYTRIX_ROOT");
  if (env && *env) {
    snprintf(out, out_sz, "%s", env);
    return 1;
  }

  char cur[PATH_MAX];
  if (!getcwd(cur, sizeof(cur) - 1))
    return 0;

  for (;;) {
    char probe[PATH_MAX];
    size_t marker_n = marker_file ? strlen(marker_file) : 0;
    if (marker_n == 0 || strlen(cur) + marker_n + 2 >= sizeof(probe))
      return 0;
    snprintf(probe, sizeof(probe), "%s/%s", cur, marker_file);
    if (nyt_is_file(probe)) {
      snprintf(out, out_sz, "%s", cur);
      ny_setenv("NYTRIX_ROOT", cur, 1);
      return 1;
    }
    size_t n = strlen(cur);
    while (n > 0 && cur[n - 1] == '/')
      cur[--n] = '\0';
    while (n > 0 && cur[n - 1] != '/')
      cur[--n] = '\0';
    while (n > 0 && cur[n - 1] == '/')
      cur[--n] = '\0';
    if (n == 0)
      break;
  }
  return 0;
}

static inline int nyt_ensure_repo_root_cmake(char *out, size_t out_sz) {
  return nyt_ensure_repo_root_by_marker(out, out_sz, "CMakeLists.txt");
}
