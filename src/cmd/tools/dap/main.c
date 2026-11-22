/*
 * Nytrix debug adapter: a Debug Adapter Protocol proxy in front of a native
 * debugger (GDB's DAP interpreter or lldb-dap).
 *
 * Editors speak plain DAP to this executable and never need to know how
 * Nytrix programs are built. A `launch` whose `program` is a `.ny` file, or
 * an `.nshape` fixture with an embedded `source ny` block, is compiled here
 * with debug information before the request reaches the debugger; every
 * other message passes through unchanged. Paths of an `.nshape` build are
 * rewritten in both directions, so breakpoints and stack frames refer to the
 * fixture the user opened. Works on POSIX and Windows.
 *
 *   ny-dap [--debugger PATH] [--ny PATH] [--log FILE]
 *
 * NYTRIX_DEBUGGER and NYTRIX_NY override the debugger and compiler. The
 * default debugger is the first of gdb (DAP needs GDB 14+) and lldb-dap on
 * PATH, lldb-dap first on macOS.
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "base/subprocess.h"
#include "base/util.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

int color_mode = -1;
int verbose_enabled = 0;

#define DAP_EXE_SUFFIX_MAX 8
#ifdef _WIN32
#define DAP_EXE_SUFFIX ".exe"
#define DAP_PATH_SEP ';'
#else
#define DAP_EXE_SUFFIX ""
#define DAP_PATH_SEP ':'
#endif

/*
 * Growable byte buffer
 */

typedef struct {
  char *data;
  size_t len, cap;
} dap_buf_t;

static bool buf_reserve(dap_buf_t *b, size_t extra) {
  if (b->len + extra + 1 <= b->cap)
    return true;
  size_t cap = b->cap ? b->cap * 2 : 256;
  while (cap < b->len + extra + 1)
    cap *= 2;
  char *next = realloc(b->data, cap);
  if (!next)
    return false;
  b->data = next;
  b->cap = cap;
  return true;
}

static void buf_add_n(dap_buf_t *b, const char *s, size_t n) {
  if (!buf_reserve(b, n))
    return;
  memcpy(b->data + b->len, s, n);
  b->len += n;
  b->data[b->len] = '\0';
}

static void buf_add(dap_buf_t *b, const char *s) { buf_add_n(b, s, strlen(s)); }

static void buf_add_json_string(dap_buf_t *b, const char *s) {
  buf_add(b, "\"");
  for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
    char esc[8];
    if (*p == '"' || *p == '\\') {
      esc[0] = '\\';
      esc[1] = (char)*p;
      buf_add_n(b, esc, 2);
    } else if (*p == '\n') {
      buf_add(b, "\\n");
    } else if (*p == '\r') {
      buf_add(b, "\\r");
    } else if (*p == '\t') {
      buf_add(b, "\\t");
    } else if (*p < 0x20) {
      snprintf(esc, sizeof(esc), "\\u%04x", *p);
      buf_add(b, esc);
    } else {
      buf_add_n(b, (const char *)p, 1);
    }
  }
  buf_add(b, "\"");
}

/*
 * Minimal JSON reading: enough to find keys and strings in DAP requests
 */

static const char *json_ws(const char *p) {
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
    ++p;
  return p;
}

/*
 * Returns the character after the JSON value starting at p (NULL if bad).
 */
static const char *json_skip(const char *p) {
  p = json_ws(p);
  if (*p == '"') {
    for (++p; *p && *p != '"'; ++p)
      if (*p == '\\' && p[1])
        ++p;
    return *p ? p + 1 : NULL;
  }
  if (*p == '{' || *p == '[') {
    int depth = 0;
    for (; *p; ++p) {
      if (*p == '"') {
        p = json_skip(p);
        if (!p)
          return NULL;
        --p;
      } else if (*p == '{' || *p == '[') {
        ++depth;
      } else if (*p == '}' || *p == ']') {
        if (--depth == 0)
          return p + 1;
      }
    }
    return NULL;
  }
  while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' &&
         *p != '\n' && *p != '\r' && *p != '\t')
    ++p;
  return p;
}

/*
 * Value of `key` among the members of the object starting at obj.
 */
