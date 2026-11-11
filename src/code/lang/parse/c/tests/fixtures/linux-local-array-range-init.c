/*
 * Fixture: initialize a local aggregate from a bounded byte range.
 */
struct nyc_ioc_probe {
  unsigned char bytes[4];
};

int main(void) {
  unsigned int long wide = 1;
  unsigned char matrix[2][4];
  unsigned char aligned_bytes[24] __attribute__((aligned(8)));
  _Static_assert(
      !__builtin_types_compatible_p(typeof(matrix[0]), typeof(&matrix[0][0])),
      "array row type must remain an array");
  unsigned char values[6] = {
      [1 ...(4 - 1)] = 7,
      11,
  };
  switch (9) {
  case (sizeof(struct nyc_ioc_probe[1]) ==
                sizeof(struct nyc_ioc_probe)
            ? 9
            : __invalid_size_argument_for_IOC):
    break;
  default:
    return 1;
  }
  return sizeof(wide) != 8 || sizeof(matrix[0]) != 4 ||
         sizeof(matrix[0][0]) != 1 || ((unsigned long)aligned_bytes & 7) != 0 ||
         values[0] != 0 || values[1] != 7 || values[2] != 7 || values[3] != 7 ||
         values[4] != 11 || values[5] != 0;
}
