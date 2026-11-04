register unsigned long current_stack_pointer asm("rsp");
extern void nyc_register_constraint_callee(void);
int nyc_register_call_constraint(void) {
  asm volatile("call nyc_register_constraint_callee" : "+r"(current_stack_pointer));
  return 17;
}
