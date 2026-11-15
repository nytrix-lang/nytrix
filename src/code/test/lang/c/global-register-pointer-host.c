/*
 * Host-side harness for pointer-typed global register-variable support.
 */
int nyc_register_pointer_support(void);
int main(void) { return nyc_register_pointer_support() != 1; }
