#include <stdint.h>
#include <stdio.h>

struct win_point {
  int64_t x;
  int64_t y;
};

struct win_word {
  int64_t value;
};

struct win_big {
  int64_t a;
  int64_t b;
  int64_t c;
};

extern struct win_word c_win_word_add(struct win_word, struct win_word);
extern struct win_point c_win_point_add(struct win_point, struct win_point);
extern struct win_big c_win_big_make(int64_t);
extern int64_t c_win_big_sum(struct win_big);
extern int64_t c_win_big_sum_after4(int64_t, int64_t, int64_t, int64_t,
                                    struct win_big);
extern int64_t c_win_many_with_point(int64_t, int64_t, int64_t, int64_t,
                                     struct win_point, int64_t);

int main(void) {
  struct win_word word = c_win_word_add((struct win_word){.value = 12},
                                        (struct win_word){.value = 30});
  struct win_point point = c_win_point_add(
      (struct win_point){.x = 1, .y = 3},
      (struct win_point){.x = 5, .y = 7});
  struct win_big big = c_win_big_make(40);
  if (word.value != 42 || point.x != 6 || point.y != 10 || big.a != 40 ||
      big.b != 41 || big.c != 42 || c_win_big_sum(big) != 123 ||
      c_win_big_sum_after4(1, 2, 3, 4, big) != 133 ||
      c_win_many_with_point(1, 2, 3, 4, point, 5) != 31) {
    fprintf(stderr,
            "Win64 aggregate mismatch: word=%lld point=(%lld,%lld) "
            "big=(%lld,%lld,%lld) sums=(%lld,%lld) mixed=%lld\n",
            (long long)word.value, (long long)point.x, (long long)point.y,
            (long long)big.a, (long long)big.b, (long long)big.c,
            (long long)c_win_big_sum(big),
            (long long)c_win_big_sum_after4(1, 2, 3, 4, big),
            (long long)c_win_many_with_point(1, 2, 3, 4, point, 5));
    return 1;
  }
  return 0;
}
