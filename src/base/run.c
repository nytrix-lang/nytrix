/*
 * nytrix: safe-run resource limits
 *
 * Applies POSIX setrlimit() resource limits before user code starts.
 * No-op when safe_run fields are zero (no limits configured).
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "base/options.h"
#include "base/util.h"
#include "wire/build.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifndef _WIN32
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <unistd.h>
#include <sys/wait.h>
#endif

/* Check whether any safe-run limit is actually configured. */
static bool ny_safe_run_any_set(const ny_safe_run_t *sr) {
  return sr->cpu_seconds > 0 || sr->wall_seconds > 0 ||
         sr->max_rss_bytes > 0 ||
         sr->max_files > 0 || sr->max_processes > 0 ||
         sr->max_output_bytes > 0 ||
         sr->telemetry ||
         sr->contain_process_group;
}

#ifdef _WIN32

int ny_safe_run_apply_limits(const ny_safe_run_t *sr) {
  if (!sr || !ny_safe_run_any_set(sr))
    return 0;
  /* Windows limits belong to a Job Object and are installed by spawn(). */
  return 0;
}

static bool ny_win_cmd_append(char **buf, size_t *len, size_t *cap,
                              const char *s, size_t n) {
  if (*len + n + 1 > *cap) {
    size_t next = *cap ? *cap : 256;
    while (next < *len + n + 1)
      next *= 2;
    char *grown = (char *)realloc(*buf, next);
    if (!grown)
      return false;
    *buf = grown;
    *cap = next;
  }
  memcpy(*buf + *len, s, n);
  *len += n;
  (*buf)[*len] = '\0';
  return true;
}

static bool ny_win_cmd_arg(char **buf, size_t *len, size_t *cap,
                           const char *arg) {
  if (*len && !ny_win_cmd_append(buf, len, cap, " ", 1))
    return false;
  if (!ny_win_cmd_append(buf, len, cap, "\"", 1))
    return false;
  size_t slashes = 0;
  for (const char *p = arg ? arg : "";; ++p) {
    if (*p == '\\') {
      ++slashes;
      continue;
    }
    if (*p == '\"' || *p == '\0') {
      size_t count = slashes * 2 + (*p == '\"' ? 1 : 0);
      for (size_t i = 0; i < count; ++i)
        if (!ny_win_cmd_append(buf, len, cap, "\\", 1))
          return false;
      slashes = 0;
      if (*p == '\0')
        break;
      if (!ny_win_cmd_append(buf, len, cap, "\"", 1))
        return false;
      continue;
    }
    while (slashes) {
      if (!ny_win_cmd_append(buf, len, cap, "\\", 1))
        return false;
      --slashes;
    }
    if (!ny_win_cmd_append(buf, len, cap, p, 1))
      return false;
  }
  return ny_win_cmd_append(buf, len, cap, "\"", 1);
}

static uint64_t ny_win_now_ms(void) {
  return (uint64_t)GetTickCount64();
}

static void ny_win_forward_pipe(HANDLE pipe, HANDLE dst, uint64_t limit,
                                uint64_t *total, bool *breached) {
  for (;;) {
    DWORD available = 0;
    if (!PeekNamedPipe(pipe, NULL, 0, NULL, &available, NULL) || !available)
      return;
    char buf[4096];
    DWORD want = available < sizeof(buf) ? available : (DWORD)sizeof(buf);
    DWORD got = 0;
    if (!ReadFile(pipe, buf, want, &got, NULL) || !got)
      return;
    uint64_t allowed = got;
    if (limit && *total + allowed > limit)
      allowed = limit > *total ? limit - *total : 0;
    if (allowed) {
      DWORD wrote = 0;
      (void)WriteFile(dst, buf, (DWORD)allowed, &wrote, NULL);
    }
    *total += got;
    if (limit && *total > limit) {
      *breached = true;
      return;
    }
  }
}

