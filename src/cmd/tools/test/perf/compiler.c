/*
 * Compiler process capture and source snapshot utilities for performance tests.
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "compiler.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

bool nyt_compiler_snapshot(const char *source, char *path, size_t capacity,
                           char *error, size_t error_capacity) {
  if (error && error_capacity)
    error[0] = 0;
  if (!source || !*source || !path || !capacity)
    return false;
  path[0] = 0;
#ifdef _WIN32
  static unsigned serial;
  int n = snprintf(path, capacity, "%s.bench-%lu-%u.exe", source,
                   (unsigned long)GetCurrentProcessId(), serial++);
  if (n > 0 && (size_t)n < capacity && CopyFileA(source, path, TRUE))
    return true;
  if (error && error_capacity)
    snprintf(error, error_capacity,
             "cannot snapshot compiler (Windows error %lu)",
             (unsigned long)GetLastError());
#else
  FILE *input = fopen(source, "rb");
  struct stat before = {0}, after = {0};
  if (!input)
    goto failed;
  if (fstat(fileno(input), &before) != 0 || !S_ISREG(before.st_mode) ||
      !(before.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH))) {
    fclose(input);
    errno = EACCES;
    goto failed;
  }
  int n = snprintf(path, capacity, "%s.bench-XXXXXX", source);
  if (n <= 0 || (size_t)n >= capacity) {
    fclose(input);
    errno = ENAMETOOLONG;
    goto failed;
  }
  int fd = mkstemp(path);
  if (fd < 0) {
    fclose(input);
    goto failed;
  }
  FILE *output = fdopen(fd, "wb");
  if (!output) {
    close(fd);
    fclose(input);
    remove(path);
    goto failed;
  }
  bool ok = true;
  size_t total = 0, count;
  unsigned char buffer[65536];
  while ((count = fread(buffer, 1, sizeof(buffer), input)) != 0) {
    if (fwrite(buffer, 1, count, output) != count) {
      ok = false;
      break;
    }
    total += count;
  }
  ok = ok && !ferror(input) && fstat(fileno(input), &after) == 0 &&
       before.st_size == after.st_size && before.st_mtime == after.st_mtime &&
       before.st_ctime == after.st_ctime && (off_t)total == before.st_size;
  if (fclose(input) != 0)
    ok = false;
  if (fflush(output) != 0 || fchmod(fd, 0700) != 0)
    ok = false;
  if (fclose(output) != 0)
    ok = false;
  if (ok)
    return true;
  remove(path);
  errno = EIO;
failed:
  if (error && error_capacity)
    snprintf(error, error_capacity, "cannot snapshot compiler: %s",
             strerror(errno));
#endif
  path[0] = 0;
  return false;
}

int nyt_compiler_snapshot_selftest(void) {
#ifdef _WIN32
  /*
   * Windows snapshots are exercised by the native benchmark route.
   */
  return 0;
#else
  char directory[] = "/tmp/ny-compiler-snapshot-XXXXXX";
  if (!mkdtemp(directory))
    return 1;
  char source[512], replacement[512], snapshot[512], second[512], error[256];
  snprintf(source, sizeof(source), "%s/compiler", directory);
  snprintf(replacement, sizeof(replacement), "%s/replacement", directory);
  const char original[] = "#!/bin/sh\nexit 0\n";
  FILE *out = fopen(source, "wb");
  bool ok = out != NULL;
  if (out) {
    ok = fwrite(original, 1, sizeof(original) - 1, out) == sizeof(original) - 1;
    ok = fclose(out) == 0 && ok && chmod(source, 0700) == 0;
  }
  snapshot[0] = second[0] = 0;
  ok = ok && nyt_compiler_snapshot(source, snapshot, sizeof(snapshot), error,
                                   sizeof(error));
  out = fopen(replacement, "wb");
  if (out) {
    ok = fputs("#!/bin/sh\nexit 7\n", out) >= 0 && ok;
    ok = fclose(out) == 0 && ok;
    ok = rename(replacement, source) == 0 && ok;
  } else
    ok = false;
  /*
   * Atomic replacement and then deletion must not affect the private copy.
   */
  ok = unlink(source) == 0 && ok;
  char actual[sizeof(original)] = {0};
  FILE *in = snapshot[0] ? fopen(snapshot, "rb") : NULL;
  if (in) {
    ok = fread(actual, 1, sizeof(actual), in) == sizeof(original) - 1 && ok;
    ok = fclose(in) == 0 && ok;
  } else
    ok = false;
  ok = ok && memcmp(actual, original, sizeof(original) - 1) == 0 &&
       access(snapshot, X_OK) == 0 &&
       !nyt_compiler_snapshot(source, second, sizeof(second), error,
                              sizeof(error)) &&
       !second[0] && error[0];
  if (ok) {
    pid_t child = fork();
    if (child == 0) {
      execl(snapshot, snapshot, (char *)NULL);
      _exit(127);
    }
    int status = 0;
    ok = child > 0 && waitpid(child, &status, 0) == child &&
         WIFEXITED(status) && WEXITSTATUS(status) == 0;
  }
  if (snapshot[0])
    remove(snapshot);
  remove(source);
  remove(replacement);
  rmdir(directory);
  fprintf(stderr, "compiler snapshot selftest: %s\n", ok ? "passed" : "failed");
  return ok ? 0 : 1;
#endif
}
