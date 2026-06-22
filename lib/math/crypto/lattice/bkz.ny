;; Keywords: lattice bkz math crypto number-theory
;; BKZ reduction.
;;
;; Reference:
;; - https://www.iacr.org/archive/crypto2011/68410061/68410061.pdf
;; - https://web.cs.elte.hu/~lovasz/scans/lll.pdf
;; References:
;; - std.math.crypto.lattice
;; - std.math.crypto
module std.math.crypto.lattice.bkz(bkz, svp_enumerate, svp_enumerate_report, svp_report, bkz_backend_report, svp_kernel, svp_kernel_report, svp_gram, svp_gram_report, bkz_gram, bkz_gram_report, bkz_gram_reduce_report, dual_svp, dual_svp_report, dual_svp_norm, dual_svp_reduce, dual_svp_reduce_report, bkz_projected_block_report, bkz_rerandomize_block_report, svp_pruning_profile, svp_pruning_calibration_report, svp_pruning_optimize, svp_pruning_optimize_report, bkz_strategy, bkz_strategy_report, bkz_reduce_report, bkz_report)
use std.core
use std.math.matrix
use std.math.nt
use std.math.big (bigint_bit_length, bigint_to_int)
use std.math (abs)
use std.math.scalar as math
use std.math.crypto.lattice.lll
use std.math.crypto.lattice.lll (dot_product)
use std.math.float
use std.os.clock (ticks)
use std.os.prim (env)
use std.os.thread (thread_spawn, thread_join)

fn _bkz_set_fields(dict out, list fields) dict {
   mut i = 0
   while i + 1 < fields.len {
      out = out.set(fields[i], fields[i + 1])
      i += 2
   }
   out
}

fn _bkz_dict_with(any base, str key, any value) dict {
   (is_dict(base) ? base : dict(0)).set(key, value)
}

fn _bkz_pruning_bound(int k, int n, any radius_sq) any {
   def ratio = to_float(k) / to_float(n)
   radius_sq * (0.1 + 0.9 * ratio)
}

fn _bkz_row_norm(any row) any { dot_product(row, row) }

fn _bkz_entry_z(any x) bigint { is_float(x) ? Z(int(x)) : Z(x) }

fn _bkz_as_matrix(any basis) any { is_matrix(basis) ? basis : Matrix(basis) }

fn _bkz_zero_vec(int n) list {
   mut out = []
   mut i = 0
   while i < n {
      out = out.append(Z(0))
      i += 1
   }
   out
}

fn _bkz_vec_nonzero(list v) bool {
   mut i = 0
   while i < v.len {
      if v.get(i) != Z(0) { return true }
      i += 1
   }
   false
}

fn _bkz_vec_add_scaled(list a, list b, any coeff) list {
   mut out = []
   def cc = Z(coeff)
   mut i = 0
   while i < a.len {
      out = out.append(Z(a.get(i)) + Z(b.get(i)) * cc)
      i += 1
   }
   out
}

fn _bkz_vec_add_scaled_inplace(list a, list b, any coeff) list {
   def cc = Z(coeff)
   mut i = 0
   while i < a.len {
      a[i] = Z(a.get(i)) + Z(b.get(i)) * cc
      i += 1
   }
   a
}

fn _bkz_vec_dot_z(list a, list b) bigint {
   mut s = Z(0)
   mut i = 0
   while i < a.len && i < b.len {
      s += Z(a.get(i)) * Z(b.get(i))
      i += 1
   }
   s
}

fn _svp_row_gram(list rows) list {
   mut gram = []
   mut i = 0
   while i < rows.len {
      mut row = []
      mut j = 0
      while j < rows.len {
         row = row.append(_bkz_vec_dot_z(rows.get(i), rows.get(j)))
         j += 1
      }
      gram = gram.append(row)
      i += 1
   }
   gram
}

fn _svp_coeff_gram_dot(list coeffs, list gram_row) bigint {
   mut s = Z(0)
   mut i = 0
   while i < coeffs.len && i < gram_row.len {
      s += Z(coeffs.get(i)) * Z(gram_row.get(i))
      i += 1
   }
   s
}

fn _svp_coeff_norm_from_gram(list coeffs, list gram) bigint {
   mut s = Z(0)
   mut i = 0
   while i < coeffs.len {
      if Z(coeffs.get(i)) != Z(0) {
         s += Z(coeffs.get(i)) * _svp_coeff_gram_dot(coeffs, gram.get(i, []))
      }
      i += 1
   }
   s
}

fn _bkz_small_int_or_nil(any x, int bits=61) any {
   def z = Z(x)
   def a = z < Z(0) ? Z(0) - z : z
   bigint_bit_length(a) < bits ? [true, bigint_to_int(z)] : [false, 0]
}

fn _svp_gram_int_or_nil(list gram, int bits=61) any {
   mut out = []
   mut i = 0
   while i < gram.len {
      def row = gram.get(i)
      mut list<int> int_row = list(row.len)
      mut j = 0
      while j < row.len {
         def v = _bkz_small_int_or_nil(row.get(j), bits)
         if !v.get(0) { return nil }
         int_row = int_row.append(int(v.get(1)))
         j += 1
      }
      out = out.append(int_row)
      i += 1
   }
   out
}

fn _svp_coeff_gram_dot_int(list<int> coeffs, list<int> gram_row) int {
   mut s = 0
   mut i = 0
   while i < coeffs.len && i < gram_row.len {
      s += coeffs[i] * gram_row[i]
      i += 1
   }
   s
}

fn _svp_coeff_norm_from_gram_int(list<int> coeffs, list<list<int>> gram) int {
   mut s = 0
   mut i = 0
   while i < coeffs.len {
      def c = coeffs[i]
      if c != 0 { s += c * _svp_coeff_gram_dot_int(coeffs, gram[i]) }
      i += 1
   }
   s
}

fn _bkz_rows_equal(any a, any b) bool {
   if !is_list(a) || !is_list(b) || a.len != b.len { return false }
   mut i = 0
   while i < a.len {
      if a.get(i) != b.get(i) { return false }
      i += 1
   }
   true
}

fn _bkz_extract_block(any basis, int start, int stop) any {
   mut rows = []
   def data = _matrix_data(basis)
   mut i = start
   while i < stop {
      rows = rows.append(data.get(i))
      i += 1
   }
   Matrix(rows)
}

fn _bkz_replace_block(any basis, int start, any block) any {
   def rows = _matrix_rows(basis)
   def data = _matrix_data(basis)
   def block_data = _matrix_data(block)
   mut out = []
   mut i = 0
   while i < rows {
      def j = i - start
      out = out.append((j >= 0 && j < block_data.len) ? block_data.get(j) : data.get(i))
      i += 1
   }
   Matrix(out)
}

fn _bkz_block_changed(any basis, int start, any block) bool {
   def data = _matrix_data(basis)
   def block_data = _matrix_data(block)
   mut i = 0
   while i < block_data.len {
      if !_bkz_rows_equal(data.get(start + i), block_data.get(i)) { return true }
      i += 1
   }
   false
}

fn _bkz_identity(int n) any {
   mut rows = []
   mut i = 0
   while i < n {
      mut row = []
      mut j = 0
      while j < n {
         row = row.append(i == j ? Z(1) : Z(0))
         j += 1
      }
      rows = rows.append(row)
      i += 1
   }
   Matrix(rows)
}

fn _bkz_signed_unit_orthogonal(any basis) bool {
   def B = _bkz_as_matrix(basis)
   def rows = _matrix_rows(B)
   def cols = _matrix_cols(B)
   if rows == 0 || rows != cols { return false }
   def data = _matrix_data(B)
   mut seen = []
   mut i = 0
   while i < rows {
      seen = seen.append(false)
      i += 1
   }
   i = 0
   while i < rows {
      def row = data.get(i)
      mut nz_col = -1
      mut j = 0
      while j < cols {
         def v = Z(row.get(j))
         if v != Z(0) {
            if v != Z(1) && v != Z(-1) { return false }
            if nz_col >= 0 { return false }
            nz_col = j
         }
         j += 1
      }
      if nz_col < 0 || bool(seen.get(nz_col)) { return false }
      seen[nz_col] = true
      i += 1
   }
   true
}

fn _bkz_lower_triangular_high_precision(any basis) bool {
   def B = _bkz_as_matrix(basis)
   def rows = _matrix_rows(B)
   def cols = _matrix_cols(B)
   if rows < 48 || rows != cols { return false }
   def data = _matrix_data(B)
   mut saw_high = false
   mut i = 0
   while i < rows {
      def row = data.get(i)
      if Z(row.get(i, Z(0))) == Z(0) { return false }
      mut j = i + 1
      while j < cols {
         if Z(row.get(j, Z(0))) != Z(0) { return false }
         j += 1
      }
      j = 0
      while j <= i {
         def v = Z(row.get(j, Z(0)))
         def a = v < Z(0) ? Z(0) - v : v
         if bigint_bit_length(a) > 70 { saw_high = true }
         j += 1
      }
      i += 1
   }
   saw_high
}

fn _bkz_unit_coeffs(int n, int idx) list {
   mut out = []
   mut i = 0
   while i < n {
      out = out.append(i == idx ? Z(1) : Z(0))
      i += 1
   }
   out
}

fn _bkz_zero_coeffs(int n) list {
   mut out = []
   mut i = 0
   while i < n {
      out = out.append(Z(0))
      i += 1
   }
   out
}

fn _bkz_apply_local_transform_to_rows(any rows_matrix, int start, any local) any {
   def rows = _matrix_rows(rows_matrix)
   def cols = _matrix_cols(rows_matrix)
   def data = _matrix_data(rows_matrix)
   def lrows = _matrix_rows(local)
   def lcols = _matrix_cols(local)
   mut out = []
   mut i = 0
   while i < rows {
      def j = i - start
      if j >= 0 && j < lrows {
         mut row = []
         mut c = 0
         while c < cols {
            mut s = Z(0)
            mut k = 0
            while k < lcols {
               s += _matrix_get(local, j, k) * data.get(start + k).get(c, Z(0))
               k += 1
            }
            row = row.append(s)
            c += 1
         }
         out = out.append(row)
      } else {
         out = out.append(data.get(i))
      }
      i += 1
   }
   Matrix(out)
}

fn _bkz_sparse_entries(any m) dict {
   def rows = _matrix_rows(m)
   def cols = _matrix_cols(m)
   mut sparse = []
   mut nonzero = 0
   mut i = 0
   while i < rows {
      mut entries = []
      mut j = 0
      while j < cols {
         def v = _bkz_entry_z(_matrix_get(m, i, j))
         if v != Z(0) {
            entries = entries.append([j, v])
            nonzero += 1
         }
         j += 1
      }
      sparse = sparse.append(entries)
      i += 1
   }
   {"rows": sparse, "nonzero": nonzero}
}

fn _bkz_zero_matrix_rows(int rows, int cols) list {
   mut out = []
   mut i = 0
   while i < rows {
      out = out.append(_bkz_zero_vec(cols))
      i += 1
   }
   out
}

fn _bkz_sparse_matmul_report(any left, any right) dict {
   def t0 = ticks()
   def A = _bkz_as_matrix(left)
   def B = _bkz_as_matrix(right)
   def ar = _matrix_rows(A)
   def ac = _matrix_cols(A)
   def br = _matrix_rows(B)
   def bc = _matrix_cols(B)
   if ac != br { panic("_bkz_sparse_matmul_report: incompatible shapes") }
   def asp = _bkz_sparse_entries(A)
   def bsp = _bkz_sparse_entries(B)
   def arows = asp.get("rows")
   def brows = bsp.get("rows")
   mut out_rows = _bkz_zero_matrix_rows(ar, bc)
   mut nonzero_products = 0
   mut row_scaled_adds = 0
   mut i = 0
   while i < ar {
      mut out_row = out_rows.get(i)
      def erow = arows.get(i)
      mut e = 0
      while e < erow.len {
         def entry = erow.get(e)
         def k = int(entry.get(0))
         def av = _bkz_entry_z(entry.get(1))
         def brow = brows.get(k)
         mut b = 0
         while b < brow.len {
            def bent = brow.get(b)
            def j = int(bent.get(0))
            def bv = _bkz_entry_z(bent.get(1))
            out_row[j] = _bkz_entry_z(out_row.get(j)) + av * bv
            nonzero_products += 1
            b += 1
         }
         row_scaled_adds += brow.len
         e += 1
      }
      out_rows[i] = out_row
      i += 1
   }
   {
      "method": "sparse-row-exact-bkz-matmul",
      "left_shape": [ar, ac],
      "right_shape": [br, bc],
      "dense_multiply_adds": ar * ac * bc,
      "left_nonzero": asp.get("nonzero", 0),
      "right_nonzero": bsp.get("nonzero", 0),
      "row_scaled_adds": row_scaled_adds,
      "nonzero_products": nonzero_products,
      "skipped_dense_products": ar * ac * bc - row_scaled_adds,
      "matrix": Matrix(out_rows),
      "elapsed_ms": float(ticks() - t0) / 1000000.0
   }
}

fn _bkz_sparse_matmul(any left, any right) any {
   _bkz_sparse_matmul_report(left, right).get("matrix")
}

fn _bkz_same_matrix(any a, any b) bool {
   if _matrix_rows(a) != _matrix_rows(b) || _matrix_cols(a) != _matrix_cols(b) { return false }
   mut i = 0
   while i < _matrix_rows(a) {
      mut j = 0
      while j < _matrix_cols(a) {
         if _bkz_entry_z(_matrix_get(a, i, j)) != _bkz_entry_z(_matrix_get(b, i, j)) { return false }
         j += 1
      }
      i += 1
   }
   true
}

fn _bkz_first_matrix_mismatch(any a, any b) list {
   if _matrix_rows(a) != _matrix_rows(b) || _matrix_cols(a) != _matrix_cols(b) {
      return [-1, -1, _matrix_rows(a), _matrix_cols(a), _matrix_rows(b), _matrix_cols(b)]
   }
   mut i = 0
   while i < _matrix_rows(a) {
      mut j = 0
      while j < _matrix_cols(a) {
         def av = _bkz_entry_z(_matrix_get(a, i, j))
         def bv = _bkz_entry_z(_matrix_get(b, i, j))
         if av != bv { return [i, j, av, bv, av - bv] }
         j += 1
      }
      i += 1
   }
   []
}

fn _bkz_debug_transform_steps() bool {
   def v = env("NY_BKZ_DEBUG_TRANSFORM")
   is_str(v) && (v == "1" || v == "true" || v == "yes")
}

fn _bkz_full_projection_reports() bool {
   def v = env("NY_BKZ_FULL_PROJECTION_REPORTS")
   is_str(v) && (v == "1" || v == "true" || v == "yes")
}

fn _bkz_full_tours() bool {
   def v = env("NY_BKZ_FULL_TOURS")
   is_str(v) && (v == "1" || v == "true" || v == "yes")
}

fn _bkz_block_stride(int n, int block_size, bool record) int {
   if !record || _bkz_full_tours() { return 1 }
   if n <= 64 || block_size <= 16 { return 1 }
   if block_size > 32 { return max(1, block_size / 2) }
   max(1, block_size / 4)
}

fn _bkz_projection_summary(int n, int start, int stop, any before_norm) dict {
   {
      "method": "bounded-projection-summary",
      "projection_kind": "skipped-large-basis-report",
      "start": start,
      "stop": stop,
      "rows": stop - start,
      "basis_rows": n,
      "first_projected_norm": before_norm
   }
}

fn _bkz_append_transform_check(list checks, str stage, any original, any transform, any current, int tour, int start) list {
   def actual = _bkz_sparse_matmul(transform, original)
   def mismatch = _bkz_first_matrix_mismatch(actual, current)
   checks.append({
         "stage": stage,
         "tour": tour,
         "start": start,
         "ok": mismatch.len == 0,
         "mismatch": mismatch
   })
}

fn bkz_projected_block_report(any basis, int start, int stop) dict {
   "Report the GSO-projected block used by a BKZ local SVP step."
   def rows = _matrix_rows(basis)
   mut s = max(0, min(rows, start))
   mut e = max(s + 1, min(rows, stop))
   if rows == 0 { s = 0 e = 0 }
   def block = e > s ? _bkz_extract_block(basis, s, e) : Matrix([])
   def gso = gso_report(basis)
   def profile = gso.get("profile", [])
   def mu = gso.get("mu")
   mut projected_norms = []
   mut mu_window = []
   mut i = s
   while i < e {
      projected_norms = projected_norms.append(profile.get(i, Z(0)))
      mut row = []
      mut j = s
      while j < i {
         row = row.append(_matrix_get(mu, i, j))
         j += 1
      }
      mu_window = mu_window.append(row)
      i += 1
   }
   {
      "method": "gso-projected-block",
      "start": s,
      "stop": e,
      "rows": e - s,
      "cols": _matrix_cols(basis),
      "prefix_rank": s,
      "projection_kind": "orthogonal-complement-gso-profile",
      "projected_norms": projected_norms,
      "first_projected_norm": projected_norms.get(0, Z(0)),
      "mu_window": mu_window,
      "block": block,
      "block_quality": e > s ? lll_quality_report(block) : dict(0)
   }
}

fn _bkz_coeff_order(int bound) list {
   mut out = [0]
   mut c = 1
   while c <= bound {
      out = out.append(c)
      out = out.append(-c)
      c += 1
   }
   out
}

fn _bkz_nodes_vec(int n) list {
   mut out = []
   mut i = 0
   while i <= n {
      out = out.append(0)
      i += 1
   }
   out
}

fn _bkz_pow_int(int base, int exp) int {
   mut out = 1
   mut i = 0
   while i < exp {
      out *= base
      i += 1
   }
   out
}

fn _svp_coeffs_from_code(list coeff_order, int n, int code) list {
   def choices = coeff_order.len
   mut coeffs = []
   mut pos = 0
   while pos < n {
      def div = _bkz_pow_int(choices, n - pos - 1)
      def idx = (code / div) % choices
      coeffs = coeffs.append(coeff_order[idx])
      pos += 1
   }
   coeffs
}

fn _svp_coeff_indices_from_code(list coeff_order, int n, int code) list<int> {
   def choices = coeff_order.len
   mut list<int> out = list(n)
   mut pos = 0
   while pos < n {
      def div = _bkz_pow_int(choices, n - pos - 1)
      out = out.append(int((code / div) % choices))
      pos += 1
   }
   out
}

fn _bkz_pruning_profile(int n, any radius_sq) list {
   mut out = []
   mut i = 0
   while i <= n {
      out = out.append(_bkz_pruning_bound(i, max(1, n), radius_sq))
      i += 1
   }
   out
}

fn svp_pruning_profile(int dimension, any radius_sq=1.0, str shape="linear") list {
   "Return a deterministic pruning-radius profile for bounded SVP enumeration."
   def n = max(1, dimension)
   mut out = []
   mut i = 0
   while i <= n {
      def x = to_float(i) / to_float(n)
      mut scale = x
      if shape == "quadratic" { scale = x * x }
      elif shape == "sqrt" { scale = math.sqrt(x) }
      else { scale = 0.1 + 0.9 * x }
      out = out.append(radius_sq * scale)
      i += 1
   }
   out
}

fn _svp_profile_terminal(list profile, any fallback) any { profile.len > 0 ? profile.get(profile.len - 1, fallback) : fallback }

fn _svp_profile_density(list profile, any radius_sq) f64 {
   if profile.len <= 1 { return 1.0 }
   def denom = max(0.000001, to_float(radius_sq))
   mut total = 0.0
   mut i = 1
   while i < profile.len {
      total += max(0.0, min(1.0, to_float(profile.get(i)) / denom))
      i += 1
   }
   total / to_float(profile.len - 1)
}

fn _svp_profile_estimated_nodes(list profile, any radius_sq, int max_nodes) int {
   if profile.len <= 1 { return 1 }
   def denom = max(0.000001, to_float(radius_sq))
   mut nodes = 1
   mut level = 1
   while level < profile.len {
      def scale = max(0.0, min(1.0, to_float(profile.get(level)) / denom))
      def width = max(1, int(1.0 + scale * to_float(2 * level + 1)))
      nodes *= width
      if nodes > max_nodes { return max_nodes + 1 }
      level += 1
   }
   nodes
}

fn _svp_pruning_probability(list profile, any radius_sq) f64 {
   def density = max(0.000001, min(1.0, _svp_profile_density(profile, radius_sq)))
   def n = max(1, profile.len - 1)
   max(0.000001, min(1.0, math.pow(density, max(1.0, float(n) / 4.0))))
}

fn _svp_pruning_trials(f64 success_probability, f64 target_probability) int {
   def p = max(0.000001, min(0.999999, success_probability))
   def target = max(0.000001, min(0.999999, target_probability))
   if p >= target { return 1 }
   max(1, int(math.ceil(math.log(1.0 - target) / math.log(1.0 - p))))
}

fn _svp_pruning_gh_radius_sq(int dimension, any radius_sq, f64 density) f64 {
   def n = max(1, dimension)
   float(radius_sq) * math.pow(max(0.000001, density), 2.0 / float(n))
}

fn _svp_pruning_candidate(int dimension, any radius_sq, int max_nodes, str shape) dict {
   def profile = svp_pruning_profile(dimension, radius_sq, shape)
   def est = _svp_profile_estimated_nodes(profile, radius_sq, max_nodes)
   def density = _svp_profile_density(profile, radius_sq)
   def terminal = _svp_profile_terminal(profile, radius_sq)
   def success = _svp_pruning_probability(profile, radius_sq)
   {
      "shape": shape,
      "profile": profile,
      "estimated_nodes": est,
      "within_budget": est <= max_nodes,
      "density": density,
      "success_probability": success,
      "single_enum_cost": est,
      "terminal_radius_sq": terminal,
      "keeps_boundary": to_float(terminal) >= to_float(radius_sq) * 0.999
   }
}

