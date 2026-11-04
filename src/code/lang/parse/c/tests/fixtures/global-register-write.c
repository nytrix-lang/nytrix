register unsigned long cur asm("rsp");
void nyc_register_write(void) { cur = 0; }
