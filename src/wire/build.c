#include "wire/build.h"
#include "base/common.h"
#include "base/util.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

const char *ny_builder_choose_cc(void) {
  const char *cc = getenv("NYTRIX_CC");
  if (!cc)
    cc = getenv("CC");
  if (!cc)
    cc = "clang";
  return cc;
}

int ny_exec_spawn(const char *const argv[]) {
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
}

bool ny_builder_compile_runtime(const char *cc, const char *out_runtime,
                                const char *out_ast, bool debug) {
  const char *root = ny_src_root();
  static char include_arg[PATH_MAX + 12];
  static char runtime_src[PATH_MAX];
  static char ast_src[PATH_MAX];
  snprintf(include_arg, sizeof(include_arg), "-I%s/src", root);
  snprintf(runtime_src, sizeof(runtime_src), "%s/src/rt/init.c", root);
  snprintf(ast_src, sizeof(ast_src), "%s/src/rt/ast.c", root);
  const char *const runtime_args[] = {cc,
                                      "-std=gnu11",
                                      debug ? "-g" : "-O0",
                                      "-fno-pie",
                                      "-fvisibility=hidden",
                                      "-ffunction-sections",
                                      "-fdata-sections",
                                      "-DNYTRIX_RUNTIME_ONLY",
                                      include_arg,
                                      "-c",
                                      runtime_src,
                                      "-o",
                                      out_runtime,
                                      NULL};
  if (verbose_enabled >= 2) {
    fprintf(stderr, "[**] Spawning runtime build:");
    for (int j = 0; runtime_args[j]; j++)
      fprintf(stderr, " %s", runtime_args[j]);
    fprintf(stderr, "\n");
  }
  int rc = ny_exec_spawn(runtime_args);
  if (rc != 0) {
    NY_LOG_ERR("Runtime compilation failed (exit=%d)\n", rc);
    return false;
  }
  if (out_ast) {
    const char *const ast_args[] = {cc,
                                    "-std=gnu11",
                                    debug ? "-g" : "-Os",
                                    "-fno-pie",
                                    "-fvisibility=hidden",
                                    "-ffunction-sections",
                                    "-fdata-sections",
                                    include_arg,
                                    "-c",
                                    ast_src,
                                    "-o",
                                    out_ast,
                                    NULL};
    rc = ny_exec_spawn(ast_args);
    if (rc != 0) {
      NY_LOG_ERR("Runtime AST compilation failed (exit=%d)\n", rc);
      return false;
    }
  }
  return true;
}

bool ny_builder_link(const char *cc, const char *obj_path,
                     const char *runtime_obj, const char *runtime_ast_obj,
                     const char *const extra_objs[], size_t extra_count,
                     const char *const link_dirs[], size_t link_dir_count,
                     const char *const link_libs[], size_t link_lib_count,
                     const char *output_path, bool link_strip, bool debug) {
  const size_t max_args = 128;
  const char *argv[max_args];
  size_t idx = 0;
  argv[idx++] = cc;
  if (debug)
    argv[idx++] = "-g";
  argv[idx++] = "-no-pie";
  argv[idx++] = obj_path;
  if (runtime_obj)
    argv[idx++] = runtime_obj;
  if (runtime_ast_obj)
    argv[idx++] = runtime_ast_obj;
  const char *shared_rt_path = NULL;
  for (size_t i = 0; i < extra_count; ++i) {
    if (idx + 12 >= max_args)
      break;
    argv[idx++] = extra_objs[i];
    /* Remember the first .so so we can add an rpath */
    if (!shared_rt_path) {
      const char *p = extra_objs[i];
      const char *dot = strrchr(p, '.');
      if (dot && strcmp(dot, ".so") == 0) {
        shared_rt_path = p;
      }
    }
  }
  for (size_t i = 0; i < link_dir_count; ++i) {
    if (idx + 1 >= max_args)
      break;
    argv[idx++] = link_dirs[i];
  }
  argv[idx++] = "-Wl,--build-id=none";
  argv[idx++] = "-Wl,--gc-sections";
  argv[idx++] = "-Wl,-O1";
  argv[idx++] = "-Wl,--no-as-needed";
  if (link_strip)
    argv[idx++] = "-Wl,--strip-all";
  if (shared_rt_path) {
    static char rpath_buf[PATH_MAX];
    const char *slash = strrchr(shared_rt_path, '/');
    if (slash) {
      size_t len = (size_t)(slash - shared_rt_path);
      if (len >= sizeof(rpath_buf))
        len = sizeof(rpath_buf) - 1;
      memcpy(rpath_buf, shared_rt_path, len);
      rpath_buf[len] = '\0';
      static char rpath_arg[PATH_MAX + 16];
      snprintf(rpath_arg, sizeof(rpath_arg), "-Wl,-rpath,%s", rpath_buf);
      argv[idx++] = rpath_arg;
      static char ldir_arg[PATH_MAX + 4];
      snprintf(ldir_arg, sizeof(ldir_arg), "-L%s", rpath_buf);
      argv[idx++] = ldir_arg;
      argv[idx++] = "-lnytrixrt";
    }
  }
  argv[idx++] = "-o";
  argv[idx++] = output_path;
  const char *readline_env = getenv("NYTRIX_LINK_READLINE");
  bool link_readline = readline_env && strcmp(readline_env, "0") != 0;
  argv[idx++] = "-lm";
  if (link_readline)
    argv[idx++] = "-lreadline";
  argv[idx++] = "-ldl";
  char *shared_buf = NULL;
  const char *shared_env = getenv("NYTRIX_SHARED_LIBS");
  const char *shared_libs[16];
  size_t shared_count = 0;
  if (shared_env) {
    shared_buf = ny_strdup(shared_env);
    if (shared_buf) {
      char *token_t = strtok(shared_buf, ":, ");
      while (token_t && shared_count < 16) {
        shared_libs[shared_count++] = token_t;
        token_t = strtok(NULL, ":, ");
      }
    }
  }
  for (size_t i = 0; i < shared_count; ++i) {
    argv[idx++] = shared_libs[i];
  }
  if (shared_buf)
    free(shared_buf);
  for (size_t i = 0; i < link_lib_count; ++i) {
    if (idx + 1 >= max_args)
      break;
    argv[idx++] = link_libs[i];
  }
  argv[idx++] = "-Wl,--as-needed";
  argv[idx] = NULL;
  int rc = ny_exec_spawn(argv);
  if (rc != 0) {
    NY_LOG_ERR("Linking failed (exit=%d)\n", rc);
    return false;
  }
  return true;
}

bool ny_builder_strip(const char *path) {
  if (!path)
    return false;
  const char *const argv[] = {"strip", "-s", path, NULL};
  int rc = ny_exec_spawn(argv);
  if (rc != 0) {
    NY_LOG_ERR("strip %s failed (exit=%d)\n", path, rc);
    return false;
  }
  return true;
}
