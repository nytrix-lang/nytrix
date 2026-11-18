/*
 * C front-end regression test: weak function.
 */
__attribute__((weak)) int nyc_weak_override(void) { return 1; }
__attribute__((weak)) int nyc_weak_data = 1;
int nyc_weak_suffix_data __attribute__((weak)) = 1;
int nyc_weak_alias_target(void) { return 3; }
extern int nyc_weak_alias(void)
    __attribute__((weak, alias("nyc_weak_alias_target")));
int nyc_second_weak_alias_target(void) { return 5; }
extern int nyc_second_weak_alias(void)
    __attribute__((weak, alias("nyc_second_weak_alias_target")));
