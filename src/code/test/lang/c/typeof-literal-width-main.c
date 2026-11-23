/* Host driver for typeof-literal-width.c. */
long a1(long), a2(long), a4(long), a5(long), a6(long), a7(long), a8(long long);
unsigned long a3(unsigned long), a9(unsigned long);
int main(void) {
  long v = 0x7fff38b3ed28L;
  return a1(v) != v || a2(v) != v || a3(v) != (unsigned long)v || a4(v) != v ||
         a5(v) != v || a6(v) != v || a7(v) != v || a8(v) != v || a9(v) != (unsigned long)v;
}
