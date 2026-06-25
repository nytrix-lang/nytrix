;; Keywords: protocol sharing math crypto
;; Protocol-analysis routines for Shamir secret sharing, recovery, and forgery analysis.
;; Reference:
;; - https://web.mit.edu/6.857/OldStuff/Fall03/ref/Shamir-HowToShareASecret.pdf
;; - https://cacr.uwaterloo.ca/hac/about/chap12.pdf
;; References:
;; - std.math.crypto.protocol
;; - std.math.crypto
module std.math.crypto.protocol.sharing(lagrange_interpolate_mod, lagrange_recover_secret, int_to_charset, shamir_generate_shares, shamir_recover, shamir_lagrange, shamir_recovery_weights, shamir_recover_with_weights, shamir_share_forgery, shamir_verify_consistency, shamir_lagrange_weight_zero, shamir_replace_share_value, shamir_forced_share_value, share_forgery, deterministic_coefficients_recover, shamir_recover_small_coeff_secret, shamir_recover_ascii_hex_coeffs)
use std.core
use std.core as core
use std.math.nt
use std.math.bin
use std.core.str
use std.math.smt
use std.math.crypto.lattice.cvp as cvpmod
use std.math.crypto.lattice.flatter as flattermod
use std.math.crypto.lattice.lll as lllmod
use std.os.prim (env)

mut _sss_weight_cache_prime = nil
mut _sss_weight_cache_xs = nil
mut _sss_weight_cache_weights = nil

fn _sss_env_enabled(str name, bool fallback=false) bool {
   def v = env(name)
   if !is_str(v) || v.len == 0 { return fallback }
   v == "1" || v == "true" || v == "yes" || v == "on"
}

fn _sss_env_int(str name, int fallback) int {
   def v = env(name)
   if is_str(v) && v.len > 0 { return atoi(v) }
   fallback
}

fn _sss_trace_enabled() bool {
   _sss_env_enabled("NY_SHAMIR_TRACE")
}

fn _sss_trace(str label, any value=nil) any {
   if _sss_trace_enabled() {
      print("[shamir]", label, value)
   }
}

fn _sss_batch_inverse_mod(list vals, any p) list {
   def n = vals.len
   if n == 0 { return [] }
   mut prefix = list(n)
   __list_set_len(prefix, n)
   mut acc = Z(1)
   mut i = 0
   while i < n {
      prefix[i] = acc
      acc = mod(acc * vals[i], p)
      i += 1
   }
   mut inv_acc = inverse_mod(acc, p)
   i = n - 1
   while i >= 0 {
      def v = vals[i]
      vals[i] = mod(inv_acc * prefix[i], p)
      inv_acc = mod(inv_acc * v, p)
      i -= 1
   }
   vals
}

fn _sss_same_xs(list xs, any cached) bool {
   if cached == nil || xs.len != cached.len { return false }
   mut i = 0
   while i < xs.len {
      if xs[i] != cached[i] { return false }
      i += 1
   }
   true
}

fn _sss_lagrange_weights_zero(list xs, any p) list {
   if _sss_weight_cache_weights != nil && _sss_weight_cache_prime == p && _sss_same_xs(xs, _sss_weight_cache_xs) {
      return _sss_weight_cache_weights
   }
   def n = xs.len
   mut dens = list(n)
   mut suffix = list(n + 1)
   __list_set_len(dens, n)
   __list_set_len(suffix, n + 1)
   suffix[n] = Z(1)
   mut i = n - 1
   while i >= 0 {
      suffix[i] = mod(suffix[i + 1] * mod(-xs[i], p), p)
      i -= 1
   }
   i = 0
   while i < n {
      mut den = Z(1)
      mut j = 0
      while j < n {
         if i != j { den = mod(den * mod(xs[i] - xs[j], p), p) }
         j += 1
      }
      dens[i] = den
      i += 1
   }
   def inv_dens = _sss_batch_inverse_mod(dens, p)
   mut weights = list(n)
   __list_set_len(weights, n)
   mut prefix = Z(1)
   i = 0
   while i < n {
      def neg_x = mod(-xs[i], p)
      weights[i] = mod(prefix * suffix[i + 1] * inv_dens[i], p)
      prefix = mod(prefix * neg_x, p)
      i += 1
   }
   _sss_weight_cache_prime = p
   _sss_weight_cache_xs = xs
   _sss_weight_cache_weights = weights
   weights
}

