#ifndef NY_BASE_JSON_H
#define NY_BASE_JSON_H

#include <stddef.h>

/* Encode one byte inside a JSON string, without its surrounding quotes. */
static inline size_t ny_json_escape_byte(unsigned char c, char out[7]) {
  switch (c) {
  case '"':
    out[0] = '\\';
    out[1] = '"';
    out[2] = '\0';
    return 2;
  case '\\':
    out[0] = '\\';
    out[1] = '\\';
    out[2] = '\0';
    return 2;
  case '\n':
    out[0] = '\\';
    out[1] = 'n';
    out[2] = '\0';
    return 2;
  case '\r':
    out[0] = '\\';
    out[1] = 'r';
    out[2] = '\0';
    return 2;
  case '\t':
    out[0] = '\\';
    out[1] = 't';
    out[2] = '\0';
    return 2;
  default:
    if (c < 0x20) {
      static const char hex[] = "0123456789abcdef";
      out[0] = '\\';
      out[1] = 'u';
      out[2] = '0';
      out[3] = '0';
      out[4] = hex[c >> 4];
      out[5] = hex[c & 0x0f];
      out[6] = '\0';
      return 6;
    }
    out[0] = (char)c;
    out[1] = '\0';
    return 1;
  }
}

#endif
