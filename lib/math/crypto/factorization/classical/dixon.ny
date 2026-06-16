;; Keywords: factorization classical dixon euler gcd
;; References: std.math.crypto.factorization
module std.math.crypto.factorization.classical.dixon(dixon_factor, euler_factor)
use std.math.nt
use std.math.scalar as math
use std.os (ticks)
use std.math.crypto.factorization.classical.misc
use std.math.crypto.factorization.classical.qs

fn dixon_factor(any n, int max_base=256, int max_scan=20000) any {
   "Dixon-style congruence scan over prime bases.
   Returns [p, q] or nil."
   def nz = _z(n)
   if nz <= 1 { return nil }
   if nz % 2 == 0 { return [Z(2), nz / Z(2)] }
   mut start = isqrt(nz)
   if start * start < nz { start = start + Z(1) }
   mut base = [Z(2)]
   mut base_j2n = [mod(Z(4), nz)]
   mut bcount = 1
   while bcount <= int(max_base) {
      def lp = _z(base.get(base.len - 1, Z(2)))
      def target = _z(base_j2n.get(base_j2n.len - 1, Z(0)))
      mut x = start
      mut step = 0
      while step < int(max_scan) {
         def x2 = mod(x * x, nz)
         if x2 == target {
            def p = gcd(_z(x - lp), nz)
            if _is_nontrivial_factor(p, nz) { return [p, nz / p] }
         }
         x = x + Z(1)
         step += 1
      }
      def np = next_prime(lp)
      base = base.append(np)
      base_j2n = base_j2n.append(mod(np * np, nz))
      bcount += 1
   }
   nil
}

fn euler_factor(any n, any max_a=nil) any {
   "Euler factorization using two sum-of-squares representations.
   Returns [p, q] or nil."
   def nz = _z(n)
   if nz <= 1 { return nil }
   if nz % 2 == 0 { return [Z(2), nz / Z(2)] }
   mut end_a = isqrt(nz)
   if max_a != nil {
      def ma = _z(max_a)
      if ma < end_a { end_a = ma }
   }
   mut sols = []
   mut a = Z(0)
   mut first_b = Z(-1)
   while a <= end_a && sols.len < 2 {
      def rem = nz - a * a
      if rem >= 0 {
         def b = isqrt(rem)
         if b * b == rem && b != first_b && a != first_b {
            sols = sols.append([b, a])
            first_b = b
         }
      }
      a = a + Z(1)
   }
   if sols.len < 2 { return nil }
   def s0, s1 = sols.get(0, []), sols.get(1, [])
   def a0, b0 = _z(s0.get(0, 0)), _z(s0.get(1, 0))
   def c0, d0 = _z(s1.get(0, 0)), _z(s1.get(1, 0))
   def k, h = gcd(a0 - c0, d0 - b0), gcd(a0 + c0, d0 + b0)
   def m, l = gcd(a0 + c0, d0 - b0), gcd(a0 - c0, d0 + b0)
   def p, q = gcd(k * k + h * h, nz), gcd(l * l + m * m, nz)
   if _is_nontrivial_factor(p, nz) { return [p, nz / p] }
   if _is_nontrivial_factor(q, nz) { return [q, nz / q] }
   nil
}
