static int nyc_unused_operand_asm(void) {
  int value = 0; asm volatile("" : "+r"(value)); return value;
}
int main(void) { return 0; }
