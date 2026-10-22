/*
 * Native result oracle: compares VM-executed NYIR results against
 * native-emitted code for correctness validation and fuzz testing.
 */
#include "base/common.h"
#include "base/time.h"
#include "base/util.h"
#include "code/backend/internal.h"
#include "code/lang/pipeline/build.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern int64_t rt_any_to_i64(int64_t value);
#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif
#ifdef _WIN32
#include <windows.h>
#endif

static const char *nyr_native_oracle_function_name(const char *const *names,
                                                   size_t index) {
  return names && names[index] && names[index][0] ? names[index] : "unknown_fn";
}

static size_t nyr_native_oracle_name_hash(const char *name) {
  uint64_t hash = UINT64_C(1469598103934665603);
  for (const unsigned char *p = (const unsigned char *)name; *p; ++p)
    hash = (hash ^ *p) * UINT64_C(1099511628211);
  return (size_t)hash;
}

/*
 * Native-result oracle: emit, assemble, execute, capture, and compare the
 * selected backend result with the NYIR VM result.
 */

static bool nyr_native_result_oracle_emit_asm(
    const ny_native_target_info_t *target, const nyr_func_t *rt_main,
    const nyr_func_t *funcs, const char *const *names, size_t count,
    const char *path, char *err, size_t err_len) {
  nyr_writer_t w = {0};
  char **emitted_aliases = NULL;
  size_t emitted_alias_count = 0;
  size_t emitted_alias_cap = 0;
  size_t *emitted_function_slots = NULL;
  size_t emitted_function_slot_cap = 0;
  bool ok = false;
  if (!target || !rt_main || !path || !*path) {
    nyr_set_error(err, err_len, "native oracle: missing assembly target");
    return false;
  }
  if (target->target != NY_NATIVE_TARGET_X86_64) {
    nyr_set_error(err, err_len,
                  "native oracle: only x86-64 raw-int native is supported");
    return false;
  }
  if (!nyr_printf(&w, "# Nytrix native result oracle (NYIR only)\n") ||
      !nyr_put(&w, "\t.text\n"))
    goto done;
  /*
   * Materialize pooled literals before code so local-label relocations are
   * resolved by the assembler as real cross-section references.
   */
  if (!nyr_strtab_append_asm(&w, err, err_len) ||
      !nyr_arraytab_append_asm(&w, err, err_len))
    goto done;
  if (count) {
    if (count > SIZE_MAX / 2) {
      nyr_set_error(err, err_len,
                    "native oracle: function symbol table overflow");
      goto done;
    }
    emitted_function_slot_cap = 64;
    while (emitted_function_slot_cap < count * 2) {
      if (emitted_function_slot_cap > SIZE_MAX / 2) {
        nyr_set_error(err, err_len,
                      "native oracle: function symbol table overflow");
        goto done;
      }
      emitted_function_slot_cap *= 2;
    }
    if (emitted_function_slot_cap >
        SIZE_MAX / sizeof(*emitted_function_slots)) {
      nyr_set_error(err, err_len,
                    "native oracle: function symbol table overflow");
      goto done;
    }
    emitted_function_slots =
        calloc(emitted_function_slot_cap, sizeof(*emitted_function_slots));
    if (!emitted_function_slots) {
      nyr_set_error(err, err_len,
                    "native oracle: function symbol table allocation failed");
      goto done;
    }
  }
  for (size_t i = 0; i < count; ++i) {
    const char *function_name = nyr_native_oracle_function_name(names, i);
    size_t slot = nyr_native_oracle_name_hash(function_name) &
                  (emitted_function_slot_cap - 1);
    while (emitted_function_slots[slot]) {
      size_t previous = emitted_function_slots[slot] - 1;
      if (strcmp(nyr_native_oracle_function_name(names, previous),
                 function_name) == 0)
        break;
      slot = (slot + 1) & (emitted_function_slot_cap - 1);
    }
    if (emitted_function_slots[slot])
      continue;
    emitted_function_slots[slot] = i + 1;
    char label[256];
    snprintf(label, sizeof(label), "ny_fn_%s", function_name);
    if (!nyr_printf(&w, "\t.section .text.%s,\"ax\",@progbits\n", label))
      goto done;
    if (!nyr_emit_func(&w, target, &funcs[i], label, false, err, err_len))
      goto done;
  }
  if (!nyr_put(&w, "\t.section .text.rt_main,\"ax\",@progbits\n"))
    goto done;
  if (!nyr_emit_func(&w, target, rt_main, "rt_main", false, err, err_len))
    goto done;
  /*
   * Reachability records function names by their local AST spelling, while
   * calls from imported modules carry the canonical qualified spelling.  The
   * normal object writer resolves that relationship internally; this small
   * standalone assembly harness must publish the equivalent aliases.
   */
  for (size_t pass = 0; pass <= count; ++pass) {
    const nyr_func_t *f = pass < count ? &funcs[pass] : rt_main;
    if (!f)
      continue;
    for (size_t j = 0; j < f->len; ++j) {
      const nyr_inst_t *in = &f->data[j];
      if (in->op != NYIR_CALL || !in->symbol || !strchr(in->symbol, '.'))
        continue;
      bool exact_function_emitted = false;
      for (size_t k = 0; k < count; ++k)
        if (names && names[k] && strcmp(names[k], in->symbol) == 0) {
          exact_function_emitted = true;
          break;
        }
      if (exact_function_emitted)
        continue;
      const char *tail = strrchr(in->symbol, '.');
      if (!tail || !tail[1])
        continue;
      for (size_t k = 0; k < count; ++k) {
        const char *bare = names && names[k] ? names[k] : NULL;
        if (!bare || strcmp(bare, tail + 1) != 0)
          continue;
        char qualified[768];
        char local[512];
        int qn = snprintf(qualified, sizeof(qualified), "ny_fn_%s", in->symbol);
        int ln = snprintf(local, sizeof(local), "ny_fn_%s", bare);
        if (qn <= 0 || (size_t)qn >= sizeof(qualified) || ln <= 0 ||
            (size_t)ln >= sizeof(local))
          goto done;
        bool emitted = false;
        for (size_t a = 0; a < emitted_alias_count; ++a)
          if (strcmp(emitted_aliases[a], qualified) == 0) {
            emitted = true;
            break;
          }
        if (emitted)
          break;
        if (emitted_alias_count == emitted_alias_cap) {
          size_t next = emitted_alias_cap ? emitted_alias_cap * 2 : 16;
          if (next < emitted_alias_cap || next > SIZE_MAX / sizeof(char *)) {
            nyr_set_error(err, err_len, "native oracle: alias table overflow");
            goto done;
          }
          char **grown = realloc(emitted_aliases, next * sizeof(char *));
          if (!grown) {
            nyr_set_error(err, err_len,
                          "native oracle: alias table allocation failed");
            goto done;
          }
          emitted_aliases = grown;
          emitted_alias_cap = next;
        }
        size_t alias_len = strlen(qualified);
        emitted_aliases[emitted_alias_count] = malloc(alias_len + 1);
        if (!emitted_aliases[emitted_alias_count]) {
          nyr_set_error(err, err_len,
                        "native oracle: alias name allocation failed");
          goto done;
        }
        memcpy(emitted_aliases[emitted_alias_count++], qualified,
               alias_len + 1);
        if (!nyr_printf(&w, "\t.globl\t%s\n\t.set\t%s,%s\n", qualified,
                        qualified, local))
          goto done;
        break;
      }
    }
  }
  /*
   * Early optimization checkpoints legitimately retain top-level mutable
   * cells as ADDR_SYMBOL/LOAD/STORE operations.  The normal object writer
   * appends those cells as data definitions; the standalone oracle assembly
   * must provide the equivalent common symbols or its temporary harness link
   * fails on names such as `sum` and `i`.
   */
  const char *globals[256] = {0};
  size_t global_count = 0;
  for (size_t pass = 0; pass <= count; ++pass) {
    const nyr_func_t *f = pass < count ? &funcs[pass] : rt_main;
    if (!f)
      continue;
    for (size_t j = 0; j < f->len; ++j) {
      const nyr_inst_t *in = &f->data[j];
      if (in->op != NYIR_ADDR_SYMBOL || !in->symbol ||
          !nyr_globaltab_has(in->symbol))
        continue;
      bool seen = false;
      for (size_t k = 0; k < global_count; ++k)
        if (strcmp(globals[k], in->symbol) == 0) {
          seen = true;
          break;
        }
      if (seen)
        continue;
      if (global_count == sizeof(globals) / sizeof(globals[0])) {
        nyr_set_error(err, err_len, "native oracle: too many global cells");
        goto done;
      }
      globals[global_count++] = in->symbol;
    }
  }
  for (size_t i = 0; i < global_count; ++i)
    if (!nyr_printf(&w, "\t.comm\t%s,8,8\n", globals[i]))
      goto done;
  ok = ny_write_file(path, w.data ? w.data : "", w.len) == 0;
  if (!ok)
    nyr_set_error(err, err_len, "native oracle: failed to write %s: %s", path,
                  strerror(errno));
done:
  for (size_t i = 0; i < emitted_alias_count; ++i)
    free(emitted_aliases[i]);
  free(emitted_aliases);
  free(emitted_function_slots);
  free(w.data);
  return ok;
}

