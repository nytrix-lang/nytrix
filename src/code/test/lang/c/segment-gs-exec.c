/*
 * Runs %gs-relative loads, stores, increments and subscripts against a block installed with arch_prctl and prints the results.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#define ARCH_SET_GS 0x1001
extern long syscall(long number, ...);

static long block[32];

__attribute__((noinline)) long read_long(unsigned long off) {
  return *(__seg_gs long *)off;
}
__attribute__((noinline)) int read_int(unsigned long off) {
  return *(__seg_gs int *)off;
}
__attribute__((noinline)) unsigned short read_u16(unsigned long off) {
  return *(__seg_gs unsigned short *)off;
}
__attribute__((noinline)) unsigned char read_u8(unsigned long off) {
  return *(__seg_gs unsigned char *)off;
}
__attribute__((noinline)) void write_int(unsigned long off, int v) {
  *(__seg_gs int *)off = v;
}
__attribute__((noinline)) void bump(unsigned long off) {
  (*(__seg_gs long *)off)++;
  *(__seg_gs long *)off += 40;
}
__attribute__((noinline)) int indexed(unsigned long base, int i) {
  __seg_gs int *p = (__seg_gs int *)base;
  p[i] = p[i - 1] + 100;
  return p[i];
}

int main(void) {
  for (int i = 0; i < 32; ++i)
    block[i] = 0x0102030405060708L * (i + 1);
  if (syscall(SYS_arch_prctl, ARCH_SET_GS, (unsigned long)block) != 0) {
    puts("arch_prctl failed");
    return 1;
  }
  printf("%lx %x %x %x\n", read_long(8), read_int(16), read_u16(24), read_u8(33));
  write_int(48, -7);
  bump(64);
  int first = read_int(48);
  printf("%d %lx\n", first, read_long(64));
  int stored = indexed(96, 3);
  printf("%d %d\n", stored, read_int(96 + 12));
  printf("%lx\n", block[6]);
  return 0;
}
