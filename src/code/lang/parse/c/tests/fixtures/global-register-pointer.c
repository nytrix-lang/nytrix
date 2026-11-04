register void *stack asm("rsp");
int nyc_register_pointer_support(void) { typeof(stack) value = stack; __typeof__(stack) copy = value; return copy != 0; }