static bool nyr_native_parse_i64(const char *s, int64_t *out) {
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

static bool nyr_native_parse_expected_result(const char *s,
                                             nyr_scalar_class_t return_class,
                                             int64_t *out) {
  if (return_class != NYIR_SCALAR_F32 && return_class != NYIR_SCALAR_F64)
    return nyr_native_parse_i64(s, out);
  if (!s || !*s || !out)
    return false;
  /*
   * Existing oracle fixtures often spell floating results as their raw IEEE
   * integer bits. Preserve that form; only decimal/exponent notation asks
   * for numeric floating-point parsing (for example `3.75` or `1e-3`).
   */
  bool has_float_marker = false;
  for (const char *p = s; *p; ++p)
    if (*p == '.' || *p == 'e' || *p == 'E') {
      has_float_marker = true;
      break;
    }
  if (!has_float_marker)
    return nyr_native_parse_i64(s, out);
  errno = 0;
  char *end = NULL;
  /*
   * A value can have both facts when typed locals are unioned by the NYIR
   * type map (for example, an f32 expression stored into an f64 local).
   * The wider declared return fact is authoritative in that case.
   */
  if (return_class == NYIR_SCALAR_F32) {
    float value = strtof(s, &end);
    if (errno != 0 || end == s)
      return false;
    while (end && *end && isspace((unsigned char)*end))
      end++;
    if (end && *end)
      return false;
    uint32_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    *out = (int64_t)bits;
    return true;
  }
  double value = strtod(s, &end);
  if (errno != 0 || end == s)
    return false;
  while (end && *end && isspace((unsigned char)*end))
    end++;
  if (end && *end)
    return false;
  memcpy(out, &value, sizeof(value));
  return true;
}

static bool nyr_native_oracle_has_runtime_call(const nyr_func_t *f) {
  if (!f)
    return false;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op == NYIR_CALL && in->symbol && strncmp(in->symbol, "rt_", 3) == 0)
      return true;
  }
  return false;
}

