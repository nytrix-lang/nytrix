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

bool ny_extract_line(const char *src, int line, const char **out_start,
                     size_t *out_len) {
  if (!src || line <= 0 || !out_start || !out_len)
    return false;
  const char *cur = src;
  int cur_line = 1;
  while (*cur && cur_line < line) {
    if (*cur == '\n')
      cur_line++;
    cur++;
  }
  if (cur_line != line)
    return false;
  const char *start = cur;
  while (*cur && *cur != '\n')
    cur++;
  *out_start = start;
  *out_len = (size_t)(cur - start);
  return true;
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
  if (read >= 2 && content[0] == '#' && content[1] == '!') {
    char *newline = strchr(content, '\n');
    if (newline) {
      size_t skip_len = (size_t)(newline - content) + 1;
      if (skip_len < read) {
        size_t new_len = read - skip_len;
        memmove(content, newline + 1, new_len);
        content[new_len] = '\0';
      } else {
        content[0] = '\0';
      }
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

bool ny_write_if_changed(const char *path, const char *content, size_t len) {
  if (!path || !content)
    return false;
  char *old = ny_read_file(path);
  if (old) {
    size_t old_len = strlen(old);
    if (old_len == len && memcmp(old, content, len) == 0) {
      free(old);
      return false;
    }
    free(old);
  }
  return ny_write_file(path, content, len) == 0;
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

bool ny_env_enabled_default_on(const char *name) {
  const char *v = getenv(name);
  if (!v || !*v)
    return true;
  return ny_env_is_truthy(v);
}
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
  size_t i = 0;
  while (i + 8 <= len) {
    h ^= p[i + 0];
    h *= prime;
    h ^= p[i + 1];
    h *= prime;
    h ^= p[i + 2];
    h *= prime;
    h ^= p[i + 3];
    h *= prime;
    h ^= p[i + 4];
    h *= prime;
    h ^= p[i + 5];
    h *= prime;
    h ^= p[i + 6];
    h *= prime;
    h ^= p[i + 7];
    h *= prime;
    i += 8;
  }
  for (; i < len; ++i) {
    h ^= p[i];
    h *= prime;
  }
  return h;
}

uint64_t ny_fnv1a64_cstr(const char *s, uint64_t seed) {
  if (!s)
    return seed ? seed : NY_FNV1A64_OFFSET_BASIS;
  const unsigned char *p = (const unsigned char *)s;
  uint64_t h = seed ? seed : NY_FNV1A64_OFFSET_BASIS;
  const uint64_t prime = NY_FNV1A64_PRIME;
  for (;;) {
    const unsigned char *z = (const unsigned char *)memchr(p, 0, 8);
    if (z) {
      while (p < z) {
        h ^= *p++;
        h *= prime;
      }
      return h;
    }
    h ^= p[0];
    h *= prime;
    h ^= p[1];
    h *= prime;
    h ^= p[2];
    h *= prime;
    h ^= p[3];
    h *= prime;
    h ^= p[4];
    h *= prime;
    h ^= p[5];
    h *= prime;
    h ^= p[6];
    h *= prime;
    h ^= p[7];
    h *= prime;
    p += 8;
  }
}

static inline uint64_t ny_fasthash_mix(uint64_t h) {
  h ^= h >> 23;
  h *= 0x2127599bf4325c37ULL;
  h ^= h >> 47;
  return h;
}

uint64_t ny_hash64_fast(const void *data, size_t len) {
  const uint64_t m = 0x880355f21e6d1965ULL;
  const uint8_t *pos = (const uint8_t *)data;
  const uint8_t *end = pos + (len & ~((size_t)7));
  uint64_t h = 0xcbf29ce484222325ULL ^ (len * m);
  while (pos < end) {
    uint64_t v;
    memcpy(&v, pos, sizeof(v));
    h ^= ny_fasthash_mix(v);
    h *= m;
    pos += 8;
  }
  uint64_t v = 0;
  switch (len & 7) {
  case 7:
    v ^= (uint64_t)pos[6] << 48;
  case 6:
    v ^= (uint64_t)pos[5] << 40;
  case 5:
    v ^= (uint64_t)pos[4] << 32;
  case 4:
    v ^= (uint64_t)pos[3] << 24;
  case 3:
    v ^= (uint64_t)pos[2] << 16;
  case 2:
    v ^= (uint64_t)pos[1] << 8;
  case 1:
    v ^= (uint64_t)pos[0];
    h ^= ny_fasthash_mix(v);
    h *= m;
  }
  return ny_fasthash_mix(h);
}

uint64_t ny_hash64_fast_cstr(const char *s) {
  if (!s)
    return 0;
  return ny_hash64_fast(s, strlen(s));
}

int ny_levenshtein(const char *s1, const char *s2) {
  size_t l1 = strlen(s1);
  size_t l2 = strlen(s2);

  if (l1 < l2) {
    return ny_levenshtein(s2, s1);
  }

  if (l2 == 0)
    return l1;

  int stack_v[2048];
  int *v = stack_v;
  bool v_heap = false;

  if (l2 + 1 > 2048) {
    v = malloc((l2 + 1) * sizeof(int));
    if (!v)
      exit(1);
    v_heap = true;
  }

  for (size_t i = 0; i <= l2; i++)
    v[i] = i;

  for (size_t i = 0; i < l1; i++) {
    int current_left = i + 1;
    int prev_diag = v[0];
    v[0] = current_left;

    for (size_t j = 0; j < l2; j++) {
      int up = v[j + 1];
      int diag = prev_diag;
      int cost = (s1[i] == s2[j]) ? 0 : 1;

      int min = up + 1;
      int ins = current_left + 1;
      if (ins < min)
        min = ins;
      int sub = diag + cost;
      if (sub < min)
        min = sub;

      prev_diag = up;
      v[j + 1] = min;
      current_left = min;
    }
  }

  int res = v[l2];
  if (v_heap)
    free(v);
  return res;
}

typedef struct {
  uint64_t hash;
  int count;
} log_entry_t;

static log_entry_t *g_log_seen = NULL;
static size_t g_log_seen_cap = 0;
static size_t g_log_seen_len = 0;

bool ny_log_should_emit(const char *fmt) {
  if (!fmt)
    return false;
  if (g_log_seen_cap == 0) {
    g_log_seen_cap = 1024;
    g_log_seen = calloc(g_log_seen_cap, sizeof(log_entry_t));
  }
  uint64_t h = ny_hash64_cstr(fmt);
  size_t mask = g_log_seen_cap - 1;
  size_t idx = (size_t)h & mask;
  while (g_log_seen[idx].hash != 0) {
    if (g_log_seen[idx].hash == h) {
      g_log_seen[idx].count++;
      return g_log_seen[idx].count <= 10;
    }
    idx = (idx + 1) & mask;
  }
  if (g_log_seen_len * 2 >= g_log_seen_cap) {
    size_t old_cap = g_log_seen_cap;
    log_entry_t *old_tbl = g_log_seen;
    g_log_seen_cap *= 2;
    g_log_seen = calloc(g_log_seen_cap, sizeof(log_entry_t));
    mask = g_log_seen_cap - 1;
    for (size_t i = 0; i < old_cap; i++) {
      if (old_tbl[i].hash != 0) {
        size_t nidx = (size_t)old_tbl[i].hash & mask;
        while (g_log_seen[nidx].hash != 0)
          nidx = (nidx + 1) & mask;
        g_log_seen[nidx] = old_tbl[i];
      }
    }
    free(old_tbl);
    idx = (size_t)h & mask;
    while (g_log_seen[idx].hash != 0)
      idx = (idx + 1) & mask;
  }
  g_log_seen[idx].hash = h;
  g_log_seen[idx].count = 1;
  g_log_seen_len++;
  return true;
}

void ny_print_snippet(const char *src, int line, int col, int len,
                      const char *color) {
  if (!src || line <= 0 || col <= 0)
    return;
  const char *line_start = NULL;
  size_t line_len = 0;
  if (!ny_extract_line(src, line, &line_start, &line_len))
    return;
  if (line_len == 0)
    return;
  size_t caret_col = (size_t)(col - 1);
  if (caret_col > line_len)
    caret_col = line_len;
  size_t caret_len = len ? len : 1;
  if (caret_col + caret_len > line_len)
    caret_len = line_len > caret_col ? (line_len - caret_col) : 1;
  const size_t max_len = 200;
  size_t start = 0;
  size_t end = line_len;
  bool prefix = false;
  bool suffix = false;
  if (line_len > max_len) {
    if (caret_col > max_len / 2)
      start = caret_col - max_len / 2;
    if (start + max_len > line_len)
      start = line_len - max_len;
    end = start + max_len;
    prefix = start > 0;
    suffix = end < line_len;
  }
  size_t show_len = end - start;
  char *buf = malloc(show_len + 1);
  if (!buf)
    return;
  for (size_t i = 0; i < show_len; i++) {
    char c = line_start[start + i];
    buf[i] = (c == '\t') ? ' ' : c;
  }
  buf[show_len] = '\0';
  int width = 1;
  for (int tmp = line; tmp >= 10; tmp /= 10)
    width++;
  const char *gray = clr(NY_CLR_GRAY);
  const char *reset = clr(NY_CLR_RESET);
  const char *mark = clr(color);
  fprintf(stderr, "  %s%*d%s | %s%s%s\n", gray, width, line, reset,
          prefix ? "..." : "", buf, suffix ? "..." : "");
  size_t caret_pad = caret_col - start + (prefix ? 3 : 0);
  fprintf(stderr, "  %s%*s%s | ", gray, width, "", reset);
  for (size_t i = 0; i < caret_pad; i++)
    fputc(' ', stderr);
  fputs(mark, stderr);
  for (size_t i = 0; i < caret_len; i++)
    fputc('^', stderr);
  fputs(reset, stderr);
  fputc('\n', stderr);
  free(buf);
}