static const char *json_member(const char *obj, const char *key) {
  const char *p = obj ? json_ws(obj) : NULL;
  if (!p || *p != '{')
    return NULL;
  size_t key_len = strlen(key);
  for (p = json_ws(p + 1); *p && *p != '}';) {
    if (*p != '"')
      return NULL;
    const char *name = p + 1;
    const char *after = json_skip(p);
    if (!after)
      return NULL;
    bool match = (size_t)(after - 1 - name) == key_len &&
                 strncmp(name, key, key_len) == 0;
    p = json_ws(after);
    if (*p != ':')
      return NULL;
    p = json_ws(p + 1);
    if (match)
      return p;
    p = json_skip(p);
    if (!p)
      return NULL;
    p = json_ws(p);
    if (*p == ',')
      p = json_ws(p + 1);
  }
  return NULL;
}

/*
 * Decodes the JSON string at p; NULL when p is not a string.
 */
static char *json_string(const char *p) {
  if (!p || *(p = json_ws(p)) != '"')
    return NULL;
  dap_buf_t out = {0};
  for (++p; *p && *p != '"'; ++p) {
    char c = *p;
    if (c == '\\' && p[1]) {
      c = *++p;
      if (c == 'n')
        c = '\n';
      else if (c == 't')
        c = '\t';
      else if (c == 'r')
        c = '\r';
      else if (c == 'u' && p[1] && p[2] && p[3] && p[4]) {
        char hex[5] = {p[1], p[2], p[3], p[4], 0};
        unsigned long cp = strtoul(hex, NULL, 16);
        p += 4;
        char utf8[4];
        size_t n = 0;
        if (cp < 0x80) {
          utf8[n++] = (char)cp;
        } else if (cp < 0x800) {
          utf8[n++] = (char)(0xc0 | (cp >> 6));
          utf8[n++] = (char)(0x80 | (cp & 0x3f));
        } else {
          utf8[n++] = (char)(0xe0 | (cp >> 12));
          utf8[n++] = (char)(0x80 | ((cp >> 6) & 0x3f));
          utf8[n++] = (char)(0x80 | (cp & 0x3f));
        }
        buf_add_n(&out, utf8, n);
        continue;
      }
    }
    buf_add_n(&out, &c, 1);
  }
  if (!out.data)
    buf_add(&out, "");
  return out.data;
}

/*
 * Framed I/O
 */

static FILE *dap_log;
static FILE *client_out;
#ifdef _WIN32
static CRITICAL_SECTION client_lock;
#define CLIENT_LOCK() EnterCriticalSection(&client_lock)
#define CLIENT_UNLOCK() LeaveCriticalSection(&client_lock)
#else
static pthread_mutex_t client_lock = PTHREAD_MUTEX_INITIALIZER;
#define CLIENT_LOCK() pthread_mutex_lock(&client_lock)
#define CLIENT_UNLOCK() pthread_mutex_unlock(&client_lock)
#endif

static void log_line(const char *fmt, ...) {
  if (!dap_log)
    return;
  va_list ap;
  va_start(ap, fmt);
  vfprintf(dap_log, fmt, ap);
  va_end(ap);
  fputc('\n', dap_log);
  fflush(dap_log);
}

/*
 * Reads one Content-Length framed message; NULL at end of stream.
 */
static char *read_frame(FILE *in, size_t *len_out) {
  char line[256];
  long length = -1;
  while (fgets(line, sizeof(line), in)) {
    if (line[0] == '\r' || line[0] == '\n') {
      if (length < 0)
        continue;
      char *body = malloc((size_t)length + 1);
      if (!body || fread(body, 1, (size_t)length, in) != (size_t)length) {
        free(body);
        return NULL;
      }
      body[length] = '\0';
      if (len_out)
        *len_out = (size_t)length;
      return body;
    }
    if (strncmp(line, "Content-Length:", 15) == 0)
      length = strtol(line + 15, NULL, 10);
  }
  return NULL;
}

static void write_frame(FILE *out, const char *body, size_t len) {
  fprintf(out, "Content-Length: %zu\r\n\r\n", len);
  fwrite(body, 1, len, out);
  fflush(out);
}

