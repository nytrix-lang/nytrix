void nyc_mwaitx_inline_asm(unsigned int eax, unsigned int ebx,
                           unsigned int ecx) {
  __asm__ volatile("mwaitx" : : "a"(eax), "b"(ebx), "c"(ecx));
}

int main(void) { return 0; }
