#include <stdio.h>

struct a64_pair {
  double x;
  double y;
};
struct a64_triple {
  double x;
  double y;
  double z;
};
struct a64_quad {
  double x;
  double y;
  double z;
  double w;
};
struct a64_five {
  double a;
  double b;
  double c;
  double d;
  double e;
};
struct a64_point {
  long x;
  long y;
};
struct a64_big {
  long x;
  long y;
  long z;
};
union a64_pair_union {
  struct a64_pair pair;
  double values[2];
};
union a64_mixed_union {
  double floating;
  long integer;
};

extern struct a64_pair c_a64_pair_scale(struct a64_pair, double);
extern struct a64_triple c_a64_triple_shift(struct a64_triple, double);
extern struct a64_point c_a64_point_add(struct a64_point, struct a64_point);
extern struct a64_big c_a64_big_shift(struct a64_big, long);
extern struct a64_pair c_a64_call_pair_scale(struct a64_pair);
extern struct a64_triple c_a64_call_triple_shift(struct a64_triple);
extern struct a64_quad c_a64_call_quad_scale(struct a64_quad);
extern struct a64_five c_a64_call_five_identity(struct a64_five);
extern struct a64_point c_a64_call_point_add(struct a64_point,
                                             struct a64_point);
extern struct a64_big c_a64_call_big_shift(struct a64_big);
extern union a64_pair_union c_a64_union_identity(union a64_pair_union);
extern union a64_pair_union c_a64_call_union_identity(union a64_pair_union);
extern union a64_mixed_union
    c_a64_call_mixed_union_identity(union a64_mixed_union);

static int same_double(double left, double right) {
  return left > right - 0.000001 && left < right + 0.000001;
}

int main(void) {
  struct a64_pair pair = c_a64_call_pair_scale((struct a64_pair){2.0, 3.5});
  struct a64_triple triple =
      c_a64_call_triple_shift((struct a64_triple){1.0, 2.0, 3.0});
  struct a64_quad quad =
      c_a64_call_quad_scale((struct a64_quad){1.0, 2.0, 3.0, 4.0});
  struct a64_five five =
      c_a64_call_five_identity((struct a64_five){5.0, 10.0, 15.0, 20.0, 25.0});
  struct a64_point point =
      c_a64_call_point_add((struct a64_point){4, 7}, (struct a64_point){8, 9});
  struct a64_big big = c_a64_call_big_shift((struct a64_big){10, 20, 30});
  union a64_pair_union direct_union =
      c_a64_union_identity((union a64_pair_union){.pair = {6.0, 8.0}});
  union a64_pair_union pair_union =
      c_a64_call_union_identity((union a64_pair_union){.pair = {6.0, 8.0}});
  union a64_mixed_union mixed_union =
      c_a64_call_mixed_union_identity((union a64_mixed_union){.integer = 29});
  if (!same_double(pair.x, 4.0) || !same_double(pair.y, 7.0) ||
      !same_double(triple.x, 1.5) || !same_double(triple.y, 2.5) ||
      !same_double(triple.z, 3.5) || point.x != 12 || point.y != 16 ||
      !same_double(quad.x, 2.0) || !same_double(quad.y, 4.0) ||
      !same_double(quad.z, 6.0) || !same_double(quad.w, 8.0) ||
      !same_double(five.a, 5.0) || !same_double(five.b, 10.0) ||
      !same_double(five.c, 15.0) || !same_double(five.d, 20.0) ||
      !same_double(five.e, 25.0) || big.x != 15 || big.y != 25 || big.z != 35 ||
      !same_double(direct_union.pair.x, 6.0) ||
      !same_double(direct_union.pair.y, 8.0) ||
      !same_double(pair_union.pair.x, 6.0) ||
      !same_double(pair_union.pair.y, 8.0) || mixed_union.integer != 29) {
    fprintf(stderr,
            "AArch64 C aggregate ABI mismatch: pair=(%.3f,%.3f) "
            "triple=(%.3f,%.3f,%.3f) point=(%ld,%ld) big=(%ld,%ld,%ld) "
            "quad=(%.3f,%.3f,%.3f,%.3f) "
            "five=(%.3f,%.3f,%.3f,%.3f,%.3f) union=(%.3f,%.3f) "
            "direct-union=(%.3f,%.3f) mixed=%ld\n",
            pair.x, pair.y, triple.x, triple.y, triple.z, point.x, point.y,
            big.x, big.y, big.z, quad.x, quad.y, quad.z, quad.w, five.a, five.b,
            five.c, five.d, five.e, pair_union.pair.x, pair_union.pair.y,
            direct_union.pair.x, direct_union.pair.y, mixed_union.integer);
    return 1;
  }
  puts("AArch64 C aggregate ABI parity: ok");
  return 0;
}
