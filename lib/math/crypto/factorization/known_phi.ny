;; Keywords: factorization known-phi math crypto number-theory
;; Integer-factorization routines for factorization from phi or private-exponent leaks.
;; to recover p and q from the sum S = p+q and product P = p*q = N.
;; Reference:
;; - https://cacr.uwaterloo.ca/hac/about/chap8.pdf
;; - https://crypto.stanford.edu/~dabo/pubs/papers/RSA-survey.pdf
;; References:
;; - std.math.crypto.factorization
;; - std.math.crypto
module std.math.crypto.factorization.known_phi(factor_from_phi, factor_from_multiple_phi, factor_from_phi_multi, factor_from_phi_with_e_d, recover_phi_from_d, solve_quadratic_roots)
use std.math.nt

fn solve_quadratic_roots(any sum_pq, any prod_pq) any {
   "Solve x^2 - sum_pq*x + prod_pq = 0 and return [r1, r2].
   Uses the quadratic formula: x = (sum +- sqrt(sum^2 - 4*prod)) / 2.
   Returns nil if the discriminant is not a perfect square."
   def disc = sum_pq * sum_pq - 4 * prod_pq
   if disc < 0 { return nil }
   if !is_perfect_square(disc) { return nil }
   def sqrt_disc = isqrt(disc)
   def r1 = (sum_pq + sqrt_disc) / 2
   def r2 = (sum_pq - sqrt_disc) / 2
   [r1, r2]
}

fn factor_from_phi(any n, any phi) any {
   "Factor n given phi(n). Since phi = (p-1)(q-1) = n - p - q + 1,
   we have p + q = n + 1 - phi. With p*q = n, we solve
   x^2 - (n+1-phi)*x + n = 0.
   Returns [p, q] with p <= q, or nil if factoring fails."
   def sum_pq = n + 1 - phi
   mut result = solve_quadratic_roots(sum_pq, n)
   if result == nil { return nil }
   mut p, q = result.get(0), result.get(1)
   if p * q != n { return nil }
   if p < 2 { return nil }
   if q < 2 { return nil }
   (p < q) ? [p, q] : [q, p]
}

fn factor_from_multiple_phi(any n, any multiple_phi, any bases=[2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37]) any {
   "Factor semiprime n given any nonzero multiple of phi(n).
   This is the same square-root-of-1 idea used when factoring from e*d-1:
   write the multiple as 2^t*r, then search deterministic bases for a
   nontrivial square root of 1 modulo n. Returns [p, q] or nil."
   if n <= 1 || multiple_phi <= 0 { return nil }
   mut r, t = multiple_phi, 0
   while (r % 2) == 0 {
      r = r / 2
      t += 1
   }
   if t == 0 { return nil }
   mut bi = 0
   while bi < bases.len {
      def a0 = Z(bases.get(bi)) % n
      bi += 1
      if a0 <= 1 { continue }
      def g0 = gcd(a0, n)
      if g0 > 1 && g0 < n {
         def q0 = n / g0
         return(g0 < q0) ? [g0, q0] : [q0, g0]
      }
      mut y = power_mod(a0, r, n)
      if y == 1 || y == n - 1 { continue }
      mut j = 0
      while j < t {
         def x = power_mod(y, 2, n)
         if x == 1 {
            def p = gcd(y - 1, n)
            if p > 1 && p < n {
               def q = n / p
               return(p < q) ? [p, q] : [q, p]
            }
            break
         }
         if x == n - 1 { break }
         y = x
         j += 1
      }
   }
   nil
}

fn factor_from_phi_multi(any n, any phi, int num_primes) any {
   "Factor n given phi(n) when n is a product of multiple equal-size primes.
   Uses the relationship between elementary symmetric polynomials and phi.
   For num_primes=2 this is the standard two-prime case.
   Returns a list of prime factors, or nil."
   if num_primes == 2 { return factor_from_phi(n, phi) }
   mut primes = []
   mut pending = [n]
   def bases = [2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31]
   while pending.len > 0 {
      def cur = pending.get(pending.len - 1)
      pending = slice(pending, 0, pending.len - 1)
      if cur <= 1 { continue }
      if is_prime(cur) {
         if !primes.contains(cur) { primes = primes.append(cur) }
         continue
      }
      mut split = false
      mut bi = 0
      while bi < bases.len {
         if split { break }
         def w = bases.get(bi)
         if w >= cur {
            bi += 1
            continue
         }
         mut i = 1
         mut pow2 = 2
         while phi % pow2 == 0 {
            def sqrt1 = power_mod(w, phi / pow2, cur)
            if sqrt1 > 1 {
               if sqrt1 == cur - 1 {
                  i += 1
                  pow2 = pow2 * 2
                  continue
               }
               def p = gcd(cur, sqrt1 + 1)
               if p > 1 {
                  if p < cur && cur % p == 0 {
                     def q = cur / p
                     pending = pending.append(p)
                     pending = pending.append(q)
                     split = true
                     break
                  }
               }
            }
            i += 1
            pow2 = pow2 * 2
         }
         bi += 1
      }
      if !split { return nil }
   }
   if num_primes > 0 { if primes.len != num_primes { return nil } }
   primes
}

fn factor_from_phi_with_e_d(any n, any e, any d) any {
   "Factor n given the public exponent e and private exponent d.
   Uses the fact that e*d - 1 = k*phi(n) for some k.
   Tries each possible k to recover phi, then factors.
   Returns [p, q] or nil."
   def ed_minus_1 = e * d - 1
   mut k = 1
   while k <= e {
      if ed_minus_1 % k == 0 {
         def phi_candidate = ed_minus_1 / k
         mut result = factor_from_phi(n, phi_candidate)
         if result != nil { return result }
      }
      k += 1
   }
   nil
}

fn recover_phi_from_d(any n, any e, any d) any {
   "Recover phi(n) given n, e, and d.
   Since e*d = 1 mod phi(n), we have e*d - 1 = k*phi(n).
   Returns the correct phi(n), or nil."
   def ed_minus_1 = e * d - 1
   mut k = 1
   while k <= e {
      if ed_minus_1 % k == 0 {
         def phi_candidate = ed_minus_1 / k
         def sum_check = n + 1 - phi_candidate
         def disc = sum_check * sum_check - 4 * n
         if disc >= 0 {
            mut s = isqrt(disc)
            if s * s == disc { return phi_candidate }
         }
      }
      k += 1
   }
   nil
}

#main {
   def p, q = 61, 53
   def n = p * q
   def phi = (p - 1) * (q - 1)
   def e = 17
   def d = e.invmod(phi)
   assert(factor_from_phi(n, phi) == [q, p], "factor from phi")
   assert(factor_from_multiple_phi(n, phi * 13) == [q, p], "factor from multiple phi")
   assert(solve_quadratic_roots(p + q, p * q) == [p, q], "quadratic roots")
   assert(factor_from_phi_with_e_d(n, e, d) == [q, p], "factor from e d")
   assert(recover_phi_from_d(n, e, d) == phi, "recover phi")
   print("✓ std.math.crypto.factorization.known_phi self-test passed")
}