fn lagrange_interpolate_mod(list points, number x, number prime) number {
   "Evaluate the unique polynomial through [x,y] points at x over a prime field."
   def p = Z(prime)
   def x0 = Z(x)
   def n = points.len
   mut xs = list(n)
   __list_set_len(xs, n)
   mut i = 0
   while i < n {
      def pi = points[i]
      xs[i] = mod(Z(pi[0]), p)
      if xs[i] == x0 { return mod(Z(pi[1]), p) }
      i += 1
   }
   mut dens = list(n)
   mut suffix = list(n + 1)
   __list_set_len(dens, n)
   __list_set_len(suffix, n + 1)
   suffix[n] = Z(1)
   i = n - 1
   while i >= 0 {
      suffix[i] = mod(suffix[i + 1] * mod(x0 - xs[i], p), p)
      i -= 1
   }
   i = 0
   while i < n {
      mut den = Z(1)
      mut j = 0
      while j < n {
         if i != j {
            den = mod(den * mod(xs[i] - xs[j], p), p)
         }
         j += 1
      }
      dens[i] = den
      i += 1
   }
   def inv_dens = _sss_batch_inverse_mod(dens, p)
   mut total = Z(0)
   mut prefix = Z(1)
   i = 0
   while i < n {
      def diff = mod(x0 - xs[i], p)
      total = mod(total + mod(Z(points[i][1]), p) * prefix * suffix[i + 1] * inv_dens[i], p)
      prefix = mod(prefix * diff, p)
      i += 1
   }
   return mod(total, p)
}

fn lagrange_recover_secret(list points, number prime) number {
   "Recover the constant term of a Shamir polynomial at x=0 from [x,y] points over prime."
   def p = Z(prime)
   def n = points.len
   mut xs = list(n)
   __list_set_len(xs, n)
   mut i = 0
   while i < n {
      def pi = points[i]
      xs[i] = mod(Z(pi[0]), p)
      if xs[i] == Z(0) { return mod(Z(pi[1]), p) }
      i += 1
   }
   def weights = _sss_lagrange_weights_zero(xs, p)
   mut total = Z(0)
   i = 0
   while i < n {
      total = mod(total + mod(Z(points[i][1]), p) * weights[i], p)
      i += 1
   }
   mod(total, p)
}

fn int_to_charset(number x, str charset) str {
   "Convert a non-negative integer to a string using the given charset as the digit alphabet."
   def base = charset.len
   if base <= 1 { return "" }
   mut n = Z(x)
   if bigint_eq(n, Z(0)) { return str_slice(charset, 0, 1, 1) }
   mut out = ""
   while bigint_gt(n, Z(0)) {
      def digit = mod(n, Z(base))
      def idx = atoi(bigint_to_str(digit))
      out = str_add(str_slice(charset, idx, idx + 1, 1), out)
      n = bigint_div(n, Z(base))
   }
   return out
}

fn shamir_generate_shares(number secret, int threshold, int n_shares, number prime) list {
   "Generate n_shares Shamir shares of the integer secret with a deterministic test polynomial."
   def p = Z(prime)
   mut coeffs = list(threshold)
   __list_set_len(coeffs, threshold)
   coeffs[0] = Z(secret)
   mut i = 1
   while i < threshold {
      coeffs[i] = mod(Z(secret) * Z(i + 1) + Z(i * i + 7), p)
      i += 1
   }
   mut shares = list(n_shares)
   __list_set_len(shares, n_shares)
   mut x = 1
   while x <= n_shares {
      def xz = Z(x)
      mut j = coeffs.len - 1
      mut y = Z(0)
      while j >= 0 {
         y = mod(y * xz + coeffs[j], p)
         j -= 1
      }
      shares[x - 1] = [Z(x), y]
      x += 1
   }
   shares
}

fn shamir_lagrange(list points, number prime) number {
   "Lagrange interpolation at x=0 over a prime field."
   lagrange_recover_secret(points, prime)
}

fn shamir_recover(list shares, number prime) number {
   "Recover a Shamir secret from [x, y] share pairs."
   lagrange_recover_secret(shares, prime)
}

fn shamir_recovery_weights(list shares, number prime) list {
   "Precompute Lagrange weights at x=0 for repeated recovery on the same x coordinates."
   def p = Z(prime)
   def n = shares.len
   mut xs = list(n)
   __list_set_len(xs, n)
   mut i = 0
   while i < n {
      def si = shares[i]
      xs[i] = mod(Z(si[0]), p)
      i += 1
   }
   _sss_lagrange_weights_zero(xs, p)
}

fn shamir_recover_with_weights(list shares, list weights, number prime) number {
   "Recover a Shamir secret using weights from shamir_recovery_weights for the same share x coordinates."
   def p = Z(prime)
   def n = shares.len
   if weights.len < n { return Z(0) }
   mut total = Z(0)
   mut i = 0
   while i < n {
      total = mod(total + mod(Z(shares[i][1]), p) * weights[i], p)
      i += 1
   }
   mod(total, p)
}

fn shamir_share_forgery(number p, number s_orig, number s_target, number x, number y, list xs) number {
   "Forge one Shamir share so recombination shifts s_orig to s_target."
   def c_val = shamir_lagrange_weight_zero(x, xs, p)
   mod((Z(s_target) - Z(s_orig)) * inverse_mod(c_val, p) + Z(y), p)
}

