struct nyc_anonymous_box {
  long guard;
  union {
    struct { long correct; long incorrect; };
    struct { long miss; long hit; };
    long miss_hit[2];
  };
};
long nyc_anonymous_member(void *opaque) {
  struct nyc_anonymous_box *box = opaque;
  box->miss = 17;
  box->hit = 25;
  return box->correct + box->incorrect;
}
