/*
 * Verify inline asm IRQ stack switching preserves register boundaries and
 * stack pointer restoration across helper calls.
 */
register unsigned long current_stack_pointer __asm__("rsp");

extern void handler(void *);

#define SWITCH_STACK(stack, argument, before, after)                           \
  do {                                                                         \
    register void *tos __asm__("r11") = (stack);                               \
    __asm__ volatile("movq\t%%rsp, (%[tos])\n"                                 \
                     "movq\t%[tos], %%rsp\n" before                            \
                     "movq\t%[arg1], %%rdi\n"                                  \
                     "1: call %c[__func]\n" after                              \
                     "popq\t%%rsp\n"                                           \
                     : "+r"(tos), "+r"(current_stack_pointer)                  \
                     : [__func] "i"(handler), [tos] "r"(tos),                  \
                       [arg1] "r"(argument)                                    \
                     : "cc", "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9",    \
                       "r10", "memory");                                       \
  } while (0)

void on_irq_stack(void *stack, void *argument) {
  SWITCH_STACK(stack, argument, "call irq_enter_rcu\n", "call irq_exit_rcu\n");
}

void on_plain_stack(void *stack, void *argument) {
  SWITCH_STACK(stack, argument, "", "");
}
