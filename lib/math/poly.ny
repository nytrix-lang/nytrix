;; Keywords: poly polynomial
;; Polynomial arithmetic, evaluation, modular reduction, interpolation, and roots.
;; Reference:
;; - https://cacr.uwaterloo.ca/hac/about/chap2.pdf
;; - https://cacr.uwaterloo.ca/hac/about/chap14.pdf
module std.math.crypto.poly(poly_add, poly_mod_add, poly_mul, poly_eval, poly_small_roots, poly_gcd,
   poly_set_at, poly_derivative_mod, poly_mod_eval,
   poly_mod_gcd, poly_mod_div, poly_mod_mul, poly_mod_pow, poly_factor_cz,
   poly_subproduct_tree, poly_multipoint_eval, poly_interpolate, poly_hgcd,
   poly_mod_gcd_fast, poly_sylvester_matrix, poly_resultant, poly_resultant_mod,
   poly_resultant_quadratic_xn_minus_const_mod, poly_mod_roots,
   poly2_new, poly2_add, poly2_mul, poly2_eval,
poly2_to_univariate_x, poly2_to_univariate_y, poly2_resultant_x)

use std.math.nt
use std.math.crypto.ntt as ntt
use std.math.matrix

fn poly_set_at(list: poly, int: idx, any: val): list {
   "Internal: Set polynomial coefficient at index idx to val. Returns modified polynomial."
   if(!is_list(poly)){ return poly }
   if(idx < 0 || idx >= poly.len){ return poly }
   poly[idx] = val
   poly
}

fn poly_add(list: a, list: b): list {
   "Add two polynomials coefficient-wise, returning new polynomial result."
   def na, nb = a.len, b.len
   mut n = (na > nb) ? na : nb
   mut result = []
   mut i = 0
   while(i < n){
      def va, vb = (i < na) ? a[i] : 0, (i < nb) ? b[i] : 0
      result = result.append(va + vb)
      i += 1
   }
   result
}

fn poly_mod_add(list: a, list: b, any: p): list {
   "Add two polynomials modulo p."
   def na, nb = a.len, b.len
   mut n = (na > nb) ? na : nb
   mut result = []
   mut i = 0
   while(i < n){
      def va, vb = (i < na) ? a[i] : 0, (i < nb) ? b[i] : 0
      result = result.append(mod(va + vb, p))
      i += 1
   }
   result
}

fn poly_mul(list: a, list: b): list {
   "Multiply two polynomials using convolution, returning new polynomial result."
   def na, nb = a.len, b.len
   if(na == 0 || nb == 0){ return [] }
   def nr = na + nb - 1
   mut result = []
   mut i = 0
   while(i < nr){ result = result.append(0) i += 1 }
   i = 0
   while(i < na){
      mut j = 0
      while(j < nb){
         mut idx = i + j
         def cur = result[idx]
         def prod = a[i] * b[j]
         result[idx] = cur + prod
         j += 1
      }
      i += 1
   }
   result
}

fn poly_eval(list: poly, any: x): any {
   "Evaluate polynomial at point x using Horner's method. Returns the computed value."
   mut n = poly.len
   if(n == 0){ return 0 }
   mut result = 0
   mut i = n - 1
   while(i >= 0){
      result = result * x + poly[i]
      i = i - 1
   }
   result
}

fn poly_mod_eval(list: poly, any: x, any: modulus): any {
   "Evaluate polynomial at x modulo modulus using Horner's method with modular reduction at each step."
   mut n = poly.len
   if(n == 0){ return 0 }
   mut result = 0
   mut i = n - 1
   while(i >= 0){
      result = (result * x + poly[i]) % modulus
      i = i - 1
   }
   result
}

fn poly_small_roots(list: poly, int: bound): list {
   "Find all integer roots of poly with absolute value <= bound by brute force search."
   mut roots = list(0)
   mut x = 0 - bound
   while(x <= bound){
      mut val = poly_eval(poly, x)
      if(val == 0){ roots = roots.append(x) }
      x += 1
   }
   roots
}