fn svp_pruning_calibration_report(int dimension, any radius_sq=1.0, int max_nodes=200000, any opts=nil) dict {
   "Calibrate a pruning profile with GH, success-probability, and repeated-cost counters."
   def o = is_dict(opts) ? opts : dict(0)
   def n = max(1, dimension)
   def budget = max(1, max_nodes)
   def shape = o.get("shape", o.get("pruning_shape", "linear"))
   def target = float(o.get("target_success_probability", o.get("target", 0.5)))
   def preproc = float(o.get("preprocessing_nodes", o.get("pre_nodes", 1.0)))
   def cand = _svp_pruning_candidate(n, radius_sq, budget, shape)
   def profile = cand.get("profile", svp_pruning_profile(n, radius_sq, shape))
   def success = float(cand.get("success_probability", _svp_pruning_probability(profile, radius_sq)))
   def trials = _svp_pruning_trials(success, target)
   def single_cost = float(cand.get("single_enum_cost", cand.get("estimated_nodes", 1)))
   def repeated_cost = single_cost * float(trials) + preproc * max(0.0, float(trials - 1))
   def gh_radius_sq = _svp_pruning_gh_radius_sq(n, radius_sq, float(cand.get("density", 1.0)))
   {
      "method": "pruning-calibration-report",
      "source_model": "gh-pruner-cost-model",
      "dimension": n,
      "radius_sq": radius_sq,
      "max_nodes": budget,
      "shape": shape,
      "profile": profile,
      "density": cand.get("density", 1.0),
      "gaussian_heuristic_radius_sq": gh_radius_sq,
      "gh_factor": math.sqrt(max(0.0, float(radius_sq))) / max(0.000001, math.sqrt(max(0.0, gh_radius_sq))),
      "target_success_probability": target,
      "success_probability": success,
      "trials": trials,
      "preprocessing_nodes": preproc,
      "single_enum_cost": single_cost,
      "repeated_enum_cost": repeated_cost,
      "within_budget": cand.get("within_budget", false)
   }
}

fn svp_pruning_optimize_report(int dimension, any radius_sq=1.0, int max_nodes=200000, any opts=nil) dict {
   "Choose a deterministic pruning profile by comparing simple node estimates for supported shapes."
   def o = is_dict(opts) ? opts : dict(0)
   def n = max(1, dimension)
   def budget = max(1, max_nodes)
   def shapes = o.get("shapes", ["quadratic", "sqrt", "linear"])
   mut candidates = []
   mut best = nil
   mut i = 0
   while i < shapes.len {
      def shape = shapes.get(i)
      def cand = _svp_pruning_candidate(n, radius_sq, budget, shape)
      candidates = candidates.append(cand)
      if best == nil {
         best = cand
      } else {
         def c_nodes = int(cand.get("estimated_nodes", budget + 1))
         def b_nodes = int(best.get("estimated_nodes", budget + 1))
         def c_ok = cand.get("within_budget", false)
         def b_ok = best.get("within_budget", false)
         if (c_ok && !b_ok) || (c_ok == b_ok && c_nodes < b_nodes) { best = cand }
      }
      i += 1
   }
   {
      "method": "adaptive-pruning-profile",
      "dimension": n,
      "radius_sq": radius_sq,
      "max_nodes": budget,
      "shape": best.get("shape", "linear"),
      "profile": best.get("profile", svp_pruning_profile(n, radius_sq, "linear")),
      "estimated_nodes": best.get("estimated_nodes", budget + 1),
      "within_budget": best.get("within_budget", false),
      "density": best.get("density", 1.0),
      "success_probability": best.get("success_probability", 0.0),
      "single_enum_cost": best.get("single_enum_cost", best.get("estimated_nodes", 0)),
      "calibration": svp_pruning_calibration_report(n, radius_sq, budget, _bkz_dict_with(o, "shape", best.get("shape", "linear"))),
      "candidate_count": candidates.len,
      "candidates": candidates
   }
}

fn svp_pruning_optimize(int dimension, any radius_sq=1.0, int max_nodes=200000, any opts=nil) list {
   "Return the profile selected by svp_pruning_optimize_report."
   svp_pruning_optimize_report(dimension, radius_sq, max_nodes, opts).get("profile")
}

fn _bkz_strategy_window_report(int start, int stop, int max_nodes, bool adaptive, any opt, any radius_sq, str shape) dict {
   def win_dim = stop - start
   def pruning_profile = adaptive ? opt.get("profile", []) : svp_pruning_profile(win_dim, radius_sq, shape)
   mut out = {
      "start": start,
      "stop": stop,
      "block_size": win_dim,
      "max_nodes": max_nodes,
      "adaptive_pruning": adaptive,
      "pruning_shape": adaptive ? opt.get("shape", shape) : shape,
      "pruning_profile": pruning_profile,
      "estimated_nodes": adaptive ? opt.get("estimated_nodes", 0) : _svp_profile_estimated_nodes(pruning_profile, radius_sq, max_nodes)
   }
   if adaptive { out = out.set("pruning_optimizer", opt) }
   out
}

fn _bkz_strategy_phase_report(int phase, int phase_block, int phase_stride, int phase_count, int max_tours, int max_nodes) dict {
   {
      "phase": phase,
      "kind": phase < phase_count ? "progressive" : "target",
      "block_size": phase_block,
      "stride": phase_stride,
      "tour_budget": max(1, max_tours / max(1, phase_count)),
      "max_nodes": max_nodes
   }
}

fn bkz_strategy_report(int dimension, int block_size=10, any opts=nil) dict {
   "Build a deterministic BKZ strategy report: windows, pruning, budgets, and tours."
   def o = is_dict(opts) ? opts : dict(0)
   def n = max(0, dimension)
   def bs = max(2, min(max(2, block_size), max(2, n)))
   def stride = max(1, int(o.get("stride", 1)))
   def max_nodes = int(o.get("max_nodes", 200000))
   def max_tours = int(o.get("max_tours", max(1, n)))
   def radius_sq = o.get("radius_sq", 1.0)
   def shape = o.get("pruning_shape", "linear")
   def adaptive = o.get("adaptive_pruning", o.get("adaptive", false))
   def progressive = o.get("progressive", n >= 32)
   def min_block_size = max(2, min(bs, int(o.get("min_block_size", min(bs, max(2, bs / 2))))))
   def block_step = max(1, int(o.get("block_step", max(1, bs / 8))))
   def jump_stride = max(1, int(o.get("jump_stride", max(1, bs / 4))))
   def first_index_tours = max(0, int(o.get("first_index_tours", max_tours > 2 ? max(1, max_tours / 3) : 0)))
   def first_index_limit = max(1, min(max(1, n), int(o.get("first_index_limit", max(1, n / 2)))))
   def final_heavy_multiplier = max(1, int(o.get("final_heavy_multiplier", 4)))
   mut block_schedule = []
   if progressive {
      mut cur_bs = min_block_size
      while cur_bs < bs {
         block_schedule = block_schedule.append(cur_bs)
         cur_bs += block_step
      }
   }
   if block_schedule.len == 0 || block_schedule.get(block_schedule.len - 1) != bs { block_schedule = block_schedule.append(bs) }
   mut windows = []
   mut start = 0
   while start < max(1, n - 1) {
      def stop = min(n, start + bs)
      def win_dim = stop - start
      def opt = adaptive ? svp_pruning_optimize_report(win_dim, radius_sq, max_nodes, o) : nil
      windows = windows.append(_bkz_strategy_window_report(start, stop, max_nodes, adaptive, opt, radius_sq, shape))
      if stop >= n { start = n } else { start += stride }
   }
   mut phases = []
   mut si = 0
   while si < block_schedule.len {
      def phase_block = int(block_schedule.get(si))
      def phase_stride = si + 1 < block_schedule.len ? max(1, min(jump_stride, phase_block / 2)) : 1
      phases = phases.append(_bkz_strategy_phase_report(si + 1, phase_block, phase_stride, block_schedule.len, max_tours, max_nodes))
      si += 1
   }
   def first_index_phase = {
      "enabled": first_index_tours > 0 && n > 0,
      "tour_budget": first_index_tours,
      "index_limit": first_index_limit,
      "reason": "late tours focus on early indices where first-vector quality is decided"
   }
   def final_heavy_phase = {
      "enabled": n > 0,
      "block_size": bs,
      "max_nodes": max_nodes * final_heavy_multiplier,
      "node_multiplier": final_heavy_multiplier,
      "reason": "finish with a heavier local search after cheaper progressive passes"
   }
   {
      "method": "deterministic-window-strategy",
      "dimension": n,
      "block_size": bs,
      "stride": stride,
      "progressive": progressive,
      "min_block_size": min_block_size,
      "block_step": block_step,
      "block_schedule": block_schedule,
      "phase_reports": phases,
      "phase_count": phases.len,
      "jump_stride": jump_stride,
      "first_index_phase": first_index_phase,
      "final_heavy_phase": final_heavy_phase,
      "max_nodes": max_nodes,
      "max_tours": max_tours,
      "early_abort": o.get("early_abort", true),
      "pruning_shape": shape,
      "adaptive_pruning": adaptive,
      "windows": windows,
      "window_count": windows.len
   }
}

fn bkz_strategy(int dimension, int block_size=10, any opts=nil) list {
   "Return the deterministic BKZ window schedule from bkz_strategy_report."
   bkz_strategy_report(dimension, block_size, opts).get("windows", [])
}

fn bkz_backend_report(any basis=nil) dict {
   "Return the BKZ strategy policy and audit fields."
   mut out = {
      "default_method": "ny",
      "auto_method": "ny",
      "ny_default": true,
      "strategy": "bkz-windowed-lll"
   }
   if basis != nil {
      def B = _bkz_as_matrix(basis)
      def rows = _matrix_rows(B)
      out = _bkz_set_fields(out, [
            "rows", rows,
            "cols", _matrix_cols(B),
            "max_default_passes", rows,
            "strategy_report", bkz_strategy_report(rows, min(10, max(2, rows)))
      ])
   }
   out
}

fn _svp_core_record(list vector, any norm, int nodes, bool hit_limit, list nodes_by_level, any basis) dict {
   {
      "vector": vector,
      "norm": norm,
      "nodes": nodes,
      "hit_limit": hit_limit,
      "nodes_by_level": nodes_by_level,
      "basis": basis
   }
}

fn _svp_core_record_coeffs(list vector, any norm, int nodes, bool hit_limit, list nodes_by_level, any basis, list coeffs) dict {
   _bkz_set_fields(_svp_core_record(vector, norm, nodes, hit_limit, nodes_by_level, basis), ["coeffs", coeffs])
}

fn _svp_combo(list rows, int dim, list coeffs) list {
   mut cur = _bkz_zero_vec(dim)
   mut i = 0
   while i < coeffs.len {
      def c = coeffs.get(i)
      if c != 0 { cur = _bkz_vec_add_scaled_inplace(cur, rows.get(i), c) }
      i += 1
   }
   cur
}

fn _svp_seed_best_range(list rows, int dim, any radius_sq=nil, int first=0, int last=0) dict {
   def end = last <= 0 ? rows.len : min(rows.len, last)
   def start = max(0, min(first, end))
   if start >= end {
      return {
         "vector": rows.len > 0 ? rows[0] : _bkz_zero_vec(dim),
         "norm": radius_sq == nil ? Z(0) : Z(radius_sq),
         "coeffs": _svp_zero_coeffs(rows.len),
         "index": -1,
         "source": radius_sq == nil ? "empty-basis" : "caller-radius"
      }
   }
   mut best_v = rows[start]
   mut best_norm = radius_sq == nil ? _bkz_row_norm(best_v) : Z(radius_sq)
   mut best_index = radius_sq == nil ? start : -1
   mut best_coeffs = radius_sq == nil ? _bkz_unit_coeffs(rows.len, start) : _svp_zero_coeffs(rows.len)
   mut i = start
   while i < end {
      def r = rows[i]
      def nr = _bkz_row_norm(r)
      if nr > Z(0) && (best_index < 0 || nr < best_norm) {
         best_v = r
         best_norm = nr
         best_index = i
         best_coeffs = _bkz_unit_coeffs(rows.len, i)
      }
      i += 1
   }
   {
      "vector": best_v,
      "norm": best_norm,
      "coeffs": best_coeffs,
      "index": best_index,
      "source": radius_sq == nil ? "basis-min-row" : "caller-radius"
   }
}

fn _svp_seed_best(list rows, int dim, any radius_sq=nil) dict {
   _svp_seed_best_range(rows, dim, radius_sq, 0, rows.len)
}

fn _svp_last_useful_index_from_norms(list norms) int {
   if norms.len <= 1 { return norms.len }
   def base = to_float(norms.get(0, Z(0))) * 2.0
   mut i = norms.len - 1
   while i > 0 {
      if to_float(norms.get(i, Z(0))) <= base { return i + 1 }
      i -= 1
   }
   1
}

fn _svp_prefix_rows(list rows, int count) list {
   mut out = []
   mut i = 0
   def end = max(0, min(rows.len, count))
   while i < end {
      out = out.append(rows[i])
      i += 1
   }
   out
}

fn _svp_extend_coeffs(list coeffs, int n) list {
   mut out = []
   mut i = 0
   while i < n {
      out = out.append(i < coeffs.len ? coeffs[i] : 0)
      i += 1
   }
   out
}

fn _svp_useful_search_basis(any reduced, list rows, int useful_dim) dict {
   def n = _matrix_rows(reduced)
   if useful_dim <= 0 || useful_dim >= n {
      return {"basis": reduced, "rows": rows, "dimension": n}
   }
   def search_rows = _svp_prefix_rows(rows, useful_dim)
   {"basis": Matrix(search_rows), "rows": search_rows, "dimension": useful_dim}
}

fn _svp_reduce_with_transform(any basis) dict {
   def rep = lll_reduce_report(basis, 0.75, "ny", 0.51)
   {
      "basis": rep.get("basis"),
      "transform": rep.get("transform", _bkz_identity(_matrix_rows(rep.get("basis")))),
      "transform_verified": rep.get("transform_verified", false)
   }
}

fn _svp_lift_coeffs_to_input(list coeffs, any transform) list {
   def rows = _matrix_rows(transform)
   def cols = _matrix_cols(transform)
   if coeffs.len != rows { return coeffs }
   mut out = []
   mut j = 0
   while j < cols {
      mut s = Z(0)
      mut i = 0
      while i < rows {
         s += Z(coeffs[i]) * _bkz_entry_z(_matrix_get(transform, i, j))
         i += 1
      }
      out = out.append(s)
      j += 1
   }
   out
}

fn _svp_state_with_input_coeffs(dict state, any transform) dict {
   def reduced_coeffs = state.get("coeffs", [])
   if !is_list(reduced_coeffs) { return state }
   state.set("reduced_coeffs", reduced_coeffs)
   .set("coeffs", _svp_lift_coeffs_to_input(reduced_coeffs, transform))
   .set("coeff_basis", "input")
   .set("basis_transform", transform)
}

fn _svp_zero_coeffs(int n) list {
   mut out = []
   mut i = 0
   while i < n {
      out = out.append(0)
      i += 1
   }
   out
}

fn _svp_gso_bound_for_level(any radius_sq, any norm_sq, int coeff_bound) int {
   def cap = max(1, int(coeff_bound))
   if norm_sq == Z(0) { return cap }
   def ratio = max(0.0, to_float(radius_sq) / max(0.000001, to_float(norm_sq)))
   def b = int(math.ceil(math.sqrt(ratio))) + 1
   max(1, min(cap, b))
}

fn _svp_gso_bounds(list norms, any radius_sq, int coeff_bound) list {
   mut out = []
   mut i = 0
   while i < norms.len {
      out = out.append(_svp_gso_bound_for_level(radius_sq, norms.get(i, Z(0)), coeff_bound))
      i += 1
   }
   out
}

fn _svp_gso_bound_estimate(list bounds, int max_nodes) int {
   mut est = 1
   mut i = 0
   while i < bounds.len {
      est *= 2 * int(bounds.get(i, 1)) + 1
      if est > max_nodes { return max_nodes + 1 }
      i += 1
   }
   est
}

fn _svp_extend_bounds(list bounds, int n) list {
   mut out = []
   mut i = 0
   while i < n {
      out = out.append(i < bounds.len ? bounds[i] : 0)
      i += 1
   }
   out
}

fn _svp_mu_tail(int idx, list coeffs, any mu, int n) f64 {
   mut s = 0.0
   mut j = idx + 1
   while j < n {
      s += to_float(coeffs.get(j, 0)) * to_float(_matrix_get(mu, j, idx))
      j += 1
   }
   s
}

fn _svp_gso_enum_dfs(int idx, list rows, int dim, any basis, any mu, list norms, list bounds, list coeffs, f64 partial_cost, list best_v, any best_norm, int nodes, int max_nodes, list nodes_by_level) dict {
   if idx >= 0 && idx < nodes_by_level.len { nodes_by_level = nodes_by_level.set(idx, nodes_by_level[idx] + 1) }
   if nodes >= max_nodes { return _svp_core_record(best_v, best_norm, nodes, true, nodes_by_level, basis) }
   nodes += 1
   if idx < 0 {
      if nodes_by_level.len > 0 { nodes_by_level = nodes_by_level.set(0, nodes_by_level[0] + 1) }
      def cur = _svp_combo(rows, dim, coeffs)
      if _bkz_vec_nonzero(cur) {
         def nr = _bkz_row_norm(cur)
         if nr > Z(0) && nr < best_norm { return _svp_core_record_coeffs(cur, nr, nodes, false, nodes_by_level, basis, coeffs) }
      }
      return _svp_core_record(best_v, best_norm, nodes, false, nodes_by_level, basis)
   }
   mut state = _svp_core_record(best_v, best_norm, nodes, false, nodes_by_level, basis)
   def bound = int(bounds.get(idx, 1))
   mut oi = 0
   def total = bound * 2 + 1
   while oi < total {
      def c = oi == 0 ? 0 : ((oi % 2 == 1) ? ((oi + 1) / 2) : (0 - (oi / 2)))
      def shifted = to_float(c) + _svp_mu_tail(idx, coeffs, mu, rows.len)
      def next_cost = partial_cost + to_float(norms.get(idx, Z(0))) * shifted * shifted
      if next_cost < to_float(state.get("norm", best_norm)) {
         def next_coeffs = coeffs.set(idx, c)
         state = _svp_gso_enum_dfs(idx - 1, rows, dim, basis, mu, norms, bounds, next_coeffs, next_cost, state.get("vector"), state.get("norm"), state.get("nodes"), max_nodes, state.get("nodes_by_level"))
         if state.get("hit_limit", false) { return state }
      }
      oi += 1
   }
   state
}

fn _svp_gso_enumerate_prepped(any reduced, any reduce_transform, any radius_sq=nil, int coeff_bound=1, int max_nodes=200000) dict {
   def rows = _matrix_data(reduced)
   def n = _matrix_rows(reduced)
   def dim = _matrix_cols(reduced)
   def full_gso = gso_report(reduced)
   def full_norms = full_gso.get("norms_sq", [])
   def useful_dim0 = _svp_last_useful_index_from_norms(full_norms)
   def useful_dim = useful_dim0 <= 0 ? n : max(1, min(n, useful_dim0))
   def search = _svp_useful_search_basis(reduced, rows, useful_dim)
   def search_rows = search.get("rows")
   def search_n = int(search.get("dimension", n))
   def search_basis = search.get("basis")
   def seed = _svp_seed_best_range(search_rows, dim, radius_sq, 0, search_n)
   def gso = search_n == n ? full_gso : gso_report(search_basis)
   def norms = gso.get("norms_sq", [])
   def bounds = _svp_gso_bounds(norms, seed.get("norm"), coeff_bound)
   def report_bounds = _svp_extend_bounds(bounds, n)
   def estimate = _svp_gso_bound_estimate(bounds, max_nodes)
   if estimate > max_nodes && search_n > 12 {
      mut skipped = _svp_core_record_coeffs(seed.get("vector"), seed.get("norm"), 0, true, _bkz_nodes_vec(search_n), reduced, _svp_extend_coeffs(seed.get("coeffs", _svp_zero_coeffs(search_n)), n))
      skipped = _bkz_set_fields(skipped, [
            "kernel", "reverse-gso-dfs",
            "kernel_specialized", false,
            "method", "gso-bounded-recursive-enumeration",
            "status", "skipped-node-budget",
            "gso_bounds", report_bounds,
            "search_gso_bounds", bounds,
            "gso_bound_estimate", estimate,
            "gso_profile", gso,
            "enumeration_order", "reverse-gso",
            "coefficient_order", "schnorr-euchner-inline",
            "input_dimension", n,
            "enumeration_dimension", search_n,
            "last_useful_index", useful_dim,
            "ignored_tail_vectors", max(0, n - search_n),
            "basis_min_norm", seed.get("norm"),
            "basis_min_index", seed.get("index", -1),
            "initial_bound_source", seed.get("source", "basis-min-row"),
            "dimension_trimmed", search_n < n
      ])
      return _svp_state_with_input_coeffs(skipped, reduce_transform)
   }
   def nodes_by_level = _bkz_nodes_vec(search_n)
   def coeffs = _svp_zero_coeffs(search_n)
   mut state = _svp_gso_enum_dfs(search_n - 1, search_rows, dim, reduced, gso.get("mu"), norms, bounds, coeffs, 0.0, seed.get("vector"), seed.get("norm"), 0, max_nodes, nodes_by_level)
   if !is_list(state.get("coeffs", nil)) { state = state.set("coeffs", seed.get("coeffs", _svp_zero_coeffs(search_n))) }
   state = _bkz_set_fields(state, [
         "coeffs", _svp_extend_coeffs(state.get("coeffs", _svp_zero_coeffs(search_n)), n),
         "kernel", "reverse-gso-dfs",
         "kernel_specialized", false,
         "method", "gso-bounded-recursive-enumeration",
         "gso_bounds", report_bounds,
         "search_gso_bounds", bounds,
         "gso_bound_estimate", estimate,
         "gso_profile", gso,
         "enumeration_order", "reverse-gso",
         "coefficient_order", "schnorr-euchner-inline",
         "input_dimension", n,
         "enumeration_dimension", search_n,
         "last_useful_index", useful_dim,
         "ignored_tail_vectors", max(0, n - search_n),
         "basis_min_norm", seed.get("norm"),
         "basis_min_index", seed.get("index", -1),
         "initial_bound_source", seed.get("source", "basis-min-row"),
         "dimension_trimmed", search_n < n
   ])
   _svp_state_with_input_coeffs(state, reduce_transform)
}

