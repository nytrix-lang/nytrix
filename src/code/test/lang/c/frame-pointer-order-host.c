extern long omit_fp_sum8(long, long, long, long, long, long, long, long);

int main(void) {
  if (omit_fp_sum8(1, 2, 3, 4, 5, 6, 7, 8) != 36)
    return 1;
  return omit_fp_sum8(-8, -7, -6, -5, -4, -3, -2, -1) != -36;
}