fn poly_derivative(list: poly): list {
   "Compute formal derivative of polynomial. Returns derivative as coefficient list."
   mut n = poly.len
   if(n <= 1){ return [0] }
   mut result = list(n - 1)
   mut i = 1
   while(i < n){
      result = result.append(poly[i] * i)
      i += 1
   }
   result
}

fn poly_derivative_mod(list: poly, any: modulus): list {
   "Compute formal derivative of polynomial with coefficients reduced modulo modulus."
   mut n = poly.len
   if(n <= 1){ return [0] }
   mut result = list(n - 1)
   mut i = 1
   while(i < n){
      result = result.append((poly[i] * i) % modulus)
      i += 1
   }
   result
}

fn poly_modulus(list: a, list: b): list {
   "Polynomial remainder a mod b over the ambient coefficient ring."
   if(b.len == 0){ return clone(a) }
   mut r = clone(a)
   def db = b.len - 1
   def lead = b[db]
   if(lead == 0){ return r }
   while(r.len >= b.len && r.len > 0){
      def dr = r.len - 1
      def factor = r[dr] / lead
      mut i = 0
      while(i < b.len){
         def idx = dr - db + i
         r[idx] = r[idx] - factor * b[i]
         i += 1
      }
      while(r.len > 0 && r[r.len - 1] == 0){ r.pop() }
   }
   r
}

fn poly_gcd(list: a, list: b): list {
   "Compute GCD of two polynomials using Euclidean algorithm. Returns the GCD polynomial."
   while(b.len > 0){
      mut r = poly_modulus(a, b)
      a, b = b, r
   }
   a
}

fn poly_mod_gcd(list: a, list: b, any: p): list {
   "GCD of polynomials a, b modulo p."
   mut u, v = clone(a), clone(b)
   while(v.len > 1 || (v.len == 1 && v[0] != 0)){
      def r = poly_mod_div(u, v, p).get(1)
      u, v = v, r
   }
   def lc = u[u.len - 1]
   if(lc == 1 || lc == 0){ return u }
   def lci = inverse_mod(lc, p)
   mut res = []
   mut i = 0
   while(i < u.len){
      res = res.append((u[i] * lci) % p)
      i += 1
   }
   res
}

fn poly_mod_div(list: a, list: b, any: p): list {
   "Polynomial division with remainder modulo p. Returns [q, r]."
   def na, nb = a.len, b.len
   if(nb == 0){ panic("poly_mod_div: division by zero") }
   if(na < nb){ return [[0], a] }
   def lcb = b[nb - 1]
   def lcbi = inverse_mod(lcb, p)
   mut r, q = clone(a), list(na - nb + 1)
   mut i = 0 while(i <= (na - nb)){ q = q.append(0) i += 1 }
   mut deg_r = na - 1
   while(deg_r >= nb - 1){
      def lead_r = r[deg_r]
      if(lead_r == 0){ deg_r -= 1 continue }
      def factor = (lead_r * lcbi) % p
      def shift = deg_r - nb + 1
      q[shift] = factor
      mut j = 0
      while(j < nb){
         def idx = shift + j
         def val = (r[idx] - factor * b[j]) % p
         r[idx] = (val + p) % p
         j += 1
      }
      deg_r -= 1
   }
   while(r.len > 1 && r[r.len - 1] == 0){ r = r.slice(0, r.len - 1) }
   [q, r]
}

fn poly_mod_divmod(list: a, list: b, any: p): list { poly_mod_div(a, b, p) }

fn poly_neg(list: a, any: p): list {
   mut out = []
   mut i = 0
   while(i < a.len){
      out = out.append(mod(0 - a.get(i), p))
      i += 1
   }
   out
}

fn poly_mod_mul(list: a, list: b, any: p): list {
   "Polynomial multiplication modulo p."
   def res = poly_mul(a, b)
   mut i = 0
   while(i < res.len){
      res[i] = (res[i] % p + p) % p
      i += 1
   }
   res
}

fn poly_mod_pow(list: a, int: e, list: f, any: p): list {
   "Polynomial exponentiation a^e mod f(x) over GF(p)."
   mut res = [1]
   mut base = a
   mut exp = e
   while(exp > 0){
      if(exp & 1 != 0){ res = poly_mod_div(poly_mod_mul(res, base, p), f, p).get(1) }
      base = poly_mod_div(poly_mod_mul(base, base, p), f, p).get(1)
      exp = exp >> 1
   }
   res
}

