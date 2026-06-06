;; Keywords: ecc edwards math crypto public-key
;; Elliptic-curve routines for Edwards-curve arithmetic.
;; Complete addition formulas for Edwards: x^2 + y^2 = 1 + d*x^2*y^2
;; and Twisted Edwards: a*x^2 + y^2 = 1 + d*x^2*y^2
;; Reference:
;; - https://www.secg.org/sec1-v2.pdf
;; - https://www.rfc-editor.org/rfc/rfc8032
;; References:
;; - std.math.crypto.ecc
;; - std.math.crypto
module std.math.crypto.ecc.edwards(edwards_point_add, edwards_scalar_mult, edwards_is_on_curve, twisted_edwards_add, twisted_edwards_scalar_mult)
use std.math.nt
use std.math.crypto.ecc

fn edwards_is_on_curve(any x, any y, any d, any p) bool {
   "Check whether(x,y) lies on Edwards curve x^2 + y^2 = 1 + d*x^2*y^2 over F_p."
   def x2, y2 = (x * x) % p, (y * y) % p
   def lhs, rhs = (x2 + y2) % p, (1 + d * x2 % p * y2 % p) % p
   lhs == rhs
}

fn edwards_point_add(any P, any Q, any d, any p) any {
   "Add points P and Q on Edwards curve x^2 + y^2 = 1 + d*x^2*y^2 over F_p " +
   "using complete addition. Returns [x3,y3] or nil."
   if(P == nil){ return Q }
   if(Q == nil){ return P }
   def x1, y1 = P[0], P[1]
   def x2, y2 = Q[0], Q[1]
   def x1x2 = (x1 * x2) % p
   def y1y2 = (y1 * y2) % p
   def dx1x2y1y2 = (d * x1x2 % p * y1y2 % p) % p
   def den1 = (1 + dx1x2y1y2) % p
   mut den2 = (1 - dx1x2y1y2) % p
   if(den2 < 0){ den2 = den2 + p }
   def den1_inv, den2_inv = inverse_mod(den1, p), inverse_mod(den2, p)
   mut x3, y3 = ((x1 * y2 + y1 * x2) % p * den1_inv) % p, ((y1 * y2 - x1 * x2) % p * den2_inv) % p
   if(y3 < 0){ y3 = y3 + p }
   if(x3 < 0){ x3 = x3 + p }
   [x3, y3]
}

fn edwards_scalar_mult(any k, any P, any d, any p) any {
   "Multiply Edwards point P by scalar k via double-and-add on x^2 + y^2 = 1 + d*x^2*y^2."
   if(k == 0){ return nil }
   if(P == nil){ return nil }
   mut Q, R = nil, P
   mut kb = k
   while(kb > 0){
      if(kb & 1 != 0){ Q = edwards_point_add(Q, R, d, p) }
      R = edwards_point_add(R, R, d, p)
      kb = kb >> 1
   }
   Q
}

fn twisted_edwards_add(any P, any Q, any a, any d, any p) any {
   "Add points P and Q on Twisted Edwards a*x^2 + y^2 = 1 + d*x^2*y^2 over F_p. " +
   "Returns [x3,y3] or nil."
   if(P == nil){ return Q }
   if(Q == nil){ return P }
   def x1, y1 = P[0], P[1]
   def x2, y2 = Q[0], Q[1]
   def x1x2 = (x1 * x2) % p
   def y1y2 = (y1 * y2) % p
   def dx1x2y1y2 = (d * x1x2 % p * y1y2 % p) % p
   def den1 = (1 + dx1x2y1y2) % p
   mut den2 = (1 - dx1x2y1y2) % p
   if(den2 < 0){ den2 = den2 + p }
   def den1_inv, den2_inv = inverse_mod(den1, p), inverse_mod(den2, p)
   mut x3 = ((x1 * y2 + y1 * x2) % p * den1_inv) % p
   def a_x1x2 = (a * x1 * x2) % p
   mut y3 = ((y1 * y2 - a_x1x2) % p * den2_inv) % p
   if(y3 < 0){ y3 = y3 + p }
   if(x3 < 0){ x3 = x3 + p }
   [x3, y3]
}

fn twisted_edwards_scalar_mult(any k, any P, any a, any d, any p) any {
   "Multiply Twisted Edwards point P by scalar k via double-and-add on " +
   "a*x^2 + y^2 = 1 + d*x^2*y^2."
   if(k == 0){ return nil }
   if(P == nil){ return nil }
   mut Q, R = nil, P
   mut kb = k
   while(kb > 0){
      if(kb & 1 != 0){ Q = twisted_edwards_add(Q, R, a, d, p) }
      R = twisted_edwards_add(R, R, a, d, p)
      kb = kb >> 1
   }
   Q
}
