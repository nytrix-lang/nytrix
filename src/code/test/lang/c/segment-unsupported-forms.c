/* Segment qualifiers on aggregate pointers, fields and typedefs are rejected, not dropped. */
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
#ifdef SEG_CAST_OK
int f(unsigned long offset) { return *(__seg_gs int *)offset; }
#endif
