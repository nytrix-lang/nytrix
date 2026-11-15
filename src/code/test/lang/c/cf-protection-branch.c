/*
 * Fixture: enable CET indirect-branch landing pads on emitted C functions.
 */
static int nyc_cf_target(int value) { return value + 1; }
static int (*volatile nyc_cf_callback)(int) = nyc_cf_target;

int main(void) { return nyc_cf_callback(41) != 42; }
