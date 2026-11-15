/*
 * Fixture driver: verify an x86 put-user helper stores a value at an address.
 */
int put_user_asm_probe(unsigned long *, unsigned long);
int main(void) {
  unsigned long address = 0;
  return put_user_asm_probe(&address, 5) != 0 || address != 5;
}
