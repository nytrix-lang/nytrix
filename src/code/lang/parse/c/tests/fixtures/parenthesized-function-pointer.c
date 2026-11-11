/*
 * Fixture: parse parenthesized function-pointer declarators and attributes.
 */
typedef void (*parenthesized_pointer_type)(void);
static void parenthesized_pointer_target(void) {}
static parenthesized_pointer_type parenthesized_pointer
    __attribute__((__unused__)) __attribute__((__section__(".nyc_exitcall"))) =
        parenthesized_pointer_target;
