double apply_binary_callback(double (*callback)(double, double),
                             double left, double right);
void *apply_pointer_callback(void *(*callback)(void *, long),
                             void *pointer, long offset);
