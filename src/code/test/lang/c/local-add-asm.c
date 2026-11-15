/*
 * Fixture: update a nested local aggregate field with inline assembly.
 */
struct nyc_local_counter {
  struct {
    long counter;
  } a;
};
void nyc_local_add_probe(long delta, struct nyc_local_counter *value) {
  __asm__ volatile(" addq %1,%0" : "+m"(value->a.counter) : "ir"(delta));
}
void nyc_local_sub_probe(long delta, struct nyc_local_counter *value) {
  __asm__ volatile(" subq %1,%0" : "+m"(value->a.counter) : "ir"(delta));
}
