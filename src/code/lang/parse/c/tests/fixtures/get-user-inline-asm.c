int __get_user_8(unsigned long *pointer) { return *pointer != 0; }

int nyc_get_user_8_asm(unsigned long *pointer) {
  int result;
  unsigned long value;
  unsigned long stack = 0;
  __asm__ volatile("call __get_user_%c[size]"
                   : "=a"(result), "=r"(value), "+r"(stack)
                   : "0"(pointer), [size] "i"(sizeof(*pointer)));
  return result + (int)value;
}

int __get_user_nocheck_8(unsigned long *pointer) { return *pointer != 0; }

int nyc_get_user_nocheck_8_asm(unsigned long *pointer) {
  int result;
  unsigned long value;
  unsigned long stack = 0;
  __asm__ volatile("call __get_user_nocheck_%c[size]"
                   : "=a"(result), "=r"(value), "+r"(stack)
                   : "0"(pointer), [size] "i"(sizeof(*pointer)));
  return result + (int)value;
}
