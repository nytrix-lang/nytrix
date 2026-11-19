/* NYC input for signed division and remainder by 2^31. */
#include <stdint.h>

__attribute__((noinline)) int64_t nyc_div_pow2_31(int64_t x) {
  return x / INT64_C(2147483648);
}

__attribute__((noinline)) int64_t nyc_mod_pow2_31(int64_t x) {
  return x % INT64_C(2147483648);
}
