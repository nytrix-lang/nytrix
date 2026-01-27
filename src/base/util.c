/**
 * @file util.c
 * @brief Unified utilities for Nytrix compiler (File I/O + String)
 */
#define _XOPEN_SOURCE 500
#include "base/util.h"
#include "base/common.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifdef __linux__
#include <linux/limits.h>
#endif

char *ny_read_file(const char *path) {
  if (!path)
    return NULL;
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  if (size < 0) {
    fclose(f);
    return NULL;
  }
  fseek(f, 0, SEEK_SET);
  char *content = malloc((size_t)size + 1);
  if (!content) {
    fclose(f);
    return NULL;
  }
  size_t read = fread(content, 1, (size_t)size, f);
  content[read] = '\0';
  fclose(f);

  // Skip shebang line if present (e.g., #!/bin/ny)
  if (read >= 2 && content[0] == '#' && content[1] == '!') {
    char *newline = strchr(content, '\n');
    if (newline) {
      // Move content past the shebang line
      size_t skip_len = (newline - content) + 1;
      size_t new_len = read - skip_len;
      memmove(content, newline + 1, new_len + 1); // +1 for null terminator
    }
  }

  return content;
}

int ny_write_file(const char *path, const char *content, size_t len) {
  if (!path || !content)
    return -1;
  FILE *f = fopen(path, "wb");
  if (!f)
    return -1;
  size_t written = fwrite(content, 1, len, f);
  fclose(f);
  return (written == len) ? 0 : -1;
}

int ny_ensure_dir(const char *path) {
  struct stat st = {0};
  if (stat(path, &st) == -1) {
    return mkdir(path, 0755);
  }
  return 0;
}

int ny_copy_file(const char *src, const char *dst) {
  FILE *in = fopen(src, "rb");
  if (!in)
    return -1;
  FILE *out = fopen(dst, "wb");
  if (!out) {
    fclose(in);
    return -1;
  }
  char buf[8192];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
    if (fwrite(buf, 1, n, out) != n) {
      fclose(in);
      fclose(out);
      return -1;
    }
  }
  fclose(in);
  fclose(out);
  return 0;
}

void ny_write_text_file(const char *path, const char *contents) {
  if (!path || !contents)
    return;
  ny_write_file(path, contents, strlen(contents));
}

static bool nytrix_has_sources(const char *root) {
  char probe[8192];
  // Check for the runtime include header as a sign of source presence
  snprintf(probe, sizeof(probe), "%s/src/rt/runtime.h", root);
  return access(probe, R_OK) == 0;
}

char *ny_get_executable_dir(void) {
  static char buf[PATH_MAX];
  if (buf[0])
    return buf;
  ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (len != -1) {
    buf[len] = '\0';
    char *slash = strrchr(buf, '/');
    if (slash)
      *slash = '\0';
    return buf;
  }
  return NULL;
}

const char *ny_src_root(void) {
  static char buf[PATH_MAX];
  if (buf[0])
    return buf;
  const char *env = getenv("NYTRIX_ROOT");
  if (env && *env && nytrix_has_sources(env)) {
    snprintf(buf, sizeof(buf), "%s", env);
    return buf;
  }
  char cwd[PATH_MAX];
  if (getcwd(cwd, sizeof(cwd))) {
    char cur[PATH_MAX];
    snprintf(cur, sizeof(cur), "%s", cwd);
    for (;;) {
      if (nytrix_has_sources(cur)) {
        snprintf(buf, sizeof(buf), "%s", cur);
        return buf;
      }
      char *slash = strrchr(cur, '/');
      if (!slash || slash == cur)
        break;
      *slash = '\0';
    }
  }

  char *exe_dir = ny_get_executable_dir();
  if (exe_dir) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", exe_dir);
    size_t len = strlen(tmp);
    if (len >= 6 && strcmp(tmp + len - 6, "/build") == 0) {
      tmp[len - 6] = '\0';
    }
    if (nytrix_has_sources(tmp)) {
      snprintf(buf, sizeof(buf), "%s", tmp);
      return buf;
    }
  }

  // Fallback to standard installation paths
  const char *install_paths[] = {"/usr/share/nytrix", "/usr/local/share/nytrix",
                                 "/opt/nytrix/share"};
  for (size_t i = 0; i < sizeof(install_paths) / sizeof(install_paths[0]);
       i++) {
    if (nytrix_has_sources(install_paths[i])) {
      snprintf(buf, sizeof(buf), "%s", install_paths[i]);
      return buf;
    }
  }

  snprintf(buf, sizeof(buf), ".");
  return buf;
}

// --- String Utils ---

char *ny_strdup(const char *s) {
  if (!s)
    return NULL;
  size_t len = strlen(s);
  char *copy = malloc(len + 1);
  if (!copy)
    return NULL;
  memcpy(copy, s, len + 1);
  return copy;
}

// ny_strndup is static inline in common.h

void ny_str_list_append(char ***list, size_t *len, size_t *cap,
                        const char *str) {
  if (*len == *cap) {
    size_t new_cap = *cap ? (*cap * 2) : 8;
    char **tmp = realloc(*list, new_cap * sizeof(char *));
    if (!tmp) {
      fprintf(stderr, "OOM in str_list_append\n");
      exit(1);
    }
    *list = tmp;
    *cap = new_cap;
  }
  (*list)[(*len)++] = ny_strdup(str);
}

void ny_str_list_free(char **list, size_t count) {
  if (!list)
    return;
  for (size_t i = 0; i < count; ++i) {
    free(list[i]);
  }
  free(list);
}

uint64_t ny_fnv1a64(const void *data, size_t len, uint64_t seed) {
  const uint8_t *p = (const uint8_t *)data;
  uint64_t h = seed ? seed : 14695981039346656037ULL;
  const uint64_t prime = 1099511628211ULL;
  for (size_t i = 0; i < len; ++i) {
    h ^= p[i];
    h *= prime;
  }
  return h;
}

int ny_levenshtein(const char *s1, const char *s2) {
  size_t l1 = strlen(s1);
  size_t l2 = strlen(s2);
  if (l1 == 0)
    return l2;
  if (l2 == 0)
    return l1;

  int *v0 = malloc((l2 + 1) * sizeof(int));
  int *v1 = malloc((l2 + 1) * sizeof(int));
  if (!v0 || !v1)
    exit(1);

  for (size_t i = 0; i <= l2; i++)
    v0[i] = i;

  for (size_t i = 0; i < l1; i++) {
    v1[0] = i + 1;
    for (size_t j = 0; j < l2; j++) {
      int cost = (s1[i] == s2[j]) ? 0 : 1;
      int del = v0[j + 1] + 1;
      int ins = v1[j] + 1;
      int sub = v0[j] + cost;
      int min = del;
      if (ins < min)
        min = ins;
      if (sub < min)
        min = sub;
      v1[j + 1] = min;
    }
    for (size_t j = 0; j <= l2; j++)
      v0[j] = v1[j];
  }

  int res = v0[l2];
  free(v0);
  free(v1);
  return res;
}
