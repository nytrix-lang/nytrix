/*
 * Fixture: pass a packed aggregate as an inline-assembly memory operand.
 */
struct descriptor {
  unsigned short limit;
  unsigned long base;
} __attribute__((packed));
void load_descriptor(const struct descriptor *value) {
  __asm__ volatile("lgdt %0" : : "m"(*value));
}
