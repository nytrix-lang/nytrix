/*
 * No instruction follows a noreturn call or __builtin_unreachable.
 */
void die(const char *) __attribute__((noreturn));
static __attribute__((always_inline)) inline int tripled(int x) { return x * 3; }
int checked(int x) {
  if (x > 3)
    die("big");
  if (x < -3) {
    __asm__ volatile("ud2");
    __builtin_unreachable();
  }
  return tripled(x) + 1;
}
