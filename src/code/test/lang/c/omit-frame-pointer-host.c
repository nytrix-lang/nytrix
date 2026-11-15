/*
 * Fixture driver: check calls to a multi-argument function without a frame
 * pointer.
 */
long omit_fp_call(long);
int main(void) { return omit_fp_call(3) != 91; }
