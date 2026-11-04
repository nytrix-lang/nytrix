static int nyc_used_operand_asm(void) {
  int value = 0; asm volatile("" : "+r"(value)); return value;
}
int main(void) { return nyc_used_operand_asm(); }
