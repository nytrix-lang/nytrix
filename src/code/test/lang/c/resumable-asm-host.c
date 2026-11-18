/*
 * Host-side C counterpart for the resumable asm test.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdlib.h>
#include <ucontext.h>

volatile int trap_visits;
static volatile sig_atomic_t traps;

static void resume_trap(int signal, siginfo_t *info, void *context) {
  (void)info;
  if (signal != SIGILL || ++traps > 3)
    _Exit(30);
  ((ucontext_t *)context)->uc_mcontext.gregs[REG_RIP] += 2;
}

extern int resume_plain_trap(int);
#ifndef NYC_SIMPLE_TRAPS
extern int resume_labelled_trap(int);
#endif
extern int resume_conditional_trap(int);

int main(void) {
  struct sigaction action = {0};
  action.sa_sigaction = resume_trap;
  action.sa_flags = SA_SIGINFO;
  if (sigemptyset(&action.sa_mask) || sigaction(SIGILL, &action, 0))
    return 1;
  if (resume_plain_trap(7) != 12 || traps != 1 || trap_visits != 1)
    return 2;
#ifndef NYC_SIMPLE_TRAPS
  if (resume_labelled_trap(9) != 18 || traps != 2 || trap_visits != 2)
    return 3;
#endif
  sig_atomic_t before_conditional = traps;
  if (resume_conditional_trap(0) != 9 || traps != before_conditional)
    return 4;
  if (resume_conditional_trap(1) != 10 || traps != before_conditional + 1)
    return 5;
  return 0;
}
