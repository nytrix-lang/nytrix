/* Host driver for scoped-once-loops.c: result and cleanup count per exit. */
extern int ends;
int scope_normal(int *), scope_return(int *), scope_goto(int *);
int scope_break(int *), scope_continue(int *);
#define CHECK(call, value, count)                                              \
  do {                                                                         \
    ends = 0;                                                                  \
    if ((call) != (value) || ends != (count))                                  \
      return __LINE__;                                                         \
  } while (0)
int main(void) {
  int one = 1, five = 5;
  CHECK(scope_normal(&one), 2, 1);
  CHECK(scope_return(&one), 1, 1);
  CHECK(scope_return(&five), -14, 1);
  CHECK(scope_goto(&one), 1, 1);
  CHECK(scope_goto(&five), -14, 1);
  CHECK(scope_break(&one), 2, 1);
  CHECK(scope_break(&five), 1, 1);
  CHECK(scope_continue(&one), 2, 1);
  CHECK(scope_continue(&five), 1, 1);
  return 0;
}
