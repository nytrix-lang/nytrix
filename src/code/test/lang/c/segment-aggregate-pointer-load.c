struct record {
  unsigned long value;
  unsigned long decoy;
};

unsigned long read_segment_record(struct record **p) {
  struct record *record = *(__typeof__(*p) __seg_gs *)(unsigned long)p;
  return record->value;
}

void write_segment_record(struct record **p, struct record *value) {
  *(__typeof__(*p) __seg_gs *)(unsigned long)p = value;
}

struct record *assign_segment_record(struct record **p, struct record *value) {
  return (*(__typeof__(*p) __seg_gs *)(unsigned long)p = value);
}

void segment_mask(unsigned long *p, unsigned long set, unsigned long keep) {
  __asm__ volatile("orq %[val], %[var]"
                   : [var] "+m"(*(unsigned long __seg_gs *)(unsigned long)p)
                   : [val] "re"(set));
  __asm__ volatile("andq %[val], %[var]"
                   : [var] "+m"(*(unsigned long __seg_gs *)(unsigned long)p)
                   : [val] "re"(keep));
}

_Bool segment_decrement_is_zero(unsigned long *p) {
  _Bool zero;
  __asm__ volatile("decq %[var]"
                   : [var] "+m"(*(unsigned long __seg_gs *)(unsigned long)p),
                     "=@cce"(zero)
                   :
                   : "memory");
  return zero;
}

_Bool segment_try_replace(unsigned long *p, unsigned long *expected,
                          unsigned long replacement) {
  unsigned long old = *expected;
  _Bool matched;
  __asm__ volatile("cmpxchgq %[new], %[var]"
                   : "=@ccz"(matched), [old] "+a"(old),
                     [var] "+m"(*(unsigned long __seg_gs *)(unsigned long)p)
                   : [new] "r"(replacement)
                   : "memory");
  if (!matched)
    *expected = old;
  return matched;
}

unsigned short segment_post_decrement(unsigned short *p) {
  return (*(volatile unsigned short __seg_gs *)(unsigned long)p)--;
}

float segment_pre_increment(float *p) {
  return ++*(volatile float __seg_gs *)(unsigned long)p;
}

extern __seg_gs volatile unsigned named_word, named_word_end;
extern __seg_gs volatile unsigned short named_half;

unsigned segment_named_word(unsigned value) {
  unsigned initial = named_word;
  named_word = value;
  named_word += 3;
  unsigned next = ++named_word;
  return initial * 1000 + next + *(&named_word) + named_word_end;
}

unsigned short segment_named_half(unsigned short value) {
  named_half = value;
  return named_half++;
}
