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

struct a64_pair c_a64_pair_scale(struct a64_pair value, double scale) {
  struct a64_pair result;
  result.x = value.x * scale;
  result.y = value.y * scale;
  return result;
}

struct a64_triple c_a64_triple_shift(struct a64_triple value, double shift) {
  struct a64_triple result;
  result.x = value.x + shift;
  result.y = value.y + shift;
  result.z = value.z + shift;
  return result;
}

struct a64_point c_a64_point_add(struct a64_point left,
                                 struct a64_point right) {
  struct a64_point result;
  result.x = left.x + right.x;
  result.y = left.y + right.y;
  return result;
}

struct a64_big c_a64_big_shift(struct a64_big value, long shift) {
  struct a64_big result;
  result.x = value.x + shift;
  result.y = value.y + shift;
  result.z = value.z + shift;
  return result;
}

/*
 * These wrappers exercise the C frontend's AAPCS64 call-site lowering as
 * well as its function-entry and return lowering.
 */
struct a64_pair c_a64_call_pair_scale(struct a64_pair value) {
  return c_a64_pair_scale(value, 2.0);
}

struct a64_triple c_a64_call_triple_shift(struct a64_triple value) {
  return c_a64_triple_shift(value, 0.5);
}

struct a64_quad c_a64_quad_scale(struct a64_quad value, double scale) {
  struct a64_quad result;
  result.x = value.x * scale;
  result.y = value.y * scale;
  result.z = value.z * scale;
  result.w = value.w * scale;
  return result;
}

struct a64_quad c_a64_call_quad_scale(struct a64_quad value) {
  return c_a64_quad_scale(value, 2.0);
}

struct a64_five c_a64_call_five_identity(struct a64_five value) {
  return value;
}

struct a64_point c_a64_call_point_add(struct a64_point left,
                                      struct a64_point right) {
  return c_a64_point_add(left, right);
}

struct a64_big c_a64_call_big_shift(struct a64_big value) {
  return c_a64_big_shift(value, 5);
}

union a64_pair_union c_a64_union_identity(union a64_pair_union value) {
  return value;
}

union a64_pair_union c_a64_call_union_identity(union a64_pair_union value) {
  return c_a64_union_identity(value);
}

union a64_mixed_union
c_a64_call_mixed_union_identity(union a64_mixed_union value) {
  return value;
}