fn shamir_lagrange_weight_zero(number x, list xs, number prime) number {
   "Return the Lagrange basis weight at zero for share x given peer x values.
   This is the multiplier applied to that share's y value when recovering the
   Shamir secret at x=0."
   def p = Z(prime)
   mut weight = Z(1)
   mut den = Z(1)
   mut i = 0
   while i < xs.len {
      def xi = Z(xs[i])
      weight = mod(weight * xi, p)
      den = mod(den * mod(xi - Z(x), p), p)
      i += 1
   }
   mod(weight * inverse_mod(den, p), p)
}

fn shamir_replace_share_value(number p, number current_secret, number target_secret, number x, number current_y, list peer_xs) number {
   "Return a replacement y for share x that changes recovered secret to target_secret.
   current_secret is the secret recovered with the current_y share already included."
   shamir_share_forgery(p, current_secret, target_secret, x, current_y, peer_xs)
}

fn shamir_forced_share_value(number p, number partial_secret, number target_secret, number x, list peer_xs) number {
   "Return the y value for a missing or zeroed share x that forces target_secret."
   shamir_replace_share_value(p, partial_secret, target_secret, x, Z(0), peer_xs)
}

fn share_forgery(number p, number s_orig, number s_target, number x, number y, list xs) number {
   "Alias for shamir_share_forgery."
   shamir_share_forgery(Z(p), Z(s_orig), Z(s_target), Z(x), Z(y), xs)
}

fn shamir_verify_consistency(list shares, number prime) bool {
   "Check adjacent share pairs reconstruct the same secret."
   def n = shares.len
   if n < 2 { return true }
   def ref_secret = shamir_recover([shares[0], shares[1]], prime)
   mut i = 1
   while i < n - 1 {
      if shamir_recover([shares[i], shares[i + 1]], prime) != ref_secret { return false }
      i += 1
   }
   true
}

fn deterministic_coefficients_recover(number p, int k, number a1, fnptr next_coeff_fn, number x, number y) number {
   "Recover the Shamir secret when polynomial coefficients were generated deterministically."
   mut s, a = Z(y), Z(a1)
   mut i = 1
   while i < k {
      s -= a * (Z(x) ^ Z(i))
      a = Z(next_coeff_fn(a))
      i += 1
   }
   mod(s, Z(p))
}

fn _sss_pow_row(any x, int degree, any p) list {
   mut row = []
   mut cur = Z(1)
   mut j = 0
   while j <= degree {
      row = row.append(cur)
      cur = mod(cur * Z(x), p)
      j += 1
   }
   row
}

fn _sss_linear_affine_basis(list mat, list rhs, any p) list {
   mut rows = []
   mut i = 0
   while i < mat.len {
      mut row = []
      mut j = 0
      while j < mat[i].len {
         row = row.append(mod(Z(mat[i][j]), p))
         j += 1
      }
      row = row.append(mod(Z(rhs[i]), p))
      rows = rows.append(row)
      i += 1
   }
   def m = rows.len
   def n = mat[0].len
   mut pivots = []
   mut r = 0
   mut c = 0
   while c < n && r < m {
      mut pivot = -1
      mut scan = r
      while scan < m && pivot < 0 {
         if rows[scan][c] != Z(0) { pivot = scan }
         scan += 1
      }
      if pivot >= 0 {
         def tmp = rows[r]
         rows[r] = rows[pivot]
         rows[pivot] = tmp
         def inv = inverse_mod(rows[r][c], p)
         mut j = c
         while j <= n {
            rows[r][j] = mod(rows[r][j] * inv, p)
            j += 1
         }
         mut rr = 0
         while rr < m {
            if rr != r && rows[rr][c] != Z(0) {
               def factor = rows[rr][c]
               j = c
               while j <= n {
                  rows[rr][j] = mod(rows[rr][j] - factor * rows[r][j], p)
                  j += 1
               }
            }
            rr += 1
         }
         pivots = pivots.append(c)
         r += 1
      }
      c += 1
   }
   mut part = []
   i = 0
   while i < n {
      part = part.append(Z(0))
      i += 1
   }
   i = 0
   while i < pivots.len {
      part[pivots[i]] = rows[i][n]
      i += 1
   }
   mut kernel = []
   c = 0
   while c < n {
      mut is_pivot = false
      i = 0
      while i < pivots.len {
         if pivots[i] == c { is_pivot = true }
         i += 1
      }
      if !is_pivot {
         mut v = []
         i = 0
         while i < n {
            v = v.append(Z(0))
            i += 1
         }
         v[c] = Z(1)
         i = 0
         while i < pivots.len {
            v[pivots[i]] = mod(-rows[i][c], p)
            i += 1
         }
         kernel = kernel.append(v)
      }
      c += 1
   }
   [part, kernel, pivots]
}

