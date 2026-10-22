#if defined(__linux__) && defined(__x86_64__)

#include "base/util.h"
#include "code/backend/internal.h"
#include "code/backend/object/internal.h"
#include "support.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

typedef double (*vec4_f64_fn)(double *, double *, double *, double *);

static int emit(nyr_func_t *f, nyr_op_t op, int a, int b, int c, int64_t imm) {
  return nyr_ir_emit(f, (nyr_inst_t){.op = op,
                                     .dst = -1,
                                     .a = a,
                                     .b = b,
                                     .c = c,
                                     .d = -1,
                                     .e = -1,
                                     .f = -1,
                                     .imm = imm});
}

static int add_offset(nyr_func_t *f, int base, int64_t offset) {
  int displacement = nyr_opt_const(f, offset);
  return displacement < 0 ? -1
                          : emit(f, NYIR_ADD_I64, base, displacement, -1, 0);
}

static bool store_vector(nyr_func_t *f, int base, int64_t offset, int value) {
  int address = offset == 0 ? base : add_offset(f, base, offset);
  return address >= 0 &&
         emit(f, NYIR_VEC4_STORE_F64, address, value, -1, 0) >= 0;
}

static bool build_vectors(nyr_func_t *f) {
  memset(f, 0, sizeof(*f));
  f->param_count = 4;
  f->param_types = calloc(f->param_count, sizeof(*f->param_types));
  if (!f->param_types)
    return false;
  for (size_t i = 0; i < f->param_count; ++i)
    f->param_types[i] = NYIR_PARAM_I64;

  int a_ptr = emit(f, NYIR_LOAD_LOCAL, -1, -1, -1, 0);
  int b_ptr = emit(f, NYIR_LOAD_LOCAL, -1, -1, -1, 1);
  int c_ptr = emit(f, NYIR_LOAD_LOCAL, -1, -1, -1, 2);
  int out_ptr = emit(f, NYIR_LOAD_LOCAL, -1, -1, -1, 3);
  int a = emit(f, NYIR_VEC4_LOAD_F64, a_ptr, -1, -1, 0);
  int b = emit(f, NYIR_VEC4_LOAD_F64, b_ptr, -1, -1, 0);
  int c = emit(f, NYIR_VEC4_LOAD_F64, c_ptr, -1, -1, 0);
  int add = emit(f, NYIR_VEC4_ADD_F64, a, b, -1, 0);
  int sub = emit(f, NYIR_VEC4_SUB_F64, a, b, -1, 0);
  int mul = emit(f, NYIR_VEC4_MUL_F64, a, b, -1, 0);
  int div = emit(f, NYIR_VEC4_DIV_F64, a, b, -1, 0);
  int fma_result = emit(f, NYIR_VEC4_FMA_F64, a, b, c, 0);
  int shuffled = emit(f, NYIR_VEC4_SHUFFLE_F64, a, -1, -1, 0x1b);
  int scalar = nyr_opt_const_f64(f, 2.5);
  int splat = emit(f, NYIR_VEC4_SET1_F64, scalar, -1, -1, 0);
  int start = nyr_opt_const_f64(f, 0.5);
  int reduction = emit(f, NYIR_VEC4_REDUCE_ADD_F64, start, a, -1, 0);
  if (a_ptr < 0 || b_ptr < 0 || c_ptr < 0 || out_ptr < 0 || a < 0 || b < 0 ||
      c < 0 || add < 0 || sub < 0 || mul < 0 || div < 0 || fma_result < 0 ||
      shuffled < 0 || scalar < 0 || splat < 0 || start < 0 || reduction < 0 ||
      !store_vector(f, out_ptr, 0, add) || !store_vector(f, out_ptr, 32, sub) ||
      !store_vector(f, out_ptr, 64, mul) ||
      !store_vector(f, out_ptr, 96, div) ||
      !store_vector(f, out_ptr, 128, fma_result) ||
      !store_vector(f, out_ptr, 160, shuffled) ||
      !store_vector(f, out_ptr, 192, splat))
    return false;
  (void)nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_RET,
                                    .dst = -1,
                                    .a = reduction,
                                    .b = -1,
                                    .c = -1,
                                    .d = -1,
                                    .e = -1,
                                    .f = -1,
                                    .scalar_class = NYIR_SCALAR_F64});
  char err[256] = {0};
  bool verified = nyr_ir_verify(f, err, sizeof(err));
  if (!verified)
    fprintf(stderr, "NYIR verification: %s\n", err);
  return verified;
}

