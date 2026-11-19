#include <stdint.h>
#include <stdio.h>

int64_t nyc_div_pow2_31(int64_t);
int64_t nyc_mod_pow2_31(int64_t);

int main(void) {
  const int64_t values[] = {
    INT64_MIN, INT64_MIN + 1, -INT64_C(4294967297),
    -INT64_C(4294967296), -INT64_C(2147483649),
    -INT64_C(2147483648), -INT64_C(2147483647),
    -1, 0, 1, INT64_C(2147483647), INT64_C(2147483648),
    INT64_C(2147483649), INT64_C(4294967295),
    INT64_C(4294967296), INT64_MAX
  };
  for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
    int64_t x = values[i];
    int64_t q = nyc_div_pow2_31(x);
    int64_t r = nyc_mod_pow2_31(x);
    int64_t ref_q = x / INT64_C(2147483648);
    int64_t ref_r = x % INT64_C(2147483648);
    if (q != ref_q || r != ref_r) {
      fprintf(stderr, "x=%lld q=%lld/%lld r=%lld/%lld\n",
              (long long)x, (long long)q, (long long)ref_q,
              (long long)r, (long long)ref_r);
      return 1;
    }
  }
  return 0;
}
