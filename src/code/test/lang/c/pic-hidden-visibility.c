/*
 * Verify GCC visibility pragmas keep hidden symbols PC-relative and
 * interposable default-visibility symbols GOT-addressed under PIC.
 */
#pragma GCC visibility push(hidden)
extern int startup_data;
extern int dead_data;
extern int startup_value(void);

#pragma GCC visibility push(default)
extern int interposable_data;
#pragma GCC visibility pop

static inline __attribute__((always_inline)) int *local_startup_address(void) {
  return &startup_data;
}

int *startup_address(void) {
  int use_dead_data = 0;
  if (use_dead_data)
    return &dead_data;
  return local_startup_address();
}

int call_startup_value(void) { return startup_value(); }
#pragma GCC visibility pop

int *interposable_address(void) { return &interposable_data; }
