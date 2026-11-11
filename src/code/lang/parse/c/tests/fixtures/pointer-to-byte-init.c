/*
 * Fixture: reject implicit initialization of an integer byte from a pointer.
 */
unsigned char reject_pointer_to_byte(void *value) {
  unsigned char invalid = value;
  return invalid;
}
