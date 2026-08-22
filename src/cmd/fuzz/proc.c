/*
 * Fuzz process runner: spawns and monitors fuzzer worker subprocesses
 * with timeouts, resource limits, and output capture for crash detection.
 */
#include "core.h"
#include <strings.h>

#ifdef _WIN32
#include <io.h>
#include <process.h>
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#include <poll.h>
#endif

void proc_result_free(proc_result_t *r) {
  free(r->out);
  free(r->err);
  r->out = NULL;
  r->err = NULL;
}

#ifdef _WIN32

static void set_nonblock(int fd) {
  (void)fd;
}

static void drain_fd(int fd, str_buf_t *buf, bool *open_flag) {
  char tmp[4096];
  for (;;) {
    int n = _read(fd, tmp, sizeof(tmp));
    if (n > 0) {
      (void)sb_append_n(buf, tmp, (size_t)n);
      continue;
    }
    if (n == 0) {
      _close(fd);
      *open_flag = false;
      return;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return;
    _close(fd);
    *open_flag = false;
    return;
  }
}

static bool proc_path_exists(const char *path) {
  struct _stat st;
  return path && *path && _stat(path, &st) == 0;
}

static bool proc_looks_like_nytrix_root(const char *path) {
  char cmake[4096], src[4096], fuzz[4096], tests[4096], readme[4096];
  if (!path || !*path) return false;
  ny_join_path(cmake, sizeof(cmake), path, "CMakeLists.txt");
  ny_join_path(src, sizeof(src), path, "src");
  ny_join_path(fuzz, sizeof(fuzz), path, "src\\cmd\\fuzz");
  ny_join_path(tests, sizeof(tests), path, "etc\\tests");
  ny_join_path(readme, sizeof(readme), path, "README.md");
  return proc_path_exists(cmake) && proc_path_exists(src) &&
         proc_path_exists(fuzz) && proc_path_exists(tests) &&
         proc_path_exists(readme);
}

static bool proc_find_nytrix_root_from_path(const char *start, char *out, size_t out_sz) {
  if (!start || !*start || !out || !out_sz) return false;
  char cur[4096];
  snprintf(cur, sizeof(cur), "%s", start);
  while (1) {
    if (proc_looks_like_nytrix_root(cur)) {
      snprintf(out, out_sz, "%s", cur);
      return true;
    }
    char *bslash = strrchr(cur, '\\');
    char *slash = strrchr(cur, '/');
    char *sep = bslash > slash ? bslash : slash;
    if (!sep || sep == cur) break;
    *sep = '\0';
  }
  return false;
}

static bool proc_find_nytrix_root(char *const argv[], char *out, size_t out_sz) {
  const char *env = getenv("NYTRIX_ROOT");
  if (env && *env && proc_find_nytrix_root_from_path(env, out, out_sz)) return true;

  char exe[4096];
  DWORD n = GetModuleFileNameA(NULL, exe, (DWORD)(sizeof(exe) - 1u));
  if (n > 0 && n < sizeof(exe)) {
    exe[n] = '\0';
    if (proc_find_nytrix_root_from_path(exe, out, out_sz)) return true;
  }

  if (argv && argv[0] && *argv[0]) {
    if (argv[0][0] == '\\' || (argv[0][1] == ':')) {
      if (proc_find_nytrix_root_from_path(argv[0], out, out_sz)) return true;
    } else {
      char cwd_buf[4096], abs_buf[4096];
      if (getcwd(cwd_buf, sizeof(cwd_buf))) {
        ny_join_path(abs_buf, sizeof(abs_buf), cwd_buf, argv[0]);
        if (proc_find_nytrix_root_from_path(abs_buf, out, out_sz))
          return true;
      }
    }
  }

  const char *pwd = getenv("PWD");
  if (pwd && *pwd && proc_find_nytrix_root_from_path(pwd, out, out_sz)) return true;
  char cwd_buf[4096];
  return getcwd(cwd_buf, sizeof(cwd_buf)) &&
         proc_find_nytrix_root_from_path(cwd_buf, out, out_sz);
}

#else /* POSIX */

static void set_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void drain_fd(int fd, str_buf_t *buf, bool *open_flag) {
  char tmp[4096];
  for (;;) {
    ssize_t n = read(fd, tmp, sizeof(tmp));
    if (n > 0) {
      (void)sb_append_n(buf, tmp, (size_t)n);
      continue;
    }
    if (n == 0) {
      close(fd);
      *open_flag = false;
      return;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return;
    close(fd);
    *open_flag = false;
    return;
  }
}

static bool proc_path_exists(const char *path) {
  struct stat st;
  return path && *path && stat(path, &st) == 0;
}

static bool proc_looks_like_nytrix_root(const char *path) {
  char cmake[4096], src[4096], fuzz[4096], tests[4096], readme[4096];
  if (!path || !*path || path[0] != '/') return false;
  ny_join_path(cmake, sizeof(cmake), path, "CMakeLists.txt");
  ny_join_path(src, sizeof(src), path, "src");
  ny_join_path(fuzz, sizeof(fuzz), path, "src/cmd/fuzz");
  ny_join_path(tests, sizeof(tests), path, "etc/tests");
  ny_join_path(readme, sizeof(readme), path, "README.md");
  return proc_path_exists(cmake) && proc_path_exists(src) &&
         proc_path_exists(fuzz) && proc_path_exists(tests) &&
         proc_path_exists(readme);
}

static bool proc_find_nytrix_root_from_path(const char *start, char *out, size_t out_sz) {
  if (!start || !*start || !out || !out_sz) return false;
  char cur[4096];
  snprintf(cur, sizeof(cur), "%s", start);
  while (1) {
    if (proc_looks_like_nytrix_root(cur)) {
      snprintf(out, out_sz, "%s", cur);
      return true;
    }
    char *slash = strrchr(cur, '/');
    if (!slash || slash == cur) break;
    *slash = '\0';
  }
  return false;
}

static bool proc_find_nytrix_root(char *const argv[], char *out, size_t out_sz) {
  const char *env = getenv("NYTRIX_ROOT");
  if (env && *env && proc_find_nytrix_root_from_path(env, out, out_sz)) return true;

  char exe[4096];
  ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1u);
  if (n > 0 && (size_t)n < sizeof(exe)) {
    exe[n] = '\0';
    if (proc_find_nytrix_root_from_path(exe, out, out_sz)) return true;
  }

  if (argv && argv[0] && *argv[0]) {
    if (argv[0][0] == '/') {
      if (proc_find_nytrix_root_from_path(argv[0], out, out_sz)) return true;
    } else {
      char cwd_buf[4096], abs_buf[4096];
      if (getcwd(cwd_buf, sizeof(cwd_buf))) {
        ny_join_path(abs_buf, sizeof(abs_buf), cwd_buf, argv[0]);
        if (proc_find_nytrix_root_from_path(abs_buf, out, out_sz))
          return true;
      }
    }
  }

  const char *pwd = getenv("PWD");
  if (pwd && *pwd && proc_find_nytrix_root_from_path(pwd, out, out_sz)) return true;
  char cwd_buf[4096];
  return getcwd(cwd_buf, sizeof(cwd_buf)) &&
         proc_find_nytrix_root_from_path(cwd_buf, out, out_sz);
}

#endif /* _WIN32 */

static void proc_set_path(char *out, size_t out_sz, const char *root, const char *leaf) {
  if (!out || !out_sz) return;
  out[0] = '\0';
  if (!root || !*root || !leaf || !*leaf) return;
  ny_join_path(out, out_sz, root, leaf);
  ny_ensure_dir_recursive(out);
}

static void proc_prepare_child_cache_env(char *const argv[], char *root, size_t root_sz,
                                         char *tmp, size_t tmp_sz,
                                         char *scratch, size_t scratch_sz,
                                         char *xdg, size_t xdg_sz,
                                         char *nytrix_cache, size_t nytrix_cache_sz) {
  if (root && root_sz) root[0] = '\0';
  if (tmp && tmp_sz) tmp[0] = '\0';
  if (scratch && scratch_sz) scratch[0] = '\0';
  if (xdg && xdg_sz) xdg[0] = '\0';
  if (nytrix_cache && nytrix_cache_sz) nytrix_cache[0] = '\0';
  if (ny_env_is_truthy(getenv("NYTRIX_KEEP_EXTERNAL_TMP"))) return;
  if (!proc_find_nytrix_root(argv, root, root_sz) || !root[0]) return;

  const char *tmp_override = getenv("NYTRIX_CHILD_TMPDIR");
  if (tmp_override && *tmp_override) {
    snprintf(tmp, tmp_sz, "%s", tmp_override);
    ny_ensure_dir_recursive(tmp);
  } else {
    proc_set_path(tmp, tmp_sz, root, "build/cache/tmp");
  }
  const char *scratch_override = getenv("NYTRIX_SCRATCH_ROOT");
  if (scratch_override && *scratch_override) {
    snprintf(scratch, scratch_sz, "%s", scratch_override);
    ny_ensure_dir_recursive(scratch);
  } else {
    proc_set_path(scratch, scratch_sz, root, "build/cache/scratch");
  }
  proc_set_path(xdg, xdg_sz, root, "build/cache/xdg");
  proc_set_path(nytrix_cache, nytrix_cache_sz, root, "build/cache/nytrix");
}

#ifdef _WIN32

static void build_env_block(char *const envp[], char *buf, size_t buf_sz) {
  buf[0] = '\0';
  size_t pos = 0;
  for (int i = 0; envp && envp[i]; ++i) {
    size_t len = strlen(envp[i]);
    if (pos + len + 2 >= buf_sz) break;
    memcpy(buf + pos, envp[i], len);
    pos += len;
    buf[pos++] = '\0';
  }
  buf[pos] = '\0';
}

proc_result_t run_proc(char *const argv[], const char *cwd, double timeout_s) {
  proc_result_t result;
  memset(&result, 0, sizeof(result));
  result.rc = 127;
  str_buf_t out = {0}, err = {0};
  double start = now_ms();
  char child_root[4096], child_tmp[4096], child_scratch[4096];
  char child_xdg[4096], child_nytrix_cache[4096];
  proc_prepare_child_cache_env(argv, child_root, sizeof(child_root),
                               child_tmp, sizeof(child_tmp),
                               child_scratch, sizeof(child_scratch),
                               child_xdg, sizeof(child_xdg),
                               child_nytrix_cache, sizeof(child_nytrix_cache));

  HANDLE out_read = NULL, out_write = NULL;
  HANDLE err_read = NULL, err_write = NULL;
  SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
  if (!CreatePipe(&out_read, &out_write, &sa, 0) ||
      !CreatePipe(&err_read, &err_write, &sa, 0)) {
    (void)sb_append(&err, "CreatePipe failed");
    result.err = sb_take(&err);
    result.out = sb_take(&out);
    result.elapsed_ms = now_ms() - start;
    return result;
  }
  SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(err_read, HANDLE_FLAG_INHERIT, 0);

  char cmd_line[8192];
  cmd_line[0] = '\0';
  if (argv && argv[0]) {
    snprintf(cmd_line, sizeof(cmd_line), "\"%s\"", argv[0]);
    for (int i = 1; argv[i]; ++i) {
      size_t clen = strlen(cmd_line);
      snprintf(cmd_line + clen, sizeof(cmd_line) - clen, " \"%s\"", argv[i]);
    }
  }

  char env_block[16384];
  char *envp[64];
  int eidx = 0;
  static char e_root[4096], e_tmp[4096], e_tmp2[4096], e_tmp3[4096];
  static char e_tmp4[4096], e_scratch[4096], e_xdg[4096], e_cache[4096];
  if (child_root[0]) {
    snprintf(e_root, sizeof(e_root), "NYTRIX_ROOT=%s", child_root);
    envp[eidx++] = e_root;
  }
  if (child_tmp[0]) {
    snprintf(e_tmp, sizeof(e_tmp), "TMPDIR=%s", child_tmp);
    snprintf(e_tmp2, sizeof(e_tmp2), "TMP=%s", child_tmp);
    snprintf(e_tmp3, sizeof(e_tmp3), "TEMP=%s", child_tmp);
    snprintf(e_tmp4, sizeof(e_tmp4), "NYTRIX_CHILD_TMPDIR=%s", child_tmp);
    envp[eidx++] = e_tmp;
    envp[eidx++] = e_tmp2;
    envp[eidx++] = e_tmp3;
    envp[eidx++] = e_tmp4;
  }
  if (child_scratch[0]) {
    snprintf(e_scratch, sizeof(e_scratch), "NYTRIX_SCRATCH_ROOT=%s", child_scratch);
    envp[eidx++] = e_scratch;
  }
  if (child_xdg[0]) {
    snprintf(e_xdg, sizeof(e_xdg), "XDG_CACHE_HOME=%s", child_xdg);
    envp[eidx++] = e_xdg;
  }
  if (child_nytrix_cache[0]) {
    snprintf(e_cache, sizeof(e_cache), "NYTRIX_CACHE_DIR=%s", child_nytrix_cache);
    envp[eidx++] = e_cache;
  }
  envp[eidx] = NULL;
  build_env_block(envp, env_block, sizeof(env_block));

  STARTUPINFOA si = {sizeof(si)};
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = out_write;
  si.hStdError = err_write;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

  DWORD creation_flags = CREATE_NO_WINDOW;
  PROCESS_INFORMATION pi = {0};
  BOOL ok = CreateProcessA(NULL, cmd_line, NULL, NULL, TRUE, creation_flags,
                           env_block[0] ? env_block : NULL, cwd, &si, &pi);
  CloseHandle(out_write); out_write = NULL;
  CloseHandle(err_write); err_write = NULL;

  if (!ok) {
    CloseHandle(out_read);
    CloseHandle(err_read);
    (void)sb_append(&err, "CreateProcess failed");
    result.err = sb_take(&err);
    result.out = sb_take(&out);
    result.elapsed_ms = now_ms() - start;
    return result;
  }

  HANDLE proc_handle = pi.hProcess;
  DWORD proc_id = pi.dwProcessId;
  (void)proc_id;
  CloseHandle(pi.hThread);

  bool out_open = true, err_open = true, exited = false, term_sent = false;
  DWORD exit_code = 0;
  double deadline = start + timeout_s * 1000.0;
  double term_deadline = 0.0;
  (void)term_deadline;
  while (out_open || err_open || !exited) {
    if (!exited) {
      if (WaitForSingleObject(proc_handle, 0) == WAIT_OBJECT_0) {
        exited = true;
        GetExitCodeProcess(proc_handle, &exit_code);
      }
    }
    double now = now_ms();
    if (!exited && timeout_s > 0.0 && now >= deadline && !term_sent) {
      result.timed_out = true;
      term_sent = true;
      term_deadline = now + 1000.0;
      TerminateProcess(proc_handle, 124);
    }
    char tmp[4096];
    DWORD nread = 0;
    if (out_open) {
      if (ReadFile(out_read, tmp, sizeof(tmp), &nread, NULL) && nread > 0)
        (void)sb_append_n(&out, tmp, (size_t)nread);
      else
        out_open = false;
    }
    if (err_open) {
      if (ReadFile(err_read, tmp, sizeof(tmp), &nread, NULL) && nread > 0)
        (void)sb_append_n(&err, tmp, (size_t)nread);
      else
        err_open = false;
    }
    if (!out_open && !err_open && !exited) Sleep(1);
  }
  CloseHandle(out_read);
  CloseHandle(err_read);
  CloseHandle(proc_handle);

  if (result.timed_out) {
    result.rc = 124;
    char note[160];
    snprintf(note, sizeof(note), "\n[nytrix] timeout after %.2fs; killed process", timeout_s);
    (void)sb_append(&err, note);
  } else {
    result.rc = (int)exit_code;
  }
  result.out = sb_take(&out);
  result.err = sb_take(&err);
  result.elapsed_ms = now_ms() - start;
  return result;
}

#else /* POSIX */

proc_result_t run_proc(char *const argv[], const char *cwd, double timeout_s) {
  proc_result_t result;
  memset(&result, 0, sizeof(result));
  result.rc = 127;
  int out_pipe[2] = {-1, -1};
  int err_pipe[2] = {-1, -1};
  str_buf_t out = {0}, err = {0};
  double start = now_ms();
  char child_root[4096], child_tmp[4096], child_scratch[4096];
  char child_xdg[4096], child_nytrix_cache[4096];
  proc_prepare_child_cache_env(argv, child_root, sizeof(child_root),
                               child_tmp, sizeof(child_tmp),
                               child_scratch, sizeof(child_scratch),
                               child_xdg, sizeof(child_xdg),
                               child_nytrix_cache, sizeof(child_nytrix_cache));
  if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
    (void)sb_append(&err, "pipe failed");
    result.err = sb_take(&err);
    result.out = sb_take(&out);
    result.elapsed_ms = now_ms() - start;
    return result;
  }
  pid_t pid = fork();
  if (pid == 0) {
    (void)setpgid(0, 0);
    close(out_pipe[0]);
    close(err_pipe[0]);
    (void)dup2(out_pipe[1], STDOUT_FILENO);
    (void)dup2(err_pipe[1], STDERR_FILENO);
    close(out_pipe[1]);
    close(err_pipe[1]);
    if (child_root[0]) (void)setenv("NYTRIX_ROOT", child_root, 1);
    if (child_tmp[0]) {
      (void)setenv("TMPDIR", child_tmp, 1);
      (void)setenv("TMP", child_tmp, 1);
      (void)setenv("TEMP", child_tmp, 1);
      (void)setenv("NYTRIX_CHILD_TMPDIR", child_tmp, 1);
    }
    if (child_scratch[0]) (void)setenv("NYTRIX_SCRATCH_ROOT", child_scratch, 1);
    if (child_xdg[0]) (void)setenv("XDG_CACHE_HOME", child_xdg, 1);
    if (child_nytrix_cache[0]) (void)setenv("NYTRIX_CACHE_DIR", child_nytrix_cache, 1);
    if (cwd && *cwd && chdir(cwd) == 0) {
      char actual_cwd[4096];
      if (getcwd(actual_cwd, sizeof(actual_cwd)))
        (void)setenv("PWD", actual_cwd, 1);
      else
        (void)setenv("PWD", cwd, 1);
    }
    execvp(argv[0], argv);
    perror(argv[0]);
    _exit(127);
  }
  close(out_pipe[1]);
  close(err_pipe[1]);
  if (pid < 0) {
    close(out_pipe[0]);
    close(err_pipe[0]);
    (void)sb_append(&err, "fork failed");
    result.err = sb_take(&err);
    result.out = sb_take(&out);
    result.elapsed_ms = now_ms() - start;
    return result;
  }
  (void)setpgid(pid, pid);
  set_nonblock(out_pipe[0]);
  set_nonblock(err_pipe[0]);
  bool out_open = true, err_open = true, exited = false, term_sent = false;
  int status = 0;
  double deadline = start + timeout_s * 1000.0;
  double term_deadline = 0.0;
  while (out_open || err_open || !exited) {
    if (!exited) {
      pid_t got = waitpid(pid, &status, WNOHANG);
      if (got == pid) exited = true;
    }
    double now = now_ms();
    if (!exited && timeout_s > 0.0 && now >= deadline && !term_sent) {
      result.timed_out = true;
      term_sent = true;
      term_deadline = now + 1000.0;
      if (kill(-pid, SIGTERM) != 0) (void)kill(pid, SIGTERM);
    }
    if (!exited && term_sent && now >= term_deadline) {
      if (kill(-pid, SIGKILL) != 0) (void)kill(pid, SIGKILL);
      term_deadline = now + 1000000.0;
    }
    struct pollfd fds[2];
    nfds_t nfds = 0;
    if (out_open) {
      fds[nfds].fd = out_pipe[0];
      fds[nfds].events = POLLIN | POLLHUP | POLLERR;
      fds[nfds].revents = 0;
      ++nfds;
    }
    if (err_open) {
      fds[nfds].fd = err_pipe[0];
      fds[nfds].events = POLLIN | POLLHUP | POLLERR;
      fds[nfds].revents = 0;
      ++nfds;
    }
    if (nfds) (void)poll(fds, nfds, 20);
    if (out_open) drain_fd(out_pipe[0], &out, &out_open);
    if (err_open) drain_fd(err_pipe[0], &err, &err_open);
    if (!nfds && !exited) usleep(10000);
  }
  if (result.timed_out) {
    result.rc = 124;
    char note[160];
    snprintf(note, sizeof(note), "\n[nytrix] timeout after %.2fs; killed process group", timeout_s);
    (void)sb_append(&err, note);
  } else if (WIFEXITED(status)) {
    result.rc = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    result.rc = 128 + WTERMSIG(status);
  } else {
    result.rc = 1;
  }
  result.out = sb_take(&out);
  result.err = sb_take(&err);
  result.elapsed_ms = now_ms() - start;
  return result;
}

