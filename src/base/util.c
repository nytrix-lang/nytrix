/*
 * General utility belt: path manipulation, temp files, base64, hex dump,
 * env parsing, process identity, and small-string operations used everywhere.
 */
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif
#define _XOPEN_SOURCE 500
#include "base/util.h"
#include "base/common.h"
#include "base/curl.h"
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <process.h>
#include <windows.h>
#define access _access
#else
#include <sys/wait.h>
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
static bool ny_path_separator(char c) {
#ifdef _WIN32
  return c == '/' || c == '\\';
#else
  return c == '/';
#endif
}

static bool ny_mkdir_one(const char *path, unsigned int mode) {
  struct stat st;
  if (stat(path, &st) == 0)
    return S_ISDIR(st.st_mode);
#ifdef _WIN32
  int rc = _mkdir(path);
#else
  int rc = mkdir(path, (mode_t)mode);
#endif
  if (rc != 0 && errno != EEXIST)
    return false;
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool ny_mkdir_p_mode(const char *path, unsigned int mode) {
  if (!path || !*path)
    return false;
  size_t len = strlen(path);
  char *tmp = malloc(len + 1);
  if (!tmp)
    return false;
  memcpy(tmp, path, len + 1);

  size_t root_len = 0;
  if (ny_path_separator(tmp[0]))
    root_len = 1;
#ifdef _WIN32
  if (len >= 3 && tmp[1] == ':' && (tmp[2] == '/' || tmp[2] == '\\'))
    root_len = 3;
#endif
  while (len > root_len && ny_path_separator(tmp[len - 1]))
    tmp[--len] = '\0';

  size_t start = root_len ? root_len : 1;
  for (size_t i = start; i < len; ++i) {
    if (!ny_path_separator(tmp[i]))
      continue;
    char sep = tmp[i];
    tmp[i] = '\0';
    bool ok = !(i == 2 && root_len == 3) && ny_mkdir_one(tmp, mode);
    tmp[i] = sep;
    if (!ok) {
      free(tmp);
      return false;
    }
  }
  bool ok = ny_mkdir_one(tmp, mode);
  free(tmp);
  return ok;
}

bool ny_mkdir_p(const char *path) { return ny_mkdir_p_mode(path, 0755); }

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

bool ny_compiler_asserts_enabled(void) {
  static int cached = -1;
  if (cached >= 0)
    return cached != 0;
  const char *env = getenv("NYTRIX_COMPILER_ASSERTS");
  if (env && *env) {
    cached = ny_env_enabled("NYTRIX_COMPILER_ASSERTS") ? 1 : 0;
  } else {
    /*
     * Always-on by default unless NYTRIX_NO_COMPILER_ASSERTS is set
     */
    cached = ny_env_enabled("NYTRIX_NO_COMPILER_ASSERTS") ? 0 : 1;
  }
  return cached != 0;
}

void ny_compiler_assert_fail(const char *file, int line, const char *func,
                             const char *cond, const char *fmt, ...) {
  fprintf(stderr, "%sInternal compiler assertion failed:%s %s:%d in %s\n",
          clr(NY_CLR_RED), clr(NY_CLR_RESET), file ? file : "<unknown>", line,
          func ? func : "<unknown>");
  if (cond && *cond)
    fprintf(stderr, "  condition: %s\n", cond);
  if (fmt && *fmt) {
    fprintf(stderr, "  detail: ");
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
  }
  fprintf(stderr, "  hint: rerun with --dump-on-error or keep "
                  "NYTRIX_COMPILER_ASSERTS=1 under stress\n");
  fflush(stderr);
  if (getenv("NYTRIX_COMPILER_ASSERT_ABORT") &&
      strcmp(getenv("NYTRIX_COMPILER_ASSERT_ABORT"), "0") != 0) {
    abort();
  }
  exit(1);
}

void ny_compiler_invariant_fail(const char *file, int line, const char *func,
                                const char *invariant_name,
                                const char *src_file, int src_line, int src_col,
                                const char *module_or_fn,
                                const char *state_detail, const char *fmt,
                                ...) {
  char detail[512] = {0};
  if (fmt && *fmt) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof(detail), fmt, ap);
    va_end(ap);
  }
  fprintf(stderr, "%s[E9001] Internal compiler invariant failed:%s %s\n",
          clr(NY_CLR_RED), clr(NY_CLR_RESET),
          invariant_name ? invariant_name : "unspecified invariant");
  if (src_file && src_line > 0)
    fprintf(stderr, "  source: %s:%d:%d\n", src_file, src_line,
            src_col > 0 ? src_col : 1);
  if (module_or_fn && *module_or_fn)
    fprintf(stderr, "  context: %s\n", module_or_fn);
  if (state_detail && *state_detail)
    fprintf(stderr, "  state: %s\n", state_detail);
  if (detail[0])
    fprintf(stderr, "  detail: %s\n", detail);
  fprintf(stderr, "  location: %s:%d in %s\n", file ? file : "<unknown>", line,
          func ? func : "<unknown>");
  fflush(stderr);
  if (getenv("NYTRIX_COMPILER_ASSERT_ABORT") &&
      strcmp(getenv("NYTRIX_COMPILER_ASSERT_ABORT"), "0") != 0) {
    abort();
  }
  exit(1);
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
#ifdef P_tmpdir
  return ny_cache_path(buf, sizeof(buf), P_tmpdir);
