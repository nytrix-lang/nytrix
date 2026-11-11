/*
 * Fixture: convert signed and unsigned __int128 values at call boundaries.
 */
static int unsigned_sink(unsigned __int128 value) { return 0; }

static int call_unsigned_sink(__int128 value) { return unsigned_sink(value); }

static __int128 values[1];

int main(void) { return call_unsigned_sink(values[0]); }
