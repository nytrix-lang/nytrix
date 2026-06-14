;; Keywords: lattice mvpoly math crypto number-theory
;; Lattice routines for multivariate polynomial arithmetic for lattice attacks.
;; Representation: ["mvpoly", nvars, [[[e0, e1, ...], coeff], ...]]
;; References:
;; - std.math.crypto.lattice
;; - std.math.crypto
module std.math.crypto.lattice.mvpoly(mv_poly, mv_const, mv_var, mv_monomial, mv_is_poly, mv_nvars, mv_terms, mv_normalize, mv_add, mv_neg, mv_sub, mv_scale, mv_mul, mv_pow, mv_mod_coeffs, mv_monomial_mul, mv_degree, mv_degrees, mv_constant, mv_coeff, mv_eval, mv_max_norm_scaled, mv_create_lattice, mv_reconstruct_polynomials, mv_find_roots, mv_small_roots_modular, mv_small_roots_integer)
use std.core
use std.math.nt
use std.math.matrix
use std.math.crypto.lattice.lll
use std.os.prim

fn _mv_abs_z(any x) bigint {
   def z = Z(x)
   z < Z(0) ? Z(0) - z : z
}

fn _mv_zero_exps(int n) list {
   "Return an all-zero exponent vector of length n."
   mut xs = []
   mut i = 0
   while i < n {
      xs = xs.append(0)
      i += 1
   }
   xs
}

fn _mv_exps_eq(any a, any b) bool {
   "Return true when two exponent vectors are equal."
   if !is_list(a) || !is_list(b) || a.len != b.len { return false }
   mut i = 0
   while i < a.len {
      if a.get(i) != b.get(i) { return false }
      i += 1
   }
   true
}

fn _mv_exps_add(list a, list b) list {
   "Add two exponent vectors component-wise."
   mut out = []
   mut i = 0
   while i < a.len {
      out = out.append(a.get(i) + b.get(i))
      i += 1
   }
   out
}

fn _mv_bound_scale(list exps, list bounds) bigint {
   "Return product(bounds[i] ^ exps[i]) for lattice scaling."
   mut s, i = Z(1), 0
   while i < exps.len {
      s = s * bigint_pow(Z(bounds.get(i, 1)), Z(exps.get(i, 0)))
      i += 1
   }
   s
}

fn _mv_concat(list a, list b) list {
   "Return a clone of a followed by b."
   mut out = clone(a)
   mut i = 0
   while i < b.len {
      out = out.append(b.get(i))
      i += 1
   }
   out
}

fn _mv_term(list exps, any coeff) list { [clone(exps), Z(coeff)] }

fn _mv_term_exps(any term) list { term.get(0) }

fn _mv_term_coeff(any term) bigint { Z(term.get(1)) }

fn _mv_replace_or_remove(list out, int j, list exps, any coeff) list {
   "Replace term j, or remove it when the new coefficient is zero."
   coeff == Z(0) ? _mv_concat(slice(out, 0, j), slice(out, j + 1, out.len)) :
   _mv_concat(_mv_concat(slice(out, 0, j), [_mv_term(exps, coeff)]), slice(out, j + 1, out.len))
}

fn mv_poly(int nvars, list terms) list {
   "Create and normalize a sparse multivariate polynomial."
   mv_normalize(["mvpoly", nvars, terms])
}

fn mv_const(int nvars, any c) list {
   "Create a constant sparse polynomial with nvars variables."
   ["mvpoly", nvars, Z(c) == Z(0) ? [] : [[_mv_zero_exps(nvars), Z(c)]]]
}

fn mv_var(int nvars, int idx) list {
   "Create variable x_idx in a polynomial ring with nvars variables."
   mut e = _mv_zero_exps(nvars)
   e[idx] = 1
   ["mvpoly", nvars, [[e, Z(1)]]]
}

fn mv_monomial(list exps, any coeff=1) list {
   "Create one monomial from exponent vector exps and coefficient coeff."
   mv_poly(exps.len, [[clone(exps), Z(coeff)]])
}

