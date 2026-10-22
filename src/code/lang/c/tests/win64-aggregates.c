struct win_point {
  long x;
  long y;
};

struct win_word {
  long value;
};

struct win_big {
  long a;
  long b;
  long c;
};

struct win_point c_win_point_add(struct win_point left,
                                 struct win_point right) {
  struct win_point result;
  result.x = left.x + right.x;
  result.y = left.y + right.y;
  return result;
}

struct win_word c_win_word_add(struct win_word left,
                               struct win_word right) {
  struct win_word result;
  result.value = left.value + right.value;
  return result;
}

struct win_big c_win_big_make(long seed) {
  struct win_big result;
  result.a = seed;
  result.b = seed + 1;
  result.c = seed + 2;
  return result;
}

long c_win_big_sum(struct win_big value) {
  return value.a + value.b + value.c;
}

long c_win_big_sum_after4(long a, long b, long c, long d,
                          struct win_big value) {
  return a + b + c + d + value.a + value.b + value.c;
}

long c_win_many_with_point(long a, long b, long c, long d,
                           struct win_point value, long tail) {
  return a + b + c + d + value.x + value.y + tail;
}
