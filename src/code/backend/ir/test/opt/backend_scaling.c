/*
 * Compile-only backend stress guard. Build a large, many-block NYIR function,
 * then measure machine lowering, allocation, and complete object emission as
 * separate stages. This intentionally bypasses source optimization so the
 * input size remains deterministic as optimizer heuristics evolve.
 */
#define _POSIX_C_SOURCE 200809L
#include "code/backend/internal.h"
#include "code/backend/ir/machine.h"
#include "support.h"

#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum { STRESS_BLOCKS = 2500 };

static double elapsed_ms(struct timespec start, struct timespec end) {
  return (double)(end.tv_sec - start.tv_sec) * 1000.0 +
         (double)(end.tv_nsec - start.tv_nsec) / 1000000.0;
}

static bool build_stress_ir(nyr_func_t *f) {
  nyr_opt_begin(f);
  int skip = nyr_opt_const(f, 0);
  for (int64_t block = 0; block < STRESS_BLOCKS; ++block) {
    nyr_opt_label(f, block);
    int left = nyr_opt_const(f, block + 1);
    int right = nyr_opt_const(f, 3);
    int sum = nyr_opt_binop(f, NYIR_ADD_I64, left, right);
    if (block + 2 < STRESS_BLOCKS)
      nyr_opt_br_if(f, skip, block + 2);
    if (block + 1 < STRESS_BLOCKS)
      nyr_opt_br(f, block + 1);
    else
      nyr_opt_ret(f, sum);
  }
  return nyr_opt_verify(f, "backend-scaling/input");
}

int main(void) {
  nyr_func_t ir;
  if (!build_stress_ir(&ir))
    return 1;

  ny_options options = {0};
  options.native_backend = NY_NATIVE_BACKEND_X86_64;
  const nyr_target_request_t target_request =
      nyr_target_request_make(options.native_backend, options.native_abi,
                              options.host_triple, options.opt_size);
  ny_native_target_info_t target = {0};
  if (!nyr_target_info_init(&target, &target_request)) {
    fprintf(stderr, "[backend-scaling] failed to initialize x86-64 target\n");
    nyr_ir_func_free(&ir);
    return 1;
  }

  char err[256] = {0};
  struct timespec lower_start, lower_end, alloc_start, alloc_end;
  struct timespec emit_start, emit_end;
  nyr_mach_func_t mach = {0};
  clock_gettime(CLOCK_MONOTONIC, &lower_start);
  bool lowered = nyr_mach_lower_nir(&ir, &mach, target.caps, err, sizeof(err));
  clock_gettime(CLOCK_MONOTONIC, &lower_end);
  if (!lowered || mach.inst_len < 10000 || mach.block_len < 2000) {
    fprintf(stderr,
            "[backend-scaling] lowering failed/too small: ok=%d insts=%zu "
            "blocks=%zu err=%s\n",
            lowered, mach.inst_len, mach.block_len, err);
    nyr_mach_func_free(&mach);
    nyr_ir_func_free(&ir);
    return 1;
  }

  nyr_mach_regalloc_t alloc = {0};
  clock_gettime(CLOCK_MONOTONIC, &alloc_start);
  bool allocated = nyr_mach_regalloc_build(&mach, 5, &alloc);
  clock_gettime(CLOCK_MONOTONIC, &alloc_end);
  if (!allocated) {
    fprintf(stderr, "[backend-scaling] register allocation failed\n");
    nyr_mach_regalloc_free(&alloc);
    nyr_mach_func_free(&mach);
    nyr_ir_func_free(&ir);
    return 1;
  }

  char object_path[128];
  snprintf(object_path, sizeof(object_path),
           "/tmp/nytrix-backend-scaling-%ld.o", (long)getpid());
  unlink(object_path);
  clock_gettime(CLOCK_MONOTONIC, &emit_start);
  bool emitted = nyr_native_emit_elf64_object_from_nir(
      &ir, &target, object_path, "ny_backend_scaling", false, err, sizeof(err));
  clock_gettime(CLOCK_MONOTONIC, &emit_end);
  struct stat object_stat = {0};
  bool object_ok = emitted && stat(object_path, &object_stat) == 0 &&
                   object_stat.st_size > 0;
  if (!object_ok)
    fprintf(stderr, "[backend-scaling] object emission failed: %s\n", err);
  unlink(object_path);

  printf("[backend-scaling] insts=%zu blocks=%zu vregs=%zu lower=%.2fms "
         "regalloc=%.2fms object=%.2fms bytes=%lld\n",
         mach.inst_len, mach.block_len, mach.vreg_len,
         elapsed_ms(lower_start, lower_end), elapsed_ms(alloc_start, alloc_end),
         elapsed_ms(emit_start, emit_end),
         object_ok ? (long long)object_stat.st_size : 0LL);

  nyr_mach_regalloc_free(&alloc);
  nyr_mach_func_free(&mach);
  nyr_ir_func_free(&ir);
  return object_ok ? 0 : 1;
}
