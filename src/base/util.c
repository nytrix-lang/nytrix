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
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include "base/dirent.h"
#include <direct.h>
#include <io.h>
#include <process.h>
#include <windows.h>
#define access _access
#else
#include <dirent.h>
#include <sys/file.h>
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
  (void)mode;
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
  int close_status = fclose(f);
  return (written == len && close_status == 0) ? 0 : -1;
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

void ny_write_text_file(const char *path, const char *contents) {
  if (!path || !contents)
    return;
  ny_write_file(path, contents, strlen(contents));
}

static bool nytrix_has_sources(const char *root) {
  char probe[8192];
  snprintf(probe, sizeof(probe), "%s/src/code/backend/runtime/runtime.h", root);
  return ny_access(probe, R_OK) == 0;
}

typedef struct {
  char name[256];
  uint64_t size;
  time_t modified;
} ny_cache_disk_entry;

static bool ny_cache_published_name(const char *name) {
  if (!name || name[0] == '.')
    return false;
  const char *extension = strrchr(name, '.');
  return extension &&
         (!strcmp(extension, ".ny") || !strcmp(extension, ".bc") ||
          !strcmp(extension, ".ll") || !strcmp(extension, ".ir") ||
          !strcmp(extension, ".so") || !strcmp(extension, ".obj") ||
          !strcmp(extension, ".manifest") || !strcmp(extension, ".libs"));
}

static int ny_cache_disk_entry_compare(const void *left, const void *right) {
  const ny_cache_disk_entry *a = left, *b = right;
  if (a->modified != b->modified)
    return a->modified < b->modified ? -1 : 1;
  return strcmp(a->name, b->name);
}

static bool ny_cache_disk_stat(const char *path, struct stat *st) {
#ifdef _WIN32
  DWORD attributes = GetFileAttributesA(path);
  if (attributes == INVALID_FILE_ATTRIBUTES ||
      (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
    return false;
  return stat(path, st) == 0 && S_ISREG(st->st_mode);
#else
  return lstat(path, st) == 0 && S_ISREG(st->st_mode) &&
         st->st_uid == geteuid();
#endif
}

static void ny_cache_remove_abandoned_temporary(const char *directory,
                                                const char *name) {
  const char *marker = strstr(name, ".tmp.");
  if (!marker || (size_t)(marker - name) >= 256)
    return;
  char published[256];
  memcpy(published, name, (size_t)(marker - name));
  published[marker - name] = '\0';
  if (!ny_cache_published_name(published))
    return;
  char *end = NULL;
  errno = 0;
  unsigned long owner = strtoul(marker + 5, &end, 10);
  if (errno || end == marker + 5 || !owner || owner > INT_MAX || *end != '.' ||
      !end[1])
    return;
#ifdef _WIN32
  HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)owner);
  bool abandoned;
  if (process) {
    abandoned = WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
    CloseHandle(process);
  } else {
    abandoned = GetLastError() == ERROR_INVALID_PARAMETER;
  }
#else
  bool abandoned = kill((pid_t)owner, 0) != 0 && errno == ESRCH;
#endif
  if (!abandoned)
    return;
  char path[PATH_MAX];
  int n = snprintf(path, sizeof(path), "%s/%s", directory, name);
  struct stat st;
  if (n > 0 && (size_t)n < sizeof(path) && ny_cache_disk_stat(path, &st))
    (void)remove(path);
}

