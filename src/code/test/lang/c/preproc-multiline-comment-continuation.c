/*
 * Fixture: preserve preprocessing across a multiline comment in a macro.
 */
#define NYC_CALLBACK_SIZE          \
  (2 + 4 + /* first part,          \
            * continued comment */ \
   (2 + 3) * 4)

_Static_assert(NYC_CALLBACK_SIZE == 26,
               "block-comment newlines remain inside continued macros");

int main(void) { return NYC_CALLBACK_SIZE != 26; }
