unsigned long nyc_hweight64_probe(unsigned long);
unsigned int nyc_hweight32_probe(unsigned int);
unsigned int __sw_hweight32(unsigned int value) {
  unsigned int count = 0;
  for (; value; value &= value - 1)
    ++count;
  return count;
}
unsigned long __sw_hweight64(unsigned long value) {
  unsigned long count = 0;
  for (; value; value &= value - 1)
    ++count;
  return count;
}
int main(void) {
  return nyc_hweight64_probe(0) != 0 ||
         nyc_hweight64_probe(0x8000000000000001UL) != 2 ||
         nyc_hweight32_probe(0) != 0 ||
         nyc_hweight32_probe(0x80000001U) != 2;
}
