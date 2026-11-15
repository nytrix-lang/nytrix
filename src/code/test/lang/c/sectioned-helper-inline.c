/*
 * Placement/retention does not prohibit inlining a translation-local helper.
 */
extern int missing_begin[], missing_end[];
int main(void);
static volatile int count;
static void __attribute__((section(".init.text"))) empty(int *begin, int *end) {
}
extern void disabled_log(void);
static void feature(int *begin, int *end, int *cfi, int *cfi_end, int enabled) {
  if (0 && enabled)
    disabled_log();
}
void public_empty(int *begin, int *end) {}
static int *__attribute__((noinline)) tick(void) {
  ++count;
  return (int *)(unsigned long)count;
}
int main(void) {
  empty(missing_begin, missing_end);
  feature(missing_begin, missing_end, missing_begin, missing_end, 1);
  public_empty(missing_begin, missing_end);
  empty(tick(), tick());
  return count != 2;
}