#else
  return ny_cache_path(buf, sizeof(buf), ".");
#endif
#endif
}
static void ny_strip_shebang_inplace(char *content, size_t read) {
  if (!content || read < 2 || content[0] != '#' || content[1] != '!')
    return;
  char *newline = strchr(content, '\n');
  if (!newline)
    return;
  size_t skip_len = (size_t)(newline - content) + 1;
  if (skip_len < read) {
    size_t new_len = read - skip_len;
    memmove(content, newline + 1, new_len);
    content[new_len] = '\0';
  } else {
    content[0] = '\0';
  }
}

char *ny_read_file(const char *path) {
  size_t read = 0;
  char *content = ny_read_file_raw(path, &read);
  if (!content)
    return NULL;
  ny_strip_shebang_inplace(content, read);
  return content;
}

typedef struct {
  char *data;
  size_t len;
  size_t cap;
} ny_curl_buf_t;

static size_t ny_curl_write_cb(void *ptr, size_t size, size_t nmemb,
                               void *userdata) {
  size_t total = 0;
  if (!ny_size_mul_ok(size, nmemb, &total))
    return 0;
  ny_curl_buf_t *buf = (ny_curl_buf_t *)userdata;
  size_t required = 0;
  if (!ny_size_add_ok(total, 1, &required) ||
      !ny_size_add_ok(buf->len, required, &required))
    return 0;
  if (required > buf->cap) {
    size_t new_cap = buf->cap ? buf->cap : 8192;
    while (new_cap < required) {
      if (new_cap > SIZE_MAX / 2)
        return 0;
      new_cap *= 2;
    }
    char *new_data = realloc(buf->data, new_cap);
    if (!new_data)
      return 0;
    buf->data = new_data;
    buf->cap = new_cap;
  }
  memcpy(buf->data + buf->len, ptr, total);
  buf->len += total;
  buf->data[buf->len] = '\0';
  return total;
}

