/* A zero-length local object must not sit at the end of .data. */
static char zero_length_marker[0] __attribute__((used));
int keep(void) { return 7; }