int ny_safe_run_spawn(const ny_safe_run_t *sr, const char *const argv[],
                      const char *workload) {
  if (!sr || !ny_safe_run_any_set(sr))
    return ny_exec_spawn(argv);
  if (!argv || !argv[0])
    return 127;
  if (sr->max_files > 0)
    fprintf(stderr, "safe-run: Windows has no Job Object open-file limit; "
                    "files:%d is not active\n", sr->max_files);

  char *cmd = NULL;
  size_t cmd_len = 0, cmd_cap = 0;
  for (size_t i = 0; argv[i]; ++i) {
    if (!ny_win_cmd_arg(&cmd, &cmd_len, &cmd_cap, argv[i])) {
      free(cmd);
      return 127;
    }
  }
  SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
  HANDLE out_r = NULL, out_w = NULL, err_r = NULL, err_w = NULL;
  if (!CreatePipe(&out_r, &out_w, &sa, 0)) {
    free(cmd);
    return 127;
  }
  if (!CreatePipe(&err_r, &err_w, &sa, 0)) {
    CloseHandle(out_r);
    CloseHandle(out_w);
    free(cmd);
    return 127;
  }
  SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOA si = {0};
  PROCESS_INFORMATION pi = {0};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  si.hStdOutput = out_w;
  si.hStdError = err_w;
  BOOL created = CreateProcessA(NULL, cmd, NULL, NULL, TRUE,
                                CREATE_SUSPENDED, NULL, NULL, &si, &pi);
  free(cmd);
  CloseHandle(out_w);
  CloseHandle(err_w);
  if (!created) {
    CloseHandle(out_r);
    CloseHandle(err_r);
    return 127;
  }

  HANDLE job = CreateJobObjectA(NULL, NULL);
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION info;
  memset(&info, 0, sizeof(info));
  info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (sr->cpu_seconds > 0) {
    info.BasicLimitInformation.PerProcessUserTimeLimit.QuadPart =
        (LONGLONG)sr->cpu_seconds * 10000000LL;
    info.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_TIME;
  }
  if (sr->max_processes > 0) {
    info.BasicLimitInformation.ActiveProcessLimit = (DWORD)sr->max_processes;
    info.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
  }
  if (sr->max_rss_bytes > 0) {
    info.ProcessMemoryLimit = (SIZE_T)sr->max_rss_bytes;
    info.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY;
  }
  if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                       &info, sizeof(info)) ||
      !AssignProcessToJobObject(job, pi.hProcess)) {
    fprintf(stderr, "safe-run: could not configure Windows Job Object "
                    "(error %lu)\n", (unsigned long)GetLastError());
    TerminateProcess(pi.hProcess, 125);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (job) CloseHandle(job);
    CloseHandle(out_r);
    CloseHandle(err_r);
    return 125;
  }
  ResumeThread(pi.hThread);

  uint64_t started = ny_win_now_ms();
  uint64_t output = 0;
  bool output_breach = false, wall_breach = false;
  for (;;) {
    ny_win_forward_pipe(out_r, GetStdHandle(STD_OUTPUT_HANDLE),
                        sr->max_output_bytes, &output, &output_breach);
    ny_win_forward_pipe(err_r, GetStdHandle(STD_ERROR_HANDLE),
                        sr->max_output_bytes, &output, &output_breach);
    if (output_breach) {
      TerminateJobObject(job, 125);
      break;
    }
    if (sr->wall_seconds > 0 &&
        ny_win_now_ms() - started >= (uint64_t)sr->wall_seconds * 1000u) {
      wall_breach = true;
      TerminateJobObject(job, 124);
      break;
    }
    if (WaitForSingleObject(pi.hProcess, 10) == WAIT_OBJECT_0)
      break;
  }
  WaitForSingleObject(pi.hProcess, INFINITE);
  ny_win_forward_pipe(out_r, GetStdHandle(STD_OUTPUT_HANDLE),
                      sr->max_output_bytes, &output, &output_breach);
  ny_win_forward_pipe(err_r, GetStdHandle(STD_ERROR_HANDLE),
                      sr->max_output_bytes, &output, &output_breach);
  DWORD code = 125;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(out_r);
  CloseHandle(err_r);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  CloseHandle(job);
  if (wall_breach) {
    fprintf(stderr, "safe-run: %s exceeded wall-time limit %d seconds; "
                    "raise wall:<seconds> or disable --safe-run\n",
            workload ? workload : "workload", sr->wall_seconds);
    return 124;
  }
  if (output_breach) {
    fprintf(stderr, "safe-run: %s exceeded output limit %llu bytes "
                    "(observed at least %llu); raise output:<bytes> or disable "
                    "--safe-run\n", workload ? workload : "workload",
            (unsigned long long)sr->max_output_bytes,
            (unsigned long long)output);
    return 125;
  }
  return (int)code;
}

