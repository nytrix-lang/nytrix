static unsigned int nyc_atomic_and_value = 255;
static void nyc_atomic_bitwise(unsigned int mask) {
  asm volatile("lock andl %1, %0" : "+m"(nyc_atomic_and_value)
               : "ir"(mask) : "memory");
  asm volatile("lock orl %1, %0" : "+m"(nyc_atomic_and_value)
               : "ir"(128) : "memory");
  asm volatile("lock xorl %1, %0" : "+m"(nyc_atomic_and_value)
               : "ir"(12) : "memory");
  asm volatile("lock addl %1, %0" : "+m"(nyc_atomic_and_value)
               : "ir"(5) : "memory");
  asm volatile("lock addl %1, %0" : "+m"(nyc_atomic_and_value)
               : "ir"(mask) : "memory");
  asm volatile("lock subl %1, %0" : "+m"(nyc_atomic_and_value)
               : "ir"(mask) : "memory");
}
int main(void) { nyc_atomic_bitwise(60); return nyc_atomic_and_value != 181; }