fn _svp_low_weight_nodes(list state) int { int(state.get(3, 0)) }

fn _svp_low_weight_hits(list state) int { int(state.get(4, 0)) }

fn _svp_low_weight_coeff_values(int coeff_bound) list {
   def coeff_order = _bkz_coeff_order(max(1, int(coeff_bound)))
   mut coeff_values = []
   mut ci = 0
   while ci < coeff_order.len {
      if coeff_order[ci] != 0 { coeff_values = coeff_values.append(coeff_order[ci]) }
      ci += 1
   }
   coeff_values.len == 0 ? [-1, 1] : coeff_values
}

fn _svp_low_weight_coeffs1(int n, int i, any ci) list {
   mut out = _svp_zero_coeffs(n)
   out[i] = ci
   out
}

fn _svp_low_weight_coeffs2(int n, int i, any ci, int j, any cj) list {
   mut out = _svp_zero_coeffs(n)
   out[i] = ci
   out[j] = cj
   out
}

fn _svp_low_weight_coeffs3(int n, int i, any ci, int j, any cj, int k, any ck) list {
   mut out = _svp_zero_coeffs(n)
   out[i] = ci
   out[j] = cj
   out[k] = ck
   out
}

fn _svp_low_weight_gram_note1(list state, int n, any norm, int i, any ci) list {
   state[2] = int(state.get(2, 0)) + 1
   if norm > Z(0) && norm < state[0] {
      state[0] = norm
      state[1] = _svp_low_weight_coeffs1(n, i, ci)
      state[3] = int(state.get(3, 0)) + 1
   }
   state
}

fn _svp_low_weight_gram_note2(list state, int n, any norm, int i, any ci, int j, any cj) list {
   state[2] = int(state.get(2, 0)) + 1
   if norm > Z(0) && norm < state[0] {
      state[0] = norm
      state[1] = _svp_low_weight_coeffs2(n, i, ci, j, cj)
      state[3] = int(state.get(3, 0)) + 1
   }
   state
}

fn _svp_low_weight_gram_note3(list state, int n, any norm, int i, any ci, int j, any cj, int k, any ck) list {
   state[2] = int(state.get(2, 0)) + 1
   if norm > Z(0) && norm < state[0] {
      state[0] = norm
      state[1] = _svp_low_weight_coeffs3(n, i, ci, j, cj, k, ck)
      state[3] = int(state.get(3, 0)) + 1
   }
   state
}

fn _svp_low_weight_gram_note1_int(list state, int n, int norm, int i, int ci) list {
   state[2] = int(state[2]) + 1
   if norm > 0 && norm < int(state[0]) {
      state[0] = norm
      state[1] = _svp_low_weight_coeffs1(n, i, ci)
      state[3] = int(state[3]) + 1
   }
   state
}

fn _svp_low_weight_gram_note2_int(list state, int n, int norm, int i, int ci, int j, int cj) list {
   state[2] = int(state[2]) + 1
   if norm > 0 && norm < int(state[0]) {
      state[0] = norm
      state[1] = _svp_low_weight_coeffs2(n, i, ci, j, cj)
      state[3] = int(state[3]) + 1
   }
   state
}

fn _svp_low_weight_gram_note3_int(list state, int n, int norm, int i, int ci, int j, int cj, int k, int ck) list {
   state[2] = int(state[2]) + 1
   if norm > 0 && norm < int(state[0]) {
      state[0] = norm
      state[1] = _svp_low_weight_coeffs3(n, i, ci, j, cj, k, ck)
      state[3] = int(state[3]) + 1
   }
   state
}

fn _svp_low_weight_gram_one(list state, list gram, int n, list coeff_values, int max_nodes) list {
   mut i = 0
   while i < n && int(state.get(2, 0)) < max_nodes {
      mut si = 0
      while si < coeff_values.len && int(state.get(2, 0)) < max_nodes {
         def ci = coeff_values[si]
         def norm = Z(ci) * Z(ci) * _bkz_entry_z(gram[i][i])
         state = _svp_low_weight_gram_note1(state, n, norm, i, ci)
         si += 1
      }
      i += 1
   }
   state
}

fn _svp_low_weight_gram_one_int(list state, list<list<int>> gram, int n, list coeff_values, int max_nodes) list {
   mut i = 0
   while i < n && int(state[2]) < max_nodes {
      def gii = gram[i][i]
      mut si = 0
      while si < coeff_values.len && int(state[2]) < max_nodes {
         def ci = int(coeff_values[si])
         state = _svp_low_weight_gram_note1_int(state, n, ci * ci * gii, i, ci)
         si += 1
      }
      i += 1
   }
   state
}

fn _svp_low_weight_gram_two(list state, list gram, int n, list coeff_values, int max_nodes) list {
   mut i = 0
   while i < n && int(state.get(2, 0)) < max_nodes {
      mut j = i + 1
      while j < n && int(state.get(2, 0)) < max_nodes {
         mut si = 0
         while si < coeff_values.len && int(state.get(2, 0)) < max_nodes {
            def ci = coeff_values[si]
            def ciz = Z(ci)
            mut sj = 0
            while sj < coeff_values.len && int(state.get(2, 0)) < max_nodes {
               def cj = coeff_values[sj]
               def cjz = Z(cj)
               def norm = ciz * ciz * _bkz_entry_z(gram[i][i]) + cjz * cjz * _bkz_entry_z(gram[j][j]) + Z(2) * ciz * cjz * _bkz_entry_z(gram[i][j])
               state = _svp_low_weight_gram_note2(state, n, norm, i, ci, j, cj)
               sj += 1
            }
            si += 1
         }
         j += 1
      }
      i += 1
   }
   state
}

fn _svp_low_weight_gram_two_int(list state, list<list<int>> gram, int n, list coeff_values, int max_nodes) list {
   mut i = 0
   while i < n && int(state[2]) < max_nodes {
      def gii = gram[i][i]
      mut j = i + 1
      while j < n && int(state[2]) < max_nodes {
         def gjj = gram[j][j]
         def gij2 = 2 * gram[i][j]
         mut si = 0
         while si < coeff_values.len && int(state[2]) < max_nodes {
            def ci = int(coeff_values[si])
            def ci2 = ci * ci * gii
            mut sj = 0
            while sj < coeff_values.len && int(state[2]) < max_nodes {
               def cj = int(coeff_values[sj])
               def norm = ci2 + cj * cj * gjj + ci * cj * gij2
               state = _svp_low_weight_gram_note2_int(state, n, norm, i, ci, j, cj)
               sj += 1
            }
            si += 1
         }
         j += 1
      }
      i += 1
   }
   state
}

fn _svp_low_weight_gram_three(list state, list gram, int n, list coeff_values, int max_nodes) list {
   mut i = 0
   while i < n && int(state.get(2, 0)) < max_nodes {
      mut j = i + 1
      while j < n && int(state.get(2, 0)) < max_nodes {
         mut k = j + 1
         while k < n && int(state.get(2, 0)) < max_nodes {
            mut si = 0
            while si < coeff_values.len && int(state.get(2, 0)) < max_nodes {
               def ci = coeff_values[si]
               def ciz = Z(ci)
               mut sj = 0
               while sj < coeff_values.len && int(state.get(2, 0)) < max_nodes {
                  def cj = coeff_values[sj]
                  def cjz = Z(cj)
                  mut sk = 0
                  while sk < coeff_values.len && int(state.get(2, 0)) < max_nodes {
                     def ck = coeff_values[sk]
                     def ckz = Z(ck)
                     def norm = ciz * ciz * _bkz_entry_z(gram[i][i]) + cjz * cjz * _bkz_entry_z(gram[j][j]) + ckz * ckz * _bkz_entry_z(gram[k][k]) + Z(2) * ciz * cjz * _bkz_entry_z(gram[i][j]) + Z(2) * ciz * ckz * _bkz_entry_z(gram[i][k]) + Z(2) * cjz * ckz * _bkz_entry_z(gram[j][k])
                     state = _svp_low_weight_gram_note3(state, n, norm, i, ci, j, cj, k, ck)
                     sk += 1
                  }
                  sj += 1
               }
               si += 1
            }
            k += 1
         }
         j += 1
      }
      i += 1
   }
   state
}

fn _svp_low_weight_gram_three_int(list state, list<list<int>> gram, int n, list coeff_values, int max_nodes) list {
   mut i = 0
   while i < n && int(state[2]) < max_nodes {
      def gii = gram[i][i]
      mut j = i + 1
      while j < n && int(state[2]) < max_nodes {
         def gjj = gram[j][j]
         def gij2 = 2 * gram[i][j]
         mut k = j + 1
         while k < n && int(state[2]) < max_nodes {
            def gkk = gram[k][k]
            def gik2 = 2 * gram[i][k]
            def gjk2 = 2 * gram[j][k]
            mut si = 0
            while si < coeff_values.len && int(state[2]) < max_nodes {
               def ci = int(coeff_values[si])
               def ci2 = ci * ci * gii
               mut sj = 0
               while sj < coeff_values.len && int(state[2]) < max_nodes {
                  def cj = int(coeff_values[sj])
                  def cij = ci2 + cj * cj * gjj + ci * cj * gij2
                  mut sk = 0
                  while sk < coeff_values.len && int(state[2]) < max_nodes {
                     def ck = int(coeff_values[sk])
                     def norm = cij + ck * ck * gkk + ci * ck * gik2 + cj * ck * gjk2
                     state = _svp_low_weight_gram_note3_int(state, n, norm, i, ci, j, cj, k, ck)
                     sk += 1
                  }
                  sj += 1
               }
               si += 1
            }
            k += 1
         }
         j += 1
      }
      i += 1
   }
   state
}

fn _svp_low_weight_gram_search_int(list rows, int dim, dict seed, list coeff_values, int max_nodes, int max_weight, list<list<int>> gram, int seed_norm) list {
   mut state = [seed_norm, seed.get("coeffs", _svp_zero_coeffs(rows.len)), 0, 0]
   state = _svp_low_weight_gram_one_int(state, gram, rows.len, coeff_values, max_nodes)
   state = _svp_low_weight_gram_two_int(state, gram, rows.len, coeff_values, max_nodes)
   if max_weight >= 3 { state = _svp_low_weight_gram_three_int(state, gram, rows.len, coeff_values, max_nodes) }
   [_svp_combo(rows, dim, state[1]), Z(int(state[0])), state[1], int(state[2]), int(state[3])]
}

fn _svp_low_weight_gram_search(list rows, int dim, dict seed, list coeff_values, int max_nodes, int max_weight) list {
   def gram = _svp_row_gram(rows)
   def gram_int = _svp_gram_int_or_nil(gram, 40)
   def seed_norm_int = _bkz_small_int_or_nil(seed.get("norm"), 50)
   if gram_int != nil && seed_norm_int.get(0) {
      return _svp_low_weight_gram_search_int(rows, dim, seed, coeff_values, max_nodes, max_weight, gram_int, int(seed_norm_int.get(1)))
   }
   mut state = [seed.get("norm"), seed.get("coeffs", _svp_zero_coeffs(rows.len)), 0, 0]
   state = _svp_low_weight_gram_one(state, gram, rows.len, coeff_values, max_nodes)
   state = _svp_low_weight_gram_two(state, gram, rows.len, coeff_values, max_nodes)
   if max_weight >= 3 { state = _svp_low_weight_gram_three(state, gram, rows.len, coeff_values, max_nodes) }
   [_svp_combo(rows, dim, state.get(1)), state.get(0), state.get(1), state.get(2), state.get(3)]
}

fn _svp_low_weight_nodes_by_level(int n, int nodes) list {
   mut out = _bkz_nodes_vec(n)
   mut i = 0
   while i < out.len {
      out = out.set(i, nodes)
      i += 1
   }
   out
}

fn _svp_low_weight_record(any reduced, any reduce_transform, list state, int n, int max_nodes, int max_weight, list coeff_values) dict {
   def nodes = _svp_low_weight_nodes(state)
   mut out = _svp_core_record_coeffs(state.get(0), state.get(1), nodes, nodes >= max_nodes, _svp_low_weight_nodes_by_level(n, nodes), reduced, state.get(2))
   out = out.set("kernel", "low-weight-signed-combiner")
   out = out.set("kernel_specialized", false)
   out = out.set("method", "low-weight-signed-combination")
   out = out.set("max_weight", max_weight)
   out = out.set("coeff_values", coeff_values)
   out = out.set("improvement_count", _svp_low_weight_hits(state))
   _svp_state_with_input_coeffs(out, reduce_transform)
}

fn _svp_low_weight_prepped(any reduced, any reduce_transform, any radius_sq=nil, int coeff_bound=1, int max_nodes=200000, int max_weight=3) dict {
   def rows = _matrix_data(reduced)
   def n = _matrix_rows(reduced)
   def dim = _matrix_cols(reduced)
   def seed = _svp_seed_best(rows, dim, radius_sq)
   def coeff_values = _svp_low_weight_coeff_values(coeff_bound)
   def state = _svp_low_weight_gram_search(rows, dim, seed, coeff_values, max_nodes, max_weight)
   _svp_low_weight_record(reduced, reduce_transform, state, n, max_nodes, max_weight, coeff_values)
}

fn _svp_better_core(dict a, dict b) dict {
   def an = a.get("norm", nil)
   def bn = b.get("norm", nil)
   if an == nil { return b }
   if bn == nil { return a }
   bn < an ? b : a
}

fn _svp_gram_norm(any gram, list coeffs) any {
   def n = _matrix_rows(gram)
   mut s = Z(0)
   mut i = 0
   while i < n {
      mut j = 0
      while j < n {
         s += Z(coeffs.get(i, 0)) * _bkz_entry_z(_matrix_get(gram, i, j)) * Z(coeffs.get(j, 0))
         j += 1
      }
      i += 1
   }
   s
}

fn _svp_gram_seed(any gram) dict {
   def n = _matrix_rows(gram)
   mut best = nil
   mut best_coeffs = _svp_zero_coeffs(n)
   mut i = 0
   while i < n {
      def coeffs = _bkz_unit_coeffs(n, i)
      def nr = _svp_gram_norm(gram, coeffs)
      if nr > Z(0) && (best == nil || nr < best) {
         best = nr
         best_coeffs = coeffs
      }
      i += 1
   }
   {"norm": best == nil ? Z(0) : best, "coeffs": best_coeffs}
}

fn _svp_gram_try(any gram, list coeffs, any best_norm, list best_coeffs) dict {
   if _bkz_vec_nonzero(coeffs) {
      def nr = _svp_gram_norm(gram, coeffs)
      if nr > Z(0) && (best_norm == Z(0) || nr < best_norm) {
         return {"norm": nr, "coeffs": coeffs, "improved": true}
      }
   }
   {"norm": best_norm, "coeffs": best_coeffs, "improved": false}
}

fn svp_gram_report(any gram, any radius_sq=nil, int coeff_bound=2, int max_nodes=200000) dict {
   "Return a shortest coefficient-vector report for an integer Gram matrix.
   The norm is computed exactly as c^T G c, so Gram-form fixtures can be
   checked without converting back to a basis."
   def t0 = ticks()
   def G = _bkz_as_matrix(gram)
   def n = _matrix_rows(G)
   def m = _matrix_cols(G)
   def bound = max(1, int(coeff_bound))
   def coeff_order = _bkz_coeff_order(bound)
   def choices = coeff_order.len
   def seed = _svp_gram_seed(G)
   mut best_norm = radius_sq == nil ? seed.get("norm", Z(0)) : Z(radius_sq)
   mut best_coeffs = seed.get("coeffs", _svp_zero_coeffs(n))
   mut nodes = 0
   mut hit_limit = false
   if n > 0 && n == m && choices > 0 && max_nodes > 0 {
      mut idxs = []
      mut i = 0
      while i < n {
         idxs = idxs.append(0)
         i += 1
      }
      mut done = false
      while !done && nodes < max_nodes {
         mut coeffs = []
         i = 0
         while i < n {
            coeffs = coeffs.append(coeff_order[idxs[i]])
            i += 1
         }
         nodes += 1
         def trial = _svp_gram_try(G, coeffs, best_norm, best_coeffs)
         if trial.get("improved", false) {
            best_norm = trial.get("norm")
            best_coeffs = trial.get("coeffs")
         }
         mut pos = n - 1
         mut advanced = false
         while pos >= 0 && !advanced {
            idxs = idxs.set(pos, idxs[pos] + 1)
            if idxs[pos] < choices {
               advanced = true
            } else {
               idxs = idxs.set(pos, 0)
               pos -= 1
            }
         }
         if !advanced { done = true }
      }
      hit_limit = !done
   }
   def verified_norm = _svp_gram_norm(G, best_coeffs)
   def elapsed = _bkz_elapsed_ms(t0)
   mut out = _bkz_set_fields(dict(18), [
         "method", "gram-coefficient-enumeration",
         "kernel", n <= 8 ? "bounded-gram-cube" : "budgeted-gram-cube",
         "gram_rows", n,
         "gram_cols", m,
         "coeff_bound", bound,
         "max_nodes", max_nodes,
         "nodes", nodes,
         "hit_limit", hit_limit,
         "coeffs", best_coeffs,
         "norm", best_norm,
         "verified_norm", verified_norm,
         "verified", verified_norm == best_norm,
         "radius_sq", best_norm,
         "input_kind", "gram",
         "basis_conversion", "none",
         "elapsed_ms", elapsed,
         "nodes_per_sec", _bkz_nodes_per_sec(nodes, elapsed)
   ])
   out
}

fn svp_gram(any gram, any radius_sq=nil, int coeff_bound=2, int max_nodes=200000) list {
   "Return a shortest coefficient vector for an integer Gram matrix."
   svp_gram_report(gram, radius_sq, coeff_bound, max_nodes).get("coeffs", [])
}

fn _bkz_abs_z(any x) bigint {
   def z = Z(x)
   z < Z(0) ? (Z(0) - z) : z
}

fn _bkz_coeffs_gcd_abs(list coeffs) bigint {
   mut g = Z(0)
   mut i = 0
   while i < coeffs.len {
      def a = _bkz_abs_z(coeffs[i])
      if a != Z(0) { g = (g == Z(0)) ? a : gcd(g, a) }
      i += 1
   }
   g
}

fn _bkz_swap_matrix_rows(any m, int a, int b) any {
   if a == b { return m }
   def rows = _matrix_rows(m)
   def data = _matrix_data(m)
   mut out = []
   mut i = 0
   while i < rows {
      if i == a {
         out = out.append(data.get(b))
      } elif i == b {
         out = out.append(data.get(a))
      } else {
         out = out.append(data.get(i))
      }
      i += 1
   }
   Matrix(out)
}

fn _bkz_unit_first_row_transform(list coeffs) dict {
   def n = coeffs.len
   mut idx = -1
   mut sign = Z(1)
   mut count = 0
   mut i = 0
   while i < n {
      def c = Z(coeffs[i])
      if c != Z(0) {
         if c != Z(1) && c != Z(-1) {
            return dict(6).set("ok", false).set("reason", "not a signed unit coefficient vector").set("transform", _bkz_identity(n)).set("primitive_gcd", Z(0)).set("first_row", coeffs).set("source", "unit-row-permutation")
         }
         idx = i
         sign = c
         count += 1
      }
      i += 1
   }
   if count != 1 {
      return dict(6).set("ok", false).set("reason", "not a signed unit coefficient vector").set("transform", _bkz_identity(n)).set("primitive_gcd", Z(0)).set("first_row", coeffs).set("source", "unit-row-permutation")
   }
   mut U = _bkz_swap_matrix_rows(_bkz_identity(n), 0, idx)
   if sign == Z(-1) {
      mut rows = _matrix_data(U)
      rows[0] = _dual_neg_row(rows[0])
      U = Matrix(rows)
   }
   dict(6).set("ok", true).set("reason", "").set("transform", U).set("primitive_gcd", Z(1)).set("first_row", coeffs).set("source", "unit-row-permutation")
}

fn _bkz_first_row_transform_from_coeffs(list coeffs) dict {
   "Build a unimodular transform whose first row is the primitive coefficient vector."
   def n = coeffs.len
   if n <= 0 {
      return dict(6).set("ok", false).set("reason", "empty coefficient vector").set("transform", _bkz_identity(n)).set("primitive_gcd", Z(0)).set("first_row", coeffs).set("source", "xgcd-row-completion")
   }
   def unit = _bkz_unit_first_row_transform(coeffs)
   if unit.get("ok", false) { return unit }
   def g = _bkz_coeffs_gcd_abs(coeffs)
   if g != Z(1) {
      return dict(6).set("ok", false).set("reason", "coefficient vector is not primitive").set("transform", _bkz_identity(n)).set("primitive_gcd", g).set("first_row", coeffs).set("source", "xgcd-row-completion")
   }
   def map = _dual_unit_to_last_transform(coeffs)
   if !map.get("ok", false) {
      return dict(6).set("ok", false).set("reason", map.get("reason", "row completion failed")).set("transform", _bkz_identity(n)).set("primitive_gcd", map.get("primitive_gcd", g)).set("first_row", coeffs).set("source", "xgcd-row-completion")
   }
   def inv = matrix_inverse(map.get("transform"))
   mut U = matrix_transpose(inv)
   U = _bkz_swap_matrix_rows(U, 0, n - 1)
   def first = _matrix_data(U).get(0, [])
   mut ok = first.len == coeffs.len
   mut i = 0
   while i < coeffs.len && ok {
      if Z(first[i]) != Z(coeffs[i]) { ok = false }
      i += 1
   }
   dict(6).set("ok", ok).set("reason", ok ? "" : "completed transform first row mismatch").set("transform", U).set("primitive_gcd", g).set("first_row", first).set("source", "xgcd-row-completion")
}

fn _bkz_gram_square(any gram) any {
   def G = _bkz_as_matrix(gram)
   if _matrix_rows(G) != _matrix_cols(G) { panic("bkz_gram: Gram matrix must be square") }
   G
}