fn mv_is_poly(any p) bool {
   "Return true when p has the Ny mvpoly representation."
   is_list(p) && p.len >= 3 && p.get(0) == "mvpoly"
}

fn mv_nvars(any p) int {
   "Return the variable count of an mvpoly value."
   mv_is_poly(p) ? p.get(1) : 0
}

fn mv_terms(any p) list {
   "Return the sparse term list of an mvpoly value."
   mv_is_poly(p) ? p.get(2) : []
}

fn mv_normalize(any p) any {
   "Combine like terms and remove zero coefficients from an mvpoly."
   if !mv_is_poly(p) { return p }
   def n = mv_nvars(p)
   def terms = mv_terms(p)
   mut out = []
   mut i = 0
   while i < terms.len {
      def term = terms.get(i)
      def exps = _mv_term_exps(term)
      def coeff = _mv_term_coeff(term)
      if coeff != Z(0) {
         mut found = false
         mut j = 0
         while j < out.len {
            def old = out.get(j)
            if _mv_exps_eq(_mv_term_exps(old), exps) {
               def nc = _mv_term_coeff(old) + coeff
               out = _mv_replace_or_remove(out, j, exps, nc)
               found = true
               j = out.len
            } else {
               j += 1
            }
         }
         if !found { out = out.append(_mv_term(exps, coeff)) }
      }
      i += 1
   }
   ["mvpoly", n, out]
}

fn mv_add(any a, any b) any {
   "Add two sparse multivariate polynomials."
   if !mv_is_poly(a) { return b }
   if !mv_is_poly(b) { return a }
   mv_poly(mv_nvars(a), _mv_concat(mv_terms(a), mv_terms(b)))
}

fn mv_neg(any a) list {
   "Negate a sparse multivariate polynomial."
   def terms = mv_terms(a)
   mut ts = []
   mut i = 0
   while i < terms.len {
      def t = terms.get(i)
      ts = ts.append(_mv_term(_mv_term_exps(t), -_mv_term_coeff(t)))
      i += 1
   }
   mv_poly(mv_nvars(a), ts)
}

fn mv_sub(any a, any b) any {
   "Subtract polynomial b from a."
   mv_add(a, mv_neg(b))
}

fn mv_scale(any a, any c) list {
   "Multiply every coefficient of a polynomial by c."
   def n = mv_nvars(a)
   def zero = Z(c) == Z(0)
   def terms = zero ? [] : mv_terms(a)
   mut ts = []
   mut i = 0
   while i < terms.len {
      def t = terms.get(i)
      ts = ts.append(_mv_term(_mv_term_exps(t), _mv_term_coeff(t) * Z(c)))
      i += 1
   }
   mv_poly(n, ts)
}

fn mv_mul(any a, any b) list {
   "Multiply two sparse multivariate polynomials."
   def n = mv_nvars(a)
   def terms_a = mv_terms(a)
   def terms_b = mv_terms(b)
   mut ts = []
   mut i = 0
   while i < terms_a.len {
      def ta = terms_a.get(i)
      mut j = 0
      while j < terms_b.len {
         def tb = terms_b.get(j)
         ts = ts.append(_mv_term(_mv_exps_add(_mv_term_exps(ta), _mv_term_exps(tb)), _mv_term_coeff(ta) * _mv_term_coeff(tb)))
         j += 1
      }
      i += 1
   }
   mv_poly(n, ts)
}

fn mv_pow(any a, any e) list {
   "Raise a sparse multivariate polynomial to a non-negative integer power."
   def n = mv_nvars(a)
   mut res = mv_const(n, 1)
   mut base = a
   mut ee = Z(e)
   while ee > Z(0) {
      res = ee % Z(2) == Z(1) ? mv_mul(res, base) : res
      base = mv_mul(base, base)
      ee = ee / Z(2)
   }
   res
}

fn mv_mod_coeffs(any a, any m) list {
   "Reduce all polynomial coefficients modulo m."
   def terms = mv_terms(a)
   mut ts = []
   mut i = 0
   while i < terms.len {
      def t = terms.get(i)
      ts = ts.append(_mv_term(_mv_term_exps(t), mod(_mv_term_coeff(t), m)))
      i += 1
   }
   mv_poly(mv_nvars(a), ts)
}

