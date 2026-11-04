extern int nyc_asm_target;
extern int __tracepoint_nyc_asm_tracepoint;
extern int nyc_tracepoint_ptr_entry;
extern int __nyc_asm_blob_0;
extern int nyc_asm_trampoline(void);
extern int nyc_asm_bare_trampoline(void);
extern int nyc_asm_text_trampoline(void);
extern int nyc_asm_function(void);
extern unsigned long nyc_asm_read_rsp(void);
extern const unsigned char nyc_asm_abs[];

int main(void) {
  const char *encoded_target =
      (const char *)&__nyc_asm_blob_0 + __nyc_asm_blob_0;
  return encoded_target != (const char *)&nyc_asm_target ||
         (char *)&nyc_tracepoint_ptr_entry + nyc_tracepoint_ptr_entry !=
             (char *)&__tracepoint_nyc_asm_tracepoint ||
         nyc_asm_trampoline() != nyc_asm_function() ||
         nyc_asm_bare_trampoline() != nyc_asm_function() ||
         (nyc_asm_read_rsp() & 15UL) != 8UL ||
         nyc_asm_text_trampoline == nyc_asm_function ||
         nyc_asm_abs[0] != 'A' ||
         nyc_asm_abs[1] != 0 || nyc_asm_abs[2] != 'B' || nyc_asm_abs[3] != 0 ||
         *(const int **)(nyc_asm_abs + 8) != &nyc_asm_target;
}