static void to_client(const char *body, size_t len) {
  CLIENT_LOCK();
  write_frame(client_out, body, len);
  CLIENT_UNLOCK();
}

static int adapter_seq = 1000000;

static void send_output(const char *category, const char *text) {
  dap_buf_t b = {0};
  char head[96];
  snprintf(head, sizeof(head),
           "{\"seq\":%d,\"type\":\"event\",\"event\":\"output\","
           "\"body\":{\"category\":",
           adapter_seq++);
  buf_add(&b, head);
  buf_add_json_string(&b, category);
  buf_add(&b, ",\"output\":");
  buf_add_json_string(&b, text);
  buf_add(&b, "}}");
  to_client(b.data, b.len);
  free(b.data);
}

static void send_error_response(const char *request, const char *message) {
  const char *seq_value = json_member(request, "seq");
  long seq = seq_value ? strtol(seq_value, NULL, 10) : 0;
  char *command = json_string(json_member(request, "command"));
  dap_buf_t b = {0};
  char head[96];
  snprintf(head, sizeof(head),
           "{\"seq\":%d,\"type\":\"response\",\"request_seq\":%ld,"
           "\"success\":false,\"command\":",
           adapter_seq++, seq);
  buf_add(&b, head);
  buf_add_json_string(&b, command ? command : "");
  buf_add(&b, ",\"message\":");
  buf_add_json_string(&b, message);
  buf_add(&b, ",\"body\":{\"error\":{\"id\":1,\"format\":");
  buf_add_json_string(&b, message);
  buf_add(&b, "}}}");
  to_client(b.data, b.len);
  free(b.data);
  free(command);
}

/*
 * Tools
 */

static bool is_executable_file(const char *path) {
#ifdef _WIN32
  DWORD attrs = GetFileAttributesA(path);
  return attrs != INVALID_FILE_ATTRIBUTES &&
         !(attrs & FILE_ATTRIBUTE_DIRECTORY);
#else
  return access(path, X_OK) == 0;
#endif
}

/*
 * Resolves a bare tool name on PATH (adding .exe on Windows).
 */
static char *find_on_path(const char *name) {
  if (strchr(name, '/') || strchr(name, '\\'))
    return is_executable_file(name) ? strdup(name) : NULL;
  const char *path = getenv("PATH");
  if (!path)
    return NULL;
  char candidate[4096];
  for (const char *p = path; *p;) {
    const char *end = strchr(p, DAP_PATH_SEP);
    size_t n = end ? (size_t)(end - p) : strlen(p);
    if (n > 0 && n < sizeof(candidate) - strlen(name) - DAP_EXE_SUFFIX_MAX) {
      snprintf(candidate, sizeof(candidate), "%.*s/%s%s", (int)n, p, name,
               DAP_EXE_SUFFIX);
      if (is_executable_file(candidate))
        return strdup(candidate);
    }
    if (!end)
      break;
    p = end + 1;
  }
  return NULL;
}

/*
 * The compiler next to this adapter, else `ny` on PATH.
 */
static char *find_compiler(const char *override) {
  if (override && *override)
    return strdup(override);
  const char *env = getenv("NYTRIX_NY");
  if (env && *env)
    return strdup(env);
  const char *dir = ny_get_executable_dir(); /* static buffer, not owned */
  if (dir) {
    char sibling[4096];
    snprintf(sibling, sizeof(sibling), "%s/ny%s", dir, DAP_EXE_SUFFIX);
    if (is_executable_file(sibling))
      return strdup(sibling);
  }
  return find_on_path("ny");
}

static char *find_debugger(const char *override) {
  if (override && *override)
    return find_on_path(override);
  const char *env = getenv("NYTRIX_DEBUGGER");
  if (env && *env)
    return find_on_path(env);
#ifdef __APPLE__
  static const char *const order[] = {"lldb-dap", "lldb-vscode", "gdb"};
#else
  static const char *const order[] = {"gdb", "lldb-dap", "lldb-vscode"};
#endif
  for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); ++i) {
    char *found = find_on_path(order[i]);
    if (found)
      return found;
  }
  return NULL;
}

