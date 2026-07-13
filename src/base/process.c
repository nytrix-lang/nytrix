#include "base/process.h"

#include <stdint.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
  memcpy(*buf + *len, data, size);
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
  DWORD got = 0;
  while (ReadFile(read_pipe, chunk, sizeof(chunk), &got, NULL) && got) {
    if (!ny_capture_append(out, &len, &cap, chunk, (size_t)got))
      break;
  }
  CloseHandle(read_pipe);
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 127;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  if (!*out)
    *out = _strdup("");
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
  for (;;) {
    ssize_t got = read(pipefd[0], chunk, sizeof(chunk));
    if (got > 0) {
      if (!ny_capture_append(out, &len, &cap, chunk, (size_t)got))
        break;
      continue;
    }
    if (got < 0 && errno == EINTR)
      continue;
    break;
  }
  close(pipefd[0]);
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
  if (!*out)
    *out = strdup("");
  if (WIFEXITED(status))
    return WEXITSTATUS(status);
  if (WIFSIGNALED(status))
    return 128 + WTERMSIG(status);
  return 127;
}
#endif
