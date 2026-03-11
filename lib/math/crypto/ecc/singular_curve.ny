;; Keywords: ecc singular-curve
;; Singular-curve attack routines for elliptic-curve discrete logs.
;; Singular curves have discriminant = 0 mod p and DLP reduces to F_p^* or F_p^+
;; Reference:
;; - https://www.secg.org/sec1-v2.pdf
;; - https://www.rfc-editor.org/rfc/rfc8032
module std.math.crypto.ecc.singular_curve(detect_singular, singular_curve_dlp)
use std.math.nt
use std.math.crypto.ecc

fn detect_singular(any: a, any: b, any: p): bool {
   "Check whether y^2 = x^3 + ax + b is singular mod p. " +
   "Singular iff discriminant 4a^3 + 27b^2 == 0 mod p."
   def a2, a3 = (a * a) % p, (a2 * a) % p
   def b2 = (b * b) % p
   def disc = (4 * a3 + 27 * b2) % p
   disc == 0
}

fn singular_curve_dlp(any: Px, any: Py, any: Qx, any: Qy, any: a, any: b, any: p): any {
   "Solve DLP on singular y^2 = x^3 + ax + b over F_p. " +
   "Given P=(Px,Py), Q=(Qx,Qy), find k with Q=k*P via isomorphism to F_p^* (node) " +
   "or F_p^+ (cusp). Returns k or nil."
   if(!detect_singular(a, b, p)){ return nil }
   def x0 = singular_point(a, b, p)
   if(x0 < 0){ return nil }
   def curve_type = classify_singularity(a, b, x0, p)
   if(curve_type == 1){
      def k = solve_dlp_multiplicative(Px, Py, Qx, Qy, x0, p)
      k
   } else {
      def k = solve_dlp_additive(Px, Py, Qx, Qy, x0, p)
      k
   }
}

fn singular_point(any: a, any: b, any: p): any {
   "Find x-coordinate of singular point on y^2 = x^3 + ax + b mod p. " +
   "Singular point satisfies f(x)=f'(x)=0. Returns x or -1."
   def a2, a3 = (a * a) % p, (a2 * a) % p
   def b2 = (b * b) % p
   def disc = (4 * a3 + 27 * b2) % p
   if(disc != 0){ return -1 }
   def a_val = a
   mut x0 = 0
   while(x0 < p){
      def f = (x0 * x0 * x0 + a_val * x0 + b) % p
      def fp = (3 * x0 * x0 + a_val) % p
      if(f == 0 && fp == 0){ return x0 }
      x0 += 1
   }
   -1
}

fn classify_singularity(any: a, any: b, any: x0, any: p): int {
   "Classify singularity at x0 on y^2 = x^3 + ax + b mod p. " +
   "Returns 1 for node(two tangents) or 0 for cusp(single tangent)."
   def f2 = (6 * x0) % p
   if(f2 != 0){
      def second = (6 * x0 * x0 + a) % p
      if(second == 0){ 0 } else { 1 }
   } else {
      0
   }
}

fn solve_dlp_multiplicative(any: Px, any: Py, any: Qx, any: Qy, any: x0, any: p): any {
   "Solve DLP via F_p^* isomorphism for nodal singular curve. " +
   "Maps points to multiplicative group and solves discrete log."
   def tP_num = (Py) % p
   mut tP_den = (Px - x0) % p
   if(tP_den < 0){ tP_den = tP_den + p }
   if(tP_den == 0){ return nil }
   def tP_den_inv = inverse_mod(tP_den, p)
   def tP = (tP_num * tP_den_inv) % p
   def tQ_num = (Qy) % p
   mut tQ_den = (Qx - x0) % p
   if(tQ_den < 0){ tQ_den = tQ_den + p }
   if(tQ_den == 0){ return nil }
   def tQ_den_inv = inverse_mod(tQ_den, p)
   def tQ = (tQ_num * tQ_den_inv) % p
   if(tP == 0 || tQ == 0){ return nil }
   def order = p - 1
   def k = solve_dlog_in_group(tP, tQ, p, order)
   k
}

fn solve_dlp_additive(any: Px, any: Py, any: Qx, any: Qy, any: x0, any: p): any {
   "Solve DLP via F_p^+ isomorphism for cuspidal singular curve. " +
   "Maps points to additive group and solves discrete log."
   def uP_num = (Px - x0) % p
   mut uP_den = (Py) % p
   if(uP_den < 0){ uP_den = uP_den + p }
   if(uP_den == 0){ return nil }
   def uP_den_inv = inverse_mod(uP_den, p)
   def uP = (uP_num * uP_den_inv) % p
   def uQ_num = (Qx - x0) % p
   mut uQ_den = (Qy) % p
   if(uQ_den < 0){ uQ_den = uQ_den + p }
   if(uQ_den == 0){ return nil }
   def uQ_den_inv = inverse_mod(uQ_den, p)
   def uQ = (uQ_num * uQ_den_inv) % p
   if(uP == 0){ return nil }
   def uP_inv = inverse_mod(uP, p)
   def k = (uQ * uP_inv) % p
   k
}

fn solve_dlog_in_group(any: g, any: h, any: p, any: order): any {
   "Solve discrete log g^k = h in F_p^* of given order using brute force. Returns k or nil if not found."
   mut val = 1
   mut k = 0
   while(k < order){
      if(val == h){ return k }
      val = (val * g) % p
      k += 1
   }
   nil
}
