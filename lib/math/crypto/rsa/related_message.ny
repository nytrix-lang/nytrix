;; Keywords: rsa related-message math crypto
;; RSA related-message attacks routines.
;; Reference:
;; - https://people.csail.mit.edu/rivest/Rsapaper.pdf
;; - https://crypto.stanford.edu/~dabo/pubs/papers/RSA-survey.pdf
;; References:
;; - std.math.crypto.rsa
;; - std.math.crypto
module std.math.crypto.rsa.related_message(franklin_reiter_attack, franklin_reiter_e3_successor, coppersmith_short_pad)
use std.core
use std.math.nt
use std.math.crypto.poly
use std.math.crypto.lattice.small_roots (modular_univariate)

fn franklin_reiter_attack(any n, int e, any c1, any c2, list f1_coeffs, list f2_coeffs) any {
   "Franklin-Reiter related message attack.  Recovers the shared message m
   given two ciphertexts c1, c2 encrypted under the same key(n, e) where
   m is related by polynomials f1 and f2.  Returns m or nil."
   def list g1 = _poly_pow_mod(f1_coeffs, e, n)
   def c1_const = g1.get(0)
   poly_set_at(g1, 0, (c1_const - c1) % n)
   mut list g2 = _poly_pow_mod(f2_coeffs, e, n)
   def c2_const = g2.get(0)
   poly_set_at(g2, 0, (c2_const - c2) % n)
   def g = _poly_gcd_mod(g1, g2, n)
   if g.len == 2 {
      def a, b = g.get(1), g.get(0)
      return ((0 - b) * inverse_mod(a, n)) % n
   }
   nil
}

fn franklin_reiter_e3_successor(any n, any c_m, any c_m_plus_1) any {
   "Closed-form Franklin-Reiter recovery for e=3 and related messages m, m+1.
   Given c_m = m^3 mod n and c_m_plus_1 = (m+1)^3 mod n, returns m or nil."
   def num = (Z(2) * c_m + c_m_plus_1 - Z(1)) % n
   def den = (c_m_plus_1 - c_m + Z(2)) % n
   def inv = inverse_mod(den, n)
   if inv == nil { return nil }
   (num * inv) % n
}

fn _poly_pow_mod(list p, int e, any m) list {
   "Compute p(x)^e mod m using binary exponentiation on polynomials.
   Returns the resulting polynomial coefficients."
   if e == 0 { return [1] }
   mut res = [1]
   mut base = p
   mut exp = e
   while exp > 0 {
      if exp % 2 == 1 { res = _poly_mul_mod(res, base, m) }
      base = _poly_mul_mod(base, base, m)
      exp = exp / 2
   }
   res
}

