/*
 * Inline assembly fixture for aligned 128-bit compare-and-exchange.
 */
struct __attribute__((aligned(16))) nyc_cmpxchg16b_pair {
  unsigned long low;
  unsigned long high;
};

static struct nyc_cmpxchg16b_pair nyc_cmpxchg16b_value = {1, 2};

int main(void) {
  struct nyc_cmpxchg16b_pair *pointer = &nyc_cmpxchg16b_value;
  struct nyc_cmpxchg16b_pair old = {1, 2};
  struct nyc_cmpxchg16b_pair next = {3, 4};
  _Bool success;
  asm volatile("lock cmpxchg16b %[ptr]"
               : "=@ccz"(success), [ptr] "+m"(*pointer), "+a"(old.low),
                 "+d"(old.high)
               : "b"(next.low), "c"(next.high)
               : "memory");
  if (!success || nyc_cmpxchg16b_value.low != 3 ||
      nyc_cmpxchg16b_value.high != 4 || old.low != 1 || old.high != 2)
    return 1;

  old.low = 99;
  old.high = 98;
  next.low = 5;
  next.high = 6;
  asm volatile("lock cmpxchg16b %[ptr]"
               : "=@ccz"(success), [ptr] "+m"(*pointer), "+a"(old.low),
                 "+d"(old.high)
               : "b"(next.low), "c"(next.high)
               : "memory");
  if (success || nyc_cmpxchg16b_value.low != 3 ||
      nyc_cmpxchg16b_value.high != 4 || old.low != 3 || old.high != 4)
    return 2;
  return 0;
}