fn mv_monomial_mul(any a, list exps, any coeff=1) list {
   "Multiply polynomial a by one monomial."
   mv_mul(a, mv_monomial(exps, coeff))
}

fn mv_degree(any a, int idx) int {
   "Return the degree of polynomial a in variable idx."
   def terms = mv_terms(a)
   mut d, i = 0, 0
   while i < terms.len {
      def e = _mv_term_exps(terms.get(i)).get(idx, 0)
      if e > d { d = e }
      i += 1
   }
   d
}

fn mv_degrees(any a) list {
   "Return per-variable degrees for polynomial a."
   mut ds = []
   mut i = 0
   while i < mv_nvars(a) {
      ds = ds.append(mv_degree(a, i))
      i += 1
   }
   ds
}

fn mv_constant(any a) bigint {
   "Return the constant coefficient of polynomial a."
   mv_coeff(a, _mv_zero_exps(mv_nvars(a)))
}

fn mv_coeff(any a, list exps) bigint {
   "Return the coefficient for exponent vector exps."
   def terms = mv_terms(a)
   mut i = 0
   while i < terms.len {
      def t = terms.get(i)
      if _mv_exps_eq(_mv_term_exps(t), exps) { return _mv_term_coeff(t) }
      i += 1
   }
   Z(0)
}

fn mv_eval(any a, list vals) bigint {
   "Evaluate polynomial a at integer values vals."
   def terms = mv_terms(a)
   mut acc = Z(0)
   mut i = 0
   while i < terms.len {
      def t = terms.get(i)
      def exps = _mv_term_exps(t)
      mut term = _mv_term_coeff(t)
      mut j = 0
      while j < exps.len {
         term = term * bigint_pow(Z(vals.get(j, 0)), Z(exps.get(j, 0)))
         j += 1
      }
      acc = acc + term
      i += 1
   }
   acc
}

fn mv_max_norm_scaled(any a, list bounds) list {
   "Return [exponents, value] for the largest bound-scaled coefficient."
   def terms = mv_terms(a)
   mut best_e = _mv_zero_exps(mv_nvars(a))
   mut best = Z(0)
   mut i = 0
   while i < terms.len {
      def t = terms.get(i)
      def exps = _mv_term_exps(t)
      def v = _mv_abs_z(_mv_term_coeff(t) * _mv_bound_scale(exps, bounds))
      if v > best {
         best = v
         best_e = exps
      }
      i += 1
   }
   [best_e, best]
}

fn _mv_monomial_index(list monomials, list exps) int {
   "Return the index of exponent vector exps in monomials, or -1."
   mut i = 0
   while i < monomials.len {
      if _mv_exps_eq(monomials.get(i), exps) { return i }
      i += 1
   }
   -1
}

fn _mv_collect_monomials(list shifts) list {
   "Collect distinct monomial exponent vectors from shifted polynomials."
   mut mons = []
   mut i = 0
   while i < shifts.len {
      def ts = mv_terms(shifts.get(i))
      mut j = 0
      while j < ts.len {
         def exps = _mv_term_exps(ts.get(j))
         if _mv_monomial_index(mons, exps) < 0 { mons = mons.append(clone(exps)) }
         j += 1
      }
      i += 1
   }
   mons
}

fn mv_create_lattice(any shifts, list bounds) any {
   "Create a coefficient lattice from shifted polynomials and root bounds."
   def scan = is_list(shifts) ? shifts : []
   def monomials = _mv_collect_monomials(scan)
   mut rows = []
   mut i = 0
   while i < scan.len {
      def p = scan.get(i)
      mut row = []
      mut j = 0
      while j < monomials.len {
         def exps = monomials.get(j)
         row = row.append(mv_coeff(p, exps) * _mv_bound_scale(exps, bounds))
         j += 1
      }
      rows = rows.append(row)
      i += 1
   }
   scan.len == 0 ? nil : [Matrix(rows), monomials]
}

