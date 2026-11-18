/*
 * C front-end regression test: resumable asm simple.
 */
extern volatile int trap_visits;

int resume_plain_trap(int value) {
  __asm__ volatile("ud2" ::: "memory");
  ++trap_visits;
  return value + 5;
}

int resume_conditional_trap(int condition) {
  if (condition)
    __asm__ volatile("ud2" ::: "memory");
  return condition + 9;
}
