/*
 * Fixture: initialize object-pointer compound literals with a null void
 * pointer.
 */
int main(void) {
  const char *const *ids = (const char *const[]){"LEN0036", (void *)0};
  return ids[0][0] != 'L' || ids[1] != 0;
}
