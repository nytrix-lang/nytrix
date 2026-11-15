/*
 * Fixture: dead static functions must not retain their local static objects.
 */
extern int undefined_addressable_target;

static void unreferenced_function(void) {
  static int *__attribute__((used, section(".discard.addressable"))) marker =
      &undefined_addressable_target;
  (void)marker;
}

int main(void) { return 0; }
