/*
 * C parser fixture for an indirect inline-assembly tail jump.
 */
static unsigned long nyc_jump_observed_value;

static void nyc_jump_landing(unsigned long ignored, unsigned long value) {
  (void)ignored;
  nyc_jump_observed_value = value;
}

void nyc_indirect_asm_jump(unsigned long ignored, unsigned long value) {
  void (*target)(unsigned long, unsigned long) = nyc_jump_landing;
  __asm__ volatile("jmp *%0" : : "r"(target), "S"(value));
}

unsigned long nyc_indirect_asm_jump_observed(void) {
  return nyc_jump_observed_value;
}
