/*
 * Fixture driver: exercise condition-code outputs from atomic assembly ops.
 */
unsigned char sub_and_test_asm(long *, long);
void addl_asm(int *, unsigned int);
int main(void) {
  long value = 12;
  if (sub_and_test_asm(&value, 5) != 0 || value != 7)
    return 1;
  if (sub_and_test_asm(&value, 7) != 1 || value != 0)
    return 2;
  int word = 10;
  addl_asm(&word, 7U);
  if (word != 17)
    return 3;
  return 0;
}
