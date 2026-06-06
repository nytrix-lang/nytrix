;; Keywords: rsa mersenne-exponent math crypto
;; RSA attacks involving Mersenne-shaped exponents routines.
;; Reference:
;; - https://people.csail.mit.edu/rivest/Rsapaper.pdf
;; References:
;; - std.math.crypto.rsa
;; - std.math.crypto
module std.math.crypto.rsa.mersenne_exponent(rsa_mersenne_plaintext_from_factors, rsa_mersenne_plaintext_scan_from_factors)
use std.math.nt
use std.math.bin
use std.core.str as str

fn _rsa_mersenne_plain_from_power(any mg, any g) any {
   if(g == Z(1)){ return Z(mg).bytes.text }
   def gi = bigint_to_int(g)
   if(gi <= 1 || gi > 1000000){ return nil }
   def m = nth_root(mg, gi)
   if(m == nil || bigint_pow(m, g) != mg){ return nil }
   Z(m).bytes.text
}

fn rsa_mersenne_plaintext_from_factors(any n, any c, any p, any q, int k) any {
   "Decrypt RSA where e = 2^k - 1 and factors p,q are known.
   If gcd(e, phi)>1, returns the exact g-th-root plaintext when it exists.
   Returns [k, gcd(e, phi), plaintext] or nil."
   if(k <= 0){ return nil }
   def nn, pp, qq = Z(n), Z(p), Z(q)
   def e = (Z(1) << Z(k)) - Z(1)
   def phi = (pp - Z(1)) * (qq - Z(1))
   def g = gcd(e, phi)
   if(g == Z(0)){ return nil }
   def d = inverse_mod(e / g, phi)
   if(d == nil){ return nil }
   def dp = mod(d, pp - Z(1))
   def dq = mod(d, qq - Z(1))
   def mp = power_mod(mod(c, pp), dp, pp)
   def mq = power_mod(mod(c, qq), dq, qq)
   def qinv = inverse_mod(qq, pp)
   if(qinv == nil){ return nil }
   def h = mod((mp - mq) * qinv, pp)
   def mg = mq + h * qq
   def pt = _rsa_mersenne_plain_from_power(mg, g)
   if(pt == nil){ return nil }
   [k, g, pt]
}

fn rsa_mersenne_plaintext_scan_from_factors(
   any n, any c, any p, any q, int max_k, str prefix="", str suffix=""
) any {
   "Scan k for e = 2^k - 1 and return [k, gcd(e, phi), plaintext].
   Optional prefix/suffix filters keep plaintext selection deterministic."
   def phi = (Z(p) - Z(1)) * (Z(q) - Z(1))
   mut k = 1
   while(k <= max_k){
      def e = (Z(1) << Z(k)) - Z(1)
      def g = gcd(e, phi)
      def gi = bigint_to_int(g)
      if(g == Z(1) || gi <= 1 || gi > 64){
         k += 1
         continue
      }
      def hit = rsa_mersenne_plaintext_from_factors(n, c, p, q, k)
      if(hit != nil){
         def pt = hit[2]
         if((prefix == "" || str.startswith(pt, prefix)) &&
            (suffix == "" || str.endswith(pt, suffix))){
            return hit
         }
      }
      k += 1
   }
   nil
}
