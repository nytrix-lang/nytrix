/* JSON report and artifact APIs for the Nytrix fuzzing commands. */
#ifndef NYTRIX_TEST_FUZZ_JSON_H
#define NYTRIX_TEST_FUZZ_JSON_H

#include "core.h"

static const char *fuzz_json_skip_ws(const char *p) {
  while (p && *p && isspace((unsigned char)*p))
    ++p;
  return p;
}

static char *fuzz_json_string_dup(const char **cursor, const char *end) {
  const char *p = *cursor;
  if (!p || p >= end || *p != '"')
    return NULL;
  ++p;
  str_buf_t b = {0};
  while (p < end && *p) {
    unsigned char c = (unsigned char)*p++;
    if (c == '"') {
      *cursor = p;
      return sb_take(&b);
    }
    if (c == '\\' && p < end) {
      unsigned char e = (unsigned char)*p++;
      switch (e) {
      case '"':
        c = '"';
        break;
      case '\\':
        c = '\\';
        break;
      case '/':
        c = '/';
        break;
      case 'b':
        c = '\b';
        break;
      case 'f':
        c = '\f';
        break;
      case 'n':
        c = '\n';
        break;
      case 'r':
        c = '\r';
        break;
      case 't':
        c = '\t';
        break;
      case 'u':
        if (p + 4 <= end)
          p += 4;
        c = '?';
        break;
      default:
        c = e;
        break;
      }
    }
    if (!sb_append_c(&b, (char)c)) {
      free(b.data);
      return NULL;
    }
  }
  free(b.data);
  return NULL;
}

#endif
