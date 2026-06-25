;; Keywords: lattice small-roots math crypto number-theory
;; Lattice routines for small-root search over modular and integer polynomials.
;; Univariate modular roots are solved directly; multivariate helpers build
;; Ny mvpoly lattices and return bounded candidate roots where available.
;; References:
;; - std.math.crypto.lattice
;; - std.math.crypto
module std.math.crypto.lattice.small_roots(small_roots_backend_report, create_lattice_univariate, reduce_lattice_basis, reduce_lattice_basis_report, reconstruct_polynomials_univariate, modular_univariate, modular_univariate_report, small_roots_strategy_unavailable, howgrave_graham_modular_univariate, aono_integer_multivariate, blomer_may_modular_trivariate, blomer_may_modular_bivariate, coron_integer_bivariate, coron_direct_integer_bivariate, ernst_integer_trivariate_1, ernst_integer_trivariate_2, herrmann_may_modular_bivariate, herrmann_may_modular_multivariate, jochemsz_may_integer_multivariate, jochemsz_may_modular_multivariate, nitaj_fouotsa_modular_trivariate)
use std.core
use std.core.common as common
use std.os.clock (ticks)
use std.math.nt
use std.math (abs)
use std.math.crypto.error
use std.math.crypto.poly
use std.math.crypto.lattice.lll
use std.math.crypto.lattice.mvpoly
use std.math.matrix

mut _sr_trace_cache = -1
mut _sr_exact_gcd_cache = -1

fn small_roots_backend_report() dict {
   "Return the small-roots lattice reduction policy used by the Ny implementation."
   mut out = dict(10)
   out = out.set("default_method", "ny")
   out = out.set("auto_method", "ny")
   out = out.set("ny_default", true)
   out = out.set("lll_default_delta", 0.99)
   out = out.set("lll_default_eta", 0.51)
   out
}

fn _sr_rows(any m) int {
   "Return row count from the compact matrix tuple used by this module."
   int(m[0])
}

fn _sr_cols(any m) int {
   "Return column count from the compact matrix tuple used by this module."
   int(m[1])
}

fn _sr_data(any m) list {
   "Return row data from the compact matrix tuple used by this module."
   m[2]
}

fn _sr_get(any m, int i, int j) any {
   "Read a matrix cell, returning zero for out-of-range sparse accesses."
   def data = _sr_data(m)
   if i < 0 || i >= data.len { return Z(0) }
   def row = data[i]
   if !is_list(row) || j < 0 || j >= row.len { return Z(0) }
   row[j]
}

fn _sr_trace_on() bool {
   "Return true when small-roots trace logging is enabled."
   _sr_trace_cache = common.cached_env_enabled(_sr_trace_cache, "NY_SMALL_ROOTS_TRACE")
   _sr_trace_cache == 1
}

fn _sr_exact_gcd_on() bool {
   "Return true when exact GCD fallback is enabled."
   _sr_exact_gcd_cache = common.cached_env_enabled(_sr_exact_gcd_cache, "NY_SMALL_ROOTS_EXACT_GCD")
   _sr_exact_gcd_cache == 1
}

fn _sr_trace(str stage, any t0) any {
   "Emit a timed trace line for a small-roots stage when tracing is enabled."
   if _sr_trace_on() { eprint("[small_roots] " + stage + " ms=" + to_str(float(ticks() - t0) / 1000000.0)) }
}

fn _poly_copy(list p) list {
   "Copy a coefficient list."
   mut out = []
   mut i = 0
   while i < p.len {
      out = out.append(p[i])
      i += 1
   }
   out
}

fn _poly_trim(any p) list {
   "Remove trailing zero coefficients from a polynomial coefficient list."
   if !is_list(p) { return [] }
   mut out = _poly_copy(p)
   while out.len > 1 && out[out.len - 1] == Z(0) { out = slice(out, 0, out.len - 1) }
   out
}

fn _poly_is_zero(list p) bool {
   "Return true when every polynomial coefficient is zero."
   mut i = 0
   while i < p.len {
      if p[i] != Z(0) { return false }
      i += 1
   }
   true
}

fn _sr_small_nonnegative_int(any x, str name) int {
   "Convert x to a non-negative host int or raise a named parameter error."
   if is_int(x) {
      if x < 0 { panic("SmallRootsParameterError: " + name + " must be non-negative") }
      return x
   }
   if is_bigint(x) {
      if x < Z(0) { panic("SmallRootsParameterError: " + name + " must be non-negative") }
      return bigint_to_int(x)
   }
   panic("SmallRootsTypeError: " + name + " must be an int or bigint")
}

fn _poly_pow(list p, any e) list {
   "Raise a coefficient-list polynomial to a non-negative integer power."
   def early = case e {
      0 -> [Z(1)]
      1 -> _poly_copy(p)
      _ -> {}
   }
   if early != nil { return early }
   mut res = [Z(1)]
   mut base = _poly_copy(p)
   mut exp = _sr_small_nonnegative_int(e, "exponent")
   while exp > 0 {
      res = (exp % 2 == 1) ? poly_mul(res, base) : res
      base = poly_mul(base, base)
      exp = exp / 2
   }
   res
}

fn _poly_shift(list p, int s) list {
   "Multiply polynomial p by x^s."
   mut out = []
   mut i = 0
   while i < s {
      out = out.append(Z(0))
      i += 1
   }
   i = 0
   while i < p.len {
      out = out.append(p[i])
      i += 1
   }
   out
}

fn _poly_div_exact(list a, list b) any {
   "Return exact quotient a / b over ZZ coefficients, or nil."
   def aa = _poly_trim(a)
   def bb = _poly_trim(b)
   if _poly_is_zero(bb) { return nil }
   if aa.len < bb.len { return nil }
   mut r, q = _poly_copy(aa), []
   mut qi = 0
   while qi <= aa.len - bb.len {
      q = q.append(Z(0))
      qi += 1
   }
   def db = bb.len - 1
   def lcb = bb[db]
   while r.len >= bb.len && !_poly_is_zero(r) {
      def dr = r.len - 1
      def lcr = r[dr]
      if mod(lcr, lcb) != Z(0) { return nil }
      def factor = lcr / lcb
      def shift = dr - db
      q[shift] = q[shift] + factor
      mut i = 0
      while i <= db {
         def idx = shift + i
         r[idx] = r[idx] - factor * bb[i]
         i += 1
      }
      r = _poly_trim(r)
   }
   _poly_is_zero(r) ? _poly_trim(q) : nil
}

