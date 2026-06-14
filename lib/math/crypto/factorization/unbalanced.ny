;; Keywords: factorization unbalanced math crypto number-theory
;; Integer-factorization routines for factorization of unbalanced semiprimes.
;; Unbalanced: one prime is much smaller than the other.
;; Trial division and Fermat methods are effective here.
;; Reference:
;; - https://cacr.uwaterloo.ca/hac/about/chap8.pdf
;; - https://crypto.stanford.edu/~dabo/pubs/papers/RSA-survey.pdf
;; References:
;; - std.math.crypto.factorization
;; - std.math.crypto
module std.math.crypto.factorization.unbalanced(unbalanced_factor, unbalanced_factor_fermat, factor_unbalanced, factor_unbalanced_wheels, detect_unbalanced, estimate_small_factor_bits, factor_three_unbalanced)
use std.core
use std.math.nt

fn unbalanced_factor(any n) any {
   "Factor n = p * q where p and q are significantly different in size.
   First tries trial division up to n^(1/4), then falls back to Fermat.
   Returns [p, q] with p <= q, or nil."
   def limit = isqrt(isqrt(n)) + 1
   mut p2 = 2
   while p2 <= limit && n % p2 != 0 { p2 += 1 }
   if p2 <= limit && n % p2 == 0 {
      mut q = n / p2
      return [p2, q]
   }
   mut p3 = 3
   while p3 <= limit && n % p3 != 0 { p3 = p3 + 2 }
   if p3 <= limit && n % p3 == 0 {
      mut q = n / p3
      return [p3, q]
   }
   mut p5 = 5
   while p5 * p5 <= limit && p5 * p5 <= n {
      while n % p5 == 0 {
         mut q = n / p5
         return [p5, q]
      }
      def p5p = p5 + 2
      while n % p5p == 0 {
         mut q = n / p5p
         return [p5p, q]
      }
      p5 = p5 + 6
   }
   def fermat = unbalanced_factor_fermat(n)
   fermat
}

fn unbalanced_factor_fermat(any n) any {
   "Factor n using Fermat method, effective when one factor
   is much smaller(a will be close to sqrt(n)).
   Returns [p, q] or nil."
   mut a = isqrt(n)
   mut count = 0
   def max_iter = isqrt(n) / 2
   while count < max_iter {
      def a_sq = a * a
      def diff = a_sq - n
      if diff >= 0 {
         mut s = isqrt(diff)
         if s * s == diff {
            mut p, q = a - s, a + s
            if p > 1 && p * q == n { return(p < q) ? [p, q] : [q, p] }
         }
      }
      a += 1
      count += 1
   }
   nil
}

fn factor_unbalanced(any n, any small_bound) any {
   "Factor n assuming the smaller prime p < small_bound.
   Uses trial division up to small_bound.
   Returns [p, q] with p <= q, or nil if no small factor found."
   if small_bound < 2 { return nil }
   if n % 2 == 0 {
      mut q = n / 2
      return [2, q]
   }
   if n % 3 == 0 {
      mut q = n / 3
      return [3, q]
   }
   mut p = 5
   while p < small_bound && p * p <= n {
      while n % p == 0 {
         mut q = n / p
         return [p, q]
      }
      mut p2 = p + 2
      if p2 < small_bound {
         while n % p2 == 0 {
            mut q = n / p2
            return [p2, q]
         }
      }
      p = p + 6
   }
   nil
}

fn factor_unbalanced_wheels(any n, any small_bound) any {
   "Optimized trial division using wheel factorization.
   Skips multiples of 2, 3, 5 for faster small-factor search.
   Returns [p, q] or nil."
   def wheel = [1, 7, 11, 13, 17, 19, 23, 29]
   def nw = wheel.len
   def small_primes = [2, 3, 5]
   def ns = small_primes.len
   mut i = 0
   while i < ns {
      def sp = small_primes.get(i)
      if n % sp == 0 {
         mut q = n / sp
         return [sp, q]
      }
      i += 1
   }
   mut base = 0
   while base < small_bound {
      mut j = 0
      while j < nw {
         mut p = base + wheel.get(j)
         if p < 2 {
            j += 1
            continue
         }
         if p >= small_bound { break }
         if p * p > n { return nil }
         if n % p == 0 {
            mut q = n / p
            return [p, q]
         }
         j += 1
      }
      base = base + 30
   }
   nil
}

fn detect_unbalanced(any n) bool {
   "Detect if n has an unbalanced factorization.
   Returns true if the smallest factor is much smaller than sqrt(n).
   Uses a threshold of sqrt(n)/100."
   def sqrt_n = isqrt(n)
   mut threshold = sqrt_n / 100
   if threshold < Z(2) { threshold = Z(2) }
   mut result = factor_unbalanced(n, threshold)
   result != nil
}

fn estimate_small_factor_bits(any n) int {
   "Estimate the bit length of the smaller factor.
   Returns approximate bit count of the smaller prime,
   or -1 if n appears balanced."
   def sqrt_n = isqrt(n)
   mut a = sqrt_n
   mut count = 0
   def max_search = 1000000
   while count < max_search {
      def a_sq = a * a
      def diff = a_sq - n
      if diff >= 0 {
         mut s = isqrt(diff)
         if s * s == diff {
            mut p = a - s
            if p > 1 {
               def bits = bit_length(p)
               return bits
            }
         }
      }
      a += 1
      count += 1
   }
   -1
}

fn factor_three_unbalanced(any n, any bound1, any bound2) any {
   "Factor n = p * q * r where p < q < r and p < bound1, q < bound2.
   Finds the smallest factor first, then factors the quotient.
   Returns [p, q, r] or nil."
   def p_result = factor_unbalanced(n, bound1)
   if p_result == nil { return nil }
   mut p = p_result.get(0)
   def remaining = p_result.get(1)
   def q_result = factor_unbalanced(remaining, bound2)
   if q_result == nil { return nil }
   mut q, r = q_result.get(0), q_result.get(1)
   [p, q, r]
}

#main {
   def p, q = 17, 999983
   def n = p * q
   assert(unbalanced_factor(n) == [p, q] && factor_unbalanced(n, 100) == [p, q] && factor_unbalanced_wheels(n, 100) == [p, q], "unbalanced semiprime")
   assert(detect_unbalanced(n) && !detect_unbalanced(997 * 1009) && estimate_small_factor_bits(n) > 0, "unbalanced detection")
   assert(factor_three_unbalanced(3 * 5 * 997, 10, 20) == [3, 5, 997], "three unbalanced factors")
   print("✓ std.math.crypto.factorization.unbalanced self-test passed")
}
