/*
 * Parser fixture for a GS-address-space read-modify-write operand.
 */
void nyc_inline_asm_gs_percpu_add8(unsigned char *address,
                                   unsigned char value) {
  __asm__ volatile("addb %[val], %[var]"
                   : [var] "+m"(*((__seg_gs unsigned char *)address))
                   : [val] "qi"(value));
}

void nyc_inline_asm_gs_percpu_add16(unsigned short *address,
                                    unsigned short value) {
  __asm__ volatile("addw %[val], %[var]"
                   : [var] "+m"(*((__seg_gs unsigned short *)address))
                   : [val] "ri"(value));
}

void nyc_inline_asm_gs_percpu_add32(unsigned int *address, unsigned int value) {
  __asm__ volatile("addl %[val], %[var]"
                   : [var] "+m"(*((__seg_gs unsigned int *)address))
                   : [val] "ri"(value));
}

void nyc_inline_asm_gs_percpu_add64(unsigned long *address,
                                    unsigned long value) {
  __asm__ volatile("addq %[val], %[var]"
                   : [var] "+m"(*((__seg_gs unsigned long *)address))
                   : [val] "re"(value));
}