fn poly_factor_cz(list: f, any: p): list {
   "Cantor-Zassenhaus factorization of monic square-free polynomial f over GF(p).
   p must be an odd prime. Returns a list of irreducible factors."
   def n = f.len - 1
   if(n <= 1){ return [f] }
   mut factors = [f]
   mut attempts = 0
   while(attempts < 100){
      mut all_irreducible = true
      mut list: new_factors = []
      mut i = 0
      while(i < factors.len){
         def list: g = factors.get(i, [])
         def deg_g = g.len - 1
         if(deg_g <= 1){
            new_factors = new_factors.append(g)
            i += 1
            continue
         }
         mut list: a = []
         mut j = 0
         while(j < deg_g){
            a = a.append(bigint_random(p))
            j += 1
         }
         def list: g_luck = poly_mod_gcd(g, a, p)
         if(g_luck.len > 1 && g_luck.len < g.len){
            new_factors = new_factors.append(g_luck)
            new_factors = new_factors.append(poly_mod_div(g, g_luck, p).get(0))
            all_irreducible = false
            i += 1
            continue
         }
         def list: h_poly = poly_mod_pow(a, (p - 1) / 2, g, p)
         def list: h_minus_1 = poly_add(h_poly, [bigint_from_int(-1)])
         def list: split = poly_mod_gcd(g, h_minus_1, p)
         if(split.len > 1 && split.len < g.len){
            new_factors = new_factors.append(split)
            new_factors = new_factors.append(poly_mod_div(g, split, p).get(0))
            all_irreducible = false
         } else {
            new_factors = new_factors.append(g)
         }
         i += 1
      }
      factors = new_factors
      if(all_irreducible){
         mut deg_sum = 0
         i = 0
         while(i < factors.len){
            deg_sum += (factors.get(i).len - 1)
            i += 1
         }
         if(deg_sum == n){ return factors }
      }
      attempts += 1
   }
   factors
}

fn poly_mod_roots(list: f, any: p): list {
   "Find all roots of polynomial f over GF(p) using Cantor-Zassenhaus factorization."
   def factors = poly_factor_cz(f, p)
   mut roots = []
   mut i = 0
   while(i < factors.len){
      def g = factors.get(i)
      if(g.len == 2){
         def a, b = g.get(1), g.get(0)
         roots = roots.append(mod_sub(0, mod_mul(b, inverse_mod(a, p), p), p))
      }
      i += 1
   }
   roots
}

fn poly_subproduct_tree(list: points, any: p): list {
   "Build a subproduct tree for the given points modulo p.
   Returns a list of lists representing the tree levels."
   def n = points.len
   if(n == 0){ return [] }
   mut level = []
   mut i = 0
   while(i < n){
      level = level.append([mod(0 - points.get(i), p), 1])
      i += 1
   }
   mut tree = [level]
   while(level.len > 1){
      mut next_level = []
      mut j = 0
      while(j < level.len - 1){
         next_level = next_level.append(poly_mod_mul(level.get(j), level.get(j + 1), p))
         j += 2
      }
      if(level.len % 2 == 1){ next_level = next_level.append(level.get(level.len - 1)) }
      level = next_level
      tree = tree.append(level)
   }
   tree
}

fn poly_multipoint_eval(list: poly, list: points, any: p): list {
   "Fast multipoint evaluation of poly at given points modulo p.
   Runs in O(M(n) log n) using subproduct tree and remainders."
   def tree = poly_subproduct_tree(points, p)
   def n = points.len
   mut remainders = [poly_mod_div(poly, tree.get(tree.len - 1).get(0), p).get(1)]
   mut l = tree.len - 2
   while(l >= 0){
      mut next_remainders = []
      mut i = 0
      while(i < remainders.len){
         def r = remainders.get(i)
         def level = tree.get(l)
         next_remainders = next_remainders.append(poly_mod_div(r, level.get(2 * i), p).get(1))
         if(2 * i + 1 < level.len){ next_remainders = next_remainders.append(poly_mod_div(r, level.get(2 * i + 1), p).get(1)) }
         i += 1
      }
      remainders = next_remainders
      l -= 1
   }
   mut results = []
   mut j = 0
   while(j < n){
      def r = remainders.get(j)
      results = results.append((r.len > 0) ? r.get(0) : 0)
      j += 1
   }
   results
}

