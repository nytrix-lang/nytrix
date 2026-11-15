/*
 * Runtime fixture comparing builtin first-set-bit behavior with assembly.
 */
static int asm_ffs(int x) {
  int r;
  __asm__("bsfl %1,%0" : "=r"(r) : "rm"(x), "0"(-1));
  return r + 1;
}
int main(void) {
  return __builtin_ffs(0) != 0 || __builtin_ffs(1) != 1 ||
         __builtin_ffs(8) != 4 || __builtin_ffs(0x40000000) != 31 ||
         asm_ffs(0) != 0 || asm_ffs(1) != 1 || asm_ffs(8) != 4;
}
