/*
 * A vectorized f64 sum beside scalar floats held in registers: the
 * reduction's scratch registers must not disturb `scale` or `bias`. Local
 * allocations and a constant trip count are what make the sum vectorize.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define N 100

static uint64_t opaque(uint64_t v) {
  volatile uint64_t x = v;
  return x;
}

int main(void) {
  double *x = malloc(N * sizeof(double));
  double *y = malloc(N * sizeof(double));
  if (!x || !y)
    return 2;
  for (int i = 0; i < N; i++) {
    x[i] = i + 1;
    y[i] = 0;
  }
  double scale = (double)opaque(3);
  double bias = (double)opaque(7);
  for (int k = 0; k < 4; k++)
    for (int c = 0; c < N; c++)
      y[c] -= scale * x[c] + bias;
  double total = 0.0;
  for (int i = 0; i < N; i++)
    total += x[i];
  double result = total * scale + bias;
  /*
   * Overlapping malloc accesses: each element depends on the previous write,
   * so widening this loop would read stale values.
   */
  for (int i = 0; i < N - 1; i++)
    x[i + 1] = x[i] + 2.0;
  /*
   * A signed counter below zero must keep its sign in the index.
   */
  double below = 0.0;
  for (int i = -5; i < 5; i++)
    below += y[i + 5] * i;
  /*
   * The float-to-int conversion must not clobber the constant's register.
   */
  long scaled = 0;
  for (int i = 0; i < N; i++)
    scaled += (long)(x[i] * 1000.0);
  /*
   * Branch-selected buffers. Distinct buffers may vectorize; when both PHIs
   * can name one buffer the loop must keep its scalar dependence.
   */
  double *z = malloc(N * sizeof(double));
  if (!z)
    return 2;
  for (int i = 0; i < N; i++)
    z[i] = i;
  double *from = x, *to = z;
  if (opaque(1))
    to = x;
  for (int i = 1; i < N; i++)
    to[i] = from[i - 1] + 1.0;
  printf("checksum=%.1f %.1f %.1f %.1f %ld %.1f\n", result, y[9], x[N - 1],
         below, scaled, to[N - 1]);
  return result == 15157.0 && y[9] == -148.0 && below == -520.0 ? 0 : 1;
}
