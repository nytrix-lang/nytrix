/*
 * Fixture: evaluate Linux-style constant ilog2/order_base_2 bit-field widths.
 */
#define TEST_ILOG2(n)                                        \
  (__builtin_constant_p(n)                                   \
       ? ((n) < 2 ? 0 : 63 - __builtin_clzll(n))             \
       : (sizeof(n) <= 4 ? __ilog2_u32(n) : __ilog2_u64(n)))
#define TEST_ORDER_BASE_2(n)                                  \
  (__builtin_constant_p(n)                                    \
       ? ((n) == 0 || (n) == 1 ? 0 : TEST_ILOG2((n) - 1) + 1) \
       : __order_base_2(n))

struct probe {
  unsigned char width : TEST_ORDER_BASE_2(4);
};

int main(void) {
  struct probe value;
  value.width = 3;
  return value.width == 3 ? 0 : 1;
}