fn _poly_degree_zz(list p) int {
   "Return the degree of a ZZ polynomial, or -1 for zero."
   def pp = _poly_trim(p)
   _poly_is_zero(pp) ? -1 : len(pp) - 1
}

fn _poly_scalar_mul_zz(list p, any s) list {
   "Multiply a ZZ polynomial by scalar s."
   mut out = []
   mut i = 0
   while i < p.len {
      out = out.append(p[i] * s)
      i += 1
   }
   _poly_trim(out)
}

fn _poly_content_abs_zz(list p) bigint {
   "Return the absolute content gcd of a ZZ polynomial."
   mut c, i = Z(0), 0
   while i < p.len {
      def a = abs(Z(p[i]))
      c = (a == Z(0)) ? c : ((c == Z(0)) ? a : gcd(c, a))
      i += 1
   }
   c
}

fn _poly_primitive_part_zz(list p) list {
   "Return primitive part of a ZZ polynomial with positive leading coefficient."
   mut pp = _poly_trim(p)
   if _poly_is_zero(pp) { return [Z(0)] }
   def c = _poly_content_abs_zz(pp)
   if c > Z(1) {
      mut out = []
      mut i = 0
      while i < pp.len {
         out = out.append(pp[i] / c)
         i += 1
      }
      pp = _poly_trim(out)
   }
   if pp[len(pp) - 1] < Z(0) { pp = _poly_scalar_mul_zz(pp, Z(-1)) }
   pp
}

fn _poly_pseudo_remainder_zz(list a, list b) any {
   "Compute a content-normalized pseudo-remainder over ZZ polynomials."
   mut r = _poly_primitive_part_zz(a)
   def bb = _poly_primitive_part_zz(b)
   if _poly_is_zero(bb) { return nil }
   def db = _poly_degree_zz(bb)
   def lcb = bb[db]
   while !_poly_is_zero(r) && _poly_degree_zz(r) >= db {
      def dr = _poly_degree_zz(r)
      def lcr = r[dr]
      def shift = dr - db
      mut n = len(r)
      if shift + len(bb) > n { n = shift + len(bb) }
      mut next = []
      mut i = 0
      while i < n {
         def rv, bv = (i < len(r)) ? r[i] * lcb : Z(0), (i >= shift && i < shift + len(bb)) ? bb[i - shift] * lcr : Z(0)
         next = next.append(rv - bv)
         i += 1
      }
      r = _poly_primitive_part_zz(next)
   }
   _poly_trim(r)
}

fn _poly_gcd_zz(list a, list b) list {
   "Return primitive polynomial gcd over ZZ coefficients."
   mut u, v = _poly_primitive_part_zz(a), _poly_primitive_part_zz(b)
   if _poly_is_zero(u) { return v }
   if _poly_is_zero(v) { return u }
   while !_poly_is_zero(v) {
      def r0 = _poly_pseudo_remainder_zz(u, v)
      case r0 {
         nil -> { return [Z(1)] }
         _ -> {
            def r = _poly_primitive_part_zz(r0)
            u, v = v, r
         }
      }
   }
   _poly_primitive_part_zz(u)
}

fn _append_unique_poly(list polys, list p) list {
   "Append polynomial p if it is not already present."
   def pp = _poly_trim(p)
   polys.contains(pp) ? polys : polys.append(pp)
}

fn _poly_common_factor_candidates(list polys) list {
   "Extract candidate common factors from reconstructed relation polynomials."
   mut out = []
   def n = min(len(polys), 18)
   mut i = 0
   while i < n {
      def a = _poly_trim(polys[i])
      if _poly_degree_zz(a) > 0 {
         mut j = i + 1
         while j < n {
            def b = _poly_trim(polys[j])
            if _poly_degree_zz(b) > 0 {
               def g = _poly_gcd_zz(a, b)
               def dg = _poly_degree_zz(g)
               if dg > 0 {
                  out = _append_unique_poly(out, g)
                  if dg <= 2 { return out }
               }
            }
            j += 1
         }
      }
      i += 1
   }
   mut base = nil
   mut done = false
   i = 0
   def poly_count = polys.len
   while !done && i < poly_count && i < 16 {
      def p = _poly_trim(polys[i])
      def dp = _poly_degree_zz(p)
      def first_base = dp > 0 && base == nil
      def do_gcd = dp > 0 && base != nil
      def base_for_gcd = base == nil ? [Z(0)] : base
      def g = _poly_gcd_zz(base_for_gcd, p)
      def dg = do_gcd ? _poly_degree_zz(g) : 0
      out = dg > 0 ? _append_unique_poly(out, g) : out
      base = first_base ? p : ((dg > 0) ? g : base)
      done = dg > 0 && dg <= 2
      i += 1
   }
   out
}

fn create_lattice_univariate(any shifts, any X) any {
   "Builds the univariate lattice basis and monomial degree list."
   if !is_list(shifts) || shifts.len <= 0 { return nil }
   mut max_deg = 0
   mut i = 0
   while i < shifts.len {
      def p = _poly_trim(shifts[i])
      if p.len > 0 {
         def deg = p.len - 1
         if deg > max_deg { max_deg = deg }
      }
      i += 1
   }
   mut monomials = []
   i = 0
   while i <= max_deg {
      monomials = monomials.append(i)
      i += 1
   }
   mut data = []
   i = 0
   while i < shifts.len {
      def s = _poly_trim(shifts[i])
      mut row = []
      mut j = 0
      while j <= max_deg {
         def coeff = (j < s.len) ? s[j] : Z(0)
         row = row.append(coeff * bigint_pow(Z(X), Z(j)))
         j += 1
      }
      data = data.append(row)
      i += 1
   }
   [Matrix(data), monomials]
}

