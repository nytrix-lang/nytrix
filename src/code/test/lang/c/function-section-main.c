/*
 * Fixture driver: call a function emitted into a custom text section.
 */
long nyc_section_function(void);
long nyc_parameter_section_function(unsigned long value);
int main(void) {
  return nyc_section_function() != 18 ||
         nyc_parameter_section_function(6) != 23;
}
