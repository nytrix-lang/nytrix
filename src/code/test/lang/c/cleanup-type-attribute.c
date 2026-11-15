/*
 * Runtime fixture for cleanup attributes on values with custom typedefs.
 */
static int cleaned;

static void record_cleanup(int *value) { cleaned += *value; }

int main(void) {
  {
    int __attribute__((cleanup(record_cleanup))) value = 7;
    if (cleaned != 0)
      return 1;
    if (value != 7)
      return 2;
  }
  return cleaned == 7 ? 0 : 3;
}
