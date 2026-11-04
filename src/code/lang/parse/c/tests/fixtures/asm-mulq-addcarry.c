unsigned long long nyc_mulq_addcarry(unsigned long long low,
                                     unsigned long long multiplier,
                                     unsigned long long addend,
                                     unsigned long long *high_out) {
  unsigned long long high;
  __asm__("mulq %[mul]"
          : "+a"(low), "=d"(high)
          : [mul] "rm"(multiplier));
  __asm__("addq %[add], %[lo]; adcq $0, %[hi]"
          : [lo] "+r"(low), [hi] "+r"(high)
          : [add] "erm"(addend));
  *high_out = high;
  return low;
}

unsigned long long nyc_divq(unsigned long long low,
                            unsigned long long high,
                            unsigned long long divisor,
                            unsigned long long *remainder_out) {
  __asm__("divq %[div]"
          : [lo] "+a"(low), [hi] "+d"(high)
          : [div] "rm"(divisor));
  *remainder_out = high;
  return low;
}
