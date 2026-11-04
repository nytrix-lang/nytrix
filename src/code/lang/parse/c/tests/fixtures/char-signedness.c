int nyc_plain_char_signed(void) { return (char)-1 < 0; }
int nyc_signed_char_signed(void) { return (signed char)-1 < 0; }
#ifdef __CHAR_UNSIGNED__
int nyc_char_unsigned_macro(void) { return 1; }
#else
int nyc_char_unsigned_macro(void) { return 0; }
#endif
