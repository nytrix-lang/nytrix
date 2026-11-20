/*
 * C frontend test fixture: segment aggregate pointer load.
 */
#include <asm/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

struct record {
  unsigned long value;
  unsigned long decoy;
};

extern unsigned long read_segment_record(struct record **p);
extern void write_segment_record(struct record **p, struct record *value);
extern struct record *assign_segment_record(struct record **p,
                                            struct record *value);
extern void segment_mask(unsigned long *p, unsigned long set,
                         unsigned long keep);
extern _Bool segment_decrement_is_zero(unsigned long *p);
extern _Bool segment_try_replace(unsigned long *p, unsigned long *expected,
                                 unsigned long replacement);
extern unsigned short segment_post_decrement(unsigned short *p);
extern float segment_pre_increment(float *p);
extern unsigned segment_named_word(unsigned value);
extern unsigned short segment_named_half(unsigned short value);

unsigned named_words[] = {19, 29, 23, 31, 37};
unsigned short named_halves[] = {41, 43, 47, 53, 59, 61};
__asm__(".globl named_word\n.set named_word, named_words\n"
        ".globl named_word_end\n.set named_word_end, named_words+4\n"
        ".globl named_half\n.set named_half, named_halves");

int main(void) {
  struct record original = {17, 180};
  struct record alternate = {29, 290};
  struct record replacement = {83, 830};
  struct record *slots[] = {&original, &alternate};
  unsigned long saved;
  if (syscall(SYS_arch_prctl, ARCH_GET_GS, &saved) != 0)
    return 2;
  if (syscall(SYS_arch_prctl, ARCH_SET_GS, sizeof(slots[0])) != 0)
    return 3;
  unsigned long value = read_segment_record(slots);
  write_segment_record(slots, &replacement);
  int good_store = slots[0] == &original && slots[1] == &replacement &&
                   read_segment_record(slots) == 83;
  struct record *assigned = assign_segment_record(slots, &alternate);
  int good_assignment =
      slots[0] == &original && slots[1] == &alternate && assigned == &alternate;
  unsigned long counts[] = {7, 4};
  segment_mask(counts, 2, 3);
  int good_mask = counts[0] == 7 && counts[1] == 2;
  _Bool first = segment_decrement_is_zero(counts);
  _Bool second = segment_decrement_is_zero(counts);
  int good_decrement = !first && second && counts[0] == 7 && counts[1] == 0;
  unsigned long expected = 7;
  _Bool mismatch = segment_try_replace(counts, &expected, 32);
  int good_mismatch =
      !mismatch && expected == 0 && counts[0] == 7 && counts[1] == 0;
  _Bool match = segment_try_replace(counts, &expected, 32);
  int good_match = match && expected == 0 && counts[0] == 7 && counts[1] == 32;
  unsigned short words[] = {37, 41, 43, 47, 0, 53};
  unsigned short prior = segment_post_decrement(words);
  int good_decrement_store =
      prior == 0 && words[0] == 37 && words[4] == 65535 && words[5] == 53;
  float floats[] = {1.f, 2.f, 4.5f, 8.f};
  float incremented = segment_pre_increment(floats);
  int good_increment_store = incremented == 5.5f && floats[0] == 1.f &&
                             floats[2] == 5.5f && floats[3] == 8.f;
  unsigned named_result = segment_named_word(61);
  unsigned short named_prior = segment_named_half(65535);
  int good_named_word = named_result == 23161 && named_words[0] == 19 &&
                        named_words[2] == 65 && named_words[3] == 31;
  int good_named_half = named_prior == 65535 && named_halves[0] == 41 &&
                        named_halves[4] == 0 && named_halves[5] == 61;
  if (syscall(SYS_arch_prctl, ARCH_SET_GS, saved) != 0)
    return 4;
  if (!good_decrement_store || !good_increment_store)
    return 5 + !good_decrement_store + 2 * !good_increment_store;
  if (!good_named_word || !good_named_half)
    return 8 + !good_named_word + 2 * !good_named_half;
  return value != 29 || !good_store || !good_assignment || !good_mask ||
         !good_decrement || !good_mismatch || !good_match;
}
