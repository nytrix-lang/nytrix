unsigned long nyc_inline_asm_memory_output(unsigned long, unsigned long,
                                           unsigned long);

int main(void) {
  return nyc_inline_asm_memory_output(2, 4, 8) != 51 ||
         nyc_inline_asm_memory_output(17, 56, 3) != 113;
}