fn reduce_lattice_basis_report(any L, any delta=0.99, str method="ny", any eta=0.51) dict {
   "Shared report-first lattice reduction wrapper for small-roots callers.
   Defaults to LLL and records profile movement for audit output.
   `auto` resolves to the same path."
   def rep = lll_reduce_report(L, delta, method, eta)
   mut out = dict(14)
   out = out.set("method", method)
   out = out.set("selected_method", rep.get("selected_method", method))
   out = out.set("rows", _sr_rows(L))
   out = out.set("cols", _sr_cols(L))
   out = out.set("delta", delta)
   out = out.set("eta", eta)
   out = out.set("profile_before", rep.get("profile_before", []))
   out = out.set("profile_after", rep.get("profile_after", []))
   out = out.set("quality_after", rep.get("after", dict(0)))
   out = out.set("transform_verified", rep.get("transform_verified", false))
   out = out.set("elapsed_ms", rep.get("elapsed_ms", 0.0))
   out = out.set("basis", rep.get("basis"))
   out
}

fn reduce_lattice_basis(any L, any delta=0.99, str method="ny", any eta=0.51) any {
   "Shared lattice reduction wrapper. Returns only the reduced basis."
   reduce_lattice_basis_report(L, delta, method, eta).get("basis")
}

fn _poly_eval_mod_local(list p, any x, any modulus) bigint {
   "Evaluate polynomial p at x modulo modulus."
   mut r, i = Z(0), len(p) - 1
   while i >= 0 {
      r = mod(r * x + p[i], modulus)
      i -= 1
   }
   r
}

fn _poly_roots_mod_bruteforce(list p, any prime) list {
   "Find roots of p modulo a small prime by brute force."
   mut roots = []
   mut x = 0
   while x < prime {
      def root = Z(x)
      roots = _poly_eval_mod_local(p, root, Z(prime)) == Z(0) ? roots.append(root) : roots
      x += 1
   }
   roots
}

fn _append_unique_root(list roots, any r) list {
   "Append root r if it is not already present."
   roots.contains(r) ? roots : roots.append(r)
}

fn _poly_degree_for_roots(list p) int {
   "Return degree used for root-search ordering."
   def pp = _poly_trim(p)
   len(pp) - 1
}

fn _sort_polys_for_roots(list polys) list {
   "Sort relation polynomials by increasing root-search degree."
   mut out = []
   mut i = 0
   while i < polys.len {
      out = out.append(polys[i])
      i += 1
   }
   i = 0
   while i < out.len {
      mut best = i
      mut j = i + 1
      while j < out.len {
         def aj, ab = out[j], out[best]
         def dj, db = _poly_degree_for_roots(aj), _poly_degree_for_roots(ab)
         best = (dj < db || (dj == db && len(aj) < len(ab))) ? j : best
         j += 1
      }
      if best != i {
         def tmp = out[i]
         out[i] = out[best]
         out[best] = tmp
      }
      i += 1
   }
   out
}

fn _prime_root_sets_by_branching(list p, list primes, int max_branch=12) list {
   "Collect usable root sets modulo primes while limiting CRT branching."
   mut sets = []
   mut pi = 0
   while pi < primes.len {
      def pr = primes[pi]
      def rmods = _poly_roots_mod_bruteforce(p, pr)
      def branch = rmods.len
      sets = (branch > 0 && branch < pr && branch <= max_branch) ? sets.append([branch, pr, rmods]) : sets
      pi += 1
   }
   mut i = 0
   while i < sets.len {
      mut best = i
      mut j = i + 1
      while j < sets.len {
         def a, b = sets[j], sets[best]
         best = (a[0] < b[0] || (a[0] == b[0] && a[1] < b[1])) ? j : best
         j += 1
      }
      if best != i {
         def tmp = sets[i]
         sets[i] = sets[best]
         sets[best] = tmp
      }
      i += 1
   }
   sets
}

fn _sr_crt2_z(any a1, any m1, any a2, any m2) bigint {
   "CRT for two possibly-BigInt congruences. Returns the least non-negative solution."
   def za1 = Z(a1)
   def zm1 = Z(m1)
   def za2 = Z(a2)
   def zm2 = Z(m2)
   def g = gcd(zm1, zm2)
   if mod(za2 - za1, g) != Z(0) { return Z(0) }
   def m1g = zm1 / g
   def m2g = zm2 / g
   def lcm = m1g * zm2
   def inv = inverse_mod(m1g, m2g)
   def t = mod(((za2 - za1) / g) * inv, m2g)
   mod(za1 + zm1 * t, lcm)
}

fn _sr_crt_expand(list candidates, list rmods, any pr, int seed=1) list {
   "CRT-expand candidate residue/modulus pairs by residues modulo pr."
   mut next = []
   mut seen = dict(seed)
   def zpr = Z(pr)
   mut ci = 0
   def candidates_len = candidates.len
   def rmods_len = rmods.len
   while ci < candidates_len {
      def cand = candidates[ci]
      def a = cand[0]
      def m = cand[1]
      mut ri = 0
      while ri < rmods_len {
         def g = gcd(Z(m), zpr)
         def nm, na = (Z(m) / g) * zpr, _sr_crt2_z(a, m, rmods[ri], zpr)
         def key = bigint_to_str(na)
         if !seen.contains(key) {
            seen[key] = true
            next = next.append([na, nm])
         }
         ri += 1
      }
      ci += 1
   }
   next
}

fn _sr_center_residue(any r, any m) any {
   "Center a CRT residue around zero."
   r > m / Z(2) ? r - m : r
}

fn _sr_crt_mod_exceeds(list candidates, any bound) bool {
   "Return true once CRT modulus covers the signed search interval."
   candidates.len > 0 && candidates[0][1] > Z(2) * Z(bound) + Z(1)
}