fn poly_interpolate(list: points, list: values, any: p): list {
   "Fast polynomial interpolation over GF(p).
   Finds the unique polynomial of degree < points.len passing through(points, values).
   Runs in O(M(n) log n) using subproduct tree."
   def n = points.len
   if(n == 0){ return [] }
   if(n == 1){ return [mod(values.get(0), p)] }
   def tree = poly_subproduct_tree(points, p)
   def root_poly = tree.get(tree.len - 1).get(0)
   def dM = poly_derivative_mod(root_poly, p)
   def denoms = poly_multipoint_eval(dM, points, p)
   mut b, i = [], 0
   while(i < n){
      b = b.append(mod(values.get(i) * inverse_mod(denoms.get(i), p), p))
      i += 1
   }
   _poly_interpolate_upward(tree, 0, 0, b.len, b, p)
}

fn _poly_interpolate_upward(list: tree, int: level_idx, int: start, int: length, list: b, any: p): list {
   "Internal recursive upward pass for fast interpolation.
   Combines L and R segments: Res = L * M_right + R * M_left."
   if(length == 1){ return [b.get(start)] }
   def m, L = length / 2, _poly_interpolate_upward(tree, level_idx + 1, start, m, b, p)
   def R = _poly_interpolate_upward(tree, level_idx + 1, start + m, length - m, b, p)
   def level = tree.get(level_idx)
   def M_left = level.get(2 * (start >> level_idx))
   def M_right = level.get(2 * (start >> level_idx) + 1)
   poly_mod_add(poly_mod_mul(L, M_right, p), poly_mod_mul(R, M_left, p), p)
}

fn poly_hgcd(list: a, list: b, any: p): list {
   "Half-GCD algorithm: compute a transformation matrix M such that M * [a, b]^T = [a', b'] with deg(b') < n/2."
   def n, m = a.len, n / 2
   if(b.len <= m || n < 32){ return [[1], [0], [0], [1]] }
   def k = m
   def a_hi, b_hi = poly_div_x(a, k), poly_div_x(b, k)
   def R1 = poly_hgcd(a_hi, b_hi, p)
   def ab = _poly_mat_mul_vec(R1, a, b, p)
   mut a_new, b_new = ab.get(0), ab.get(1)
   if(b_new.len <= m){ return R1 }
   def res = poly_mod_divmod(a_new, b_new, p)
   def q = res.get(0)
   def r = res.get(1)
   def k2 = 2 * m - (b_new.len - 1)
   def b_hi2 = poly_div_x(b_new, k2)
   def r_hi2 = poly_div_x(r, k2)
   def R2 = poly_hgcd(b_hi2, r_hi2, p)
   _poly_mat_mat_mul(R2, [[0], [1], [1], poly_neg(q, p)], R1, p)
}

fn poly_mod_gcd_fast(list: a, list: b, any: p): list {
   "Subquadratic GCD using Half-GCD recursive reduction."
   mut u, v = a, b
   while(v.len > 32){
      def M = poly_hgcd(u, v, p)
      def uv = _poly_mat_mul_vec(M, u, v, p)
      u, v = uv.get(0), uv.get(1)
      if(v.len > 0){
         def res = poly_mod_divmod(u, v, p)
         u, v = v, res.get(1)
      }
   }
   poly_mod_gcd(u, v, p)
}

fn _poly_mat_mul_vec(list: M, list: a, list: b, any: p): list {
   def m11 = M.get(0) def m12 = M.get(1)
   def m21 = M.get(2) def m22 = M.get(3)
   def r1 = poly_mod_add(poly_mod_mul(m11, a, p), poly_mod_mul(m12, b, p), p)
   def r2 = poly_mod_add(poly_mod_mul(m21, a, p), poly_mod_mul(m22, b, p), p)
   [r1, r2]
}

