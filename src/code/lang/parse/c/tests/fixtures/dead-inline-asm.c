int main(void) {
  int value = 7;
  if (sizeof(value) == 8)
    __asm__ volatile("unsupported_dead_instruction" : : "r"(value) : "memory");
  return value != 7;
}
