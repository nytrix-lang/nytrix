/**
 * @file util.c
 * @brief Unified utilities for Nytrix compiler (File I/O + String)
 */
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif
#define _XOPEN_SOURCE 500
#include "base/util.h"
#include "base/common.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <windows.h>
#define access _access
#else
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifdef __linux__
#include <linux/limits.h>
#endif

static int ny_is_sep(char c) { return c == '/' || c == '\\'; }
static const char *ny_first_nonempty_env(const char *const *names,
                                         size_t name_count) {
  if (!names)
    return NULL;
  for (size_t i = 0; i < name_count; ++i) {
    const char *v = getenv(names[i]);
    if (v && *v)
      return v;
  }
  return NULL;
}
static const char *ny_cache_path(char *buf, size_t cap, const char *path) {
  if (!buf || cap == 0 || !path)
    return NULL;
  snprintf(buf, cap, "%s", path);
  return buf;
}
static char *ny_last_sep(char *s) {
  char *a = strrchr(s, '/');
  char *b = strrchr(s, '\\');
  if (!a)
    return b;
  if (!b)
    return a;
  return (a > b) ? a : b;
}

void ny_join_path(char *out, size_t out_len, const char *dir,
                  const char *name) {
  if (!out || out_len == 0)
    return;
  if (!dir || !*dir) {
    snprintf(out, out_len, "%s", name ? name : "");
    return;
  }
  size_t dlen = strlen(dir);
  int needs_sep = 1;
  if (dlen > 0 && ny_is_sep(dir[dlen - 1]))
    needs_sep = 0;
  if (needs_sep)
    snprintf(out, out_len, "%s/%s", dir, name ? name : "");
  else
    snprintf(out, out_len, "%s%s", dir, name ? name : "");
}

const char *ny_get_temp_dir(void) {
  static char buf[PATH_MAX];
  if (buf[0])
    return buf;
#ifdef _WIN32
  DWORD n = GetTempPathA(sizeof(buf), buf);
  if (n > 0 && n < sizeof(buf)) {
    return buf;
  }
  {
    static const char *envs[] = {"TEMP", "TMP"};
    const char *t = ny_first_nonempty_env(envs, sizeof(envs) / sizeof(envs[0]));
    if (t)
      return ny_cache_path(buf, sizeof(buf), t);
  }
  return ny_cache_path(buf, sizeof(buf), "C:\\\\Temp");
#else
  {
    static const char *envs[] = {"TMPDIR", "TMP", "TEMP"};
    const char *t = ny_first_nonempty_env(envs, sizeof(envs) / sizeof(envs[0]));
    if (t)
      return ny_cache_path(buf, sizeof(buf), t);
  }
  return ny_cache_path(buf, sizeof(buf), "/tmp");
#endif
}
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
#ifdef _WIN32
    return _mkdir(path);
#else
    return mkdir(path, 0755);
#endif
  }
  return 0;
}

int ny_copy_file(const char *src, const char *dst) {
  struct stat src_st;
  int have_src_mode = (stat(src, &src_st) == 0);
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
#ifdef _WIN32
  if (have_src_mode) {
    int mode = 0;
#ifdef _S_IREAD
    if (src_st.st_mode & _S_IREAD)
      mode |= _S_IREAD;
#endif
#ifdef _S_IWRITE
    if (src_st.st_mode & _S_IWRITE)
      mode |= _S_IWRITE;
#endif
#ifdef _S_IEXEC
    if (src_st.st_mode & _S_IEXEC)
      mode |= _S_IEXEC;
#endif
    if (mode)
      (void)_chmod(dst, mode);
  }
#else
  if (have_src_mode) {
    /* Preserve executable bits so cached native artifacts remain runnable. */
    (void)chmod(dst, src_st.st_mode & 07777);
  }
#endif
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
#ifdef _WIN32
  char tmp[PATH_MAX];
  DWORD len = GetModuleFileNameA(NULL, tmp, sizeof(tmp));
  if (len > 0 && len < sizeof(tmp)) {
    tmp[len] = '\0';
    char *slash = strrchr(tmp, '\\');
    if (!slash)
      slash = strrchr(tmp, '/');
    if (slash)
      *slash = '\0';
    snprintf(buf, sizeof(buf), "%s", tmp);
    return buf;
  }
#elif defined(__APPLE__)
  uint32_t size = (uint32_t)sizeof(buf);
  if (_NSGetExecutablePath(buf, &size) == 0) {
    char *slash = strrchr(buf, '/');
    if (slash)
      *slash = '\0';
    return buf;
  }
#else
  ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (len != -1) {
    buf[len] = '\0';
    char *slash = strrchr(buf, '/');
    if (slash)
      *slash = '\0';
    return buf;
  }
#endif
  return NULL;
}

