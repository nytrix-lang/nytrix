;; Keywords: factorization shor math crypto number-theory
;; Integer-factorization routines for Shor-style order-finding support.
;; Given an order relation a^s = 1 mod n, recover non-trivial factors.
;; References:
;; - std.math.crypto.factorization
;; - std.math.crypto
module std.math.crypto.factorization.shor(shor_factor_from_order)
use std.math.nt

fn shor_factor_from_order(any n, any a, any s) any {
   "Recover a non-trivial factor of n given a^s = 1 mod n and the order s of a.
   Tries all divisors of s as in the classical post-processing step."
   if(power_mod(Z(a), Z(s), Z(n)) != Z(1)){ return nil }
   def ds = divisors(Z(s))
   mut i = 0
   while(i < ds.len){
      def r = ds.get(i)
      def br = power_mod(Z(a), Z(s) / r, Z(n))
      def p = gcd(br - Z(1), Z(n))
      if(p > Z(1) && p < Z(n) && Z(n) % p == Z(0)){ return [p, Z(n) / p] }
      i += 1
   }
   nil
}
