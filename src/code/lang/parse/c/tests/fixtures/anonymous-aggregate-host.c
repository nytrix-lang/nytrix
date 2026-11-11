/*
 * Host-side oracle for anonymous struct and union member layout.
 */
struct nyc_anonymous_box {
  long guard;
  union {
    struct {
      long correct;
      long incorrect;
    };
    struct {
      long miss;
      long hit;
    };
    long miss_hit[2];
  };
};
extern long nyc_anonymous_member(void *);
int main(void) {
  struct nyc_anonymous_box box = {.guard = 1};
  long result = nyc_anonymous_member(&box);
  return result != 42 || box.miss_hit[0] != 17 || box.miss_hit[1] != 25;
}