fn _poly_integer_roots_crt(list p, any X) list {
   "Recover bounded integer roots by CRT-combining small-prime roots."
   def bound = Z(X)
   def primes = [101, 103, 107, 109, 113, 127, 131, 137, 139, 149, 151,
      157, 163, 167, 173, 179, 181, 191, 193, 197, 199, 211, 223, 227,
      229, 233, 239, 241, 251, 257, 263, 269, 271, 277, 281, 283, 293,
      307, 311, 313, 317, 331, 337, 347, 349, 353, 359, 367, 373, 379,
      383, 389, 397, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43,
      47, 53, 59, 61, 67, 71, 73, 79, 83, 89, 97]
   def prime_sets = _prime_root_sets_by_branching(p, primes, 10)
   mut candidates = [[Z(0), Z(1)]]
   mut roots = []
   mut pi = 0
   while pi < prime_sets.len && roots.len == 0 {
      def prime_pack = prime_sets[pi]
      def pr = prime_pack[1]
      def rmods = prime_pack[2]
      def rmods_len = rmods.len
      if rmods_len > 0 {
         def candidates_len = candidates.len
         if _sr_trace_on() { eprint("[small_roots] crt prime=" + to_str(pr) + " roots_mod=" + to_str(rmods_len) + " candidates=" + to_str(candidates_len)) }
         def next = _sr_crt_expand(candidates, rmods, pr, candidates_len * rmods_len + 3)
         if len(next) > 2048 {
            pi += 1
            continue
         }
         candidates = next
         if _sr_crt_mod_exceeds(candidates, bound) {
            mut ci = 0
            def candidates_now = candidates.len
            while ci < candidates_now {
               def cand = candidates[ci]
               def r = _sr_center_residue(cand[0], cand[1])
               roots = (abs(r) <= bound && poly_eval(p, r) == Z(0)) ? _append_unique_root(roots, r) : roots
               ci += 1
            }
            pi = primes.len
         }
      }
      pi += 1
   }
   roots
}

fn _intersect_root_sets(list a, list b) list {
   "Intersect two small root-residue lists."
   mut out = []
   mut i = 0
   def b_len = b.len
   while i < a.len {
      def x = a[i]
      mut j = 0
      mut hit = false
      while !hit && j < b_len {
         hit = x == b[j]
         j += 1
      }
      out = hit ? _append_unique_root(out, x) : out
      i += 1
   }
   out
}

fn _common_roots_mod_prime(list polys, any prime, int max_polys=10) list {
   "Find residue roots shared by at least two relation polynomials."
   mut common = []
   mut used = 0
   mut i = 0
   def polys_len = polys.len
   while i < polys_len && i < max_polys {
      def p = _poly_trim(polys[i])
      def rs = p.len > 1 ? _poly_roots_mod_bruteforce(p, prime) : []
      def active = rs.len > 0 && rs.len < prime
      def next = (active && used > 0) ? _intersect_root_sets(common, rs) : []
      def accept = active && (used == 0 || next.len > 0)
      common = accept ? (used == 0 ? rs : next) : common
      used += accept ? 1 : 0
      i += 1
   }
   used >= 2 ? common : []
}

fn _root_exact_hits(list polys, any r, int max_polys=12) int {
   "Count relation polynomials that vanish exactly at integer root r."
   mut hits = 0
   mut i = 0
   def polys_len = polys.len
   while i < polys_len && i < max_polys {
      def p = _poly_trim(polys[i])
      hits += (p.len > 1 && poly_eval(p, r) == Z(0)) ? 1 : 0
      i += 1
   }
   hits
}

fn _poly_common_roots_crt(list polys, any X) list {
   "Recover bounded roots shared by reconstructed relations via CRT."
   if _sr_trace_on() { eprint("[small_roots] common_crt enter polys=" + to_str(len(polys))) }
   def bound = Z(X)
   def primes = [101, 103, 107, 109, 113, 127, 131, 137, 139, 149, 151,
      157, 163, 167, 173, 179, 181, 191, 193, 197, 199, 211, 223, 227,
      229, 233, 239, 241, 251, 257, 263, 269, 271, 277, 281, 283, 293,
      307, 311, 313, 317, 331, 337, 347, 349, 353, 359, 367, 373, 379,
      383, 389, 397]
   mut candidates = [[Z(0), Z(1)]]
   mut roots = []
   mut pi = 0
   while pi < primes.len && roots.len == 0 {
      def pr = primes[pi]
      def rmods = _common_roots_mod_prime(polys, pr, 10)
      def rmods_len = rmods.len
      def trace = _sr_trace_on()
      def _probe_log = (trace && pi == 0) ? eprint("[small_roots] common_crt probe prime=" + to_str(pr) + " roots_mod=" + to_str(rmods_len)) : 0
      def active = rmods_len > 0 && rmods_len <= 8
      def candidates_len = active ? candidates.len : 0
      def _crt_log = (trace && active) ? eprint("[small_roots] common_crt prime=" + to_str(pr) + " roots_mod=" + to_str(rmods_len) + " candidates=" + to_str(candidates_len)) : 0
      def next = active ? _sr_crt_expand(candidates, rmods, pr, candidates_len * rmods_len + 3) : []
      if active && len(next) > 4096 { return [] }
      candidates = active ? next : candidates
      def verify = active && _sr_crt_mod_exceeds(candidates, bound)
      mut ci = 0
      def candidates_now = verify ? candidates.len : 0
      while verify && ci < candidates_now {
         def cand = candidates[ci]
         def r = _sr_center_residue(cand[0], cand[1])
         roots = (abs(r) <= bound && _root_exact_hits(polys, r, 12) >= 2) ? _append_unique_root(roots, r) : roots
         ci += 1
      }
      pi = verify ? primes.len : pi
      pi += 1
   }
   roots
}

fn _root_valid_for_modulus(list ff, any r, any N, any min_factor=nil) bool {
   "Check whether r satisfies the original modular root condition."
   def fr = poly_eval(ff, r)
   def exact = mod(fr, N) == Z(0)
   def bounded = min_factor != nil && min_factor > Z(1)
   def factor = bounded ? gcd(abs(fr), Z(N)) : Z(0)
   def threshold = bounded ? Z(min_factor) : Z(0)
   exact || (bounded && factor >= threshold)
}

