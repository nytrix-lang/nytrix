/* Converting a non-zero integer to _Bool yields 1 for arguments and struct members, not the low byte. */
typedef _Bool bool;
static inline bool sp(unsigned long f) { return f & 0x200; }
static inline bool sp2(unsigned long f) { bool r = f & 0x200; return r; }
bool g1(unsigned long f) { return sp(f); }
int g2(unsigned long f) { return sp(f); }
unsigned char g3(unsigned long f) { return sp(f); }
bool g4(unsigned long f) { return f & 0x200; }
int take(bool b);
int g5(unsigned long f) { return take(f & 0x200); }
int g6(unsigned long f, bool special) { return special; }
int g7(unsigned long f) { return g6(f, f & 0x200); }
struct s { int a; bool b; };
int s1(unsigned long f, struct s *p) { p->b = f & 0x200; return p->b; }
int s2(unsigned long f) { struct s v = {.a = 1, .b = f & 0x200}; return v.b; }
