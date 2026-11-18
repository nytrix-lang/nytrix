/*
 * C front-end regression test: noreturn call.
 */
static void c_noreturn_stop(void) __attribute__((noreturn));

static void c_noreturn_stop(void) {
  for (;;) {
  }
}

static int c_noreturn_branch(int stop) {
  if (stop)
    c_noreturn_stop();
  return 37;
}

int main(void) { return c_noreturn_branch(0) == 37 ? 0 : 1; }
