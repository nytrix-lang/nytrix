/*
 * Compile-only stress guards for large single- and multi-block functions.
 */
#define _POSIX_C_SOURCE 200809L
#include "code/backend/internal.h"
#include "code/backend/ir/machine.h"
#include "support.h"

#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum { STRESS_VALUES = 5000 };
enum { CFG_STRESS_BLOCKS = 2500, CFG_STRESS_PAIRS_PER_BLOCK = 2 };
enum { LOWER_BUDGET_MS = 500 };
enum { REGALLOC_BUDGET_MS = 1500 };
enum { EMIT_BUDGET_MS = 1500 };

static double elapsed_ms(struct timespec a, struct timespec b) {
  return (double)(b.tv_sec - a.tv_sec) * 1000.0 +
         (double)(b.tv_nsec - a.tv_nsec) / 1000000.0;
}

static bool build_monolithic_ir(nyr_func_t *f) {
  nyr_opt_begin(f);
  int *values = malloc(sizeof(*values) * STRESS_VALUES);
  if (!values)
    return false;
  for (int i = 0; i < STRESS_VALUES; ++i)
    values[i] = nyr_opt_const(f, i + 1);
  int sum = values[0];
  for (int i = 1; i < STRESS_VALUES; ++i)
    sum = nyr_opt_binop(f, NYIR_ADD_I64, sum, values[i]);
  nyr_opt_ret(f, sum);
  free(values);
  return f->len >= 10000 &&
         nyr_opt_verify(f, "backend-monolithic-scaling/input");
}

static bool build_many_block_ir(nyr_func_t *f) {
  nyr_opt_begin(f);
  int skip = nyr_opt_const(f, 0);
  int sum = -1;
  for (int block = 0; block < CFG_STRESS_BLOCKS; ++block) {
    nyr_opt_label(f, 5000 + block);
    sum = nyr_opt_const(f, 0);
    for (int pair = 0; pair < CFG_STRESS_PAIRS_PER_BLOCK; ++pair) {
      int value =
          nyr_opt_const(f, (int64_t)block * CFG_STRESS_PAIRS_PER_BLOCK + pair);
      sum = nyr_opt_binop(f, NYIR_ADD_I64, sum, value);
    }
    if (block + 2 < CFG_STRESS_BLOCKS)
      nyr_opt_br_if(f, skip, 5000 + block + 2);
    if (block + 1 < CFG_STRESS_BLOCKS)
      nyr_opt_br(f, 5000 + block + 1);
  }
  nyr_opt_ret(f, sum);
  return f->len >= 10000 &&
         nyr_opt_verify(f, "backend-many-block-scaling/input");
}

