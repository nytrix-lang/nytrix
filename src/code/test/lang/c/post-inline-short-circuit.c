/*
 * Disabled feature branches must not retain unavailable linker symbols.
 */
extern int unavailable_feature(void);
static inline int feature_enabled(unsigned flags) { return 0; }
int check(unsigned flags) {
  if (flags && feature_enabled(flags))
    return unavailable_feature();
  return flags ? 23 : 17;
}
int main(void) {
  return check(0) != 17 || check(1) != 23 || check(0xffffffffU) != 23;
}
