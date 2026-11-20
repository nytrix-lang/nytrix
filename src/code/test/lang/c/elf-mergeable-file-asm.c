__asm__(".section \"nyc_merge_strings\",\"aMS\",%progbits,1\n"
        ".globl nyc_merge_string\n"
        "nyc_merge_string:\n"
        ".asciz \"NYC\"\n"
        ".previous\n");

__asm__(".section \"nyc_merge_refs\",\"a\"\n"
        ".globl nyc_merge_ref\n"
        "nyc_merge_ref:\n"
        ".long nyc_merge_string - .\n"
        ".previous\n");

extern const char nyc_merge_string[];
extern const int nyc_merge_ref;

int main(void) {
  unsigned long resolved = (unsigned long)&nyc_merge_ref + nyc_merge_ref;
  return resolved != (unsigned long)nyc_merge_string ||
         nyc_merge_string[1] != 'Y';
}
