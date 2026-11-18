/*
 * C front-end regression test: asm segment memory host32.
 */
typedef unsigned int u32;
typedef unsigned short u16;

extern volatile u32 fs_memory[4], gs_memory[4];
extern int configure_segments(u16 fs_selector, u16 gs_selector);
extern int exercise_segments(void);

struct descriptor {
  u32 entry;
  u32 base;
  u32 limit;
  u32 flags;
};

static int install(struct descriptor *descriptor) {
  int result;
  __asm__ volatile("int $0x80"
                   : "=a"(result)
                   : "0"(243), "b"(descriptor)
                   : "memory");
  return result;
}

__attribute__((force_align_arg_pointer)) void _start(void) {
  struct descriptor fs = {0xffffffffu, (u32)(__UINTPTR_TYPE__)fs_memory,
                          0xfffffu, 0x51};
  struct descriptor gs = {0xffffffffu, (u32)(__UINTPTR_TYPE__)gs_memory,
                          0xfffffu, 0x51};
  int result = 90;
  if (!install(&fs) && !install(&gs)) {
    result = configure_segments((u16)((fs.entry << 3) | 3),
                                (u16)((gs.entry << 3) | 3));
    if (!result)
      result = exercise_segments();
  }
  __asm__ volatile("int $0x80" : : "a"(1), "b"(result) : "memory");
  __builtin_unreachable();
}
