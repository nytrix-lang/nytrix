/*
 * Fixture driver: verify zero initialization of static pointer arrays.
 */
int nyc_static_pointer_arrays_are_zero(void);
int main(void) { return !nyc_static_pointer_arrays_are_zero(); }
