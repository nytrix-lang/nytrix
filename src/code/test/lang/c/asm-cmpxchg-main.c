/*
 * Runtime harness for compare-and-exchange inline assembly fixtures.
 */
int cmpxchg_asm(int *, int, int);
unsigned long cmpxchg_u32_asm(unsigned int *, unsigned int, unsigned int);
void *cmpxchg_pointer_asm(void **, void *, void *);
unsigned long long cmpxchg_qword_classic_asm(unsigned long long *,
                                             unsigned long long,
                                             unsigned long long);
struct cmpxchg_object {
  unsigned long value;
};
struct cmpxchg_object *
cmpxchg_pointer_null_expected_asm(struct cmpxchg_object **,
                                  struct cmpxchg_object *);
int main(void) {
  static int first, second, third;
  int value = 5;
  if (cmpxchg_asm(&value, 5, 9) != 5 || value != 9)
    return 1;
  if (cmpxchg_asm(&value, 7, 11) != 1009 || value != 9)
    return 2;
  unsigned int wide = 0xf0000000U;
  if (cmpxchg_u32_asm(&wide, 0xf0000000U, 0xf0000001U) != 0xf0000000UL ||
      wide != 0xf0000001U)
    return 3;
  if (cmpxchg_u32_asm(&wide, 0x80000000U, 7U) != 0x1f0000001UL ||
      wide != 0xf0000001U)
    return 4;
  void *pointer = &first;
  if (cmpxchg_pointer_asm(&pointer, &first, &second) != &first ||
      pointer != &second)
    return 5;
  if ((unsigned long)cmpxchg_pointer_asm(&pointer, &first, &third) !=
          (unsigned long)&second + 1 ||
      pointer != &second)
    return 6;
  unsigned long long qword = 0x123456789abcdef0ULL;
  if (cmpxchg_qword_classic_asm(&qword, 0x123456789abcdef0ULL,
                                0xfedcba9876543210ULL) !=
          0x123456789abcdef0ULL ||
      qword != 0xfedcba9876543210ULL)
    return 7;
  if (cmpxchg_qword_classic_asm(&qword, 0x123456789abcdef0ULL, 1) !=
          0xfedcba9876543210ULL ||
      qword != 0xfedcba9876543210ULL)
    return 8;
  struct cmpxchg_object *object = 0;
  struct cmpxchg_object replacement_object = {13};
  if (cmpxchg_pointer_null_expected_asm(&object, &replacement_object) != 0 ||
      object != &replacement_object)
    return 9;
  return 0;
}
