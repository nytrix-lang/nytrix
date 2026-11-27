/*
 * Every store of a non-zero integer to a _Bool must produce exactly 1.
 */
#include <stdbool.h>
/*
 * Every store of a non-zero integer to a _Bool must produce exactly 1.
 */
bool gb;
bool garr[4];
struct holder { bool flag; int pad; };
int st_ptr(int v) { bool b = false; bool *p = &b; *p = v; return b; }
int st_param_ptr(bool *p, int v) { *p = v; return *p; }
int st_param_index(bool *p, int v) { p[1] = v; return p[1]; }
int st_global(int v) { gb = v; return gb; }
int st_global_array(int i, int v) { garr[i] = v; return garr[i]; }
int st_local_array(int v) { bool a[3] = {0, 0, 0}; a[2] = v; return a[2]; }
int st_member(struct holder *h, int v) { h->flag = v; return h->flag; }
int st_increment(void) { bool b = true; b++; return b; }
int st_static_init(void) { static bool s = 0x100; return s; }
int st_global_stdbool(int v) { gb = v & 0x200; return gb; }
bool *gpb;
int st_global_ptr(bool *q, int v) { gpb = q; *gpb = v; return *q; }
int st_param_value(bool b) { b = 0x300; return b; }
int st_compound(bool *p, int v) { *p = false; *p |= v; return *p; }
int st_compound_add(bool *p, int v) { *p = true; *p += v; return *p; }
struct bool_ops {
  bool (*take)(bool);
  int (*mix)(int, bool, long);
};
int st_indirect_field(struct bool_ops *o, int v) { return o->take(v); }
int st_indirect_mix(struct bool_ops *o, int v) { return o->mix(7, v, 9L); }
static bool local_take(bool b) { return b; }
int st_indirect_param(bool (*cb)(bool), int v) { return cb(v); }
int st_indirect_local(int v) {
  bool (*cb)(bool) = local_take;
  return cb(v);
}
typedef bool (*bool_fn_t)(bool);
struct bool_typedef_ops { bool_fn_t take; };
static bool_fn_t bool_global_fn = local_take;
int st_indirect_global(int v) { return bool_global_fn(v); }
int st_indirect_typedef_param(bool_fn_t cb, int v) { return cb(v); }
int st_indirect_typedef_local(int v) {
  bool_fn_t cb = local_take;
  return cb(v);
}
int st_indirect_typedef_field(struct bool_typedef_ops *o, int v) { return o->take(v); }