fn _poly_mat_mat_mul(list: A, list: B, list: C, any: p): list {
   def a11 = A.get(0) def a12 = A.get(1) def a21 = A.get(2) def a22 = A.get(3)
   def b11 = B.get(0) def b12 = B.get(1) def b21 = B.get(2) def b22 = B.get(3)
   def r11 = poly_mod_add(poly_mod_mul(a11, b11, p), poly_mod_mul(a12, b21, p), p)
   def r12 = poly_mod_add(poly_mod_mul(a11, b12, p), poly_mod_mul(a12, b22, p), p)
   def r21 = poly_mod_add(poly_mod_mul(a21, b11, p), poly_mod_mul(a22, b21, p), p)
   def r22 = poly_mod_add(poly_mod_mul(a21, b12, p), poly_mod_mul(a22, b22, p), p)
   def c11 = C.get(0) def c12 = C.get(1) def c21 = C.get(2) def c22 = C.get(3)
   def f11 = poly_mod_add(poly_mod_mul(r11, c11, p), poly_mod_mul(r12, c21, p), p)
   def f12 = poly_mod_add(poly_mod_mul(r11, c12, p), poly_mod_mul(r12, c22, p), p)
   def f21 = poly_mod_add(poly_mod_mul(r21, c11, p), poly_mod_mul(r22, c21, p), p)
   def f22 = poly_mod_add(poly_mod_mul(r21, c12, p), poly_mod_mul(r22, c22, p), p)
   [f11, f12, f21, f22]
}

fn poly_div_x(list: a, int: k): list {
   "Returns a / x^k(removes first k coefficients)."
   if(a.len <= k){ return [] }
   a.slice(k, a.len)
}

fn poly_sylvester_matrix(list: a, list: b): list {
   "Construct the Sylvester matrix of polynomials a and b."
   def na, nb = a.len - 1, b.len - 1
   def n = na + nb
   mut matrix = list(n)
   mut i = 0
   while(i < nb){
      mut row = list(n)
      mut k = 0 while(k < i){ row = row.append(Z(0)) k += 1 }
      mut j = 0 while(j <= na){ row = row.append(a.get(na - j)) j += 1 }
      while(row.len < n){ row = row.append(Z(0)) }
      matrix = matrix.append(row)
      i += 1
   }
   i = 0
   while(i < na){
      mut row = list(n)
      mut k = 0 while(k < i){ row = row.append(Z(0)) k += 1 }
      mut j = 0 while(j <= nb){ row = row.append(b.get(nb - j)) j += 1 }
      while(row.len < n){ row = row.append(Z(0)) }
      matrix = matrix.append(row)
      i += 1
   }
   [n, n, matrix]
}

fn poly_resultant(list: a, list: b): any {
   "Compute the resultant of polynomials a and b via Sylvester determinant."
   def S = poly_sylvester_matrix(a, b)
   matrix_det(S)
}

fn poly_resultant_mod(list: a, list: b, any: modn): any {
   "Compute the resultant of polynomials a and b modulo `modn`.
   This is much faster than `poly_resultant` when you only need the resultant
   in Z/modnZ(common in Coppersmith/resultant attacks). If the modular
   elimination hits a non-invertible pivot(possible for composite moduli),
   we fall back to the integer determinant and reduce modulo `modn`."
   def S = poly_sylvester_matrix(a, b)
   def detm = matrix_det_mod(S, modn)
   if(detm != nil && detm != Z(0)){ return detm }
   mod(matrix_det(S), modn)
}

fn _poly_trim_mod_local(list: p, any: modn): list {
   mut out = []
   mut i = 0
   while(i < len(p)){
      out = out.append(mod(p.get(i), modn))
      i += 1
   }
   while(out.len > 1 && out.get(out.len - 1) == Z(0)){ out = out.slice(0, out.len - 1) }
   out
}

