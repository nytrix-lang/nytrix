void *invalid_immediate(void *pointer) {
  asm("leaq %c1(%%rip), %0" : "=r"(pointer) : "i"(pointer));
  return pointer;
}