#endif /* !_WIN32 */

char *normalize_output_pair(const char *out, const char *err) {
  str_buf_t combined = {0};
  if (out) (void)sb_append(&combined, out);
  if (err && *err) {
    if (combined.len) (void)sb_append_c(&combined, '\n');
    (void)sb_append(&combined, err);
  }
  str_buf_t norm = {0};
  char *data = combined.data ? combined.data : strdup("");
  size_t len = combined.len;
  size_t start = 0;
  for (size_t i = 0; i <= len; ++i) {
    if (i != len && data[i] != '\n') continue;
    size_t a = start, b = i;
    while (a < b && isspace((unsigned char)data[a])) ++a;
    while (b > a && isspace((unsigned char)data[b - 1])) --b;
    if (b > a) {
      if (norm.len) (void)sb_append_c(&norm, '\n');
      (void)sb_append_n(&norm, data + a, b - a);
    }
    start = i + 1;
  }
  free(data);
  return sb_take(&norm);
}

static int dbl_cmp(const void *a, const void *b) {
  double x = *(const double *)a;
  double y = *(const double *)b;
  return (x > y) - (x < y);
}

void run_many_result_free(run_many_result_t *r) {
  free(r->out);
  free(r->err);
  free(r->normalized);
  memset(r, 0, sizeof(*r));
}

