/*
 * Host driver for inline-loop-entry.c.
 */
int nyc_inline_loop_entry(char *);
int main(void) {
  char buffer[16] = {'a', 1, 'x', 'y', 'z', 'q', 'q', 0};
  return nyc_inline_loop_entry(buffer) ? 0 : 1;
}