int main(void) {
  nyr_func_t ir;
  if (!build_monolithic_ir(&ir))
    return 1;

  ny_options options = {0};
  options.native_backend = NY_NATIVE_BACKEND_X86_64;
  const nyr_target_request_t target_request =
      nyr_target_request_make(options.native_backend, options.native_abi,
                              options.host_triple, options.opt_size);
  ny_native_target_info_t target = {0};
  if (!nyr_target_info_init(&target, &target_request)) {
    fprintf(stderr, "[backend-monolithic-scaling] target init failed\n");
    nyr_ir_func_free(&ir);
    return 1;
  }

  char err[256] = {0};
  nyr_mach_func_t mach = {0};
  nyr_mach_regalloc_t alloc = {0};
  struct timespec start, end, alloc_start, alloc_end, emit_start, emit_end;
  clock_gettime(CLOCK_MONOTONIC, &start);
  bool lowered = nyr_mach_lower_nir(&ir, &mach, target.caps, err, sizeof(err));
  clock_gettime(CLOCK_MONOTONIC, &end);
  double lower_ms = elapsed_ms(start, end);
  if (!lowered || mach.inst_len < 10000 || mach.block_len != 1 ||
      lower_ms > LOWER_BUDGET_MS) {
    fprintf(stderr,
            "[backend-monolithic-scaling] lowering failed/shape changed: "
            "ok=%d insts=%zu blocks=%zu lower=%.2fms budget=%dms err=%s\n",
            lowered, mach.inst_len, mach.block_len, lower_ms, LOWER_BUDGET_MS,
            err);
    nyr_mach_func_free(&mach);
    nyr_ir_func_free(&ir);
    return 1;
  }

  clock_gettime(CLOCK_MONOTONIC, &alloc_start);
  bool allocated = nyr_mach_regalloc_build(&mach, 5, &alloc);
  clock_gettime(CLOCK_MONOTONIC, &alloc_end);
  double regalloc_ms = elapsed_ms(alloc_start, alloc_end);
  if (!allocated || regalloc_ms > REGALLOC_BUDGET_MS) {
    fprintf(stderr,
            "[backend-monolithic-scaling] register allocation %s: %.2fms "
            "(budget %dms)\n",
            allocated ? "exceeded scaling budget" : "failed", regalloc_ms,
            REGALLOC_BUDGET_MS);
    nyr_mach_regalloc_free(&alloc);
    nyr_mach_func_free(&mach);
    nyr_ir_func_free(&ir);
    return 1;
  }

  char object_path[128];
  snprintf(object_path, sizeof(object_path),
           "/tmp/nytrix-backend-monolithic-%ld.o", (long)getpid());
  unlink(object_path);
  clock_gettime(CLOCK_MONOTONIC, &emit_start);
  bool emitted = nyr_native_emit_elf64_object_from_nir(
      &ir, &target, object_path, "ny_backend_monolithic_scaling", false, err,
      sizeof(err));
  clock_gettime(CLOCK_MONOTONIC, &emit_end);
  struct stat object_stat = {0};
  double emit_ms = elapsed_ms(emit_start, emit_end);
  bool object_ok = emitted && stat(object_path, &object_stat) == 0 &&
                   object_stat.st_size > 0 && emit_ms <= EMIT_BUDGET_MS;
  if (!object_ok)
    fprintf(stderr,
            "[backend-monolithic-scaling] emission %s: %.2fms budget=%dms "
            "err=%s\n",
            emitted ? "exceeded scaling budget or produced no object"
                    : "failed",
            emit_ms, EMIT_BUDGET_MS, err);
  unlink(object_path);

  printf("[backend-monolithic-scaling] nyir-insts=%zu machine-insts=%zu "
         "blocks=%zu vregs=%zu lower=%.2fms regalloc=%.2fms object=%.2fms "
         "bytes=%lld\n",
         ir.len, mach.inst_len, mach.block_len, mach.vreg_len, lower_ms,
         regalloc_ms, emit_ms,
         object_ok ? (long long)object_stat.st_size : 0LL);

  nyr_mach_regalloc_free(&alloc);
  nyr_mach_func_free(&mach);
  nyr_ir_func_free(&ir);
  if (!object_ok)
    return 1;

  nyr_func_t cfg_ir;
  if (!build_many_block_ir(&cfg_ir))
    return 1;
  memset(err, 0, sizeof(err));
  memset(&mach, 0, sizeof(mach));
  memset(&alloc, 0, sizeof(alloc));
  clock_gettime(CLOCK_MONOTONIC, &start);
  lowered = nyr_mach_lower_nir(&cfg_ir, &mach, target.caps, err, sizeof(err));
  clock_gettime(CLOCK_MONOTONIC, &end);
  lower_ms = elapsed_ms(start, end);
  if (!lowered || mach.inst_len < 10000 || mach.block_len < 2000 ||
      lower_ms > LOWER_BUDGET_MS) {
    fprintf(stderr,
            "[backend-many-block-scaling] lowering failed/shape changed: "
            "ok=%d insts=%zu blocks=%zu lower=%.2fms budget=%dms err=%s\n",
            lowered, mach.inst_len, mach.block_len, lower_ms, LOWER_BUDGET_MS,
            err);
    nyr_mach_func_free(&mach);
    nyr_ir_func_free(&cfg_ir);
    return 1;
  }
  clock_gettime(CLOCK_MONOTONIC, &alloc_start);
  allocated = nyr_mach_regalloc_build(&mach, 5, &alloc);
  clock_gettime(CLOCK_MONOTONIC, &alloc_end);
  regalloc_ms = elapsed_ms(alloc_start, alloc_end);
  if (!allocated || regalloc_ms > REGALLOC_BUDGET_MS) {
    fprintf(stderr,
            "[backend-many-block-scaling] register allocation %s: %.2fms "
            "(budget %dms)\n",
            allocated ? "exceeded scaling budget" : "failed", regalloc_ms,
            REGALLOC_BUDGET_MS);
    nyr_mach_regalloc_free(&alloc);
    nyr_mach_func_free(&mach);
    nyr_ir_func_free(&cfg_ir);
    return 1;
  }
  snprintf(object_path, sizeof(object_path),
           "/tmp/nytrix-backend-many-block-%ld.o", (long)getpid());
  unlink(object_path);
  clock_gettime(CLOCK_MONOTONIC, &emit_start);
  emitted = nyr_native_emit_elf64_object_from_nir(
      &cfg_ir, &target, object_path, "ny_backend_many_block_scaling", false,
      err, sizeof(err));
  clock_gettime(CLOCK_MONOTONIC, &emit_end);
  memset(&object_stat, 0, sizeof(object_stat));
  emit_ms = elapsed_ms(emit_start, emit_end);
  object_ok = emitted && stat(object_path, &object_stat) == 0 &&
              object_stat.st_size > 0 && emit_ms <= EMIT_BUDGET_MS;
  if (!object_ok)
    fprintf(stderr,
            "[backend-many-block-scaling] emission %s: %.2fms budget=%dms "
            "err=%s\n",
            emitted ? "exceeded scaling budget or produced no object"
                    : "failed",
            emit_ms, EMIT_BUDGET_MS, err);
  unlink(object_path);
  printf("[backend-many-block-scaling] nyir-insts=%zu machine-insts=%zu "
         "blocks=%zu vregs=%zu lower=%.2fms regalloc=%.2fms object=%.2fms "
         "bytes=%lld\n",
         cfg_ir.len, mach.inst_len, mach.block_len, mach.vreg_len, lower_ms,
         regalloc_ms, emit_ms,
         object_ok ? (long long)object_stat.st_size : 0LL);
  nyr_mach_regalloc_free(&alloc);
  nyr_mach_func_free(&mach);
  nyr_ir_func_free(&cfg_ir);
  return object_ok ? 0 : 1;
}
