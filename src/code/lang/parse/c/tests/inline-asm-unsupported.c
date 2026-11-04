int nyc_inline_asm_unsupported(void) {
  int value;
  __asm__ volatile("leaq %c1(%%rip), %0" : "=r"(value) : "i"(value));
  return value;
}

int nyc_inline_asm_expression_unsupported(void) { return asm("nop"); }

void nyc_inline_asm_goto_unsupported(void) {
  asm goto("jmp %l0" : : : : out);
out:
  return;
}
