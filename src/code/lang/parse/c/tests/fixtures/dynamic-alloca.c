/*
 * Fixture: allocate and access a runtime-sized stack buffer with alloca.
 */
int main(void) {
  unsigned long size = 64;
  unsigned char *bytes = (unsigned char *)__builtin_alloca(size);
  bytes[0] = 37;
  return bytes[0] != 37;
}
