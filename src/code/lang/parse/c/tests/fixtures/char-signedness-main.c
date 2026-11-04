int nyc_plain_char_signed(void);
int nyc_signed_char_signed(void);
int nyc_char_unsigned_macro(void);
#ifdef __CHAR_UNSIGNED__
int main(void) { return nyc_plain_char_signed() || !nyc_signed_char_signed() || !nyc_char_unsigned_macro(); }
#else
int main(void) { return !nyc_plain_char_signed() || !nyc_signed_char_signed() || nyc_char_unsigned_macro(); }
#endif