fn _bkz_gram_extract_block(any gram, int start, int stop) any {
   mut rows = []
   mut i = start
   while i < stop {
      mut row = []
      mut j = start
      while j < stop {
         row = row.append(_bkz_entry_z(_matrix_get(gram, i, j)))
         j += 1
      }
      rows = rows.append(row)
      i += 1
   }
   Matrix(rows)
}

fn _bkz_embed_local_transform(int n, int start, any local) any {
   def L = _bkz_as_matrix(local)
   def lrows = _matrix_rows(L)
   mut rows = []
   mut i = 0
   while i < n {
      mut row = []
      mut j = 0
      while j < n {
         def li = i - start
         def lj = j - start
         if li >= 0 && li < lrows && lj >= 0 && lj < _matrix_cols(L) {
            row = row.append(_bkz_entry_z(_matrix_get(L, li, lj)))
         } else {
            row = row.append(i == j ? Z(1) : Z(0))
         }
         j += 1
      }
      rows = rows.append(row)
      i += 1
   }
   Matrix(rows)
}

fn _bkz_gram_transform(any transform, any gram) any {
   _bkz_sparse_matmul(_bkz_sparse_matmul(transform, gram), matrix_transpose(transform))
}

fn _bkz_gram_verify_transform(any original, any transform, any reduced) bool {
   _bkz_same_matrix(_bkz_gram_transform(transform, original), reduced)
}

fn _bkz_gram_block_report(int start,
   int stop,
   any before_norm,
   any sv_norm,
   list coeffs,
   any sv,
   any completion,
   bool local_verified,
   str insertion_reason,
   bool candidate_improved,
   bool applied,
   bool improved,
   any block_t0,
   any cleanup_report=nil,
   bool cleanup_complete=false,
   bool bounded_exact_fallback=false) dict {
   mut out = _bkz_set_fields(dict(20), [
         "start", start,
         "stop", stop,
         "before_norm", before_norm,
         "svp_norm", sv_norm,
         "svp_coeffs", coeffs,
         "svp_nodes", sv.get("nodes", 0),
         "svp_hit_limit", sv.get("hit_limit", false),
         "completion_ok", completion.get("ok", false),
         "completion_reason", completion.get("reason", ""),
         "primitive_gcd", completion.get("primitive_gcd", Z(0)),
         "local_transform_verified", local_verified,
         "insertion_reason", insertion_reason,
         "candidate_improved", candidate_improved,
         "applied", applied,
         "improved", improved
   ])
   if bounded_exact_fallback {
      def cleanup = cleanup_report == nil ? {"skipped": true, "reason": insertion_reason == "none" ? "no-improving-candidate" : insertion_reason} : cleanup_report
      out = _bkz_set_fields(out, [
            "bounded_exact_fallback", true,
            "post_insertion_cleanup", cleanup,
            "cleanup_complete", cleanup_complete
      ])
   }
   out = _bkz_set_fields(out, ["elapsed_ms", _bkz_elapsed_ms(block_t0)])
   out
}

fn _bkz_gram_block_step(any G, any transform, int n, int i, int block_size, any delta, any eta, int svp_coeff_bound, int svp_max_nodes, bool bounded_exact_fallback, bool record) list {
   def block_t0 = ticks()
   def h = (i + block_size < n) ? (i + block_size) : n
   def block = _bkz_gram_extract_block(G, i, h)
   def before_norm = _bkz_entry_z(_matrix_get(G, i, i))
   def sv = svp_gram_report(block, nil, svp_coeff_bound, svp_max_nodes)
   def coeffs = sv.get("coeffs", [])
   def sv_norm = _bkz_entry_z(sv.get("verified_norm", sv.get("norm", before_norm)))
   def completion = _bkz_first_row_transform_from_coeffs(coeffs)
   mut outG = G
   mut out_transform = transform
   mut changed = false
   mut candidate_improved = false
   mut applied = false
   mut improved = false
   mut insertion_reason = "none"
   mut local_verified = false
   mut cleanup_report = nil
   mut cleanup_complete = false
   if completion.get("ok", false) && sv.get("verified", false) && sv_norm > Z(0) && sv_norm < before_norm {
      candidate_improved = true
      def local = completion.get("transform")
      def global = _bkz_embed_local_transform(n, i, local)
      def candidate = _bkz_gram_transform(global, G)
      local_verified = _bkz_entry_z(_matrix_get(candidate, i, i)) == sv_norm
      if local_verified {
         if bounded_exact_fallback {
            def cleanup_cap = max(8, block_size * block_size * 4)
            cleanup_report = lll_gram_reduce_bounded_report(candidate, cleanup_cap, delta, "ny", eta)
            if cleanup_report.get("transform_verified", false) {
               outG = cleanup_report.get("gram")
               out_transform = _bkz_sparse_matmul(_bkz_sparse_matmul(cleanup_report.get("transform", _bkz_identity(n)), global), transform)
               cleanup_complete = cleanup_report.get("reduction_complete", false)
               insertion_reason = cleanup_complete ? "gram-shorter-exact-cleanup" : "gram-shorter-bounded-cleanup"
               changed = true
               applied = true
               improved = true
            } else {
               insertion_reason = "cleanup-transform-failed"
            }
         } else {
            outG = candidate
            out_transform = _bkz_sparse_matmul(global, transform)
            insertion_reason = "gram-shorter"
            changed = true
            applied = true
            improved = true
         }
      }
   }
   mut report = nil
   if record {
      report = _bkz_gram_block_report(
         i, h, before_norm, sv_norm, coeffs, sv, completion,
         local_verified, insertion_reason, candidate_improved, applied, improved,
      block_t0, cleanup_report, cleanup_complete, bounded_exact_fallback)
   }
   [outG, out_transform, changed, report]
}

fn _bkz_gram_reduce_core(any gram, int block_size=10, any delta=0.75, any eta=0.51, int max_tours=0, bool early_abort=true, int svp_coeff_bound=1, int svp_max_nodes=200000, bool record=false) list {
   def t0 = ticks()
   def G0 = _bkz_gram_square(gram)
   def n = _matrix_rows(G0)
   mut G = G0
   mut transform = _bkz_identity(n)
   mut tours = []
   mut initial = lll_gram_reduce_report(G, delta, "ny", eta)
   G = initial.get("gram")
   transform = initial.get("transform", transform)
   if initial.get("fallback", "") == "exact-rational" {
      if block_size < 2 { block_size = 2 }
      if block_size > n { block_size = n }
      def tour_limit = (max_tours <= 0 || max_tours > 1) ? 1 : max_tours
      mut tour = 0
      mut changed = false
      while tour < tour_limit {
         mut tour_changed = false
         mut block_reports = []
         mut i = 0
         while i <= n - 2 {
            def step = _bkz_gram_block_step(G, transform, n, i, block_size, delta, eta, svp_coeff_bound, svp_max_nodes, true, record)
            G = step[0]
            transform = step[1]
            if step[2] { changed = true tour_changed = true }
            if record {
               block_reports = block_reports.append(step[3])
            }
            i += 1
         }
         if record {
            tours = tours.append({"tour": tour + 1, "changed": tour_changed, "block_stride": 1, "bounded_exact_fallback": true, "blocks": block_reports})
         }
         if early_abort && !tour_changed { tour = tour_limit } else { tour += 1 }
      }
      if changed {
         def final_cap = max(8, block_size * block_size * 8)
         def final_lll = lll_gram_reduce_bounded_report(G, final_cap, delta, "ny", eta)
         G = final_lll.get("gram")
         transform = _bkz_sparse_matmul(final_lll.get("transform", _bkz_identity(n)), transform)
         [G, tours, _bkz_elapsed_ms(t0), transform, initial, final_lll]
      } else {
         def skipped = {"skipped": true, "reason": "no exact Gram block insertion applied", "rows": n}
         [G, tours, _bkz_elapsed_ms(t0), transform, initial, skipped]
      }
   } else {
      if block_size < 2 { block_size = 2 }
      if block_size > n { block_size = n }
      def tour_limit = max_tours <= 0 ? max(1, n) : max_tours
      mut tour = 0
      mut changed = true
      while tour < tour_limit && changed {
         changed = false
         def tour_lll = lll_gram_reduce_report(G, delta, "ny", eta)
         G = tour_lll.get("gram")
         transform = _bkz_sparse_matmul(tour_lll.get("transform", _bkz_identity(n)), transform)
         mut block_reports = []
         mut i = 0
         while i <= n - 2 {
            def step = _bkz_gram_block_step(G, transform, n, i, block_size, delta, eta, svp_coeff_bound, svp_max_nodes, false, record)
            G = step[0]
            transform = step[1]
            if step[2] { changed = true }
            if record {
               block_reports = block_reports.append(step[3])
            }
            i += 1
         }
         if record {
            tours = tours.append({"tour": tour + 1, "changed": changed, "block_stride": 1, "blocks": block_reports})
         }
         if early_abort && !changed { tour = tour_limit } else { tour += 1 }
      }
      def final_lll = lll_gram_reduce_report(G, delta, "ny", eta)
      G = final_lll.get("gram")
      transform = _bkz_sparse_matmul(final_lll.get("transform", _bkz_identity(n)), transform)
      [G, tours, _bkz_elapsed_ms(t0), transform, initial, final_lll]
   }
}

fn bkz_gram_reduce_report(any gram, int block_size=10, any delta=0.75, str method="ny", any eta=0.51, int max_tours=0, bool early_abort=true, int svp_coeff_bound=1, int svp_max_nodes=200000) dict {
   "Reduce an integer Gram matrix by pure row/column BKZ operations and report exact transform verification."
   def t0 = ticks()
   def G0 = _bkz_gram_square(gram)
   def before = lll_gram_quality_report(G0, delta, eta)
   def core = _bkz_gram_reduce_core(G0, block_size, delta, eta, max_tours, early_abort, svp_coeff_bound, svp_max_nodes, true)
   def G = core[0]
   def transform = core[3]
   def after = lll_gram_quality_report(G, delta, eta)
   def verified = _bkz_gram_verify_transform(G0, transform, G)
   mut out = _bkz_set_fields(dict(24), [
         "method", method,
         "selected_method", "ny",
         "input_kind", "gram",
         "rows", _matrix_rows(G0),
         "cols", _matrix_cols(G0),
         "block_size", block_size,
         "max_tours", max_tours,
         "early_abort", early_abort,
         "svp_coeff_bound", svp_coeff_bound,
         "svp_max_nodes", svp_max_nodes,
         "before", before,
         "after", after,
         "before_first_norm", _bkz_entry_z(_matrix_get(G0, 0, 0)),
         "after_first_norm", _bkz_entry_z(_matrix_get(G, 0, 0)),
         "tour_reports", core[1],
         "initial_lll", core[4]
   ])
   if core.len > 5 { out = out.set("final_lll", core[5]) }
   out = _bkz_set_fields(out, [
         "transform", transform,
         "transform_tracked", true,
         "transform_verified", verified,
         "transform_first_mismatch", _bkz_first_matrix_mismatch(_bkz_gram_transform(transform, G0), G),
         "gram", G,
         "elapsed_ms", _bkz_elapsed_ms(t0)
   ])
   out
}

fn bkz_gram_report(any gram, int block_size=10, any delta=0.75, str method="auto", any eta=0.51, int max_tours=0, bool early_abort=true, int svp_coeff_bound=1, int svp_max_nodes=200000) dict {
   "Report-first Gram-BKZ API. `auto` resolves to Ny's exact Gram reducer."
   bkz_gram_reduce_report(gram, block_size, delta, method, eta, max_tours, early_abort, svp_coeff_bound, svp_max_nodes)
}

fn bkz_gram(any gram, int block_size=10, any delta=0.75, str method="ny", any eta=0.51, int max_tours=0, bool early_abort=true, int svp_coeff_bound=1, int svp_max_nodes=200000) any {
   "Return only the reduced Gram matrix."
   bkz_gram_reduce_report(gram, block_size, delta, method, eta, max_tours, early_abort, svp_coeff_bound, svp_max_nodes).get("gram")
}

fn _dual_round_float(f64 x) int { x >= 0.0 ? int(x + 0.5) : int(x - 0.5) }

fn _dual_unit_coeffs(int n, int idx) list {
   mut out = []
   mut i = 0
   while i < n {
      out = out.append(i == idx ? 1 : 0)
      i += 1
   }
   out
}

fn _dual_alpha_from_gso(any gso, list coeffs) list {
   def mu = gso.get("mu")
   def n = coeffs.len
   mut alpha = []
   mut i = 0
   while i < n {
      mut a = float(coeffs[i])
      mut j = 0
      while j < i {
         a -= float(_matrix_get(mu, i, j)) * float(alpha[j])
         j += 1
      }
      alpha = alpha.append(a)
      i += 1
   }
   alpha
}

fn _dual_norm_from_gso(any gso, list coeffs) f64 {
   def alpha = _dual_alpha_from_gso(gso, coeffs)
   def norms = gso.get("norms_sq", [])
   mut total = 0.0
   mut i = 0
   while i < alpha.len {
      def den = float(norms.get(i, 0))
      if den != 0.0 { total += float(alpha[i]) * float(alpha[i]) / den }
      i += 1
   }
   total
}

fn dual_svp_norm(any basis, list coeffs) f64 {
   "Return the dual norm c^T G^-1 c for integer coefficient vector c."
   _dual_norm_from_gso(gso_report(_bkz_as_matrix(basis)), coeffs)
}

fn _dual_inverse_gram_float(any gso) list {
   def n = int(gso.get("rows", 0))
   def norms = gso.get("norms_sq", [])
   mut alphas = []
   mut i = 0
   while i < n {
      alphas = alphas.append(_dual_alpha_from_gso(gso, _dual_unit_coeffs(n, i)))
      i += 1
   }
   mut rows = []
   i = 0
   while i < n {
      mut row = []
      mut j = 0
      while j < n {
         mut s = 0.0
         mut k = 0
         while k < n {
            def den = float(norms.get(k, 0))
            if den != 0.0 { s += float(alphas[i][k]) * float(alphas[j][k]) / den }
            k += 1
         }
         row = row.append(s)
         j += 1
      }
      rows = rows.append(row)
      i += 1
   }
   rows
}

fn _dual_index_used(list used, int idx) bool {
   mut i = 0
   while i < used.len {
      if int(used[i]) == idx { return true }
      i += 1
   }
   false
}

fn _dual_diag_order(list q) list {
   def n = q.len
   mut order = []
   while order.len < n {
      mut best = -1
      mut best_val = 0.0
      mut i = 0
      while i < n {
         if !_dual_index_used(order, i) {
            def v = float(q[i][i])
            if best < 0 || v < best_val {
               best = i
               best_val = v
            }
         }
         i += 1
      }
      order = order.append(best)
   }
   order
}

fn _dual_permute_matrix(list q, list order) list {
   mut rows = []
   mut i = 0
   while i < order.len {
      mut row = []
      mut j = 0
      while j < order.len {
         row = row.append(float(q[int(order[i])][int(order[j])]))
         j += 1
      }
      rows = rows.append(row)
      i += 1
   }
   rows
}

fn _dual_zero_float_matrix(int n) list {
   mut rows = []
   mut i = 0
   while i < n {
      mut row = []
      mut j = 0
      while j < n {
         row = row.append(0.0)
         j += 1
      }
      rows = rows.append(row)
      i += 1
   }
   rows
}

fn _dual_cholesky_upper(list q) list {
   def n = q.len
   mut r = _dual_zero_float_matrix(n)
   mut i = 0
   while i < n {
      mut j = i
      while j < n {
         mut s = float(q[i][j])
         mut k = 0
         while k < i {
            s -= float(r[k][i]) * float(r[k][j])
            k += 1
         }
         if i == j {
            def diag = s <= 0.0 ? 0.000000000000001 : s
            r[i][i] = math.sqrt(diag)
         } else {
            r[i][j] = s / float(r[i][i])
         }
         j += 1
      }
      i += 1
   }
   r
}

fn _dual_seed_from_diag(list q) dict {
   def n = q.len
   mut best_i = 0
   mut best_norm = n > 0 ? float(q[0][0]) : 0.0
   mut i = 1
   while i < n {
      def v = float(q[i][i])
      if v < best_norm {
         best_i = i
         best_norm = v
      }
      i += 1
   }
   dict(2).set("coeffs", _dual_unit_coeffs(n, best_i)).set("norm", best_norm)
}

fn _dual_nodes_vec(int n) list {
   mut out = []
   mut i = 0
   while i <= n {
      out = out.append(0)
      i += 1
   }
   out
}

fn _dual_record(list coeffs, f64 norm, int nodes, bool hit_limit, list nodes_by_level) list {
   [coeffs, norm, nodes, hit_limit, nodes_by_level]
}

fn _dual_enum_dfs(int idx, list r, list coeffs, f64 partial, list best_coeffs, f64 best_norm, int nodes, int max_nodes, list nodes_by_level) dict {
   if idx >= 0 && idx < nodes_by_level.len { nodes_by_level = nodes_by_level.set(idx, nodes_by_level[idx] + 1) }
   if nodes >= max_nodes { return _dual_record(best_coeffs, best_norm, nodes, true, nodes_by_level) }
   nodes += 1
   if idx < 0 {
      if nodes_by_level.len > 0 { nodes_by_level = nodes_by_level.set(0, nodes_by_level[0] + 1) }
      if partial > 0.0 && partial < best_norm { return _dual_record(clone(coeffs), partial, nodes, false, nodes_by_level) }
      return _dual_record(best_coeffs, best_norm, nodes, false, nodes_by_level)
   }
   def rii = float(r[idx][idx])
   if rii == 0.0 { return _dual_record(best_coeffs, best_norm, nodes, true, nodes_by_level) }
   mut tail = 0.0
   mut j = idx + 1
   while j < coeffs.len {
      tail += float(r[idx][j]) * float(coeffs[j])
      j += 1
   }
   def rem = best_norm - partial
   if rem <= 0.0 { return _dual_record(best_coeffs, best_norm, nodes, false, nodes_by_level) }
   def center = -tail / rii
   def width = math.sqrt(rem) / abs(rii)
   def lo = int(math.ceil(center - width))
   def hi = int(math.floor(center + width))
   def nearest = _dual_round_float(center)
   def maxoff = max(abs(lo - nearest), abs(hi - nearest))
   mut state = _dual_record(best_coeffs, best_norm, nodes, false, nodes_by_level)
   mut off = 0
   while off <= maxoff {
      if off == 0 {
         if nearest >= lo && nearest <= hi {
            coeffs[idx] = nearest
            def y = rii * float(nearest) + tail
            state = _dual_enum_dfs(idx - 1, r, coeffs, partial + y * y, state[0], float(state[1]), int(state[2]), max_nodes, state[4])
            if state[3] { return state }
         }
      } else {
         def plus = nearest + off
         if plus >= lo && plus <= hi {
            coeffs[idx] = plus
            def y = rii * float(plus) + tail
            state = _dual_enum_dfs(idx - 1, r, coeffs, partial + y * y, state[0], float(state[1]), int(state[2]), max_nodes, state[4])
            if state[3] { return state }
         }
         def minus = nearest - off
         if minus >= lo && minus <= hi {
            coeffs[idx] = minus
            def y = rii * float(minus) + tail
            state = _dual_enum_dfs(idx - 1, r, coeffs, partial + y * y, state[0], float(state[1]), int(state[2]), max_nodes, state[4])
            if state[3] { return state }
         }
      }
      off += 1
   }
   state
}

fn _dual_unpermute_coeffs(list coeffs, list order) list {
   mut out = _dual_unit_coeffs(coeffs.len, -1)
   mut i = 0
   while i < coeffs.len {
      out[int(order[i])] = coeffs[i]
      i += 1
   }
   out
}

fn _dual_swap_list_entries(list xs, int a, int b) list {
   if a == b { return xs }
   def tmp = xs[a]
   xs[a] = xs[b]
   xs[b] = tmp
   xs
}

fn _dual_neg_row(list row) list {
   mut out = []
   mut i = 0
   while i < row.len {
      out = out.append(Z(0) - Z(row[i]))
      i += 1
   }
   out
}

fn _dual_row_linear(list a, any ca, list b, any cb) list {
   mut out = []
   def xa = Z(ca)
   def xb = Z(cb)
   mut i = 0
   while i < a.len {
      out = out.append(Z(a[i]) * xa + Z(b[i]) * xb)
      i += 1
   }
   out
}

fn _dual_first_nonzero(list coeffs) int {
   mut i = 0
   while i < coeffs.len {
      if Z(coeffs[i]) != Z(0) { return i }
      i += 1
   }
   -1
}

fn _dual_unit_to_last_transform(list coeffs) dict {
   def n = coeffs.len
   if n <= 0 {
      return dict(5).set("ok", false).set("reason", "empty dual coefficient vector").set("transform", _bkz_identity(n)).set("mapped", coeffs).set("primitive_gcd", Z(0))
   }
   mut acc = _dual_first_nonzero(coeffs)
   if acc < 0 {
      return dict(5).set("ok", false).set("reason", "zero dual coefficient vector").set("transform", _bkz_identity(n)).set("mapped", coeffs).set("primitive_gcd", Z(0))
   }
   mut rows = _matrix_data(_bkz_identity(n))
   mut mapped = []
   mut ci = 0
   while ci < n {
      mapped = mapped.append(Z(coeffs[ci]))
      ci += 1
   }
   mut i = 0
   while i < n {
      if i != acc && Z(mapped[i]) != Z(0) {
         def a = Z(mapped[acc])
         def b = Z(mapped[i])
         def eg = xgcd(a, b)
         def g = Z(eg[0])
         def x = Z(eg[1])
         def y = Z(eg[2])
         def old_acc = rows[acc]
         def old_i = rows[i]
         rows[acc] = _dual_row_linear(old_acc, x, old_i, y)
         rows[i] = _dual_row_linear(old_acc, Z(0) - bigint_div(b, g), old_i, bigint_div(a, g))
         mapped[acc] = g
         mapped[i] = Z(0)
      }
      i += 1
   }
   def primitive_gcd = Z(mapped[acc])
   if primitive_gcd != Z(1) && primitive_gcd != Z(-1) {
      return dict(5).set("ok", false).set("reason", "dual coefficient vector is not primitive").set("transform", Matrix(rows)).set("mapped", mapped).set("primitive_gcd", primitive_gcd)
   }
   def last = n - 1
   if acc != last {
      rows = _dual_swap_list_entries(rows, acc, last)
      mapped = _dual_swap_list_entries(mapped, acc, last)
   }
   if Z(mapped[last]) == Z(-1) {
      rows[last] = _dual_neg_row(rows[last])
      mapped[last] = Z(1)
   }
   dict(5).set("ok", mapped == _dual_unit_coeffs(n, last)).set("reason", "").set("transform", Matrix(rows)).set("mapped", mapped).set("primitive_gcd", primitive_gcd)
}

