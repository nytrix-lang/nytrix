register unsigned long cur asm("rsp");
unsigned long *nyc_register_steal(void) { return &cur; }
