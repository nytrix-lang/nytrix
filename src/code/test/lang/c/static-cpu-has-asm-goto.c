/*
 * Fixture: compile Linux-style feature detection with asm-goto alternatives.
 */
static int nyc_has_bit(const unsigned char *p, unsigned char bit) {
  asm goto("# ALT: oldinstr\n\ttestb %[bitnum], %a[cap_byte]\n"
           "\tjnz %l[t_yes]\n\tjmp %l[t_no]\n"
           :
           : [feature] "i"(1), [bitnum] "i"(bit), [cap_byte] "i"(p)
           :
           : t_yes, t_no);
t_yes:
  return 1;
t_no:
  return 0;
}
int main(void) {
  const unsigned char yes = 4, no = 2;
  return nyc_has_bit(&yes, 4) != 1 || nyc_has_bit(&no, 4) != 0;
}
