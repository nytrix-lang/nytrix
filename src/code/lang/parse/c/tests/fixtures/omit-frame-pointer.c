long omit_fp_sum8(long a, long b, long c, long d, long e, long f, long g, long h) { return a + b + c + d + e + f + g + h; }
long omit_fp_call(long x) { long a = x + 1, b = x + 2, c = x + 3, d = x + 4, e = x + 5, f = x + 6; return omit_fp_sum8(x, a, b, c, d, e, f, x + 7) + a + b + c + d + e + f; }
