;; Keywords: ecc montgomery math crypto public-key
;; Elliptic-curve routines for Montgomery-curve arithmetic.
;; Reference:
;; - https://cr.yp.to/ecdh/curve25519-20060209.pdf
;; References:
;; - std.math.crypto.ecc
;; - std.math.crypto
module std.math.crypto.ecc.montgomery(montgomery_is_on_curve, montgomery_negate, montgomery_point_add, montgomery_point_double, montgomery_scalar_mult, montgomery_base_point)
use std.math.nt

fn montgomery_is_on_curve(any P, any A, any B, any p) bool {
   "Return true when P=[x,y] lies on B*y^2 = x^3 + A*x^2 + x over Fp."
   if P == nil { return true }
   def x, y = Z(P[0]), Z(P[1])
   def lhs, rhs = mod(B * y * y, p), mod(x * x * x + A * x * x + x, p)
   lhs == rhs
}

fn montgomery_negate(any P, any p) any {
   "Return -P for a Montgomery affine point."
   if P == nil { return nil }
   [Z(P[0]), mod(-Z(P[1]), p)]
}

fn montgomery_point_double(any P, any A, any B, any p) any {
   "Double a Montgomery affine point."
   if P == nil { return nil }
   def x, y = Z(P[0]), Z(P[1])
   if mod(y, p) == Z(0) { return nil }
   def m = mod((Z(3) * x * x + Z(2) * A * x + Z(1)) * inverse_mod(Z(2) * B * y, p), p)
   def rx = mod(B * m * m - A - Z(2) * x, p)
   def ry = mod(m * (x - rx) - y, p)
   [rx, ry]
}

fn montgomery_point_add(any P, any Q, any A, any B, any p) any {
   "Add two Montgomery affine points."
   if P == nil { return Q }
   if Q == nil { return P }
   def px, py = Z(P[0]), Z(P[1])
   def qx, qy = Z(Q[0]), Z(Q[1])
   if px == qx {
      if mod(py + qy, p) == Z(0) { return nil }
      return montgomery_point_double(P, A, B, p)
   }
   def m = mod((qy - py) * inverse_mod(qx - px, p), p)
   def rx = mod(B * m * m - A - px - qx, p)
   def ry = mod(m * (px - rx) - py, p)
   [rx, ry]
}

fn montgomery_scalar_mult(any k, any P, any A, any B, any p) any {
   "Scalar multiply a Montgomery affine point with a left-to-right ladder."
   mut kk = Z(k)
   if P == nil || kk == Z(0) { return nil }
   mut base = P
   if kk < Z(0) {
      kk = -kk
      base = montgomery_negate(P, p)
   }
   mut r0, r1 = nil, base
   mut bit = bit_length(kk) - 1
   while bit >= 0 {
      def is_one = ((kk / (Z(1) << bit)) % Z(2)) != Z(0)
      if is_one {
         r0, r1 = montgomery_point_add(r0, r1, A, B, p), montgomery_point_double(r1, A, B, p)
      } else {
         r1, r0 = montgomery_point_add(r0, r1, A, B, p), montgomery_point_double(r0, A, B, p)
      }
      bit -= 1
   }
   r0
}

fn montgomery_base_point(any x, any A, any B, any p) any {
   "Return one affine point with the given x-coordinate, or nil if none exists."
   def xx = Z(x)
   def rhs = mod((xx * xx * xx + A * xx * xx + xx) * inverse_mod(B, p), p)
   def y = tonelli_shanks(rhs, p)
   if y == nil { return nil }
   [xx, y]
}
