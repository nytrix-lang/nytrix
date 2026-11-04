unsigned long get_user_asm_probe(unsigned long *);
unsigned long get_user_asm_probe32(unsigned long *);
int main(void) {
  unsigned long address = 0;
  return get_user_asm_probe(&address) != 0x1234567800000008UL ||
         get_user_asm_probe32(&address) != 8;
}
