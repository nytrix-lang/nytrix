/*
 * Subprocess runner: spawn, capture stdout/stderr, and wait with timeout
 * for compiler self-tests, fuzzer workers, and native executable probes.
 */
#include "base/process.h"
#include "base/options.h"
#include "base/util.h"
#include "base/compat.h"

#include <stdint.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static bool ny_capture_append(char **buf, size_t *len, size_t *cap,
                              const char *data, size_t size) {
  if (!buf || !len || !cap || (!data && size))
    return false;
  if (size > SIZE_MAX - *len - 1)
    return false;
  size_t need = *len + size + 1;
  if (need > *cap) {
    size_t next = *cap ? *cap : 4096;
    while (next < need) {
      if (next > SIZE_MAX / 2)
        return false;
      next *= 2;
    }
    char *grown = (char *)realloc(*buf, next);
    if (!grown)
      return false;
    *buf = grown;
    *cap = next;
  }
  memmove(*buf + *len, data, size);
  *len += size;
  (*buf)[*len] = '\0';
  return true;
}

#ifdef _WIN32
#include <windows.h>

static bool ny_win_quote_arg(char **cmd, size_t *len, size_t *cap,
                             const char *arg) {
  if (*len && !ny_capture_append(cmd, len, cap, " ", 1))
    return false;
  if (!ny_capture_append(cmd, len, cap, "\"", 1))
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
        if (!ny_capture_append(cmd, len, cap, "\\", 1))
          return false;
      slashes = 0;
      if (*p == '\0')
        break;
      if (!ny_capture_append(cmd, len, cap, "\"", 1))
        return false;
      continue;
    }
    while (slashes--) {
      if (!ny_capture_append(cmd, len, cap, "\\", 1))
        return false;
    }
    slashes = 0;
    if (!ny_capture_append(cmd, len, cap, p, 1))
      return false;
  }
  return ny_capture_append(cmd, len, cap, "\"", 1);
}

int ny_process_capture(const char *const argv[], char **out,
                       bool discard_stderr) {
  if (out)
    *out = NULL;
  if (!argv || !argv[0] || !*argv[0] || !out)
    return 127;
  char *cmd = NULL;
  size_t cmd_len = 0, cmd_cap = 0;
  for (size_t i = 0; argv[i]; ++i) {
    if (!ny_win_quote_arg(&cmd, &cmd_len, &cmd_cap, argv[i]) ||
        cmd_len >= 32767) {
      free(cmd);
      return 127;
    }
  }
  HANDLE read_pipe = NULL, write_pipe = NULL;
  SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
  if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) {
    free(cmd);
    return 127;
  }
  SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);
  HANDLE null_err = INVALID_HANDLE_VALUE;
  if (discard_stderr)
    null_err = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  STARTUPINFOA si = {0};
  PROCESS_INFORMATION pi = {0};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  si.hStdOutput = write_pipe;
  si.hStdError = discard_stderr && null_err != INVALID_HANDLE_VALUE
                     ? null_err
                     : GetStdHandle(STD_ERROR_HANDLE);
  BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
  CloseHandle(write_pipe);
  if (null_err != INVALID_HANDLE_VALUE)
    CloseHandle(null_err);
  free(cmd);
  if (!ok) {
    CloseHandle(read_pipe);
    return 127;
  }
  char chunk[4096];
  size_t len = 0, cap = 0;
  bool capture_ok = true;
  DWORD got = 0;
  while (ReadFile(read_pipe, chunk, sizeof(chunk), &got, NULL) && got) {
    if (!ny_capture_append(out, &len, &cap, chunk, (size_t)got)) {
      capture_ok = false;
      break;
    }
  }
  CloseHandle(read_pipe);
  DWORD waited = WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 127;
  BOOL got_code = GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  if (!capture_ok) {
    free(*out);
    *out = NULL;
  }
  if (!*out)
    *out = _strdup("");
  if (!capture_ok || waited != WAIT_OBJECT_0 || !got_code)
    return 127;
  return (int)code;
}

#else
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;

int ny_process_capture(const char *const argv[], char **out,
                       bool discard_stderr) {
  if (out)
    *out = NULL;
  if (!argv || !argv[0] || !*argv[0] || !out)
    return 127;
  int pipefd[2];
  if (pipe(pipefd) != 0)
    return 127;
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
  posix_spawn_file_actions_addclose(&actions, pipefd[0]);
  posix_spawn_file_actions_addclose(&actions, pipefd[1]);
  if (discard_stderr)
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null",
                                     O_WRONLY, 0);
  pid_t pid = -1;
  int spawn_rc = posix_spawnp(&pid, argv[0], &actions, NULL,
                              (char *const *)argv, environ);
  posix_spawn_file_actions_destroy(&actions);
  close(pipefd[1]);
  if (spawn_rc != 0) {
    close(pipefd[0]);
    return 127;
  }
  char chunk[4096];
  size_t len = 0, cap = 0;
  bool capture_ok = true;
  for (;;) {
    ssize_t got = read(pipefd[0], chunk, sizeof(chunk));
    if (got > 0) {
      if (!ny_capture_append(out, &len, &cap, chunk, (size_t)got)) {
        capture_ok = false;
        break;
      }
      continue;
    }
    if (got < 0 && errno == EINTR)
      continue;
    break;
  }
  close(pipefd[0]);
  int status = 0;
  pid_t waited;
  do {
    waited = waitpid(pid, &status, 0);
  } while (waited < 0 && errno == EINTR);
  if (!capture_ok) {
    free(*out);
    *out = NULL;
  }
  if (!*out)
    *out = strdup("");
  if (!capture_ok || waited != pid)
    return 127;
  if (WIFEXITED(status))
    return WEXITSTATUS(status);
  if (WIFSIGNALED(status))
    return 128 + WTERMSIG(status);
  return 127;
}
#endif

