/*
 * Linux-style assembly fixture for WRMSR alternatives and register operands.
 */
typedef unsigned int u32;
typedef unsigned long long u64;

static inline __attribute__((__always_inline__)) void
nyc_alternative_msr_write(unsigned int msr, u64 value, unsigned int feature) {
  __asm__ __volatile__(
      "# ALT: oldinstr\n"
      "771:\n\t"
      "\n772:\n"
      "# ALT: padding\n"
      ".skip -(((775f-774f)-(772b-771b)) > 0) * "
      "((775f-774f)-(772b-771b)),0x90\n"
      "773:\n"
      ".pushsection .altinstructions, \"aM\", @progbits, 14\n"
      " .long 771b - .\n"
      " .long 774f - .\n"
      " .4byte %c[feature]\n"
      " .byte 773b-771b\n"
      " .byte 775f-774f\n"
      ".popsection\n"
      ".pushsection .altinstr_replacement, \"ax\"\n"
      "912: "
      ".pushsection .discard.annotate_data, \"M\", @progbits, 8; "
      ".long 912b - ., 1; .popsection\n"
      "# ALT: replacement\n"
      "774:\n\t"
      "wrmsr\n"
      "775:\n"
      ".popsection\n"
      :
      : "c"(msr), "a"((u32)value),
        "d"((u32)(value >> 32)), [feature] "i"(feature)
      : "memory");
}

void nyc_alternative_msr_write_callsite(void) {
  nyc_alternative_msr_write(0x48, 0x12345678ULL, 246);
}