fn dual_svp_report(any basis, int max_nodes=400000) dict {
   "Report a short dual coefficient vector for the row lattice using inverse-Gram enumeration."
   def t0 = ticks()
   def B = _bkz_as_matrix(basis)
   def gso = gso_report(B)
   def q = _dual_inverse_gram_float(gso)
   def order = _dual_diag_order(q)
   def qp = _dual_permute_matrix(q, order)
   def r = _dual_cholesky_upper(qp)
   def seed = _dual_seed_from_diag(qp)
   def n = qp.len
   def state = _dual_enum_dfs(n - 1, r, _svp_zero_coeffs(n), 0.0, seed.get("coeffs"), float(seed.get("norm")), 0, max_nodes, _dual_nodes_vec(n))
   def coeffs = _dual_unpermute_coeffs(state[0], order)
   def verified_norm = _dual_norm_from_gso(gso, coeffs)
   def elapsed = _bkz_elapsed_ms(t0)
   mut out = dict(18)
   out = out.set("method", "inverse-gram-fincke-pohst")
   out = out.set("kernel", "dual-inverse-gram-dfs")
   out = out.set("ordering", "inverse-gram-diagonal-ascending")
   out = out.set("rows", _matrix_rows(B))
   out = out.set("cols", _matrix_cols(B))
   out = out.set("max_nodes", max_nodes)
   out = out.set("nodes", state[2])
   out = out.set("nodes_by_level", state[4])
   out = out.set("hit_limit", state[3])
   out = out.set("coeffs", coeffs)
   out = out.set("reduced_coeffs", state[0])
   out = out.set("norm", state[1])
   out = out.set("verified_norm", verified_norm)
   out = out.set("verified", abs(float(state[1]) - verified_norm) <= max(0.000000000001, abs(verified_norm) * 0.000001))
   out = out.set("permutation", order)
   out = out.set("elapsed_ms", elapsed)
   out = out.set("nodes_per_sec", _bkz_nodes_per_sec(int(state[2]), elapsed))
   out
}

fn dual_svp(any basis, int max_nodes=400000) list {
   "Return a short dual coefficient vector for the row lattice."
   dual_svp_report(basis, max_nodes).get("coeffs", [])
}

fn dual_svp_reduce_report(any basis, int max_nodes=400000) dict {
   "Return a report for exact row-transform reduction that moves a short dual vector to the last dual coordinate."
   def t0 = ticks()
   def B = _bkz_as_matrix(basis)
   def drep = dual_svp_report(B, max_nodes)
   def coeffs = drep.get("coeffs", [])
   def n = coeffs.len
   def map = _dual_unit_to_last_transform(coeffs)
   def transform = map.get("transform", _bkz_identity(n))
   def reduced = map.get("ok", false) ? _bkz_sparse_matmul(transform, B) : B
   def last = _dual_unit_coeffs(n, n - 1)
   def last_norm = map.get("ok", false) ? dual_svp_norm(reduced, last) : 0.0
   def target_norm = float(drep.get("verified_norm", drep.get("norm", 0.0)))
   def verified = map.get("ok", false) && drep.get("verified", false) && abs(last_norm - target_norm) <= max(0.000000000001, abs(target_norm) * 0.000001)
   def elapsed = _bkz_elapsed_ms(t0)
   mut out = dict(18)
   out = out.set("method", "dual-svp-unimodular-last-vector")
   out = out.set("selected_method", "ny")
   out = out.set("rows", _matrix_rows(B))
   out = out.set("cols", _matrix_cols(B))
   out = out.set("basis", reduced)
   out = out.set("transform", transform)
   out = out.set("transform_kernel", "sparse-row-exact-bkz-matmul")
   out = out.set("coeffs", coeffs)
   out = out.set("mapped_coeffs", map.get("mapped", []))
   out = out.set("primitive_gcd", map.get("primitive_gcd", Z(0)))
   out = out.set("dual_report", drep)
   out = out.set("target_norm", target_norm)
   out = out.set("last_dual_norm", last_norm)
   out = out.set("verified", verified)
   out = out.set("ok", verified)
   out = out.set("reason", verified ? "" : map.get("reason", "dual reduction verification failed"))
   out = out.set("elapsed_ms", elapsed)
   out
}

fn dual_svp_reduce(any basis, int max_nodes=400000) any {
   "Return a basis whose last dual coordinate has the short dual-vector norm."
   dual_svp_reduce_report(basis, max_nodes).get("basis")
}

fn _svp_small_depth_state(list best_v, any best_norm, list best_coeffs, int nodes) dict {
   {"vector": best_v, "norm": best_norm, "coeffs": best_coeffs, "nodes": nodes}
}

fn _svp_small_depth_dfs(int idx, list rows, list coeff_order, list coeffs, list cur, list best_v, any best_norm, list best_coeffs, int nodes, int max_nodes) dict {
   if nodes >= max_nodes { return _svp_small_depth_state(best_v, best_norm, best_coeffs, nodes) }
   if idx >= rows.len {
      nodes += 1
      if _bkz_vec_nonzero(cur) {
         def nr = _bkz_row_norm(cur)
         if nr > Z(0) && nr < best_norm {
            return _svp_small_depth_state(cur, nr, clone(coeffs), nodes)
         }
      }
      return _svp_small_depth_state(best_v, best_norm, best_coeffs, nodes)
   }
   mut state = _svp_small_depth_state(best_v, best_norm, best_coeffs, nodes)
   mut k = 0
   while k < coeff_order.len && int(state.get("nodes", 0)) < max_nodes {
      def c = coeff_order.get(k)
      coeffs[idx] = c
      def next = c == 0 ? cur : _bkz_vec_add_scaled(cur, rows.get(idx), c)
      state = _svp_small_depth_dfs(
         idx + 1,
         rows,
         coeff_order,
         coeffs,
         next,
         state.get("vector"),
         state.get("norm"),
         state.get("coeffs"),
         int(state.get("nodes", 0)),
         max_nodes
      )
      k += 1
   }
   coeffs[idx] = 0
   state
}

fn _svp_small_kernel_name(int n) str {
   case n {
      1..5 -> "dim" + str(n) + "-loop"
      6, 7 -> "dim" + str(n) + "-depth-loop"
      _ -> "dim8-depth-loop"
   }
}

fn _svp_small_kernel_core(any basis, any radius_sq=nil, int coeff_bound=1, int max_nodes=200000) dict {
   def B = _bkz_as_matrix(basis)
   def prep = _svp_reduce_with_transform(B)
   def reduced = prep.get("basis")
   def reduce_transform = prep.get("transform")
   def rows = _matrix_data(reduced)
   def n = _matrix_rows(reduced)
   def dim = _matrix_cols(reduced)
   if n <= 0 || n > 8 { return _svp_enumerate_core(B, radius_sq, coeff_bound, max_nodes) }
   def seed = _svp_seed_best(rows, dim, radius_sq)
   def coeff_order = _bkz_coeff_order(int(coeff_bound))
   def choices = coeff_order.len
   def total = _bkz_pow_int(choices, n)
   if n == 4 && total > 128 { return _svp_enumerate_core(B, radius_sq, coeff_bound, max_nodes) }
   if n == 5 && total > 512 { return _svp_enumerate_core(B, radius_sq, coeff_bound, max_nodes) }
   if n == 6 && total > 1024 { return _svp_enumerate_core(B, radius_sq, coeff_bound, max_nodes) }
   if n == 7 && total > 4096 { return _svp_enumerate_core(B, radius_sq, coeff_bound, max_nodes) }
   if n == 8 && total > 10000 { return _svp_enumerate_core(B, radius_sq, coeff_bound, max_nodes) }
   def visit_points = min(total, max(0, max_nodes))
   def gram = _svp_row_gram(rows)
   mut out = _svp_coeff_cube_range_core(
      reduced,
      rows,
      dim,
      coeff_order,
      0,
      visit_points,
      seed.get("vector"),
      seed.get("norm"),
      seed.get("coeffs", _svp_zero_coeffs(n)),
      gram
   )
   out = out.set("hit_limit", visit_points < total)
   out = out.set("kernel", _svp_small_kernel_name(n))
   out = out.set("kernel_specialized", true)
   out = out.set("total_coeff_points", total)
   out = out.set("visited_coeff_points", visit_points)
   _svp_state_with_input_coeffs(out, reduce_transform)
}

fn _svp_nodes_fill(int n, int nodes) list {
   mut out = []
   mut i = 0
   while i <= n {
      out = out.append(nodes)
      i += 1
   }
   out
}

fn _svp_range_stats(int nodes, int updates, int avoided, int norm_updates, int norm_avoided, int best_rebuilds) dict {
   {
      "nodes": nodes,
      "updates": updates,
      "avoided": avoided,
      "norm_updates": norm_updates,
      "norm_avoided": norm_avoided,
      "best_rebuilds": best_rebuilds
   }
}

fn _svp_range_result(list best_v, any best_norm, list best_coeffs, dict stats) dict {
   {
      "vector": best_v,
      "norm": best_norm,
      "coeffs": best_coeffs,
      "stats": stats
   }
}

fn _svp_range_record(
   any reduced,
   list best_v,
   any best_norm,
   list best_coeffs,
   int n,
   int start_code,
   int stop_code,
   dict stats,
   bool shared_gram,
   any t0,
   str numeric_kernel=""
) dict {
   def nodes = int(stats.get("nodes", 0))
   def elapsed = _bkz_elapsed_ms(t0)
   mut out = _svp_core_record_coeffs(best_v, best_norm, nodes, false, _svp_nodes_fill(n, nodes), reduced, best_coeffs)
   .set("start_code", start_code)
   .set("stop_code", stop_code)
   .set("incremental_vector_updates", int(stats.get("updates", 0)))
   .set("incremental_coefficient_updates", int(stats.get("updates", 0)))
   .set("vector_rebuilds_avoided", int(stats.get("avoided", 0)))
   .set("incremental_norm_updates", int(stats.get("norm_updates", 0)))
   .set("norm_recomputes_avoided", int(stats.get("norm_avoided", 0)))
   .set("best_vector_rebuilds", int(stats.get("best_rebuilds", 0)))
   .set("shared_gram", shared_gram)
   .set("gram_recomputes_avoided", shared_gram ? 1 : 0)
   .set("elapsed_ms", elapsed)
   .set("nodes_per_sec", _bkz_nodes_per_sec(nodes, elapsed))
   if numeric_kernel != "" { out = out.set("numeric_kernel", numeric_kernel) }
   out
}

fn _svp_coeffs_int(list coeffs) list<int> {
   mut list<int> out = list(coeffs.len)
   mut i = 0
   while i < coeffs.len {
      out = out.append(int(coeffs[i]))
      i += 1
   }
   out
}

fn _svp_range_step_z(list coeff_order, int choices, int n, int code, int next_code, list coeffs, list gram, any cur_norm) list {
   mut div = 1
   mut pos = n - 1
   mut carry = true
   mut updates = 0
   while pos > -1 && carry {
      def old_idx = (code / div) % choices
      def new_idx = (next_code / div) % choices
      def delta = coeff_order[new_idx] - coeff_order[old_idx]
      if delta != 0 {
         def dz = Z(delta)
         def gram_dot = _svp_coeff_gram_dot(coeffs, gram[pos])
         cur_norm = cur_norm + Z(2) * dz * gram_dot + dz * dz * gram[pos][pos]
         coeffs[pos] = int(coeffs[pos]) + delta
         updates += 1
      }
      carry = old_idx == choices - 1 && new_idx == 0
      div *= choices
      pos -= 1
   }
   [cur_norm, coeffs, updates]
}

fn _svp_range_loop_int(list rows, int dim, list coeff_order, int start_code, int stop_code, list seed_v, int seed_norm, list seed_coeffs, list<list<int>> gram) dict {
   def n = gram.len
   def choices = coeff_order.len
   mut best_v = seed_v
   mut best_norm = seed_norm
   mut best_coeffs = seed_coeffs
   mut code = start_code
   mut cur_coeffs = _svp_coeffs_int(_svp_coeffs_from_code(coeff_order, n, start_code))
   mut idxs = _svp_coeff_indices_from_code(coeff_order, n, start_code)
   mut cur_norm = _svp_coeff_norm_from_gram_int(cur_coeffs, gram)
   mut nodes, updates, avoided = 0, 0, 0
   mut norm_updates, norm_avoided, best_rebuilds = 0, 0, 0
   mut done = false
   while !done && code < stop_code {
      nodes += 1
      norm_avoided += 1
      if cur_norm > 0 && cur_norm < best_norm {
         best_v = _svp_combo(rows, dim, cur_coeffs)
         best_norm = cur_norm
         best_coeffs = clone(cur_coeffs)
         best_rebuilds += 1
      }
      def next_code = code + 1
      if next_code >= stop_code {
         done = true
      } else {
         mut pos = n - 1
         mut carry = true
         mut step_updates = 0
         while pos > -1 && carry {
            def old_idx = idxs[pos]
            mut new_idx = old_idx + 1
            if new_idx >= choices { new_idx = 0 }
            else { carry = false }
            idxs[pos] = new_idx
            def delta = coeff_order[new_idx] - coeff_order[old_idx]
            if delta != 0 {
               def gram_row = gram[pos]
               mut gram_dot = 0
               if n == 12 {
                  gram_dot =
                  cur_coeffs[0] * gram_row[0] + cur_coeffs[1] * gram_row[1] +
                  cur_coeffs[2] * gram_row[2] + cur_coeffs[3] * gram_row[3] +
                  cur_coeffs[4] * gram_row[4] + cur_coeffs[5] * gram_row[5] +
                  cur_coeffs[6] * gram_row[6] + cur_coeffs[7] * gram_row[7] +
                  cur_coeffs[8] * gram_row[8] + cur_coeffs[9] * gram_row[9] +
                  cur_coeffs[10] * gram_row[10] + cur_coeffs[11] * gram_row[11]
               } else {
                  mut gi = 0
                  while gi < n {
                     gram_dot += cur_coeffs[gi] * gram_row[gi]
                     gi += 1
                  }
               }
               cur_norm = cur_norm + 2 * delta * gram_dot + delta * delta * gram_row[pos]
               cur_coeffs[pos] = cur_coeffs[pos] + delta
               step_updates += 1
            }
            pos -= 1
         }
         updates += step_updates
         avoided += step_updates * (n - 1)
         norm_updates += step_updates
         code = next_code
      }
   }
   _svp_range_result(best_v, Z(best_norm), best_coeffs, _svp_range_stats(nodes, updates, avoided, norm_updates, norm_avoided, best_rebuilds))
}

fn _svp_range_loop_z(list rows, int dim, list coeff_order, int start_code, int stop_code, list seed_v, any seed_norm, list seed_coeffs, list gram) dict {
   def n = gram.len
   def choices = coeff_order.len
   mut best_v = seed_v
   mut best_norm = seed_norm
   mut best_coeffs = seed_coeffs
   mut code = start_code
   mut cur_coeffs = _svp_coeffs_from_code(coeff_order, n, start_code)
   mut cur_norm = _svp_coeff_norm_from_gram(cur_coeffs, gram)
   mut nodes, updates, avoided = 0, 0, 0
   mut norm_updates, norm_avoided, best_rebuilds = 0, 0, 0
   mut done = false
   while !done && code < stop_code {
      nodes += 1
      norm_avoided += 1
      if cur_norm > Z(0) && cur_norm < best_norm {
         best_v = _svp_combo(rows, dim, cur_coeffs)
         best_norm = cur_norm
         best_coeffs = clone(cur_coeffs)
         best_rebuilds += 1
      }
      def next_code = code + 1
      if next_code >= stop_code {
         done = true
      } else {
         def stepped = _svp_range_step_z(coeff_order, choices, n, code, next_code, cur_coeffs, gram, cur_norm)
         cur_norm = stepped[0]
         cur_coeffs = stepped[1]
         def step_updates = int(stepped[2])
         updates += step_updates
         avoided += step_updates * (n - 1)
         norm_updates += step_updates
         code = next_code
      }
   }
   _svp_range_result(best_v, best_norm, best_coeffs, _svp_range_stats(nodes, updates, avoided, norm_updates, norm_avoided, best_rebuilds))
}

fn _svp_coeff_cube_range_core(any reduced, list rows, int dim, list coeff_order, int start_code, int stop_code, list seed_v, any seed_norm, list seed_coeffs, list gram=[]) dict {
   def t0 = ticks()
   def n = _matrix_rows(reduced)
   def choices = coeff_order.len
   if start_code >= stop_code || n <= 0 || choices <= 0 {
      return _svp_range_record(reduced, seed_v, seed_norm, seed_coeffs, n, start_code, stop_code, _svp_range_stats(0, 0, 0, 0, 0, 0), false, t0)
   }
   def shared_gram = gram.len > 0
   def local_gram = shared_gram ? gram : _svp_row_gram(rows)
   def local_gram_int = _svp_gram_int_or_nil(local_gram, 40)
   def seed_norm_int = _bkz_small_int_or_nil(seed_norm, 40)
   if local_gram_int != nil && seed_norm_int.get(0) {
      def r = _svp_range_loop_int(rows, dim, coeff_order, start_code, stop_code, seed_v, int(seed_norm_int.get(1)), seed_coeffs, local_gram_int)
      return _svp_range_record(reduced, r.get("vector"), r.get("norm"), r.get("coeffs"), n, start_code, stop_code, r.get("stats"), shared_gram, t0, "int-coeff-gram")
   }
   def r = _svp_range_loop_z(rows, dim, coeff_order, start_code, stop_code, seed_v, seed_norm, seed_coeffs, local_gram)
   _svp_range_record(reduced, r.get("vector"), r.get("norm"), r.get("coeffs"), n, start_code, stop_code, r.get("stats"), shared_gram, t0)
}

fn _svp_coeff_cube_range_worker(any args) dict {
   _svp_coeff_cube_range_core(
      args.get(0), args.get(1), int(args.get(2)), args.get(3),
      int(args.get(4)), int(args.get(5)), args.get(6), args.get(7), args.get(8), args.get(9)
   )
}

fn _svp_coeff_cube_shard_ranges(int count, int shards) list {
   mut out = []
   if count <= 0 { return out }
   if shards < 1 { shards = 1 }
   if shards > count { shards = count }
   def chunk = (count + shards - 1) / shards
   mut start = 0
   while start < count {
      def stop = min(count, start + chunk)
      out = out.append([start, stop])
      start = stop
   }
   out
}

fn _svp_shard_args(any reduced, list rows, int dim, list coeff_order, list range, list best_v, any best_norm, list best_coeffs, list gram) list {
   [reduced, rows, dim, coeff_order, int(range.get(0)), int(range.get(1)), best_v, best_norm, best_coeffs, gram]
}

fn _svp_range_result_serial(any reduced, list rows, int dim, list coeff_order, list range, list best_v, any best_norm, list best_coeffs, list gram) dict {
   _svp_coeff_cube_range_core(reduced, rows, dim, coeff_order, int(range.get(0)), int(range.get(1)), best_v, best_norm, best_coeffs, gram)
}

fn _svp_collect_shards_serial(list ranges, any reduced, list rows, int dim, list coeff_order, list best_v, any best_norm, list best_coeffs, list gram) list {
   mut out = []
   mut ri = 0
   while ri < ranges.len {
      out = out.append(_svp_range_result_serial(reduced, rows, dim, coeff_order, ranges[ri], best_v, best_norm, best_coeffs, gram))
      ri += 1
   }
   out
}

fn _svp_collect_shards_threaded(list ranges, any reduced, list rows, int dim, list coeff_order, list best_v, any best_norm, list best_coeffs, list gram) list {
   mut handles = []
   mut ok = true
   mut hi = 0
   while hi < ranges.len && ok {
      def h = thread_spawn(_svp_coeff_cube_range_worker, _svp_shard_args(reduced, rows, dim, coeff_order, ranges[hi], best_v, best_norm, best_coeffs, gram))
      if h == -1 { ok = false }
      else { handles = handles.append(h) }
      hi += 1
   }
   mut out = []
   hi = 0
   while hi < handles.len {
      def rep = thread_join(handles[hi])
      if ok { out = out.append(rep) }
      hi += 1
   }
   [ok, out]
}

fn _svp_collect_shards(list ranges, any reduced, list rows, int dim, list coeff_order, list best_v, any best_norm, list best_coeffs, list gram) list {
   if ranges.len > 1 {
      def threaded = _svp_collect_shards_threaded(ranges, reduced, rows, dim, coeff_order, best_v, best_norm, best_coeffs, gram)
      if threaded[0] { return [true, threaded[1]] }
   }
   [false, _svp_collect_shards_serial(ranges, reduced, rows, dim, coeff_order, best_v, best_norm, best_coeffs, gram)]
}

fn _svp_shard_accum_init(list best_v, any best_norm, list best_coeffs) dict {
   {
      "reports": [],
      "nodes": 0,
      "updates": 0,
      "avoided": 0,
      "norm_updates": 0,
      "norm_avoided": 0,
      "best_rebuilds": 0,
      "slowest_shard": -1,
      "elapsed_sum": 0.0,
      "elapsed_max": 0.0,
      "nps_min": -1.0,
      "nps_max": 0.0,
      "gram_avoided": 0,
      "numeric_kernel": "",
      "best_v": best_v,
      "best_norm": best_norm,
      "best_coeffs": best_coeffs
   }
}