static bool debugger_is_gdb(const char *path) {
  const char *base = strrchr(path, '/');
  const char *bs = strrchr(path, '\\');
  if (bs && (!base || bs > base))
    base = bs;
  base = base ? base + 1 : path;
  return strncmp(base, "gdb", 3) == 0;
}

/*
 * Debugger process with pipes
 */

typedef struct {
  FILE *in;  /* we write requests here */
  FILE *out; /* we read responses/events here */
#ifdef _WIN32
  HANDLE process;
#else
  pid_t pid;
#endif
} dap_backend_t;

static bool spawn_backend(dap_backend_t *be, const char *debugger) {
  const char *argv[6] = {debugger, NULL, NULL, NULL, NULL, NULL};
  if (debugger_is_gdb(debugger)) {
    argv[1] = "--interpreter=dap";
    argv[2] = "--nx";
    argv[3] = "--quiet";
  }
#ifdef _WIN32
  SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
  HANDLE child_in_r, child_in_w, child_out_r, child_out_w;
  if (!CreatePipe(&child_in_r, &child_in_w, &sa, 0) ||
      !CreatePipe(&child_out_r, &child_out_w, &sa, 0))
    return false;
  SetHandleInformation(child_in_w, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(child_out_r, HANDLE_FLAG_INHERIT, 0);
  char cmd[8192] = "";
  for (size_t i = 0; argv[i]; ++i) {
    strncat(cmd, i ? " \"" : "\"", sizeof(cmd) - strlen(cmd) - 1);
    strncat(cmd, argv[i], sizeof(cmd) - strlen(cmd) - 1);
    strncat(cmd, "\"", sizeof(cmd) - strlen(cmd) - 1);
  }
  STARTUPINFOA si = {0};
  PROCESS_INFORMATION pi = {0};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = child_in_r;
  si.hStdOutput = child_out_w;
  si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                           NULL, NULL, &si, &pi);
  CloseHandle(child_in_r);
  CloseHandle(child_out_w);
  if (!ok)
    return false;
  CloseHandle(pi.hThread);
  be->process = pi.hProcess;
  be->in = _fdopen(_open_osfhandle((intptr_t)child_in_w, _O_BINARY), "wb");
  be->out = _fdopen(_open_osfhandle((intptr_t)child_out_r, _O_BINARY), "rb");
#else
  int to_child[2], from_child[2];
  if (pipe(to_child) != 0)
    return false;
  if (pipe(from_child) != 0) {
    close(to_child[0]);
    close(to_child[1]);
    return false;
  }
  pid_t pid = fork();
  if (pid < 0)
    return false;
  if (pid == 0) {
    dup2(to_child[0], STDIN_FILENO);
    dup2(from_child[1], STDOUT_FILENO);
    close(to_child[0]);
    close(to_child[1]);
    close(from_child[0]);
    close(from_child[1]);
    execv(debugger, (char *const *)argv);
    _exit(127);
  }
  close(to_child[0]);
  close(from_child[1]);
  be->pid = pid;
  be->in = fdopen(to_child[1], "wb");
  be->out = fdopen(from_child[0], "rb");
#endif
  return be->in && be->out;
}

/*
 * Build step for launch requests
 */

typedef struct {
  char *build_dir;
  char *built_source; /* temp .ny written for an .nshape fixture */
  char *fixture;      /* the .nshape the user opened */
} dap_build_t;

static dap_build_t build_state;

static bool has_suffix(const char *s, const char *suffix) {
  size_t a = strlen(s), b = strlen(suffix);
  return a >= b && strcmp(s + a - b, suffix) == 0;
}

static char *make_build_dir(void) {
  const char *tmp = ny_get_temp_dir();
  char path[4096];
  for (int attempt = 0; attempt < 100; ++attempt) {
#ifdef _WIN32
    unsigned long id = GetCurrentProcessId();
#else
    unsigned long id = (unsigned long)getpid();
#endif
    snprintf(path, sizeof(path), "%s/nytrix-dap-%lu-%d",
             tmp && *tmp ? tmp : ".", id, attempt);
#ifdef _WIN32
    if (CreateDirectoryA(path, NULL))
#else
    if (mkdir(path, 0700) == 0)
#endif
      return strdup(path);
  }
  return NULL;
}

