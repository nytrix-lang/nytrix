/*
 * C frontend test fixture: loop entry range.
 */
struct time_pair {
  long seconds, nanoseconds;
};

__attribute__((noinline)) void normalize_time(struct time_pair *t, long seconds,
                                              long nanoseconds) {
  while (nanoseconds >= 1000000000L) {
    __asm__("" : "+rm"(nanoseconds));
    nanoseconds -= 1000000000L;
    ++seconds;
  }
  while (nanoseconds < 0) {
    __asm__("" : "+rm"(nanoseconds));
    nanoseconds += 1000000000L;
    --seconds;
  }
  t->seconds = seconds;
  t->nanoseconds = nanoseconds;
}

int main(void) {
  struct time_pair t;
  normalize_time(&t, 7, 2000000004L);
  if (t.seconds != 9 || t.nanoseconds != 4)
    return 1;
  normalize_time(&t, 7, -1000000004L);
  if (t.seconds != 5 || t.nanoseconds != 999999996L)
    return 2;
  normalize_time(&t, -7, 0);
  return t.seconds != -7 || t.nanoseconds != 0;
}