fn _poly_mul_mod(list a, list b, any m) list {
   "Multiply two polynomials a(x) and b(x) with all coefficients reduced mod m.
   Returns the product polynomial coefficients."
   def na, nb = a.len, b.len
   if na == 0 || nb == 0 { return [] }
   def nr = na + nb - 1
   mut res = list(nr)
   mut i = 0
   while i < nr {
      res = res.append(Z(0))
      i += 1
   }
   i = 0
   while i < na {
      mut j = 0
      while j < nb {
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

fn _poly_gcd_mod(list a, list b, any m) list {
   mut va, vb = a, b
   while vb.len > 0 {
      def r = _poly_mod_mod(va, vb, m)
      va, vb = vb, r
   }
   va
}

fn _lagrange_interpolate_mod(list points, list values, any modn) list {
   "Lagrange interpolate a polynomial of degree < points.len over Z/modn.
   Returns coefficient list in ascending degree order."
   def nn = Z(modn)
   def n = points.len
   mut result = list(n)
   mut i = 0
   while i < n {
      result = result.append(Z(0))
      i += 1
   }
   mut j = 0
   while j < n {
      def xj, yj = Z(points.get(j)), Z(values.get(j))
      mut numer = [Z(1)]
      mut denom = Z(1)
      mut k = 0
      while k < n {
         if k != j {
            def xk = Z(points.get(k))
            numer = numer.append(Z(0))
            mut i = numer.len - 1
            while i >= 1 {
               def val = (numer.get(i - 1) + mod(-xk, nn) * numer.get(i)) % nn
               numer.set(i, val)
               i -= 1
            }
            numer.set(0, mod(-xk * numer.get(0), nn))
            denom = denom * mod(xj - xk, nn) % nn
         }
         k += 1
      }
      def inv_denom = inverse_mod(denom, nn)
      if inv_denom == nil { return [] }
      mut i = 0
      while i < numer.len {
         def inc = yj * numer.get(i) % nn * inv_denom % nn
         result.set(i, (result.get(i) + inc) % nn)
         i += 1
      }
      j += 1
   }
   while result.len > 1 && result.get(result.len - 1) == Z(0) {
      result = slice(result, 0, result.len - 1)
   }
   result
}

fn coppersmith_short_pad(any n, int e, any c1, any c2, int m_param=3, int t_param=1) any {
   "Coppersmith's short-pad attack for RSA with small public exponent.
   Given two ciphertexts c1 = m^e mod n and c2 = (m+r)^e mod n where r
   is unknown but |r| < n^(1/e^2), recovers m.
   For e=3 this succeeds when the random padding is smaller than n^(1/9).
   Tunes lattice parameters m_param(shift count) and t_param(extra shifts)."
   def nn = Z(n)
   def cc1, cc2 = Z(c1), Z(c2)
   def deg_res = e * e

   mut f1 = [mod(-cc1, nn), Z(0), Z(0), Z(1)]
   if e > 3 {
      f1 = list(e + 1)
      mut i = 0
      while i <= e {
         f1 = f1.append(i == e ? Z(1) : (i == 0 ? mod(-cc1, nn) : Z(0)))
         i += 1
      }
   }

   mut points = []
   mut res_vals = []
   mut y_val = 0
   while y_val <= deg_res {
      def y = Z(y_val)
      mut f2 = list(e + 1)
      mut i = 0
      while i <= e {
         def binom = _binom(e, i)
         f2 = f2.append(binom * pow(y, Z(e - i)) % nn)
         i += 1
      }
      f2.set(0, mod(f2.get(0) - cc2, nn))

      def rv = poly_resultant_mod(f1, f2, nn)
      points = points.append(y_val)
      res_vals = res_vals.append(rv)
      y_val += 1
   }

   def h_poly = _lagrange_interpolate_mod(points, res_vals, nn)
   if h_poly.len == 0 { return nil }

   def n_bits = bit_length(nn)
   def delta_bits = n_bits / (e * e)
   def X = Z(1) << Z(delta_bits)

   def roots = modular_univariate(h_poly, nn, m_param, t_param, X)
   if roots.len == 0 { return nil }
   def r0 = Z(roots.get(0))

   def f2_shift = [r0, Z(1)]
   franklin_reiter_attack(nn, e, cc1, cc2, [Z(0), Z(1)], f2_shift)
}

fn _binom(int n, int k) bigint {
   mut res = Z(1)
   if k < 0 || k > n { return Z(0) }
   if k > n - k { k = n - k }
   mut i = 1
   while i <= k {
      res = res * Z(n - i + 1) / Z(i)
      i += 1
   }
   res
}

fn _poly_mod_mod(list a, list b, any m) list {
   def na, nb = a.len, b.len
   if nb == 0 { return a }
   mut remainder = clone(a)
   def inv_lead_b = inverse_mod(b.get(nb - 1), m)
   mut deg_r = na - 1
   while deg_r >= nb - 1 {
      def coeff = (remainder.get(deg_r) * inv_lead_b) % m
      def shift = deg_r - nb + 1
      mut j = 0
      while j < nb {
         def idx = shift + j
         def cur = remainder.get(idx)
         def sub = (coeff * b.get(j)) % m
         remainder.set(idx, (cur - sub) % m)
         j += 1
      }
      while remainder.len > 0 && remainder.get(remainder.len - 1) % m == 0 {
         remainder = slice(remainder, 0, remainder.len - 1)
      }
      deg_r = remainder.len - 1
   }
   remainder
}

#main {
   def p, q = Z(32416190071), Z(32416190039)
   def n = p * q
   def e = 3
   def m = Z(123456789)
   def r = Z(12)
   def c1 = power_mod(m, e, n)
   def c2 = power_mod(m + r, e, n)
   def recovered = coppersmith_short_pad(n, e, c1, c2)
   assert(recovered == m, "coppersmith_short_pad recovers message")
   print("✓ std.math.crypto.rsa.related_message self-test passed")
}
