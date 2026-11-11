/*
 * Fixture driver: call a function emitted into a custom text section.
 */
long nyc_section_function(void);
int main(void) { return nyc_section_function() != 18; }
