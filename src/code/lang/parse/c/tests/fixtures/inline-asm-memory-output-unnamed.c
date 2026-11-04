static unsigned long value = 8;
int main(void) {
  __asm__ volatile("addq $1, %0" : "+m"(value) : : "memory");
  return value != 9;
}