static bool is_vec4_f64_op(nyr_op_t op) {
  switch (op) {
  case NYIR_VEC4_LOAD_F64:
  case NYIR_VEC4_STORE_F64:
  case NYIR_VEC4_ADD_F64:
  case NYIR_VEC4_SUB_F64:
  case NYIR_VEC4_MUL_F64:
  case NYIR_VEC4_DIV_F64:
  case NYIR_VEC4_FMA_F64:
  case NYIR_VEC4_SET1_F64:
  case NYIR_VEC4_SHUFFLE_F64:
  case NYIR_VEC4_REDUCE_ADD_F64:
    return true;
  default:
    return false;
  }
}

static bool results_match(const double *a, const double *b, const double *c,
                          const double *out, double returned) {
  double sum = 0.5;
  for (unsigned lane = 0; lane < 4; ++lane)
    sum += a[lane];
  if (returned != sum) {
    uint64_t returned_bits, sum_bits;
    memcpy(&returned_bits, &returned, sizeof(returned_bits));
    memcpy(&sum_bits, &sum, sizeof(sum_bits));
    fprintf(stderr,
            "reduction result %.17g (0x%016llx), expected %.17g "
            "(0x%016llx); lanes:",
            returned, (unsigned long long)returned_bits, sum,
            (unsigned long long)sum_bits);
    for (unsigned lane = 0; lane < 4; ++lane)
      fprintf(stderr, " %.17g", out[lane]);
    fprintf(stderr, "\n");
    return false;
  }
  for (unsigned lane = 0; lane < 4; ++lane) {
    bool ok = out[lane] == a[lane] + b[lane] &&
              out[4 + lane] == a[lane] - b[lane] &&
              out[8 + lane] == a[lane] * b[lane] &&
              out[12 + lane] == a[lane] / b[lane] &&
              out[16 + lane] == fma(a[lane], b[lane], c[lane]) &&
              out[20 + lane] == a[3 - lane] && out[24 + lane] == 2.5;
    if (!ok) {
      fprintf(stderr,
              "lane %u add %.17g sub %.17g mul %.17g div %.17g fma %.17g "
              "shuffle %.17g splat %.17g\n",
              lane, out[lane], out[4 + lane], out[8 + lane], out[12 + lane],
              out[16 + lane], out[20 + lane], out[24 + lane]);
      return false;
    }
  }
  return true;
}

static bool write_all(int fd, const char *data, size_t len) {
  while (len) {
    ssize_t n = write(fd, data, len);
    if (n <= 0)
      return false;
    data += (size_t)n;
    len -= (size_t)n;
  }
  return true;
}

static bool assemble(const char *asm_path, const char *so_path) {
  pid_t child = fork();
  if (child < 0)
    return false;
  if (child == 0) {
    execlp("cc", "cc", "-shared", "-fPIC", "-o", so_path, asm_path, "-lm",
           (char *)NULL);
    _exit(127);
  }
  int status = 0;
  return waitpid(child, &status, 0) == child && WIFEXITED(status) &&
         WEXITSTATUS(status) == 0;
}

/*
 * Lower the four-lane NYIR through the target-independent machine form,
 * encode it with the x86-64 machine bundle, and execute it in-place.
 */
