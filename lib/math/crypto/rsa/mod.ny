;; Keywords: rsa public-key key op encrypt decrypt signature verify pkcs1-v15 multiprime modular-arithmetic crt math crypto
;; RSA facade for keys, operations, signatures, PKCS#1 v1.5 helpers, and modular arithmetic.
;; References:
;; - https://people.csail.mit.edu/rivest/Rsapaper.pdf
;; - https://crypto.stanford.edu/~dabo/pubs/papers/RSA-survey.pdf
module std.math.crypto.rsa(op, wiener, extended_wiener, boneh_durfee, hastad, low_exponent, common_modulus, common_prime, related_message, non_coprime, known_d, fixed_points, multiprime, partial, repeated_roots, crt_fault, d_fault, known_crt_exponents, cca_malleability, stereotyped, lsb_oracle, bleichenbacher, manger, rsa_solve, signature, pkcs1_v15, recover_modulus, desmedt_odlyzko, partial_key_exposure, cherkaoui_semmouni, nitaj_crt_rsa, mersenne_exponent, key, lcg_rng, rsa_lcg_factor_power2, low_exp_attack_report, common_modulus_attack_report, hastad_attack_report)
use std.math.nt
use std.math.crypto.rsa.op
use std.math.crypto.rsa.wiener
use std.math.crypto.rsa.low_exponent
use std.math.crypto.rsa.common_modulus
use std.math.crypto.rsa.lcg_rng as lcg_rng_mod
use std.math.crypto.factorization.fermat
use std.math.crypto.factorization.pollard

fn rsa_lcg_factor_power2(any n, any mult, any inc, int modulus_bits=512, int max_gap=4096) list {
   "Recover RSA factors when primes are consecutive LCG prime states modulo 2^modulus_bits."
   lcg_rng_mod.rsa_lcg_factor_power2(n, mult, inc, modulus_bits, max_gap)
}

fn _rsa_solve_with_factors(any n, any e, any c, any p, any q) any {
   def d = compute_d(e, compute_phi(p, q))
   if d <= 0 { return nil }
   [power_mod(c, d, n), d]
}

fn _rsa_small_d_brute(any n, any e, any c, int start, int limit) any {
   mut d = start
   while d < limit {
      def m = power_mod(c, d, n)
      if power_mod(m, e, n) == c { return [m, d] }
      d += 1
   }
   nil
}

fn _rsa_small_d_brute_limit(any n) int {
   "Keep exhaustive private-exponent search for toy challenge keys only.
   Real RSA-sized moduli should use Wiener/factorization paths instead of a
   blind 100k modular-exponent sweep."
   def bits = bit_length(Z(n))
   if bits <= 32 { return 100000 }
   if bits <= 48 { return 4096 }
   0
}

fn rsa_solve(any n, any e, any c, any e2=0, any c2=0) any {
   "Automatically attempt to solve RSA ciphertext c given(n, e).
   Order: low_exponent → wiener → bounded Fermat → pollard_pm1 → common_modulus → batch_gcd → small_d_brute(toy keys).
   Returns [m, method, extra] or nil."
   def m1 = low_exp_attack(c, e)
   if m1 != nil { return [m1, "low_exponent", 0] }
   def r_wiener = wiener_attack(n, e)
   if r_wiener != nil {
      def d_wi = r_wiener.get(0)
      return [power_mod(c, d_wi, n), "wiener", d_wi]
   }
   def r_fermat = fermat_attack(n, 8192)
   if r_fermat != nil {
      def p, q = r_fermat.get(0), r_fermat.get(1)
      def solved = _rsa_solve_with_factors(n, e, c, p, q)
      if solved != nil { return [solved.get(0), "fermat", solved.get(1)] }
   }
   def p_p1 = pollard_pm1(n, 100000)
   if p_p1 != nil {
      def q = n / p_p1
      def solved = _rsa_solve_with_factors(n, e, c, p_p1, q)
      if solved != nil { return [solved.get(0), "pollard_pm1", p_p1] }
   }
   if e2 > 0 && c2 > 0 {
      def m_cm = common_modulus_attack(n, e, c, e2, c2)
      if m_cm != nil { return [m_cm, "common_modulus", 0] }
   }
   if e2 > 0 && c2 == 0 {
      def g = gcd(n, e2)
      if g > 1 && g < n {
         def p, q = g, n / g
         def solved = _rsa_solve_with_factors(n, e, c, p, q)
         if solved != nil { return [solved.get(0), "batch_gcd", p] }
      }
   }
   def limit = _rsa_small_d_brute_limit(n)
   if limit > 1 {
      def small = _rsa_small_d_brute(n, e, c, 1, limit)
      if small != nil { return [small[0], "small_d_brute", small[1]] }
   }
   nil
}
