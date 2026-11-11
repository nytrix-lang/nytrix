/*
 * Preprocessor fixture for diagnosing invalid constant shift expressions.
 */
#if 1 << 64
long c_invalid_shift(void) { return 1; }
#else
long c_invalid_shift(void) { return 0; }
#endif
