register unsigned long bound asm("rbx");
unsigned long nyc_non_rsp_register_read(void) { return bound; }
