/* Host driver for jump-thread-implied-branch.c. */
int implied(long);
static int pass_check;
int check(long p) { return p != 0 ? pass_check : 0; }
int work(void) { return 7; }
int main(void) {
  pass_check = 1;
  if (implied(1) != 7 || implied(0) != -14)
    return 1;
  pass_check = 0;
  return implied(1) != -14;
}
