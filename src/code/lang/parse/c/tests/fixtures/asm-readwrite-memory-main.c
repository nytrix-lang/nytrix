/*
 * Fixture driver: verify read-write memory constraints in assembly helpers.
 */
int asm_readwrite_memory_value = 5;
int exchange_asm_memory(int input);
unsigned long asm_readwrite_memory_value_q = 5;
unsigned long exchange_asm_memory_q(unsigned long input);
unsigned char asm_readwrite_memory_value_b = 5;
unsigned char exchange_asm_memory_b(unsigned char input);
signed char asm_readwrite_memory_value_bs = -128;
signed char exchange_asm_memory_bs(signed char input);
short asm_readwrite_memory_value_w = -32768;
short exchange_asm_memory_w(short input);
unsigned long asm_xchg_memory_value_q = 5;
unsigned long exchange_xchg_memory_q(unsigned long input);
unsigned long exchange_xchg_pointer_q(unsigned long input,
                                      unsigned long *pointer);
void *xchg_object_pointer_value = (void *)5;
void *exchange_xchg_object_pointer(void *input, void **pointer);
int main(void) {
  return exchange_asm_memory(7) != 5 || asm_readwrite_memory_value != 12 ||
         exchange_asm_memory_q(7) != 5 || asm_readwrite_memory_value_q != 12 ||
         exchange_asm_memory_b(7) != 5 || asm_readwrite_memory_value_b != 12 ||
         exchange_asm_memory_bs(1) != -128 ||
         asm_readwrite_memory_value_bs != -127 ||
         exchange_xchg_memory_q(7) != 5 || asm_xchg_memory_value_q != 7 ||
         exchange_xchg_pointer_q(11, &asm_xchg_memory_value_q) != 7 ||
         asm_xchg_memory_value_q != 11 ||
         exchange_xchg_object_pointer((void *)7, &xchg_object_pointer_value) !=
             (void *)5 ||
         xchg_object_pointer_value != (void *)7 ||
         exchange_asm_memory_w(1) != -32768 ||
         asm_readwrite_memory_value_w != -32767;
}