fn _sss_eval_coeffs(list coeffs, any x, any p) any {
   mut acc = Z(0)
   mut pow = Z(1)
   mut i = 0
   while i < coeffs.len {
      acc = mod(acc + Z(coeffs[i]) * pow, p)
      pow = mod(pow * Z(x), p)
      i += 1
   }
   acc
}

fn _sss_shares_match(list shares, list coeffs, any p) bool {
   mut i = 0
   while i < shares.len {
      if _sss_eval_coeffs(coeffs, shares[i][0], p) != mod(Z(shares[i][1]), p) { return false }
      i += 1
   }
   true
}

fn _sss_center(any x, any p) any {
   def v = mod(Z(x), p)
   v > p / Z(2) ? v - p : v
}

fn _sss_free_columns(int degree, list pivots) list {
   mut free_cols = []
   mut col = 0
   while col <= degree {
      mut is_pivot = false
      mut i = 0
      while i < pivots.len {
         if pivots[i] == col { is_pivot = true }
         i += 1
      }
      if !is_pivot { free_cols = free_cols.append(col) }
      col += 1
   }
   free_cols
}

fn _sss_small_homogeneous_lattice(list kernel, list free_cols, int degree, any p) list {
   mut hom = []
   mut i = 0
   while i < kernel.len {
      mut row = []
      mut j = 1
      while j <= degree {
         row = row.append(_sss_center(kernel[i][j], p))
         j += 1
      }
      hom = hom.append(row)
      i += 1
   }
   i = 0
   while i <= degree {
      mut is_free = false
      mut fi = 0
      while fi < free_cols.len {
         if free_cols[fi] == i { is_free = true }
         fi += 1
      }
      if !is_free {
         def coord = i - 1
         mut row = []
         mut j = 0
         while j < degree {
            row = row.append(j == coord ? p : Z(0))
            j += 1
         }
         hom = hom.append(row)
      }
      i += 1
   }
   hom
}

fn _sss_part_small_and_target(list part, int degree, any p) list {
   mut part_small = []
   mut target = []
   mut i = 1
   while i <= degree {
      def pi = _sss_center(part[i], p)
      part_small = part_small.append(pi)
      target = target.append(-pi)
      i += 1
   }
   [part_small, target]
}

fn _sss_ascii_hex_center(int byte_count) any {
   mut out = Z(0)
   mut pow = Z(1)
   mut i = 0
   while i < byte_count {
      out += Z(59) * pow
      pow *= Z(256)
      i += 1
   }
   out
}

fn _sss_ascii_hex_fill(int byte_count, int value) any {
   mut out = Z(0)
   mut pow = Z(1)
   mut i = 0
   while i < byte_count {
      out += Z(value) * pow
      pow *= Z(256)
      i += 1
   }
   out
}

fn _sss_full_homogeneous_lattice(list kernel, list free_cols, int degree, any p) list {
   mut hom = []
   mut i = 0
   while i < kernel.len {
      mut row = []
      mut j = 0
      while j <= degree {
         row = row.append(_sss_center(kernel[i][j], p))
         j += 1
      }
      hom = hom.append(row)
      i += 1
   }
   i = 0
   while i <= degree {
      mut is_free = false
      mut fi = 0
      while fi < free_cols.len {
         if free_cols[fi] == i { is_free = true }
         fi += 1
      }
      if !is_free {
         mut row = []
         mut j = 0
         while j <= degree {
            row = row.append(j == i ? p : Z(0))
            j += 1
         }
         hom = hom.append(row)
      }
      i += 1
   }
   hom
}

fn _sss_ascii_hex_bytes(any x, int byte_count) bool {
   def bs = Z(x).bytes
   if bs.len != byte_count { return false }
   mut i = 0
   while i < bs.len {
      def c = bs[i]
      if !((c >= 48 && c <= 57) || (c >= 65 && c <= 70)) { return false }
      i += 1
   }
   true
}

fn _sss_smt_hex_byte(any ctx, any solver, str name) any {
   def b = smt.z3_int_const(ctx, name)
   def digit = smt.z3_mk_and(ctx, [smt.z3_int_ge(ctx, b, smt.z3_int_val(ctx, 48)), smt.z3_int_le(ctx, b, smt.z3_int_val(ctx, 57))])
   def alpha = smt.z3_mk_and(ctx, [smt.z3_int_ge(ctx, b, smt.z3_int_val(ctx, 65)), smt.z3_int_le(ctx, b, smt.z3_int_val(ctx, 70))])
   smt.z3_solver_assert(ctx, solver, smt.z3_mk_or(ctx, [digit, alpha]))
   b
}

