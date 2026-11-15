/*
 * Inline assembly fixture for read-write memory operands and clobbers.
 */
void nyc_add_memory(long *value, long delta) {
  __asm__ volatile("addq %[delta], %[value]"
                   : [value] "+m"(*value)
                   : [delta] "re"(delta)
                   : "memory");
}
int main(void) {
  long value = 39;
  nyc_add_memory(&value, 3);
  return value != 42;
}
