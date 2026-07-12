typedef struct SumPairLayout {
  long a;
  long b;
  long c;
} SumPairLayout;

long sum_pair(SumPairLayout p) {
  return p.a + p.b + p.c;
}