char *ny_read_url(const char *url) {
  if (!url)
    return NULL;

  ny_curl_state_t curl_state = {0};
  if (ny_curl_init(&curl_state)) {
    void *curl = ny_curl_easy_init(&curl_state);
    if (curl) {
      ny_curl_buf_t buf = {0};
      curl_state.setopt(curl, NY_CURLOPT_URL, url);
      curl_state.setopt(curl, NY_CURLOPT_WRITEFUNCTION, ny_curl_write_cb);
      curl_state.setopt(curl, NY_CURLOPT_WRITEDATA, &buf);
      curl_state.setopt(curl, NY_CURLOPT_FOLLOWLOCATION, 1L);
      curl_state.setopt(curl, NY_CURLOPT_USERAGENT, "nytrix-compiler/0.5");
      curl_state.setopt(curl, NY_CURLOPT_FAILONERROR, 1L);
      curl_state.setopt(curl, NY_CURLOPT_TIMEOUT, 30L);

      int res = ny_curl_easy_perform(&curl_state, curl);
      curl_state.cleanup(curl);
      ny_curl_cleanup(&curl_state);

      if (res == 0 && buf.data) {
        ny_strip_shebang_inplace(buf.data, buf.len);
        return buf.data;
      }
      free(buf.data);
    } else {
      ny_curl_cleanup(&curl_state);
    }
  }

  return NULL;
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
    if (fwrite(buf, 1, n, out) == n)
      continue;
    fclose(in);
    fclose(out);
    return -1;
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

static uint32_t ny_sha256_rotr32(uint32_t value, unsigned count) {
  return (value >> count) | (value << (32u - count));
}

static void ny_sha256_block(uint32_t state[8], const unsigned char block[64]) {
  static const uint32_t k[64] = {
      0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
      0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
      0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
      0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
      0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
      0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
      0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
      0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
      0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
      0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
      0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
      0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
      0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
  uint32_t words[64];
  for (size_t i = 0; i < 16; ++i) {
    size_t j = i * 4;
    words[i] = ((uint32_t)block[j] << 24) | ((uint32_t)block[j + 1] << 16) |
               ((uint32_t)block[j + 2] << 8) | (uint32_t)block[j + 3];
  }
  for (size_t i = 16; i < 64; ++i) {
    uint32_t x = words[i - 15];
    uint32_t y = words[i - 2];
    uint32_t s0 = ny_sha256_rotr32(x, 7) ^ ny_sha256_rotr32(x, 18) ^ (x >> 3);
    uint32_t s1 = ny_sha256_rotr32(y, 17) ^ ny_sha256_rotr32(y, 19) ^ (y >> 10);
    words[i] = words[i - 16] + s0 + words[i - 7] + s1;
  }
  uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4],
           f = state[5], g = state[6], h = state[7];
  for (size_t i = 0; i < 64; ++i) {
    uint32_t s1 = ny_sha256_rotr32(e, 6) ^ ny_sha256_rotr32(e, 11) ^
                  ny_sha256_rotr32(e, 25);
    uint32_t choose = (e & f) ^ (~e & g);
    uint32_t t1 = h + s1 + choose + k[i] + words[i];
    uint32_t s0 = ny_sha256_rotr32(a, 2) ^ ny_sha256_rotr32(a, 13) ^
                  ny_sha256_rotr32(a, 22);
    uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    uint32_t t2 = s0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

bool ny_sha256_file_hex(const char *path, char out[65]) {
  if (!path || !out)
    return false;
  FILE *file = fopen(path, "rb");
  if (!file)
    return false;
  uint32_t state[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                       0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
  unsigned char block[64];
  uint64_t total_bytes = 0;
  size_t used = 0;
  for (;;) {
    size_t count = fread(block + used, 1, sizeof(block) - used, file);
    total_bytes += count;
    used += count;
    if (used == sizeof(block)) {
      ny_sha256_block(state, block);
      used = 0;
    }
    if (count == 0) {
      if (ferror(file)) {
        fclose(file);
        return false;
      }
      break;
    }
  }
  fclose(file);
  uint64_t total_bits = total_bytes * UINT64_C(8);
  block[used++] = 0x80;
  if (used > 56) {
    memset(block + used, 0, sizeof(block) - used);
    ny_sha256_block(state, block);
    used = 0;
  }
  memset(block + used, 0, 56 - used);
  for (size_t i = 0; i < 8; ++i)
    block[63 - i] = (unsigned char)(total_bits >> (i * 8));
  ny_sha256_block(state, block);

  static const char hex[] = "0123456789abcdef";
  for (size_t i = 0; i < 8; ++i) {
    for (size_t j = 0; j < 4; ++j) {
      unsigned char byte = (unsigned char)(state[i] >> (24 - j * 8));
      out[(i * 4 + j) * 2] = hex[byte >> 4];
      out[(i * 4 + j) * 2 + 1] = hex[byte & 0x0f];
    }
  }
  out[64] = '\0';
  return true;
}

void ny_write_text_file(const char *path, const char *contents) {
  if (!path || !contents)
    return;
  ny_write_file(path, contents, strlen(contents));
}

static bool nytrix_has_sources(const char *root) {
  char probe[8192];
  snprintf(probe, sizeof(probe), "%s/src/code/lang/runtime/runtime.h", root);
  return ny_access(probe, R_OK) == 0;
}

const char *ny_get_executable_path(void) {
  static char buf[PATH_MAX];
  if (buf[0])
    return buf;
#ifdef _WIN32
  DWORD len = GetModuleFileNameA(NULL, buf, sizeof(buf));
  if (len > 0 && len < sizeof(buf)) {
    buf[len] = '\0';
    return buf;
  }
#elif defined(__APPLE__)
  uint32_t size = (uint32_t)sizeof(buf);
  if (_NSGetExecutablePath(buf, &size) == 0) {
    return buf;
  }
#else
  ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (len != -1) {
    buf[len] = '\0';
    return buf;
  }
#endif
  return NULL;
}

char *ny_get_executable_dir(void) {
  const char *path = ny_get_executable_path();
  if (!path)
    return NULL;
  static char dir[PATH_MAX];
  snprintf(dir, sizeof(dir), "%s", path);
  char *slash = strrchr(dir, '/');
#ifdef _WIN32
  if (!slash)
    slash = strrchr(dir, '\\');
#endif
  if (slash)
    *slash = '\0';
  return dir;
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
    char cur[PATH_MAX];
    snprintf(cur, sizeof(cur), "%s", exe_dir);
    for (;;) {
      if (nytrix_has_sources(cur)) {
        return ny_cache_path(buf, sizeof(buf), cur);
      }
      char *slash = ny_last_sep(cur);
      if (!slash || slash == cur)
        break;
      *slash = '\0';
    }
    {
      char tmp[PATH_MAX];
#ifdef NYTRIX_INSTALL_DATADIR
      snprintf(tmp, sizeof(tmp), "%s/../%s/nytrix", exe_dir,
               NYTRIX_INSTALL_DATADIR);
      if (nytrix_has_sources(tmp)) {
        return ny_cache_path(buf, sizeof(buf), tmp);
      }
#endif
      snprintf(tmp, sizeof(tmp), "%s/../share/nytrix", exe_dir);
      if (nytrix_has_sources(tmp)) {
        return ny_cache_path(buf, sizeof(buf), tmp);
      }
      snprintf(tmp, sizeof(tmp), "%s/../../share/nytrix", exe_dir);
      if (nytrix_has_sources(tmp)) {
        return ny_cache_path(buf, sizeof(buf), tmp);
      }
    }
  }
#ifndef _WIN32
  const char *install_paths[] = {"/usr/share/nytrix", "/usr/local/share/nytrix",
                                 "/opt/nytrix/share/nytrix",
                                 "/opt/nytrix/share",
                                 "/opt/homebrew/share/nytrix"};
  for (size_t i = 0; i < sizeof(install_paths) / sizeof(install_paths[0]);
       i++) {
    if (!nytrix_has_sources(install_paths[i]))
      continue;
    return ny_cache_path(buf, sizeof(buf), install_paths[i]);
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

const char *ny_default_cache_root_dir(void) {
  static char path[PATH_MAX];
  const char *override = getenv("NYTRIX_CACHE_DIR");
  if (override && *override)
    return ny_cache_path(path, sizeof(path), override);

  const char *xdg = getenv("XDG_CACHE_HOME");
  if (xdg && *xdg) {
    ny_join_path(path, sizeof(path), xdg, "nytrix");
    return path;
  }

  const char *home = getenv("HOME");
  if (!home)
    home = getenv("USERPROFILE");
  if (home && *home) {
    ny_join_path(path, sizeof(path), home, ".cache/nytrix");
    return path;
  }

  const char *src = ny_src_root();
  if (src && *src) {
    ny_join_path(path, sizeof(path), src, "build/cache/nytrix");
    return path;
  }

  ny_join_path(path, sizeof(path), ny_get_temp_dir(), "nytrix");
  return path;
}

char *ny_strdup(const char *s) {
  if (!s)
    return NULL;
  size_t len = strlen(s);
  size_t bytes = 0;
  if (!ny_size_add_ok(len, 1, &bytes))
    return NULL;
  char *copy = malloc(bytes);
  if (!copy)
    return NULL;
  memcpy(copy, s, bytes);
  return copy;
}

static int ny_path_ends_with(const char *path, const char *suffix) {
  if (!path || !suffix)
    return 0;
  size_t n = strlen(path);
  size_t m = strlen(suffix);
  return n >= m && strcmp(path + n - m, suffix) == 0;
}

static int ny_shlib_spawn(char *const argv[]) {
  if (!argv || !argv[0])
    return -1;
#ifdef _WIN32
  intptr_t rc = _spawnvp(_P_WAIT, argv[0], (const char *const *)argv);
  return rc < 0 ? -1 : (int)rc;
#else
  pid_t pid = fork();
  if (pid < 0)
    return -1;
  if (pid == 0) {
    execvp(argv[0], argv);
    _exit(127);
  }
  int status = 0;
  if (waitpid(pid, &status, 0) < 0)
    return -1;
  if (WIFEXITED(status))
    return WEXITSTATUS(status);
  return -1;
#endif
}

char *ny_ensure_shared_lib(const char *path) {
  if (!path || !*path)
    return NULL;

  /*
   * Only rewrite path-like shared objects that have a sibling .c. Bare names
   * (curl, libz.so) and system libs stay as requested.
   */
  int is_shlib = ny_path_ends_with(path, ".so") ||
                 ny_path_ends_with(path, ".dll") ||
                 ny_path_ends_with(path, ".dylib");
  if (!is_shlib)
    return ny_strdup(path);
  if (!strchr(path, '/')
#ifdef _WIN32
      && !strchr(path, '\\')
#endif
  )
    return ny_strdup(path);

  char src[PATH_MAX];
  size_t plen = strlen(path);
  if (plen + 1 >= sizeof(src))
    return ny_strdup(path);
  memcpy(src, path, plen + 1);
  char *dot = strrchr(src, '.');
  if (!dot)
    return ny_strdup(path);
  if (strcmp(dot, ".so") != 0 && strcmp(dot, ".dll") != 0 &&
      strcmp(dot, ".dylib") != 0)
    return ny_strdup(path);
  memcpy(dot, ".c", sizeof(".c"));
  if (ny_access(src, R_OK) != 0)
    return ny_strdup(path);

  const char *base = strrchr(path, '/');
#ifdef _WIN32
  const char *bslash = strrchr(path, '\\');
  if (!base || (bslash && bslash > base))
    base = bslash;
#endif
  base = base ? base + 1 : path;

  char cache_dir[PATH_MAX];
  ny_join_path(cache_dir, sizeof(cache_dir), ny_get_temp_dir(), "nytrix-shlib");
  ny_ensure_dir_recursive(cache_dir);

  char out[PATH_MAX];
  ny_join_path(out, sizeof(out), cache_dir, base);

  struct stat st_src, st_out;
  int need_build = 1;
  if (stat(src, &st_src) == 0 && stat(out, &st_out) == 0 &&
      st_out.st_mtime >= st_src.st_mtime)
    need_build = 0;

  if (need_build) {
    const char *cc = getenv("CC");
    if (!cc || !*cc)
      cc = "cc";
    char *argv[] = {
        (char *)cc,      (char *)"-shared",
#ifndef _WIN32
        (char *)"-fPIC",
#endif
        (char *)"-O2",   (char *)"-o",      out, src, NULL,
    };
    if (ny_shlib_spawn(argv) != 0 || ny_access(out, R_OK) != 0) {
      /*
       * Fall back to the requested path if build failed (may still exist).
       */
      return ny_strdup(path);
    }
  }
  return ny_strdup(out);
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

const char *ny_env_str(const char *name) {
  if (!name || !*name)
    return NULL;
  return getenv(name);
}

const char *ny_env_str_nonempty(const char *name) {
  const char *v = ny_env_str(name);
  return (v && *v) ? v : NULL;
}

int ny_env_int(const char *name, int fallback) {
  const char *v = ny_env_str_nonempty(name);
  if (!v)
    return fallback;
  char *end = NULL;
  long x = strtol(v, &end, 10);
  if (end == v)
    return fallback;
  if (x < INT32_MIN)
    x = INT32_MIN;
  if (x > INT32_MAX)
    x = INT32_MAX;
  return (int)x;
}

int ny_env_int_range(const char *name, int fallback, int minv, int maxv) {
  int v = ny_env_int(name, fallback);
  if (v < minv)
    return minv;
  if (v > maxv)
    return maxv;
  return v;
}

bool ny_str_list_append(char ***list, size_t *len, size_t *cap,
                        const char *str) {
  if (!list || !len || !cap)
    return false;
  if (*len == *cap) {
    if (*cap > SIZE_MAX / 2)
      return false;
    size_t new_cap = *cap ? (*cap * 2) : 8;
    size_t bytes = 0;
    if (!ny_size_mul_ok(new_cap, sizeof(char *), &bytes))
      return false;
    char **tmp = realloc(*list, bytes);
    if (!tmp)
      return false;
    *list = tmp;
    *cap = new_cap;
  }
  char *copy = ny_strdup(str);
  if (!copy)
    return false;
  (*list)[(*len)++] = copy;
  return true;
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
  if (!s1 || !s2)
    return -1;
  size_t l1 = strlen(s1);
  size_t l2 = strlen(s2);
  if (l1 < l2)
    return ny_levenshtein(s2, s1);
  if (l1 > (size_t)INT_MAX || l2 > (size_t)INT_MAX)
    return -1;
  if (l2 == 0)
    return (int)l1;

  int stack_v[2048];
  int *v = stack_v;
  bool v_heap = false;
  size_t v_count = 0;
  size_t v_bytes = 0;
  if (!ny_size_add_ok(l2, 1, &v_count) ||
      !ny_size_mul_ok(v_count, sizeof(int), &v_bytes))
    return -1;
  if (v_count > 2048) {
    v = malloc(v_bytes);
    if (!v)
      exit(1);
    v_heap = true;
  }

  for (size_t i = 0; i <= l2; i++)
    v[i] = (int)i;

  for (size_t i = 0; i < l1; i++) {
    int current_left = (int)i + 1;
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
    if (!g_log_seen) {
      g_log_seen_cap = 0;
      return false;
    }
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
  if (g_log_seen_len >= g_log_seen_cap / 2) {
    if (g_log_seen_cap > SIZE_MAX / 2)
      return false;
    size_t old_cap = g_log_seen_cap;
    log_entry_t *old_tbl = g_log_seen;
    size_t new_cap = g_log_seen_cap * 2;
    log_entry_t *new_tbl = calloc(new_cap, sizeof(log_entry_t));
    if (!new_tbl)
      return false;
    g_log_seen = new_tbl;
    g_log_seen_cap = new_cap;
    mask = new_cap - 1;
    for (size_t i = 0; i < old_cap; i++) {
      if (old_tbl[i].hash == 0)
        continue;
      size_t nidx = (size_t)old_tbl[i].hash & mask;
      while (g_log_seen[nidx].hash != 0)
        nidx = (nidx + 1) & mask;
      g_log_seen[nidx] = old_tbl[i];
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

static int g_complexity_enabled = -1;

void ny_complexity_note(const char *file, int line, const char *func,
                        const char *category, const char *detail) {
  if (g_complexity_enabled < 0)
    g_complexity_enabled = ny_env_enabled("NYTRIX_COMPLEXITY_REPORT");
  if (!g_complexity_enabled || !verbose_enabled)
    return;
  fprintf(stderr, "[complexity] %s:%d in %s: [%s] %s\n", file ? file : "?",
          line, func ? func : "?", category ? category : "misc",
          detail ? detail : "");
}
