#include <stdint.h>

static volatile uint32_t memory_word;

int main(void) {
  int ret = 0;
  int old = 0;
  int replacement = 2;

  __asm__ volatile("1: lock cmpxchgl %3, %2\n"
                   "2:\n"
                   ".pushsection __ex_table,\"aM\",@progbits,12\n"
                   ".balign 4\n"
                   ".long (1b) - .\n"
                   ".long (2b) - .\n"
                   ".long 0xfff21a11\n"
                   ".popsection\n"
                   : "+r"(ret), "=a"(old), "+m"(memory_word)
                   : "r"(replacement), "1"(old)
                   : "memory");
  if (ret || old != 0 || memory_word != 2)
    return 1;

  ret = 0;
  old = 17;
  replacement = 9;
  __asm__ volatile("1: lock cmpxchgl %3, %2\n"
                   "2:\n"
                   ".pushsection __ex_table,\"aM\",@progbits,12\n"
                   ".balign 4\n"
                   ".long (1b) - .\n"
                   ".long (2b) - .\n"
                   ".long 0xfff21a11\n"
                   ".popsection\n"
                   : "+r"(ret), "=a"(old), "+m"(memory_word)
                   : "r"(replacement), "1"(old)
                   : "memory");
  if (ret || old != 2 || memory_word != 2)
    return 2;

  int exchange = 5;
  old = 0;
  ret = 0;
  __asm__ volatile("1: xchgl %0, %2\n"
                   "2:\n"
                   ".pushsection __ex_table,\"aM\",@progbits,12\n"
                   ".balign 4\n"
                   ".long (1b) - .\n"
                   ".long (2b) - .\n"
                   ".long 0xfff21a11\n"
                   ".popsection\n"
                   : "=r"(old), "=r"(ret), "+m"(memory_word)
                   : "0"(exchange), "1"(0)
                   : "memory");
  if (ret || old != 2 || memory_word != 5)
    return 3;

  int addition = 7;
  old = 0;
  ret = 0;
  __asm__ volatile("1: lock xaddl %0, %2\n"
                   "2:\n"
                   ".pushsection __ex_table,\"aM\",@progbits,12\n"
                   ".balign 4\n"
                   ".long (1b) - .\n"
                   ".long (2b) - .\n"
                   ".long 0xfff21a11\n"
                   ".popsection\n"
                   : "=r"(old), "=r"(ret), "+m"(memory_word)
                   : "0"(addition), "1"(0));
  if (ret || old != 5 || memory_word != 12)
    return 4;

  int temporary = 0;
  int operand = 3;
  old = 0;
  ret = 0;
  __asm__ volatile("1: movl %2, %0\n"
                   "2: movl %0, %3\n"
                   "orl %4, %3\n"
                   "3: lock cmpxchgl %3, %2\n"
                   "jnz 2b\n"
                   "4:\n"
                   ".pushsection __ex_table,\"aM\",@progbits,12\n"
                   ".balign 4\n"
                   ".long (1b) - .\n"
                   ".long (4b) - .\n"
                   ".long 0xfff21a11\n"
                   ".long (3b) - .\n"
                   ".long (4b) - .\n"
                   ".long 0xfff21a11\n"
                   ".popsection\n"
                   : "=&a"(old), "=&r"(ret), "+m"(memory_word), "=&r"(temporary)
                   : "r"(operand), "1"(0));
  if (ret || old != 12 || memory_word != 15)
    return 5;

  operand = 10;
  old = 0;
  ret = 0;
  __asm__ volatile("1: movl %2, %0\n"
                   "2: movl %0, %3\n"
                   "andl %4, %3\n"
                   "3: lock cmpxchgl %3, %2\n"
                   "jnz 2b\n"
                   "4:\n"
                   ".pushsection __ex_table,\"aM\",@progbits,12\n"
                   ".balign 4\n"
                   ".long (1b) - .\n"
                   ".long (4b) - .\n"
                   ".long 0xfff21a11\n"
                   ".long (3b) - .\n"
                   ".long (4b) - .\n"
                   ".long 0xfff21a11\n"
                   ".popsection\n"
                   : "=&a"(old), "=&r"(ret), "+m"(memory_word), "=&r"(temporary)
                   : "r"(operand), "1"(0));
  if (ret || old != 15 || memory_word != 10)
    return 6;

  operand = 7;
  old = 0;
  ret = 0;
  __asm__ volatile("1: movl %2, %0\n"
                   "2: movl %0, %3\n"
                   "xorl %4, %3\n"
                   "3: lock cmpxchgl %3, %2\n"
                   "jnz 2b\n"
                   "4:\n"
                   ".pushsection __ex_table,\"aM\",@progbits,12\n"
                   ".balign 4\n"
                   ".long (1b) - .\n"
                   ".long (4b) - .\n"
                   ".long 0xfff21a11\n"
                   ".long (3b) - .\n"
                   ".long (4b) - .\n"
                   ".long 0xfff21a11\n"
                   ".popsection\n"
                   : "=&a"(old), "=&r"(ret), "+m"(memory_word), "=&r"(temporary)
                   : "r"(operand), "1"(0));
  return ret || old != 10 || memory_word != 13 ? 7 : 0;
}