/*
 * The `source ny` block of an .nshape fixture containing `line` (else the
 * first), written with leading blank lines so line numbers match the fixture.
 */
static bool extract_nshape_source(const char *fixture, long line,
                                  const char *dest) {
  FILE *in = fopen(fixture, "rb");
  if (!in)
    return false;
  dap_buf_t text = {0};
  char chunk[4096];
  size_t n;
  while ((n = fread(chunk, 1, sizeof(chunk), in)) > 0)
    buf_add_n(&text, chunk, n);
  fclose(in);
  if (!text.data)
    return false;
  long current = 0, best_start = -1, best_end = -1;
  char terminator[128] = "";
  long open_line = -1;
  for (char *p = text.data; *p;) {
    char *eol = strchr(p, '\n');
    size_t len = eol ? (size_t)(eol - p) : strlen(p);
    char row[1024];
    snprintf(row, sizeof(row), "%.*s", (int)(len < 1023 ? len : 1023), p);
    char *r = row;
    while (*r == ' ' || *r == '\t')
      ++r;
    size_t rl = strlen(r);
    while (rl && (r[rl - 1] == '\r' || r[rl - 1] == ' '))
      r[--rl] = '\0';
    if (open_line < 0 && strncmp(r, "source ny", 9) == 0) {
      const char *marker = strstr(r, "<<");
      if (marker) {
        marker += 2;
        while (*marker == '\'' || *marker == '"' || *marker == ' ')
          ++marker;
        size_t m = 0;
        while (marker[m] && marker[m] != '\'' && marker[m] != '"' &&
               m < sizeof(terminator) - 1)
          ++m;
        snprintf(terminator, sizeof(terminator), "%.*s", (int)m, marker);
        open_line = current;
      }
    } else if (open_line >= 0 && strcmp(r, terminator) == 0) {
      bool contains = line >= open_line && line <= current;
      if (best_start < 0 || contains) {
        best_start = open_line + 1;
        best_end = current;
      }
      open_line = -1;
      if (contains)
        break;
    }
    ++current;
    if (!eol)
      break;
    p = eol + 1;
  }
  if (best_start < 0) {
    free(text.data);
    return false;
  }
  FILE *out = fopen(dest, "wb");
  if (!out) {
    free(text.data);
    return false;
  }
  long row = 0;
  for (char *p = text.data; *p && row < best_end;) {
    char *eol = strchr(p, '\n');
    size_t len = eol ? (size_t)(eol - p) + 1 : strlen(p);
    if (row >= best_start)
      fwrite(p, 1, len, out);
    else
      fputc('\n', out);
    ++row;
    if (!eol)
      break;
    p = eol + 1;
  }
  fclose(out);
  free(text.data);
  return true;
}

/*
 * Compiles a .ny/.nshape program into an executable with debug info.
 * Returns the executable path, or NULL after reporting the failure.
 */