#ifdef _WIN32
static int ny_process_path_like(const char *s) {
  if (!s || !*s)
    return 0;
  if (strchr(s, '\\') || strchr(s, '/'))
    return 1;
  if (isalpha((unsigned char)s[0]) && s[1] == ':')
    return 1;
  return 0;
}
#endif

int ny_exec_spawn(const char *const argv[]) {
  if (verbose_enabled >= 2 && argv && argv[0]) {
    fprintf(stderr, "[cmd]");
    for (size_t i = 0; argv[i]; ++i) {
      const char *arg = argv[i];
      bool needs_quote = false;
      for (const char *p = arg; *p; ++p) {
        if (isspace((unsigned char)*p) || *p == '"' || *p == '\'') {
          needs_quote = true;
          break;
        }
      }
      fputc(' ', stderr);
      if (!needs_quote) {
        fputs(arg, stderr);
      } else {
        fputc('"', stderr);
        for (const char *p = arg; *p; ++p) {
          if (*p == '"' || *p == '\\')
            fputc('\\', stderr);
          fputc(*p, stderr);
        }
        fputc('"', stderr);
      }
    }
    fputc('\n', stderr);
  }
#ifdef _WIN32
  const char *a0 = argv[0] ? argv[0] : "";
  int rc = -1;
  if (ny_process_path_like(a0) && ny_access(a0, F_OK) == 0)
    rc = _spawnv(_P_WAIT, a0, (const char *const *)argv);
  else
    rc = _spawnvp(_P_WAIT, a0, (const char *const *)argv);
  if (rc < 0 && errno == ENOENT) {
    const char *base = strrchr(a0, '/');
    if (!base)
      base = strrchr(a0, '\\');
    base = base ? base + 1 : a0;
    char base_buf[PATH_MAX];
    snprintf(base_buf, sizeof(base_buf), "%s", base);
    char *name = base_buf;
    while (*name && isspace((unsigned char)*name))
      name++;
    if (*name == '"' || *name == '\'') {
      char q = *name++;
      char *endq = strchr(name, q);
      if (endq)
        *endq = '\0';
    }
    char *ws = strpbrk(name, " \t");
    if (ws)
      *ws = '\0';
    if (strcasecmp(name, "clang") == 0 || strcasecmp(name, "clang.exe") == 0 ||
        strcasecmp(name, "clang-cl") == 0 ||
        strcasecmp(name, "clang-cl.exe") == 0) {
      static const char *const win_clang_candidates[] = {
          "C:\\msys64\\ucrt64\\bin\\clang.exe",
          "C:\\msys64\\clang64\\bin\\clang.exe",
          "C:\\msys64\\mingw64\\bin\\clang.exe",
          "C:\\tools\\msys64\\ucrt64\\bin\\clang.exe",
          "C:\\tools\\msys64\\clang64\\bin\\clang.exe",
          "C:\\tools\\msys64\\mingw64\\bin\\clang.exe",
          "C:\\PROGRA~1\\LLVM\\bin\\clang.exe",
          "C:\\PROGRA~2\\LLVM\\bin\\clang.exe",
          "C:\\PROGRA~1\\LLVM\\bin\\clang-cl.exe",
          "C:\\PROGRA~2\\LLVM\\bin\\clang-cl.exe",
          "C:\\Program Files\\LLVM\\bin\\clang.exe",
          "C:\\Program Files (x86)\\LLVM\\bin\\clang.exe",
          "C:\\Program Files\\LLVM\\bin\\clang-cl.exe",
          "C:\\Program Files (x86)\\LLVM\\bin\\clang-cl.exe",
      };
      const char *argv2[128];
      size_t k = 0;
      while (argv[k] && k + 1 < (sizeof(argv2) / sizeof(argv2[0]))) {
        argv2[k] = argv[k];
        k++;
      }
      argv2[k] = NULL;
      for (size_t i = 0;
           i < sizeof(win_clang_candidates) / sizeof(win_clang_candidates[0]);
           ++i) {
        if (ny_access(win_clang_candidates[i], F_OK) != 0)
          continue;
        argv2[0] = win_clang_candidates[i];
        rc = _spawnv(_P_WAIT, argv2[0], (const char *const *)argv2);
        if (rc >= 0)
          return rc;
      }
    }
  }
  if (rc < 0) {
    perror("_spawnvp");
    return -1;
  }
  return rc;
#else
  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    return -1;
  }
  if (pid == 0) {
    execvp(argv[0], (char *const *)argv);
    perror("execvp");
    _exit(127);
  }
  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    perror("waitpid");
    return -1;
  }
  if (WIFEXITED(status))
    return WEXITSTATUS(status);
  if (WIFSIGNALED(status)) {
    fprintf(stderr, "process %s terminated by signal %d\n", argv[0],
            WTERMSIG(status));
    return -1;
  }
  return status;
#endif
}
