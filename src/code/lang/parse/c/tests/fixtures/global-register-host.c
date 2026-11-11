/*
 * Host-side harness for reading a global register-variable fixture.
 */
unsigned long nyc_register_support(void);
int main(void) { return nyc_register_support() != 0; }