static char *build_program(const char *program, const char *arguments,
                           const char *compiler_override) {
  char *ny = find_compiler(compiler_override);
  if (!ny) {
    send_output("stderr", "ny-dap: cannot find the ny compiler (set "
                          "NYTRIX_NY or put ny on PATH)\n");
    return NULL;
  }
  if (!build_state.build_dir)
    build_state.build_dir = make_build_dir();
  if (!build_state.build_dir) {
    free(ny);
    send_output("stderr", "ny-dap: cannot create a build directory\n");
    return NULL;
  }
  const char *source = program;
  if (has_suffix(program, ".nshape")) {
    const char *line_value = json_member(arguments, "sourceLine");
    long line = line_value ? strtol(line_value, NULL, 10) : 0;
    char dest[4096];
    const char *base = strrchr(program, '/');
    base = base ? base + 1 : program;
    snprintf(dest, sizeof(dest), "%s/%s.ny", build_state.build_dir, base);
    if (!extract_nshape_source(program, line, dest)) {
      free(ny);
      send_output("stderr", "ny-dap: no `source ny` block to debug in this "
                            "fixture\n");
      return NULL;
    }
    free(build_state.built_source);
    free(build_state.fixture);
    build_state.built_source = strdup(dest);
    build_state.fixture = strdup(program);
    source = build_state.built_source;
  }
  char exe[4096];
  snprintf(exe, sizeof(exe), "%s/program%s", build_state.build_dir,
           DAP_EXE_SUFFIX);
  const char *argv[64];
  size_t argc = 0;
  argv[argc++] = ny;
  argv[argc++] = "-O0";
  argv[argc++] = "-g";
  argv[argc++] = "--debug-locals";
  argv[argc++] = "--dwarf-version=5";
  argv[argc++] = "-no-strip";
  char *extra[32];
  size_t extra_n = 0;
  const char *list = json_member(arguments, "compilerArgs");
  if (list && *json_ws(list) == '[') {
    for (const char *p = json_ws(json_ws(list) + 1);
         *p == '"' && extra_n < 32 && argc < 56;) {
      extra[extra_n] = json_string(p);
      argv[argc++] = extra[extra_n++];
      p = json_skip(p);
      if (!p)
        break;
      p = json_ws(p);
      if (*p == ',')
        p = json_ws(p + 1);
    }
  }
  argv[argc++] = "-o";
  argv[argc++] = exe;
  argv[argc++] = source;
  argv[argc] = NULL;
  dap_buf_t line = {0};
  buf_add(&line, "ny-dap: building");
  for (size_t i = 0; i < argc; ++i) {
    buf_add(&line, " ");
    buf_add(&line, argv[i]);
  }
  buf_add(&line, "\n");
  send_output("console", line.data);
  free(line.data);
  char *captured = NULL;
  int status = ny_process_capture(argv, &captured, false);
  if (captured && *captured)
    send_output(status == 0 ? "console" : "stderr", captured);
  free(captured);
  for (size_t i = 0; i < extra_n; ++i)
    free(extra[i]);
  free(ny);
  if (status != 0)
    return NULL;
  return strdup(exe);
}

/*
 * Replaces every occurrence of JSON-escaped `from` with JSON-escaped `to`.
 */
static char *replace_path(const char *body, const char *from, const char *to) {
  if (!from || !to || !*from)
    return NULL;
  dap_buf_t ef = {0}, et = {0}, out = {0};
  buf_add_json_string(&ef, from);
  buf_add_json_string(&et, to);
  /*
   * Match without the surrounding quotes: a path can sit inside a string.
   */
  ef.data[ef.len - 1] = '\0';
  const char *needle = ef.data + 1;
  size_t needle_len = ef.len - 2;
  bool changed = false;
  for (const char *p = body;;) {
    const char *hit = strstr(p, needle);
    if (!hit) {
      buf_add(&out, p);
      break;
    }
    buf_add_n(&out, p, (size_t)(hit - p));
    buf_add_n(&out, et.data + 1, et.len - 2);
    p = hit + needle_len;
    changed = true;
  }
  free(ef.data);
  free(et.data);
  if (!changed) {
    free(out.data);
    return NULL;
  }
  return out.data;
}

/*
 * Handles a client request before it reaches the debugger. Returns the
 * (possibly rewritten) message to forward, or NULL when it was answered here.
 */
static char *prepare_request(char *body, const char *compiler) {
  char *command = json_string(json_member(body, "command"));
  if (!command)
    return body;
  if (strcmp(command, "launch") == 0) {
    const char *arguments = json_member(body, "arguments");
    const char *program_value = json_member(arguments, "program");
    char *program = json_string(program_value);
    if (program &&
        (has_suffix(program, ".ny") || has_suffix(program, ".nshape"))) {
      char *exe = build_program(program, arguments, compiler);
      if (!exe) {
        send_error_response(body, "Building the program failed; see the "
                                  "Debug Console.");
        free(program);
        free(command);
        free(body);
        return NULL;
      }
      const char *value_end = json_skip(program_value);
      dap_buf_t rewritten = {0};
      buf_add_n(&rewritten, body, (size_t)(json_ws(program_value) - body));
      buf_add_json_string(&rewritten, exe);
      buf_add(&rewritten, value_end);
      free(exe);
      free(body);
      body = rewritten.data;
    }
    free(program);
  } else if (build_state.fixture && build_state.built_source) {
    char *mapped =
        replace_path(body, build_state.fixture, build_state.built_source);
    if (mapped) {
      free(body);
      body = mapped;
    }
  }
  free(command);
  return body;
}