fn reconstruct_polynomials_univariate(any B, any f, any modulus, list monomials, any X, bool divide_original=true) list {
   "Reconstruct short integer polynomials from a reduced lattice basis."
   mut polys = []
   mut fallback = []
   mut row = 0
   while row < _sr_rows(B) {
      mut norm_squared = Z(0)
      mut weight = 0
      mut poly = []
      mut ok = true
      mut col = 0
      def cols = _sr_cols(B)
      while col < cols && ok {
         def entry = _sr_get(B, row, col)
         def nonzero = entry != Z(0)
         norm_squared += nonzero ? entry * entry : Z(0)
         weight += nonzero ? 1 : 0
         def deg = int(monomials.get(col, col))
         def scale = bigint_pow(Z(X), Z(deg))
         def scalable = scale != Z(0)
         def valid = scalable ? mod(entry, scale) == Z(0) : false
         while poly.len <= deg { poly = poly.append(Z(0)) }
         poly[deg] = valid ? entry / scale : Z(0)
         ok = valid
         col += 1
      }
      def usable = ok && weight > 0
      poly = usable ? _poly_trim(poly) : poly
      def q = (usable && divide_original && f != nil) ? _poly_div_exact(poly, f) : nil
      poly = q != nil ? q : poly
      def norm_ok = modulus == nil || modulus == Z(0) || norm_squared * Z(max(1, poly.len)) < modulus * modulus
      def nonzero_relation = usable && !_poly_is_zero(poly) && !(poly.len == 1 && poly[0] != Z(0))
      polys = (nonzero_relation && norm_ok) ? polys.append(poly) : polys
      fallback = (nonzero_relation && !norm_ok) ? fallback.append(poly) : fallback
      row += 1
   }
   len(polys) > 0 ? polys : fallback
}

fn _roots_from_poly(list pp, any X) list {
   "Find bounded integer roots from low-degree relation polynomials."
   mut rs = []
   if pp.len <= 1 { return rs }
   def deg = pp.len - 1
   case deg {
      1 -> {
         def a0, a1 = pp[0], pp[1]
         def has_slope = a1 != Z(0)
         def num = has_slope ? -a0 : Z(0)
         def divisible = has_slope ? num % a1 == Z(0) : false
         def r0 = divisible ? num / a1 : Z(0)
         rs = (divisible && abs(r0) <= Z(X)) ? rs.append(r0) : rs
      }
      2 -> {
         def a0, a1 = pp[0], pp[1]
         def a2 = pp[2]
         def quadratic = a2 != Z(0)
         def D = quadratic ? a1*a1 - Z(4)*a2*a0 : Z(-1)
         def square = quadratic && D >= Z(0) && is_square(D)
         def sD = square ? isqrt(D) : Z(0)
         def exact = square && sD*sD == D
         def den = exact ? Z(2)*a2 : Z(1)
         def n1 = exact ? -a1 + sD : Z(0)
         def div1 = exact ? n1 % den == Z(0) : false
         def r1 = div1 ? n1 / den : Z(0)
         rs = (div1 && abs(r1) <= Z(X)) ? rs.append(r1) : rs
         def n2 = exact ? -a1 - sD : Z(0)
         def div2 = exact ? n2 % den == Z(0) : false
         def r2 = div2 ? n2 / den : Z(0)
         rs = (div2 && abs(r2) <= Z(X)) ? rs.append(r2) : rs
      }
      _ -> {}
   }
   rs = len(rs) == 0 ? _poly_integer_roots_crt(pp, X) : rs
   rs
}

fn _modular_univariate_shifts(list ff, any N, int m, int t, int delta) list {
   "Build Howgrave-Graham univariate shift polynomials."
   mut shifts = []
   mut i = 0
   while i < m {
      def Ni = bigint_pow(Z(N), Z(m - i))
      def list fi = _poly_pow(ff, i)
      mut j = 0
      while j < delta {
         mut list g = _poly_shift(fi, j)
         g = is_list(g) ? g : []
         mut k = 0
         while k < g.len {
            poly_set_at(g, k, g[k] * Ni)
            k += 1
         }
         shifts = shifts.append(g)
         j += 1
      }
      i += 1
   }
   def list fm = _poly_pow(ff, m)
   i = 0
   while i < t {
      shifts = shifts.append(_poly_shift(fm, i))
      i += 1
   }
   shifts
}

fn _sr_filter_roots(list ff, any N, any min_factor, list candidates, bool trace_accept=false) list {
   "Filter candidate roots against the original modular predicate."
   mut roots = []
   mut i = 0
   while i < candidates.len {
      def r = candidates[i]
      def valid = _root_valid_for_modulus(ff, r, N, min_factor)
      def _accept_log = (valid && trace_accept && _sr_trace_on()) ? eprint("[small_roots] accept common root=" + to_str(r)) : 0
      roots = valid ? _append_unique_root(roots, r) : roots
      i += 1
   }
   roots
}

fn _sr_roots_from_polys(list ff, any N, any min_factor, any X, list search_polys, bool stop_on_hit=true, bool trace_poly=false) list {
   "Extract and validate roots from reconstructed relation polynomials."
   mut roots = []
   mut i = 0
   while (!stop_on_hit || roots.len == 0) && i < search_polys.len {
      def p = search_polys[i]
      def pp = _poly_trim(p)
      def rs = _roots_from_poly(pp, X)
      def _poly_log = (trace_poly && pp.len > 1 && _sr_trace_on()) ? eprint("[small_roots] root_poly index=" + to_str(i) + " degree=" + to_str(pp.len - 1)) : 0
      mut j = 0
      while j < rs.len {
         def r = rs[j]
         def valid = _root_valid_for_modulus(ff, r, N, min_factor)
         def _accept_log = (valid && trace_poly && _sr_trace_on()) ? eprint("[small_roots] accept poly=" + to_str(i) + " root=" + to_str(r)) : 0
         roots = valid ? _append_unique_root(roots, r) : roots
         j += 1
      }
      i += 1
   }
   roots
}

fn _sr_empty_modular_report(str reason, str reduction_method="ny") dict {
   mut out = dict(8)
   out = out.set("success", false)
   out = out.set("reason", reason)
   out = out.set("roots", [])
   out = out.set("root_count", 0)
   out = out.set("reduction_method", reduction_method)
   out
}

