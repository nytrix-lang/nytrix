/*
 * Runtime harness for a function emitted by file-scope assembly.
 */
extern void nyc_file_asm_ret(void);
int main(void) {
  nyc_file_asm_ret();
  return 0;
}
