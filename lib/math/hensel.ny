;; Keywords: hensel hensel-lifting math crypto
;; Hensel lifting: linear and nonlinear Hensel root lifting mod p^k
;; Reference:
;; - https://cacr.uwaterloo.ca/hac/about/chap2.pdf
;; - https://cacr.uwaterloo.ca/hac/about/chap14.pdf
;; References:
;; - std.math.crypto
module std.math.crypto.hensel(hensel_lift_linear, hensel_lift, hensel_roots, poly_eval_mod, poly_derivative, find_roots_mod_p)
use std.math.nt

fn poly_eval_mod(list poly, any x, any modulus) any {
   "Evaluate polynomial at x modulo modulus using Horner's method. Returns the result mod modulus."
   case poly.len {
      0 -> Z(0)
      _ -> {
         mut result, i = Z(0), poly.len - 1
         while(i >= 0){
            result = mod(result * Z(x) + Z(poly.get(i)), modulus)
            i = i - 1
         }
         result
      }
   }
}

fn poly_derivative(list poly) list {
   "Compute formal derivative of polynomial. Returns derivative as coefficient list."
   def n = poly.len
   if(n <= 1){ return [Z(0)] }
   mut result = list(0)
   mut i = 1
   while(i < n){
      result = result.append(Z(poly.get(i)) * Z(i))
      i += 1
   }
   result
}

fn find_roots_mod_p(list poly, int p) list {
   "Find all roots of polynomial mod p by brute force search over [0, p). Returns list of roots."
   def n = poly.len
   if(n == 0 || p <= 0){ return list(0) }
   mut roots = list(0)
   mut x = 0
   while(x < p){
      def val = poly_eval_mod(poly, x, p)
      if(val == Z(0)){ roots = roots.append(Z(x)) }
      x += 1
   }
   roots
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
   if(p <= 0 || k <= 0){ return list(0) }
   def pk = _int_pow(p, k)
   def pk1 = _int_pow(p, k + 1)
   mut lifted = list(0)
   mut ri = 0
   while(ri < roots.len){
      def root = Z(roots.get(ri))
      mut i = 0
      while(i < p){
         def candidate = root + Z(i) * pk
         if(poly_eval_mod(poly, candidate, pk1) == Z(0)){ lifted = lifted.append(candidate) }
         i += 1
      }
      ri += 1
   }
   lifted
}

fn hensel_lift(list poly, any r, int p, int k) any {
   "Hensel lifting: lift root r of poly mod p to mod p^k. poly is coefficient list [c0, c1, ..., cn]. Returns lifted root."
   if(k <= 1){ return Z(r) }
   mut roots = [Z(r)]
   mut power = 1
   while(power < k){
      roots = hensel_lift_linear(poly, p, power, roots)
      if(roots.len == 0){ return Z(r) }
      power += 1
   }
   roots.get(0)
}

fn hensel_roots(list poly, int p, int k) list {
   "Find all roots of poly mod p^k using Hensel lifting. Returns list of all lifted roots."
   if(k <= 0){ return list(0) }
   mut roots = find_roots_mod_p(poly, p)
   mut power = 1
   while(power < k){
      roots = hensel_lift_linear(poly, p, power, roots)
      power += 1
   }
   roots
}