run_many_result_t run_binary_many_native(const char *root, const char *path,
                                                double timeout_s, int runs, int warmup) {
  run_many_result_t r;
  memset(&r, 0, sizeof(r));
  runs = runs < 1 ? 1 : runs;
  warmup = warmup < 0 ? 0 : warmup;
  double *samples = (double *)calloc((size_t)runs, sizeof(double));
  char *baseline = NULL;
  int sample_count = 0;
  for (int idx = 0; idx < warmup + runs; ++idx) {
    char *argv[] = {(char *)path, NULL};
    proc_result_t pr = run_proc(argv, root, timeout_s);
    char *normalized = normalize_output_pair(pr.out, pr.err);
    if (pr.rc != 0) {
      r.rc = pr.rc;
      r.out = pr.out ? strdup(pr.out) : strdup("");
      r.err = pr.err ? strdup(pr.err) : strdup("");
      r.normalized = normalized;
      proc_result_free(&pr);
      free(baseline);
      free(samples);
      return r;
    }
    if (!baseline) {
      baseline = strdup(normalized ? normalized : "");
    } else if (strcmp(normalized ? normalized : "", baseline) != 0) {
      str_buf_t msg = {0};
      (void)sb_append(&msg, "unstable output: expected ");
      (void)sb_append(&msg, baseline);
      (void)sb_append(&msg, ", got ");
      (void)sb_append(&msg, normalized ? normalized : "");
      r.rc = 1;
      r.out = pr.out ? strdup(pr.out) : strdup("");
      r.err = sb_take(&msg);
      r.normalized = normalized;
      proc_result_free(&pr);
      free(baseline);
      free(samples);
      return r;
    }
    free(r.out);
    free(r.err);
    free(r.normalized);
    r.out = pr.out ? strdup(pr.out) : strdup("");
    r.err = pr.err ? strdup(pr.err) : strdup("");
    r.normalized = normalized;
    if (idx >= warmup && sample_count < runs) samples[sample_count++] = pr.elapsed_ms;
    proc_result_free(&pr);
  }
  if (sample_count) {
    qsort(samples, (size_t)sample_count, sizeof(double), dbl_cmp);
    if (sample_count % 2) r.median_ms = samples[sample_count / 2];
    else r.median_ms = (samples[sample_count / 2 - 1] + samples[sample_count / 2]) / 2.0;
  }
  r.rc = 0;
  free(baseline);
  free(samples);
  return r;
}
