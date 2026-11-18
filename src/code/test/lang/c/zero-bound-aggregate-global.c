/*
 * C front-end regression test: zero bound aggregate global.
 */
struct zero_bound_range {
  unsigned long start;
  unsigned long end;
};

extern struct zero_bound_range zero_bound_ranges[];
struct zero_bound_range zero_bound_ranges[0];

int main(void) { return sizeof(zero_bound_ranges) != 0; }