const char *ny_src_root(void) {
  static char buf[PATH_MAX];
  if (buf[0])
    return buf;
  const char *env = getenv("NYTRIX_ROOT");
  if (env && *env && nytrix_has_sources(env)) {
    return ny_cache_path(buf, sizeof(buf), env);
  }
  char cwd[PATH_MAX];
  if (getcwd(cwd, sizeof(cwd))) {
    char cur[PATH_MAX];
    snprintf(cur, sizeof(cur), "%s", cwd);
    for (;;) {
      if (nytrix_has_sources(cur)) {
        return ny_cache_path(buf, sizeof(buf), cur);
      }
      char *slash = ny_last_sep(cur);
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
    if (len >= 6 && ny_is_sep(tmp[len - 6]) &&
        strcmp(tmp + len - 5, "build") == 0) {
      tmp[len - 6] = '\0';
    }
    if (nytrix_has_sources(tmp)) {
      return ny_cache_path(buf, sizeof(buf), tmp);
    }

    /*
     * Installed layout fallback:
     *   <prefix>/bin/ny -> <prefix>/share/nytrix/src/...
     * This keeps ny usable from any working directory across custom prefixes.
     */
    snprintf(tmp, sizeof(tmp), "%s/../share/nytrix", exe_dir);
    if (nytrix_has_sources(tmp)) {
      return ny_cache_path(buf, sizeof(buf), tmp);
    }
  }

#ifndef _WIN32
  const char *install_paths[] = {"/usr/share/nytrix", "/usr/local/share/nytrix",
                                 "/opt/nytrix/share",
                                 "/opt/homebrew/share/nytrix"};
  for (size_t i = 0; i < sizeof(install_paths) / sizeof(install_paths[0]);
       i++) {
    if (nytrix_has_sources(install_paths[i])) {
      return ny_cache_path(buf, sizeof(buf), install_paths[i]);
    }
  }
#else
  const char *pd = getenv("PROGRAMDATA");
  if (pd && *pd) {
    char tmp[PATH_MAX];
    ny_join_path(tmp, sizeof(tmp), pd, "nytrix");
    if (nytrix_has_sources(tmp)) {
      return ny_cache_path(buf, sizeof(buf), tmp);
    }
  }
#endif

  return ny_cache_path(buf, sizeof(buf), ".");
}

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

bool ny_env_is_truthy(const char *v) {
  if (!v || !*v)
    return false;
  if (strcmp(v, "0") == 0 || strcmp(v, "false") == 0 ||
      strcmp(v, "False") == 0 || strcmp(v, "FALSE") == 0 ||
      strcmp(v, "off") == 0 || strcmp(v, "OFF") == 0 || strcmp(v, "no") == 0 ||
      strcmp(v, "NO") == 0) {
    return false;
  }
  return true;
}

bool ny_env_enabled(const char *name) {
  if (!name || !*name)
    return false;
  return ny_env_is_truthy(getenv(name));
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
  uint64_t h = seed ? seed : NY_FNV1A64_OFFSET_BASIS;
  const uint64_t prime = NY_FNV1A64_PRIME;
  for (size_t i = 0; i < len; ++i) {
    h ^= p[i];
    h *= prime;
  }
  return h;
}

uint64_t ny_fnv1a64_cstr(const char *s, uint64_t seed) {
  if (!s)
    return seed ? seed : NY_FNV1A64_OFFSET_BASIS;
  return ny_fnv1a64(s, strlen(s), seed);
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