fn _sss_ascii_hex_affine_smt_recover(list shares, list part, list kernel, int degree, any p, int byte_count) any {
   if kernel.len == 0 || !smt.z3_available() { return nil }
   mut selected = nil
   mut ki = 0
   while ki < kernel.len && selected == nil {
      if mod(kernel[ki][degree], p) != Z(0) { selected = kernel[ki] }
      ki += 1
   }
   if selected == nil { return nil }
   _sss_trace("affine-smt:start", {"degree": degree, "kernel": kernel.len})
   def inv_top = inverse_mod(selected[degree], p)
   mut g = []
   mut base = []
   mut i = 0
   while i <= degree {
      g = g.append(mod(selected[i] * inv_top, p))
      i += 1
   }
   i = 0
   while i <= degree {
      base = base.append(mod(part[i] - part[degree] * g[i], p))
      i += 1
   }
   smt.z3_global_timeout_ms(30000)
   def ctx = smt.z3_ctx_new()
   if !ctx { return nil }
   def solver = smt.z3_solver_new(ctx)
   if !solver {
      smt.z3_ctx_del(ctx)
      return nil
   }
   mut t_terms = []
   mut pow = Z(1)
   mut k = 0
   while k < byte_count {
      def b = _sss_smt_hex_byte(ctx, solver, "t_" + to_str(k))
      t_terms = t_terms.append(smt.z3_int_mul(ctx, [b, smt.z3_int_val(ctx, pow)]))
      pow *= Z(256)
      k += 1
   }
   def t = smt.z3_int_add(ctx, t_terms)
   def lo = _sss_ascii_hex_fill(byte_count, 48)
   def hi = _sss_ascii_hex_fill(byte_count, 70)
   _sss_trace("affine-smt:bounds", {"lo": lo, "hi": hi})
   smt.z3_solver_assert(ctx, solver, smt.z3_int_ge(ctx, t, smt.z3_int_val(ctx, lo)))
   smt.z3_solver_assert(ctx, solver, smt.z3_int_le(ctx, t, smt.z3_int_val(ctx, hi)))
   mut coeffs = []
   i = 0
   while i <= degree {
      mut c_terms = []
      pow = Z(1)
      k = 0
      while k < byte_count {
         def b = _sss_smt_hex_byte(ctx, solver, "c" + to_str(i) + "_" + to_str(k))
         c_terms = c_terms.append(smt.z3_int_mul(ctx, [b, smt.z3_int_val(ctx, pow)]))
         pow *= Z(256)
         k += 1
      }
      def c = smt.z3_int_add(ctx, c_terms)
      coeffs = coeffs.append(c)
      smt.z3_solver_assert(ctx, solver, smt.z3_int_ge(ctx, c, smt.z3_int_val(ctx, lo)))
      smt.z3_solver_assert(ctx, solver, smt.z3_int_le(ctx, c, smt.z3_int_val(ctx, hi)))
      def wrap = smt.z3_int_const(ctx, "w" + to_str(i))
      def wrap_min = bigint_div(Z(base[i]) + Z(g[i]) * lo - hi, p) - Z(1)
      def wrap_max = bigint_div(Z(base[i]) + Z(g[i]) * hi - lo, p) + Z(1)
      if i == 0 { _sss_trace("affine-smt:wrap0", {"min": wrap_min, "max": wrap_max}) }
      smt.z3_solver_assert(ctx, solver, smt.z3_int_ge(ctx, wrap, smt.z3_int_val(ctx, wrap_min)))
      smt.z3_solver_assert(ctx, solver, smt.z3_int_le(ctx, wrap, smt.z3_int_val(ctx, wrap_max)))
      def rhs = smt.z3_int_sub(ctx, [
            smt.z3_int_add(ctx, [smt.z3_int_val(ctx, base[i]), smt.z3_int_mul(ctx, [smt.z3_int_val(ctx, g[i]), t])]),
            smt.z3_int_mul(ctx, [smt.z3_int_val(ctx, p), wrap])
         ])
      smt.z3_solver_assert(ctx, solver, smt.z3_eq(ctx, c, rhs))
      i += 1
   }
   if !smt.z3_solver_check(ctx, solver) {
      _sss_trace("ascii-affine-smt:miss", {"reason": "unsat-or-timeout"})
      smt.z3_solver_del(ctx, solver)
      smt.z3_ctx_del(ctx)
      return nil
   }
   mut out = []
   i = 0
   while i <= degree {
      def v = smt.z3_model_eval_u64(ctx, solver, coeffs[i])
      if v == nil {
         smt.z3_solver_del(ctx, solver)
         smt.z3_ctx_del(ctx)
         return nil
      }
      out = out.append(Z(v))
      i += 1
   }
   smt.z3_solver_del(ctx, solver)
   smt.z3_ctx_del(ctx)
   _sss_shares_match(shares, out, p) ? [out[0], out] : nil
}

fn _sss_zero_row(int n) list {
   mut row = []
   mut i = 0
   while i < n {
      row = row.append(Z(0))
      i += 1
   }
   row
}

