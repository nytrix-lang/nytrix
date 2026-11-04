int nyc_bsrq_probe(unsigned long);
int nyc_bsrl_probe(unsigned int);
unsigned long nyc_bsr_generic_probe(unsigned long);
int main(void) {
  return nyc_bsrq_probe(0) != 0 ||
         nyc_bsrq_probe(1) != 1 ||
         nyc_bsrq_probe(0x100000000UL) != 33 ||
         nyc_bsrq_probe(0x8000000000000000UL) != 64 ||
         nyc_bsrl_probe(0) != 0 ||
         nyc_bsrl_probe(1) != 1 ||
         nyc_bsrl_probe(0x80000000U) != 32 ||
         nyc_bsrl_probe(0x120000U) != 21 ||
         nyc_bsr_generic_probe(0x1000UL) != 12 ||
         nyc_bsr_generic_probe(0x8000000000000000UL) != 63;
}
