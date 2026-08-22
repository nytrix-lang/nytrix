/*
 * Native result oracle: compares VM-executed NYIR results against
 * native-emitted code for correctness validation and fuzz testing.
 */
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
#ifdef _WIN32
#include <windows.h>
#endif

/*
 * Native-result oracle: emit, assemble, execute, capture, and compare the
 * selected backend result with the NYIR VM result.
 */

static bool ny_native_result_oracle_emit_asm(
    const ny_native_target_info_t *target, const nyir_func_t *rt_main,
    const nyir_func_t *funcs, const char **names, size_t count,
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
  if (!ny_native_strtab_append_asm(&w, err, err_len) ||
      !ny_native_arraytab_append_asm(&w, err, err_len))
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


static bool ny_native_oracle_has_runtime_call(const nyir_func_t *f) {
  if (!f)
    return false;
  for (size_t i = 0; i < f->len; ++i) {
    const nyir_inst_t *in = &f->data[i];
    if (in->op == NYIR_CALL && in->symbol &&
        strncmp(in->symbol, "rt_", 3) == 0)
      return true;
  }
  return false;
}

static bool ny_native_oracle_has_runtime_calls(const nyir_func_t *rt_main,
                                                const nyir_func_t *funcs,
                                                size_t count) {
  if (ny_native_oracle_has_runtime_call(rt_main))
    return true;
  for (size_t i = 0; i < count; ++i)
    if (ny_native_oracle_has_runtime_call(&funcs[i]))
      return true;
  return false;
}
static unsigned ny_native_nir_return_float_flags(const nyir_func_t *f) {
  if (!f || f->next_value <= 0)
    return 0;
  size_t locals = 0;
  for (size_t i = 0; i < f->len; ++i) {
    const nyir_inst_t *in = &f->data[i];
    if ((in->op == NYIR_LOAD_LOCAL || in->op == NYIR_STORE_LOCAL) &&
        in->imm >= 0 && (uint64_t)in->imm < SIZE_MAX &&
        (size_t)in->imm + 1 > locals)
      locals = (size_t)in->imm + 1;
  }
  nyir_type_map_t types = {0};
  if (!nyir_type_map_init(&types, f, locals))
    return 0;
  unsigned flags = 0;
  for (size_t i = f->len; i > 0; --i) {
    const nyir_inst_t *in = &f->data[i - 1];
    if (in->op != NYIR_RET)
      continue;
    flags = in->flags & (NYIR_INST_F_RET_F64 | NYIR_INST_F_RET_F32);
    if (in->a >= 0 && (size_t)in->a < types.value_count) {
      if (types.value_f64[in->a])
        flags |= NYIR_INST_F_RET_F64;
      if (types.value_f32[in->a])
        flags |= NYIR_INST_F_RET_F32;
    }
    break;
  }
  nyir_type_map_free(&types);
  return flags;
}
static int ny_native_run_capture_i64(const char *exe, int64_t *out,
                                     char *err, size_t err_len) {
#ifdef _WIN32
  if (!exe || !*exe || !out) {
    ny_native_set_err(err, err_len, "native oracle: invalid Windows capture input");
    return -1;
  }
  char cmdline[4096];
  int cmd_len = snprintf(cmdline, sizeof(cmdline), "\"%s\"", exe);
  if (cmd_len < 0 || (size_t)cmd_len >= sizeof(cmdline)) {
    ny_native_set_err(err, err_len, "native oracle: executable path is too long");
    return -1;
  }
  SECURITY_ATTRIBUTES sa;
  memset(&sa, 0, sizeof(sa));
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  HANDLE read_pipe = NULL;
  HANDLE write_pipe = NULL;
  if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0) ||
      !SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0)) {
    if (read_pipe)
      CloseHandle(read_pipe);
    if (write_pipe)
      CloseHandle(write_pipe);
    ny_native_set_err(err, err_len,
                      "native oracle: CreatePipe failed (win32=%lu)",
                      (unsigned long)GetLastError());
    return -1;
  }
  STARTUPINFOA si;
  PROCESS_INFORMATION pi;
  memset(&si, 0, sizeof(si));
  memset(&pi, 0, sizeof(pi));
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = write_pipe;
  si.hStdError = write_pipe;
  BOOL started = CreateProcessA(NULL, cmdline, NULL, NULL, TRUE,
                                CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
  CloseHandle(write_pipe);
  if (!started) {
    CloseHandle(read_pipe);
    ny_native_set_err(err, err_len,
                      "native oracle: CreateProcess failed (win32=%lu)",
                      (unsigned long)GetLastError());
    return -1;
  }
  char buf[256];
  char sink[256];
  size_t len = 0;
  for (;;) {
    DWORD got = 0;
    void *dst = len < sizeof(buf) - 1 ? (void *)(buf + len) : (void *)sink;
    DWORD cap = len < sizeof(buf) - 1
                    ? (DWORD)(sizeof(buf) - 1 - len)
                    : (DWORD)sizeof(sink);
    if (!ReadFile(read_pipe, dst, cap, &got, NULL) || got == 0)
      break;
    if (len < sizeof(buf) - 1)
      len += got;
  }
  CloseHandle(read_pipe);
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD exit_code = 1;
  GetExitCodeProcess(pi.hProcess, &exit_code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  buf[len] = '\0';
  if (exit_code != 0) {
    ny_native_set_err(err, err_len,
                      "native oracle: harness failed (status=%lu output=%.*s)",
                      (unsigned long)exit_code, 180, buf);
    return -1;
  }
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
#endif
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
}

/*
 * Run the VM/native comparison on an already-built NYIR bundle.  This is used
 * both for the end-to-end program oracle and for per-pass differential checks
 * during rt_main optimization.
 */
bool ny_native_result_oracle_for_nir(nyir_func_t *rt_main,
                                     nyir_func_t *funcs,
                                     const char **names, size_t count,
                                     const ny_options *opt, char *err,
                                     size_t err_len) {
  if (!rt_main || !opt)
    return true;
  ny_native_target_info_t target = {0};
  if (!ny_native_target_info_init(&target, opt) ||
      target.target != NY_NATIVE_TARGET_X86_64) {
    ny_native_set_err(err, err_len,
                      "native oracle: x86-64 native backend is required");
    return false;
  }

  bool ok = false;
  nyir_eval_result_t vm = {0};
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

  if (!ny_native_eval_ir_value(rt_main, funcs, names, count, opt, &vm, err,
                               err_len))
    goto done;
  if (!vm.returned) {
    ny_native_set_err(err, err_len, "native oracle: VM did not return");
    goto done;
  }
  unsigned return_float_flags = ny_native_nir_return_float_flags(rt_main);
  bool returns_f64 =
      (return_float_flags & NYIR_INST_F_RET_F64) != 0;
  bool returns_f32 =
      (return_float_flags & NYIR_INST_F_RET_F32) != 0;
  if (!ny_native_result_oracle_emit_asm(&target, rt_main, funcs, names, count,
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
  char runtime_obj[4096] = {0};
  bool needs_runtime =
      ny_native_oracle_has_runtime_calls(rt_main, funcs, count);
  if (needs_runtime) {
    char *exe_dir = ny_get_executable_dir();
    if (!exe_dir ||
        snprintf(runtime_obj, sizeof(runtime_obj),
                 "%s/CMakeFiles/nytrix_runtime.dir/src/rt/init.c.o", exe_dir) < 0 ||
        ny_access(runtime_obj, R_OK) != 0) {
      ny_native_set_err(
          err, err_len,
          "native oracle: runtime object unavailable for NYIR runtime calls");
      goto done;
    }
  }
  int link_status;
  if (needs_runtime) {
    const char *link_argv[] = {
        cc, c_path, obj_path, runtime_obj, "-Wl,--gc-sections",
        "-Wl,--allow-multiple-definition", "-lm", "-lpthread", "-ldl", "-lz",
        "-lgmp", "-no-pie", "-o", exe_path, NULL};
    link_status = ny_exec_spawn(link_argv);
  } else {
    const char *link_argv[] = {cc, c_path, obj_path, "-no-pie", "-o", exe_path,
                               NULL};
    link_status = ny_exec_spawn(link_argv);
  }
  if (link_status != 0) {
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
  nyir_eval_result_free(&vm);
  unlink(asm_path);
  unlink(obj_path);
  unlink(c_path);
  unlink(exe_path);
  return ok;
}

bool ny_native_result_oracle_for_program(const program_t *prog,
                                         const ny_options *opt, char *err,
                                         size_t err_len) {
  if (!prog || !opt || !opt->native_result_oracle)
    return true;

  nyir_func_t rt_main = {0};
  nyir_func_t funcs[128];
  const char *names[128];
  memset(funcs, 0, sizeof(funcs));
  memset(names, 0, sizeof(names));
  size_t count = 0;
  char local_err[512] = {0};
  if (!ny_native_build_nir(prog, opt, &rt_main, funcs, &count, names, 128,
                           local_err, sizeof(local_err))) {
    ny_native_set_err(err, err_len, "native oracle: %s",
                      local_err[0] ? local_err : "failed to build NYIR");
    return false;
  }

  bool ok = ny_native_result_oracle_for_nir(&rt_main, funcs, names, count, opt,
                                            err, err_len);
  nyir_func_free(&rt_main);
  for (size_t i = 0; i < count; ++i)
    nyir_func_free(&funcs[i]);
  return ok;
}

/*
 * Deterministic scalar NYIR fuzzer for the VM/native oracle.
 * Generates straight-line integer arithmetic with a seeded LCG and verifies
 * that the optimized native backend matches the NYIR VM on every program.
 */

typedef struct {
  uint64_t state;
} ny_native_fuzz_rng_t;

static uint64_t ny_native_fuzz_rng_next(ny_native_fuzz_rng_t *rng) {
  uint64_t x = rng->state;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  rng->state = x;
  return x * UINT64_C(2685821657736338717);
}

static int64_t ny_native_fuzz_i64(ny_native_fuzz_rng_t *rng, int bits) {
  uint64_t mask = bits >= 64 ? UINT64_MAX : ((UINT64_C(1) << bits) - 1);
  int64_t v = (int64_t)(ny_native_fuzz_rng_next(rng) & mask);
  /*
   * Sign-extend.
   */
  if (bits < 64 && (v >> (bits - 1)) & 1)
    v |= ~mask;
  return v;
}

static size_t ny_native_fuzz_pick(ny_native_fuzz_rng_t *rng, size_t n) {
  return n ? (size_t)(ny_native_fuzz_rng_next(rng) % n) : 0;
}

static bool ny_native_fuzz_emit_noerr(nyir_func_t *f, nyir_inst_t inst) {
  size_t before = f->len;
  (void)nyir_emit(f, inst);
  return f->len > before;
}

static bool ny_native_fuzz_emit_binary(nyir_func_t *f, nyir_op_t op,
                                       int left, int right, int *dst_out) {
  int dst = f->next_value++;
  bool ok = ny_native_fuzz_emit_noerr(f, (nyir_inst_t){.op = op,
                                                         .dst = dst,
                                                         .a = left,
                                                         .b = right,
                                                         .c = -1,
                                                         .d = -1,
                                                         .e = -1,
                                                         .f = -1,
                                                         .imm = 0,
                                                         .cmp = NYIR_CMP_EQ,
                                                         .symbol = NULL,
                                                         .flags = 0,
                                                         .effects = NYIR_EFFECT_NONE});
  *dst_out = dst;
  return ok;
}

static bool ny_native_fuzz_program(ny_native_fuzz_rng_t *rng,
                                   nyir_func_t *out, int min_insts,
                                   int max_insts) {
  memset(out, 0, sizeof(*out));
  int inst_count = min_insts +
                   (int)(ny_native_fuzz_rng_next(rng) % (max_insts - min_insts + 1));
  int *values = malloc((size_t)inst_count * sizeof(*values));
  if (!values)
    return false;
  int value_count = 0;
  bool ok = true;
  for (int i = 0; i < inst_count && ok; ++i) {
    if (value_count < 2 || (ny_native_fuzz_rng_next(rng) & 3) == 0) {
      /*
       * Emit a constant.  Keep values small so folds stay predictable.
       */
      int64_t c = ny_native_fuzz_i64(rng, 16);
      int dst = out->next_value++;
      ok = ny_native_fuzz_emit_noerr(out, (nyir_inst_t){.op = NYIR_CONST_I64,
                                                          .dst = dst,
                                                          .a = -1,
                                                          .b = -1,
                                                          .c = -1,
                                                          .d = -1,
                                                          .e = -1,
                                                          .f = -1,
                                                          .imm = c,
                                                          .cmp = NYIR_CMP_EQ,
                                                          .symbol = NULL,
                                                          .flags = 0,
                                                          .effects = NYIR_EFFECT_NONE});
      values[value_count++] = dst;
    } else {
      /*
       * Emit a binary op on two existing values.
       */
      size_t li = ny_native_fuzz_pick(rng, (size_t)value_count);
      size_t ri = ny_native_fuzz_pick(rng, (size_t)value_count);
      int left = values[li];
      int right = values[ri];
      static const nyir_op_t ops[] = {
          NYIR_ADD_I64, NYIR_SUB_I64, NYIR_MUL_I64,
          NYIR_AND_I64, NYIR_OR_I64,  NYIR_XOR_I64,
      };
      nyir_op_t op = ops[ny_native_fuzz_pick(rng, sizeof(ops) / sizeof(ops[0]))];
      nyir_cmp_t cmp = NYIR_CMP_EQ;
      if ((ny_native_fuzz_rng_next(rng) & 3) == 0) {
        op = NYIR_CMP_I64;
        cmp = (nyir_cmp_t)ny_native_fuzz_pick(rng, 6);
      }
      /*
       * Avoid division/mod by zero: skip div/mod entirely for now.
       */
      int dst = 0;
      if (op == NYIR_CMP_I64) {
        dst = out->next_value++;
        ok = ny_native_fuzz_emit_noerr(out, (nyir_inst_t){.op = op,
                                                            .dst = dst,
                                                            .a = left,
                                                            .b = right,
                                                            .c = -1,
                                                            .d = -1,
                                                            .e = -1,
                                                            .f = -1,
                                                            .imm = 0,
                                                            .cmp = cmp,
                                                            .symbol = NULL,
                                                            .flags = 0,
                                                            .effects = NYIR_EFFECT_NONE});
      } else {
        ok = ny_native_fuzz_emit_binary(out, op, left, right, &dst);
      }
      if (ok)
        values[value_count++] = dst;
    }
  }
  if (ok && value_count > 0) {
    ok = ny_native_fuzz_emit_noerr(out, (nyir_inst_t){.op = NYIR_RET,
                                                        .dst = -1,
                                                        .a = values[value_count - 1],
                                                        .b = -1,
                                                        .c = -1,
                                                        .d = -1,
                                                        .e = -1,
                                                        .f = -1,
                                                        .imm = 0,
                                                        .cmp = NYIR_CMP_EQ,
                                                        .symbol = NULL,
                                                        .flags = 0,
                                                        .effects = NYIR_EFFECT_CONTROL});
  }
  free(values);
  if (!ok) {
    nyir_func_free(out);
    return false;
  }
  return true;
}

bool ny_native_oracle_fuzz(const ny_options *opt, int count, char *err,
                           size_t err_len) {
  if (count <= 0)
    return true;
  ny_native_target_info_t target = {0};
  if (!ny_native_target_info_init(&target, opt) ||
      target.target != NY_NATIVE_TARGET_X86_64) {
    ny_native_set_err(err, err_len,
                      "native oracle fuzz: x86-64 native backend is required");
    return false;
  }
  bool ok = true;
  int failures = 0;
  for (int seed = 0; seed < count && failures < 3; ++seed) {
    ny_native_fuzz_rng_t rng = {(uint64_t)(seed + 1)};
    nyir_func_t rt_main = {0};
    if (!ny_native_fuzz_program(&rng, &rt_main, 3, 16)) {
      ny_native_set_err(err, err_len,
                        "native oracle fuzz: failed to generate program %d", seed);
      ok = false;
      break;
    }
    char local_err[512] = {0};
    if (!ny_native_result_oracle_for_nir(&rt_main, NULL, NULL, 0, opt,
                                         local_err, sizeof(local_err))) {
      fprintf(stderr, "native oracle fuzz seed=%d failed: %s\n", seed,
              local_err[0] ? local_err : NY_NATIVE_UNKNOWN_ERR);
      nyir_dump(stderr, &rt_main, "<fuzz-failure>");
      ++failures;
      ok = false;
    }
    nyir_func_free(&rt_main);
  }
  if (ok)
    fprintf(stderr, "native oracle fuzz: %d programs passed\n", count);
  if (!ok && err && err_len > 0 && err[0] == '\0')
    ny_native_set_err(err, err_len, "native oracle fuzz: mismatch detected");
  return ok;
}
