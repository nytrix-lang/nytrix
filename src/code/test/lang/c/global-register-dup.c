/*
 * Negative fixture for duplicate global register-variable declarations.
 */
register unsigned long cur asm("rsp");
register unsigned long cur asm("rax");
