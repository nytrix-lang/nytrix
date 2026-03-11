;; Keywords: rsa multiprime
;; RSA multiprime RSA recovery routines.
;; N = p1 * p2 * ... * pk, phi = product of (pi - 1) for all i.
;; Reference:
;; - https://people.csail.mit.edu/rivest/Rsapaper.pdf
;; - https://crypto.stanford.edu/~dabo/pubs/papers/RSA-survey.pdf
module std.math.crypto.rsa.multiprime(multiprime_phi, multiprime_decrypt, repeated_prime_factor, repeated_prime_phi)
use std.core
use std.math.nt

fn multiprime_phi(list: primes): any {
   "Compute Euler's totient for multi-prime RSA where N = p1 * p2 * ... * pk.
   phi(N) = (p1-1) * (p2-1) * ... * (pk-1).
   primes: list of distinct prime factors of N.
   Returns phi(N)."
   mut result = 1
   mut i = 0
   mut n_primes = primes.len
   while(i < n_primes){
      def p_i = primes.get(i)
      result = result * (p_i - 1)
      i += 1
   }
   result
}

fn multiprime_decrypt(any: c, any: d, any: n): any {
   "Decrypt ciphertext c using private exponent d and modulus n for multi-prime RSA.
   This is the same as standard RSA decrypt: m = c^d mod n.
   c: ciphertext, d: private exponent, n: public modulus(product of all primes).
   Returns the recovered plaintext."
   def m = power_mod(c, d, n)
   m
}

fn repeated_prime_factor(any: n, int: min_power=2, int: max_power=64): any {
   "Recover [p, r] when RSA modulus n is an exact prime power p^r.
   Returns nil if no exact prime power is found in the requested exponent range."
   def nn = Z(n)
   mut r = min_power
   while(r <= max_power){
      def p = nth_root(nn, r)
      if(p > Z(1) && is_prime(p)){
         mut acc = Z(1)
         mut i = 0
         while(i < r){
            acc = acc * p
            i += 1
         }
         if(acc == nn){ return [p, r] }
      }
      r += 1
   }
   nil
}

fn repeated_prime_phi(any: p, int: r): any {
   "Compute Euler phi(p^r) = p^(r-1) * (p-1)."
   def pp = Z(p)
   mut acc = Z(1)
   mut i = 1
   while(i < r){
      acc = acc * pp
      i += 1
   }
   acc * (pp - Z(1))
}
