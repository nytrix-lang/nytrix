#include <stdint.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

extern int protected_buffer(int);
extern int unprotected_buffer(int);
extern int protected_scalar(void);
uintptr_t __stack_chk_guard = UINT64_C(0x39192736abcdef00);
uintptr_t custom_guard = UINT64_C(0x6ef427185a903200);
#if GUARD_MODE == 2
static uintptr_t gs_storage[16];
#endif
/*
 * A linker-resolved TLS/per-CPU offset, not a normal process address.
 */
__asm__(".globl __nyc_guard_offset\n.set __nyc_guard_offset,40");

__attribute__((noreturn)) void __stack_chk_fail(void) { _exit(93); }
void change_guard(void) {
#if GUARD_MODE == 3
  custom_guard ^= UINT64_C(0x100);
#else
  __stack_chk_guard ^= UINT64_C(0x100);
#endif
}

void consume(char *buffer, int corrupt) {
  if (corrupt != 1)
    return;
  uintptr_t guard;
#if GUARD_MODE == 0
  guard = __stack_chk_guard;
#elif GUARD_MODE == 1
  __asm__ volatile("movq %%fs:40,%0" : "=r"(guard));
#elif GUARD_MODE == 2
  __asm__ volatile("movq %%gs:%c1,%0" : "=r"(guard) : "i"(GUARD_OFFSET));
#else
  guard = custom_guard;
#endif
  /*
   * Deliberately corrupt the caller's stack, including any stale spills of
   * the cookie. The test must reach __stack_chk_fail, not merely find its
   * symbol in disassembly. main reserves mapped space above this frame.
   */
  for (unsigned i = 0; i < 512; ++i) {
    uintptr_t word;
    memcpy(&word, buffer + i, sizeof(word));
    if (word == guard)
      buffer[i] ^= 1;
  }
}

int main(int argc, char **argv) {
  volatile unsigned char mapped_stack[2048];
  for (unsigned i = 0; i < sizeof(mapped_stack); ++i)
    mapped_stack[i] = 0;
#if GUARD_MODE == 2
  gs_storage[GUARD_OFFSET / sizeof(uintptr_t)] = __stack_chk_guard;
  if (syscall(SYS_arch_prctl, 0x1001, gs_storage))
    return 4;
#endif
  if (argc > 1) {
    if (argv[1][0] == 's')
      protected_scalar();
    else
      protected_buffer(1);
    return 2; /* A protected corruption must not return here. */
  }
  if (protected_buffer(0) != 12 || protected_buffer(3) != 11)
    return 5;
  /*
   * Attribute opt-out must work even under -fstack-protector-all.
   */
  if (unprotected_buffer(1) != 12)
    return 6;
  return mapped_stack[0];
}
