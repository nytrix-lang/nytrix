;; Keywords: rsa related-message
;; RSA related-message attacks routines.
;; Reference:
;; - https://people.csail.mit.edu/rivest/Rsapaper.pdf
;; - https://crypto.stanford.edu/~dabo/pubs/papers/RSA-survey.pdf
module std.math.crypto.rsa.related_message(franklin_reiter_attack, franklin_reiter_e3_successor)
use std.core
use std.math.nt
use std.math.crypto.poly

fn franklin_reiter_attack(any: n, int: e, any: c1, any: c2, list: f1_coeffs, list: f2_coeffs): any {
   "Franklin-Reiter related message attack.  Recovers the shared message m
   given two ciphertexts c1, c2 encrypted under the same key(n, e) where
   m is related by polynomials f1 and f2.  Returns m or nil."
   def list: g1 = poly_pow_mod(f1_coeffs, e, n)
   def c1_const = g1.get(0)
   poly_set_at(g1, 0, (c1_const - c1) % n)
   mut list: g2 = poly_pow_mod(f2_coeffs, e, n)
   def c2_const = g2.get(0)
   poly_set_at(g2, 0, (c2_const - c2) % n)
   def g = poly_gcd_mod(g1, g2, n)
   if(g.len == 2){
      def a, b = g.get(1), g.get(0)
      return((0 - b) * inverse_mod(a, n)) % n
   }
   nil
}

fn franklin_reiter_e3_successor(any: n, any: c_m, any: c_m_plus_1): any {
   "Closed-form Franklin-Reiter recovery for e=3 and related messages m, m+1.
   Given c_m = m^3 mod n and c_m_plus_1 = (m+1)^3 mod n, returns m or nil."
   def num = (Z(2) * c_m + c_m_plus_1 - Z(1)) % n
   def den = (c_m_plus_1 - c_m + Z(2)) % n
   def inv = inverse_mod(den, n)
   if(inv == nil){ return nil }
   (num * inv) % n
}

fn poly_pow_mod(list: p, int: e, any: m): list {
   "Compute p(x)^e mod m using binary exponentiation on polynomials.
   Returns the resulting polynomial coefficients."
   if(e == 0){ return [1] }
   mut res = [1]
   mut base = p
   mut exp = e
   while(exp > 0){
      if(exp % 2 == 1){ res = poly_mul_mod(res, base, m) }
      base = poly_mul_mod(base, base, m)
      exp = exp / 2
   }
   res
}

fn poly_mul_mod(list: a, list: b, any: m): list {
   "Multiply two polynomials a(x) and b(x) with all coefficients reduced mod m.
   Returns the product polynomial coefficients."
   def na, nb = a.len, b.len
   if(na == 0 || nb == 0){ return [] }
   def nr = na + nb - 1
   mut res = list(nr)
   mut i = 0
   while(i < nr){
      res = res.append(Z(0))
      i += 1
   }
   i = 0
   while(i < na){
      mut j = 0
      while(j < nb){
         def idx = i + j
         def cur = res.get(idx)
         def prod = (a.get(i) * b.get(j)) % m
         res.set(idx, (cur + prod) % m)
         j += 1
      }
      i += 1
   }
   res
}

fn poly_gcd_mod(list: a, list: b, any: m): list {
   "Compute the GCD of two polynomials a(x) and b(x) over Z_m.
   Returns the GCD polynomial coefficients."
   mut va, vb = a, b
   while(vb.len > 0){
      def r = poly_mod_mod(va, vb, m)
      va, vb = vb, r
   }
   va
}

fn poly_mod_mod(list: a, list: b, any: m): list {
   "Compute a(x) mod b(x) over Z_m via polynomial long division.
   Returns the remainder polynomial coefficients."
   def na, nb = a.len, b.len
   if(nb == 0){ return a }
   mut remainder = clone(a)
   def inv_lead_b = inverse_mod(b.get(nb - 1), m)
   mut deg_r = na - 1
   while(deg_r >= nb - 1){
      def coeff = (remainder.get(deg_r) * inv_lead_b) % m
      def shift = deg_r - nb + 1
      mut j = 0
      while(j < nb){
         def idx = shift + j
         def cur = remainder.get(idx)
         def sub = (coeff * b.get(j)) % m
         remainder.set(idx, (cur - sub) % m)
         j += 1
      }
      while(remainder.len > 0 && remainder.get(remainder.len - 1) % m == 0){
         remainder = slice(remainder, 0, remainder.len - 1)
      }
      deg_r = remainder.len - 1
   }
   remainder
}
