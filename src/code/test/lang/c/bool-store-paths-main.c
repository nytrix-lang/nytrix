/* Host driver for bool-store-paths.c: every _Bool store path yields 0 or 1. */
#include <stdbool.h>
struct holder { bool flag; int pad; };
int st_ptr(int), st_param_ptr(bool *, int), st_param_index(bool *, int);
int st_global(int), st_global_array(int, int), st_local_array(int);
int st_member(struct holder *, int), st_increment(void), st_static_init(void);
int st_global_stdbool(int), st_global_ptr(bool *, int), st_param_value(bool);
int st_compound(bool *, int), st_compound_add(bool *, int);
struct bool_ops {
  bool (*take)(bool);
  int (*mix)(int, bool, long);
};
int st_indirect_field(struct bool_ops *, int), st_indirect_mix(struct bool_ops *, int);
int st_indirect_param(bool (*)(bool), int), st_indirect_local(int);
/* The callees read the argument register as a GCC-built function would. */
static bool take_bool(bool b) { return b; }
static int mix_bool(int a, bool b, long c) { return a + b * 100 + (int)c; }
int main(void) {
  bool cell[2] = {false, false};
  struct holder h = {false, 0};
  int ok = st_ptr(0x200) == 1 && st_param_ptr(&cell[0], 0x100) == 1 &&
           st_param_index(cell, 0x400) == 1 && st_global(0x200) == 1 &&
           st_global_array(1, 0x200) == 1 && st_local_array(0x200) == 1 &&
           st_member(&h, 0x200) == 1 && st_increment() == 1 &&
           st_static_init() == 1 && st_global_stdbool(0x200) == 1 &&
           st_global_ptr(&cell[1], 0x100) == 1 && st_param_value(1) == 1 &&
           st_compound(&cell[0], 0x200) == 1 && st_compound_add(&cell[1], 0x100) == 1;
  struct bool_ops ops = {take_bool, mix_bool};
  ok = ok && st_indirect_field(&ops, 0x200) == 1 && st_indirect_mix(&ops, 0x300) == 7 + 100 + 9 &&
       st_indirect_mix(&ops, 0) == 7 + 9 &&
       st_indirect_param(take_bool, 0x200) == 1 && st_indirect_local(0x400) == 1;
  /* A zero value must still store 0. */
  ok = ok && st_ptr(0) == 0 && st_global(0) == 0 && st_member(&h, 0) == 0;
  return !ok;
}
