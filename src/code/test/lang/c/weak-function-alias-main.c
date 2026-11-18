/*
 * Entry point for the weak function alias C front-end test.
 */
int nyc_weak_alias(void);
int nyc_second_weak_alias(void);

int main(void) { return nyc_weak_alias() != 3 || nyc_second_weak_alias() != 5; }
