/*
 * Segment qualifiers on aggregate pointers, fields and typedefs and arrays of segment pointers are rejected, not dropped.
 */
#ifdef SEG_AGGREGATE_PARAM
struct S { int a; int b; };
int f(__seg_gs struct S *p) { return p->b; }
#endif
#ifdef SEG_FIELD
struct H { __seg_gs int *ptr; };
int f(struct H *h) { return *h->ptr; }
#endif
#ifdef SEG_TYPEDEF
typedef __seg_gs int gsint;
int f(gsint *p) { return *p; }
#endif
#ifdef SEG_GLOBAL_ARRAY
int __seg_gs *table[4];
int f(int i) { return *table[i]; }
#endif
#ifdef SEG_LOCAL_ARRAY
int f(int i) { int __seg_gs *table[2] = {0, 0}; return *table[i]; }
#endif
#ifdef SEG_PARAM_ARRAY
int f(int __seg_gs *table[2], int i) { return *table[i]; }
#endif
#ifdef SEG_CAST_OK
int f(unsigned long offset) { return *(__seg_gs int *)offset; }
#endif
