/*
 * C fixture for assigning to a global register variable.
 */
register unsigned long cur asm("rsp");
void nyc_register_write(void) { cur = 0; }
