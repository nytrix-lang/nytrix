long c_bitwise(long left, long right) {
  return (~left & right) | (left ^ right);
}

long c_shift_left(long value, long amount) { return value << amount; }

long c_shift_right(long value, long amount) { return value >> amount; }

long c_shift_precedence(long value) { return value + 2 << 1; }

long c_shift_additive_precedence(long value) { return value << 1 + 1; }

long c_shift_compound(long value) {
  value <<= 2;
  value >>= 1;
  return value;
}