fn _sss_ascii_hex_embedding_scan(list shares, any rows_or_basis, int const_idx, any w_const, any center, list pow256, int degree, int byte_count, any p) any {
   def data = is_int(rows_or_basis[0]) ? rows_or_basis[2] : rows_or_basis
   mut const_hits = 0
   mut share_fail = 0
   mut ascii_fail = 0
   mut r = 0
   while r < data.len {
      def row = data[r]
      if row[const_idx] == w_const || row[const_idx] == -w_const {
         const_hits += 1
         def sign = row[const_idx] == w_const ? Z(1) : Z(-1)
         mut coeffs = []
         mut ci = 0
         while ci <= degree {
            mut v = center
            mut k = 0
            while k < byte_count {
               v += sign * row[ci * byte_count + k] * pow256[k]
               k += 1
            }
            coeffs = coeffs.append(v)
            ci += 1
         }
         if _sss_shares_match(shares, coeffs, p) {
            mut ok = true
            ci = 0
            while ci < coeffs.len {
               if !_sss_ascii_hex_bytes(coeffs[ci], byte_count) { ok = false }
               ci += 1
            }
            if ok { return [coeffs[0], coeffs] }
            ascii_fail += 1
         } else {
            if share_fail == 0 && _sss_trace_enabled() {
               _sss_trace("embedding:candidate-share-miss", {
                     "secret": coeffs.len > 0 ? coeffs[0] : nil,
                     "coeff1": coeffs.len > 1 ? coeffs[1] : nil,
                     "coeff1_ascii": coeffs.len > 1 ? _sss_ascii_hex_bytes(coeffs[1], byte_count) : false,
                     "eval0": coeffs.len > 0 ? _sss_eval_coeffs(coeffs, shares[0][0], p) : nil,
                     "want0": shares[0][1],
                  })
            }
            share_fail += 1
         }
      }
      r += 1
   }
   if _sss_trace_enabled() {
      _sss_trace("embedding:scan-miss", {"const_hits": const_hits, "share_fail": share_fail, "ascii_fail": ascii_fail})
   }
   nil
}

fn _sss_ascii_hex_embedding_recover(list shares, list part, list kernel, int degree, any p, int byte_count) any {
   if kernel.len == 0 { return nil }
   mut ki = 0
   mut selected = nil
   while ki < kernel.len && selected == nil {
      if mod(kernel[ki][degree], p) != Z(0) { selected = kernel[ki] }
      ki += 1
   }
   if selected == nil { return nil }
   def inv_top = inverse_mod(selected[degree], p)
   mut g = []
   mut i = 0
   while i <= degree {
      g = g.append(mod(selected[i] * inv_top, p))
      i += 1
   }
   mut base = []
   i = 0
   while i <= degree {
      base = base.append(mod(part[i] - part[degree] * g[i], p))
      i += 1
   }
   def coeff_vars = (degree + 1) * byte_count
   def const_idx = coeff_vars
   def eq_idx = coeff_vars + 1
   def dim = coeff_vars + 1 + degree
   def center = _sss_ascii_hex_center(byte_count)
   def w_eq = Z(2) ^ Z(_sss_env_int("NY_SHAMIR_EQ_BITS", 16))
   def w_const = Z(11)
   mut pow256 = []
   i = 0
   mut cur = Z(1)
   while i < byte_count {
      pow256 = pow256.append(cur)
      cur *= Z(256)
      i += 1
   }
   mut rows = []
   i = 0
   while i < degree {
      mut k = 0
      while k < byte_count {
         mut row = _sss_zero_row(dim)
         row[i * byte_count + k] = Z(1)
         row[eq_idx + i] = pow256[k] * w_eq
         rows = rows.append(row)
         k += 1
      }
      i += 1
   }
   mut k = 0
   while k < byte_count {
      mut row = _sss_zero_row(dim)
      row[degree * byte_count + k] = Z(1)
      i = 0
      while i < degree {
         row[eq_idx + i] = mod(-g[i] * pow256[k], p) * w_eq
         i += 1
      }
      rows = rows.append(row)
      k += 1
   }
   mut const_row = _sss_zero_row(dim)
   const_row[const_idx] = w_const
   i = 0
   while i < degree {
      def target = mod(base[i] + center * g[i] - center, p)
      const_row[eq_idx + i] = mod(-target, p) * w_eq
      i += 1
   }
   rows = rows.append(const_row)
   i = 0
   while i < degree {
      mut row = _sss_zero_row(dim)
      row[eq_idx + i] = p * w_eq
      rows = rows.append(row)
      i += 1
   }
   mut work = [dim, dim, rows]
   mut found = _sss_ascii_hex_embedding_scan(shares, work, const_idx, w_const, center, pow256, degree, byte_count, p)
   def step_cap = _sss_env_int("NY_SHAMIR_LLL_STEPS", 2048)
   def max_passes = _sss_env_int("NY_SHAMIR_LLL_PASSES", 64)
   mut pass = 0
   while found == nil && pass < max_passes {
      work = lllmod.lll_reduce_bounded(work, step_cap, 0.99, "bounded-fast-no-transform", 0.51)
      found = _sss_ascii_hex_embedding_scan(shares, work, const_idx, w_const, center, pow256, degree, byte_count, p)
      pass += 1
   }
   if found != nil {
      if _sss_trace_enabled() { _sss_trace("embedding:lll-hit", {"passes": pass}) }
      return found
   }
   if _sss_env_enabled("NY_SHAMIR_FLATTER_FALLBACK") {
      def reduced = flattermod.flatter_reduce([dim, dim, rows], 0.99, 8, 0.51)
      found = _sss_ascii_hex_embedding_scan(shares, reduced, const_idx, w_const, center, pow256, degree, byte_count, p)
      if found != nil { return found }
   }
   if _sss_trace_enabled() { _sss_trace("embedding:miss", {"dim": dim, "passes": pass}) }
   nil
}