fn mv_reconstruct_polynomials(any B, any original, any modulus, list monomials, list bounds) list {
   "Reconstruct candidate polynomials from an LLL-reduced mvpoly lattice."
   def n = bounds.len
   mut polys = []
   mut row = 0
   while row < _matrix_rows(B) {
      mut terms = []
      mut norm_squared = Z(0)
      mut weight = 0
      mut col = 0
      while col < _matrix_cols(B) {
         def entry = mat_get(B, row, col)
         if entry != Z(0) {
            norm_squared = norm_squared + entry * entry
            weight += 1
            def exps = monomials.get(col)
            def scale = _mv_bound_scale(exps, bounds)
            terms = (scale != Z(0) && mod(entry, scale) == Z(0)) ? terms.append([exps, entry / scale]) : terms
         }
         col += 1
      }
      if weight > 0 && (modulus == nil || norm_squared * Z(weight) < Z(modulus) * Z(modulus)) {
         def p = mv_poly(n, terms)
         if len(mv_terms(p)) > 0 { polys = polys.append(p) }
      }
      row += 1
   }
   polys
}

fn _mv_root_rec(list polys, list bounds, int idx, list vals, list out, any modulus, int max_roots, int max_checks, list checks) list {
   "Recursive bounded root enumerator for mvpoly systems."
   if checks.get(0) >= max_checks { return out }
   if idx >= bounds.len {
      checks[0] = checks.get(0) + 1
      mut ok = true
      mut i = 0
      while i < polys.len {
         def v = mv_eval(polys.get(i), vals)
         if modulus == nil { if v != Z(0) { ok = false } } else { if mod(v, modulus) != Z(0) { ok = false } }
         i += 1
      }
      if ok && out.len < max_roots { out = out.append(clone(vals)) }
      return out
   }
   mut x = 0 - int(bounds.get(idx, 0))
   while x <= int(bounds.get(idx, 0)) && out.len < max_roots && checks.get(0) < max_checks {
      vals[idx] = x
      out = _mv_root_rec(polys, bounds, idx + 1, vals, out, modulus, max_roots, max_checks, checks)
      x += 1
   }
   out
}

fn mv_find_roots(any polys, list bounds, any modulus=nil, int max_roots=32, int max_checks=200000) list {
   "Brute-force bounded roots for sparse polynomials, optionally modulo modulus."
   if !is_list(polys) || polys.len == 0 { return [] }
   def env_checks = env("NY_MVPOLY_MAX_CHECKS")
   if is_str(env_checks) && env_checks.len > 0 { max_checks = atoi(env_checks) }
   mut vals = []
   mut i = 0
   while i < bounds.len {
      vals = vals.append(0)
      i += 1
   }
   _mv_root_rec(polys, bounds, 0, vals, [], modulus, max_roots, max_checks, [0])
}

fn mv_small_roots_modular(any f, any modulus, list shifts, list bounds, str method="ny") list {
   "Run modular small-root reconstruction and bounded root search.
   `method=\"auto\"` uses the same reduction path."
   mut polys = [f]
   def pack = mv_create_lattice(shifts, bounds)
   if pack != nil {
      def B = lll(pack.get(0), 0.8, method)
      def rec = mv_reconstruct_polynomials(B, f, modulus, pack.get(1), bounds)
      polys = _mv_concat(polys, rec)
   }
   mv_find_roots(polys, bounds, modulus)
}

fn mv_small_roots_integer(any f, list shifts, list bounds, str method="ny") list {
   "Run integer small-root reconstruction and bounded root search.
   `method=\"auto\"` uses the same reduction path."
   mut polys = [f]
   def pack = mv_create_lattice(shifts, bounds)
   if pack != nil {
      def B = lll(pack.get(0), 0.8, method)
      def rec = mv_reconstruct_polynomials(B, f, nil, pack.get(1), bounds)
      polys = _mv_concat(polys, rec)
   }
   mv_find_roots(polys, bounds, nil)
}
