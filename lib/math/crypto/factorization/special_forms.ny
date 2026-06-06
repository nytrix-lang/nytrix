;; Keywords: factorization special-forms math crypto number-theory
;; Integer-factorization routines for factorization for special algebraic forms.
;; References:
;; - std.math.crypto.factorization
;; - std.math.crypto
module std.math.crypto.factorization.special_forms(factor_2pn, factor_xyxz)
use std.math.nt

fn _z(any x) any { is_bigint(x) ? x : Z(x) }

fn _ceil_isqrt(any n) any {
   mut s = isqrt(n)
   if(s * s < _z(n)){ s = s + Z(1) }
   s
}

fn factor_2pn(any N, any P=3) any {
   "Factor N with the 2PN midpoint trick.
   Useful when sqrt(2*P*N) is close to(P*p + 2*q)/2."
   def nz, pz = _z(N), _z(P)
   if(nz <= 0 || pz <= 2 || pz % Z(2) == 0){ return nil }
   def P2N = (pz * nz) << 1
   def A = _ceil_isqrt(P2N)
   def c = -(A * A) + A + P2N
   def disc = Z(1) - (c << 2)
   if(disc < 0){ return nil }
   def sd = isqrt(disc)
   if(sd * sd != disc){ return nil }
   def roots = [(-Z(1) + sd) >> 1, (-Z(1) - sd) >> 1]
   mut i = 0
   while(i < roots.len){
      def x = _z(roots.get(i, 0))
      if(x >= 0){
         def p1, q1 = (A + x) / pz, (A - x - Z(1)) >> 1
         if(p1 > 1 && q1 > 1 && p1 * q1 == nz){ return [p1, q1] }
         def p2, q2 = (A - x - Z(1)) / pz, (A + x) >> 1
         if(p2 > 1 && q2 > 1 && p2 * q2 == nz){ return [p2, q2] }
      }
      i += 1
   }
   nil
}

fn factor_xyxz(any n, any base=3, any max_power=nil) any {
   "Factor n in x^y*x^z style scans by testing next_prime(base^k)."
   def nz = _z(n)
   def b = _z(base)
   if(nz <= 1 || b <= 1){ return nil }
   mut maxp = 0
   if(max_power == nil){ maxp = max(1, (bit_length(nz) / max(1, bit_length(b)) + 1) / 2) } else { maxp = int(max_power) }
   mut power = 1
   while(power <= maxp){
      def p = next_prime(bigint_pow(b, Z(power)))
      if(p > 1 && nz % p == 0){ return [p, nz / p] }
      power += 1
   }
   nil
}

#main {
   def p = next_prime(bigint_pow(Z(3), Z(4)))
   def q = Z(101)
   def n = p * q
   assert(factor_xyxz(n, 3, 8) == [p, q], "special forms xyxz factor")
   assert(factor_2pn(Z(11) * Z(13), 3) == [Z(11), Z(13)], "special forms 2pn factor")
   print("✓ std.math.crypto.factorization.special_forms self-test passed")
}
