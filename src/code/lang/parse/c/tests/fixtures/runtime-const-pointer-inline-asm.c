/*
 * C fixture for runtime-constant pointer expressions in inline assembly.
 */
struct nyc_marker;
static struct nyc_marker *nyc_fixture_symbol;

#define nyc_runtime_const_pointer(sym)                    \
  ({                                                      \
    typeof(sym) result;                                   \
    asm("mov %1,%0\n1:\n"                                 \
        ".pushsection runtime_ptr_" #sym ",\"a\"\n\t"     \
        ".long 1b - %c2 - .\n"                            \
        ".popsection"                                     \
        : "=r"(result)                                    \
        : "i"(0x0123456789abcdefull), "i"(sizeof(long))); \
    result;                                               \
  })

static struct nyc_marker *nyc_get_runtime_const_pointer(void) {
  return nyc_runtime_const_pointer(nyc_fixture_symbol);
}

int main(void) {
  return (unsigned long)nyc_get_runtime_const_pointer() !=
         0x0123456789abcdefull;
}
