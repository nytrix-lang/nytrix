/*
 * C fixture for reading a global register variable assigned to a non-stack
 * register.
 */
register unsigned long bound asm("rbx");
unsigned long nyc_non_rsp_register_read(void) { return bound; }