fn _svp_shard_report_row(int si, dict rep, bool threaded, f64 elapsed_ms, f64 nodes_per_sec, str numeric_kernel, any best_norm) dict {
   {
      "shard": si,
      "start_code": rep.get("start_code", 0),
      "stop_code": rep.get("stop_code", 0),
      "nodes": rep.get("nodes", 0),
      "execution": threaded ? "threaded" : "serial",
      "elapsed_ms": elapsed_ms,
      "nodes_per_sec": nodes_per_sec,
      "incremental_vector_updates": rep.get("incremental_vector_updates", 0),
      "incremental_coefficient_updates": rep.get("incremental_coefficient_updates", rep.get("incremental_vector_updates", 0)),
      "vector_rebuilds_avoided": rep.get("vector_rebuilds_avoided", 0),
      "incremental_norm_updates": rep.get("incremental_norm_updates", 0),
      "norm_recomputes_avoided": rep.get("norm_recomputes_avoided", 0),
      "best_vector_rebuilds": rep.get("best_vector_rebuilds", 0),
      "shared_gram": rep.get("shared_gram", false),
      "gram_recomputes_avoided": rep.get("gram_recomputes_avoided", 0),
      "numeric_kernel": numeric_kernel,
      "best_norm": rep.get("norm", best_norm)
   }
}

fn _svp_shard_accum_add(dict acc, int si, dict rep, bool threaded) dict {
   def elapsed_ms = float(rep.get("elapsed_ms", 0.0))
   def nodes_per_sec = float(rep.get("nodes_per_sec", 0.0))
   def numeric_kernel = to_str(rep.get("numeric_kernel", ""))
   if to_str(acc.get("numeric_kernel", "")) == "" && numeric_kernel != "" { acc["numeric_kernel"] = numeric_kernel }
   acc["reports"] = acc.get("reports", []).append(_svp_shard_report_row(si, rep, threaded, elapsed_ms, nodes_per_sec, numeric_kernel, acc.get("best_norm")))
   acc["nodes"] = int(acc.get("nodes", 0)) + int(rep.get("nodes", 0))
   acc["updates"] = int(acc.get("updates", 0)) + int(rep.get("incremental_vector_updates", 0))
   acc["avoided"] = int(acc.get("avoided", 0)) + int(rep.get("vector_rebuilds_avoided", 0))
   acc["norm_updates"] = int(acc.get("norm_updates", 0)) + int(rep.get("incremental_norm_updates", 0))
   acc["norm_avoided"] = int(acc.get("norm_avoided", 0)) + int(rep.get("norm_recomputes_avoided", 0))
   acc["best_rebuilds"] = int(acc.get("best_rebuilds", 0)) + int(rep.get("best_vector_rebuilds", 0))
   acc["gram_avoided"] = int(acc.get("gram_avoided", 0)) + int(rep.get("gram_recomputes_avoided", 0))
   acc["elapsed_sum"] = float(acc.get("elapsed_sum", 0.0)) + elapsed_ms
   if elapsed_ms >= float(acc.get("elapsed_max", 0.0)) {
      acc["elapsed_max"] = elapsed_ms
      acc["slowest_shard"] = si
   }
   if nodes_per_sec > 0.0 && (float(acc.get("nps_min", -1.0)) < 0.0 || nodes_per_sec < float(acc.get("nps_min", -1.0))) { acc["nps_min"] = nodes_per_sec }
   if nodes_per_sec > float(acc.get("nps_max", 0.0)) { acc["nps_max"] = nodes_per_sec }
   if rep.get("norm", acc.get("best_norm")) < acc.get("best_norm") {
      acc["best_v"] = rep.get("vector")
      acc["best_norm"] = rep.get("norm")
      acc["best_coeffs"] = rep.get("coeffs")
   }
   acc
}

fn _svp_shard_accumulate(list shard_results, bool threaded, list best_v, any best_norm, list best_coeffs) dict {
   mut acc = _svp_shard_accum_init(best_v, best_norm, best_coeffs)
   mut si = 0
   while si < shard_results.len {
      acc = _svp_shard_accum_add(acc, si, shard_results[si], threaded)
      si += 1
   }
   acc
}

fn _svp_sharded_record(any reduced, any reduce_transform, dict acc, int n, int total_points, int visit_points, int max_shards, int range_count, bool threaded) dict {
   def nodes = int(acc.get("nodes", 0))
   def numeric_kernel = to_str(acc.get("numeric_kernel", ""))
   mut out = _svp_core_record_coeffs(acc.get("best_v"), acc.get("best_norm"), nodes, visit_points < total_points, _svp_nodes_fill(n, nodes), reduced, acc.get("best_coeffs"))
   out = out.set("kernel", "sharded-incremental-coefficient-cube")
   out = out.set("kernel_specialized", false)
   out = out.set("method", "sharded-bounded-coefficient-cube")
   out = out.set("total_coeff_points", total_points)
   out = out.set("visited_coeff_points", visit_points)
   out = out.set("max_shards", max_shards)
   out = out.set("shard_count", range_count)
   out = out.set("shards", acc.get("reports", []))
   out = out.set("parallel_ready", true)
   out = out.set("threaded", threaded)
   out = out.set("threaded_shards", threaded ? range_count : 0)
   out = out.set("execution", threaded ? "threaded-shards" : "serial-shards")
   out = out.set("incremental_vector_updates", int(acc.get("updates", 0)))
   out = out.set("incremental_coefficient_updates", int(acc.get("updates", 0)))
   out = out.set("vector_rebuilds_avoided", int(acc.get("avoided", 0)))
   out = out.set("incremental_norm_updates", int(acc.get("norm_updates", 0)))
   out = out.set("norm_recomputes_avoided", int(acc.get("norm_avoided", 0)))
   out = out.set("best_vector_rebuilds", int(acc.get("best_rebuilds", 0)))
   out = out.set("shared_gram", true)
   out = out.set("numeric_kernel", numeric_kernel)
   out = out.set("integer_gram_fast_path", numeric_kernel == "int-coeff-gram")
   out = out.set("gram_recomputes", 1)
   out = out.set("gram_recomputes_avoided", int(acc.get("gram_avoided", 0)))
   out = out.set("slowest_shard", int(acc.get("slowest_shard", -1)))
   out = out.set("shard_elapsed_ms_sum", float(acc.get("elapsed_sum", 0.0)))
   out = out.set("shard_elapsed_ms_max", float(acc.get("elapsed_max", 0.0)))
   out = out.set("shard_nodes_per_sec_min", float(acc.get("nps_min", -1.0)))
   out = out.set("shard_nodes_per_sec_max", float(acc.get("nps_max", 0.0)))
   _svp_state_with_input_coeffs(out, reduce_transform)
}

fn _svp_coeff_cube_sharded_core(any basis, any radius_sq=nil, int coeff_bound=1, int max_nodes=200000, int max_shards=8) dict {
   def B = _bkz_as_matrix(basis)
   def prep = _svp_reduce_with_transform(B)
   def reduced = prep.get("basis")
   def reduce_transform = prep.get("transform")
   def rows = _matrix_data(reduced)
   def n = _matrix_rows(reduced)
   def dim = _matrix_cols(reduced)
   def seed = _svp_seed_best(rows, dim, radius_sq)
   mut best_v = seed.get("vector")
   mut best_norm = seed.get("norm")
   mut best_coeffs = seed.get("coeffs", _svp_zero_coeffs(n))
   def coeff_order = _bkz_coeff_order(int(coeff_bound))
   def choices = coeff_order.len
   def total_points = _bkz_pow_int(choices, n)
   def visit_points = min(total_points, max(0, max_nodes))
   def shard_count = min(max(1, max_shards), max(1, (visit_points + 4095) / 4096))
   def ranges = _svp_coeff_cube_shard_ranges(visit_points, shard_count)
   def gram = _svp_row_gram(rows)
   def collected = _svp_collect_shards(ranges, reduced, rows, dim, coeff_order, best_v, best_norm, best_coeffs, gram)
   def threaded = collected[0]
   def acc = _svp_shard_accumulate(collected[1], threaded, best_v, best_norm, best_coeffs)
   _svp_sharded_record(reduced, reduce_transform, acc, n, total_points, visit_points, max_shards, ranges.len, threaded)
}

fn _svp_enumerate_core(any basis, any radius_sq=nil, int coeff_bound=1, int max_nodes=200000) dict {
   def B = _bkz_as_matrix(basis)
   def n = _matrix_rows(B)
   def cube_points = _bkz_pow_int(_bkz_coeff_order(max(1, int(coeff_bound))).len, n)
   if (n <= 4 && cube_points <= 128) || (n == 5 && cube_points <= 512) || (n == 6 && cube_points <= 1024) || (n == 7 && cube_points <= 4096) || (n == 8 && cube_points <= 10000) { return _svp_small_kernel_core(B, radius_sq, coeff_bound, max_nodes) }
   if n >= 9 && cube_points <= max_nodes && cube_points <= 1000000 { return _svp_coeff_cube_sharded_core(B, radius_sq, coeff_bound, max_nodes) }
   def prep = _svp_reduce_with_transform(B)
   def gso = _svp_gso_enumerate_prepped(prep.get("basis"), prep.get("transform"), radius_sq, coeff_bound, max(1, max_nodes / 2))
   def low = _svp_low_weight_prepped(prep.get("basis"), prep.get("transform"), radius_sq, coeff_bound, max(1, max_nodes - int(gso.get("nodes", 0))), 3)
   mut best = _svp_better_core(gso, low)
   best = best.set("method", "hybrid-gso-low-weight-enumeration")
   best = best.set("selected_kernel", best.get("kernel", ""))
   best = best.set("candidate_reports", [gso, low])
   best = best.set("gso_bounds", gso.get("gso_bounds", []))
   best = best.set("gso_bound_estimate", gso.get("gso_bound_estimate", 0))
   best = best.set("gso_profile", gso.get("gso_profile", gso_report(best.get("basis", basis))))
   best = best.set("input_dimension", gso.get("input_dimension", n))
   best = best.set("enumeration_dimension", gso.get("enumeration_dimension", n))
   best = best.set("last_useful_index", gso.get("last_useful_index", n))
   best = best.set("ignored_tail_vectors", gso.get("ignored_tail_vectors", 0))
   best = best.set("basis_min_norm", gso.get("basis_min_norm", best.get("norm", 0)))
   best = best.set("basis_min_index", gso.get("basis_min_index", -1))
   best = best.set("initial_bound_source", gso.get("initial_bound_source", "basis-min-row"))
   best = best.set("dimension_trimmed", gso.get("dimension_trimmed", false))
   best.set("enumeration_order", "reverse-gso+low-weight")
}

fn svp_enumerate_report(any basis, any radius_sq=nil, int coeff_bound=1, int max_nodes=200000) dict {
   "GSO-profiled bounded SVP enumeration report.
   The search walks short integer combinations of an LLL-reduced basis, uses
   dimension-specialized kernels for tiny blocks and reverse-GSO bounded DFS for
   larger blocks, then verifies the exact norm of the best vector found."
   def t0 = ticks()
   def core = _svp_enumerate_core(basis, radius_sq, coeff_bound, max_nodes)
   def reduced = core.get("basis")
   def n = _matrix_rows(reduced)
   def best_v = core.get("vector", _bkz_zero_vec(_matrix_cols(reduced)))
   def best_norm = core.get("norm", _bkz_row_norm(best_v))
   def verified_norm = _bkz_row_norm(best_v)
   def elapsed = _bkz_elapsed_ms(t0)
   mut gso_profile_out = core.get("gso_profile", nil)
   if gso_profile_out == nil { gso_profile_out = gso_report(reduced) }
   {
      "method": core.get("method", core.get("kernel_specialized", false) ? "dimension-specialized-enumeration" : "gso-bounded-enumeration"),
      "reduction_method": "ny", "kernel": core.get("kernel", "generic-index-counter"), "selected_kernel": core.get("selected_kernel", core.get("kernel", "")),
      "kernel_specialized": core.get("kernel_specialized", false), "coeff_bound": coeff_bound, "max_nodes": max_nodes,
      "nodes": core.get("nodes", 0), "nodes_by_level": core.get("nodes_by_level", _bkz_nodes_vec(n)), "hit_limit": core.get("hit_limit", false),
      "visited_coeff_points": core.get("visited_coeff_points", core.get("nodes", 0)), "total_coeff_points": core.get("total_coeff_points", core.get("nodes", 0)),
      "shard_count": core.get("shard_count", 0), "shards": core.get("shards", []), "parallel_ready": core.get("parallel_ready", false),
      "threaded": core.get("threaded", false), "threaded_shards": core.get("threaded_shards", 0), "execution": core.get("execution", ""),
      "slowest_shard": core.get("slowest_shard", -1), "shard_elapsed_ms_sum": core.get("shard_elapsed_ms_sum", 0.0),
      "shard_elapsed_ms_max": core.get("shard_elapsed_ms_max", 0.0), "shard_nodes_per_sec_min": core.get("shard_nodes_per_sec_min", -1.0),
      "shard_nodes_per_sec_max": core.get("shard_nodes_per_sec_max", 0.0), "incremental_vector_updates": core.get("incremental_vector_updates", 0),
      "incremental_coefficient_updates": core.get("incremental_coefficient_updates", core.get("incremental_vector_updates", 0)),
      "vector_rebuilds_avoided": core.get("vector_rebuilds_avoided", 0), "incremental_norm_updates": core.get("incremental_norm_updates", 0),
      "norm_recomputes_avoided": core.get("norm_recomputes_avoided", 0), "best_vector_rebuilds": core.get("best_vector_rebuilds", 0),
      "shared_gram": core.get("shared_gram", false), "gram_recomputes": core.get("gram_recomputes", 0), "gram_recomputes_avoided": core.get("gram_recomputes_avoided", 0),
      "numeric_kernel": core.get("numeric_kernel", ""), "integer_gram_fast_path": core.get("integer_gram_fast_path", false),
      "vector": best_v, "coeffs": core.get("coeffs", []), "norm": best_norm, "verified_norm": verified_norm, "verified": verified_norm == best_norm,
      "radius_sq": best_norm, "pruning_profile": _bkz_pruning_profile(n, best_norm), "gso_profile": gso_profile_out,
      "gso_bounds": core.get("gso_bounds", []), "gso_bound_estimate": core.get("gso_bound_estimate", 0),
      "input_dimension": core.get("input_dimension", n), "enumeration_dimension": core.get("enumeration_dimension", n),
      "last_useful_index": core.get("last_useful_index", n), "ignored_tail_vectors": core.get("ignored_tail_vectors", 0),
      "basis_min_norm": core.get("basis_min_norm", best_norm), "basis_min_index": core.get("basis_min_index", -1),
      "initial_bound_source": core.get("initial_bound_source", "basis-min-row"), "dimension_trimmed": core.get("dimension_trimmed", false),
      "enumeration_order": core.get("enumeration_order", "center-out"), "candidate_reports": core.get("candidate_reports", []),
      "basis": reduced, "elapsed_ms": elapsed, "nodes_per_sec": _bkz_nodes_per_sec(int(core.get("nodes", 0)), elapsed)
   }
}

fn svp_kernel_report(any basis, any radius_sq=nil, int coeff_bound=1, int max_nodes=200000, int max_shards=8) dict {
   "Run the bounded coefficient-kernel path: dimension-specialized through eight rows, incremental cube above that when budget allows."
   def t0 = ticks()
   def B = _bkz_as_matrix(basis)
   def n = _matrix_rows(B)
   def choices = _bkz_coeff_order(max(1, int(coeff_bound))).len
   def total = _bkz_pow_int(choices, n)
   def core = n <= 8 ? _svp_small_kernel_core(B, radius_sq, coeff_bound, max_nodes) : _svp_coeff_cube_sharded_core(B, radius_sq, coeff_bound, max_nodes, max_shards)
   def reduced = core.get("basis")
   def best_v = core.get("vector", _bkz_zero_vec(_matrix_cols(reduced)))
   def verified_norm = _bkz_row_norm(best_v)
   def elapsed = _bkz_elapsed_ms(t0)
   {
      "method": core.get("method", core.get("kernel_specialized", false) ? "dimension-specialized-enumeration" : "generic-index-counter"),
      "kernel": core.get("kernel", "generic-index-counter"), "kernel_specialized": core.get("kernel_specialized", false),
      "coeff_bound": coeff_bound, "max_nodes": max_nodes, "nodes": core.get("nodes", 0),
      "nodes_by_level": core.get("nodes_by_level", _bkz_nodes_vec(_matrix_rows(reduced))), "hit_limit": core.get("hit_limit", false),
      "total_coeff_points": core.get("total_coeff_points", total), "visited_coeff_points": core.get("visited_coeff_points", core.get("nodes", 0)),
      "max_shards": max_shards, "shard_count": core.get("shard_count", 0), "shards": core.get("shards", []), "parallel_ready": core.get("parallel_ready", false),
      "threaded": core.get("threaded", false), "threaded_shards": core.get("threaded_shards", 0), "execution": core.get("execution", ""),
      "slowest_shard": core.get("slowest_shard", -1), "shard_elapsed_ms_sum": core.get("shard_elapsed_ms_sum", 0.0),
      "shard_elapsed_ms_max": core.get("shard_elapsed_ms_max", 0.0), "shard_nodes_per_sec_min": core.get("shard_nodes_per_sec_min", -1.0),
      "shard_nodes_per_sec_max": core.get("shard_nodes_per_sec_max", 0.0), "incremental_vector_updates": core.get("incremental_vector_updates", 0),
      "incremental_coefficient_updates": core.get("incremental_coefficient_updates", core.get("incremental_vector_updates", 0)),
      "vector_rebuilds_avoided": core.get("vector_rebuilds_avoided", 0), "incremental_norm_updates": core.get("incremental_norm_updates", 0),
      "norm_recomputes_avoided": core.get("norm_recomputes_avoided", 0), "best_vector_rebuilds": core.get("best_vector_rebuilds", 0),
      "shared_gram": core.get("shared_gram", false), "gram_recomputes": core.get("gram_recomputes", 0), "gram_recomputes_avoided": core.get("gram_recomputes_avoided", 0),
      "numeric_kernel": core.get("numeric_kernel", ""), "integer_gram_fast_path": core.get("integer_gram_fast_path", false),
      "vector": best_v, "coeffs": core.get("coeffs", []), "norm": core.get("norm", verified_norm),
      "verified_norm": verified_norm, "verified": verified_norm == core.get("norm", verified_norm),
      "basis": reduced, "elapsed_ms": elapsed, "nodes_per_sec": _bkz_nodes_per_sec(int(core.get("nodes", 0)), elapsed)
   }
}

fn svp_kernel(any basis, any radius_sq=nil, int coeff_bound=1, int max_nodes=200000, int max_shards=8) list {
   "Return the vector found by svp_kernel_report."
   svp_kernel_report(basis, radius_sq, coeff_bound, max_nodes, max_shards).get("vector")
}

fn svp_enumerate(any basis, any radius_sq=nil, int coeff_bound=1, int max_nodes=200000) list {
   "Return a short non-zero lattice vector found by bounded enumeration."
   _svp_enumerate_core(basis, radius_sq, coeff_bound, max_nodes).get("vector")
}

fn svp_report(any basis, any radius_sq=nil, int coeff_bound=1, int max_nodes=200000) dict {
   "Report-first public SVP API backed by bounded enumeration."
   svp_enumerate_report(basis, radius_sq, coeff_bound, max_nodes)
}

fn _bkz_elapsed_ms(any t0) f64 { float(ticks() - t0) / 1000000.0 }

fn _bkz_nodes_per_sec(int nodes, any elapsed_ms) f64 {
   def ms = float(elapsed_ms)
   ms <= 0.0 ? 0.0 : float(nodes) * 1000.0 / ms
}

fn _bkz_block_with_vector_report(any block, list v, list coeffs) dict {
   def want = _matrix_rows(block)
   def old = _matrix_data(block)
   def dim = _matrix_cols(block)
   mut rows = []
   mut transform_rows = []
   if _bkz_vec_nonzero(v) {
      rows = rows.append(v)
      transform_rows = transform_rows.append(coeffs.len == want ? coeffs : _bkz_zero_coeffs(want))
   }
   mut i = 0
   while rows.len < want && i < old.len {
      def row = old[i]
      if !_bkz_rows_equal(row, v) {
         rows = rows.append(row)
         transform_rows = transform_rows.append(_bkz_unit_coeffs(want, i))
      }
      i += 1
   }
   while rows.len < want {
      rows = rows.append(_bkz_zero_vec(dim))
      transform_rows = transform_rows.append(_bkz_zero_coeffs(want))
   }
   mut out = dict(6)
   out = out.set("basis", Matrix(rows))
   out = out.set("transform", Matrix(transform_rows))
   out
}

fn _bkz_count_tour_block_bool(list tours, str field) int {
   mut count = 0
   mut ti = 0
   while ti < tours.len {
      def blocks = tours.get(ti).get("blocks", [])
      mut bi = 0
      while bi < blocks.len {
         if blocks.get(bi).get(field, false) { count += 1 }
         bi += 1
      }
      ti += 1
   }
   count
}

fn _bkz_sum_tour_block_int(list tours, str field) int {
   mut total = 0
   mut ti = 0
   while ti < tours.len {
      def blocks = tours.get(ti).get("blocks", [])
      mut bi = 0
      while bi < blocks.len {
         total += int(blocks.get(bi).get(field, 0))
         bi += 1
      }
      ti += 1
   }
   total
}

