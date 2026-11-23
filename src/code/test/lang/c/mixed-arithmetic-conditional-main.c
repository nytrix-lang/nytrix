/* Host driver for mixed-arithmetic-conditional.c. */
double nyr_mixed_double(int);
float nyr_mixed_float(int);
double nyr_mixed_widen(int);
int nyr_pragma_scope(int);
int main(void) {
  return nyr_mixed_double(1) != 1.5 || nyr_mixed_double(0) != 0.0 ||
         nyr_mixed_float(0) != 3.0f || nyr_mixed_float(1) != 2.0f ||
         nyr_mixed_widen(0) != 3.0 || nyr_mixed_widen(1) != 2.0 ||
         nyr_pragma_scope(4) != 5;
}
