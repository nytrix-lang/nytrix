;; Keywords: factorization fermat math crypto number-theory
;; Integer-factorization routines for Fermat and near-square factorization.
;; Expresses n = a^2 - b^2 = (a-b)(a+b) where a = (p+q)/2, b = (p-q)/2.
;; Reference:
;; - https://cacr.uwaterloo.ca/hac/about/chap8.pdf
;; - https://crypto.stanford.edu/~dabo/pubs/papers/RSA-survey.pdf
;; References:
;; - std.math.crypto.factorization
;; - std.math.crypto
module std.math.crypto.factorization.fermat(fermat_factor, fermat_factor_bounded, fermat_attack, is_perfect_square)
use std.math.nt

fn _fermat_isqrt_int(int n) int {
   if n <= 0 { return 0 }
   mut hi = 1
   while hi <= n / hi { hi *= 2 }
   mut lo = hi / 2
   while lo + 1 < hi {
      def mid = (lo + hi) / 2
      if mid <= n / mid { lo = mid } else { hi = mid }
   }
   lo
}

fn _fermat_is_square_int(int n) bool {
   if n < 0 { return false }
   def r = _fermat_isqrt_int(n)
   r * r == n
}

fn _fermat_factor_bounded_i62(int n, int max_iter) any {
   mut a = _fermat_isqrt_int(n)
   mut count = 0
   while count < max_iter {
      def diff = a * a - n
      if diff >= 0 && _fermat_is_square_int(diff) {
         def b = _fermat_isqrt_int(diff)
         def p = a - b
         def q = a + b
         if p > 1 && q > 1 { return p < q ? [Z(p), Z(q)] : [Z(q), Z(p)] }
         return nil
      }
      a += 1
      count += 1
   }
   nil
}

fn fermat_factor(any n) list {
   "Factor n using Fermat method. Returns [p, q] with p <= q.
   Assumes n is an odd composite. Starts a = isqrt(n) and
   increments a until a^2 - n is a perfect square."
   mut a = isqrt(n)
   mut a_sq = a * a
   def diff = a_sq - n
   if diff >= 0 && is_perfect_square(diff) {
      def b = isqrt(diff)
      mut p, q = a - b, a + b
      (p < q) ? [p, q] : [q, p]
   } else {
      mut aa = a + 1
      mut bb = aa * aa - n
      while !is_perfect_square(bb) {
         aa += 1
         bb = aa * aa - n
      }
      def b = isqrt(bb)
      mut p, q = aa - b, aa + b
      (p < q) ? [p, q] : [q, p]
   }
}

fn fermat_factor_bounded(any n, int max_iter) any {
   "Factor n using Fermat method with an iteration limit.
   Returns [p, q] if found within max_iter iterations,
   or nil if the factors are too far apart."
   if bit_length(n) <= 62 { return _fermat_factor_bounded_i62(bigint_to_int(Z(n)), max_iter) }
   mut a = isqrt(n)
   mut count = 0
   while count < max_iter {
      def a_sq = a * a
      def diff = a_sq - n
      if diff >= 0 && is_perfect_square(diff) {
         def b = isqrt(diff)
         mut p, q = a - b, a + b
         return(p > 1 && q > 1) ? ((p < q) ? [p, q] : [q, p]) : nil
      }
      a += 1
      count += 1
   }
   nil
}

fn fermat_attack(any n, int max_iter=1000000) any {
   "RSA-oriented entrypoint for bounded Fermat factorization."
   fermat_factor_bounded(n, max_iter)
}
