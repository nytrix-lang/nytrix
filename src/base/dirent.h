#pragma once

#ifdef _WIN32
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#ifndef MAX_PATH
#define MAX_PATH 260
#endif

struct dirent {
  char d_name[MAX_PATH];
};

typedef struct DIR {
  HANDLE handle;
  WIN32_FIND_DATAA data;
  int first;
  struct dirent ent;
  char pattern[MAX_PATH * 2];
} DIR;

static inline void ny_dirent_make_pattern(char *out, size_t cap,
                                          const char *path) {
  size_t n = 0;
  if (!path || !*path) {
    strncpy(out, ".", cap - 1);
    out[cap - 1] = '\0';
    n = strlen(out);
  } else {
    strncpy(out, path, cap - 1);
    out[cap - 1] = '\0';
    n = strlen(out);
  }
  for (size_t i = 0; i < n; ++i) {
    if (out[i] == '/')
      out[i] = '\\';
  }
  if (n > 0 && out[n - 1] != '\\') {
    if (n + 1 < cap) {
      out[n++] = '\\';
      out[n] = '\0';
    }
  }
  if (n + 1 < cap) {
    out[n++] = '*';
    out[n] = '\0';
  }
}

static inline DIR *opendir(const char *path) {
  DIR *d = (DIR *)calloc(1, sizeof(DIR));
  if (!d)
    return NULL;
  ny_dirent_make_pattern(d->pattern, sizeof(d->pattern), path);
  d->handle = FindFirstFileA(d->pattern, &d->data);
  if (d->handle == INVALID_HANDLE_VALUE) {
    free(d);
    return NULL;
  }
  d->first = 1;
  return d;
}

static inline struct dirent *readdir(DIR *d) {
  if (!d)
    return NULL;
  if (d->first) {
    d->first = 0;
  } else {
    if (!FindNextFileA(d->handle, &d->data))
      return NULL;
  }
  strncpy(d->ent.d_name, d->data.cFileName, sizeof(d->ent.d_name) - 1);
  d->ent.d_name[sizeof(d->ent.d_name) - 1] = '\0';
  return &d->ent;
}

static inline int closedir(DIR *d) {
  if (!d)
    return -1;
  if (d->handle != INVALID_HANDLE_VALUE)
    FindClose(d->handle);
  free(d);
  return 0;
}

#else
#if defined(__GNUC__) || defined(__clang__)
#include_next <dirent.h>
#else
#include <dirent.h>
#endif
#endif
