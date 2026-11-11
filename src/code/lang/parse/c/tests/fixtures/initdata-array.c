/*
 * Fixture: access an initialized array placed in a custom data section.
 */
extern char __attribute__((__section__(".probe.data"))) nyc_initdata_cmd[];
char nyc_initdata_first(void) { return nyc_initdata_cmd[0]; }
