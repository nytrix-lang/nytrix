register unsigned long current_stack_pointer asm("rsp");
int main(void) {
  unsigned long stack_pointer = current_stack_pointer;
  return !stack_pointer || (stack_pointer & 7) != 0;
}
