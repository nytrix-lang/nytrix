#include "code/native/internal.h"
#include "base/common.h"
#include "base/time.h"
#include "base/util.h"
#include "wire/build.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

/* Native-result oracle: emit, assemble, execute, capture, and compare the
 * selected backend result with the NYIR VM result. */

static bool ny_native_result_oracle_emit_asm(
    const ny_native_target_info_t *target, const ny_nir_func_t *rt_main,
    const ny_nir_func_t *funcs, const char **names, size_t count,
    const char *path, char *err, size_t err_len) {
  ny_native_writer_t w = {0};
  bool ok = false;
  if (!target || !rt_main || !path || !*path) {
    ny_native_set_err(err, err_len, "native oracle: missing assembly target");
    return false;
  }
  if (target->target != NY_NATIVE_TARGET_X86_64) {
    ny_native_set_err(err, err_len,
                      "native oracle: only x86-64 raw-int native is supported");
    return false;
  }
  if (!ny_native_printf(&w, "# Nytrix native result oracle (NYIR only)\n") ||
      !ny_native_put(&w, "\t.text\n"))
    goto done;
  for (size_t i = 0; i < count; ++i) {
    char label[256];
    snprintf(label, sizeof(label), "ny_fn_%s",
             names && names[i] && names[i][0] ? names[i] : "unknown_fn");
    if (!ny_native_emit_nir_func(&w, target, &funcs[i], label, false, err,
                                 err_len))
      goto done;
  }
  if (!ny_native_emit_nir_func(&w, target, rt_main, "rt_main", false, err,
                               err_len))
    goto done;
  ok = ny_write_file(path, w.data ? w.data : "", w.len) == 0;
  if (!ok)
    ny_native_set_err(err, err_len, "native oracle: failed to write %s: %s",
                      path, strerror(errno));
done:
  free(w.data);
  return ok;
}

static bool ny_native_parse_i64(const char *s, int64_t *out) {
  if (!s || !*s || !out)
    return false;
  errno = 0;
  char *end = NULL;
  long long v = strtoll(s, &end, 10);
  if (errno != 0 || end == s)
    return false;
  while (end && *end && isspace((unsigned char)*end))
    end++;
  if (end && *end)
    return false;
  *out = (int64_t)v;
  return true;
}

static bool ny_native_nir_returns_f64(const ny_nir_func_t *f) {
  if (!f || f->next_value <= 0)
    return false;
  bool f64[4096] = {0};
  int limit = f->next_value < 4096 ? f->next_value : 4096;
  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t i = 0; i < f->len; ++i) {
      const ny_nir_inst_t *in = &f->data[i];
      if (in->dst >= 0 && in->dst < limit &&
          (in->op == NYIR_CONST_F64 || in->op == NYIR_ADD_F64 ||
           in->op == NYIR_SUB_F64 || in->op == NYIR_MUL_F64 ||
           in->op == NYIR_DIV_F64 || in->op == NYIR_I64_TO_F64 ||
           in->op == NYIR_F32_TO_F64 ||
           (in->op == NY_NIR_CALL && (in->flags & NY_NIR_INST_F_RET_F64))) &&
          !f64[in->dst]) {
        f64[in->dst] = true;
        changed = true;
      }
      if (in->op == NY_NIR_COPY && in->a >= 0 && in->a < limit &&
          in->dst >= 0 && in->dst < limit && f64[in->a] && !f64[in->dst]) {
        f64[in->dst] = true;
        changed = true;
      }
    }
  }
  for (size_t i = f->len; i > 0; --i) {
    const ny_nir_inst_t *in = &f->data[i - 1];
    if (in->op == NY_NIR_RET && in->a >= 0 && in->a < limit)
      return f64[in->a];
  }
  return false;
}

static bool ny_native_nir_returns_f32(const ny_nir_func_t *f) {
  if (!f || f->next_value <= 0)
    return false;
  bool f32v[4096] = {0};
  int limit = f->next_value < 4096 ? f->next_value : 4096;
  for (size_t i = 0; i < f->len; ++i) {
    const ny_nir_inst_t *in = &f->data[i];
    if (in->dst >= 0 && in->dst < limit &&
        (in->op == NYIR_CONST_F32 || in->op == NYIR_ADD_F32 ||
         in->op == NYIR_SUB_F32 || in->op == NYIR_MUL_F32 ||
         in->op == NYIR_DIV_F32 || in->op == NYIR_I64_TO_F32 ||
         in->op == NYIR_F64_TO_F32 ||
         (in->op == NY_NIR_CALL && (in->flags & NY_NIR_INST_F_RET_F32))) &&
        !f32v[in->dst])
      f32v[in->dst] = true;
  }
  for (size_t i = f->len; i > 0; --i) {
    const ny_nir_inst_t *in = &f->data[i - 1];
    if (in->op == NY_NIR_RET && in->a >= 0 && in->a < limit)
      return f32v[in->a];
  }
  return false;
}