/*
 * Debugger -> client pump
 */

static dap_backend_t backend;

#ifdef _WIN32
static DWORD WINAPI pump_backend(LPVOID unused)
#else
static void *pump_backend(void *unused)
#endif
{
  (void)unused;
  size_t len = 0;
  char *msg;
  while ((msg = read_frame(backend.out, &len)) != NULL) {
    log_line("<- %s", msg);
    if (build_state.fixture && build_state.built_source) {
      char *mapped =
          replace_path(msg, build_state.built_source, build_state.fixture);
      if (mapped) {
        free(msg);
        msg = mapped;
        len = strlen(msg);
      }
    }
    to_client(msg, len);
    free(msg);
  }
  log_line("debugger closed its output");
  exit(0);
#ifdef _WIN32
  return 0;
#else
  return NULL;
#endif
}

static void remove_build_dir(void) {
  if (!build_state.build_dir)
    return;
  char path[4096];
  const char *names[] = {"program" DAP_EXE_SUFFIX, NULL};
  for (size_t i = 0; names[i]; ++i) {
    snprintf(path, sizeof(path), "%s/%s", build_state.build_dir, names[i]);
    remove(path);
  }
  if (build_state.built_source)
    remove(build_state.built_source);
#ifdef _WIN32
  RemoveDirectoryA(build_state.build_dir);
#else
  rmdir(build_state.build_dir);
#endif
}

static void usage(void) {
  fputs("usage: ny-dap [--debugger PATH] [--ny PATH] [--log FILE]\n"
        "Debug Adapter Protocol over stdin/stdout. A launch whose program is\n"
        "a .ny or .nshape file is built with debug info first.\n",
        stderr);
}

int main(int argc, char **argv) {
  const char *debugger_arg = NULL, *compiler_arg = NULL, *log_arg = NULL;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--debugger") == 0 && i + 1 < argc)
      debugger_arg = argv[++i];
    else if (strcmp(argv[i], "--ny") == 0 && i + 1 < argc)
      compiler_arg = argv[++i];
    else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc)
      log_arg = argv[++i];
    else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      usage();
      return 0;
    } else {
      usage();
      return 2;
    }
  }
#ifdef _WIN32
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
  InitializeCriticalSection(&client_lock);
#else
  signal(SIGPIPE, SIG_IGN);
#endif
  client_out = stdout;
  if (log_arg)
    dap_log = fopen(log_arg, "ab");
  atexit(remove_build_dir);

  char *debugger = find_debugger(debugger_arg);
  bool started = debugger && spawn_backend(&backend, debugger);
  log_line("debugger %s: %s", debugger ? debugger : "(none)",
           started ? "started" : "unavailable");
  if (started) {
#ifdef _WIN32
    HANDLE thread = CreateThread(NULL, 0, pump_backend, NULL, 0, NULL);
    if (thread)
      CloseHandle(thread);
#else
    pthread_t thread;
    pthread_create(&thread, NULL, pump_backend, NULL);
    pthread_detach(thread);
#endif
  }

  size_t len = 0;
  char *msg;
  while ((msg = read_frame(stdin, &len)) != NULL) {
    log_line("-> %s", msg);
    if (!started) {
      char reason[512];
      snprintf(reason, sizeof(reason),
               "No DAP debugger found%s%s. Install GDB 14+ or lldb-dap, or "
               "set NYTRIX_DEBUGGER.",
               debugger ? ": " : "", debugger ? debugger : "");
      char *command = json_string(json_member(msg, "command"));
      if (command && strcmp(command, "disconnect") != 0)
        send_error_response(msg, reason);
      free(command);
      free(msg);
      continue;
    }
    msg = prepare_request(msg, compiler_arg);
    if (!msg)
      continue;
    write_frame(backend.in, msg, strlen(msg));
    free(msg);
  }
  free(debugger);
  if (started) {
    fclose(backend.in);
#ifdef _WIN32
    WaitForSingleObject(backend.process, 5000);
#else
    waitpid(backend.pid, NULL, 0);
#endif
  }
  return 0;
}
