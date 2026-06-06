;; Keywords: ecc smart-attack math crypto public-key
;; Elliptic-curve routines for Smart attack for anomalous elliptic curves.
;; Recovers k where Q = k*P on curves with tr(E) = 1 and #E(Fp) = p
;; Uses p-adic lifting to Q_p and computes phi(P) = -x/y mod p
;; Reference:
;; - https://www.secg.org/sec1-v2.pdf
;; - https://www.rfc-editor.org/rfc/rfc8032
;; References:
;; - std.math.crypto.ecc
;; - std.math.crypto
module std.math.crypto.ecc.smart_attack(smart_attack)
use std.math.nt
use std.math.crypto.ecc.ecc
use std.os.prim as os

fn smart_attack(any Px, any Py, any Qx, any Qy, any a, any p) any {
   "Smart's attack on anomalous curves. Recovers scalar k where Q = k*P on a curve y^2 = x^3+ax+b over F_p with #E(F_p) = p(anomalous curve).
   Implementation note:
   - A naive phi(P)=-x/y mod p using the original affine points can fail.
   - The standard attack lifts to mod p^2 and uses phi(pP)=-x(pP)/y(pP) mod p.
   Returns scalar k, or -1 on failure."
   def lifted = _smart_attack_p2(Px, Py, Qx, Qy, a, p)
   if(lifted != -1){ return lifted }
   _smart_attack_tiny_dlp(Px, Py, Qx, Qy, a, p)
}

fn smart_phi(any x, any y, any p) any {
   "Compute the p-adic canonical height mapping phi(P) = -x/y mod p for point P = (x, y) on an anomalous curve. Returns the height value mod p."
   if(y == 0){ return 0 }
   def y_inv = inverse_mod(y, p)
   mut result = (0 - x) * y_inv % p
   if(result < 0){ result = result + p }
   result
}

fn _hensel_lift_y(any x, any y0, any a, any b, any p) any {
   "Lift y from mod p to mod p^2 for curve y^2 = x^3 + a x + b.
   Assumes y0^2 == rhs mod p and gcd(2*y0, p)=1."
   def p2 = p * p
   def xx = mod(x, p2)
   def rhs = mod(mod(mod(xx*xx*xx, p2) + mod(a*xx, p2), p2) + b, p2)
   def y0m = mod(y0, p)
   def diff = rhs - (y0m * y0m)
   def e = mod(diff / p, p)
   def inv2y = inverse_mod(mod(Z(2) * y0m, p), p)
   if(inv2y == 0){ return nil }
   def t = mod(e * inv2y, p)
   mod(y0m + t * p, p2)
}

fn _smart_phi_from_jac_divp(any Pj, any p, any p2) any {
   "Compute phi(pP) style mapping from a Jacobian point over Z/p^2Z without affine conversion.
   For Jacobian(X:Y:Z), affine x=X/Z^2, y=Y/Z^3, so -x/y = -(X*Z)/Y.
   In Smart's attack at precision 2, points p*P and p*Q lie in the formal group:
   X*Z and Y are divisible by p, so we can divide numerator+denominator by p and
   compute the ratio mod p."
   if(Pj == nil){ return -1 }
   def X, Y = mod(Pj[0], p2), mod(Pj[1], p2)
   def Zz = mod(Pj[2], p2)
   def num = mod((Z(0) - X) * Zz, p2)
   if(mod(num, p) != Z(0)){
      if(mod(Y, p) != Z(0)){
         def inv0 = inverse_mod(mod(Y, p), p)
         if(inv0 == Z(0)){ return -1 }
         return mod(mod(num, p) * inv0, p)
      }
      return -1
   }
   def nump = mod(num / p, p)
   if(mod(Y, p) != Z(0)){
      def inv0 = inverse_mod(mod(Y, p), p)
      if(inv0 == Z(0)){ return -1 }
      return mod(nump * inv0, p)
   }
   def denp = mod(Y / p, p)
   if(denp == Z(0)){ return -1 }
   def inv = inverse_mod(denp, p)
   if(inv == Z(0)){ return -1 }
   mod(nump * inv, p)
}

fn _smart_attack_tiny_dlp(any Px, any Py, any Qx, any Qy, any a, any p) any {
   "Deterministic verification fallback for tiny anomalous-curve vectors.
   The p-adic path is the real attack ; this bounded path keeps small published
   fixtures strict instead of accepting non-nil smoke checks."
   if(!is_int(p) || p <= 0 || p > 4096){ return -1 }
   def P = [mod(Px, p), mod(Py, p)]
   def qx, qy = mod(Qx, p), mod(Qy, p)
   mut k = 1
   while(k < p){
      def R = ecc_scalar_mult(k, P, a, p)
      if(R != nil && mod(R[0], p) == qx && mod(R[1], p) == qy){ return k }
      k += 1
   }
   -1
}

fn _smart_attack_p2(any Px, any Py, any Qx, any Qy, any a, any p) any {
   use std.math.crypto.ecc.ecc
   def x1, y1 = mod(Px, p), mod(Py, p)
   def x2, y2 = mod(Qx, p), mod(Qy, p)
   mut b_p = mod(y1*y1 - x1*x1*x1 - a*x1, p)
   if(b_p < 0){ b_p = b_p + p }
   def p2 = p * p
   mut ra = Z(0)
   while(ra <= Z(8)){
      mut rb = Z(0)
      while(rb <= Z(8)){
         def a2, b2 = a + ra * p, b_p + rb * p
         def Y1, Y2 = _hensel_lift_y(x1, y1, a2, b2, p), _hensel_lift_y(x2, y2, a2, b2, p)
         if(Y1 != nil && Y2 != nil){
            def P, Q = [x1, Y1], [x2, Y2]
            def pPj = ecc_scalar_mult_jacobian(p, ecc_to_jacobian(P, p2), a2, p2)
            def pQj = ecc_scalar_mult_jacobian(p, ecc_to_jacobian(Q, p2), a2, p2)
            if(pPj != nil && pQj != nil){
               def phiP, phiQ = _smart_phi_from_jac_divp(pPj, p, p2), _smart_phi_from_jac_divp(pQj, p, p2)
               if(os.env("NYTRIX_SMART_TRACE") != 0){
                  print("[smart] ra=" + to_str(ra) + " rb=" + to_str(rb) +
                  " phiP=" + to_str(phiP) + " phiQ=" + to_str(phiQ))
               }
               if(phiP >= 0 && phiQ >= 0 && phiP != 0){
                  def inv = inverse_mod(phiP, p)
                  if(inv != 0){
                     def k = mod(phiQ * inv, p)
                     if(k != 0){ return k }
                  }
               }
            }
         }
         rb += Z(1)
      }
      ra += Z(1)
   }
   -1
}
