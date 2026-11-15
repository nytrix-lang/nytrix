/*
 * Host fixture: verify an inline-assembly memory-input helper at runtime.
 */
int nyc_inline_asm_memory_input(const char *);
int main(void) {
  const char value = 65;
  return nyc_inline_asm_memory_input(&value) != 65;
}
