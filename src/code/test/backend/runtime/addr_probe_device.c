/*
 * Regression test for the runtime's readability probe on device memory.
 *
 * A Vulkan buffer mapped from /dev/dri is VM_IO/VM_PFNMAP memory: the
 * program reads and writes it directly, but process_vm_readv refuses it with
 * EFAULT. The probe took that EFAULT as "unreadable", so raw stores into
 * mapped vertex buffers (store32 and friends) were silently dropped. Every
 * Linux process has such a mapping: the first page of [vvar] is readable and
 * refused by process_vm_readv. The probe must accept it, and still reject
 * PROT_NONE pages and unmapped pages. (The pages after it vary by kernel, so
 * only the first is asserted.)
 */
#include "code/backend/runtime/shared.h"
#include <stdio.h>
#include <string.h>
#ifdef __linux__
#include <sys/mman.h>
#endif

static int expect(const char *what, uintptr_t p, int want) {
  int got = rt_addr_probe_os(p, 1);
  if (got != want) {
    fprintf(stderr, "addr-probe-device: %s at %p: got %d, want %d\n", what,
            (void *)p, got, want);
    return 1;
  }
  return 0;
}

int main(void) {
#ifdef __linux__
  int failed = 0;
  uintptr_t vvar = 0, vvar_end = 0;
  FILE *maps = fopen("/proc/self/maps", "r");
  char line[512];
  while (maps && fgets(line, sizeof(line), maps)) {
    if (strstr(line, "[vvar]")) {
      unsigned long a = 0, b = 0;
      if (sscanf(line, "%lx-%lx", &a, &b) == 2) {
        vvar = (uintptr_t)a;
        vvar_end = (uintptr_t)b;
      }
      break;
    }
  }
  if (maps)
    fclose(maps);
  if (vvar && vvar_end - vvar >= 2 * 4096u) {
    failed |= expect("[vvar] data page (VM_IO, readable)", vvar, 1);
  } else {
    fprintf(stderr, "addr-probe-device: no [vvar] mapping; device case skipped\n");
  }

  unsigned char *rw = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  unsigned char *none =
      mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  unsigned char *gone = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (rw == MAP_FAILED || none == MAP_FAILED || gone == MAP_FAILED)
    return 1;
  rw[0] = 1;
  munmap(gone, 4096);
  failed |= expect("ordinary page", (uintptr_t)rw, 1);
  failed |= expect("PROT_NONE page", (uintptr_t)none, 0);
  failed |= expect("unmapped page", (uintptr_t)gone, 0);
  munmap(rw, 4096);
  munmap(none, 4096);
  if (!failed)
    puts("addr-probe-device: ok");
  return failed;
#else
  return 0;
#endif
}
