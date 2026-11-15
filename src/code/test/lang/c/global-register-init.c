/*
 * Negative fixture for initialization of a global register variable.
 */
register unsigned long cur asm("rsp") = 0;