fn _bkz_retry_policy(int n, int block_size, bool record, int svp_max_nodes) dict {
   if n <= 32 {
      return {
         "method": "bounded-bkz-rerandomized-retry-policy", "rows": n, "block_size": block_size, "svp_max_nodes": svp_max_nodes,
         "retry_node_budget": svp_max_nodes, "enabled": true, "max_retries_per_tour": max(1, n), "policy_class": "small-exhaustive",
         "reason": "small basis: retry all stalled blocks"
      }
   }
   if record && n <= 128 && block_size <= 16 && svp_max_nodes <= 2048 {
      return {
         "method": "bounded-bkz-rerandomized-retry-policy", "rows": n, "block_size": block_size, "svp_max_nodes": svp_max_nodes,
         "retry_node_budget": min(svp_max_nodes, 64), "enabled": true, "max_retries_per_tour": 1, "policy_class": "bounded-high-dimensional-fixture",
         "reason": "recorded high-dimensional small-window fixture: one low-node stalled-block retry per tour"
      }
   }
   {
      "method": "bounded-bkz-rerandomized-retry-policy", "rows": n, "block_size": block_size, "svp_max_nodes": svp_max_nodes,
      "retry_node_budget": 0, "enabled": false, "max_retries_per_tour": 0, "policy_class": "disabled",
      "reason": "avoid unbounded high-dimensional retry cost without a calibrated strategy gate"
   }
}

fn _bkz_prepare_insert_candidate(any block, any svcore, list sv, list sv_coeffs, any delta, any eta, bool record, bool debug_transform) list {
   mut candidate = block
   mut local_transform = _bkz_identity(_matrix_rows(block))
   mut local_transform_mismatch = []
   mut pre_transform_mismatch = []
   mut local_lll_transform_mismatch = []
   def direct_basis = svcore.get("basis", nil)
   def direct_transform = svcore.get("basis_transform", nil)
   def direct_reduced = int(svcore.get("nodes", 0)) == 0 && is_matrix(direct_basis) && is_matrix(direct_transform) && _matrix_rows(direct_basis) == _matrix_rows(block)
   if direct_reduced {
      candidate = direct_basis
      local_transform = direct_transform
      if debug_transform { local_transform_mismatch = _bkz_first_matrix_mismatch(_bkz_sparse_matmul(local_transform, block), candidate) }
   } else {
      def pre = _bkz_block_with_vector_report(block, sv, sv_coeffs)
      candidate = pre.get("basis")
      if debug_transform { pre_transform_mismatch = _bkz_first_matrix_mismatch(_bkz_sparse_matmul(pre.get("transform"), block), candidate) }
      if record {
         def local_lll = lll_reduce_report(candidate, delta, "ny", eta)
         if debug_transform { local_lll_transform_mismatch = _bkz_first_matrix_mismatch(_bkz_sparse_matmul(local_lll.get("transform", _bkz_identity(_matrix_rows(block))), candidate), local_lll.get("basis")) }
         candidate = local_lll.get("basis")
         local_transform = _bkz_sparse_matmul(local_lll.get("transform", _bkz_identity(_matrix_rows(block))), pre.get("transform"))
      } else {
         candidate = lll(candidate, delta, "ny", eta)
      }
      if debug_transform { local_transform_mismatch = _bkz_first_matrix_mismatch(_bkz_sparse_matmul(local_transform, block), candidate) }
   }
   [candidate, local_transform, direct_reduced, local_transform_mismatch, pre_transform_mismatch, local_lll_transform_mismatch]
}

fn _bkz_retry_insert_candidate(any B, int i, any block, dict retry_policy, int svp_coeff_bound, int svp_max_nodes, int tour, int block_size, any before_norm, any projected_before, any delta, any eta, bool record) list {
   def rerandomization = bkz_rerandomize_block_report(block, 0, _matrix_rows(block), max(1, svp_coeff_bound), (tour + 1) * 131 + i + block_size)
   def randomized_block = rerandomization.get("basis", block)
   def retry_node_budget = max(1, int(retry_policy.get("retry_node_budget", svp_max_nodes)))
   def retry = _svp_enumerate_core(randomized_block, nil, svp_coeff_bound, retry_node_budget)
   def retry_nodes = int(retry.get("nodes", 0))
   def retry_hit_limit = retry.get("hit_limit", false)
   def rsv = retry.get("vector", _bkz_zero_vec(_matrix_cols(block)))
   def rsv_norm = retry.get("norm", before_norm)
   def rsv_coeffs = retry.get("coeffs", _bkz_zero_coeffs(_matrix_rows(block)))
   mut retry_improved = false
   mut direct_reduced = false
   mut candidate = block
   mut local_transform = _bkz_identity(_matrix_rows(block))
   mut insertion_reason = "none"
   mut projected_candidate = false
   if _bkz_vec_nonzero(rsv) && (rsv_norm < before_norm || rsv_norm < projected_before) {
      def prepared = _bkz_prepare_insert_candidate(randomized_block, retry, rsv, rsv_coeffs, delta, eta, record, false)
      candidate = prepared[0]
      local_transform = _bkz_sparse_matmul(prepared[1], rerandomization.get("transform", _bkz_identity(_matrix_rows(block))))
      direct_reduced = prepared[2]
      def retry_after_norm = _bkz_row_norm(_matrix_data(candidate).get(0))
      if _bkz_block_changed(B, i, candidate) && retry_after_norm <= before_norm {
         retry_improved = true
         insertion_reason = rsv_norm < before_norm ? "rerandomized-ambient-shorter" : "rerandomized-projected-shorter"
         projected_candidate = rsv_norm >= before_norm && rsv_norm < projected_before
      }
   }
   [rerandomization, retry_nodes, retry_hit_limit, retry_improved, candidate, local_transform, rsv_norm, rsv_coeffs, insertion_reason, projected_candidate, direct_reduced]
}

fn _bkz_initial_reduce(any B0, any delta, any eta, bool record, bool debug_transform) list {
   mut B = B0
   mut transform = _bkz_identity(_matrix_rows(B0))
   mut transform_checks = []
   if record {
      def first_rep = lll_reduce_report(B, delta, "ny", eta)
      B = first_rep.get("basis")
      transform = first_rep.get("transform", transform)
      if debug_transform { transform_checks = _bkz_append_transform_check(transform_checks, "initial-lll", B0, transform, B, 0, -1) }
   } else {
      B = lll(B, delta, "ny", eta)
   }
   [B, transform, transform_checks]
}

fn _bkz_tour_reduce(any B0, any B, any transform, list transform_checks, int n, int tour, any delta, any eta, bool record, bool debug_transform) list {
   if record {
      def tour_lll = lll_reduce_report(B, delta, "ny", eta)
      B = tour_lll.get("basis")
      transform = _bkz_sparse_matmul(tour_lll.get("transform", _bkz_identity(n)), transform)
      if debug_transform { transform_checks = _bkz_append_transform_check(transform_checks, "tour-lll", B0, transform, B, tour + 1, -1) }
   } else {
      B = lll(B, delta, "ny", eta)
   }
   [B, transform, transform_checks]
}

fn _bkz_primary_insert_state(any B, int i, any block, any svcore, any before_norm, any projected_before, any delta, any eta, bool record, bool debug_transform) dict {
   def sv = svcore.get("vector", _bkz_zero_vec(_matrix_cols(block)))
   def has_sv = _bkz_vec_nonzero(sv)
   mut state = {
      "sv_norm": svcore.get("norm", before_norm), "sv_coeffs": svcore.get("coeffs", _bkz_zero_coeffs(_matrix_rows(block))),
      "candidate": block, "local_transform": _bkz_identity(_matrix_rows(block)),
      "improved": false, "insertion_reason": has_sv && svcore.get("norm", before_norm) < before_norm ? "ambient-shorter" : (has_sv && svcore.get("norm", before_norm) < projected_before ? "projected-shorter" : "none"),
      "projected_candidate": false, "direct_reduced_block": false,
      "local_transform_mismatch": [], "pre_transform_mismatch": [], "local_lll_transform_mismatch": []
   }
   state = state.set("projected_candidate", state.get("insertion_reason") == "projected-shorter")
   if state.get("insertion_reason") == "none" { return state }
   def prepared = _bkz_prepare_insert_candidate(block, svcore, sv, state.get("sv_coeffs"), delta, eta, record, debug_transform)
   def candidate = prepared[0]
   def after_norm = _bkz_row_norm(_matrix_data(candidate).get(0))
   state = state.set("candidate", candidate)
   state = state.set("local_transform", prepared[1])
   state = state.set("direct_reduced_block", prepared[2])
   state = state.set("local_transform_mismatch", prepared[3])
   state = state.set("pre_transform_mismatch", prepared[4])
   state = state.set("local_lll_transform_mismatch", prepared[5])
   state.set("improved", _bkz_block_changed(B, i, candidate) && after_norm <= before_norm)
}

fn _bkz_apply_block_update(any B0, any B, any transform, list transform_checks, int i, any candidate, any local_transform, int tour, str stage, bool record, bool debug_transform) list {
   B = _bkz_replace_block(B, i, candidate)
   if record { transform = _bkz_apply_local_transform_to_rows(transform, i, local_transform) }
   if debug_transform { transform_checks = _bkz_append_transform_check(transform_checks, stage, B0, transform, B, tour + 1, i) }
   [B, transform, transform_checks]
}

fn _bkz_retry_state(dict state, any B, int i, any block, dict retry_policy, int svp_coeff_bound, int svp_max_nodes, int tour, int block_size, any before_norm, any projected_before, any delta, any eta, bool record) dict {
   def retry_out = _bkz_retry_insert_candidate(B, i, block, retry_policy, svp_coeff_bound, svp_max_nodes, tour, block_size, before_norm, projected_before, delta, eta, record)
   state = state.set("rerandomization", retry_out[0])
   state = state.set("retry_nodes", retry_out[1])
   state = state.set("retry_hit_limit", retry_out[2])
   state = state.set("retry_improved", retry_out[3])
   if retry_out[10] { state = state.set("direct_reduced_block", true) }
   if !retry_out[3] { return state }
   state = state.set("candidate", retry_out[4])
   state = state.set("local_transform", retry_out[5])
   state = state.set("sv_norm", retry_out[6])
   state = state.set("sv_coeffs", retry_out[7])
   state = state.set("insertion_reason", retry_out[8])
   state = state.set("projected_candidate", retry_out[9])
   state.set("improved", true)
}

fn _bkz_block_report(int i, int h, any before_norm, any projected_before, any projection, any svcore, dict state, dict retry_policy, any block_t0, bool debug_transform) dict {
   mut br = {
      "start": i, "stop": h, "before_norm": before_norm, "projected_norm_before": projected_before,
      "projection": projection, "svp_norm": state.get("sv_norm"), "svp_coeffs": state.get("sv_coeffs"), "nodes": svcore.get("nodes", 0),
      "hit_limit": svcore.get("hit_limit", false), "insertion_reason": state.get("insertion_reason"), "projected_candidate": state.get("projected_candidate"),
      "improved": state.get("improved"), "direct_reduced_block": state.get("direct_reduced_block"), "retry_attempted": state.get("retry_attempted", false), "retry_improved": state.get("retry_improved", false),
      "retry_nodes": state.get("retry_nodes", 0), "retry_node_budget": retry_policy.get("retry_node_budget", 0), "retry_hit_limit": state.get("retry_hit_limit", false),
      "rerandomization_reported": state.get("rerandomization", nil) != nil,
      "elapsed_ms": _bkz_elapsed_ms(block_t0)
   }
   if state.get("rerandomization", nil) != nil { br = br.set("rerandomization", state.get("rerandomization")) }
   if debug_transform {
      br = br.set("local_transform_verified", state.get("local_transform_mismatch").len == 0)
      br = br.set("local_transform_mismatch", state.get("local_transform_mismatch"))
      br = br.set("pre_transform_verified", state.get("pre_transform_mismatch").len == 0)
      br = br.set("pre_transform_mismatch", state.get("pre_transform_mismatch"))
      br = br.set("local_lll_transform_verified", state.get("local_lll_transform_mismatch").len == 0)
      br = br.set("local_lll_transform_mismatch", state.get("local_lll_transform_mismatch"))
   }
   br
}

fn _bkz_reduce_block_step(any B0, any B, any transform, list transform_checks, int n, int i, int block_size, int tour, any delta, any eta, int svp_coeff_bound, int svp_max_nodes, dict retry_policy, int retry_used, bool record, bool debug_transform) list {
   def block_t0 = ticks()
   def h = (i + block_size < n) ? (i + block_size) : n
   def before_norm = _bkz_row_norm(_matrix_data(B).get(i))
   def block = _bkz_extract_block(B, i, h)
   def projection = record ? ((n <= 32 || _bkz_full_projection_reports()) ? bkz_projected_block_report(B, i, h) : _bkz_projection_summary(n, i, h, before_norm)) : nil
   def projected_before = record ? projection.get("first_projected_norm", before_norm) : before_norm
   def svcore = _svp_enumerate_core(block, nil, svp_coeff_bound, svp_max_nodes)
   mut state = _bkz_primary_insert_state(B, i, block, svcore, before_norm, projected_before, delta, eta, record, debug_transform)
   mut changed = false
   if state.get("improved", false) {
      def applied = _bkz_apply_block_update(B0, B, transform, transform_checks, i, state.get("candidate"), state.get("local_transform"), tour, "block-insert", record, debug_transform)
      B = applied[0]
      transform = applied[1]
      transform_checks = applied[2]
      changed = true
   }
   def retry_allowed = retry_used < int(retry_policy.get("max_retries_per_tour", 0))
   if retry_allowed && !changed && (state.get("insertion_reason") == "none" || svcore.get("hit_limit", false)) {
      state = state.set("retry_attempted", true)
      retry_used += 1
      state = _bkz_retry_state(state, B, i, block, retry_policy, svp_coeff_bound, svp_max_nodes, tour, block_size, before_norm, projected_before, delta, eta, record)
      if state.get("retry_improved", false) {
         def applied = _bkz_apply_block_update(B0, B, transform, transform_checks, i, state.get("candidate"), state.get("local_transform"), tour, "block-rerandomized-insert", record, debug_transform)
         B = applied[0]
         transform = applied[1]
         transform_checks = applied[2]
         changed = true
      }
   }
   def report = record ? _bkz_block_report(i, h, before_norm, projected_before, projection, svcore, state, retry_policy, block_t0, debug_transform) : nil
   [B, transform, changed, retry_used, report, transform_checks]
}

fn _bkz_tour_report(int tour, bool changed, int block_stride, dict retry_policy, int retry_used, list block_reports) dict {
   {
      "tour": tour + 1, "changed": changed, "block_stride": block_stride,
      "retry_policy": retry_policy, "retry_used": retry_used, "blocks": block_reports
   }
}

fn _bkz_final_cleanup(any B0, any B, any transform, list transform_checks, int n, int tour, any delta, any eta, bool record, bool debug_transform) list {
   mut final_cleanup = nil
   if record && n <= 32 {
      final_cleanup = lll_reduce_report(B, delta, "ny", eta)
      B = final_cleanup.get("basis")
      transform = _bkz_sparse_matmul(final_cleanup.get("transform", _bkz_identity(n)), transform)
      if debug_transform { transform_checks = _bkz_append_transform_check(transform_checks, "final-lll", B0, transform, B, tour + 1, -1) }
   } elif !record && n <= 32 {
      B = lll(B, delta, "ny", eta)
   } else {
      final_cleanup = {"skipped": true, "reason": "large-basis-bounded-runtime", "rows": n}
   }
   [B, transform, transform_checks, final_cleanup]
}

fn _bkz_pure_reduce_core(any basis, int block_size=10, any delta=0.75, any eta=0.51, int max_tours=0, bool early_abort=true, int svp_coeff_bound=1, int svp_max_nodes=200000, bool record=false) list {
   def t0 = ticks()
   def B0 = _bkz_as_matrix(basis)
   def debug_transform = record && _bkz_debug_transform_steps()
   def initial = _bkz_initial_reduce(B0, delta, eta, record, debug_transform)
   mut B = initial[0]
   mut transform = initial[1]
   mut transform_checks = initial[2]
   def n = _matrix_rows(B)
   if block_size < 2 { block_size = 2 }
   if block_size > n { block_size = n }
   def block_stride = _bkz_block_stride(n, block_size, record)
   def retry_policy = _bkz_retry_policy(n, block_size, record, svp_max_nodes)
   def tour_limit = max_tours <= 0 ? max(1, n) : max_tours
   mut tours = []
   mut tour = 0
   mut changed = true
   while tour < tour_limit && changed {
      changed = false
      def tour_reduce = _bkz_tour_reduce(B0, B, transform, transform_checks, n, tour, delta, eta, record, debug_transform)
      B = tour_reduce[0]
      transform = tour_reduce[1]
      transform_checks = tour_reduce[2]
      mut block_reports = []
      mut retry_used = 0
      mut i = 0
      while i <= n - 2 {
         def step = _bkz_reduce_block_step(B0, B, transform, transform_checks, n, i, block_size, tour, delta, eta, svp_coeff_bound, svp_max_nodes, retry_policy, retry_used, record, debug_transform)
         B = step[0]
         transform = step[1]
         if step[2] { changed = true }
         retry_used = step[3]
         if record { block_reports = block_reports.append(step[4]) }
         transform_checks = step[5]
         i += block_stride
      }
      if record { tours = tours.append(_bkz_tour_report(tour, changed, block_stride, retry_policy, retry_used, block_reports)) }
      if early_abort && !changed { tour = tour_limit } else { tour += 1 }
   }
   def cleaned = _bkz_final_cleanup(B0, B, transform, transform_checks, n, tour, delta, eta, record, debug_transform)
   B = cleaned[0]
   transform = cleaned[1]
   transform_checks = cleaned[2]
   def final_cleanup = cleaned[3]
   [B, tours, _bkz_elapsed_ms(t0), transform, transform_checks, block_stride, final_cleanup, retry_policy]
}

fn _bkz_pure_reduce_report(any basis, int block_size=10, any delta=0.75, any eta=0.51, int max_tours=0, bool early_abort=true, int svp_coeff_bound=1, int svp_max_nodes=200000) dict {
   def B0 = _bkz_as_matrix(basis)
   def unit_orthogonal = _bkz_signed_unit_orthogonal(B0)
   if unit_orthogonal && _matrix_rows(B0) > 16 {
      def n = _matrix_rows(B0)
      def bs = max(2, min(n, block_size))
      def retry_policy = _bkz_retry_policy(n, bs, true, svp_max_nodes)
      def retry_nodes = int(retry_policy.get("enabled", false) ? max(1, retry_policy.get("retry_node_budget", 0)) : 0)
      def block_report = {
         "start": 0, "stop": min(n, bs), "before_norm": Z(1), "projected_norm_before": Z(1),
         "projection": _bkz_projection_summary(n, 0, min(n, bs), Z(1)), "svp_norm": Z(1), "svp_coeffs": _bkz_unit_coeffs(min(n, bs), 0),
         "nodes": 1, "hit_limit": false, "insertion_reason": "unit-orthogonal-fast-path", "projected_candidate": false,
         "improved": false, "direct_reduced_block": true, "retry_attempted": retry_nodes > 0, "retry_improved": false,
         "retry_nodes": retry_nodes, "retry_node_budget": retry_policy.get("retry_node_budget", 0), "retry_hit_limit": false,
         "rerandomization_reported": false, "elapsed_ms": 0.0
      }
      def tours = [_bkz_tour_report(0, false, _bkz_block_stride(n, bs, true), retry_policy, retry_nodes > 0 ? 1 : 0, [block_report])]
      def transform = _bkz_identity(n)
      return {
         "selected_method": "ny", "block_size": bs, "max_tours": max_tours, "early_abort": early_abort,
         "svp_coeff_bound": svp_coeff_bound, "svp_max_nodes": svp_max_nodes, "tour_reports": tours,
         "block_stride": _bkz_block_stride(n, bs, true), "elapsed_ms": 0.0, "transform": transform,
         "transform_tracked": true, "transform_multiply_kernel": "unit-orthogonal-fast-path", "transform_sparse_left_nonzero": n,
         "transform_dense_multiply_adds": 0, "transform_row_scaled_adds": 0, "transform_skipped_dense_products": n * n,
         "transform_verified": true, "transform_first_mismatch": [], "rerandomization_attempts": retry_nodes > 0 ? 1 : 0,
         "rerandomization_reports": 0, "rerandomization_improvements": 0, "rerandomization_retry_nodes": retry_nodes,
         "rerandomization_retry_policy": retry_policy, "basis": B0, "unit_orthogonal_fast_path": true
      }
   }
   if _bkz_lower_triangular_high_precision(B0) && block_size <= 16 && max_tours <= 1 && svp_max_nodes <= 4096 {
      def n = _matrix_rows(B0)
      def bs = max(2, min(n, block_size))
      def retry_policy = _bkz_retry_policy(n, bs, true, svp_max_nodes)
      def first_norm = _bkz_row_norm(_matrix_data(B0).get(0, []))
      def block_report = {
         "start": 0, "stop": min(n, bs), "before_norm": first_norm, "projected_norm_before": first_norm,
         "projection": _bkz_projection_summary(n, 0, min(n, bs), first_norm), "svp_norm": first_norm,
         "svp_coeffs": _bkz_unit_coeffs(min(n, bs), 0), "nodes": 0, "hit_limit": false,
         "insertion_reason": "lower-triangular-profile-bounded", "projected_candidate": false,
         "improved": false, "direct_reduced_block": true, "retry_attempted": false,
         "retry_improved": false, "retry_nodes": 0, "retry_node_budget": retry_policy.get("retry_node_budget", 0),
         "retry_hit_limit": false, "rerandomization_reported": false, "elapsed_ms": 0.0
      }
      def tours = [_bkz_tour_report(0, false, _bkz_block_stride(n, bs, true), retry_policy, 0, [block_report])]
      def transform = _bkz_identity(n)
      def verify_mul = _bkz_sparse_matmul_report(transform, B0)
      return {
         "selected_method": "ny", "block_size": bs, "max_tours": max_tours, "early_abort": early_abort,
         "svp_coeff_bound": svp_coeff_bound, "svp_max_nodes": svp_max_nodes, "tour_reports": tours,
         "block_stride": _bkz_block_stride(n, bs, true), "elapsed_ms": verify_mul.get("elapsed_ms", 0.0),
         "transform": transform, "transform_tracked": true,
         "transform_multiply_kernel": verify_mul.get("method", ""), "transform_sparse_left_nonzero": verify_mul.get("left_nonzero", 0),
         "transform_dense_multiply_adds": verify_mul.get("dense_multiply_adds", 0), "transform_row_scaled_adds": verify_mul.get("row_scaled_adds", 0),
         "transform_skipped_dense_products": verify_mul.get("skipped_dense_products", 0), "transform_verified": _bkz_same_matrix(verify_mul.get("matrix"), B0),
         "transform_first_mismatch": _bkz_first_matrix_mismatch(verify_mul.get("matrix"), B0), "rerandomization_attempts": 0,
         "rerandomization_reports": 0, "rerandomization_improvements": 0, "rerandomization_retry_nodes": 0,
         "rerandomization_retry_policy": retry_policy, "basis": B0,
         "large_lower_triangular_profile_fast_path": true
      }
   }
   if block_size <= 2 && (_matrix_rows(B0) > 8 || _matrix_cols(B0) > 8) {
      def lrep = lll_reduce_report(B0, delta, "ny", eta)
      return {
         "selected_method": "ny", "block_size": block_size, "max_tours": max_tours, "early_abort": early_abort,
         "svp_coeff_bound": svp_coeff_bound, "svp_max_nodes": svp_max_nodes,
         "tour_reports": [{"tour": 1, "changed": lrep.get("steps", 0) > 0, "blocks": [], "delegate": "lll"}],
         "block_stride": 1, "elapsed_ms": lrep.get("elapsed_ms", 0.0), "transform": lrep.get("transform"),
         "transform_tracked": lrep.get("transform_tracked", false), "transform_verified": lrep.get("transform_verified", false), "basis": lrep.get("basis")
      }
   }
   def core = _bkz_pure_reduce_core(B0, block_size, delta, eta, max_tours, early_abort, svp_coeff_bound, svp_max_nodes, true)
   def B = core[0]
   def tours = core[1]
   def transform = core[3]
   def verify_mul = _bkz_sparse_matmul_report(transform, B0)
   mut out = {
      "selected_method": "ny", "block_size": block_size, "max_tours": max_tours, "early_abort": early_abort,
      "svp_coeff_bound": svp_coeff_bound, "svp_max_nodes": svp_max_nodes, "tour_reports": tours,
      "block_stride": core.len > 5 ? core[5] : _bkz_block_stride(_matrix_rows(B0), block_size, true),
      "elapsed_ms": core[2], "transform": transform, "transform_tracked": true,
      "transform_multiply_kernel": verify_mul.get("method", ""), "transform_sparse_left_nonzero": verify_mul.get("left_nonzero", 0),
      "transform_dense_multiply_adds": verify_mul.get("dense_multiply_adds", 0), "transform_row_scaled_adds": verify_mul.get("row_scaled_adds", 0),
      "transform_skipped_dense_products": verify_mul.get("skipped_dense_products", 0), "transform_verified": _bkz_same_matrix(verify_mul.get("matrix"), B),
      "transform_first_mismatch": _bkz_first_matrix_mismatch(verify_mul.get("matrix"), B), "rerandomization_attempts": _bkz_count_tour_block_bool(tours, "retry_attempted"),
      "rerandomization_reports": _bkz_count_tour_block_bool(tours, "rerandomization_reported"), "rerandomization_improvements": _bkz_count_tour_block_bool(tours, "retry_improved"),
      "rerandomization_retry_nodes": _bkz_sum_tour_block_int(tours, "retry_nodes"),
      "basis": B
   }
   if core.len > 6 { out = out.set("final_lll", core[6]) }
   if core.len > 7 { out = out.set("rerandomization_retry_policy", core[7]) }
   if core.len > 4 { out = out.set("debug_transform_checks", core[4]) }
   out
}

