/*
 * nyr: compile and run NYR text modules (etc/docs/spec/nyr.md).
 *
 *   nyr run FILE.nyr [args...]   build and run; exit status is main's result
 *   nyr build FILE.nyr -o OUT    build an executable
 *   nyr check FILE.nyr           parse, type-check and verify only
 *   nyr ir FILE.nyr              print the (optimized) NYIR as NYR text
 *   nyr asm FILE.nyr             print x86-64 assembly
 *
 * -O0..-O3 select the optimization level (default -O2). Executables are
 * linked by the system C compiler driver ($CC, default `cc`) with libc and
 * libm, so extern functions resolve against the C library.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <nyr/nyr.h>
#include <nyr/text.h>

#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static int usage(void) {
  fprintf(stderr, "usage: nyr run FILE.nyr [args...]\n"
                  "       nyr build FILE.nyr [-o OUT]\n"
                  "       nyr check FILE.nyr\n"
                  "       nyr ir FILE.nyr\n"
                  "       nyr asm FILE.nyr\n"
                  "options: -O0 -O1 -O2 -O3 (default -O2)\n");
  return 2;
}

static char *read_file(const char *path, size_t *len) {
  FILE *f = strcmp(path, "-") ? fopen(path, "rb") : stdin;
  if (!f) {
    fprintf(stderr, "nyr: cannot open %s: %s\n", path, strerror(errno));
    return NULL;
  }
  size_t cap = 1 << 16, n = 0;
  char *buf = malloc(cap);
  while (buf) {
    size_t r = fread(buf + n, 1, cap - n, f);
    n += r;
    if (n < cap)
      break;
    cap *= 2;
    char *nb = realloc(buf, cap);
    if (!nb) {
      free(buf);
      buf = NULL;
    } else
      buf = nb;
  }
  if (f != stdin)
    fclose(f);
  *len = n;
  return buf;
}

static int run_wait(char *const argv[]) {
  pid_t pid;
  int rc = posix_spawnp(&pid, argv[0], NULL, NULL, argv, environ);
  if (rc) {
    fprintf(stderr, "nyr: cannot run %s: %s\n", argv[0], strerror(rc));
    return -1;
  }
  int status = 0;
  if (waitpid(pid, &status, 0) != pid)
    return -1;
  if (WIFEXITED(status))
    return WEXITSTATUS(status);
  if (WIFSIGNALED(status)) {
    fprintf(stderr, "nyr: %s terminated by signal %d\n", argv[0],
            WTERMSIG(status));
    return 128 + WTERMSIG(status);
  }
  return -1;
}

/* Optimize a private copy of `f` at `level`. */
static bool optimized(const nyr_func_t *f, int level, nyr_func_t *out,
                      const char *name) {
  char err[512] = {0};
  nyr_func_init(out);
  /* -O0 still runs the O0 pipeline: it lowers PHIs out of SSA. */
  if (!nyr_func_clone(f, out, err, sizeof(err)) ||
      !nyr_optimize(out, (nyr_opt_level_t)level, err, sizeof(err))) {
    fprintf(stderr, "nyr: optimizing %s failed: %s\n", name, err);
    nyr_func_destroy(out);
    return false;
  }
  return true;
}

static bool build(nyr_text_module_t *m, int level, const char *output) {
  char dir[] = "/tmp/nyr-build-XXXXXX";
  if (!mkdtemp(dir)) {
    fprintf(stderr, "nyr: cannot create a build directory: %s\n",
            strerror(errno));
    return false;
  }
  size_t n = nyr_text_function_count(m);
  size_t nargv = 0;
  char **argv = calloc(n + 9, sizeof(*argv));
  char **paths = calloc(n + 1, sizeof(*paths));
  bool ok = argv && paths;
  const char *cc = getenv("CC");
  argv[nargv++] = (char *)(cc && *cc ? cc : "cc");
  argv[nargv++] = "-no-pie"; /* NYIR objects use absolute relocations */
  argv[nargv++] = "-o";
  argv[nargv++] = (char *)output;
  for (size_t i = 0; ok && i < n; ++i) {
    nyr_func_t f;
    const char *name = nyr_text_function_name(m, i);
    if (!optimized(nyr_text_function(m, i), level, &f, name)) {
      ok = false;
      break;
    }
    char err[512] = {0};
    if (asprintf(&paths[i], "%s/f%zu.o", dir, i) < 0) {
      paths[i] = NULL;
      ok = false;
    } else if (!nyr_emit_object(&f, paths[i], nyr_text_function_symbol(m, i),
                                err, sizeof(err))) {
      fprintf(stderr, "nyr: code generation for %s failed: %s\n", name, err);
      ok = false;
    } else
      argv[nargv++] = paths[i];
    nyr_func_destroy(&f);
  }
  char *data = NULL;
  if (ok && nyr_text_has_data(m)) {
    if (asprintf(&data, "%s/data.s", dir) < 0) {
      data = NULL;
      ok = false;
    } else {
      FILE *df = fopen(data, "w");
      ok = df && nyr_text_write_data(m, df);
      if (df)
        fclose(df);
      argv[nargv++] = data;
    }
  }
  if (ok) {
    argv[nargv++] = "-lm";
    argv[nargv] = NULL;
    ok = run_wait(argv) == 0;
    if (!ok)
      fprintf(stderr, "nyr: linking %s failed\n", output);
  }
  for (size_t i = 0; paths && i < n; ++i)
    if (paths[i]) {
      unlink(paths[i]);
      free(paths[i]);
    }
  if (data) {
    unlink(data);
    free(data);
  }
  rmdir(dir);
  free(paths);
  free(argv);
  return ok;
}