static bool nyr_native_oracle_has_runtime_calls(const nyr_func_t *rt_main,
                                                const nyr_func_t *funcs,
                                                size_t count) {
  if (nyr_native_oracle_has_runtime_call(rt_main))
    return true;
  for (size_t i = 0; i < count; ++i)
    if (nyr_native_oracle_has_runtime_call(&funcs[i]))
      return true;
  return false;
}
static nyr_scalar_class_t nyr_return_scalar_class(const nyr_func_t *f,
                                                   const nyr_func_t *funcs,
                                                   const char *const *names,
                                                   size_t count) {
  if (!f || f->next_value <= 0)
    return NYIR_SCALAR_UNKNOWN;
  size_t locals = 0;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if ((in->op == NYIR_LOAD_LOCAL || in->op == NYIR_STORE_LOCAL) &&
        in->imm >= 0 && (uint64_t)in->imm < SIZE_MAX &&
        (size_t)in->imm + 1 > locals)
      locals = (size_t)in->imm + 1;
  }
  nyr_type_map_t types = {0};
  if (!nyr_type_map_init(&types, f, locals))
    return NYIR_SCALAR_UNKNOWN;
  nyr_scalar_class_t result = NYIR_SCALAR_UNKNOWN;
  for (size_t i = f->len; i > 0; --i) {
    const nyr_inst_t *in = &f->data[i - 1];
    if (in->op != NYIR_RET)
      continue;
    if (in->scalar_class == NYIR_SCALAR_F64 ||
        in->scalar_class == NYIR_SCALAR_F32)
      result = (nyr_scalar_class_t)in->scalar_class;
    if (in->a >= 0 && (size_t)in->a < types.value_count) {
      bool f64 = types.value_f64[in->a];
      bool f32 = types.value_f32[in->a];
      if (result == NYIR_SCALAR_UNKNOWN && f64 != f32)
        result = f64 ? NYIR_SCALAR_F64 : NYIR_SCALAR_F32;
      if (result == NYIR_SCALAR_UNKNOWN) {
        for (size_t j = 0; j < f->len; ++j) {
          const nyr_inst_t *call = &f->data[j];
          if (call->op != NYIR_CALL || call->dst != in->a || !call->symbol)
            continue;
          for (size_t k = 0; k < count; ++k) {
            if (!names || !names[k])
              continue;
            const char *tail = strrchr(call->symbol, '.');
            if (strcmp(call->symbol, names[k]) != 0 &&
                strcmp(tail ? tail + 1 : call->symbol, names[k]) != 0)
              continue;
            result = nyr_return_scalar_class(&funcs[k], NULL, NULL, 0);
            break;
          }
          break;
        }
      }
    }
    break;
  }
  nyr_type_map_free(&types);
  return result;
}
static int nyr_native_run_capture_i64(const char *exe, int64_t *out, char *err,
                                      size_t err_len) {
#ifdef _WIN32
  if (!exe || !*exe || !out) {
    nyr_set_error(err, err_len, "native oracle: invalid Windows capture input");
    return -1;
  }
  char cmdline[4096];
  int cmd_len = snprintf(cmdline, sizeof(cmdline), "\"%s\"", exe);
  if (cmd_len < 0 || (size_t)cmd_len >= sizeof(cmdline)) {
    nyr_set_error(err, err_len, "native oracle: executable path is too long");
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
    nyr_set_error(err, err_len, "native oracle: CreatePipe failed (win32=%lu)",
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
    nyr_set_error(err, err_len,
                  "native oracle: CreateProcess failed (win32=%lu)",
                  (unsigned long)GetLastError());
    return -1;
  }
  char buf[256];
  char chunk[256];
  size_t len = 0;
  for (;;) {
    DWORD got = 0;
    if (!ReadFile(read_pipe, chunk, sizeof(chunk) - 1, &got, NULL) || got == 0)
      break;
    size_t keep = len;
    if (keep + got > sizeof(buf) - 1)
      keep = sizeof(buf) - 1 - got;
    memmove(buf, buf + len - keep, keep);
    memcpy(buf + keep, chunk, got);
    len = keep + got;
  }
  CloseHandle(read_pipe);
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD exit_code = 1;
  GetExitCodeProcess(pi.hProcess, &exit_code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  buf[len] = '\0';
  if (exit_code != 0) {
    nyr_set_error(err, err_len,
                  "native oracle: harness failed (status=%lu output=%.*s)",
                  (unsigned long)exit_code, 180, buf);
    return -1;
  }
#else
  int pipefd[2];
  if (pipe(pipefd) != 0) {
    nyr_set_error(err, err_len, "native oracle: pipe failed: %s",
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
    nyr_set_error(err, err_len, "native oracle: fork failed: %s",
                  strerror(errno));
    return -1;
  }
  char buf[256];
  char chunk[256];
  size_t len = 0;
  for (;;) {
    ssize_t n = read(pipefd[0], chunk, sizeof(chunk) - 1);
    if (n > 0) {
      /*
       * Drain arbitrary user output and retain the tail containing the
       * harness result.  Closing a full pipe early can also kill the child
       * with SIGPIPE before it returns.
       */
      size_t keep = len;
      if (keep + (size_t)n > sizeof(buf) - 1)
        keep = sizeof(buf) - 1 - (size_t)n;
      memmove(buf, buf + len - keep, keep);
      memcpy(buf + keep, chunk, (size_t)n);
      len = keep + (size_t)n;
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
    nyr_set_error(err, err_len,
                  "native oracle: harness failed (status=%d output=%.*s)",
                  status, 180, buf);
    return -1;
  }
#endif
  char *line =
      strstr(buf, "native result function=rt_main returned=yes result=");
  if (!line) {
    nyr_set_error(err, err_len,
                  "native oracle: missing result line (output=%.*s)", 180, buf);
    return -1;
  }
  line += strlen("native result function=rt_main returned=yes result=");
  char *nl = strchr(line, '\n');
  if (nl)
    *nl = '\0';
  if (!nyr_native_parse_i64(line, out)) {
    nyr_set_error(err, err_len, "native oracle: invalid result value '%s'",
                  line);
    return -1;
  }
  return 0;
}

/*
 * Run the VM/native comparison on an already-built NYIR bundle.  This is used
 * both for the end-to-end program oracle and for per-pass differential checks
 * during rt_main optimization.
 */
bool nyr_native_result_oracle_for_nir(
    nyr_func_t *rt_main, nyr_func_t *funcs, const char *const *names,
    size_t count, const ny_options *opt,
    const nyr_runtime_services_t *runtime_services, char *err, size_t err_len) {
  if (!rt_main || !opt)
    return true;
  const nyr_target_request_t target_request = nyr_target_request_make(
      opt->native_backend, opt->native_abi, opt->host_triple, opt->opt_size);
  ny_native_target_info_t target = {0};
  if (!nyr_target_info_init(&target, &target_request) ||
      target.target != NY_NATIVE_TARGET_X86_64) {
    nyr_set_error(err, err_len,
                  "native oracle: x86-64 native backend is required");
    return false;
  }

  bool ok = false;
  nyr_eval_result_t vm = {0};
  int64_t native_result = 0;
  char asm_path[4096], obj_path[4096], c_path[4096], exe_path[4096];
  unsigned long long stamp = (unsigned long long)ny_ticks_now();
  snprintf(asm_path, sizeof(asm_path), "%s/ny_oracle_%ld_%llu.s",
           ny_get_temp_dir(), (long)getpid(), stamp);
  snprintf(obj_path, sizeof(obj_path), "%s/ny_oracle_%ld_%llu.o",
           ny_get_temp_dir(), (long)getpid(), stamp);
  snprintf(c_path, sizeof(c_path), "%s/ny_oracle_%ld_%llu.c", ny_get_temp_dir(),
           (long)getpid(), stamp);
  snprintf(exe_path, sizeof(exe_path), "%s/ny_oracle_%ld_%llu",
           ny_get_temp_dir(), (long)getpid(), stamp);

  if (!runtime_services || !runtime_services->evaluate_ir ||
      !runtime_services->evaluate_ir(runtime_services->userdata, opt, rt_main,
                                     funcs, names, count, &vm, err, err_len))
    goto done;
  if (!vm.returned) {
    nyr_set_error(err, err_len, "native oracle: VM did not return");
    goto done;
  }
  nyr_scalar_class_t return_class =
      nyr_return_scalar_class(rt_main, funcs, names, count);
  bool returns_f64 = return_class == NYIR_SCALAR_F64;
  bool returns_f32 = return_class == NYIR_SCALAR_F32;
  int64_t expected = 0;
  bool has_expected =
      opt->native_result_oracle_expected &&
      opt->native_result_oracle_expected[0] &&
      nyr_native_parse_expected_result(opt->native_result_oracle_expected,
                                       return_class, &expected);
  bool decode_tagged_integer = has_expected && expected >= (INT64_MIN >> 1) &&
                               expected <= (INT64_MAX >> 1) &&
                               vm.result == ((expected << 1) | 1);
  if (decode_tagged_integer && (vm.result & 1))
    vm.result >>= 1;
  if (!nyr_native_result_oracle_emit_asm(&target, rt_main, funcs, names, count,
                                         asm_path, err, err_len))
    goto done;
  const char *cc = ny_builder_choose_cc();
  const char *as_argv[] = {cc, "-c", asm_path, "-o", obj_path, NULL};
  if (ny_exec_spawn(as_argv) != 0) {
    nyr_set_error(err, err_len,
                  "native oracle: assembler failed for NYIR output");
    goto done;
  }
  const char *harness_i64 =
      "#include <stdio.h>\n"
      "extern long long rt_set_args_raw(int, char **, char **) "
      "__attribute__((weak));\n"
      "extern long long rt_main(void);\n"
      "extern long long rt_any_to_i64(long long);\n"
      "int main(int argc, char **argv) {\n"
      "  if (rt_set_args_raw) rt_set_args_raw(argc, argv, 0);\n"
      "  long long r = rt_main();\n"
      "%s"
      "  printf(\"native result function=rt_main returned=yes "
      "result=%%lld\\n\", r);\n"
      "  return 0;\n"
      "}\n";
  const char *harness_f64 =
      "#include <stdint.h>\n"
      "#include <stdio.h>\n"
      "#include <string.h>\n"
      "extern long long rt_set_args_raw(int, char **, char **) "
      "__attribute__((weak));\n"
      "extern double rt_main(void);\n"
      "int main(int argc, char **argv) {\n"
      "  if (rt_set_args_raw) rt_set_args_raw(argc, argv, 0);\n"
      "  double r = rt_main();\n"
      "  int64_t bits = 0;\n"
      "  memcpy(&bits, &r, sizeof(bits));\n"
      "  printf(\"native result function=rt_main returned=yes "
      "result=%lld\\n\", (long long)bits);\n"
      "  return 0;\n"
      "}\n";
  const char *harness_f32 =
      "#include <stdint.h>\n"
      "#include <stdio.h>\n"
      "#include <string.h>\n"
      "extern long long rt_set_args_raw(int, char **, char **) "
      "__attribute__((weak));\n"
      "extern float rt_main(void);\n"
      "int main(int argc, char **argv) {\n"
      "  if (rt_set_args_raw) rt_set_args_raw(argc, argv, 0);\n"
      "  float r = rt_main();\n"
      "  int32_t bits = 0;\n"
      "  memcpy(&bits, &r, sizeof(bits));\n"
      "  printf(\"native result function=rt_main returned=yes "
      "result=%lld\\n\", (long long)(int64_t)(uint32_t)bits);\n"
      "  return 0;\n"
      "}\n";
  char harness_i64_source[1024];
  snprintf(harness_i64_source, sizeof(harness_i64_source), harness_i64,
           decode_tagged_integer ? "  r = rt_any_to_i64(r);\n" : "");
  const char *harness = returns_f64   ? harness_f64
                        : returns_f32 ? harness_f32
                                      : harness_i64_source;
  if (ny_write_file(c_path, harness, strlen(harness)) != 0) {
    nyr_set_error(err, err_len, "native oracle: failed to write harness");
    goto done;
  }
  char runtime_obj[4096] = {0};
  bool needs_runtime =
      decode_tagged_integer ||
      nyr_native_oracle_has_runtime_calls(rt_main, funcs, count);
  if (needs_runtime) {
    char *exe_dir = ny_get_executable_dir();
    if (!exe_dir ||
        snprintf(
            runtime_obj, sizeof(runtime_obj),
            "%s/CMakeFiles/nytrix_runtime.dir/src/code/lang/runtime/init.c.o",
            exe_dir) < 0 ||
        ny_access(runtime_obj, R_OK) != 0) {
      nyr_set_error(
          err, err_len,
          "native oracle: runtime object unavailable for NYIR runtime calls");
      goto done;
    }
  }
  int link_status;
  if (needs_runtime) {
    const char *link_argv[] = {cc,
                               c_path,
                               obj_path,
                               runtime_obj,
                               "-Wl,--gc-sections",
                               "-Wl,--allow-multiple-definition",
                               "-lm",
                               "-lpthread",
                               "-ldl",
                               "-lz",
                               "-no-pie",
                               "-o",
                               exe_path,
                               NULL};
    link_status = ny_exec_spawn(link_argv);
  } else {
    const char *link_argv[] = {cc,   c_path,   obj_path, "-no-pie",
                               "-o", exe_path, NULL};
    link_status = ny_exec_spawn(link_argv);
  }
  if (link_status != 0) {
    nyr_set_error(err, err_len, "native oracle: harness link failed");
    goto done;
  }
  if (nyr_native_run_capture_i64(exe_path, &native_result, err, err_len) != 0)
    goto done;
  /*
   * The standalone rt_main harness observes the language ABI's tagged
   * integer return, while the NYIR evaluator reports the scalar value. When
   * an expected integer is available, normalize that exact tagged encoding
   * before comparing; do not guess from arbitrary odd result bits.
   */
  if (has_expected && expected >= (INT64_MIN >> 1) &&
      expected <= (INT64_MAX >> 1) && native_result == ((expected << 1) | 1))
    native_result = expected;
  if (native_result != vm.result) {
    nyr_set_error(err, err_len,
                  "native oracle: VM/native mismatch vm=%" PRId64
                  " native=%" PRId64,
                  vm.result, native_result);
    goto done;
  }
  if (opt->native_result_oracle_expected &&
      opt->native_result_oracle_expected[0]) {
    if (!has_expected) {
      nyr_set_error(err, err_len, "native oracle: invalid expected result '%s'",
                    opt->native_result_oracle_expected);
      goto done;
    }
    if (vm.result != expected) {
      nyr_set_error(err, err_len,
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
  nyr_eval_result_free(&vm);
  /*
   * Keep a failed generated-code oracle bundle when explicitly requested.
   */
  bool keep_failed = !ok && getenv("NY_NATIVE_ORACLE_KEEP") &&
                     getenv("NY_NATIVE_ORACLE_KEEP")[0] &&
                     strcmp(getenv("NY_NATIVE_ORACLE_KEEP"), "0") != 0;
  if (keep_failed) {
    fprintf(
        stderr,
        "native oracle artifacts kept: asm=%s object=%s harness=%s exe=%s\n",
        asm_path, obj_path, c_path, exe_path);
  } else {
    unlink(asm_path);
    unlink(obj_path);
    unlink(c_path);
    unlink(exe_path);
  }
  return ok;
}

/*
 * Deterministic scalar NYIR fuzzer for the VM/native oracle.
 * Generates straight-line integer arithmetic with a seeded LCG and verifies
 * that the optimized native backend matches the NYIR VM on every program.
 */

typedef struct {
  uint64_t state;
} nyr_native_fuzz_rng_t;

static uint64_t nyr_native_fuzz_rng_next(nyr_native_fuzz_rng_t *rng) {
  uint64_t x = rng->state;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  rng->state = x;
  return x * UINT64_C(2685821657736338717);
}

static int64_t nyr_native_fuzz_i64(nyr_native_fuzz_rng_t *rng, int bits) {
  uint64_t mask = bits >= 64 ? UINT64_MAX : ((UINT64_C(1) << bits) - 1);
  int64_t v = (int64_t)(nyr_native_fuzz_rng_next(rng) & mask);
  /*
   * Sign-extend.
   */
  if (bits < 64 && (v >> (bits - 1)) & 1)
    v |= ~mask;
  return v;
}

static size_t nyr_native_fuzz_pick(nyr_native_fuzz_rng_t *rng, size_t n) {
  return n ? (size_t)(nyr_native_fuzz_rng_next(rng) % n) : 0;
}

static bool nyr_native_fuzz_emit_noerr(nyr_func_t *f, nyr_inst_t inst) {
  size_t before = f->len;
  (void)nyr_ir_emit(f, inst);
  return f->len > before;
}

static bool nyr_native_fuzz_emit_binary(nyr_func_t *f, nyr_op_t op, int left,
                                        int right, int *dst_out) {
  int dst = f->next_value++;
  bool ok =
      nyr_native_fuzz_emit_noerr(f, (nyr_inst_t){.op = op,
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

static bool nyr_native_fuzz_program(nyr_native_fuzz_rng_t *rng,
                                    nyr_func_t *out, int min_insts,
                                    int max_insts) {
  memset(out, 0, sizeof(*out));
  int inst_count = min_insts + (int)(nyr_native_fuzz_rng_next(rng) %
                                     (max_insts - min_insts + 1));
  int *values = malloc((size_t)inst_count * sizeof(*values));
  if (!values)
    return false;
  int value_count = 0;
  bool ok = true;
  for (int i = 0; i < inst_count && ok; ++i) {
    if (value_count < 2 || (nyr_native_fuzz_rng_next(rng) & 3) == 0) {
      /*
       * Emit a constant.  Keep values small so folds stay predictable.
       */
      int64_t c = nyr_native_fuzz_i64(rng, 16);
      int dst = out->next_value++;
      ok = nyr_native_fuzz_emit_noerr(
          out, (nyr_inst_t){.op = NYIR_CONST_I64,
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
      size_t li = nyr_native_fuzz_pick(rng, (size_t)value_count);
      size_t ri = nyr_native_fuzz_pick(rng, (size_t)value_count);
      int left = values[li];
      int right = values[ri];
      static const nyr_op_t ops[] = {
          NYIR_ADD_I64, NYIR_SUB_I64, NYIR_MUL_I64,
          NYIR_AND_I64, NYIR_OR_I64,  NYIR_XOR_I64,
      };
      nyr_op_t op =
          ops[nyr_native_fuzz_pick(rng, sizeof(ops) / sizeof(ops[0]))];
      nyr_cmp_t cmp = NYIR_CMP_EQ;
      if ((nyr_native_fuzz_rng_next(rng) & 3) == 0) {
        op = NYIR_CMP_I64;
        cmp = (nyr_cmp_t)nyr_native_fuzz_pick(rng, 6);
      }
      /*
       * Avoid division/mod by zero: skip div/mod entirely for now.
       */
      int dst = 0;
      if (op == NYIR_CMP_I64) {
        dst = out->next_value++;
        ok = nyr_native_fuzz_emit_noerr(
            out, (nyr_inst_t){.op = op,
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
        ok = nyr_native_fuzz_emit_binary(out, op, left, right, &dst);
      }
      if (ok)
        values[value_count++] = dst;
    }
  }
  if (ok && value_count > 0) {
    ok = nyr_native_fuzz_emit_noerr(
        out, (nyr_inst_t){.op = NYIR_RET,
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
    nyr_ir_func_free(out);
    return false;
  }
  return true;
}

bool nyr_oracle_fuzz(const ny_options *opt, int count,
                     const nyr_runtime_services_t *runtime_services, char *err,
                     size_t err_len) {
  if (count <= 0)
    return true;
  const nyr_target_request_t target_request =
      opt ? nyr_target_request_make(opt->native_backend, opt->native_abi,
                                    opt->host_triple, opt->opt_size)
          : nyr_target_request_make(NY_NATIVE_BACKEND_LLVM, NY_NATIVE_ABI_AUTO,
                                    NULL, false);
  ny_native_target_info_t target = {0};
  if (!nyr_target_info_init(&target, &target_request) ||
      target.target != NY_NATIVE_TARGET_X86_64) {
    nyr_set_error(err, err_len,
                  "native oracle fuzz: x86-64 native backend is required");
    return false;
  }
  bool ok = true;
  int failures = 0;
  for (int seed = 0; seed < count && failures < 3; ++seed) {
    nyr_native_fuzz_rng_t rng = {(uint64_t)(seed + 1)};
    nyr_func_t rt_main = {0};
    if (!nyr_native_fuzz_program(&rng, &rt_main, 3, 16)) {
      nyr_set_error(err, err_len,
                    "native oracle fuzz: failed to generate program %d", seed);
      ok = false;
      break;
    }
    char local_err[512] = {0};
    if (!nyr_native_result_oracle_for_nir(&rt_main, NULL, NULL, 0, opt,
                                          runtime_services, local_err,
                                          sizeof(local_err))) {
      fprintf(stderr, "native oracle fuzz seed=%d failed: %s\n", seed,
              local_err[0] ? local_err : NY_NATIVE_UNKNOWN_ERR);
      nyr_dump_compact(stderr, &rt_main, "<fuzz-failure>");
      ++failures;
      ok = false;
    }
    nyr_ir_func_free(&rt_main);
  }
  if (ok)
    fprintf(stderr, "native oracle fuzz: %d programs passed\n", count);
  if (!ok && err && err_len > 0 && err[0] == '\0')
    nyr_set_error(err, err_len, "native oracle fuzz: mismatch detected");
  return ok;
}
