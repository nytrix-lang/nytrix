unsigned long observed;
void copy_memory_operand(const unsigned long *value) {
  __asm__ __volatile__("movq %0, %%rax\n\t"
                       "movq %%rax, observed(%%rip)"
                       : : "m"(*value) : "memory");
}
int main(void) {
  unsigned long value = 0x123456789abcdef0UL;
  copy_memory_operand(&value);
  return observed != value;
}
