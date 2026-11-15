/*
 * Fixture: define and reference a global object with hidden ELF visibility.
 */
struct visibility_object {
  unsigned long value;
};
struct visibility_object nyc_hidden_object
    __attribute__((visibility("hidden"))) = {37};
int main(void) { return nyc_hidden_object.value != 37; }