static bool check_x64_machine_form(const nyr_func_t *f,
                                   const ny_native_target_info_t *target,
                                   const double *a, const double *b,
                                   const double *c) {
  nyr_mach_func_t machine = {0};
  nyr_obj_buf_t code = {0};
  nyr_x64_obj_symbol_def_t defs[4] = {0};
  nyr_x64_obj_reloc_t relocs[4] = {0};
  size_t def_count = 0, reloc_count = 0;
  char err[256] = {0};
  bool ok = nyr_mach_lower_nir(f, &machine, target->caps, err, sizeof(err));
  if (!ok) {
    fprintf(stderr, "machine form: four-lane f64 lower failed: %s\n", err);
    goto done;
  }
  ok = nyr_x64_mach_append_function(
      &code, defs, &def_count, relocs, &reloc_count, &machine, target,
      "ny_test_vec4_f64_mach", false, err, sizeof(err));
  if (!ok) {
    fprintf(stderr, "machine form: bundle encode failed: %s\n", err);
    goto done;
  }
  {
    int symbol =
        nyr_x64_obj_def_index(defs, def_count, "ny_test_vec4_f64_mach");
    ok = symbol >= 0;
    bool has_ymm = false;
    for (size_t i = 0; ok && i + 1 < code.len; ++i)
      if (code.data[i] == 0xc4 && (code.data[i + 1] & 0x0f) <= 2)
        has_ymm = true;
    if (ok && !has_ymm) {
      fprintf(stderr, "machine form: no VEX.256 ymm encoding found\n");
      ok = false;
    }
    void *mapping = MAP_FAILED;
    if (ok) {
      mapping = nyr_opt_execmem_alloc(code.len);
      ok = mapping != MAP_FAILED;
    }
    if (ok) {
      memcpy(mapping, code.data, code.len);
      ok = nyr_opt_execmem_protect(mapping, code.len) == 0;
    }
    if (ok) {
      vec4_f64_fn run =
          (vec4_f64_fn)((unsigned char *)mapping + defs[symbol].off);
      double out[28] = {0};
      double returned = run((double *)a, (double *)b, (double *)c, out);
      ok = results_match(a, b, c, out, returned);
      if (!ok) {
        const char *dump_path = "build/cache/scratch/native-vec4-f64-mach.bin";
        int dump = open(dump_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (dump >= 0) {
          (void)write_all(dump, (const char *)code.data, code.len);
          close(dump);
          fprintf(stderr, "machine-form bytes saved to %s\n", dump_path);
        }
      }
    }
    if (mapping != MAP_FAILED)
      nyr_opt_execmem_free(mapping, code.len);
  }
done:
  nyr_obj_free(&code);
  nyr_mach_func_free(&machine);
  return ok;
}

static uint32_t read_word(const unsigned char *data, size_t off) {
  uint32_t word;
  memcpy(&word, data + off, sizeof(word));
  return word;
}

static bool file_has_words(const char *path, const uint32_t *words,
                           size_t count) {
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return false;
  size_t cap = 1 << 16;
  size_t len = 0;
  bool ok = true;
  unsigned char *data = malloc(cap);
  if (!data) {
    close(fd);
    return false;
  }
  for (;;) {
    if (len == cap) {
      size_t next = cap * 2;
      unsigned char *grown = realloc(data, next);
      if (!grown) {
        ok = false;
        break;
      }
      data = grown;
      cap = next;
    }
    ssize_t got = read(fd, data + len, cap - len);
    if (got < 0) {
      ok = false;
      break;
    }
    if (got == 0)
      break;
    len += (size_t)got;
  }
  close(fd);
  if (ok) {
    for (size_t w = 0; ok && w < count; ++w) {
      bool found = false;
      for (size_t i = 0; i + 4 <= len; i += 4)
        if (read_word(data, i) == words[w]) {
          found = true;
          break;
        }
      if (!found)
        ok = false;
    }
  }
  free(data);
  return ok;
}

/*
 * Every four-lane f64 opcode must now emit its paired two-lane NEON form
 * instead of failing the object write. The signature words below are the
 * exact lane instructions the direct AArch64 encoder emits per op.
 */
static bool check_aarch64_emission(nyr_func_t *f,
                                   const ny_native_target_info_t *target,
                                   const char *obj_path) {
  static const nyr_op_t ops[] = {
      NYIR_VEC4_LOAD_F64,       NYIR_VEC4_STORE_F64, NYIR_VEC4_ADD_F64,
      NYIR_VEC4_SUB_F64,        NYIR_VEC4_MUL_F64,   NYIR_VEC4_DIV_F64,
      NYIR_VEC4_FMA_F64,        NYIR_VEC4_SET1_F64,  NYIR_VEC4_SHUFFLE_F64,
      NYIR_VEC4_REDUCE_ADD_F64,
  };
  static const uint32_t load_words[] = {0x3dc00000u, 0x3dc00401u};
  static const uint32_t store_words[] = {0x3d800000u, 0x3d800401u};
  static const uint32_t add_words[] = {0x4e62d400u, 0x4e63d421u};
  static const uint32_t sub_words[] = {0x4ee2d400u, 0x4ee3d421u};
  static const uint32_t mul_words[] = {0x6e62dc00u, 0x6e63dc21u};
  static const uint32_t div_words[] = {0x6e62fc00u, 0x6e63fc21u};
  static const uint32_t fma_words[] = {0x4e64cc40u, 0x4e65cc61u};
  static const uint32_t set1_words[] = {0x4e080400u, 0x4ea01c01u};
  static const uint32_t shuffle_words[] = {0x4e034020u, 0x4e044025u};
  static const uint32_t reduce_words[] = {0x7e70d802u, 0x7e70d823u,
                                          0x1e632842u};
  static const uint32_t *word_sets[] = {
      load_words, store_words, add_words,  sub_words,     mul_words,
      div_words,  fma_words,   set1_words, shuffle_words, reduce_words,
  };
  static const size_t word_counts[] = {2, 2, 2, 2, 2, 2, 2, 2, 2, 3};
  bool ok = true;
  for (size_t op_i = 0; ok && op_i < sizeof(ops) / sizeof(ops[0]); ++op_i) {
    nyr_func_t candidate = *f;
    candidate.data = malloc(f->len * sizeof(*candidate.data));
    if (!candidate.data)
      return false;
    memcpy(candidate.data, f->data, f->len * sizeof(*candidate.data));
    bool found = false;
    for (size_t i = 0; i < candidate.len; ++i) {
      if (!is_vec4_f64_op(candidate.data[i].op))
        continue;
      if (candidate.data[i].op == ops[op_i])
        found = true;
      else
        candidate.data[i].op = NYIR_NOP;
    }
    char err[512] = {0};
    ok = found && nyr_native_emit_elf64_aarch64_object_from_nirs(
                      &candidate, NULL, NULL, 0, target, obj_path,
                      "ny_test_vec4_f64_aarch64", false, err, sizeof(err));
    if (!ok) {
      fprintf(stderr, "AArch64 four-lane emission failed for %s: %s\n",
              nyr_op_name(ops[op_i]), err[0] ? err : "no detail");
    } else if (!file_has_words(obj_path, word_sets[op_i], word_counts[op_i])) {
      char keep[256];
      snprintf(keep, sizeof(keep), "build/cache/scratch/vec4-a64-missing-%s.o",
               nyr_op_name(ops[op_i]));
      (void)rename(obj_path, keep);
      fprintf(stderr, "AArch64 four-lane encoding missing for %s (kept %s)\n",
              nyr_op_name(ops[op_i]), keep);
      ok = false;
    }
    free(candidate.data);
    unlink(obj_path);
  }
  return ok;
}

int main(void) {
  char temp[] = "build/cache/scratch/native-vec4-f64-XXXXXX";
  char asm_path[256], so_path[256], aarch64_obj_path[256];
  ny_ensure_dir_recursive("build/cache/scratch");
  if (!mkdtemp(temp))
    return 1;
  snprintf(asm_path, sizeof(asm_path), "%s/vec4.s", temp);
  snprintf(so_path, sizeof(so_path), "%s/vec4.so", temp);
  snprintf(aarch64_obj_path, sizeof(aarch64_obj_path), "%s/vec4-aarch64.o",
           temp);

  ny_options opt;
  ny_options_init(&opt);
  opt.native_backend = NY_NATIVE_BACKEND_X86_64;
  const nyr_target_request_t target_request = nyr_target_request_make(
      opt.native_backend, opt.native_abi, opt.host_triple, opt.opt_size);
  ny_native_target_info_t target;
  nyr_func_t f = {0};
  nyr_writer_t writer = {0};
  char err[512] = {0};
  bool target_ok = nyr_target_info_init(&target, &target_request);
  bool ir_ok = target_ok && build_vectors(&f);

  double a[4] = {0x1.0000002p0, 1.25, -3.5, 0x1.fffffffffffffp0};
  double b[4] = {0x1.ffffffcp-1, -2.5, 0.125, 1.0000000000000002};
  double c[4] = {-1.0, 0.75, 6.0, -2.0};

  /*
   * Route one: target-independent machine form on x86-64. It must lower,
   * encode, and execute with the exact four-lane semantics.
   */
  bool machine_ok = ir_ok && check_x64_machine_form(&f, &target, a, b, c);
  if (!machine_ok)
    fprintf(stderr, "machine-form four-lane f64 route failed\n");

  /*
   * Route two: the x86-64 NYIR emitter, which must keep the two-lane
   * arithmetic packed while preserving the four-lane observable result.
   */
  bool emit_ok =
      ir_ok && nyr_emit_func(&writer, &target, &f, "ny_test_vec4_f64", false,
                             err, sizeof(err));
  bool packed_ok = emit_ok && writer.data && strstr(writer.data, "movupd") &&
                   strstr(writer.data, "addpd") &&
                   strstr(writer.data, "mulpd") && strstr(writer.data, "divpd");
  if (emit_ok && !packed_ok)
    fprintf(stderr,
            "x86-64 NYIR emitter did not preserve packed f64 operations\n");
  int fd = emit_ok ? open(asm_path, O_WRONLY | O_CREAT | O_TRUNC, 0600) : -1;
  bool written = fd >= 0 && write_all(fd, writer.data, writer.len);
  if (fd >= 0)
    close(fd);
  bool assembled = written && assemble(asm_path, so_path);
  if (emit_ok && !assembled)
    fprintf(stderr, "assembler/link stage failed\n");

  void *handle = assembled ? dlopen(so_path, RTLD_NOW | RTLD_LOCAL) : NULL;
  if (assembled && !handle)
    fprintf(stderr, "dlopen: %s\n", dlerror());
  vec4_f64_fn run = NULL;
  if (handle) {
    void *symbol = dlsym(handle, "ny_test_vec4_f64");
    if (!symbol)
      fprintf(stderr, "dlsym: %s\n", dlerror());
    memcpy(&run, &symbol, sizeof(run));
  }
  bool emitter_ok = false;
  if (run) {
    double out[28] = {0};
    double returned = run(a, b, c, out);
    emitter_ok = results_match(a, b, c, out, returned);
  }

  /*
   * Route three: AArch64 object emission carries all ten four-lane f64
   * opcodes as paired two-lane NEON sequences.
   */
  ny_options aarch64_opt;
  ny_options_init(&aarch64_opt);
  aarch64_opt.native_backend = NY_NATIVE_BACKEND_AARCH64;
  const nyr_target_request_t aarch64_target_request = nyr_target_request_make(
      aarch64_opt.native_backend, aarch64_opt.native_abi,
      aarch64_opt.host_triple, aarch64_opt.opt_size);
  ny_native_target_info_t aarch64_target;
  bool aarch64_target_ok =
      nyr_target_info_init(&aarch64_target, &aarch64_target_request);
  bool aarch64_ok =
      ir_ok && aarch64_target_ok &&
      check_aarch64_emission(&f, &aarch64_target, aarch64_obj_path);
  if (!aarch64_ok)
    fprintf(stderr, "AArch64 four-lane f64 emission failed\n");

  bool ok = machine_ok && packed_ok && assembled && emitter_ok && aarch64_ok;
  if (!ok)
    fprintf(stderr, "native four-lane f64 failed\n");
  else
    puts("[native-vec4-f64] machine-form, x86-64 emitter, and AArch64 "
         "four-lane arithmetic, memory, shuffle, reduction, splat, and fused "
         "FMA pass");

  if (handle)
    dlclose(handle);
  unlink(asm_path);
  unlink(so_path);
  unlink(aarch64_obj_path);
  rmdir(temp);
  free(writer.data);
  nyr_ir_func_free(&f);
  ny_options_free(&opt);
  return ok ? 0 : 1;
}

#else

#include <stdio.h>

int main(void) {
  puts("[native-vec4-f64] skipped outside Linux x86-64");
  return 0;
}

#endif
