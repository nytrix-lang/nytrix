/*
 * Fixture: initialize a static table with repeated GNU label addresses.
 */
int main(void) {
  static void *const targets[2] = {[0 ... 1] = &&done};
  if (targets[0] != &&done || targets[1] != &&done)
    return 1;
  goto done;
done:
  return 0;
}
