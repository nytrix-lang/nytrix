/*
 * Inline assembly fixture for writes to a GS-segment memory operand.
 */
static unsigned char gs_store_value;
int main(void) {
  unsigned char value = 37;
  asm volatile("movb %[val], %%gs:%[var]"
               : [var] "=m"(gs_store_value)
               : [val] "qi"(value));
  return gs_store_value != value;
}
