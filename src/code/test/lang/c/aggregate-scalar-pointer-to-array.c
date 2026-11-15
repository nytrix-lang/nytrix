/*
 * Fixture: preserve a scalar pointer-to-array field through dereference.
 */
struct holder {
  unsigned char (*buffer)[4];
};

int main(void) {
  unsigned char bytes[4];
  struct holder value;
  value.buffer = (void *)bytes;
  (*value.buffer)[2] = 7;
  return sizeof(__typeof__(*value.buffer)) == 4 && sizeof(*value.buffer) == 4 &&
                 (*value.buffer)[2] == 7
             ? 0
             : 1;
}
