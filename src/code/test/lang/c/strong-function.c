/*
 * C front-end regression test: strong function.
 */
int nyc_weak_override(void) { return 2; }
int nyc_weak_data = 2;
int nyc_weak_suffix_data = 2;
int nyc_weak_alias(void) { return 4; }
