/*
 * C preprocessor fixture for file-scope diagnostic pragmas.
 */
_Pragma("GCC diagnostic push")

    int main(void) {
  return 0;
}

_Pragma("GCC diagnostic pop")
