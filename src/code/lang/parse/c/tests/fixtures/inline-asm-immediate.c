extern const char nyc_asm_immediate_value[];
extern const unsigned int nyc_asm_immediate_local;
extern const unsigned char nyc_asm_bugtable_data[];
extern const unsigned char nyc_asm_bugsite[];
const unsigned char *nyc_asm_bugtable_probe(void) {
  struct nyc_bug_entry_probe { int bug; int file; unsigned short line; unsigned short flags; };
  struct nyc_bug_entry_probe *bug;
  __asm__ volatile("leaq (2f)(%%rip), %[addr]\n1:\n.globl nyc_asm_bugsite\nnyc_asm_bugsite:\n.byte 0x90\n.pushsection .nyc_bug_table,\"aw\"\n.balign 4\n.globl nyc_asm_bugtable_data\nnyc_asm_bugtable_data:\n2:\t.long 1b - .\n\t.long %c[file] - .\n\t.word %c[line]\n\t.word %c[flags]\n.org 2b+%c[size]\n.popsection\n" : [addr] "=r" (bug) : [file] "i" (__FILE__), [line] "i" (__LINE__), [flags] "i" (42), [size] "i" (sizeof(struct nyc_bug_entry_probe)));
  return (const unsigned char *)bug;
}
int main(void) {
  __auto_type flags = (1 << 0) | ((1 << 1) | ((9) << 8));
  asm volatile(".pushsection .rodata\n.globl nyc_asm_immediate_local\nnyc_asm_immediate_local:\n.long %c[flags]\n.popsection\n" : : [flags] "i" (flags));
  asm volatile(".pushsection .rodata\n.globl nyc_asm_immediate_value\nnyc_asm_immediate_value:\n.ascii \"%0\"\n.ascii \"%c0\"\n.popsection\n" : : "i" (42));
  if (nyc_asm_bugtable_probe() != nyc_asm_bugtable_data) return 1;
  if (nyc_asm_immediate_local != 2307) return 1;
  const char *bug_file = (const char *)nyc_asm_bugtable_data + 4 + ((const int *)nyc_asm_bugtable_data)[1];
  const char *expected_bug_file = __FILE__;
  while (*bug_file && *bug_file == *expected_bug_file) { ++bug_file; ++expected_bug_file; }
  if (*bug_file || *expected_bug_file) return 1;
  if ((const unsigned char *)((const char *)nyc_asm_bugtable_data + *(const int *)nyc_asm_bugtable_data) != nyc_asm_bugsite || *(const unsigned short *)(nyc_asm_bugtable_data + 10) != 42 || *(const unsigned short *)(nyc_asm_bugtable_data + 8) == 0) return 1;
  return nyc_asm_immediate_value[0] != '4' || nyc_asm_immediate_value[1] != '2' || nyc_asm_immediate_value[2] != '4' || nyc_asm_immediate_value[3] != '2';
}