static int ny_native_run_capture_i64(const char *exe, int64_t *out,
                                     char *err, size_t err_len) {
#ifdef _WIN32
  (void)exe;
  (void)out;
  ny_native_set_err(err, err_len,
                    "native oracle: result capture is not implemented on Windows");
  return -1;
#else
  int pipefd[2];
  if (pipe(pipefd) != 0) {
    ny_native_set_err(err, err_len, "native oracle: pipe failed: %s",
                      strerror(errno));
    return -1;
  }
  pid_t pid = fork();
  if (pid == 0) {
    close(pipefd[0]);
    dup2(pipefd[1], STDOUT_FILENO);
    dup2(pipefd[1], STDERR_FILENO);
    close(pipefd[1]);
    execl(exe, exe, (char *)NULL);
    _exit(127);
  }
  close(pipefd[1]);
  if (pid < 0) {
    close(pipefd[0]);
    ny_native_set_err(err, err_len, "native oracle: fork failed: %s",
                      strerror(errno));
    return -1;
  }
  char buf[256];
  size_t len = 0;
  for (;;) {
    ssize_t n = read(pipefd[0], buf + len, sizeof(buf) - 1 - len);
    if (n > 0) {
      len += (size_t)n;
      if (len >= sizeof(buf) - 1)
        break;
      continue;
    }
    if (n < 0 && errno == EINTR)
      continue;
    break;
  }
  close(pipefd[0]);
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  buf[len] = '\0';
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    ny_native_set_err(err, err_len,
                      "native oracle: harness failed (status=%d output=%.*s)",
                      status, 180, buf);
    return -1;
  }
  char *line = strstr(buf, "native result function=rt_main returned=yes result=");
  if (!line) {
    ny_native_set_err(err, err_len,
                      "native oracle: missing result line (output=%.*s)", 180,
                      buf);
    return -1;
  }
  line += strlen("native result function=rt_main returned=yes result=");
  char *nl = strchr(line, '\n');
  if (nl)
    *nl = '\0';
  if (!ny_native_parse_i64(line, out)) {
    ny_native_set_err(err, err_len,
                      "native oracle: invalid result value '%s'", line);
    return -1;
  }
  return 0;
#endif
}

