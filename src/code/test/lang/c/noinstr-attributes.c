/*
 * C parser fixture for no-instrumentation function attributes and sections.
 */
int __attribute__((__noinline__)) __attribute__((__no_instrument_function__))
__attribute__((__section__(".text.nyc_noinstr"))) __attribute__((__no_kcsan__))
__attribute__((__no_sanitize_address__)) __attribute__((__no_profile__))
__attribute__((__no_sanitize_coverage__))
__attribute__((__no_sanitize_memory__))
nyc_noinstr_function(void) {
  return 43;
}
int main(void) { return nyc_noinstr_function() != 43; }
