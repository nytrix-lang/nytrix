;; Keywords: rsa multiprime math crypto
;; RSA multiprime RSA recovery routines.
;; N = p1 * p2 * ... * pk, phi = product of (pi - 1) for all i.
;; Reference:
;; - https://people.csail.mit.edu/rivest/Rsapaper.pdf
;; - https://crypto.stanford.edu/~dabo/pubs/papers/RSA-survey.pdf
;; References:
;; - std.math.crypto.rsa
;; - std.math.crypto
module std.math.crypto.rsa.multiprime(multiprime_phi, multiprime_decrypt, repeated_prime_factor, repeated_prime_phi)
use std.core
use std.math.nt

fn multiprime_phi(list primes) any {
   "Compute Euler's totient for multi-prime RSA where N = p1 * p2 * ... * pk.
   phi(N) = (p1-1) * (p2-1) * ... * (pk-1).
   primes: list of distinct prime factors of N.
   Returns phi(N)."
   mut result = 1
   mut i = 0
   mut n_primes = primes.len
   while i < n_primes {
      def p_i = primes.get(i)
      result = result * (p_i - 1)
      i += 1
   }
   result
}

fn multiprime_decrypt(any c, any d, any n) any {
   "Decrypt ciphertext c using private exponent d and modulus n for multi-prime RSA.
   This is the same as standard RSA decrypt: m = c^d mod n.
   c: ciphertext, d: private exponent, n: public modulus(product of all primes).
   Returns the recovered plaintext."
   def m = power_mod(c, d, n)
   m
}

fn repeated_prime_factor(any n, int min_power=2, int max_power=64) any {
   "Recover [p, r] when RSA modulus n is an exact prime power p^r.
   Returns nil if no exact prime power is found in the requested exponent range."
   def nn = Z(n)
   mut r = min_power
   while r <= max_power {
      def p = nth_root(nn, r)
      if p > Z(1) && is_prime(p) {
         mut acc = Z(1)
         mut i = 0
         while i < r {
            acc = acc * p
            i += 1
         }
         if acc == nn { return [p, r] }
      }
      r += 1
   }
   nil
}

fn repeated_prime_phi(any p, int r) any {
   "Compute Euler phi(p^r) = p^(r-1) * (p-1)."
   def pp = Z(p)
   mut acc = Z(1)
   mut i = 1
   while i < r {
      acc = acc * pp
      i += 1
   }
   acc * (pp - Z(1))
}

#main {
   fn roundtrip(list primes, any e, any m) bool {
      mut n = 1
      mut i = 0
      while i < primes.len {
         n = n * primes.get(i)
         i += 1
      }
      def phi = multiprime_phi(primes)
      def d = inverse_mod(e, phi)
      def c = power_mod(m, e, n)
      multiprime_decrypt(c, d, n) == m
   }
   assert(roundtrip([5, 7, 11], 7, 100), "three-prime RSA")
   assert(roundtrip([61, 53], 17, 314), "two-prime RSA")
   assert(roundtrip([3, 5, 7, 11], 7, 50), "four-prime RSA")
   def pp = repeated_prime_factor(Z(13) * Z(13) * Z(13), 2, 5)
   assert(pp != nil, "repeated_prime_factor")
   assert(pp.get(0) == Z(13) && pp.get(1) == 3, "repeated_prime_factor result")
   assert(repeated_prime_phi(Z(13), 3) == Z(13) * Z(13) * Z(12), "repeated_prime_phi")
   print("✓ std.math.crypto.rsa.multiprime self-test passed")
}
