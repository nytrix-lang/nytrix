/*
 * Aggregate ABI coverage: struct parameters by value (register-class,
 * MEMORY-class, and mixed INTEGER/SSE), aggregate returns (two-eightbyte
 * register classes and hidden-pointer MEMORY), struct locals with member
 * stores, and scalar/aggregate arguments interleaved past the six-register
 * boundary. Every function has a host-compiled reference twin in
 * reference.c.
 */

struct c_point {
  long x;
  long y;
};

struct c_big {
  long a;
  long b;
  long c;
  long d;
};

struct c_pair {
  double first;
  double second;
};

struct c_mixed {
  long tag;
  double value;
};

long c_point_sum(struct c_point p) { return p.x + p.y; }

long c_point_sum_reference(struct c_point p) { return p.x + p.y; }

struct c_point c_point_add(struct c_point a, struct c_point b) {
  struct c_point r;
  r.x = a.x + b.x;
  r.y = a.y + b.y;
  return r;
}

struct c_point c_point_add_reference(struct c_point a, struct c_point b) {
  struct c_point r;
  r.x = a.x + b.x;
  r.y = a.y + b.y;
  return r;
}

long c_big_sum(struct c_big b) { return b.a + b.b + b.c + b.d; }

long c_big_sum_reference(struct c_big b) { return b.a + b.b + b.c + b.d; }

struct c_big c_big_make(long seed) {
  struct c_big b;
  b.a = seed;
  b.b = seed + 1;
  b.c = seed + 2;
  b.d = seed + 3;
  return b;
}

struct c_big c_big_make_reference(long seed) {
  struct c_big b;
  b.a = seed;
  b.b = seed + 1;
  b.c = seed + 2;
  b.d = seed + 3;
  return b;
}

double c_pair_second(struct c_pair p) {
  struct c_pair copy = p;
  return copy.second;
}

double c_pair_second_reference(struct c_pair p) {
  struct c_pair copy = p;
  return copy.second;
}

struct c_pair c_pair_make(double base) {
  struct c_pair p;
  p.first = base;
  p.second = base * 2.0;
  return p;
}

struct c_pair c_pair_make_reference(double base) {
  struct c_pair p;
  p.first = base;
  p.second = base * 2.0;
  return p;
}

long c_mixed_blend(struct c_mixed m, long scale) { return m.tag * scale; }

long c_mixed_blend_reference(struct c_mixed m, long scale) {
  return m.tag * scale;
}

long c_many_with_struct(long a, long b, long c, long d, long e,
                        struct c_point f, long g) {
  return a + b + c + d + e + f.x + f.y + g;
}

long c_many_with_struct_reference(long a, long b, long c, long d, long e,
                                  struct c_point f, long g) {
  return a + b + c + d + e + f.x + f.y + g;
}

long c_struct_array_roundtrip(long seed) {
  struct c_point points[3];
  long i = 0;
  while (i < 3) {
    points[i].x = seed + i;
    points[i].y = seed - i;
    i += 1;
  }
  struct c_point total;
  total.x = 0;
  total.y = 0;
  i = 0;
  while (i < 3) {
    total = points[i];
    i += 1;
  }
  return total.x * 10 + total.y;
}

long c_struct_array_roundtrip_reference(long seed) {
  struct c_point points[3];
  long i = 0;
  while (i < 3) {
    points[i].x = seed + i;
    points[i].y = seed - i;
    i += 1;
  }
  struct c_point total;
  total.x = 0;
  total.y = 0;
  i = 0;
  while (i < 3) {
    total = points[i];
    i += 1;
  }
  return total.x * 10 + total.y;
}
