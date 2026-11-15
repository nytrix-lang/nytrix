/*
 * Copy intrinsics preserve resident counters but may overwrite escaped locals.
 */
#include <string.h>

int main(void) {
  char bytes[771];
  unsigned long used = 0;
  unsigned long sum = 0;
  for (unsigned long i = 0; i < 257; ++i) {
    if (memcpy(bytes + used, "abc", 3) != bytes + used)
      return 5;
    used += 3;
    sum += i;
  }
  if (used != 771 || sum != 32896)
    return 1;
  for (unsigned long i = 0; i < used; ++i)
    if (bytes[i] != "abc"[i % 3])
      return 2;
  unsigned long value = 0;
  for (unsigned long i = 0; i < 257; ++i) {
    unsigned long replacement = i + 7;
    __builtin_memcpy(&value, &replacement, sizeof(value));
    if (value != i + 7)
      return 3;
  }
  return value == 263 ? 0 : 4;
}
