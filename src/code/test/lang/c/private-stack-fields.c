/*
 * Private fields survive unrelated calls; escaped and overlapping memory does
 * not.
 */
struct state {
  int value;
  unsigned char byte;
  unsigned short half;
  unsigned long wide;
};
extern void impossible_state(void);
static volatile int calls;
static __attribute__((noinline)) int tick(int *other) {
  ++calls;
  *other += 3;
  return *other;
}
static __attribute__((noinline)) void replace(struct state *s) {
  s->value = -17;
  s->byte = 251;
  s->half = 65530;
  s->wide = 0xfedcba9876543210UL;
}
static __attribute__((noinline)) int private(int value) {
  struct state s = {
      .value = value, .byte = 253, .half = 65531, .wide = 0x123456789abcdef0UL};
  int other = 4;
  if (s.value != 0) {
    tick(&other);
    if (s.value == 0)
      impossible_state();
  }
  return s.value == value && s.byte == 253 && s.half == 65531 &&
         s.wide == 0x123456789abcdef0UL && other == (value ? 7 : 4);
}
static __attribute__((noinline)) int escaped(void) {
  struct state s = {0};
  replace(&s);
  return s.value == -17 && s.byte == 251 && s.half == 65530 &&
         s.wide == 0xfedcba9876543210UL;
}
static __attribute__((noinline)) int overlapping(void) {
  union {
    unsigned long wide;
    unsigned char bytes[8];
  } u = {.wide = 0x12345678UL};
  u.bytes[1] = 0xab;
  return u.wide == 0x1234ab78UL;
}
int main(void) {
  return !private(0) || !private(-9) || !escaped() || !overlapping() ||
         calls != 1;
}