bool ny_native_result_oracle_for_program(const program_t *prog,
                                         const ny_options *opt, char *err,
                                         size_t err_len) {
  if (!prog || !opt || !opt->native_result_oracle)
    return true;
  ny_native_target_info_t target = {0};
  if (!ny_native_target_info_init(&target, opt) ||
      target.target != NY_NATIVE_TARGET_X86_64) {
    ny_native_set_err(err, err_len,
                      "native oracle: x86-64 native backend is required");
    return false;
  }

  ny_nir_func_t rt_main = {0};
  ny_nir_func_t funcs[128];
  const char *names[128];
  memset(funcs, 0, sizeof(funcs));
  memset(names, 0, sizeof(names));
  size_t count = 0;
  char local_err[512] = {0};
  if (!ny_native_build_nir(prog, opt, &rt_main, funcs, &count, 128, local_err,
                           sizeof(local_err))) {
    ny_native_set_err(err, err_len, "native oracle: %s",
                      local_err[0] ? local_err : "failed to build NYIR");
    return false;
  }
  size_t name_index = 0;
  for (size_t i = 0; prog && i < prog->body.len && name_index < count; ++i) {
    const stmt_t *stmt = prog->body.data[i];
    if (stmt && stmt->kind == NY_S_FUNC)
      names[name_index++] = stmt->as.fn.name ? stmt->as.fn.name : "<fn>";
  }

  bool ok = false;
  ny_nir_eval_result_t vm = {0};
  int64_t native_result = 0;
  char asm_path[4096], obj_path[4096], c_path[4096], exe_path[4096];
  unsigned long long stamp = (unsigned long long)ny_ticks_now();
  snprintf(asm_path, sizeof(asm_path), "%s/ny_oracle_%ld_%llu.s",
           ny_get_temp_dir(), (long)getpid(), stamp);
  snprintf(obj_path, sizeof(obj_path), "%s/ny_oracle_%ld_%llu.o",
           ny_get_temp_dir(), (long)getpid(), stamp);
  snprintf(c_path, sizeof(c_path), "%s/ny_oracle_%ld_%llu.c",
           ny_get_temp_dir(), (long)getpid(), stamp);
  snprintf(exe_path, sizeof(exe_path), "%s/ny_oracle_%ld_%llu",
           ny_get_temp_dir(), (long)getpid(), stamp);

  if (!ny_native_eval_ir_value(&rt_main, funcs, names, count, opt, &vm, err,
                               err_len))
    goto done;
  if (!vm.returned) {
    ny_native_set_err(err, err_len, "native oracle: VM did not return");
    goto done;
  }
  bool returns_f64 = ny_native_nir_returns_f64(&rt_main);
  bool returns_f32 = ny_native_nir_returns_f32(&rt_main);
  if (!ny_native_result_oracle_emit_asm(&target, &rt_main, funcs, names, count,
                                        asm_path, err, err_len))
    goto done;

  const char *cc = ny_builder_choose_cc();
  const char *as_argv[] = {cc, "-c", asm_path, "-o", obj_path, NULL};
  if (ny_exec_spawn(as_argv) != 0) {
    ny_native_set_err(err, err_len,
                      "native oracle: assembler failed for NYIR output");
    goto done;
  }
  const char *harness_i64 =
      "#include <stdio.h>\n"
      "extern long long rt_main(void);\n"
      "int main(void) {\n"
      "  long long r = rt_main();\n"
      "  printf(\"native result function=rt_main returned=yes result=%lld\\n\", r);\n"
      "  return 0;\n"
      "}\n";
  const char *harness_f64 =
      "#include <stdint.h>\n"
      "#include <stdio.h>\n"
      "#include <string.h>\n"
      "extern double rt_main(void);\n"
      "int main(void) {\n"
      "  double r = rt_main();\n"
      "  int64_t bits = 0;\n"
      "  memcpy(&bits, &r, sizeof(bits));\n"
      "  printf(\"native result function=rt_main returned=yes result=%lld\\n\", (long long)bits);\n"
      "  return 0;\n"
      "}\n";
  const char *harness_f32 =
      "#include <stdint.h>\n"
      "#include <stdio.h>\n"
      "#include <string.h>\n"
      "extern float rt_main(void);\n"
      "int main(void) {\n"
      "  float r = rt_main();\n"
      "  int32_t bits = 0;\n"
      "  memcpy(&bits, &r, sizeof(bits));\n"
      "  printf(\"native result function=rt_main returned=yes result=%lld\\n\", (long long)(int64_t)(uint32_t)bits);\n"
      "  return 0;\n"
      "}\n";
  const char *harness = returns_f32 ? harness_f32 :
                        returns_f64 ? harness_f64 : harness_i64;
  if (ny_write_file(c_path, harness, strlen(harness)) != 0) {
    ny_native_set_err(err, err_len, "native oracle: failed to write harness");
    goto done;
  }
  const char *link_argv[] = {cc, c_path, obj_path, "-no-pie", "-o", exe_path,
                             NULL};
  if (ny_exec_spawn(link_argv) != 0) {
    ny_native_set_err(err, err_len, "native oracle: harness link failed");
    goto done;
  }
  if (ny_native_run_capture_i64(exe_path, &native_result, err, err_len) != 0)
    goto done;
  if (native_result != vm.result) {
    ny_native_set_err(err, err_len,
                      "native oracle: VM/native mismatch vm=%" PRId64
                      " native=%" PRId64,
                      vm.result, native_result);
    goto done;
  }
  if (opt->native_result_oracle_expected &&
      opt->native_result_oracle_expected[0]) {
    int64_t expected = 0;
    if (!ny_native_parse_i64(opt->native_result_oracle_expected, &expected)) {
      ny_native_set_err(err, err_len,
                        "native oracle: invalid expected result '%s'",
                        opt->native_result_oracle_expected);
      goto done;
    }
    if (vm.result != expected) {
      ny_native_set_err(err, err_len,
                        "native oracle: expected=%" PRId64 " vm=%" PRId64
                        " native=%" PRId64,
                        expected, vm.result, native_result);
      goto done;
    }
  }
  fprintf(stderr,
          "native oracle function=rt_main vm=%" PRId64 " native=%" PRId64
          " ok=yes\n",
          vm.result, native_result);
  ok = true;

done:
  unlink(asm_path);
  unlink(obj_path);
  unlink(c_path);
  unlink(exe_path);
  ny_nir_func_free(&rt_main);
  for (size_t i = 0; i < count; ++i)
    ny_nir_func_free(&funcs[i]);
  return ok;
}
