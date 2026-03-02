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
      p += 1
      if(is_prime(p)){ return p }
   }
}

if(comptime{__main()}){
    use std.math.nt *
    use std.math *
    use std.core *
    use std.core.error *

    print("Testing Math Number Theory...")

    def res = egcd(10, 6)
    assert(get(res, 0) == 2, "egcd gcd")
    assert(get(res, 1) * 10 + get(res, 2) * 6 == 2, "egcd valid")

    assert(gcd(12, 18) == 6, "gcd")
    assert(lcm(12, 18) == 36, "lcm")

    assert(modinv(3, 11) == 4, "modinv 3 11")
    assert(modinv(2, 6) == 0, "modinv 2 6")

    assert(pow_mod(2, 10, 1000) == 24, "pow_mod")
    assert(pow_mod(2, 3, 5) == 3, "pow_mod small")

    assert(is_prime(2) == 1, "prime 2")
    assert(is_prime(3) == 1, "prime 3")
    assert(is_prime(4) == 0, "prime 4")
    assert(is_prime(7) == 1, "prime 7")
    assert(is_prime(10) == 0, "prime 10")
    assert(is_prime(97) == 1, "prime 97")
    assert(is_prime(100) == 0, "prime 100")

    assert(next_prime(10) == 11, "next_prime")

    print("✓ std.math.nt tests passed")
}
