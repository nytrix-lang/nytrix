/*
 * Fixture: place a helper-backed function in a custom ELF text section.
 */
static long nyc_section_helper(void) { return 17; }
__attribute__((section(".probe.text"))) long nyc_section_function(void) {
  return nyc_section_helper() + 1;
}

long nyc_parameter_section_function(unsigned long value);
long __attribute__((section(".probe.init.text")))
nyc_parameter_section_function(unsigned long value) {
  return value + nyc_section_helper();
}