fn modular_univariate_report(any poly_in, any N, any m, any t, any X, any min_factor=nil, str reduction_method="ny") dict {
   "Report-first Howgrave-Graham / May modular univariate small-roots path."
   def t_all = ticks()
   if !is_list(poly_in) { return _sr_empty_modular_report("polynomial must be a coefficient list", reduction_method) }
   if len(poly_in) <= 1 { return _sr_empty_modular_report("polynomial degree must be >= 1", reduction_method) }
   m, t = _sr_small_nonnegative_int(m, "m"), _sr_small_nonnegative_int(t, "t")
   def ff = _poly_trim(poly_in)
   if !is_list(ff) { return _sr_empty_modular_report("trimmed polynomial is invalid", reduction_method) }
   if len(ff) <= 1 { return _sr_empty_modular_report("trimmed polynomial degree must be >= 1", reduction_method) }
   def delta = len(ff) - 1
   def t_shifts = ticks()
   def shifts = _modular_univariate_shifts(ff, N, m, t, delta)
   _sr_trace("shifts count=" + to_str(len(shifts)), t_shifts)
   def t_lattice = ticks()
   def lattice_pack = create_lattice_univariate(shifts, X)
   case lattice_pack {
      nil -> { return _sr_empty_modular_report("failed to build univariate lattice", reduction_method) }
      _ -> {}
   }
   _sr_trace("lattice rows=" + to_str(_sr_rows(lattice_pack[0])) + " cols=" + to_str(_sr_cols(lattice_pack[0])), t_lattice)
   def t_reduce = ticks()
   def reduction = reduce_lattice_basis_report(lattice_pack[0], 0.99, reduction_method)
   def L = reduction.get("basis")
   _sr_trace("reduce", t_reduce)
   def t_reconstruct = ticks()
   def raw_polys = reconstruct_polynomials_univariate(
      L,
      ff,
      bigint_pow(Z(N), Z(m)),
      lattice_pack[1],
      X,
      false
   )
   def ordered_polys = is_list(raw_polys) ? raw_polys : []
   def common_roots = _poly_common_roots_crt(ordered_polys, X)
   def use_exact_gcd = _sr_exact_gcd_on()
   def common_roots_len = common_roots.len
   def common_polys = (common_roots_len == 0 && use_exact_gcd) ? _sort_polys_for_roots(_poly_common_factor_candidates(ordered_polys)) : []
   def polys = _sort_polys_for_roots(ordered_polys)
   def common_polys_len = common_polys.len
   mut search_polys = common_polys_len > 0 ? common_polys : polys
   _sr_trace("reconstruct polys=" + to_str(polys.len) + " common_roots=" + to_str(common_roots_len) + " common=" + to_str(common_polys_len), t_reconstruct)
   def t_roots = ticks()
   mut roots = _sr_filter_roots(ff, N, min_factor, common_roots, true)
   roots = roots.len == 0 ? _sr_roots_from_polys(ff, N, min_factor, X, search_polys, true, true) : roots
   def retry_all = roots.len == 0 && common_polys_len > 0
   search_polys = retry_all ? polys : search_polys
   roots = retry_all ? _sr_roots_from_polys(ff, N, min_factor, X, search_polys, false, false) : roots
   _sr_trace("roots count=" + to_str(len(roots)), t_roots)
   _sr_trace("total", t_all)
   def profile_before = reduction.get("profile_before", [])
   def profile_after = reduction.get("profile_after", [])
   def first_before = profile_before.get(0, Z(0))
   def first_after = profile_after.get(0, Z(0))
   mut out = dict(24)
   out = out.set("success", roots.len > 0)
   out = out.set("reason", roots.len > 0 ? "validated roots found" : "no validated roots")
   out = out.set("validation_status", roots.len > 0 ? "validated" : "no_roots")
   out = out.set("roots", roots)
   out = out.set("root_count", roots.len)
   out = out.set("degree", delta)
   out = out.set("m", m)
   out = out.set("t", t)
   out = out.set("X", X)
   out = out.set("n_bits", bit_length(Z(N)))
   out = out.set("min_factor", min_factor)
   out = out.set("shift_count", shifts.len)
   out = out.set("lattice_rows", _sr_rows(lattice_pack[0]))
   out = out.set("lattice_cols", _sr_cols(lattice_pack[0]))
   out = out.set("reduction_method", reduction_method)
   out = out.set("reduction", reduction)
   out = out.set("profile_improvement_first_norm", first_before - first_after)
   out = out.set("relation_polynomials", polys.len)
   out = out.set("common_roots_tried", common_roots_len)
   out = out.set("relation_polynomials_tried", search_polys.len)
   out = out.set("elapsed_ms", float(ticks() - t_all) / 1000000.0)
   out
}

fn modular_univariate(any poly_in, any N, any m, any t, any X, any min_factor=nil, str reduction_method="ny") list {
   "Shared Howgrave-Graham / May modular univariate small-roots path."
   modular_univariate_report(poly_in, N, m, t, X, min_factor, reduction_method).get("roots", [])
}

fn small_roots_strategy_unavailable(str name) any {
   "Raise the standard small-roots strategy error for unsupported input shapes."
   crypto_fail("lattice.small_roots." + name, "expected a Ny mvpoly sparse polynomial(or a univariate coefficient list where supported)")
}

fn howgrave_graham_modular_univariate(any f, any N, any m, any t, any X, str reduction_method="ny") list {
   "Run the Howgrave-Graham/Coppersmith univariate modular strategy."
   modular_univariate(f, N, m, t, X, nil, reduction_method)
}

fn _sr_exps(int n, int i0=0, int e0=0, int i1=0, int e1=0, int i2=0, int e2=0) list {
   "Build an exponent vector with up to three non-zero positions."
   mut out = []
   mut i = 0
   while i < n {
      mut e = 0
      e = i == i0 ? e0 : e
      e = i == i1 ? e1 : e
      e = i == i2 ? e2 : e
      out = out.append(e)
      i += 1
   }
   out
}

fn _sr_lift(any f, int n) any {
   "Lift an mvpoly to at least n variables by padding exponent vectors."
   if !mv_is_poly(f) { return f }
   if mv_nvars(f) >= n { return f }
   def terms = mv_terms(f)
   mut ts = []
   mut i = 0
   while i < terms.len {
      def t = terms.get(i)
      mut e = clone(t.get(0))
      while e.len < n { e = e.append(0) }
      ts = ts.append([e, t.get(1)])
      i += 1
   }
   mv_poly(n, ts)
}

fn _sr_aono_products(list gsets, int idx, any cur, list out) list {
   "Recursively multiply one shift from each Aono shift set."
   if idx >= gsets.len { return out.append(cur) }
   def group = gsets.get(idx)
   mut i = 0
   while i < group.len {
      out = _sr_aono_products(gsets, idx + 1, mv_mul(cur, group.get(i)), out)
      i += 1
   }
   out
}

