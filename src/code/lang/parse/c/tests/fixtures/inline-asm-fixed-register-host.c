extern unsigned long nyc_fixed_register_a;
extern unsigned long nyc_fixed_register_c;
extern unsigned long nyc_fixed_register_d;
void nyc_fixed_register_inputs(unsigned long, unsigned long, unsigned long);
int main(void) {
  nyc_fixed_register_inputs(0x12345678UL, 0xabcdef01UL, 0x98765432UL);
  return nyc_fixed_register_a != 0x12345678UL ||
         nyc_fixed_register_c != 0xabcdef01UL ||
         nyc_fixed_register_d != 0x98765432UL;
}
