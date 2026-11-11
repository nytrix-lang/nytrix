/*
 * GCC host caller for the NYC va_start aggregate-parameter fixture.
 */
struct nyc_va_pair {
  unsigned long first;
  unsigned long second;
};

struct nyc_va_mixed {
  double real;
  unsigned long integer;
};

unsigned long nyc_va_after_stack_aggregate(unsigned long a, unsigned long b,
                                           unsigned long c, unsigned long d,
                                           unsigned long e,
                                           struct nyc_va_pair fixed, ...);
int nyc_va_after_mixed_aggregate(struct nyc_va_mixed fixed, ...);

int main(void) {
  struct nyc_va_pair pair = {11, 13};
  struct nyc_va_mixed mixed = {1.25, 3};
  unsigned long stack_result =
      nyc_va_after_stack_aggregate(1, 2, 3, 4, 5, pair, 17UL, 19UL);
  return stack_result != 75 ||
         nyc_va_after_mixed_aggregate(mixed, 2.5, 7UL) != 0;
}
