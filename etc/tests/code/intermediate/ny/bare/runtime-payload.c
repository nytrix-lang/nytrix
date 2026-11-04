/*
 * Exercise the actual checked-in machine code, including relocation away from
 * page boundaries. Host: cc -O2 -Isrc runtime-payload.c -o payload-test
 * A64: aarch64-linux-gnu-gcc with the same flags; qemu-aarch64 -L
 * /usr/aarch64-linux-gnu payload-test. Optional argv[1]: random float count.
 */
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#if defined(__x86_64__)
#include "code/backend/bare/x86_64.inc"
#define CODE ny_bare_x86_64_code
#define ENTRY ny_bare_x86_64_entry
#else
#include "code/backend/bare/aarch64.inc"
#define CODE ny_bare_aarch64_code
#define ENTRY ny_bare_aarch64_entry
#endif

typedef unsigned long U;
typedef U (*Entry)(U, U, U, U, U);
static Entry entry;
static U invoke(U op, U a, U b, U c, U d) { return entry(a, b, c, d, op); }
static void release(U value) {
  U *p = (U *)value - 4;
  assert(p[0] == 0x545249584e5954UL && p[3] == 120);
  assert(p[1] >= 4096 && !(p[1] & 4095));
  assert((p[2] & 1) && ((char *)value)[p[2] >> 1] == 0);
  assert(munmap(p, p[1]) == 0);
}
static void text(U value, const char *expected) {
  assert(value && strcmp((char *)value, expected) == 0);
  assert(((U *)value)[-2] == ((strlen(expected) << 1) | 1));
  release(value);
}
static void float_case(U bits) {
  double d;
  memcpy(&d, &bits, 8);
  char expected[32];
  snprintf(expected, sizeof(expected), "%g", d);
  U got = invoke(2, bits, 0, 0, 0);
  if (!got || strcmp((char *)got, expected)) {
    fprintf(stderr, "float %016lx: got %s, expected %s\n", bits,
            got ? (char *)got : "NULL", expected);
    abort();
  }
  release(got);
}
int main(int argc, char **argv) {
  U size = (sizeof(CODE) + 8191) & ~(U)4095;
  char *mapping =
      mmap(0, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(mapping != MAP_FAILED);
  /*
   * A64 code must work at any 4-byte boundary, not only linker page zero.
   */
  char *base = mapping + 124;
  memcpy(base, CODE, sizeof(CODE));
  __builtin___clear_cache(base, base + sizeof(CODE));
  assert(mprotect(mapping, size, PROT_READ | PROT_EXEC) == 0);
  entry = (Entry)(base + ENTRY);
  const long ints[] = {0, 1, -1, 2147483647L, -2147483647L, LONG_MIN, LONG_MAX};
  for (U i = 0; i < sizeof(ints) / sizeof(*ints); ++i) {
    char expected[32];
    snprintf(expected, sizeof(expected), "%ld", ints[i]);
    text(invoke(1, (U)ints[i], 0, 0, 0), expected);
  }
  const U edges[] = {0,
                     1UL << 63,
                     1,
                     (1UL << 63) | 1,
                     0x000fffffffffffffUL,
                     0x0010000000000000UL,
                     0x7fefffffffffffffUL,
                     0x7ff0000000000000UL,
                     0xfff0000000000000UL,
                     0x7ff8000000000001UL,
                     0xfff8000000000001UL,
                     0x3ff0000000000000UL};
  for (U i = 0; i < sizeof(edges) / sizeof(*edges); ++i) {
    float_case(edges[i]);
    assert(invoke(5, edges[i], 0, 0, 0) == edges[i]);
  }
  /*
   * Around decimal carry, exponent switches and half-even ties.
   */
  const double ties[] = {999999.5, 99999.95, .00009999995, .000009999995,
                         1.234565, 1.234575, 123456.5,     123457.5,
                         .0001,    1e6};
  for (U i = 0; i < sizeof(ties) / sizeof(*ties); ++i) {
    U bits;
    memcpy(&bits, ties + i, 8);
    for (long j = -8; j <= 8; ++j)
      float_case(bits + j);
  }
  U state = 0x123456789abcdefUL;
  U rounds = argc > 1 ? strtoul(argv[1], 0, 10) : 100000;
  for (U i = 0; i < rounds; ++i) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    float_case(state);
  }
  assert(!strcmp((char *)invoke(4, 0, 0, 0, 0), "nil"));
  assert(!strcmp((char *)invoke(4, 2, 0, 0, 0), "false"));
  assert(!strcmp((char *)invoke(4, 8, 0, 0, 0), "true"));
  assert(!strcmp((char *)invoke(4, 16, 0, 0, 0), "nil"));
  text(invoke(4, invoke(7, (U)-42, 0, 0, 0), 0, 0, 0), "-42");
  text(invoke(4, 0x1234, 0, 0, 0), "<fn 0x1234>");
  text(invoke(4, 0x1236, 0, 0, 0), "<ptr 0x1236>");
  U box = invoke(6, 0x400c000000000000UL, 0, 0, 0);
  assert((box & 15) == 8 && ((U *)box)[-1] == 110);
  text(invoke(4, box, 0, 0, 0), "3.5");
  assert(munmap((char *)box - 24, 4096) == 0);
  U str = invoke(1, 42, 0, 0, 0);
  assert(invoke(4, str, 0, 0, 0) == str);
  release(str);
  text(invoke(8, (U) "banana", (U) "na", (U) "XYZ", 0), "baXYZXYZ");
  text(invoke(8, (U) "aaaaa", (U) "aa", (U) "", 0), "a");
  text(invoke(8, (U) "abc", (U) "", (U) "xyz", 0), "abc");
  text(invoke(8, 0, 0, 0, 0), "");
  const char *utf = "a\xc3\xa9\xf0\x9f\x99\x82z";
  text(invoke(9, (U)utf, 1, 3, 0), "\xc3\xa9\xf0\x9f\x99\x82");
  text(invoke(9, (U)utf, (U)-3, (U)-1, 0), "\xc3\xa9\xf0\x9f\x99\x82");
  text(invoke(9, (U)utf, (U)LONG_MIN, LONG_MAX, 0), utf);
  text(invoke(9, (U)utf, 3, 1, 0), "");
  text(invoke(9, 0, 0, 1, 0), "");
  const char invalid[] = {'a', (char)0xe0, (char)0x80, (char)0x80, 'z', 0};
  char byte[] = {(char)0x80, 0};
  text(invoke(9, (U)invalid, 2, 3, 0), byte);
  U b = invoke(10, (U) "base", 0, 0, 0);
  assert(b && !invoke(11, 1, (U) "bad", 0, 0));
  assert(!invoke(12, b, (U) "bad", (U)-1, 0));
  assert(!invoke(13, b, (U) "bad", 3, (U)-1));
  assert(invoke(11, b, (U) "-", 0, 0) == b);
  assert(invoke(12, b, (U) "abc-tail", 3, 0) == b);
  assert(invoke(13, b, (U) "xy", 2, 5000) == b);
  U *fields = (U *)(b + 32);
  U old_len = fields[1];
  assert(old_len == 10008 && !memcmp((char *)fields[0], "base-abc", 8));
  char *snapshot = strdup((char *)fields[0]);
  /*
   * Self-append must preserve an interior suffix when reserve unmaps buf.
   */
  assert(invoke(11, b, fields[0] + 5, 0, 0) == b);
  fields = (U *)(b + 32);
  assert(fields[1] == old_len * 2 - 5);
  assert(!memcmp((char *)fields[0], snapshot, old_len));
  assert(!strcmp((char *)fields[0] + old_len, snapshot + 5));
  assert(invoke(13, b, (U) "x", LONG_MAX, 3) == b);
  U cap = fields[2];
  U buf = invoke(14, b, 0, 0, 0);
  assert(!strcmp((char *)buf + old_len, snapshot + 5));
  text(invoke(9, buf, 0, 4, 0), "base");
  assert(munmap((void *)buf, cap) == 0);
  free(snapshot);
  assert(!invoke(14, 0, 0, 0, 0));
  /*
   * Static array templates clone into independent, checked buffer headers.
   */
  U template[8] = {0x4e59544255464d47UL, 3, 8, 3, 10, 20, 30, 0};
  U cloned = invoke(15, (U)(template + 4), 0, 0, 0);
  assert(cloned && cloned != (U)(template + 4));
  U *clone_header = (U *)cloned - 4;
  assert(clone_header[0] == template[0] && clone_header[1] == 3 &&
         clone_header[2] == 8 && clone_header[3] == 3);
  assert(((U *)cloned)[0] == 10 && ((U *)cloned)[1] == 20 &&
         ((U *)cloned)[2] == 30);
  ((U *)cloned)[1] = 99;
  assert(template[5] == 20);
  assert(munmap((void *)((U *)cloned - 4), 4096) == 0);
  U empty_template[4] = {0x4e59544255464d47UL, 0, 8, 0};
  U empty_clone = invoke(15, (U)(empty_template + 4), 0, 0, 0);
  assert(empty_clone && ((U *)empty_clone)[-3] == 0);
  assert(munmap((void *)((U *)empty_clone - 4), 4096) == 0);
  U bad_template[4] = {0, 1, 8, 1};
  assert(!invoke(15, (U)(bad_template + 4), 0, 0, 0));
  bad_template[0] = 0x4e59544255464d47UL;
  bad_template[1] = LONG_MAX;
  assert(!invoke(15, (U)(bad_template + 4), 0, 0, 0));
  assert(invoke(16, 0, 0, 0, 0) == 0);
  assert(invoke(16, 61, 0, 0, 0) == 1);
  assert(invoke(16, 2, 0, 0, 0) == 3);
  U tagged_string = invoke(1, (U)"tagged", 0, 0, 0);
  assert(tagged_string && invoke(16, tagged_string, 0, 0, 0) == 120);
  release(tagged_string);
  /*
   * Float printing writes precisely the digits, returns zero.
   */
  int fds[2];
  assert(!pipe(fds));
  int saved = dup(1);
  assert(dup2(fds[1], 1) == 1);
  close(fds[1]);
  assert(!invoke(3, 0x400c000000000000UL, 0, 0, 0));
  assert(dup2(saved, 1) == 1);
  close(saved);
  char printed[8] = {0};
  assert(read(fds[0], printed, 8) == 3);
  assert(!strcmp(printed, "3.5"));
  close(fds[0]);
  assert(!munmap(mapping, size));
  printf("payload: %lu random floats and runtime edge cases passed\n", rounds);
  return 0;
}
