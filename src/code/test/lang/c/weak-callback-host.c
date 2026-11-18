/*
 * Host-side C counterpart for the weak callback test.
 */
#ifndef NYC_CALLBACK_OVERRIDE
#define NYC_CALLBACK_OVERRIDE 0
#endif

static int callback(int value) { return value + 11; }
static int typedef_callback(int value) { return value + 13; }
#if NYC_CALLBACK_OVERRIDE
int (*nyc_weak_callback)(int) = callback;
int (*nyc_weak_typedef_callback)(int) = typedef_callback;
#else
extern int (*nyc_weak_callback)(int);
extern int (*nyc_weak_typedef_callback)(int);
#endif
extern int nyc_callbacks_present(void);
extern int nyc_invoke_callback(int);
extern int nyc_invoke_typedef_callback(int);
extern void nyc_clear_callbacks(void);

int main(void) {
  if (nyc_callbacks_present() != NYC_CALLBACK_OVERRIDE)
    return 1;
  nyc_weak_callback = callback;
  nyc_weak_typedef_callback = typedef_callback;
  if (nyc_invoke_callback(7) != 18 || nyc_invoke_typedef_callback(7) != 20)
    return 2;
  nyc_clear_callbacks();
  if (nyc_weak_callback || nyc_weak_typedef_callback || nyc_callbacks_present())
    return 3;
  return 0;
}
