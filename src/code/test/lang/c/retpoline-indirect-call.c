/*
 * C fixture defining a target and an indirect function-pointer call.
 */
long retpoline_target(long x) { return x * 3 + 1; }
long retpoline_call(long (*target)(long), long x) { return target(x); }

double retpoline_variadic_call(double (*target)(int, ...), double x) {
  return target(2, x, 1.25);
}
