/*
 * Target initialization owns OS syscall service-number selection.
 */
#include "code/backend/core/target.h"

#include <stdio.h>

static int check_adapter(nyr_backend_t backend, nyr_abi_t abi,
                         const char *triple, uint32_t write_nr,
                         uint32_t mmap_nr, uint32_t munmap_nr,
                         uint32_t clock_gettime_nr, uint32_t exit_nr) {
  nyr_target_request_t request =
      nyr_target_request_make(backend, abi, triple, false);
  ny_native_target_info_t target = {0};
  if (!nyr_target_info_init(&target, &request) ||
      target.syscalls.available != (write_nr != 0) ||
      target.syscalls.write_nr != write_nr ||
      target.syscalls.mmap_nr != mmap_nr ||
      target.syscalls.munmap_nr != munmap_nr ||
      target.syscalls.clock_gettime_nr != clock_gettime_nr ||
      target.syscalls.exit_nr != exit_nr) {
    fprintf(stderr, "[target-syscall-adapter] wrong services for %s\n",
            triple);
    return 0;
  }
  return 1;
}

int main(void) {
  if (!check_adapter(NY_NATIVE_BACKEND_X86_64, NY_NATIVE_ABI_SYSV,
                     "x86_64-unknown-linux-gnu", 1, 9, 11, 228, 60) ||
      !check_adapter(NY_NATIVE_BACKEND_AARCH64, NY_NATIVE_ABI_AAPCS,
                     "aarch64-unknown-linux-gnu", 64, 222, 215, 113, 93) ||
      !check_adapter(NY_NATIVE_BACKEND_X86_64, NY_NATIVE_ABI_SYSV,
                     "x86_64-apple-darwin", 0, 0, 0, 0, 0))
    return 1;
  puts("[target-syscall-adapter] Linux x86-64/AArch64 services resolved; "
       "unsupported OS remains unavailable");
  return 0;
}
