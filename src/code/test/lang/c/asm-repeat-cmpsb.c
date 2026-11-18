/*
 * C front-end regression test: asm repeat cmpsb.
 */
typedef __SIZE_TYPE__ usize;

__attribute__((noinline)) int compare_bytes(unsigned char **left,
                                            unsigned char **right,
                                            usize *remaining, int backward) {
  unsigned char *a = *left;
  unsigned char *b = *right;
  usize count = *remaining;
  int different;
  if (backward)
    __asm__ volatile("std");
  __asm__ volatile("test %3, %3\n\trepe cmpsb"
                   : "=@ccnz"(different), "+D"(a), "+S"(b), "+c"(count)
                   :
                   : "cc", "memory");
  if (backward)
    __asm__ volatile("cld");
  *left = a;
  *right = b;
  *remaining = count;
  return different;
}

int main(void) {
  unsigned char first[4] = {1, 2, 3, 4};
  unsigned char equal[4] = {1, 2, 3, 4};
  unsigned char unequal[4] = {1, 2, 9, 4};
  unsigned char *a = 0, *b = 0;
  usize count = 0;
  if (compare_bytes(&a, &b, &count, 0) || a || b || count)
    return 1;
  if (compare_bytes(&a, &b, &count, 1) || a || b || count)
    return 5;
  a = first;
  b = equal;
  count = 4;
  if (compare_bytes(&a, &b, &count, 0) || a != first + 4 || b != equal + 4 ||
      count)
    return 2;
  a = first;
  b = unequal;
  count = 4;
  if (!compare_bytes(&a, &b, &count, 0) || a != first + 3 || b != unequal + 3 ||
      count != 1)
    return 3;
  a = first + 3;
  b = unequal + 3;
  count = 4;
  if (!compare_bytes(&a, &b, &count, 1) || a != first + 1 || b != unequal + 1 ||
      count != 2)
    return 4;
  a = first;
  b = unequal;
  count = 3;
  if (!compare_bytes(&a, &b, &count, 0) || a != first + 3 || b != unequal + 3 ||
      count)
    return 6;
  a = first + 3;
  b = equal + 3;
  count = 3;
  if (compare_bytes(&a, &b, &count, 1) || a != first || b != equal || count)
    return 7;
  return 0;
}
