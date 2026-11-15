/*
 * C fixture for taking the address of a global register variable.
 */
register unsigned long cur asm("rsp");
unsigned long *nyc_register_steal(void) { return &cur; }