int ny_safe_run_call(const ny_safe_run_t *sr, ny_safe_run_child_fn fn,
                     void *ctx, const char *workload) {
  (void)sr;
  (void)fn;
  (void)ctx;
  fprintf(stderr, "safe-run: %s uses an in-process JIT callback; Windows Job "
                  "Object containment requires a helper process\n",
          workload ? workload : "JIT workload");
  return 125;
}

#else /* POSIX */

int ny_safe_run_apply_limits(const ny_safe_run_t *sr) {
  if (!sr || !ny_safe_run_any_set(sr))
    return 0;

  int failures = 0;

  /* CPU time limit (seconds). RLIMIT_CPU counts in seconds. */
  if (sr->cpu_seconds > 0) {
    struct rlimit rl;
    if (getrlimit(RLIMIT_CPU, &rl) == 0) {
      rlim_t want = (rlim_t)sr->cpu_seconds;
      if (want > rl.rlim_max)
        want = rl.rlim_max;
      rl.rlim_cur = want;
      if (setrlimit(RLIMIT_CPU, &rl) != 0) {
        fprintf(stderr, "safe-run: warning: could not set CPU time limit "
                        "to %d seconds: %s\n",
                sr->cpu_seconds, strerror(errno));
        failures++;
      }
    } else {
      fprintf(stderr, "safe-run: warning: could not get CPU time limit: %s\n",
              strerror(errno));
      failures++;
    }
  }

  /* Address space limit (bytes). RLIMIT_AS caps virtual memory / RSS. */
  if (sr->max_rss_bytes > 0) {
    struct rlimit rl;
    if (getrlimit(RLIMIT_AS, &rl) == 0) {
      rlim_t want = (rlim_t)sr->max_rss_bytes;
      if (want > rl.rlim_max)
        want = rl.rlim_max;
      rl.rlim_cur = want;
      if (setrlimit(RLIMIT_AS, &rl) != 0) {
        fprintf(stderr, "safe-run: warning: could not set address-space limit "
                        "to %llu bytes: %s\n",
                (unsigned long long)sr->max_rss_bytes, strerror(errno));
        failures++;
      }
    } else {
      fprintf(stderr, "safe-run: warning: could not get address-space limit: %s\n",
              strerror(errno));
      failures++;
    }
  }

  /* Open file descriptor limit. */
  if (sr->max_files > 0) {
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
      rlim_t want = (rlim_t)sr->max_files;
      if (want > rl.rlim_max)
        want = rl.rlim_max;
      rl.rlim_cur = want;
      if (setrlimit(RLIMIT_NOFILE, &rl) != 0) {
        fprintf(stderr, "safe-run: warning: could not set file descriptor limit "
                        "to %d: %s\n",
                sr->max_files, strerror(errno));
        failures++;
      }
    } else {
      fprintf(stderr, "safe-run: warning: could not get file descriptor limit: %s\n",
              strerror(errno));
      failures++;
    }
  }

  /* Child process limit (RLIMIT_NPROC). Available on Linux; may not exist
   * on all BSDs. */
#ifdef RLIMIT_NPROC
  if (sr->max_processes > 0) {
    struct rlimit rl;
    if (getrlimit(RLIMIT_NPROC, &rl) == 0) {
      rlim_t want = (rlim_t)sr->max_processes;
      if (want > rl.rlim_max)
        want = rl.rlim_max;
      rl.rlim_cur = want;
      if (setrlimit(RLIMIT_NPROC, &rl) != 0) {
        fprintf(stderr, "safe-run: warning: could not set process limit "
                        "to %d: %s\n",
                sr->max_processes, strerror(errno));
        failures++;
      }
    } else {
      /* ENOSYS / EINVAL on systems without NPROC — not an error. */
      if (errno != EINVAL && errno != ENOSYS) {
        fprintf(stderr, "safe-run: warning: could not get process limit: %s\n",
                strerror(errno));
        failures++;
      }
    }
  }
#endif /* RLIMIT_NPROC */

  /* Process group containment: put ourselves in a new process group so
   * descendants can be killed as a tree on timeout/limit breach. */
  if (sr->contain_process_group) {
    if (setpgid(0, 0) != 0 && errno != EPERM) {
      fprintf(stderr, "safe-run: warning: setpgid failed: %s\n",
              strerror(errno));
      /* Non-fatal: containment is best-effort. */
    }
  }

  return failures == 0 ? 0 : -1;
}

