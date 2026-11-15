extern volatile int trap_visits;

int resume_plain_trap(int value) {
  __asm__ volatile("ud2" ::: "memory");
  ++trap_visits;
  return value + 5;
}

int resume_labelled_trap(int value) {
  __asm__ volatile("9: nop\n1:\tud2\n\t.pushsection .discard.test,\"aw\"\n"
                   "\t.word 1\n\t.popsection" ::
                       : "memory");
  ++trap_visits;
  return value + 9;
}

int resume_conditional_trap(int condition) {
  if (condition)
    __asm__ volatile("ud2" ::: "memory");
  return condition + 9;
}
