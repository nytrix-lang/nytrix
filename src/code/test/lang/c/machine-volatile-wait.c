/*
 * Volatile objects change behind the compiler's back. If the reads in these
 * loops are merged or hoisted, the program never observes the signal handler
 * and the test harness times it out.
 */
#include <signal.h>
#include <stdio.h>
#include <sys/time.h>

static volatile int flag;
static volatile int counter;

struct box {
  volatile int field;
};
static struct box shared;

static void on_timer(int sig) {
  (void)sig;
  flag = 1;
  shared.field = 1;
  ++counter;
}

static int wait_global(void) {
  int spins = 0;
  while (!flag)
    ++spins;
  return spins >= 0;
}

static int wait_pointee(volatile int *p, int start) {
  while (*p == start)
    ;
  return 1;
}

static int wait_field(struct box *b) {
  while (!b->field)
    ;
  return 1;
}

int main(void) {
  signal(SIGALRM, on_timer);
  struct itimerval t = {{0, 2000}, {0, 2000}};
  setitimer(ITIMER_REAL, &t, 0);
  int ok = wait_global();
  ok &= wait_pointee(&counter, counter);
  ok &= wait_field(&shared);
  printf("checksum=%d\n", ok);
  return ok ? 0 : 1;
}