typedef struct {
  pid_t pid;
  pid_t ppid;
  bool descendant;
} ny_safe_proc_t;

static size_t ny_safe_run_proc_tree(pid_t root, ny_safe_proc_t *items,
                                    size_t cap) {
#ifdef __linux__
  DIR *proc = opendir("/proc");
  if (!proc)
    return 0;
  size_t len = 0;
  struct dirent *de = NULL;
  while (len < cap && (de = readdir(proc)) != NULL) {
    if (!isdigit((unsigned char)de->d_name[0]))
      continue;
    char path[128], line[2048];
    snprintf(path, sizeof(path), "/proc/%s/stat", de->d_name);
    FILE *f = fopen(path, "r");
    if (!f)
      continue;
    if (fgets(line, sizeof(line), f)) {
      char *tail = strrchr(line, ')');
      long ppid = 0;
      if (tail && tail[1] == ' ' &&
          sscanf(tail + 2, "%*c %ld", &ppid) == 1) {
        items[len].pid = (pid_t)strtol(de->d_name, NULL, 10);
        items[len].ppid = (pid_t)ppid;
        items[len].descendant = items[len].pid == root;
        ++len;
      }
    }
    fclose(f);
  }
  closedir(proc);
  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t i = 0; i < len; ++i) {
      if (items[i].descendant)
        continue;
      for (size_t j = 0; j < len; ++j) {
        if (items[j].descendant && items[i].ppid == items[j].pid) {
          items[i].descendant = true;
          changed = true;
          break;
        }
      }
    }
  }
  return len;
#else
  (void)root;
  (void)items;
  (void)cap;
  return 0;
#endif
}

static unsigned ny_safe_run_descendant_count(pid_t root) {
  ny_safe_proc_t items[4096];
  size_t len = ny_safe_run_proc_tree(root, items, 4096);
  unsigned count = 0;
  for (size_t i = 0; i < len; ++i)
    if (items[i].descendant && items[i].pid != root)
      ++count;
  return count;
}

static void ny_safe_run_kill_tree(pid_t pid) {
  (void)kill(pid, SIGSTOP);
#ifdef __linux__
  for (int pass = 0; pass < 3; ++pass) {
    ny_safe_proc_t items[4096];
    size_t len = ny_safe_run_proc_tree(pid, items, 4096);
    for (size_t i = 0; i < len; ++i)
      if (items[i].descendant && items[i].pid != pid)
        (void)kill(items[i].pid, SIGKILL);
  }
#endif
  if (kill(-pid, SIGKILL) != 0)
    (void)kill(pid, SIGKILL);
}

static uint64_t ny_safe_run_now_ms(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    return 0;
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

typedef struct {
  uint64_t rss_bytes;
  uint64_t vm_bytes;
  uint64_t cpu_ticks;
  unsigned descendants;
} ny_safe_run_sample_t;

static bool ny_safe_run_sample_process(pid_t pid, ny_safe_run_sample_t *out) {
  if (!out)
    return false;
  memset(out, 0, sizeof(*out));
#ifdef __linux__
  char path[128];
  snprintf(path, sizeof(path), "/proc/%ld/status", (long)pid);
  FILE *f = fopen(path, "r");
  if (!f)
    return false;
  char line[256];
  while (fgets(line, sizeof(line), f)) {
    unsigned long long kb = 0;
    if (sscanf(line, "VmRSS: %llu kB", &kb) == 1)
      out->rss_bytes = (uint64_t)kb * 1024u;
    else if (sscanf(line, "VmSize: %llu kB", &kb) == 1)
      out->vm_bytes = (uint64_t)kb * 1024u;
  }
  fclose(f);

  snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
  f = fopen(path, "r");
  if (f) {
    char stat_line[2048];
    if (fgets(stat_line, sizeof(stat_line), f)) {
      char *tail = strrchr(stat_line, ')');
      if (tail && tail[1] == ' ') {
        char *save = NULL;
        char *tok = strtok_r(tail + 2, " ", &save);
        int field = 3;
        while (tok) {
          if (field == 14)
            out->cpu_ticks += strtoull(tok, NULL, 10);
          else if (field == 15) {
            out->cpu_ticks += strtoull(tok, NULL, 10);
            break;
          }
          tok = strtok_r(NULL, " ", &save);
          ++field;
        }
      }
    }
    fclose(f);
  }

  out->descendants = ny_safe_run_descendant_count(pid);
  return true;
#else
  (void)pid;
  return false;
#endif
}

static bool ny_safe_run_pipe(int fds[2]) {
  if (pipe(fds) != 0)
    return false;
  int flags = fcntl(fds[0], F_GETFL, 0);
  if (flags >= 0)
    (void)fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);
  return true;
}

