;; Keywords: integer modular-arithmetic math
;; Integer arithmetic, modular arithmetic, roots, divisibility, and number conversion.
;; References:
;; - std.math
module std.math.integer(Z, ZZ, Int, Integer, is_bigint, bigint, gcd, lcm, xgcd, egcd, mod, inverse_mod)
use std.math.big

fn Z(any n) bigint { bigint(n) }

fn ZZ(any n) bigint { Z(n) }

fn Int(any n) bigint { Z(n) }

fn Integer(any n) bigint { Z(n) }

fn _abs_z(any x) bigint {
   def z = Z(x)
   z < Z(0) ? Z(0) - z : z
}

fn gcd(any a, any b) bigint {
   "Computes the greatest common divisor."
   mut x = _abs_z(a)
   mut y = _abs_z(b)
   while(y != Z(0)){
      def r = x % y
      x = y
      y = r
   }
   x
}

fn lcm(any a, any b) bigint {
   "Computes the least common multiple."
   def za = Z(a)
   def zb = Z(b)
   if(za == Z(0) || zb == Z(0)){ return Z(0) }
   _abs_z((za / gcd(za, zb)) * zb)
}

fn mod(any a, any b) bigint {
   "Returns a non-negative modular remainder."
   def bb = Z(b)
   if(bb == Z(0)){ return Z(0) }
   mut r = Z(a) % bb
   if(r < Z(0)){ r = r + _abs_z(bb) }
   r
}

fn xgcd(any a, any b) list {
   "Computes the extended greatest common divisor."
   mut old_r = Z(a)
   mut r = Z(b)
   mut old_s = Z(1)
   mut s = Z(0)
   mut old_t = Z(0)
   mut t = Z(1)
   while(r != Z(0)){
      def q = old_r / r
      def next_r = old_r - q * r
      old_r = r
      r = next_r
      def next_s = old_s - q * s
      old_s = s
      s = next_s
      def next_t = old_t - q * t
      old_t = t
      t = next_t
   }
   [old_r, old_s, old_t]
}

fn egcd(any a, any b) list { xgcd(a, b) }

fn inverse_mod(any a, any m) bigint {
   "Computes a modular inverse when it exists."
   def mm = Z(m)
   if(mm == Z(0)){ return Z(0) }
   def eg = xgcd(a, mm)
   if(_abs_z(eg.get(0, Z(0))) != Z(1)){ return Z(0) }
   mod(eg.get(1, Z(0)), mm)
}
