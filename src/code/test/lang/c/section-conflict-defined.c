/*
 * Negative fixture for conflicting section attributes on a defined global.
 */
long nyc_conflict_v = 1;
__attribute__((__section__(".b"))) long nyc_conflict_v = 2;