fn _sss_secret_from_small_coeffs(list shares, list coeffs, int degree, any p) list {
   def x0 = Z(shares[0][0])
   def y0 = Z(shares[0][1])
   mut secret = mod(y0, p)
   mut pow = mod(x0, p)
   mut j = 1
   while j <= degree {
      secret = mod(secret - coeffs[j] * pow, p)
      pow = mod(pow * x0, p)
      j += 1
   }
   coeffs[0] = secret
   [secret, coeffs]
}

fn shamir_recover_small_coeff_secret(list shares, number prime, int degree, any coeff_bound=nil) any {
   "Recover a Shamir secret from too few shares when non-secret coefficients are small.
   Returns [secret, coeffs] or nil.  The method solves the affine system over F_p
   and applies LLL to the lattice of possible c1..c_degree values."
   def p = Z(prime)
   mut mat = []
   mut rhs = []
   mut i = 0
   while i < shares.len {
      mat = mat.append(_sss_pow_row(shares[i][0], degree, p))
      rhs = rhs.append(Z(shares[i][1]))
      i += 1
   }
   def affine = _sss_linear_affine_basis(mat, rhs, p)
   def part = affine[0]
   def kernel = affine[1]
   def bound = coeff_bound == nil ? nil : Z(coeff_bound)
   def free_cols = _sss_free_columns(degree, affine[2])
   def hom = _sss_small_homogeneous_lattice(kernel, free_cols, degree, p)
   def centered = _sss_part_small_and_target(part, degree, p)
   def part_small = centered[0]
   def target = centered[1]
   def nearest = cvpmod.cvp(hom, target, true)
   if nearest.len == degree {
      mut coeffs = [Z(0)]
      mut ok = true
      mut j = 0
      while j < degree {
         def cj = part_small[j] + nearest[j]
         if bound != nil && (bigint_abs(cj) > bound) { ok = false }
         coeffs = coeffs.append(cj)
         j += 1
      }
      if ok {
         def recovered = _sss_secret_from_small_coeffs(shares, coeffs, degree, p)
         def secret = recovered[0]
         coeffs = recovered[1]
         if _sss_shares_match(shares, coeffs, p) { return [secret, coeffs] }
      }
   }
   nil
}

fn shamir_recover_ascii_hex_coeffs(list shares, number prime, int degree, int byte_count=8) any {
   "Recover Shamir coefficients when every coefficient is an uppercase ASCII hex byte string.
   Returns [secret, coeffs] or nil. This models random bytes encoded as uppercase ASCII hex."
   _sss_trace("ascii:start", {"shares": shares.len, "degree": degree, "byte_count": byte_count})
   def p = Z(prime)
   mut mat = []
   mut rhs = []
   mut i = 0
   while i < shares.len {
      mat = mat.append(_sss_pow_row(shares[i][0], degree, p))
      rhs = rhs.append(Z(shares[i][1]))
      i += 1
   }
   def affine = _sss_linear_affine_basis(mat, rhs, p)
   def part = affine[0]
   if _sss_env_enabled("NY_SHAMIR_SMT") {
      def affine_smt = _sss_ascii_hex_affine_smt_recover(shares, part, affine[1], degree, p, byte_count)
      if affine_smt != nil { return affine_smt }
   }
   if _sss_env_enabled("NY_SHAMIR_CVP") {
      def free_cols = _sss_free_columns(degree, affine[2])
      def hom = _sss_full_homogeneous_lattice(affine[1], free_cols, degree, p)
      def center = _sss_ascii_hex_center(byte_count)
      mut target = []
      i = 0
      while i <= degree {
         target = target.append(center - _sss_center(part[i], p))
         i += 1
      }
      def nearest = cvpmod.cvp(hom, target, true)
      if nearest.len == degree + 1 {
         mut coeffs = []
         i = 0
         while i <= degree {
            coeffs = coeffs.append(_sss_center(part[i], p) + nearest[i])
            i += 1
         }
         if _sss_shares_match(shares, coeffs, p) {
            mut ok = true
            i = 0
            while i < coeffs.len {
               if !_sss_ascii_hex_bytes(coeffs[i], byte_count) { ok = false }
               i += 1
            }
            if ok { return [coeffs[0], coeffs] }
         }
      }
   }
   _sss_ascii_hex_embedding_recover(shares, part, affine[1], degree, p, byte_count)
}