static int ny_safe_run_supervise(pid_t pid, int out_fd, int err_fd,
                                 const ny_safe_run_t *sr,
                                 const char *workload) {
  int status = 0;
  struct rusage usage;
  memset(&usage, 0, sizeof(usage));
  bool exited = false, out_open = true, err_open = true, breached = false;
  bool timed_out = false;
  uint64_t termination_ms = 0;
  uint64_t seen = 0;
  uint64_t started_ms = ny_safe_run_now_ms();
  uint64_t deadline_ms =
      sr && sr->wall_seconds > 0
          ? started_ms + (uint64_t)sr->wall_seconds * 1000u
          : 0;
  int sample_ms = sr && sr->telemetry_interval_ms >= 50
                      ? sr->telemetry_interval_ms
                      : 250;
  int warn_window = sr && sr->telemetry_window >= 2 ? sr->telemetry_window : 3;
  uint64_t next_sample_ms = started_ms + (uint64_t)sample_ms;
  uint64_t last_sample_ms = started_ms;
  uint64_t last_output = 0;
  ny_safe_run_sample_t sample = {0}, previous = {0}, peak = {0};
  int rss_streak = 0, cpu_streak = 0, vm_streak = 0, proc_streak = 0;
  int output_streak = 0;
  bool rss_warned = false, cpu_warned = false, vm_warned = false;
  bool proc_warned = false, output_warned = false;
  while (!exited || out_open || err_open) {
    if (!exited) {
      pid_t got = wait4(pid, &status, WNOHANG, &usage);
      if (got == pid)
        exited = true;
      else if (got < 0 && errno != EINTR) {
        ny_safe_run_kill_tree(pid);
        return 127;
      }
    }
    if (!exited && !timed_out && deadline_ms > 0 &&
        ny_safe_run_now_ms() >= deadline_ms) {
      timed_out = true;
      termination_ms = ny_safe_run_now_ms();
      ny_safe_run_kill_tree(pid);
    }
    uint64_t now_ms = ny_safe_run_now_ms();
    if (!exited && sr && sr->telemetry && now_ms >= next_sample_ms &&
        ny_safe_run_sample_process(pid, &sample)) {
      if (sample.rss_bytes > peak.rss_bytes)
        peak.rss_bytes = sample.rss_bytes;
      if (sample.vm_bytes > peak.vm_bytes)
        peak.vm_bytes = sample.vm_bytes;
      if (sample.descendants > peak.descendants)
        peak.descendants = sample.descendants;
      uint64_t elapsed = now_ms > last_sample_ms ? now_ms - last_sample_ms : 1;
      uint64_t output_delta = seen - last_output;
      uint64_t vm_delta = sample.vm_bytes > previous.vm_bytes
                              ? sample.vm_bytes - previous.vm_bytes
                              : 0;
      uint64_t cpu_delta = sample.cpu_ticks > previous.cpu_ticks
                               ? sample.cpu_ticks - previous.cpu_ticks
                               : 0;
      long ticks = sysconf(_SC_CLK_TCK);
      double cpu_cores = ticks > 0
                             ? ((double)cpu_delta * 1000.0) /
                                   ((double)ticks * (double)elapsed)
                             : 0.0;
      rss_streak = sr->max_rss_bytes > 0 &&
                           sample.rss_bytes * 100u >= sr->max_rss_bytes * 85u
                       ? rss_streak + 1
                       : 0;
      cpu_streak = sr->cpu_seconds > 0 && cpu_cores >= 0.90
                       ? cpu_streak + 1
                       : 0;
      vm_streak = sr->max_rss_bytes > 0 &&
                          vm_delta * 1000u * 4u >= sr->max_rss_bytes * elapsed
                      ? vm_streak + 1
                      : 0;
      proc_streak = sr->max_processes > 0 &&
                            sample.descendants * 100u >=
                                (unsigned)sr->max_processes * 85u
                        ? proc_streak + 1
                        : 0;
      output_streak = sr->max_output_bytes > 0 &&
                              output_delta * 1000u * 2u >=
                                  sr->max_output_bytes * elapsed
                          ? output_streak + 1
                          : 0;
#define NY_SAFE_WARN(streak, warned, ...)                                     \
  do {                                                                         \
    if (!(warned) && (streak) >= warn_window) {                                \
      fprintf(stderr, __VA_ARGS__);                                            \
      (warned) = true;                                                         \
    }                                                                          \
  } while (0)
      NY_SAFE_WARN(rss_streak, rss_warned,
                   "safe-run: warning: sustained RSS is near its limit "
                   "(%llu/%llu bytes)\n",
                   (unsigned long long)sample.rss_bytes,
                   (unsigned long long)sr->max_rss_bytes);
      NY_SAFE_WARN(cpu_streak, cpu_warned,
                   "safe-run: warning: sustained CPU utilization is %.2f "
                   "cores\n",
                   cpu_cores);
      NY_SAFE_WARN(vm_streak, vm_warned,
                   "safe-run: warning: sustained virtual-memory growth is "
                   "%llu bytes over %llu ms (allocation-rate proxy)\n",
                   (unsigned long long)vm_delta,
                   (unsigned long long)elapsed);
      NY_SAFE_WARN(proc_streak, proc_warned,
                   "safe-run: warning: sustained process-group descendant count is "
                   "%u (limit %d)\n",
                   sample.descendants, sr->max_processes);
      NY_SAFE_WARN(output_streak, output_warned,
                   "safe-run: warning: sustained output rate is %.0f bytes/s\n",
                   (double)output_delta * 1000.0 / (double)elapsed);
#undef NY_SAFE_WARN
      previous = sample;
      last_output = seen;
      last_sample_ms = now_ms;
      next_sample_ms = now_ms + (uint64_t)sample_ms;
    }
    struct pollfd fds[2] = {{out_fd, POLLIN | POLLHUP | POLLERR, 0},
                            {err_fd, POLLIN | POLLHUP | POLLERR, 0}};
    (void)poll(fds, 2, exited ? 0 : 20);
    for (int i = 0; i < 2; ++i) {
      bool *is_open = i == 0 ? &out_open : &err_open;
      if (!*is_open)
        continue;
      char buf[8192];
      for (;;) {
        ssize_t got = read(fds[i].fd, buf, sizeof(buf));
        if (got > 0) {
          size_t emit = (size_t)got;
          if (sr && sr->max_output_bytes && seen + emit > sr->max_output_bytes) {
            emit = seen < sr->max_output_bytes
                       ? (size_t)(sr->max_output_bytes - seen)
                       : 0;
            breached = true;
          }
          if (emit)
            (void)write(i == 0 ? STDOUT_FILENO : STDERR_FILENO, buf, emit);
          seen += (uint64_t)got;
          if (breached) {
            if (!termination_ms)
              termination_ms = ny_safe_run_now_ms();
            ny_safe_run_kill_tree(pid);
          }
          continue;
        }
        if (got == 0) {
          close(fds[i].fd);
          *is_open = false;
        }
        break;
      }
    }
    if (termination_ms && ny_safe_run_now_ms() - termination_ms >= 250u) {
      ny_safe_run_kill_tree(pid);
      if (out_open) {
        close(out_fd);
        out_open = false;
      }
      if (err_open) {
        close(err_fd);
        err_open = false;
      }
    }
  }
  if (timed_out) {
    uint64_t elapsed_ms = ny_safe_run_now_ms() - started_ms;
    fprintf(stderr,
            "safe-run: %s exceeded wall-time limit %d seconds "
            "(observed %.3f seconds, peak RSS %llu bytes, peak VM %llu "
            "bytes, descendants %u, output %llu bytes); raise "
            "wall:<seconds> or disable "
            "--safe-run\n",
            workload, sr->wall_seconds, (double)elapsed_ms / 1000.0,
            (unsigned long long)peak.rss_bytes,
            (unsigned long long)peak.vm_bytes, peak.descendants,
            (unsigned long long)seen);
    return 124;
  }
  if (breached) {
    fprintf(stderr,
            "safe-run: %s exceeded output limit %llu bytes (observed at "
            "least %llu); raise output:<bytes> or disable --safe-run\n",
            workload, (unsigned long long)sr->max_output_bytes,
            (unsigned long long)seen);
    return 125;
  }
  if (WIFEXITED(status))
    return WEXITSTATUS(status);
  if (WIFSIGNALED(status)) {
    int sig = WTERMSIG(status);
#ifdef __APPLE__
    long long peak_bytes = (long long)usage.ru_maxrss;
#else
    long long peak_bytes = (long long)usage.ru_maxrss * 1024LL;
#endif
    if ((uint64_t)peak_bytes < peak.rss_bytes)
      peak_bytes = (long long)peak.rss_bytes;
    const char *kind = "signal termination";
    const char *hint = "inspect the signal or disable --safe-run";
    if (sig == SIGXCPU) {
      kind = "CPU-time limit";
      hint = "raise cpu:<seconds> or disable --safe-run";
    } else if (sr && sr->max_rss_bytes > 0 && peak.vm_bytes > 0 &&
               peak.vm_bytes * 100u >= sr->max_rss_bytes * 90u &&
               (sig == SIGSEGV || sig == SIGABRT || sig == SIGKILL)) {
      kind = sig == SIGKILL ? "probable OOM kill" :
                              "address-space limit/allocation failure";
      hint = "raise rss:<bytes> or disable --safe-run";
    } else if (sig == SIGKILL) {
      kind = "SIGKILL (OOM status unavailable)";
      hint = "inspect the host/cgroup OOM log or raise the relevant limit";
    }
    fprintf(stderr,
            "safe-run: %s terminated by %s, signal %d (peak RSS %lld bytes, "
            "peak VM %llu bytes, descendants %u, output %llu bytes); %s\n",
            workload, kind, sig, peak_bytes,
            (unsigned long long)peak.vm_bytes, peak.descendants,
            (unsigned long long)seen, hint);
    if (sr && sr->contain_process_group)
      (void)kill(-pid, SIGKILL);
    return 128 + sig;
  }
  return 127;
}

