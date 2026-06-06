;; Keywords: number-theory hensel hensel-lifting math crypto
;; Crypto number-theory routines for Hensel lifting over modular polynomial roots.
;; Polynomials are coefficient lists in ascending order: c0 + c1*x + c2*x^2 + ...
;; References:
;; - std.math.crypto.number
;; - std.math.crypto
module std.math.crypto.number.hensel(poly_eval_mod, hensel_lift_linear, hensel_roots)
use std.core
use std.math.nt

fn poly_eval_mod(list poly, any x, any m) any {
   "Evaluate a coefficient-list polynomial at x modulo m."
   mut acc = Z(0)
   mut pow_x = Z(1)
   mut i = 0
   while(i < poly.len){
      acc = mod(acc + Z(poly.get(i)) * pow_x, m)
      pow_x = mod(pow_x * Z(x), m)
      i += 1
   }
   acc
}

fn _int_pow(any base, int exp) any {
   mut out = Z(1)
   mut b = Z(base)
   mut e = exp
   while(e > 0){
      if(e % 2 == 1){ out = out * b }
      b, e = b * b, e / 2
   }
   out
}

fn hensel_lift_linear(list poly, int p, int k, list roots) list {
   "Lift roots of poly modulo p^k to roots modulo p^(k+1) by trying root + i*p^k.
   This brute-force linear lift also works for singular roots."
   def pk = _int_pow(p, k)
   def pk1 = _int_pow(p, k + 1)
   mut lifted = []
   mut ri = 0
   while(ri < roots.len){
      def root = roots.get(ri)
      mut i = 0
      while(i < p){
         def candidate = Z(root) + Z(i) * pk
         if(poly_eval_mod(poly, candidate, pk1) == Z(0)){ lifted = lifted.append(candidate) }
         i += 1
      }
      ri += 1
   }
   lifted
}

fn hensel_roots(list poly, int p, int k) list {
   "Find roots of a coefficient-list polynomial modulo p^k by brute root search mod p, then linear Hensel lifting."
   if(k <= 0){ return [] }
   mut roots = []
   mut r = 0
   while(r < p){
      if(poly_eval_mod(poly, r, p) == Z(0)){ roots = roots.append(Z(r)) }
      r += 1
   }
   mut power = 1
   while(power < k){
      roots = hensel_lift_linear(poly, p, power, roots)
      power += 1
   }
   roots
}