int main(int argc, char **argv) {
  if (argc < 3)
    return usage();
  const char *cmd = argv[1], *path = NULL, *output = NULL;
  int level = 2, first_arg = argc;
  for (int i = 2; i < argc; ++i) {
    if (!path && argv[i][0] == '-' && argv[i][1] == 'O' && argv[i][2] &&
        !argv[i][3] && argv[i][2] >= '0' && argv[i][2] <= '3')
      level = argv[i][2] - '0';
    else if (!path && !strcmp(argv[i], "-o") && i + 1 < argc)
      output = argv[++i];
    else if (!path) {
      path = argv[i];
      if (!strcmp(cmd, "run")) {
        first_arg = i + 1;
        break;
      }
    } else if (!strcmp(argv[i], "-o") && i + 1 < argc)
      output = argv[++i];
    else if (argv[i][0] == '-' && argv[i][1] == 'O' && argv[i][2] &&
             !argv[i][3])
      level = argv[i][2] - '0';
    else
      return usage();
  }
  if (!path)
    return usage();
  size_t len = 0;
  char *src = read_file(path, &len);
  if (!src)
    return 1;
  char err[2048];
  nyr_text_module_t *m = nyr_text_parse(src, len, path, err, sizeof(err));
  free(src);
  if (!m) {
    fprintf(stderr, "%s\n", err);
    return 1;
  }
  int rc = 0;
  if (!strcmp(cmd, "check")) {
    rc = 0;
  } else if (!strcmp(cmd, "ir") || !strcmp(cmd, "asm")) {
    if (!strcmp(cmd, "ir") && nyr_text_has_data(m)) {
      /* data travels as top-level asm lines: the output is a module */
      char *buf = NULL;
      size_t blen = 0;
      FILE *mf = open_memstream(&buf, &blen);
      if (mf && nyr_text_write_data(m, mf)) {
        fclose(mf);
        mf = NULL;
        printf(";; module data\nasm {\n");
        for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
          if (line[0] == '#' || strstr(line, ".note.GNU-stack"))
            continue;
          printf("  \"");
          for (char *c = line; *c; ++c)
            if (*c == '"' || *c == '\\')
              printf("\\%c", *c);
            else if (*c == '\t')
              printf("\\t");
            else
              putchar(*c);
          printf("\"\n");
        }
        printf("}\n\n");
      }
      if (mf)
        fclose(mf);
      free(buf);
    }
    for (size_t i = 0; i < nyr_text_function_count(m) && !rc; ++i) {
      nyr_func_t f;
      const char *name = nyr_text_function_name(m, i);
      if (!optimized(nyr_text_function(m, i), level, &f, name)) {
        rc = 1;
        break;
      }
      char e[512] = {0};
      if (!strcmp(cmd, "ir")) {
        if (i)
          printf("\n");
        nyr_text_print_function(stdout, &f, name);
      } else {
        fflush(stdout);
        if (!nyr_emit_assembly(&f, "-", nyr_text_function_symbol(m, i), e,
                               sizeof(e))) {
          fprintf(stderr, "nyr: assembly for %s failed: %s\n", name, e);
          rc = 1;
        }
      }
      nyr_func_destroy(&f);
    }
  } else if (!strcmp(cmd, "build")) {
    if (nyr_text_function_find(m, "main") < 0) {
      fprintf(stderr, "nyr: %s has no 'fn main'\n", path);
      rc = 1;
    } else
      rc = build(m, level, output ? output : "a.out") ? 0 : 1;
  } else if (!strcmp(cmd, "run")) {
    if (nyr_text_function_find(m, "main") < 0) {
      fprintf(stderr, "nyr: %s has no 'fn main'\n", path);
      rc = 1;
    } else {
      char exe[] = "/tmp/nyr-run-XXXXXX";
      int fd = mkstemp(exe);
      if (fd < 0) {
        fprintf(stderr, "nyr: cannot create a temporary file\n");
        rc = 1;
      } else {
        close(fd);
        if (!build(m, level, exe))
          rc = 1;
        else {
          char **av = calloc((size_t)(argc - first_arg) + 2, sizeof(*av));
          av[0] = exe;
          for (int i = first_arg; i < argc; ++i)
            av[i - first_arg + 1] = argv[i];
          fflush(stdout);
          rc = run_wait(av);
          if (rc < 0)
            rc = 1;
          free(av);
        }
        unlink(exe);
      }
    }
  } else {
    nyr_text_module_free(m);
    return usage();
  }
  nyr_text_module_free(m);
  return rc;
}
