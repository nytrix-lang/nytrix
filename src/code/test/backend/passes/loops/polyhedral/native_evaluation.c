/* CMake-driven native checksum and SIMD regression for polyhedral fixtures. */
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef NYR_POLY_EVALUATION_DIR
#error "NYR_POLY_EVALUATION_DIR must name the imported evaluation fixtures"
#endif

typedef struct {
  char source[PATH_MAX];
  char driver[PATH_MAX];
} kernel_case_t;

static kernel_case_t cases[128];
static size_t case_count;
static char evaluation_work[PATH_MAX];

static void cleanup_work(void) {
  if (!evaluation_work[0]) return;
  DIR *stream = opendir(evaluation_work);
  if (stream) {
    struct dirent *entry;
    while ((entry = readdir(stream)) != NULL) {
      if (entry->d_name[0] == '.') continue;
      char path[PATH_MAX];
      if (snprintf(path, sizeof(path), "%s/%s", evaluation_work,
                   entry->d_name) < (int)sizeof(path))
        unlink(path);
    }
    closedir(stream);
  }
  rmdir(evaluation_work);
}

static bool suffix_is(const char *text, const char *suffix) {
  size_t a = strlen(text), b = strlen(suffix);
  return a >= b && strcmp(text + a - b, suffix) == 0;
}

static int collect_cases(const char *dir) {
  DIR *stream = opendir(dir);
  if (!stream) {
    perror(dir);
    return -1;
  }
  struct dirent *entry;
  while ((entry = readdir(stream)) != NULL) {
    if (entry->d_name[0] == '.') continue;
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name) >=
        (int)sizeof(path)) {
      fprintf(stderr, "fixture path too long: %s/%s\n", dir, entry->d_name);
      closedir(stream);
      return -1;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
      perror(path);
      closedir(stream);
      return -1;
    }
    if (S_ISDIR(st.st_mode)) {
      if (collect_cases(path) != 0) {
        closedir(stream);
        return -1;
      }
      continue;
    }
    if (!S_ISREG(st.st_mode) || !suffix_is(entry->d_name, ".c") ||
        strcmp(entry->d_name, "driver.c") == 0 ||
        strstr(path, "/native/") != NULL)
      continue;
    if (case_count == sizeof(cases) / sizeof(cases[0])) {
      fputs("too many polyhedral evaluation cases\n", stderr);
      closedir(stream);
      return -1;
    }
    kernel_case_t *test = &cases[case_count++];
    snprintf(test->source, sizeof(test->source), "%s", path);
    char *slash = strrchr(path, '/');
    size_t parent_len = slash ? (size_t)(slash - path) : 0;
    if (!parent_len || parent_len + sizeof("/driver.c") > sizeof(test->driver)) {
      fprintf(stderr, "cannot find evaluation driver for %s\n", path);
      closedir(stream);
      return -1;
    }
    memcpy(test->driver, path, parent_len);
    strcpy(test->driver + parent_len, "/driver.c");
  }
  closedir(stream);
  return 0;
}

