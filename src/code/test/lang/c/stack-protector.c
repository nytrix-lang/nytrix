/*
 * C front-end regression test: stack protector.
 */
#if NYC_EXPECT_SSP == 3
#if !defined(__SSP_ALL__) || __SSP_ALL__ != 2
#error incorrect stack-protector-all predefines
#endif
#elif NYC_EXPECT_SSP == 2
#if !defined(__SSP_STRONG__) || __SSP_STRONG__ != 3
#error incorrect stack-protector-strong predefines
#endif
#elif defined(__SSP__) || defined(__SSP_STRONG__) || defined(__SSP_ALL__)
#error disabled stack protection still advertises enabled predefines
#endif

extern void consume(char *, int);
extern void change_guard(void);

__attribute__((noinline)) int protected_buffer(int mode) {
  char buffer[32];
  buffer[0] = 7;
  buffer[31] = 3;
  consume(buffer, mode);
  if (mode == 3)
    return buffer[31] + 8;
  return buffer[0] + 5;
}

__attribute__((noinline, no_stack_protector)) int unprotected_buffer(int mode) {
  char buffer[32];
  buffer[0] = 7;
  consume(buffer, mode);
  return buffer[0] + 5;
}

__attribute__((noinline)) int protected_scalar(void) {
  change_guard();
  return 17;
}