fn aono_integer_multivariate(any F, any e, int m, list X, str roots_method="groebner") list {
   "Run Aono-style integer multivariate small-root shifts over mvpoly inputs."
   def invalid_input = !is_list(F) || F.len == 0 || !mv_is_poly(F.get(0))
   def _shape_guard = invalid_input ? small_roots_strategy_unavailable("aono.integer_multivariate") : nil
   def n = mv_nvars(F.get(0))
   mut gsets = []
   mut k = 0
   while k < F.len {
      mut gs = []
      mut i = 0
      while i <= m {
         mut j = 0
         while j <= i {
            def mono = mv_monomial(_sr_exps(n, k, i - j), 1)
            def shift = mv_scale(mv_mul(mono, mv_pow(F.get(k), j)), bigint_pow(e.get(k), m - j))
            gs = gs.append(shift)
            j += 1
         }
         i += 1
      }
      gsets = gsets.append(gs)
      k += 1
   }
   def shifts = _sr_aono_products(gsets, 0, mv_const(n, 1), [])
   def pack = mv_create_lattice(shifts, X)
   if pack != nil { def _reduced = lll(pack.get(0), 0.8, "ny") }
   return mv_find_roots(F, X, nil)
}

fn blomer_may_modular_trivariate(any f, any N, int m, int t, any X, any Y, any Z, str roots_method="groebner") list {
   "Run Blomer-May modular trivariate shifts and bounded root search."
   if !mv_is_poly(f) { small_roots_strategy_unavailable("blomer_may.modular_trivariate") }
   def n = 3
   def ff = _sr_lift(f, n)
   mut shifts = []
   mut i = 0
   while i <= m {
      mut j = 0
      while j <= i {
         mut k = 0
         while k <= j {
            shifts = shifts.append(mv_scale(mv_mul(mv_monomial([j - k, 0, k], 1), mv_pow(ff, m - i)), bigint_pow(N, i)))
            k += 1
         }
         k = 1
         while k <= t {
            shifts = shifts.append(mv_scale(mv_mul(mv_monomial([j, k, 0], 1), mv_pow(ff, m - i)), bigint_pow(N, i)))
            k += 1
         }
         j += 1
      }
      i += 1
   }
   mv_small_roots_modular(ff, bigint_pow(N, m), shifts, [X, Y, Z])
}

fn blomer_may_modular_bivariate(any f, any eM, int m, int t, any Y, any Z, str roots_method="groebner") list {
   "Run Blomer-May modular bivariate shifts and bounded root search."
   if !mv_is_poly(f) { small_roots_strategy_unavailable("blomer_may.modular_bivariate") }
   def ff = _sr_lift(f, 2)
   mut shifts = []
   mut i = 0
   while i <= m {
      mut j = 0
      while j <= i {
         shifts = shifts.append(mv_scale(mv_mul(mv_monomial([j, 0], 1), mv_pow(ff, m - i)), bigint_pow(eM, i)))
         j += 1
      }
      j = 1
      while j <= t {
         shifts = shifts.append(mv_scale(mv_mul(mv_monomial([0, j], 1), mv_pow(ff, m - i)), bigint_pow(eM, i)))
         j += 1
      }
      i += 1
   }
   mv_small_roots_modular(ff, bigint_pow(eM, m), shifts, [Y, Z])
}

fn coron_integer_bivariate(any p, int k, any X, any Y, str roots_method="groebner") list {
   "Run Coron integer bivariate shifts and bounded root search."
   if !mv_is_poly(p) { small_roots_strategy_unavailable("coron.integer_bivariate") }
   def pp = _sr_lift(p, 2)
   def p00 = mv_constant(pp)
   if p00 == 0 { return mv_find_roots([pp], [X, Y], nil) }
   def max_pack = mv_max_norm_scaled(pp, [X, Y])
   mut W = max_pack.get(1)
   while gcd(p00, X) != 1 { X += 1 }
   while gcd(p00, Y) != 1 { Y += 1 }
   while gcd(p00, W) != 1 { W += 1 }
   def u = W + mod(1 - W, abs(p00))
   def nmod = u * bigint_pow(X * Y, k)
   def q = mv_mod_coeffs(mv_scale(pp, inverse_mod(p00, nmod)), nmod)
   def delta = max(mv_degree(pp, 0), mv_degree(pp, 1))
   mut shifts = []
   mut i = 0
   while i <= k + delta {
      mut j = 0
      while j <= k + delta {
         if i <= k && j <= k { shifts = shifts.append(mv_scale(mv_mul(mv_monomial([i, j], bigint_pow(X, k - i) * bigint_pow(Y, k - j)), q), 1)) } else { shifts = shifts.append(mv_monomial([i, j], nmod)) }
         j += 1
      }
      i += 1
   }
   mv_small_roots_integer(pp, shifts, [X, Y])
}

fn coron_direct_integer_bivariate(any p, int k, any X, any Y, str echelon_algorithm="default", str roots_method="groebner") list {
   "Direct Coron integer bivariate strategy entrypoint."
   coron_integer_bivariate(p, k, X, Y, roots_method)
}

fn ernst_integer_trivariate_1(any f, int m, int t, any W, any X, any Y, any Z, bool check_bounds=true, str roots_method="groebner") list {
   "Run Ernst trivariate integer strategy family 1 over mvpoly input."
   if !mv_is_poly(f) { small_roots_strategy_unavailable("ernst.integer_trivariate_1") }
   def ff = _sr_lift(f, 3)
   def R = mv_constant(ff)
   if R == 0 { return mv_find_roots([ff], [X, Y, Z], nil) }
   while gcd(R, X) != 1 { X += 1 }
   while gcd(R, Y) != 1 { Y += 1 }
   while gcd(R, Z) != 1 { Z += 1 }
   while gcd(R, W) != 1 { W += 1 }
   def nmod = bigint_pow(X * Y, m) * bigint_pow(Z, m + t) * W
   def fnorm = mv_mod_coeffs(mv_scale(ff, inverse_mod(R, nmod)), nmod)
   mut shifts = []
   mut i = 0
   while i <= m {
      mut j = 0
      while j <= m - i {
         mut kk = 0
         while kk <= j + t {
            shifts = shifts.append(mv_mul(mv_monomial([i, j, kk], bigint_pow(X, m - i) * bigint_pow(Y, m - j) * bigint_pow(Z, m + t - kk)), fnorm))
            kk += 1
         }
         j += 1
      }
      i += 1
   }
   mv_small_roots_integer(ff, shifts, [X, Y, Z])
}