static int execute(char *const argv[], const char *fast_math,
                   const char *alias_versioning, const char *horizontal,
                   char *output, size_t output_cap) {
  int pipefd[2];
  if (pipe(pipefd) != 0) return -1;
  pid_t child = fork();
  if (child < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    return -1;
  }
  if (child == 0) {
    close(pipefd[0]);
    if (dup2(pipefd[1], STDOUT_FILENO) < 0) _exit(126);
    close(pipefd[1]);
    const char *const controls[] = {
        "NYC_DISABLE_PASS", "NYTRIX_POLY_DUMP", "NYTRIX_POLY_REPLAY",
        "NYTRIX_POLY_EXPERIMENTAL_3D", "NYTRIX_POLY_EXPERIMENTAL_FUSION",
        "NYTRIX_POLY_EXPERIMENTAL_PROMOTION",
        "NYTRIX_POLY_EXPERIMENTAL_REGISTER_BLOCK",
        "NYTRIX_POLY_EXPERIMENTAL_SIMD", "NYTRIX_POLY_HORIZONTAL",
        "NYTRIX_POLY_EXPERIMENTAL_ALIAS_VERSIONING", "NYTRIX_FAST_MATH"};
    for (size_t i = 0; i < sizeof(controls) / sizeof(controls[0]); ++i)
      unsetenv(controls[i]);
    if (fast_math) setenv("NYTRIX_FAST_MATH", fast_math, 1);
    if (alias_versioning)
      setenv("NYTRIX_POLY_EXPERIMENTAL_ALIAS_VERSIONING", alias_versioning, 1);
    if (horizontal) setenv("NYTRIX_POLY_HORIZONTAL", horizontal, 1);
    execvp(argv[0], argv);
    fprintf(stderr, "cannot execute %s: %s\n", argv[0], strerror(errno));
    _exit(127);
  }
  close(pipefd[1]);
  size_t used = 0;
  if (output && output_cap) {
    while (used + 1 < output_cap) {
      ssize_t got = read(pipefd[0], output + used, output_cap - used - 1);
      if (got <= 0) break;
      used += (size_t)got;
    }
    output[used] = '\0';
  } else {
    char discard[256];
    while (read(pipefd[0], discard, sizeof(discard)) > 0) {}
  }
  close(pipefd[0]);
  int status = 0;
  if (waitpid(child, &status, 0) < 0) return -1;
  return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}

static bool is_fp_case(const char *source) {
  const char *name = strrchr(source, '/');
  name = name ? name + 1 : source;
  return strncmp(name, "fp_", 3) == 0;
}

static int run_kernel_case(const char *nyc, const char *cc, const char *work,
                           size_t index) {
  kernel_case_t *test = &cases[index];
  char obj[PATH_MAX], nyexe[PATH_MAX], hostexe[PATH_MAX];
  snprintf(obj, sizeof(obj), "%s/kernel-%zu.o", work, index);
  snprintf(nyexe, sizeof(nyexe), "%s/kernel-%zu-ny.exe", work, index);
  snprintf(hostexe, sizeof(hostexe), "%s/kernel-%zu-host.exe", work, index);
  bool fp = is_fp_case(test->source);
  char *ny_compile[] = {(char *)nyc, "-O3", "-c", test->source, "-o", obj,
                        NULL};
  if (execute(ny_compile, fp ? "1" : NULL, NULL, NULL, NULL, 0) != 0) {
    fprintf(stderr, "NYC compile failed: %s\n", test->source);
    return 1;
  }
  char *ny_link[] = {(char *)cc, "-O2", test->driver, obj, "-o", nyexe,
                     NULL};
  if (execute(ny_link, NULL, NULL, NULL, NULL, 0) != 0) {
    fprintf(stderr, "NYR object link failed: %s\n", test->source);
    return 1;
  }
  char *host_compile_fast[] = {(char *)cc, "-O2", "-ffast-math", test->source,
                               test->driver, "-o", hostexe, NULL};
  char *host_compile[] = {(char *)cc, "-O2", test->source, test->driver, "-o",
                          hostexe, NULL};
  if (execute(fp ? host_compile_fast : host_compile, NULL, NULL, NULL, NULL,
              0) != 0) {
    fprintf(stderr, "reference compile failed: %s\n", test->source);
    return 1;
  }
  char *ny_run[] = {nyexe, "1", "0", NULL};
  char *host_run[] = {hostexe, "1", "0", NULL};
  char ny_output[256], host_output[256];
  if (execute(ny_run, NULL, NULL, NULL, ny_output, sizeof(ny_output)) != 0 ||
      execute(host_run, NULL, NULL, NULL, host_output, sizeof(host_output)) !=
          0) {
    fprintf(stderr, "evaluation execution failed: %s\n", test->source);
    return 1;
  }
  char *ny_end, *host_end;
  unsigned long long ny_sum = strtoull(ny_output, &ny_end, 10);
  unsigned long long host_sum = strtoull(host_output, &host_end, 10);
  if (ny_end == ny_output || host_end == host_output || ny_sum != host_sum) {
    fprintf(stderr, "checksum mismatch: %s (NYC=%s host=%s)\n", test->source,
            ny_output, host_output);
    return 1;
  }
  return 0;
}

