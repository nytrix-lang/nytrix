/*
 * A callee whose body starts with a loop has a back edge to its entry label.
 * Inlining it before it is optimized must store the formals before that
 * label, not inside the loop. The caller comes first so the callee is still
 * in its unoptimized form when it is spliced.
 */
static void nyc_fix(char *string, unsigned long length);

int nyc_inline_loop_entry(char *buffer) {
  __builtin_memcpy(buffer + 2, buffer, 2);
  nyc_fix(buffer + 2, 5);
  return buffer[2] == 'a' && buffer[3] == '?' && buffer[4] == 'z';
}

static void nyc_fix(char *string, unsigned long length) {
  while (length && *string) {
    if (*string < 32)
      *string = '?';
    string++;
    length--;
  }
}
