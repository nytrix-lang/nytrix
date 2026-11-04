static int count_trailing_zeros(unsigned long long value) {
  return __builtin_ctzll(value);
}
int main(void) {
  return count_trailing_zeros(1ULL) != 0 ||
         count_trailing_zeros(1ULL << 17) != 17 ||
         count_trailing_zeros(0x120000ULL) != 17 ||
         count_trailing_zeros(1ULL << 63) != 63;
}
