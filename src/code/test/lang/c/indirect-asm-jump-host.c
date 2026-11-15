/*
 * Host caller for the NYC indirect inline-assembly jump fixture.
 */
void nyc_indirect_asm_jump(unsigned long ignored, unsigned long value);
unsigned long nyc_indirect_asm_jump_observed(void);

int main(void) {
  nyc_indirect_asm_jump(1, 42);
  return nyc_indirect_asm_jump_observed() != 42;
}