static int run_horizontal_case(const char *nyc, const char *cc,
                               const char *work) {
  char source[PATH_MAX], driver[PATH_MAX], refexe[PATH_MAX];
  snprintf(source, sizeof(source), "%s/native/dot-127.c",
           NYR_POLY_EVALUATION_DIR);
  snprintf(driver, sizeof(driver), "%s/native/driver.c",
           NYR_POLY_EVALUATION_DIR);
  snprintf(refexe, sizeof(refexe), "%s/horizontal-reference.exe", work);
  char *ref_compile[] = {(char *)cc, "-O2", "-DN=127", source, driver, "-lm",
                         "-o", refexe, NULL};
  if (execute(ref_compile, NULL, NULL, NULL, NULL, 0) != 0) {
    fputs("host horizontal reference compile failed\n", stderr);
    return 1;
  }
  static const struct {
    const char *name;
    const char *fast_math;
    const char *horizontal;
  } modes[] = {{"automatic", "1", NULL}, {"one-vector", "1", "1"},
               {"two-vector", "1", "2"}, {"strict-fp", "0", "2"},
               {"unsupported-width", "1", "4"}};
  char ref_output[256];
  char *ref_run[] = {refexe, NULL};
  if (execute(ref_run, NULL, NULL, NULL, ref_output, sizeof(ref_output)) != 0) {
    fputs("host horizontal reference failed\n", stderr);
    return 1;
  }
  for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
    char obj[PATH_MAX], exe[PATH_MAX];
    snprintf(obj, sizeof(obj), "%s/horizontal-%zu.o", work, i);
    snprintf(exe, sizeof(exe), "%s/horizontal-%zu.exe", work, i);
    char *ny_compile[] = {(char *)nyc, "-O3", "-c", source, "-o", obj, NULL};
    if (execute(ny_compile, modes[i].fast_math, "1", modes[i].horizontal, NULL,
                0) != 0) {
      fprintf(stderr, "NYC horizontal mode failed to compile: %s\n",
              modes[i].name);
      return 1;
    }
    char *ny_link[] = {(char *)cc, "-O2", "-DN=127", driver, obj, "-lm", "-o",
                       exe, NULL};
    if (execute(ny_link, NULL, NULL, NULL, NULL, 0) != 0) {
      fprintf(stderr, "NYR horizontal mode failed to link: %s\n", modes[i].name);
      return 1;
    }
    char output[256];
    char *run[] = {exe, NULL};
    if (execute(run, NULL, NULL, NULL, output, sizeof(output)) != 0 ||
        !strstr(output, "56 full-buffer comparisons")) {
      fprintf(stderr, "NYR horizontal mode failed validation: %s\n%s",
              modes[i].name, output);
      return 1;
    }
    fprintf(stdout, "polyhedral native %s: %s", modes[i].name, output);
  }
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: %s /path/to/nyc /path/to/host-c-compiler\n",
            argv[0]);
    return 2;
  }
  snprintf(evaluation_work, sizeof(evaluation_work),
           "/tmp/nyr-polyhedral-evaluation-XXXXXX");
  char *work = mkdtemp(evaluation_work);
  if (!work) {
    perror("mkdtemp");
    return 1;
  }
  if (atexit(cleanup_work) != 0) {
    fputs("cannot register evaluation cleanup\n", stderr);
    cleanup_work();
    return 1;
  }
  if (collect_cases(NYR_POLY_EVALUATION_DIR) != 0 || case_count == 0) {
    fputs("no polyhedral evaluation sources were found\n", stderr);
    return 1;
  }
  for (size_t i = 0; i < case_count; ++i) {
    if (run_kernel_case(argv[1], argv[2], work, i) != 0) return 1;
    fprintf(stdout, "polyhedral C case %zu/%zu: %s\n", i + 1, case_count,
            cases[i].source);
  }
  if (run_horizontal_case(argv[1], argv[2], work) != 0) return 1;
  printf("NYR polyhedral C evaluation: %zu workload fixtures passed\n",
         case_count);
  return 0;
}
