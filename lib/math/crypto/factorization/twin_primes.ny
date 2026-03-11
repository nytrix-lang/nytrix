;; Keywords: factorization twin-primes
;; Integer-factorization routines for factorization with twin-prime structure.
;; Also handles general close-prime factorization with known k bound.
;; Reference:
;; - https://cacr.uwaterloo.ca/hac/about/chap8.pdf
;; - https://crypto.stanford.edu/~dabo/pubs/papers/RSA-survey.pdf
module std.math.crypto.factorization.twin_primes(twin_prime_factor, twin_prime_factor_general, factor_close_primes, detect_twin_prime_product, is_perfect_square)
use std.math.nt

fn twin_prime_factor(any: n): any {
   "Factor n where n = p * (p + 2), i.e., twin primes.
   From p^2 + 2p - n = 0, we get p = -1 + sqrt(1 + n).
   Returns [p, q] with q = p + 2, or nil if n is not a twin-prime product."
   def inner = n + 1
   if(!is_perfect_square(inner)){ return nil }
   mut s, p = isqrt(inner), s - 1
   mut q = p + 2
   if(p < 2){ return nil }
   if(p * q != n){ return nil }
   [p, q]
}

fn twin_prime_factor_general(any: n, any: k): any {
   "Factor n where n = p * (p + k) for a known small k.
   From p^2 + kp - n = 0, we get p = (-k + sqrt(k^2 + 4n)) / 2.
   Returns [p, p+k] or nil."
   def disc = k * k + 4 * n
   if(!is_perfect_square(disc)){ return nil }
   mut s, p = isqrt(disc), (s - k) / 2
   mut q = p + k
   if(p < 2){ return nil }
   if(p * q != n){ return nil }
   [p, q]
}

fn factor_close_primes(any: n, any: k_max): any {
   "Factor n = p * q where |p - q| <= k_max.
   Uses Fermat factorization which is efficient when primes are close.
   Returns [p, q] with p <= q, or nil if no factor found within bound."
   mut a = isqrt(n)
   mut count = 0
   def iter_limit = k_max / 2 + 2
   while(count < iter_limit){
      def a_sq = a * a
      def diff = a_sq - n
      if(diff >= 0 && is_perfect_square(diff)){
         def b = isqrt(diff)
         mut p, q = a - b, a + b
         if(p > 1 && q > 1 && p * q == n){ return(p < q) ? [p, q] : [q, p] }
      }
      a += 1
      count += 1
   }
   nil
}

fn factor_close_primes_brute(any: n, any: k_max): any {
   "Brute-force search for k in [1, k_max] where n = p * (p + k).
   Simpler but slower than the Fermat-based approach.
   Returns [p, q] or nil."
   mut k = 1
   while(k <= k_max){
      def disc = k * k + 4 * n
      if(is_perfect_square(disc)){
         mut s = isqrt(disc)
         if((s - k) % 2 == 0){
            mut p, q = (s - k) / 2, p + k
            if(p > 1 && p * q == n){ return [p, q] }
         }
      }
      k += 1
   }
   nil
}

fn detect_twin_prime_product(any: n): bool {
   "Check whether n is the product of twin primes.
   Returns true if n = p * (p+2) for some prime p."
   mut result = twin_prime_factor(n)
   if(result == nil){ return false }
   mut p, q = result.get(0), result.get(1)
   is_prime(p) && is_prime(q)
}

fn nearest_twin_candidate(any: n): any {
   "Find the nearest twin-prime product to n(for analysis).
   Returns [p, p+2, actual_product] or nil."
   def a = isqrt(n)
   mut candidate = a - 1
   while(candidate > 1){
      mut p, q = candidate, candidate + 2
      def prod = p * q
      if(prod == n){ return [p, q, prod] }
      if(prod < n){
         def next_p, next_q = candidate, candidate + 2
         def next_prod = next_p * next_q
         if(next_prod == n){ return [next_p, next_q, next_prod] }
         return nil
      }
      candidate = candidate - 1
   }
   nil
}
