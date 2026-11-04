register unsigned long cur asm("rsp");
unsigned long nyc_register_support(void) { typeof(cur) value = cur; __typeof__(cur) copy = value; return copy & 15; }