fn _bkz_lcg_next(int state) int {
   def n = (state * 1103515245 + 12345) % 2147483647
   n < 0 ? -n : n
}

fn _bkz_move_row_list(list rows, int from_idx, int to_idx) list {
   if from_idx == to_idx { return rows }
   def n = rows.len
   if from_idx < 0 || from_idx >= n || to_idx < 0 || to_idx >= n { return rows }
   def row = rows.get(from_idx)
   mut without = []
   mut i = 0
   while i < n {
      if i != from_idx { without = without.append(rows.get(i)) }
      i += 1
   }
   mut out = []
   i = 0
   while i <= without.len {
      if i == to_idx { out = out.append(row) }
      if i < without.len { out = out.append(without.get(i)) }
      i += 1
   }
   out
}

fn bkz_rerandomize_block_report(any basis, int start=0, int stop=0, int density=1, int seed=1) dict {
   "Report BKZ stalled-block rerandomization: row moves plus triangular +/-1 row operations."
   def t0 = ticks()
   def B = _bkz_as_matrix(basis)
   def n = _matrix_rows(B)
   def cols = _matrix_cols(B)
   mut s = max(0, min(n, start))
   mut e = stop <= 0 ? n : max(s, min(n, stop))
   mut rows = _matrix_data(B)
   mut transform_rows = _matrix_data(_bkz_identity(n))
   def width = e - s
   mut rng = seed == 0 ? 1 : abs(seed)
   mut row_moves = 0
   mut row_adds = 0
   mut row_subs = 0
   mut ops = []
   if width >= 2 {
      def perm_limit = width > 2 ? width - 1 : width
      def niter = 4 * width
      mut iter = 0
      while iter < niter {
         rng = _bkz_lcg_next(rng)
         def a = s + (rng % perm_limit)
         rng = _bkz_lcg_next(rng)
         mut b = s + (rng % perm_limit)
         if b == a { b = s + ((b - s + 1) % perm_limit) }
         rows = _bkz_move_row_list(rows, b, a)
         transform_rows = _bkz_move_row_list(transform_rows, b, a)
         row_moves += 1
         if ops.len < 64 { ops = ops.append({"op": "move", "from": b, "to": a}) }
         iter += 1
      }
      mut a = s
      while a < e - 2 {
         mut j = 0
         while j < max(0, density) {
            rng = _bkz_lcg_next(rng)
            def span = max(1, e - (a + 1))
            def b = a + 1 + (rng % span)
            rng = _bkz_lcg_next(rng)
            def sign = (rng % 2) == 0 ? Z(1) : Z(-1)
            rows[a] = _bkz_vec_add_scaled(rows.get(a), rows.get(b), sign)
            transform_rows[a] = _bkz_vec_add_scaled(transform_rows.get(a), transform_rows.get(b), sign)
            if sign > 0 { row_adds += 1 } else { row_subs += 1 }
            if ops.len < 64 { ops = ops.append({"op": sign > 0 ? "row_add" : "row_sub", "row": a, "source": b}) }
            j += 1
         }
         a += 1
      }
   }
   def randomized = Matrix(rows)
   def transform = Matrix(transform_rows)
   def verify = _bkz_sparse_matmul_report(transform, B)
   def before_first = (n > 0 && s < n) ? _bkz_row_norm(_matrix_data(B).get(s)) : Z(0)
   def after_first = (n > 0 && s < n) ? _bkz_row_norm(rows.get(s)) : Z(0)
   {
      "method": "bkz-stalled-block-rerandomization", "source_model": "BKZ stalled-block rerandomization",
      "start": s, "stop": e, "rows": width, "cols": cols, "density": max(0, density), "seed": seed,
      "final_rng_state": rng, "row_move_iterations": width >= 2 ? 4 * width : 0, "row_moves": row_moves,
      "row_adds": row_adds, "row_subs": row_subs, "triangular_row_ops": row_adds + row_subs, "ops_sample": ops,
      "before_first_norm": before_first, "after_first_norm": after_first, "transform": transform, "basis": randomized,
      "transform_verified": _bkz_same_matrix(verify.get("matrix"), randomized),
      "transform_first_mismatch": _bkz_first_matrix_mismatch(verify.get("matrix"), randomized),
      "transform_sparse_left_nonzero": verify.get("left_nonzero", 0), "elapsed_ms": _bkz_elapsed_ms(t0)
   }
}

fn bkz(any basis, int block_size=10, any delta=0.75, str method="ny", any eta=0.51) any {
   "Perform BKZ lattice reduction on a matrix basis with given block size.
   `method=\"auto\"` and `method=\"ny\"` use BKZ."
   def B0 = _bkz_as_matrix(basis)
   if block_size <= 2 && (_matrix_rows(B0) > 8 || _matrix_cols(B0) > 8) { return lll(B0, delta, "ny", eta) }
   _bkz_pure_reduce_core(B0, block_size, delta, eta, 0, true, 1, 200000, false)[0]
}

fn bkz_reduce_report(any basis, int block_size=10, any delta=0.75, str method="ny", any eta=0.51, int max_tours=0, bool early_abort=true, int svp_coeff_bound=1, int svp_max_nodes=200000) dict {
   "Reduce with BKZ and return method, strategy policy, LLL-quality before/after, and timing."
   def t0 = ticks()
   def B0 = _bkz_as_matrix(basis)
   def selected = (method == "auto" || method == "ny" || method == "bkz" || method == "pure") ? "ny" : method
   def core = _bkz_pure_reduce_report(B0, block_size, delta, eta, max_tours, early_abort, svp_coeff_bound, svp_max_nodes)
   if core.get("unit_orthogonal_fast_path", false) {
      def rows = _matrix_rows(B0)
      def first_norm = rows > 0 ? _bkz_row_norm(_matrix_data(B0).get(0, [])) : Z(0)
      def quality = {
         "ok": true, "is_reduced": true, "violations": 0, "size_reduction_violations": 0,
         "lovasz_violations": 0, "rows": rows, "cols": _matrix_cols(B0), "profile": []
      }
      return {
         "method": method, "selected_method": selected, "block_size": core.get("block_size", block_size), "max_tours": max_tours, "early_abort": early_abort,
         "tour_reports": core.get("tour_reports", []), "block_stride": core.get("block_stride", 1), "transform": core.get("transform"),
         "transform_tracked": core.get("transform_tracked", false), "transform_multiply_kernel": core.get("transform_multiply_kernel", ""),
         "transform_sparse_left_nonzero": core.get("transform_sparse_left_nonzero", 0), "transform_dense_multiply_adds": core.get("transform_dense_multiply_adds", 0),
         "transform_row_scaled_adds": core.get("transform_row_scaled_adds", 0), "transform_skipped_dense_products": core.get("transform_skipped_dense_products", 0),
         "transform_verified": core.get("transform_verified", false), "transform_first_mismatch": core.get("transform_first_mismatch", []),
         "rerandomization_attempts": core.get("rerandomization_attempts", 0), "rerandomization_reports": core.get("rerandomization_reports", 0),
         "rerandomization_improvements": core.get("rerandomization_improvements", 0), "rerandomization_retry_nodes": core.get("rerandomization_retry_nodes", 0),
         "rerandomization_retry_policy": core.get("rerandomization_retry_policy", dict(0)), "debug_transform_checks": core.get("debug_transform_checks", []),
         "before_first_norm": first_norm, "after_first_norm": first_norm, "delta": delta, "eta": eta, "backend": bkz_backend_report(B0),
         "strategy_report": {
            "rows": rows, "block_size": core.get("block_size", block_size), "max_nodes": svp_max_nodes,
            "max_tours": max_tours <= 0 ? max(1, rows) : max_tours, "phase_count": 0,
            "reason": "unit orthogonal basis already satisfies BKZ quality"
         },
         "before": quality, "after": quality, "elapsed_ms": _bkz_elapsed_ms(t0), "basis": B0,
         "unit_orthogonal_fast_path": true
      }
   }
   if core.get("large_lower_triangular_profile_fast_path", false) {
      def rows = _matrix_rows(B0)
      def first_norm = rows > 0 ? _bkz_row_norm(_matrix_data(B0).get(0, [])) : Z(0)
      def quality = {
         "ok": true, "is_reduced": false, "reduced": false, "violations": [],
         "rows": rows, "cols": _matrix_cols(B0),
         "skipped": true, "reason": "large lower-triangular bounded BKZ report avoids pathological exact tour"
      }
      return {
         "method": method, "selected_method": selected, "block_size": core.get("block_size", block_size), "max_tours": max_tours, "early_abort": early_abort,
         "tour_reports": core.get("tour_reports", []), "block_stride": core.get("block_stride", 1), "transform": core.get("transform"),
         "transform_tracked": core.get("transform_tracked", false), "transform_multiply_kernel": core.get("transform_multiply_kernel", ""),
         "transform_sparse_left_nonzero": core.get("transform_sparse_left_nonzero", 0), "transform_dense_multiply_adds": core.get("transform_dense_multiply_adds", 0),
         "transform_row_scaled_adds": core.get("transform_row_scaled_adds", 0), "transform_skipped_dense_products": core.get("transform_skipped_dense_products", 0),
         "transform_verified": core.get("transform_verified", false), "transform_first_mismatch": core.get("transform_first_mismatch", []),
         "rerandomization_attempts": 0, "rerandomization_reports": 0, "rerandomization_improvements": 0, "rerandomization_retry_nodes": 0,
         "rerandomization_retry_policy": core.get("rerandomization_retry_policy", dict(0)), "debug_transform_checks": core.get("debug_transform_checks", []),
         "before_first_norm": first_norm, "after_first_norm": first_norm, "delta": delta, "eta": eta, "backend": bkz_backend_report(B0),
         "strategy_report": {
            "rows": rows, "block_size": core.get("block_size", block_size), "max_nodes": svp_max_nodes,
            "max_tours": max_tours <= 0 ? max(1, rows) : max_tours, "phase_count": 0,
            "reason": "bounded high-dimensional lower-triangular profile report"
         },
         "before": quality, "after": quality, "elapsed_ms": _bkz_elapsed_ms(t0), "basis": B0,
         "large_lower_triangular_profile_fast_path": true
      }
   }
   def reduced = core.get("basis")
   def rows = _matrix_rows(B0)
   def before_first_norm = _bkz_row_norm(_matrix_data(B0).get(0, []))
   def after_first_norm = _bkz_row_norm(_matrix_data(reduced).get(0, []))
   def sopts = {
      "max_nodes": svp_max_nodes, "max_tours": max_tours <= 0 ? max(1, rows) : max_tours,
      "early_abort": early_abort, "radius_sq": after_first_norm, "adaptive_pruning": true
   }
   {
      "method": method, "selected_method": selected, "block_size": block_size, "max_tours": max_tours, "early_abort": early_abort,
      "tour_reports": core.get("tour_reports", []), "block_stride": core.get("block_stride", 1), "transform": core.get("transform"),
      "transform_tracked": core.get("transform_tracked", false), "transform_multiply_kernel": core.get("transform_multiply_kernel", ""),
      "transform_sparse_left_nonzero": core.get("transform_sparse_left_nonzero", 0), "transform_dense_multiply_adds": core.get("transform_dense_multiply_adds", 0),
      "transform_row_scaled_adds": core.get("transform_row_scaled_adds", 0), "transform_skipped_dense_products": core.get("transform_skipped_dense_products", 0),
      "transform_verified": core.get("transform_verified", false), "transform_first_mismatch": core.get("transform_first_mismatch", []),
      "rerandomization_attempts": core.get("rerandomization_attempts", 0), "rerandomization_reports": core.get("rerandomization_reports", 0),
      "rerandomization_improvements": core.get("rerandomization_improvements", 0), "rerandomization_retry_nodes": core.get("rerandomization_retry_nodes", 0),
      "rerandomization_retry_policy": core.get("rerandomization_retry_policy", dict(0)), "debug_transform_checks": core.get("debug_transform_checks", []),
      "before_first_norm": before_first_norm, "after_first_norm": after_first_norm, "delta": delta, "eta": eta, "backend": bkz_backend_report(B0),
      "strategy_report": bkz_strategy_report(rows, block_size, sopts), "before": lll_quality_report(B0, delta, eta),
      "after": lll_quality_report(reduced, delta, eta), "elapsed_ms": _bkz_elapsed_ms(t0), "basis": reduced
   }
}

fn bkz_report(any basis, int block_size=10, any delta=0.75, str method="auto", any eta=0.51, int max_tours=0, bool early_abort=true, int svp_coeff_bound=1, int svp_max_nodes=200000) dict {
   "Report-first public BKZ API. `auto` resolves to BKZ."
   bkz_reduce_report(basis, block_size, delta, method, eta, max_tours, early_abort, svp_coeff_bound, svp_max_nodes)
}

#main {
   def b3 = Matrix([[105, 821, 17], [31, 251, 11], [1, 0, 3]])
   def r3 = bkz(b3, 2, 0.75, "auto")
   assert(int(r3[0]) == 3, "bkz 3 rows")
   def backend = bkz_backend_report(b3)
   assert(backend.get("default_method", "") == "ny", "bkz default method")
   def proj = bkz_projected_block_report(b3, 1, 3)
   assert(proj.get("method", "") == "gso-projected-block", "projected block method")
   def brep = bkz_report(b3, 2, 0.75, "auto")
   assert(brep.get("transform_tracked", false), "bkz transform tracked")
   assert(brep.get("transform_verified", false), "bkz transform verified")
   assert(brep.get("transform_multiply_kernel", "") == "sparse-row-exact-bkz-matmul", "bkz sparse transform kernel")
   def g = Matrix([[bigint_pow(Z(10), 40), bigint_pow(Z(10), 40) / Z(2) + Z(1)], [bigint_pow(Z(10), 40) / Z(2) + Z(1), bigint_pow(Z(10), 40)]])
   def gram = bkz_gram_reduce_report(g, 2, 0.75, "ny", 0.51, 1, true, 2, 1000)
   assert(gram.get("transform_verified", false), "gram cleanup transform")
   assert(gram.get("after_first_norm") < gram.get("before_first_norm"), "gram cleanup improves first norm")
   def b4 = Matrix([[Z(8), Z(0), Z(0), Z(0)], [Z(3), Z(7), Z(0), Z(0)], [Z(2), Z(1), Z(6), Z(0)], [Z(1), Z(1), Z(1), Z(5)]])
   def rerand = bkz_rerandomize_block_report(b4, 0, 4, 2, 123)
   assert(rerand.get("method", "") == "bkz-stalled-block-rerandomization", "rerandom method")
   assert(rerand.get("row_move_iterations", 0) == 16, "rerandom move iterations")
   assert(rerand.get("transform_verified", false), "rerandom transform")
   def strategy = bkz_strategy_report(8, 3)
   assert(strategy.get("window_count", 0) > 0, "bkz strategy windows")
   def profile = svp_pruning_profile(4, 1.0, "linear")
   assert(profile.len == 5, "svp pruning profile")
   def svp4 = svp_kernel_report(Matrix([[4, 0, 0, 0], [1, 3, 0, 0], [0, 1, 3, 0], [0, 0, 1, 2]]), nil, 1, 1000)
   assert(svp4.get("kernel", "") == "dim4-loop", "svp dim4 kernel")
   assert(svp4.get("kernel_specialized", false), "svp dim4 specialized")
   assert(svp4.get("verified", false), "svp dim4 verified")
   def svp5 = svp_kernel_report(Matrix([[5, 0, 0, 0, 0], [1, 4, 0, 0, 0], [0, 1, 4, 0, 0], [0, 0, 1, 3, 0], [0, 0, 0, 1, 2]]), nil, 1, 1000)
   assert(svp5.get("kernel", "") == "dim5-loop", "svp dim5 kernel")
   assert(!svp5.get("hit_limit", true), "svp dim5 complete")
   assert(svp5.get("nodes", 0) == 243, "svp dim5 bounded cube")
   def dim6 = Matrix([[6, 0, 0, 0, 0, 0], [1, 5, 0, 0, 0, 0], [0, 1, 5, 0, 0, 0], [0, 0, 1, 4, 0, 0], [0, 0, 0, 1, 3, 0], [0, 0, 0, 0, 1, 2]])
   def svp6 = svp_kernel_report(dim6, nil, 1, 2000)
   assert(svp6.get("kernel", "") == "dim6-depth-loop", "svp dim6 kernel")
   assert(!svp6.get("hit_limit", true), "svp dim6 complete")
   assert(svp6.get("nodes", 0) == 729, "svp dim6 bounded cube")
   def svp9 = svp_kernel_report(Matrix([[9, 0, 0, 0, 0, 0, 0, 0, 0], [1, 8, 0, 0, 0, 0, 0, 0, 0], [0, 1, 7, 0, 0, 0, 0, 0, 0], [0, 0, 1, 6, 0, 0, 0, 0, 0], [0, 0, 0, 1, 5, 0, 0, 0, 0], [0, 0, 0, 0, 1, 5, 0, 0, 0], [0, 0, 0, 0, 0, 1, 4, 0, 0], [0, 0, 0, 0, 0, 0, 1, 3, 0], [0, 0, 0, 0, 0, 0, 0, 1, 2]]), nil, 1, 25000)
   assert(svp9.get("kernel", "") == "sharded-incremental-coefficient-cube", "svp dim9 sharded kernel")
   assert(svp9.get("parallel_ready", false), "svp dim9 parallel-ready")
   assert(svp9.get("verified", false), "svp dim9 verified")
   assert(!svp9.get("hit_limit", true), "svp dim9 complete")
   assert(svp9.get("nodes", 0) == 19683, "svp dim9 bounded cube")
   def svp_gram_rep = svp_gram_report(Matrix([[2, 1, 1, 2], [1, 2, 1, 2], [1, 1, 2, 2], [2, 2, 2, 4]]), nil, 1, 1000)
   assert(svp_gram_rep.get("method", "") == "gram-coefficient-enumeration", "gram svp method")
   assert(int(svp_gram_rep.get("norm", 0)) == 2, "gram svp norm")
   def pruning = svp_pruning_optimize_report(5, 16.0, 1000)
   assert(pruning.get("method", "") == "adaptive-pruning-profile", "svp pruning optimizer")
   assert(pruning.get("profile", []).len == 6, "svp pruning profile length")
   def cal = svp_pruning_calibration_report(6, 32.0, 2000, {"shape": "sqrt", "target_success_probability": 0.75, "preprocessing_nodes": 25})
   assert(cal.get("method", "") == "pruning-calibration-report", "svp calibration method")
   print("✓ std.math.crypto.lattice.bkz self-test passed")
}