fn ernst_integer_trivariate_2(any f, int m, int t, any W, any X, any Y, any Z, bool check_bounds=true, str roots_method="groebner") list {
   "Run Ernst trivariate integer strategy family 2 over mvpoly input."
   if !mv_is_poly(f) { small_roots_strategy_unavailable("ernst.integer_trivariate_2") }
   def ff = _sr_lift(f, 3)
   def R = mv_constant(ff)
   if R == 0 { return mv_find_roots([ff], [X, Y, Z], nil) }
   while gcd(R, X) != 1 { X += 1 }
   while gcd(R, Y) != 1 { Y += 1 }
   while gcd(R, Z) != 1 { Z += 1 }
   while gcd(R, W) != 1 { W += 1 }
   def nmod = bigint_pow(X, m) * bigint_pow(Y, m + t) * bigint_pow(Z, m) * W
   def fnorm = mv_mod_coeffs(mv_scale(ff, inverse_mod(R, nmod)), nmod)
   mut shifts = []
   mut i = 0
   while i <= m {
      mut j = 0
      while j <= m - i + t {
         mut kk = 0
         while kk <= m - i {
            shifts = shifts.append(mv_mul(mv_monomial([i, j, kk], bigint_pow(X, m - i) * bigint_pow(Y, m + t - j) * bigint_pow(Z, m - kk)), fnorm))
            kk += 1
         }
         j += 1
      }
      i += 1
   }
   mv_small_roots_integer(ff, shifts, [X, Y, Z])
}

fn herrmann_may_modular_bivariate(any f, any e, int m, int t, any X, any Y, str roots_method="groebner") list {
   "Run Herrmann-May modular bivariate shifts and bounded root search."
   if !mv_is_poly(f) { small_roots_strategy_unavailable("herrmann_may.modular_bivariate") }
   def ff = _sr_lift(f, 2)
   mut shifts = []
   mut k = 0
   while k <= m {
      mut i = 0
      while i <= m - k {
         shifts = shifts.append(mv_scale(mv_mul(mv_monomial([i, 0], 1), mv_pow(ff, k)), bigint_pow(e, m - k)))
         i += 1
      }
      k += 1
   }
   mut j = 1
   while j <= t {
      k = (m / t) * j
      while k <= m {
         shifts = shifts.append(mv_scale(mv_mul(mv_monomial([0, j], 1), mv_pow(ff, k)), bigint_pow(e, m - k)))
         k += 1
      }
      j += 1
   }
   mv_small_roots_modular(ff, bigint_pow(e, m), shifts, [X, Y])
}

fn herrmann_may_modular_multivariate(any f, any N, int m, int t, any X, str roots_method="groebner") list {
   "Run Herrmann-May modular multivariate strategy, or univariate fallback for coefficient lists."
   if is_list(f) && !mv_is_poly(f) { return modular_univariate(f, N, m, t, X.get(0, X)) }
   if !mv_is_poly(f) { small_roots_strategy_unavailable("herrmann_may_multivariate.modular_multivariate") }
   mut shifts = []
   mut k = 0
   while k <= m {
      shifts = shifts.append(mv_scale(mv_pow(f, k), bigint_pow(N, max(t - k, 0))))
      k += 1
   }
   mv_small_roots_modular(f, N, shifts, X)
}

fn jochemsz_may_integer_multivariate(any f, int m, any W, list X, any strategy, str roots_method="resultants") list {
   "Run Jochemsz-May integer multivariate shifts and bounded root search."
   if !mv_is_poly(f) { small_roots_strategy_unavailable("jochemsz_may_integer.integer_multivariate") }
   def n = mv_nvars(f)
   def a0 = mv_constant(f)
   if a0 == 0 { return mv_find_roots([f], X, nil) }
   while gcd(a0, W) != 1 { W += 1 }
   mut R, j = W, 0
   while j < X.len {
      while gcd(a0, X.get(j)) != 1 { X[j] = X.get(j) + 1 }
      R = R * bigint_pow(X.get(j), m)
      j += 1
   }
   def fnorm = mv_mod_coeffs(mv_scale(f, inverse_mod(a0, R)), R)
   mut shifts = []
   shifts = shifts.append(fnorm)
   j = 0
   while j < n {
      shifts = shifts.append(mv_monomial(_sr_exps(n, j, m + 1), R))
      j += 1
   }
   mv_small_roots_integer(f, shifts, X)
}

fn jochemsz_may_modular_multivariate(any f, any N, int m, any X, any strategy, str roots_method="groebner") list {
   "Run Jochemsz-May modular multivariate strategy, or univariate fallback for coefficient lists."
   if is_list(f) && !mv_is_poly(f) { return modular_univariate(f, N, m, 0, X.get(0, X)) }
   if !mv_is_poly(f) { small_roots_strategy_unavailable("jochemsz_may_modular.modular_multivariate") }
   mut shifts = []
   mut k = 0
   while k <= m {
      shifts = shifts.append(mv_scale(mv_pow(f, k), bigint_pow(N, m - k)))
      k += 1
   }
   mv_small_roots_modular(f, bigint_pow(N, m), shifts, X)
}

fn nitaj_fouotsa_modular_trivariate(any f, any e, int m, int t, any X, any Y, any Z, str roots_method="groebner") list {
   "Run Nitaj-Fouotsa modular trivariate shifts and bounded root search."
   if !mv_is_poly(f) { small_roots_strategy_unavailable("nitaj_fouotsa.modular_trivariate") }
   def ff = _sr_lift(f, 3)
   mut shifts = []
   mut k = 0
   while k <= m {
      mut i1 = k
      while i1 <= m {
         def i3 = m - i1
         shifts = shifts.append(mv_scale(mv_mul(mv_monomial([i1 - k, 0, i3], 1), mv_pow(ff, k)), bigint_pow(e, m - k)))
         i1 += 1
      }
      def i3b = m - k
      mut i2 = k + 1
      while i2 <= k + t {
         shifts = shifts.append(mv_scale(mv_mul(mv_monomial([0, i2 - k, i3b], 1), mv_pow(ff, k)), bigint_pow(e, m - k)))
         i2 += 1
      }
      k += 1
   }
   mv_small_roots_modular(ff, bigint_pow(e, m), shifts, [X, Y, Z])
}