static intptr_t ny_cache_directory_lock(const char *directory, bool exclusive) {
  char path[PATH_MAX];
  int n = snprintf(path, sizeof(path), "%s/.publish.lock", directory);
  if (n <= 0 || (size_t)n >= sizeof(path))
    return 0;
#ifdef _WIN32
  HANDLE lock =
      CreateFileA(path, exclusive ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ,
                  exclusive ? 0 : FILE_SHARE_READ, NULL, OPEN_ALWAYS,
                  FILE_ATTRIBUTE_NORMAL, NULL);
  return lock == INVALID_HANDLE_VALUE ? 0 : (intptr_t)lock;
#else
  int lock = open(path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (lock < 0)
    return 0;
  /*
   * A long-running cache consumer must not stall an unrelated compilation
   * merely to persist its optional result. Readers wait only for publication.
   */
  if (flock(lock, exclusive ? (LOCK_EX | LOCK_NB) : LOCK_SH) != 0) {
    close(lock);
    return 0;
  }
  return (intptr_t)lock + 1;
#endif
}

void ny_cache_read_unlock(intptr_t token) {
  if (!token)
    return;
#ifdef _WIN32
  CloseHandle((HANDLE)token);
#else
  close((int)(token - 1));
#endif
}

intptr_t ny_cache_read_lock(const char *path) {
  if (!path)
    return 0;
  char directory[PATH_MAX];
  int n = snprintf(directory, sizeof(directory), "%s", path);
  if (n <= 0 || (size_t)n >= sizeof(directory))
    return 0;
  char *slash = strrchr(directory, '/');
#ifdef _WIN32
  char *backslash = strrchr(directory, '\\');
  if (backslash && (!slash || backslash > slash))
    slash = backslash;
#endif
  if (!slash || slash == directory)
    return 0;
  *slash = '\0';
  return ny_cache_directory_lock(directory, false);
}

bool ny_cache_publish_file(const char *temporary, const char *destination) {
  if (!temporary || !destination)
    return false;
  char directory[PATH_MAX], path[PATH_MAX];
  int length = snprintf(directory, sizeof(directory), "%s", destination);
  if (length <= 0 || (size_t)length >= sizeof(directory))
    return false;
  char *slash = strrchr(directory, '/');
#ifdef _WIN32
  char *backslash = strrchr(directory, '\\');
  if (backslash && (!slash || backslash > slash))
    slash = backslash;
#endif
  if (!slash || slash == directory)
    return false;
  const char *name = destination + (slash - directory) + 1;
  size_t key_length = strcspn(name, ".");
  *slash = '\0';
  uint64_t limit = UINT64_C(256) * 1024 * 1024;
  const char *setting = getenv("NYTRIX_CACHE_MAX_MB");
  if (setting && *setting && setting[0] != '-') {
    char *end = NULL;
    errno = 0;
    unsigned long long mb = strtoull(setting, &end, 10);
    if (!errno && end != setting && !*end && mb > 0 &&
        mb <= UINT64_MAX / (1024 * 1024))
      limit = (uint64_t)mb * 1024 * 1024;
  }
  struct stat incoming;
  if (!ny_cache_disk_stat(temporary, &incoming) || incoming.st_size < 0 ||
      (uint64_t)incoming.st_size > limit)
    return false;
  intptr_t lock = ny_cache_directory_lock(directory, true);
  if (!lock)
    return false;
  bool ok = false;
  uint64_t bytes = (uint64_t)incoming.st_size;
  size_t count = 1, entry_count = 0;
  ny_cache_disk_entry *entries = NULL;
  DIR *dir = opendir(directory);
  if (!dir)
    goto done;
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (!ny_cache_published_name(entry->d_name)) {
      ny_cache_remove_abandoned_temporary(directory, entry->d_name);
      continue;
    }
    if (!strcmp(entry->d_name, name))
      continue;
    length = snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
    struct stat st;
    if (length <= 0 || (size_t)length >= sizeof(path) ||
        !strcmp(path, temporary) || !ny_cache_disk_stat(path, &st) ||
        st.st_size < 0)
      continue;
    uint64_t size = (uint64_t)st.st_size;
    bytes = size > UINT64_MAX - bytes ? UINT64_MAX : bytes + size;
    ++count;
  }
  closedir(dir);
  if (bytes > limit || count > 4096) {
    if (count > SIZE_MAX / sizeof(*entries))
      goto done;
    entries = malloc(count * sizeof(*entries));
    if (!entries)
      goto done;
    dir = opendir(directory);
    if (!dir)
      goto done;
    while ((entry = readdir(dir)) != NULL && entry_count < count) {
      size_t name_length = strlen(entry->d_name);
      if (!ny_cache_published_name(entry->d_name) ||
          name_length >= sizeof(entries[0].name) ||
          (strncmp(entry->d_name, name, key_length) == 0 &&
           (entry->d_name[key_length] == '.' ||
            entry->d_name[key_length] == '\0')))
        continue;
      length = snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
      struct stat st;
      if (length <= 0 || (size_t)length >= sizeof(path) ||
          !strcmp(path, temporary) || !ny_cache_disk_stat(path, &st) ||
          st.st_size < 0)
        continue;
      ny_cache_disk_entry *item = &entries[entry_count++];
      memcpy(item->name, entry->d_name, name_length + 1);
      item->size = (uint64_t)st.st_size;
      item->modified = st.st_mtime;
    }
    closedir(dir);
    qsort(entries, entry_count, sizeof(*entries), ny_cache_disk_entry_compare);
    for (size_t i = 0; i < entry_count && (bytes > limit || count > 4096);
         ++i) {
      length =
          snprintf(path, sizeof(path), "%s/%s", directory, entries[i].name);
      if (length > 0 && (size_t)length < sizeof(path) && remove(path) == 0) {
        bytes -= entries[i].size;
        --count;
      }
    }
  }
  if (bytes <= limit && count <= 4096) {
#ifdef _WIN32
    ok = MoveFileExA(temporary, destination, MOVEFILE_REPLACE_EXISTING) != 0;
#else
    ok = rename(temporary, destination) == 0;
#endif
  }
done:
  free(entries);
  ny_cache_read_unlock(lock);
  return ok;
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

  const char *src = ny_src_root();
  if (src && *src && nytrix_has_sources(src)) {
    char repo_cache[PATH_MAX];
    ny_join_path(repo_cache, sizeof(repo_cache), src, "build/cache/nytrix");
    if (ny_mkdir_p(repo_cache) && ny_access(repo_cache, W_OK) == 0)
      return ny_cache_path(path, sizeof(path), repo_cache);
  }

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
/*
 * Function bodies are optimized on pool threads, and passes may log.
 */
static atomic_flag g_log_seen_lock = ATOMIC_FLAG_INIT;

static bool ny_log_should_emit_locked(const char *fmt);

bool ny_log_should_emit(const char *fmt) {
  if (!fmt)
    return false;
  while (atomic_flag_test_and_set_explicit(&g_log_seen_lock,
                                           memory_order_acquire))
    ;
  bool emit = ny_log_should_emit_locked(fmt);
  atomic_flag_clear_explicit(&g_log_seen_lock, memory_order_release);
  return emit;
}

static bool ny_log_should_emit_locked(const char *fmt) {
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