static pid_t ny_safe_run_fork(int out_pipe[2], int err_pipe[2]) {
  if (!ny_safe_run_pipe(out_pipe))
    return -1;
  if (!ny_safe_run_pipe(err_pipe)) {
    close(out_pipe[0]);
    close(out_pipe[1]);
    return -1;
  }
  pid_t pid = fork();
  if (pid != 0) {
    close(out_pipe[1]);
    close(err_pipe[1]);
    if (pid < 0) {
      close(out_pipe[0]);
      close(err_pipe[0]);
    }
    return pid;
  }
  close(out_pipe[0]);
  close(err_pipe[0]);
  (void)dup2(out_pipe[1], STDOUT_FILENO);
  (void)dup2(err_pipe[1], STDERR_FILENO);
  close(out_pipe[1]);
  close(err_pipe[1]);
  return 0;
}

int ny_safe_run_spawn(const ny_safe_run_t *sr, const char *const argv[],
                      const char *workload) {
  if (!argv || !argv[0])
    return 127;
  int out_pipe[2], err_pipe[2];
  pid_t pid = ny_safe_run_fork(out_pipe, err_pipe);
  if (pid < 0)
    return 127;
  if (pid == 0) {
    if (ny_safe_run_apply_limits(sr) != 0)
      _exit(125);
    execvp(argv[0], (char *const *)argv);
    fprintf(stderr, "safe-run: could not execute %s: %s\n", argv[0],
            strerror(errno));
    _exit(127);
  }
  if (sr && sr->contain_process_group)
    (void)setpgid(pid, pid);
  return ny_safe_run_supervise(pid, out_pipe[0], err_pipe[0], sr,
                               workload ? workload : argv[0]);
}

int ny_safe_run_call(const ny_safe_run_t *sr, ny_safe_run_child_fn fn,
                     void *ctx, const char *workload) {
  if (!fn)
    return 127;
  int out_pipe[2], err_pipe[2];
  pid_t pid = ny_safe_run_fork(out_pipe, err_pipe);
  if (pid < 0)
    return 127;
  if (pid == 0) {
    if (ny_safe_run_apply_limits(sr) != 0)
      _exit(125);
    _exit(fn(ctx));
  }
  if (sr && sr->contain_process_group)
    (void)setpgid(pid, pid);
  return ny_safe_run_supervise(pid, out_pipe[0], err_pipe[0], sr,
                               workload ? workload : "JIT workload");
}

#endif /* _WIN32 / POSIX */
