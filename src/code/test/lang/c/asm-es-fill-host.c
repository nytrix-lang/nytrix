typedef unsigned int u32;
typedef unsigned short u16;
extern volatile unsigned char es_memory[256];
extern int exercise_es_fill(u16 segment);

struct descriptor {
  u32 entry;
  u32 base;
  u32 limit;
  u32 flags;
};

__attribute__((force_align_arg_pointer)) void _start(void) {
  struct descriptor descriptor = {0xffffffffu, (u32)(__UINTPTR_TYPE__)es_memory,
                                  0xfffffu, 0x51};
  int result;
  __asm__ volatile("int $0x80"
                   : "=a"(result)
                   : "0"(243), "b"(&descriptor)
                   : "memory");
  if (!result)
    result = exercise_es_fill((u16)((descriptor.entry << 3) | 3));
  __asm__ volatile("int $0x80" : : "a"(1), "b"(result) : "memory");
  __builtin_unreachable();
}
