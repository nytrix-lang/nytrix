/*
 * C front-end regression test: asm es fill.
 */
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

volatile u8 es_memory[256] __attribute__((aligned(16)));

__attribute__((noinline)) u32 fill_screen(u32 destination, int count,
                                          u16 segment, u32 value,
                                          u32 *remaining) {
  __asm__ volatile("pushw %%es ; movw %2,%%es ; shrw %%cx ; jnc 1f ; "
                   "stosw\n\t1: rep stosl ; popw %%es"
                   : "+D"(destination), "+c"(count)
                   : "bdS"(segment), "a"(value));
  *remaining = (u32)count;
  return destination;
}

int exercise_es_fill(u16 segment) {
  for (int count = 0; count < 18; ++count) {
    for (u32 i = 0; i < sizeof(es_memory); ++i)
      es_memory[i] = 0xa5;
    u16 before, after;
    u32 remaining = 0xffffffffu;
    __asm__ volatile("movw %%es,%0" : "=rm"(before));
    u32 end = fill_screen(3, count, segment, 0x5a3c5a3cu, &remaining);
    __asm__ volatile("movw %%es,%0" : "=rm"(after));
    if (before != after || remaining || end != 3 + (u32)count * 2)
      return 1;
    for (u32 i = 0; i < sizeof(es_memory); ++i) {
      u8 expected = i >= 3 && i < 3 + (u32)count * 2
                        ? (((i - 3) & 1) ? 0x5a : 0x3c)
                        : 0xa5;
      if (es_memory[i] != expected)
        return 2;
    }
  }
  return 0;
}