fn _poly_mod_add_local(list: a, list: b, any: modn): list {
   def na, nb = len(a), len(b)
   def n = (na > nb) ? na : nb
   mut out = []
   mut i = 0
   while(i < n){
      def av, bv = (i < na) ? a.get(i) : Z(0), (i < nb) ? b.get(i) : Z(0)
      out = out.append(mod(av + bv, modn))
      i += 1
   }
   _poly_trim_mod_local(out, modn)
}

fn _poly_mod_scale_local(list: a, any: s, any: modn): list {
   mut out = []
   mut i = 0
   while(i < len(a)){
      out = out.append(mod(a.get(i) * s, modn))
      i += 1
   }
   _poly_trim_mod_local(out, modn)
}

fn _poly_mod_mul_local(list: a, list: b, any: modn): list {
   if(len(a) == 0 || len(b) == 0){ return [Z(0)] }
   mut out = []
   mut i = 0
   while(i < len(a) + len(b) - 1){
      out = out.append(Z(0))
      i += 1
   }
   i = 0
   while(i < len(a)){
      mut j = 0
      while(j < len(b)){
         def idx = i + j
         out[idx] = mod(out.get(idx) + a.get(i) * b.get(j), modn)
         j += 1
      }
      i += 1
   }
   _poly_trim_mod_local(out, modn)
}

fn _poly_small_nonnegative_int(any: x, str: name): int {
   if(is_int(x)){
      if(x < 0){ panic("PolynomialExponentError: " + name + " must be non-negative") }
      return x
   }
   if(is_bigint(x)){
      if(x < Z(0)){ panic("PolynomialExponentError: " + name + " must be non-negative") }
      return bigint_to_int(x)
   }
   panic("PolynomialTypeError: " + name + " must be an int or bigint")
}

fn _poly_mod_pow_local(list: a, any: e, any: modn): list {
   mut res = [Z(1)]
   mut base = _poly_trim_mod_local(a, modn)
   mut exp = _poly_small_nonnegative_int(e, "exponent")
   while(exp > 0){
      if(exp % 2 == 1){ res = _poly_mod_mul_local(res, base, modn) }
      exp = exp / 2
      if(exp > 0){ base = _poly_mod_mul_local(base, base, modn) }
   }
   res
}

fn poly_resultant_quadratic_xn_minus_const_mod(list: a2, list: a1, list: a0, any: exponent, any: c, any: modn): list {
   "Return Res_y(a2(x)*y^2 + a1(x)*y + a0(x), y^exponent - c) mod modn as a polynomial in x.
   Uses the symmetric recurrence T_0=2, T_1=-a1, T_k=-a1*T_{k-1}-a0*a2*T_{k-2}.
   This avoids a Sylvester determinant when eliminating a quadratic against a power polynomial."
   def e = _poly_small_nonnegative_int(exponent, "exponent")
   def cval = mod(c, modn)
   def aa2 = _poly_trim_mod_local(a2, modn)
   def aa1 = _poly_trim_mod_local(a1, modn)
   def aa0 = _poly_trim_mod_local(a0, modn)
   if(e == 0){
      def v = mod(Z(1) - cval, modn)
      return [mod(v * v, modn)]
   }
   mut t0, t1 = [Z(2)], _poly_mod_scale_local(aa1, Z(-1), modn)
   mut te = (e == 1) ? t1 : t0
   def a0a2 = _poly_mod_mul_local(aa0, aa2, modn)
   mut k = 2
   while(k <= e){
      def left = _poly_mod_mul_local(_poly_mod_scale_local(aa1, Z(-1), modn), t1, modn)
      def right = _poly_mod_mul_local(_poly_mod_scale_local(a0a2, Z(-1), modn), t0, modn)
      te, t0 = _poly_mod_add_local(left, right, modn), t1
      t1 = te
      k += 1
   }
   def a0e, a2e = _poly_mod_pow_local(aa0, e, modn), _poly_mod_pow_local(aa2, e, modn)
   def mid = _poly_mod_scale_local(te, -cval, modn)
   def tail = _poly_mod_scale_local(a2e, cval * cval, modn)
   _poly_mod_add_local(_poly_mod_add_local(a0e, mid, modn), tail, modn)
}

