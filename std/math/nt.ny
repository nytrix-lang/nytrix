;; Keywords: math nt
;; Math Nt module.

module std.math.nt (
   egcd, modinv, pow_mod, is_prime, next_prime
)
use std.math *
use std.core *
use std.core.reflect *

fn egcd(a, b){
   "Extended Euclidean Algorithm. Returns [g, x, y] such that ax + by = g = gcd(a, b)."
   if(a == 0){
      return [b, 0, 1]
   }
   mut res = egcd(b % a, a)
   def g = get(res, 0)
   def y = get(res, 1)
   def x = get(res, 2)
   return [g, x - (b/a) * y, y]
}

fn modinv(a, m){
   "Modular inverse of a modulo m. Returns 0 if no inverse exists."
   mut res = egcd(a, m)
   def g = get(res, 0)
   def x = get(res, 1)
   if(g != 1){ return 0 }
   return (x % m + m) % m
}

fn pow_mod(base, exp, mod){
   "Modular exponentiation: (base^exp) % mod."
   mut mutable_exp = exp
   mut res = 1
   mut mutable_base = base % mod
   while(mutable_exp > 0){
      if(mutable_exp % 2 == 1){
         res = (res * mutable_base) % mod
      }
      mutable_base = (mutable_base * mutable_base) % mod
      mutable_exp = mutable_exp / 2
   }
   return res
}

fn is_prime(n){
   "Check if n is prime."
   if(n <= 1){ return 0 }
   if(n <= 3){ return 1 }
   if(n % 2 == 0 || n % 3 == 0){ return 0 }
   mut i = 5
   while(i * i <= n){
      if(n % i == 0 || n % (i + 2) == 0){ return 0 }
      i = i + 6
   }
   return 1
}

fn next_prime(n){
   "Return the smallest prime strictly greater than n."
   if(n <= 1){ return 2 }
   mut p = n
   while(1){
      p = p + 1
      if(is_prime(p)){ return p }
   }
}