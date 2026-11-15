/*
 * Fixture helper: exchange values using read-write inline-assembly operands.
 */
extern int asm_readwrite_memory_value;
int exchange_asm_memory(int input) {
  int previous = input;
  __asm__ volatile("lock xaddl %0, %1"
                   : "+r"(previous), "+m"(asm_readwrite_memory_value)
                   :
                   : "memory", "cc");
  return previous;
}
extern unsigned long asm_readwrite_memory_value_q;
unsigned long exchange_asm_memory_q(unsigned long input) {
  unsigned long previous = input;
  __asm__ volatile("lock xaddq %q0, %1"
                   : "+r"(previous), "+m"(asm_readwrite_memory_value_q)
                   :
                   : "memory", "cc");
  return previous;
}
extern unsigned char asm_readwrite_memory_value_b;
unsigned char exchange_asm_memory_b(unsigned char input) {
  unsigned char previous = input;
  __asm__ volatile("lock xaddb %b0, %1"
                   : "+q"(previous), "+m"(asm_readwrite_memory_value_b)
                   :
                   : "memory", "cc");
  return previous;
}
extern signed char asm_readwrite_memory_value_bs;
signed char exchange_asm_memory_bs(signed char input) {
  signed char previous = input;
  __asm__ volatile("lock xaddb %b0, %1"
                   : "+q"(previous), "+m"(asm_readwrite_memory_value_bs)
                   :
                   : "memory", "cc");
  return previous;
}
extern unsigned long asm_xchg_memory_value_q;
unsigned long exchange_xchg_memory_q(unsigned long input) {
  unsigned long previous = input;
  __asm__ volatile(""
                   "xchg"
                   "q %q0, %1"
                   : "+r"(previous), "+m"(asm_xchg_memory_value_q)
                   :
                   : "memory", "cc");
  return previous;
}
unsigned long exchange_xchg_pointer_q(unsigned long input,
                                      unsigned long *pointer) {
  unsigned long previous = input;
  __asm__ volatile(""
                   "xchg"
                   "q %q0, %1"
                   : "+r"(previous), "+m"(*pointer)
                   :
                   : "memory", "cc");
  return previous;
}
void *exchange_xchg_object_pointer(void *input, void **pointer) {
  void *previous = input;
  __asm__ volatile(""
                   "xchg"
                   "q %q0, %1"
                   : "+r"(previous), "+m"(*pointer)
                   :
                   : "memory", "cc");
  return previous;
}
extern short asm_readwrite_memory_value_w;
short exchange_asm_memory_w(short input) {
  short previous = input;
  __asm__ volatile("lock xaddw %w0, %1"
                   : "+r"(previous), "+m"(asm_readwrite_memory_value_w)
                   :
                   : "memory", "cc");
  return previous;
}
