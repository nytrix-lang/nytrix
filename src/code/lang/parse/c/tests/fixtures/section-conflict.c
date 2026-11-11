/*
 * Negative fixture for conflicting section attributes on a tentative global.
 */
__attribute__((__section__(".a"))) long nyc_conflict_v;
__attribute__((__section__(".b"))) long nyc_conflict_v;
