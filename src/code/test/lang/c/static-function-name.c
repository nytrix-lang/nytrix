/*
 * Fixture: preserve the function name in static storage through __func__.
 */
#include <string.h>

static const char *function_name(void) {
  static const char *const name = __func__;
  return name;
}

int main(void) { return strcmp(function_name(), "function_name") != 0; }
