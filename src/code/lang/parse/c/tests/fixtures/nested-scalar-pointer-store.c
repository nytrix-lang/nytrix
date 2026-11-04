static void nyc_store_nested_u64(unsigned long long **pointer, int index,
                                 unsigned long long value) {
  (*pointer)[index] = value;
}

int main(void) {
  unsigned long long values[2] = { 0, 0 };
  unsigned long long *pointer = values;
  nyc_store_nested_u64(&pointer, 1, 73);
  return values[1] != 73;
}
