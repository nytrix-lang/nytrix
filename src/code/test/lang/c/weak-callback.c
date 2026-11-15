extern int (*nyc_weak_callback)(int);
int __attribute__((weak)) (*nyc_weak_callback)(int);
int (*nyc_weak_callback)(int);
extern int (*nyc_weak_callback)(int);

typedef int (*callback_t)(int);
extern callback_t nyc_weak_typedef_callback;
callback_t nyc_weak_typedef_callback __attribute__((weak));
callback_t nyc_weak_typedef_callback;
extern callback_t nyc_weak_typedef_callback;

int nyc_callbacks_present(void) {
  return nyc_weak_callback != 0 && nyc_weak_typedef_callback != 0;
}
int nyc_invoke_callback(int value) { return nyc_weak_callback(value); }
int nyc_invoke_typedef_callback(int value) {
  return nyc_weak_typedef_callback(value);
}
void nyc_clear_callbacks(void) {
  nyc_weak_callback = 0;
  nyc_weak_typedef_callback = 0;
}
