/*
 * Named assembly labels remain addressable from C without .globl.
 */
asm(".pushsection .rodata\n"
    ".balign 4096\n"
    "nyc_local_entry:\n"
    "push %rcx\n"
    "push %r11\n"
    "push %rax\n"
    "mov $1, %rax\n"
    "syscall\n"
    "pop %rax\n"
    "pop %r11\n"
    "pop %rcx\n"
    "ret $128\n"
    "int3\n"
    ".balign 4096\n"
    ".popsection\n");
extern unsigned char nyc_local_entry[];
static unsigned char *get_entry(void) { return nyc_local_entry; }
int main(void) {
  unsigned char *p = get_entry();
  /*
   * Inspect bytes; the syscall trampoline is never executed.
   */
  return p[0] != 0x51 || p[1] != 0x41 || p[2] != 0x53 || p[3] != 0x50 ||
         p[19] != 0x00 || p[20] != 0xcc;
}
