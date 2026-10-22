#ifndef NY_BASE_ASCII_H
#define NY_BASE_ASCII_H

/* Return an ASCII hexadecimal digit's value, or -1 when c is not a digit. */
static inline int ny_ascii_hex_digit_value(unsigned char c) {
  if (c >= '0' && c <= '9')
    return (int)(c - '0');
  if (c >= 'a' && c <= 'f')
    return 10 + (int)(c - 'a');
  if (c >= 'A' && c <= 'F')
    return 10 + (int)(c - 'A');
  return -1;
}

#endif
