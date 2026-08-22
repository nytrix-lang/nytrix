/*
 * Reference GMP baseline for the optcheck integer kernel.
 * Expected output: 5168580067
 * Compile: cc -O3 -o integer-kernel-gmp integer-kernel-gmp.c -lgmp && ./integer-kernel-gmp
 */
#include <gmp.h>
int main(void) {
  mpz_t acc, branch, term;
  mpz_init_set_ui(acc, 0);
  mpz_init_set_ui(branch, 0);
  mpz_init(term);
  for (unsigned long i = 1; i <= 250000UL; ++i) {
    mpz_set_ui(term, (i % 97UL) * (i % 89UL));
    mpz_add(acc, acc, term);
    if (i % 11UL == 0) mpz_add_ui(branch, branch, i);
  }
  mpz_add(acc, acc, branch);
  mpz_t state, mixed;
  mpz_init_set_ui(state, 7);
  mpz_init_set_ui(mixed, 0);
  for (unsigned long i = 1; i <= 50000UL; ++i) {
    mpz_mul_ui(state, state, 1103515UL);
    mpz_add_ui(state, state, 12345UL);
    mpz_mod_ui(state, state, 2147483647UL);
    if (mpz_tdiv_ui(state, 13UL) == 0) mpz_add_ui(mixed, mixed, i);
  }
  mpz_add(acc, acc, mixed);
  mpz_add(acc, acc, state);
  mpz_t signed_sum, x, rem;
  mpz_init_set_ui(signed_sum, 0);
  mpz_init(x);
  mpz_init(rem);
  for (unsigned long i = 1; i <= 100000UL; ++i) {
    mpz_set_ui(x, i);
    mpz_mul_ui(x, x, 1664525UL);
    mpz_add_ui(x, x, 1013904223UL);
    if (mpz_tdiv_ui(x, 2UL) == 0) {
      mpz_mod_ui(rem, x, 65537UL);
      mpz_add(signed_sum, signed_sum, rem);
    } else {
      mpz_mod_ui(rem, x, 32749UL);
      mpz_sub(signed_sum, signed_sum, rem);
    }
  }
  mpz_add(acc, acc, signed_sum);
  gmp_printf("%Zd\n", acc);
  mpz_clear(rem); mpz_clear(x); mpz_clear(signed_sum);
  mpz_clear(mixed); mpz_clear(state); mpz_clear(branch);
  mpz_clear(term); mpz_clear(acc);
  return 0;
}
