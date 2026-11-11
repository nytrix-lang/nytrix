/*
 * Host-side oracle for aggregate values passed through variadic calls.
 */
#include <stdarg.h>

struct nyc_uid {
  unsigned int value;
};

int nyc_read_uid(int marker, ...) {
  va_list arguments;
  va_start(arguments, marker);
  struct nyc_uid uid = va_arg(arguments, struct nyc_uid);
  va_end(arguments);
  return marker + uid.value;
}
