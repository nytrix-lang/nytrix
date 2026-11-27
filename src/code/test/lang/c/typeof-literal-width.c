/*
 * typeof of an integer literal follows its suffix and magnitude, so a long parameter keeps all 64 bits.
 */
long a1(__typeof(0L) a) { return a; }
long a2(__typeof(1L) a) { return a; }
unsigned long a3(__typeof(0UL) a) { return a; }
long a4(__typeof(0LL) a) { return a; }
long a5(long x) { __typeof(0L) y = x; return y; }
long a6(long x) { __typeof__(x) y = x; return y; }
long a7(__typeof(5000000000L) a) { return a; }
long a8(long long x) { return (__typeof(0L))x; }
long a9(__typeof(sizeof(int)) a) { return a; }
