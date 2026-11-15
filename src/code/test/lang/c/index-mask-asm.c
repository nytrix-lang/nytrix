/*
 * Fixture: create an all-bits index mask with compare-and-carry assembly.
 */
static unsigned long mask_for(unsigned long index) {
  unsigned long mask, size = 5;
  asm("cmp %1,%2; sbb %0,%0" : "=r"(mask) : "g"(size), "r"(index) : "cc");
  return mask;
}
int main(void) {
  return mask_for(0) != ~0UL || mask_for(4) != ~0UL || mask_for(5) != 0 ||
         mask_for(~0UL) != 0;
}
