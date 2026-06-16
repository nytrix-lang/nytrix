#include "core.h"
#include <strings.h>

void proc_result_free(proc_result_t *r) {
  free(r->out);
  free(r->err);
  r->out = NULL;
  r->err = NULL;
}

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

static bool proc_looks_like_nynth_root(const char *path) {
  char src[4096], cli[4096], core[4096], shapes[4096], makefile[4096];
  if (!path || !*path || path[0] != '/') return false;
  ny_join_path(src, sizeof(src), path, "src");
  ny_join_path(cli, sizeof(cli), path, "src/cli.c");
  ny_join_path(core, sizeof(core), path, "src/core.h");
  ny_join_path(shapes, sizeof(shapes), path, "shapes");
  ny_join_path(makefile, sizeof(makefile), path, "Makefile");
  return proc_path_exists(src) && proc_path_exists(cli) &&
         proc_path_exists(core) && proc_path_exists(shapes) &&
         proc_path_exists(makefile);
}

static bool proc_find_nynth_root_from_path(const char *start, char *out, size_t out_sz) {
  if (!start || !*start || !out || !out_sz) return false;
  char cur[4096];
  snprintf(cur, sizeof(cur), "%s", start);
  while (1) {
    if (proc_looks_like_nynth_root(cur)) {
      snprintf(out, out_sz, "%s", cur);
      return true;
    }
    char *slash = strrchr(cur, '/');
    if (!slash || slash == cur) break;
    *slash = '\0';
  }
  return false;
}

static bool proc_find_nynth_root(char *const argv[], char *out, size_t out_sz) {
  const char *env = getenv("NYNTH_ROOT");
  if (env && *env && proc_find_nynth_root_from_path(env, out, out_sz)) return true;

  char exe[4096];
  ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1u);
  if (n > 0 && (size_t)n < sizeof(exe)) {
    exe[n] = '\0';
    if (proc_find_nynth_root_from_path(exe, out, out_sz)) return true;
  }

  if (argv && argv[0] && *argv[0]) {
    if (argv[0][0] == '/') {
      if (proc_find_nynth_root_from_path(argv[0], out, out_sz)) return true;
    } else {
      char cwd_buf[4096], abs_buf[4096];
      if (getcwd(cwd_buf, sizeof(cwd_buf))) {
        ny_join_path(abs_buf, sizeof(abs_buf), cwd_buf, argv[0]);
        if (proc_find_nynth_root_from_path(abs_buf, out, out_sz))
          return true;
      }
    }
  }

  const char *pwd = getenv("PWD");
  if (pwd && *pwd && proc_find_nynth_root_from_path(pwd, out, out_sz)) return true;
  char cwd_buf[4096];
  return getcwd(cwd_buf, sizeof(cwd_buf)) &&
         proc_find_nynth_root_from_path(cwd_buf, out, out_sz);
}

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
  if (ny_env_is_truthy(getenv("NYNTH_KEEP_EXTERNAL_TMP"))) return;
  if (!proc_find_nynth_root(argv, root, root_sz) || !root[0]) return;

  const char *tmp_override = getenv("NYNTH_CHILD_TMPDIR");
  if (tmp_override && *tmp_override) {
    snprintf(tmp, tmp_sz, "%s", tmp_override);
    ny_ensure_dir_recursive(tmp);
  } else {
    proc_set_path(tmp, tmp_sz, root, "build/cache/tmp");
  }
  const char *scratch_override = getenv("NYNTH_SCRATCH_ROOT");
  if (scratch_override && *scratch_override) {
    snprintf(scratch, scratch_sz, "%s", scratch_override);
    ny_ensure_dir_recursive(scratch);
  } else {
    proc_set_path(scratch, scratch_sz, root, "build/cache/scratch");
  }
  proc_set_path(xdg, xdg_sz, root, "build/cache/xdg");
  proc_set_path(nytrix_cache, nytrix_cache_sz, root, "build/cache/nytrix");
}

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
    if (child_root[0]) (void)setenv("NYNTH_ROOT", child_root, 1);
    if (child_tmp[0]) {
      (void)setenv("TMPDIR", child_tmp, 1);
      (void)setenv("TMP", child_tmp, 1);
      (void)setenv("TEMP", child_tmp, 1);
      (void)setenv("NYNTH_CHILD_TMPDIR", child_tmp, 1);
    }
    if (child_scratch[0]) (void)setenv("NYNTH_SCRATCH_ROOT", child_scratch, 1);
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
    snprintf(note, sizeof(note), "\n[nynth] timeout after %.2fs; killed process group", timeout_s);
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