fn _poly2_coeff(list: p, int: i, int: j): any {
   if(i < 0 || j < 0){ return 0 }
   def rows = _matrix_rows(p)
   def cols = _matrix_cols(p)
   if(i >= rows || j >= cols){ return 0 }
   mat_get(p, i, j)
}

fn poly2_new(int: rows, int: cols): list {
   "Create a new bivariate polynomial matrix(rows/cols are max degrees)."
   def rr = (rows < 0) ? 0 : rows
   def cc = (cols < 0) ? 0 : cols
   mat_new(rr + 1, cc + 1, 0)
}

fn poly2_add(list: a, list: b): list {
   "Add bivariate polynomials with support for different matrix sizes."
   def ra, ca = _matrix_rows(a), _matrix_cols(a)
   def rb, cb = _matrix_rows(b), _matrix_cols(b)
   def rr, cr = (ra > rb) ? ra : rb, (ca > cb) ? ca : cb
   mut res = mat_new(rr, cr, 0)
   mut i = 0
   while(i < rr){
      mut j = 0
      while(j < cr){
         mat_set(res, i, j, _poly2_coeff(a, i, j) + _poly2_coeff(b, i, j))
         j += 1
      }
      i += 1
   }
   res
}

fn poly2_mul(list: a, list: b): list {
   "Multiply bivariate polynomials via 2D convolution."
   def ra, ca = _matrix_rows(a), _matrix_cols(a)
   def rb, cb = _matrix_rows(b), _matrix_cols(b)
   if(ra == 0 || ca == 0 || rb == 0 || cb == 0){ return mat_new(1, 1, 0) }
   def rr, cr = ra + rb - 1, ca + cb - 1
   mut res = mat_new(rr, cr, 0)
   mut i1 = 0
   while(i1 < ra){
      mut j1 = 0
      while(j1 < ca){
         def va = mat_get(a, i1, j1)
         if(va != 0){
            mut i2 = 0
            while(i2 < rb){
               mut j2 = 0
               while(j2 < cb){
                  def vb = mat_get(b, i2, j2)
                  if(vb != 0){
                     def ir, jr = i1 + i2, j1 + j2
                     mat_set(res, ir, jr, mat_get(res, ir, jr) + va * vb)
                  }
                  j2 += 1
               }
               i2 += 1
            }
         }
         j1 += 1
      }
      i1 += 1
   }
   res
}

fn poly2_to_univariate_x(list: p, any: y_val): list {
   "Fix y = y_val and return univariate polynomial in x."
   def rows = _matrix_rows(p)
   def cols = _matrix_cols(p)
   mut res = []
   mut i = 0
   while(i < rows){
      mut coeff = 0
      mut j = cols - 1
      while(j >= 0){
         coeff = coeff * y_val + mat_get(p, i, j)
         j -= 1
      }
      res = res.append(coeff)
      i += 1
   }
   res
}

fn poly2_to_univariate_y(list: p, any: x_val): list {
   "Fix x = x_val and return univariate polynomial in y."
   def rows = _matrix_rows(p)
   def cols = _matrix_cols(p)
   mut res = []
   mut j = 0
   while(j < cols){
      mut coeff = 0
      mut i = rows - 1
      while(i >= 0){
         coeff = coeff * x_val + mat_get(p, i, j)
         i -= 1
      }
      res = res.append(coeff)
      j += 1
   }
   res
}

fn poly2_eval(list: p, any: x, any: y): any {
   "Evaluate f(x, y) at(x, y)."
   def ux = poly2_to_univariate_x(p, y)
   poly_eval(ux, x)
}

fn poly2_resultant_x(list: p1, list: p2): list {
   "Compute resultant with respect to x using evaluation/interpolation over y."
   def d1, d2 = _matrix_rows(p1), _matrix_rows(p2)
   def d_res = d1 * d2
   mut points = []
   mut res_vals = []
   mut y_val = 0
   while(points.len <= d_res){
      def u1, u2 = poly2_to_univariate_x(p1, y_val), poly2_to_univariate_x(p2, y_val)
      points = points.append(y_val)
      res_vals = res_vals.append(poly_resultant(u1, u2))
      y_val += 1
   }
   poly_interpolate(points, res_vals, 1000000007)
}
