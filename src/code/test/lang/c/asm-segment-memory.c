/*
 * C front-end regression test: asm segment memory.
 */
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef __SIZE_TYPE__ usize;

volatile u32 fs_memory[4] = {0x11223344u, 0x55667788u, 0xaabbccddu,
                             0xcafebabeu};
volatile u32 gs_memory[4] = {0x99aabbccu, 0xddeeff00u, 0x12345678u,
                             0xfeedfaceu};

#define ACCESSORS(segment, suffix, type, output_constraint, input_constraint) \
  __attribute__((noinline)) type read_##segment##suffix(usize address) {      \
    type *pointer = (type *)address;                                          \
    type value;                                                               \
    __asm__ volatile("mov" #suffix " %%" #segment ":%1,%0"                    \
                     : output_constraint(value)                               \
                     : "m"(*pointer));                                        \
    return value;                                                             \
  }                                                                           \
  __attribute__((noinline)) void write_##segment##suffix(type value,          \
                                                         usize address) {     \
    type *pointer = (type *)address;                                          \
    __asm__ volatile("mov" #suffix " %1,%%" #segment ":%0"                    \
                     : "+m"(*pointer)                                         \
                     : input_constraint(value));                              \
  }

ACCESSORS(fs, b, u8, "=q", "qi")
ACCESSORS(fs, w, u16, "=r", "ri")
ACCESSORS(fs, l, u32, "=r", "ri")
ACCESSORS(gs, b, u8, "=q", "qi")
ACCESSORS(gs, w, u16, "=r", "ri")
ACCESSORS(gs, l, u32, "=r", "ri")

int configure_segments(u16 fs_selector, u16 gs_selector) {
  u16 fs, gs;
  __asm__ volatile("movw %0,%%fs" : : "rm"(fs_selector));
  __asm__ volatile("movw %0,%%gs" : : "rm"(gs_selector));
  __asm__ volatile("movw %%fs,%0" : "=rm"(fs));
  __asm__ volatile("movw %%gs,%0" : "=rm"(gs));
  return fs != fs_selector || gs != gs_selector;
}

int exercise_segments(void) {
  if (read_fsb(0) != 0x44 || read_fsw(2) != 0x1122 ||
      read_fsl(4) != 0x55667788u || read_gsb(0) != 0xcc ||
      read_gsw(2) != 0x99aa || read_gsl(4) != 0xddeeff00u)
    return 1;
  write_fsb(0xe1, 1);
  write_fsw(0xd2c3, 6);
  write_fsl(0xdeadbeefu, 8);
  write_gsb(0x12, 1);
  write_gsw(0x3456, 6);
  write_gsl(0x789abcdeu, 8);
  if (fs_memory[0] != 0x1122e144u || fs_memory[1] != 0xd2c37788u ||
      fs_memory[2] != 0xdeadbeefu || fs_memory[3] != 0xcafebabeu ||
      gs_memory[0] != 0x99aa12ccu || gs_memory[1] != 0x3456ff00u ||
      gs_memory[2] != 0x789abcdeu || gs_memory[3] != 0xfeedfaceu)
    return 2;
  return 0;
}
