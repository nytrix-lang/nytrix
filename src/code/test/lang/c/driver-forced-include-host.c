/*
 * Host fixture: verify a forced-include definition in the compiled program.
 */
int nyc_driver_forced_value(void);
int main(void) { return nyc_driver_forced_value() != 37; }