#main {
   def prime = Z(41)
   def xs = [Z(1), Z(2), Z(3), Z(4), Z(5)]
   def ys = [Z(34), Z(0), Z(3), Z(2), Z(38)]
   def points = zip2(xs, ys)
   assert(lagrange_interpolate_mod(points, Z(0), prime) == Z(23), "lagrange_eval_at_zero")
   assert(lagrange_recover_secret(points, prime) == Z(23), "secret recovery")
   def shares = shamir_generate_shares(Z(23), 3, 5, prime)
   assert(shares[0][0] == Z(1), "share index 1")
   assert(shares[0][1] == Z(34), "share value 1")
   assert(shares[1][0] == Z(2), "share index 2")
   assert(shares[1][1] == Z(0), "share value 2")
   assert(shares[2][0] == Z(3), "share index 3")
   assert(shares[2][1] == Z(3), "share value 3")
   assert(shares[3][0] == Z(4), "share index 4")
   assert(shares[3][1] == Z(2), "share value 4")
   assert(shares[4][0] == Z(5), "share index 5")
   assert(shares[4][1] == Z(38), "share value 5")
   assert(shamir_recover([shares[0], shares[1], shares[2]], prime) == Z(23), "recover first 3")
   assert(shamir_recover([shares[2], shares[3], shares[4]], prime) == Z(23), "recover last 3")
   assert(shamir_recover([shares[0], shares[2], shares[4]], prime) == Z(23), "recover every other")
   assert(int_to_charset(Z(23), "0123456789abcdef") == "17", "int_to_charset")
   assert(shamir_recovery_weights([[Z(1), Z(34)], [Z(2), Z(0)], [Z(3), Z(3)]], prime) == [Z(3), Z(38), Z(1)], "weights")
   assert(shamir_recover_with_weights([[Z(1), Z(34)], [Z(2), Z(0)], [Z(3), Z(3)]], [Z(3), Z(38), Z(1)], prime) == Z(23), "recover with weights")
   def p2 = Z(65537)
   def secret2 = Z(4242)
   def shares2 = shamir_generate_shares(secret2, 3, 5, p2)
   assert(shares2.len == 5, "5 shares")
   assert(shamir_recover([shares2[0], shares2[1], shares2[2]], p2) == secret2, "recover 3 shares")
   assert(shamir_lagrange([[shares2[0][0], shares2[0][1]], [shares2[1][0], shares2[1][1]], [shares2[2][0], shares2[2][1]]], p2) == secret2, "lagrange at 0")
   def p3 = Z(13)
   def secret3 = Z(7)
   def shares3 = shamir_generate_shares(secret3, 3, 5, p3)
   assert(shares3.len == 5, "5 shares b4")
   assert(shamir_recover([shares3[0], shares3[2], shares3[4]], p3) == secret3, "recover with gap")
   def p4 = Z(65537)
   def secret4 = Z(12345)
   def shares4 = shamir_generate_shares(secret4, 9, 10, p4)
   assert(shares4.len == 10, "10 shares")
   assert(shamir_recover([shares4[0], shares4[1], shares4[2], shares4[3], shares4[4], shares4[5], shares4[6], shares4[7], shares4[8]], p4) == secret4, "recover 9 shares")
   def peer_xs = [Z(1), Z(2), Z(4)]
   def weight = shamir_lagrange_weight_zero(Z(3), peer_xs, Z(101))
   def forced = shamir_forced_share_value(Z(101), Z(10), Z(42), Z(3), peer_xs)
   assert(forced == shamir_replace_share_value(Z(101), Z(10), Z(42), Z(3), Z(0), peer_xs), "forced share")
   assert(mod(Z(10) + forced * weight, Z(101)) == Z(42), "forced share moves secret")
   def forged = share_forgery(Z(101), Z(10), Z(42), Z(3), Z(77), [Z(1), Z(2), Z(4)])
   assert(forged >= Z(0) && forged < Z(101), "share forgery range")
   fn next_a(number a) number { a + Z(1) }
   def rec = deterministic_coefficients_recover(Z(101), 3, Z(7), next_a, Z(2), Z(51))
   assert(rec == Z(5), "deterministic coefficients recover")
   print("✓ std.math.crypto.protocol.sharing self-test passed")
}
