/*
 * An unreferenced static inline function with a section attribute is
 * discardable.
 */
extern void nyc_missing_inline_section_target(void);

static inline __attribute__((section(".init.text"))) void
nyc_unused_inline_section(void) {
  nyc_missing_inline_section_target();
}

int main(void) { return 0; }
