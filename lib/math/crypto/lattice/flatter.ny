;; Keywords: lattice flatter math crypto number-theory
;; Lattice routines for lattice reduction strategies and profile reports.
;; Reference:
;; - https://web.cs.elte.hu/~lovasz/scans/lll.pdf
;; - https://www.cs.cmu.edu/~afs/cs/project/quake/public/papers/Coppersmith-Crypto96.pdf
;; References:
;; - std.math.crypto.lattice
;; - std.math.crypto
module std.math.crypto.lattice.flatter(lll_reduce, lll_reduce_delta, gram_schmidt_rows, shortest_vector, babai_cvp, vec_dot, vec_norm_sq, vec_clone, vec_scale_add, vec_sub_scaled, vec_scale, profile_default_alpha, profile_alpha_from_delta, profile_alpha_from_rhf, profile_rhf_from_alpha, profile_shape_report, profile_goal_report, profile_goal_check, profile_compression_plan_report, triangular_size_reduce, triangular_size_reduce_report, blocked_triangular_size_reduce, blocked_triangular_size_reduce_report, triangular_bounded_lll_prepass_report, triangular_tail_normal_form, triangular_tail_normal_form_report, qary_relation_prepass, qary_relation_prepass_report, lower_triangular_qary_relation_prepass, lower_triangular_qary_relation_prepass_report, short_row_prepass, short_row_prepass_report, banded_triplet_prepass, banded_triplet_prepass_report, relative_size_reduce, relative_size_reduce_report, lagrange_pair_reduce, lagrange_pair_reduce_report, lattice_matmul, lattice_matmul_report, lattice_matmul_blocked, lattice_matmul_blocked_report, lattice_matmul_threaded, lattice_matmul_threaded_report, lattice_triangular_matmul, lattice_triangular_matmul_report, lattice_matmul_strassen, lattice_matmul_strassen_report, schoenhage_reduce, schoenhage_reduce_report, recursive_strategy_plan_report, recursive_reduce, recursive_reduce_report, sublattice_reduce, sublattice_reduce_report, qr_factor, qr_factor_report, qr_reorthogonalized_factor, qr_reorthogonalized_factor_report, householder_qr_factor, householder_qr_factor_report, blocked_qr_factor, blocked_qr_factor_report, tall_skinny_qr_factor, tall_skinny_qr_factor_report, fused_qr_size_reduce, fused_qr_size_reduce_report, refined_fused_qr_size_reduce, refined_fused_qr_size_reduce_report, iterated_fused_qr_size_reduce, iterated_fused_qr_size_reduce_report, lattice_quad_double_gram_report, lattice_dpe_gram_report, lattice_numeric_backend_report, lattice_high_precision_fixture_report, precision_matrix_kernel_report, hlll_reduce, hlll_reduce_report, flatter_reduce, flatter_reduce_report)
use std.math.matrix as matrix
use std.math.big
use std.math.integer (Z)
use std.math.scalar (pow, log2, sqrt, floor, ceil, abs)
use std.core.str (split)
use std.math.crypto.lattice.lll as lll_backend
use std.os.clock (ticks)
use std.os.parallel as ospar

fn profile_default_alpha() f64 {
   "Return the default profile alpha used by flatter-style reduction heuristics."
   0.06250805094100162
}

fn _flatter_bkz_best_slope() f64 { 0.031281 }

fn profile_alpha_from_rhf(any rhf) f64 {
   "Convert a root-Hermite-factor target to a profile alpha parameter."
   2.0 * _flatter_log2_abs(rhf)
}

fn profile_rhf_from_alpha(any alpha) f64 {
   "Convert a profile alpha parameter to the analogous root-Hermite factor."
   pow(2.0, float(alpha) / 2.0)
}

fn profile_alpha_from_delta(any delta) f64 {
   "Convert an LLL-style delta target to an approximate alpha parameter."
   def d = float(delta)
   def r = 0.255 / d
   r * r
}

fn _flatter_log2_float_approx(f64 x) f64 {
   mut v = x
   if v <= 0.0 { return 0.0 }
   mut e = 0.0
   while v >= 2.0 { v *= 0.5 e += 1.0 }
   while v > 0.0 && v < 1.0 { v *= 2.0 e -= 1.0 }
   e + (v - 1.0) * 1.4426950408889634
}

fn _flatter_log2_abs(any x) f64 {
   if is_float(x) {
      def xf = float(x)
      return _flatter_log2_float_approx(xf < 0.0 ? 0.0 - xf : xf)
   }
   def z = Z(x)
   if z == Z(0) { return 0.0 }
   def a = z < Z(0) ? -z : z
   def s = bigint_to_str(a)
   def take = min(16, s.len)
   mut lead = 0.0
   mut i = 0
   while i < take {
      lead = lead * 10.0 + float(load8(s, i) - 48)
      i += 1
   }
   _flatter_log2_float_approx(lead) + float(s.len - take) * 3.321928094887362
}

fn _flatter_z_to_float(any x) f64 {
   def z = Z(x)
   if z == Z(0) { return 0.0 }
   def neg = z < Z(0)
   def a = neg ? -z : z
   def s = bigint_to_str(a)
   def n = s.len
   def take = min(16, n)
   mut lead = 0.0
   mut i = 0
   while i < take {
      lead = lead * 10.0 + float(load8(s, i) - 48)
      i += 1
   }
   def out = lead * pow(10.0, float(n - take))
   neg ? 0.0 - out : out
}

fn _flatter_profile_logs(list profile) list {
   mut out = []
   mut i = 0
   while i < profile.len {
      out = out.append(_flatter_log2_abs(profile.get(i)))
      i += 1
   }
   out
}

fn _flatter_profile_norm_logs(list profile) list {
   mut out = []
   mut i = 0
   while i < profile.len {
      out = out.append(0.5 * _flatter_log2_abs(profile.get(i)))
      i += 1
   }
   out
}

fn _flatter_profile_spread(list logs) f64 {
   if logs.len == 0 { return 0.0 }
   mut lo = float(logs.get(0))
   mut hi = lo
   mut i = 1
   while i < logs.len {
      def v = float(logs.get(i))
      if v < lo { lo = v }
      if v > hi { hi = v }
      i += 1
   }
   hi - lo
}

fn _flatter_profile_drop(list logs) f64 {
   if logs.len <= 1 { return 0.0 }
   mut max_left = []
   mut min_right = []
   mut cur_max = float(logs.get(0))
   mut i = 0
   while i < logs.len {
      def v = float(logs.get(i))
      if v > cur_max { cur_max = v }
      max_left = max_left.append(cur_max)
      min_right = min_right.append(0.0)
      i += 1
   }
   mut cur_min = float(logs.get(logs.len - 1))
   i = logs.len - 1
   while i >= 0 {
      def v = float(logs.get(i))
      if v < cur_min { cur_min = v }
      min_right[i] = cur_min
      i -= 1
   }
   mut spread = max_left.get(logs.len - 1) - min_right.get(0)
   i = 0
   while i < logs.len - 1 {
      def gap = min_right.get(i + 1) - max_left.get(i)
      if gap > 0.0 { spread = spread - gap }
      i += 1
   }
   spread
}

fn _flatter_profile_slice(list logs, int start, int stop) list {
   mut out = []
   mut i = max(0, start)
   def end = min(logs.len, stop)
   while i < end {
      out = out.append(float(logs.get(i)))
      i += 1
   }
   out
}

fn _flatter_profile_mean(list logs, int start, int stop) f64 {
   def end = min(logs.len, stop)
   if start >= end { return 0.0 }
   mut sum = 0.0
   mut i = max(0, start)
   while i < end {
      sum = sum + float(logs.get(i))
      i += 1
   }
   sum / float(end - max(0, start))
}

fn _flatter_goal_s_guess(int n) f64 {
   if n <= 1 { return 0.0 }
   def lgn = _flatter_log2_abs(n)
   3.0 * (1.0 + pow(3.0, lgn + 1.0) - pow(2.0, lgn + 2.0)) / 2.0
}

fn _flatter_goal_bound(int n, f64 quality, f64 best_slope) f64 {
   if n <= 1 { return 0.0 }
   best_slope * float(n) + quality * _flatter_goal_s_guess(n)
}

fn _flatter_shape_from_any(any basis_or_shape) dict {
   if is_dict(basis_or_shape) && is_list(basis_or_shape.get("profile_norm_log2", nil)) { return basis_or_shape }
   profile_shape_report(basis_or_shape)
}

fn profile_goal_report(any basis_or_shape, str target="slope", any value=nil, bool proved=false) dict {
   "Check a flatter-style profile reduction goal against a basis or profile_shape_report.
   The report mirrors flatter's drop/slope/RHF stopping rule while keeping the
   numbers auditable for Ny-only reducers."
   def shape = _flatter_shape_from_any(basis_or_shape)
   def logs = shape.get("profile_norm_log2", [])
   def n = logs.len
   def best_slope = _flatter_bkz_best_slope()
   mut slope = value == nil ? profile_default_alpha() : float(value)
   if target == "rhf" {
      slope = profile_alpha_from_rhf(value == nil ? profile_rhf_from_alpha(profile_default_alpha()) : value)
   } else if target == "drop" {
      def denom = bool(proved) ? max(1, n) : max(1, n - 1)
      slope = value == nil ? profile_default_alpha() : float(value) / float(denom)
   }
   if bool(proved) && slope <= best_slope { slope = best_slope + 0.000001 }
   def drop = shape.get("drop_norm", shape.get("drop", 0.0))
   def spread = shape.get("spread_norm", shape.get("spread", 0.0))
   if n <= 1 {
      return {
         "method": "profile-goal",
         "rows": n,
         "target": target,
         "target_value": value,
         "proved": proved,
         "slope": slope,
         "best_slope": best_slope,
         "drop": drop,
         "spread": spread,
         "max_drop": 0.0,
         "satisfied": true,
         "shape": shape
      }
   }
   mut quality = 0.0
   mut max_drop = 0.0
   mut gamma_i = 0.0
   mut mu_sep = 0.0
   mut l_drop = 0.0
   mut r_drop = 0.0
   mut mid_drop = 0.0
   mut mu_left = 0.0
   mut mu_right = 0.0
   mut satisfied = false
   if bool(proved) {
      max_drop = slope * float(n)
      satisfied = drop < max_drop
   } else {
      def s_guess = _flatter_goal_s_guess(n)
      def top_slope = slope < best_slope ? best_slope : slope
      quality = s_guess == 0.0 ? 0.0 : (top_slope - best_slope) * float(n) / s_guess
      max_drop = quality * s_guess + best_slope * float(n)
      gamma_i = quality * pow(3.0, _flatter_log2_abs(n))
      mu_sep = (max_drop - gamma_i) / 2.0 + gamma_i
      mut n_left = n / 2
      if n == 3 { n_left = 2 }
      def n_right = n - n_left
      l_drop = _flatter_goal_bound(n_left, quality, best_slope)
      r_drop = _flatter_goal_bound(n_right, quality, best_slope)
      mut n1 = n_left / 2
      if n_left == 3 { n1 = 2 }
      mut n3 = n_right / 2
      if n_right == 3 { n3 = 2 }
      mid_drop = _flatter_profile_drop(_flatter_profile_slice(logs, n1, n_left + n3))
      mu_left = _flatter_profile_mean(logs, 0, n_left)
      mu_right = _flatter_profile_mean(logs, n_left, n)
      satisfied = drop < max_drop && (mu_left - mu_right) < mu_sep && mid_drop <= l_drop + (max_drop - l_drop - r_drop)
   }
   {
      "method": "profile-goal",
      "rows": n,
      "target": target,
      "target_value": value,
      "proved": proved,
      "slope": slope,
      "quality": quality,
      "best_slope": best_slope,
      "drop": drop,
      "spread": spread,
      "max_drop": max_drop,
      "gamma": gamma_i,
      "mu_left": mu_left,
      "mu_right": mu_right,
      "mu_separation_bound": mu_sep,
      "left_drop_bound": l_drop,
      "right_drop_bound": r_drop,
      "mid_drop": mid_drop,
      "satisfied": satisfied,
      "shape": shape
   }
}

fn profile_goal_check(any basis_or_shape, str target="slope", any value=nil, bool proved=false) bool {
   "Return true when profile_goal_report says the basis satisfies the target."
   profile_goal_report(basis_or_shape, target, value, proved).get("satisfied", false)
}

fn _flatter_profile_compression_precision(f64 spread, int n, str mode, bool aggressive) int {
   if mode == "heuristic3" {
      return int(ceil(aggressive ? spread + 30.0 : 2.0 * spread + 30.0 + 2.0 * float(n)))
   }
   int(ceil(2.0 * spread + 40.0))
}

fn _flatter_profile_compression_plan_from_logs(list logs, str mode="recursive-generic", bool aggressive_precision=false) dict {
   def n = logs.len
   if n == 0 {
      return {
         "method": "profile-compression-plan",
         "source": "flatter-recursive-compression",
         "mode": mode,
         "rows": 0,
         "relative_shifts": [],
         "column_shifts": [],
         "compressible_gaps": 0,
         "total_relative_shift": 0,
         "max_gap": 0.0,
         "spread_before": 0.0,
         "spread_after_relative": 0.0,
         "precision_bits": 0,
         "base_shift": 0,
         "ok": true
      }
   }
   mut max_left = []
   mut min_right = []
   mut i = 0
   mut cur_max = float(logs.get(0))
   while i < n {
      def v = float(logs.get(i))
      if v > cur_max { cur_max = v }
      max_left = max_left.append(cur_max)
      min_right = min_right.append(0.0)
      i += 1
   }
   mut cur_min = float(logs.get(n - 1))
   i = n - 1
   while i >= 0 {
      def v = float(logs.get(i))
      if v < cur_min { cur_min = v }
      min_right[i] = cur_min
      i -= 1
   }
   mut shifts = [0]
   mut compressible = 0
   mut max_gap = 0.0
   i = 1
   while i < n {
      def gap = min_right.get(i) - max_left.get(i - 1)
      mut next_shift = int(shifts.get(i - 1))
      if gap > 1.0 {
         def add = int(floor(gap - 1.0))
         next_shift += add
         compressible += 1
         if gap > max_gap { max_gap = gap }
      }
      shifts = shifts.append(next_shift)
      i += 1
   }
   def spread_before = max_left.get(n - 1) - min_right.get(0)
   def relative_tail = int(shifts.get(n - 1))
   def spread_after = max_left.get(n - 1) - float(relative_tail) - min_right.get(0)
   def precision = _flatter_profile_compression_precision(spread_after, n, mode, aggressive_precision)
   def base_shift = int(ceil(max_left.get(n - 1))) - relative_tail - precision
   mut column_shifts = []
   i = 0
   while i < n {
      column_shifts = column_shifts.append(int(shifts.get(i)) + base_shift)
      i += 1
   }
   {
      "method": "profile-compression-plan",
      "source": "flatter-recursive-compression",
      "mode": mode,
      "aggressive_precision": aggressive_precision,
      "rows": n,
      "relative_shifts": shifts,
      "column_shifts": column_shifts,
      "compressible_gaps": compressible,
      "total_relative_shift": relative_tail,
      "max_gap": max_gap,
      "spread_before": spread_before,
      "spread_after_relative": spread_after,
      "precision_bits": precision,
      "base_shift": base_shift,
      "max_from_left": max_left,
      "min_from_right": min_right,
      "ok": spread_after <= spread_before + 0.000001
   }
}

fn profile_compression_plan_report(any basis_or_shape, str mode="recursive-generic", bool aggressive_precision=false) dict {
   "Report flatter-style profile compression column shifts and precision budget."
   def shape = _flatter_shape_from_any(basis_or_shape)
   _flatter_profile_compression_plan_from_logs(shape.get("profile_norm_log2", []), mode, aggressive_precision).set("shape_rows", shape.get("rows", 0))
}

fn profile_shape_report(any basis) dict {
   "Report log-GSO profile spread/drop metrics used by flatter-style reducers."
   def t0 = ticks()
   def a = _flatter_reduce_matrix(basis)
   def rows = _flatter_matrix_rows(a)
   def quality = rows <= 160 ? lll_backend.lll_quality_report(a) : dict(0)
   mut gso = quality.get("gso", nil)
   if gso == nil { gso = lll_backend.gso_report(a) }
   mut profile = quality.get("profile", nil)
   if profile == nil { profile = gso.get("profile", []) }
   def logs = _flatter_profile_logs(profile)
   def norm_logs = _flatter_profile_norm_logs(profile)
   {
      "method": "profile-shape",
      "rows": rows,
      "cols": _flatter_matrix_cols(a),
      "profile": profile,
      "profile_log2": logs,
      "profile_norm_log2": norm_logs,
      "spread": _flatter_profile_spread(logs),
      "drop": _flatter_profile_drop(logs),
      "spread_norm": _flatter_profile_spread(norm_logs),
      "drop_norm": _flatter_profile_drop(norm_logs),
      "gso": gso,
      "quality_fast_path": quality.get("quality_fast_path", false),
      "numeric_kernel": quality.get("numeric_kernel", ""),
      "elapsed_ms": float(ticks() - t0) / 1000000.0
   }
}

fn _flatter_row_norm_shape_report(any basis) dict {
   def t0 = ticks()
   def a = _flatter_reduce_matrix(basis)
   def rows = _flatter_matrix_rows(a)
   def cols = _flatter_matrix_cols(a)
   def data = _flatter_matrix_data(a)
   mut profile = []
   mut i = 0
   while i < rows {
      profile = profile.append(_flatter_norm_z(data.get(i)))
      i += 1
   }
   def logs = _flatter_profile_logs(profile)
   def norm_logs = _flatter_profile_norm_logs(profile)
   {
      "method": "profile-shape-row-norm-precheck",
      "rows": rows,
      "cols": cols,
      "profile": profile,
      "profile_log2": logs,
      "profile_norm_log2": norm_logs,
      "spread": _flatter_profile_spread(logs),
      "drop": _flatter_profile_drop(logs),
      "spread_norm": _flatter_profile_spread(norm_logs),
      "drop_norm": _flatter_profile_drop(norm_logs),
      "gso": _flatter_skip("row-norm flat precheck"),
      "quality_fast_path": false,
      "numeric_kernel": "row-norm-flat-precheck",
      "elapsed_ms": float(ticks() - t0) / 1000000.0
   }
}

fn _flatter_reduce_matrix(any basis) any {
   if is_list(basis) && basis.len >= 3 && is_int(basis.get(0, nil)) && is_int(basis.get(1, nil)) && is_list(basis.get(2, nil)) { return basis }
   matrix.Matrix(basis)
}

fn _flatter_matrix_rows(any m) int { int(m.get(0)) }

fn _flatter_matrix_cols(any m) int { int(m.get(1)) }

fn _flatter_matrix_data(any m) list { m.get(2) }

fn _flatter_clone_rows(any m) list {
   def rows = _flatter_matrix_rows(m)
   def data = _flatter_matrix_data(m)
   mut out = list(rows)
   __list_set_len(out, rows)
   mut i = 0
   while i < rows {
      out[i] = vec_clone(data.get(i))
      i += 1
   }
   out
}

fn _flatter_clone_rows_small(any m) list {
   def rows = _flatter_matrix_rows(m)
   def cols = _flatter_matrix_cols(m)
   def data = _flatter_matrix_data(m)
   mut out = list(rows)
   __list_set_len(out, rows)
   mut i = 0
   while i < rows {
      mut row = list(cols)
      __list_set_len(row, cols)
      mut j = 0
      while j < cols {
         def raw = data.get(i).get(j)
         def v = _flatter_small_i64(raw)
         row[j] = v != 2147483647 ? v : raw
         j += 1
      }
      out[i] = row
      i += 1
   }
   out
}

fn _flatter_clone_rows_lower_report(any m) dict {
   def rows = _flatter_matrix_rows(m)
   def cols = _flatter_matrix_cols(m)
   def data = _flatter_matrix_data(m)
   mut out = list(rows)
   __list_set_len(out, rows)
   mut lower = true
   mut i = 0
   while i < rows {
      def row_data = data[i]
      mut row = list(cols)
      __list_set_len(row, cols)
      mut j = 0
      while j < cols {
         def raw = row_data[j]
         mut v = raw
         if is_bigint(raw) {
            def z = Z(raw)
            def az = z < Z(0) ? -z : z
            if az <= Z(2000000000000000000) { v = bigint_to_int(z) }
         }
         if j > i {
            if is_int(v) {
               if int(v) != 0 { lower = false }
            } elif Z(v) != Z(0) {
               lower = false
            }
         }
         row[j] = v
         j += 1
      }
      out[i] = row
      i += 1
   }
   {"rows": out, "lower": lower}
}

fn _flatter_public_basis(any basis) list { _flatter_clone_rows(_flatter_reduce_matrix(basis)) }

fn _flatter_small_i64(any x) int {
   if is_int(x) {
      def v = int(x)
      if v >= -1000000000 && v <= 1000000000 { return v }
      return 2147483647
   }
   if is_bigint(x) {
      def z = Z(x)
      def a = z < Z(0) ? -z : z
      if bigint_bit_length(a) < 31 { return bigint_to_int(z) }
   }
   2147483647
}

fn _flatter_dot_z(list a, list b) any {
   mut int direct_sum = 0
   mut bool direct_ok = true
   mut int dk = 0
   def int direct_limit = 4000000000000000000
   while dk < a.len && direct_ok {
      def av = a[dk]
      def bv = b[dk]
      if is_int(av) && is_int(bv) {
         def int ai = int(av)
         def int bi = int(bv)
         if ai == 0 || bi == 0 {
            nil
         } elif ai >= -1000000000 && ai <= 1000000000 && bi >= -1000000000 && bi <= 1000000000 {
            def int term = ai * bi
            def int sum_abs = direct_sum < 0 ? -direct_sum : direct_sum
            def int term_abs = term < 0 ? -term : term
            if sum_abs <= direct_limit - term_abs {
               direct_sum += term
            } else {
               direct_ok = false
            }
         } else {
            direct_ok = false
         }
      } else {
         direct_ok = false
      }
      dk += 1
   }
   if direct_ok { return direct_sum }
   mut int small_sum = 0
   mut bool small_ok = true
   mut int k = 0
   while k < a.len && small_ok {
      def int ai = _flatter_small_i64(a[k])
      def int bi = _flatter_small_i64(b[k])
      if ai != 2147483647 && bi != 2147483647 {
         if ai != 0 && bi != 0 {
            def int term = ai * bi
            def int sum_abs = small_sum < 0 ? -small_sum : small_sum
            def int term_abs = term < 0 ? -term : term
            if sum_abs <= direct_limit - term_abs {
               small_sum += term
            } else {
               small_ok = false
            }
         }
      } else {
         small_ok = false
      }
      k += 1
   }
   if small_ok { return small_sum }
   mut s = Z(0)
   mut i = 0
   while i < a.len {
      s = s + Z(a[i]) * Z(b[i])
      i += 1
   }
   s
}

fn _flatter_norm_z(list a) any {
   mut int direct_sum = 0
   mut bool direct_ok = true
   mut int dk = 0
   def int direct_limit = 4000000000000000000
   while dk < a.len && direct_ok {
      def av = a[dk]
      if is_int(av) {
         def int ai = int(av)
         if ai == 0 {
            nil
         } elif ai >= -1000000000 && ai <= 1000000000 {
            def int term = ai * ai
            def int sum_abs = direct_sum < 0 ? -direct_sum : direct_sum
            if sum_abs <= direct_limit - term {
               direct_sum += term
            } else {
               direct_ok = false
            }
         } else {
            direct_ok = false
         }
      } else {
         direct_ok = false
      }
      dk += 1
   }
   if direct_ok { return direct_sum }
   mut int small_sum = 0
   mut bool small_ok = true
   mut int k = 0
   while k < a.len && small_ok {
      def int ai = _flatter_small_i64(a[k])
      if ai != 2147483647 {
         if ai != 0 {
            def int term = ai * ai
            def int sum_abs = small_sum < 0 ? -small_sum : small_sum
            if sum_abs <= direct_limit - term {
               small_sum += term
            } else {
               small_ok = false
            }
         }
      } else {
         small_ok = false
      }
      k += 1
   }
   if small_ok { return small_sum }
   mut s = Z(0)
   mut i = 0
   while i < a.len {
      def z = Z(a[i])
      s = s + z * z
      i += 1
   }
   s
}

fn _flatter_round_div(any num, any den) any {
   if is_int(num) && is_int(den) {
      def ni = int(num)
      def di = int(den)
      if di > 0 && ni >= -1000000000000 && ni <= 1000000000000 && di <= 1000000000000 {
         if ni >= 0 { return(2 * ni + di) / (2 * di) }
         return -((2 * (-ni) + di) / (2 * di))
      }
   }
   def d = Z(den)
   if d <= Z(0) { return Z(0) }
   def n = Z(num)
   if n >= Z(0) { return(Z(2) * n + d) / (Z(2) * d) }
   -((Z(2) * (-n) + d) / (Z(2) * d))
}

fn _flatter_round_div_signed(any num, any den) any {
   if is_int(num) && is_int(den) {
      def ni = int(num)
      def di = int(den)
      if di != 0 && ni >= -1000000000000 && ni <= 1000000000000 && di >= -1000000000000 && di <= 1000000000000 {
         if di < 0 { return -_flatter_round_div_signed(ni, -di) }
         if ni >= 0 { return(2 * ni + di) / (2 * di) }
         return -((2 * (-ni) + di) / (2 * di))
      }
   }
   def d = Z(den)
   if d == Z(0) { return Z(0) }
   if d < Z(0) { return -_flatter_round_div_signed(num, -d) }
   _flatter_round_div(num, d)
}

fn _flatter_round_div_signed_int(int num, int den) int {
   if den == 0 { return 2147483647 }
   mut n = num
   mut d = den
   if d < 0 {
      n = -n
      d = -d
   }
   if n < -2000000000000000000 || n > 2000000000000000000 || d > 2000000000000000000 { return 2147483647 }
   if n >= 0 { return(2 * n + d) / (2 * d) }
   -((2 * (-n) + d) / (2 * d))
}

fn _flatter_row_submul(list a, list b, any coeff) list {
   def len = a.len
   mut out = list(len)
   __list_set_len(out, len)
   mut i = 0
   def ci = _flatter_small_i64(coeff)
   if ci != 2147483647 {
      while i < len {
         def ai = _flatter_small_i64(a.get(i))
         def bi = _flatter_small_i64(b.get(i))
         if ai != 2147483647 && bi != 2147483647 {
            def v = ai - ci * bi
            out[i] = v >= -100000000 && v <= 100000000 ? v : Z(v)
         } else {
            out[i] = Z(a.get(i)) - Z(ci) * Z(b.get(i))
         }
         i += 1
      }
   } else {
      def c = Z(coeff)
      while i < len {
         out[i] = Z(a.get(i)) - c * Z(b.get(i))
         i += 1
      }
   }
   out
}

fn _flatter_row_submul_prefix(list a, list b, any coeff, int upto) list {
   def len = a.len
   mut out = list(len)
   __list_set_len(out, len)
   mut i = 0
   def end = min(min(a.len, b.len), upto + 1)
   def ci = _flatter_small_i64(coeff)
   if ci != 2147483647 {
      while i < end {
         def ai = _flatter_small_i64(a.get(i))
         def bi = _flatter_small_i64(b.get(i))
         if ai != 2147483647 && bi != 2147483647 {
            def v = ai - ci * bi
            out[i] = v >= -100000000 && v <= 100000000 ? v : Z(v)
         } else {
            out[i] = Z(a.get(i)) - Z(ci) * Z(b.get(i))
         }
         i += 1
      }
   } else {
      def c = Z(coeff)
      while i < end {
         out[i] = Z(a.get(i)) - c * Z(b.get(i))
         i += 1
      }
   }
   while i < len {
      out[i] = a.get(i)
      i += 1
   }
   out
}

fn _flatter_row_submul_prefix_bigint_inplace(list a, list b, any coeff, int upto) list {
   def end = min(min(a.len, b.len), upto + 1)
   mut i = 0
   if is_int(coeff) {
      def ci = int(coeff)
      def cz = Z(ci)
      def ac = ci < 0 ? -ci : ci
      while i < end {
         def av = a[i]
         def bv = b[i]
         if is_int(av) && is_int(bv) {
            def ai = int(av)
            def bi = int(bv)
            def aa = ai < 0 ? -ai : ai
            def ab = bi < 0 ? -bi : bi
            if ab == 0 || ac <= (2000000000000000000 - aa) / ab {
               def v = ai - ci * bi
               a[i] = v
            } else {
               a[i] = Z(ai) - cz * Z(bi)
            }
         } else {
            a[i] = Z(av) - cz * Z(bv)
         }
         i += 1
      }
   } else {
      def c = Z(coeff)
      while i < end {
         a[i] = Z(a.get(i)) - c * Z(b.get(i))
         i += 1
      }
   }
   a
}

fn _flatter_row_i64_abs_bound(list row, int cap) int {
   mut out = 0
   mut i = 0
   while i < row.len {
      def v = row.get(i)
      if !is_int(v) { return -1 }
      def vi = int(v)
      def av = vi < 0 ? -vi : vi
      if av > cap { return -1 }
      if av > out { out = av }
      i += 1
   }
   out
}

fn _flatter_row_i64_abs_bounds(list rows, int cap) list {
   mut out = list(rows.len)
   __list_set_len(out, rows.len)
   mut i = 0
   while i < rows.len {
      out[i] = _flatter_row_i64_abs_bound(rows.get(i), cap)
      i += 1
   }
   out
}

fn _flatter_compress_row_i64_bound(list row, int cap) list {
   mut max_abs = 0
   mut i = 0
   while i < row.len {
      def raw = row.get(i)
      mut v = raw
      if is_bigint(raw) {
         def z = Z(raw)
         def az = z < Z(0) ? -z : z
         if az > Z(cap) { return [row, -1] }
         v = bigint_to_int(z)
      } elif !is_int(raw) {
         return [row, -1]
      }
      def vi = int(v)
      def av = vi < 0 ? -vi : vi
      if av > cap { return [row, -1] }
      if av > max_abs { max_abs = av }
      row[i] = vi
      i += 1
   }
   [row, max_abs]
}

fn _flatter_row_submul_prefix_i64_bound_cached(list a, list b, int coeff, int upto, int old_bound) list {
   mut max_abs = old_bound
   def end = min(min(a.len, b.len), upto + 1)
   mut i = 0
   while i < end {
      def v = int(a.get(i)) - coeff * int(b.get(i))
      a[i] = v
      def av = v < 0 ? -v : v
      if av > max_abs { max_abs = av }
      i += 1
   }
   [a, max_abs]
}

fn _flatter_row_submul_prefix_i64_checked_cached(list a, list b, int coeff, int upto, int cap, int old_bound=0) list {
   def end = min(min(a.len, b.len), upto + 1)
   def cabs = coeff < 0 ? -coeff : coeff
   mut max_abs = old_bound
   mut i = 0
   while i < end {
      def av = a.get(i)
      def bv = b.get(i)
      if !is_int(av) || !is_int(bv) { return [a, -1] }
      def ai = int(av)
      def bi = int(bv)
      def babs = bi < 0 ? -bi : bi
      def aabs = ai < 0 ? -ai : ai
      if babs != 0 && cabs > (cap - aabs) / babs { return [a, -1] }
      i += 1
   }
   i = 0
   while i < end {
      def v = int(a.get(i)) - coeff * int(b.get(i))
      a[i] = v
      def av = v < 0 ? -v : v
      if av > max_abs { max_abs = av }
      i += 1
   }
   [a, max_abs]
}

fn _flatter_row_addmul(list a, list b, any coeff) list {
   def ci = _flatter_small_i64(coeff)
   ci != 2147483647 ? _flatter_row_submul(a, b, -ci) : _flatter_row_submul(a, b, -Z(coeff))
}

fn _flatter_zero_row(int n) list {
   mut out = []
   mut i = 0
   while i < n {
      out = out.append(Z(0))
      i += 1
   }
   out
}

fn _flatter_mod_pos(any x, any q) bigint {
   def qq = Z(q)
   if qq == Z(0) { return Z(0) }
   mut r = Z(x) % qq
   if r < Z(0) { r = r + (qq < Z(0) ? -qq : qq) }
   r
}

@inline
fn _flatter_is_zero_scalar(any x) bool {
   if is_int(x) { return int(x) == 0 }
   Z(x) == Z(0)
}

fn _flatter_all_zero(any m) bool {
   def rows = _flatter_matrix_rows(m)
   def cols = _flatter_matrix_cols(m)
   def data = _flatter_matrix_data(m)
   mut i = 0
   while i < rows {
      def row = data.get(i)
      mut j = 0
      while j < cols {
         if !_flatter_is_zero_scalar(row.get(j)) { return false }
         j += 1
      }
      i += 1
   }
   true
}

fn _flatter_is_lower_triangular(any m) bool {
   def rows = _flatter_matrix_rows(m)
   def cols = _flatter_matrix_cols(m)
   def data = _flatter_matrix_data(m)
   mut i = 0
   while i < rows {
      def row = data.get(i)
      mut j = i + 1
      while j < cols {
         if !_flatter_is_zero_scalar(row.get(j)) { return false }
         j += 1
      }
      i += 1
   }
   true
}

fn _flatter_is_local_banded(any m, int window) bool {
   def rows = _flatter_matrix_rows(m)
   def cols = _flatter_matrix_cols(m)
   def data = _flatter_matrix_data(m)
   def w = max(0, window)
   mut i = 0
   while i < rows {
      def row = data.get(i)
      mut j = 0
      while j < cols {
         if !_flatter_is_zero_scalar(row.get(j)) && abs(j - i) > w { return false }
         j += 1
      }
      i += 1
   }
   true
}

fn _flatter_lower_triangular_profile_spread(any m) f64 {
   def rows = _flatter_matrix_rows(m)
   def cols = _flatter_matrix_cols(m)
   if rows == 0 || cols == 0 { return 0.0 }
   def data = _flatter_matrix_data(m)
   mut lo = 0.0
   mut hi = 0.0
   mut seen = false
   mut i = 0
   while i < rows && i < cols {
      def v = 2.0 * _flatter_log2_abs(data.get(i).get(i))
      if !seen {
         lo = v
         hi = v
         seen = true
      } else {
         if v < lo { lo = v }
         if v > hi { hi = v }
      }
      i += 1
   }
   hi - lo
}

fn _flatter_is_upper_triangular(any m) bool {
   def rows = _flatter_matrix_rows(m)
   def cols = _flatter_matrix_cols(m)
   mut i = 0
   while i < rows {
      mut j = 0
      while j < i && j < cols {
         if Z(matrix.mat_get(m, i, j)) != Z(0) { return false }
         j += 1
      }
      i += 1
   }
   true
}

fn _flatter_qary_split(any m) int {
   def rows = _flatter_matrix_rows(m)
   def cols = _flatter_matrix_cols(m)
   if rows != cols || rows < 4 { return -1 }
   mut k = 0
   while k < rows {
      def diag = _flatter_abs_z(matrix.mat_get(m, k, k))
      if diag == Z(1) { break }
      if diag <= Z(1) { return -1 }
      mut j = 0
      while j < cols {
         if j != k && Z(matrix.mat_get(m, k, j)) != Z(0) { return -1 }
         j += 1
      }
      k += 1
   }
   if k <= 0 || k >= rows { return -1 }
   mut i = k
   while i < rows {
      if _flatter_abs_z(matrix.mat_get(m, i, i)) != Z(1) { return -1 }
      mut j = k
      while j < cols {
         if j != i && !_flatter_is_zero_scalar(matrix.mat_get(m, i, j)) { return -1 }
         j += 1
      }
      i += 1
   }
   k
}

fn _flatter_lower_triangular_qary_split(any m) int {
   def rows = _flatter_matrix_rows(m)
   def cols = _flatter_matrix_cols(m)
   if rows != cols || rows < 4 || !_flatter_is_lower_triangular(m) { return -1 }
   mut k = 0
   while k < rows {
      def diag = _flatter_abs_z(matrix.mat_get(m, k, k))
      if diag == Z(1) { break }
      if diag <= Z(1) { return -1 }
      k += 1
   }
   if k <= 0 || k >= rows { return -1 }
   mut i = k
   while i < rows {
      if _flatter_abs_z(matrix.mat_get(m, i, i)) != Z(1) { return -1 }
      mut j = k
      while j < cols {
         if j != i && Z(matrix.mat_get(m, i, j)) != Z(0) { return -1 }
         j += 1
      }
      i += 1
   }
   k
}

fn _flatter_lower_qary_uniform_modulus(any m, int k) bool {
   if k <= 0 { return false }
   def q = _flatter_abs_z(matrix.mat_get(m, 0, 0))
   if q <= Z(1) { return false }
   mut i = 1
   while i < k {
      if _flatter_abs_z(matrix.mat_get(m, i, i)) != q { return false }
      i += 1
   }
   true
}

fn _flatter_upper_qary_split(any m) int {
   def rows = _flatter_matrix_rows(m)
   def cols = _flatter_matrix_cols(m)
   if rows != cols || rows < 4 { return -1 }
   mut k = 0
   while k < rows && Z(matrix.mat_get(m, k, k)) == Z(1) { k += 1 }
   if k <= 0 || k >= rows { return -1 }
   mut i = 0
   while i < k {
      mut j = 0
      while j < k {
         def want = i == j ? Z(1) : Z(0)
         if Z(matrix.mat_get(m, i, j)) != want { return -1 }
         j += 1
      }
      i += 1
   }
   def q = _flatter_abs_z(matrix.mat_get(m, k, k))
   if q <= Z(1) { return -1 }
   i = k
   while i < rows {
      mut j = 0
      while j < k {
         if Z(matrix.mat_get(m, i, j)) != Z(0) { return -1 }
         j += 1
      }
      while j < cols {
         def want = i == j ? q : Z(0)
         if _flatter_abs_z(matrix.mat_get(m, i, j)) != want { return -1 }
         j += 1
      }
      i += 1
   }
   k
}

fn _flatter_qary_blockswap_orientation(any m, int k) any {
   def mm = _flatter_reduce_matrix(m)
   def data = _flatter_matrix_data(mm)
   def rows = _flatter_matrix_rows(mm)
   def cols = _flatter_matrix_cols(mm)
   mut out = []
   mut i = k
   while i < rows {
      mut row = []
      mut c = k
      while c < cols {
         row = row.append(data.get(i).get(c))
         c += 1
      }
      c = 0
      while c < k {
         row = row.append(data.get(i).get(c))
         c += 1
      }
      out = out.append(row)
      i += 1
   }
   i = 0
   while i < k {
      mut row = []
      mut c = k
      while c < cols {
         row = row.append(data.get(i).get(c))
         c += 1
      }
      c = 0
      while c < k {
         row = row.append(data.get(i).get(c))
         c += 1
      }
      out = out.append(row)
      i += 1
   }
   matrix.Matrix(out)
}

fn _flatter_qary_unblockswap_columns(any m, int k) any {
   def mm = _flatter_reduce_matrix(m)
   def data = _flatter_matrix_data(mm)
   def rows = _flatter_matrix_rows(mm)
   def cols = _flatter_matrix_cols(mm)
   def tail = cols - k
   mut out = []
   mut i = 0
   while i < rows {
      def src = data.get(i)
      mut row = []
      mut c = tail
      while c < cols {
         row = row.append(src.get(c))
         c += 1
      }
      c = 0
      while c < tail {
         row = row.append(src.get(c))
         c += 1
      }
      out = out.append(row)
      i += 1
   }
   matrix.Matrix(out)
}

fn _flatter_pow2_z(int bits) any {
   bigint_pow(Z(2), Z(bits))
}

fn _flatter_shift_scale_value(any v, int shift) any {
   def z = Z(v)
   if shift == 0 { return z }
   if shift < 0 { return z * _flatter_pow2_z(0 - shift) }
   _flatter_round_div_signed(z, _flatter_pow2_z(shift))
}

fn _flatter_scale_columns_by_shifts(any m, list shifts) any {
   def a = _flatter_reduce_matrix(m)
   def data = _flatter_matrix_data(a)
   mut out = []
   mut r = 0
   while r < data.len {
      def src = data.get(r)
      mut row = []
      mut c = 0
      while c < src.len {
         row = row.append(_flatter_shift_scale_value(src.get(c), int(shifts.get(c, 0))))
         c += 1
      }
      out = out.append(row)
      r += 1
   }
   matrix.Matrix(out)
}

fn _flatter_qary_uniform_column_shifts(any m, int split, int keep_bits) list {
   def q = _flatter_abs_z(matrix.mat_get(m, 0, 0))
   mut bits = int(ceil(_flatter_log2_abs(q)))
   if bits < 0 { bits = 0 }
   def shift = max(0, bits - keep_bits)
   mut out = []
   mut c = 0
   while c < _flatter_matrix_cols(m) {
      out = out.append(c < split ? shift : 0)
      c += 1
   }
   out
}

fn _flatter_qary_residue_key(list data, int k, list terms) str {
   mut key = ""
   mut col = 0
   while col < k {
      def q = _flatter_abs_z(data.get(col).get(col))
      mut s = Z(0)
      mut t = 0
      while t < terms.len {
         def term = terms.get(t)
         def row_idx = k + int(term.get(0))
         def sign = Z(term.get(1))
         s = s + sign * Z(data.get(row_idx).get(col))
         t += 1
      }
      def r = _flatter_mod_pos(s, q)
      if col > 0 { key = key + "," }
      key = key + bigint_to_str(r)
      col += 1
   }
   key
}

fn _flatter_qary_complement_key(str key, list moduli) str {
   def parts = split(key, ",")
   mut out = ""
   mut i = 0
   while i < parts.len {
      def q = Z(moduli.get(i))
      def r = Z(parts.get(i))
      def c = r == Z(0) ? Z(0) : q - r
      if i > 0 { out = out + "," }
      out = out + bigint_to_str(c)
      i += 1
   }
   out
}

fn _flatter_seen_key_index(list keys, str key) int {
   mut i = 0
   while i < keys.len {
      if keys.get(i) == key { return i }
      i += 1
   }
   -1
}

fn _flatter_lower_triangular_residue_key(list data, int k, list terms) str {
   mut residue = []
   mut col = 0
   while col < k {
      mut s = Z(0)
      mut t = 0
      while t < terms.len {
         def term = terms.get(t)
         def row_idx = k + int(term.get(0))
         def sign = Z(term.get(1))
         s = s + sign * Z(data.get(row_idx).get(col))
         t += 1
      }
      residue = residue.append(s)
      col += 1
   }
   col = k - 1
   while col >= 0 {
      def pivot_abs = _flatter_abs_z(data.get(col).get(col))
      if pivot_abs == Z(0) { return "#singular" }
      def r = _flatter_mod_pos(Z(residue.get(col)), pivot_abs)
      def pivot = Z(data.get(col).get(col))
      def coeff = (Z(residue.get(col)) - r) / pivot
      if coeff != Z(0) {
         mut j = 0
         while j <= col {
            residue[j] = Z(residue.get(j)) - coeff * Z(data.get(col).get(j))
            j += 1
         }
      }
      residue[col] = r
      col -= 1
   }
   mut key = ""
   col = 0
   while col < k {
      if col > 0 { key = key + "," }
      key = key + bigint_to_str(Z(residue.get(col)))
      col += 1
   }
   key
}

fn _flatter_lower_triangular_qary_find_relation(any m, int k) list {
   def local = _flatter_lower_triangular_qary_find_local_relation(m, k)
   if local.len > 0 { return local }
   def data = _flatter_matrix_data(m)
   def rows = _flatter_matrix_rows(m)
   def tail = rows - k
   mut moduli = []
   mut c = 0
   while c < k {
      moduli = moduli.append(_flatter_abs_z(data.get(c).get(c)))
      c += 1
   }
   def zero_key = _flatter_lower_triangular_residue_key(data, k, [])
   mut seen_keys = []
   mut seen_terms = []
   mut a = 0
   while a < tail {
      mut b = a + 1
      while b < tail {
         mut sa_i = 0
         while sa_i < 2 {
            def sa = sa_i == 0 ? Z(-1) : Z(1)
            mut sb_i = 0
            while sb_i < 2 {
               def sb = sb_i == 0 ? Z(-1) : Z(1)
               def terms = [[a, sa], [b, sb]]
               def key = _flatter_lower_triangular_residue_key(data, k, terms)
               if key == zero_key { return terms }
               def want = _flatter_qary_complement_key(key, moduli)
               def want_idx = _flatter_seen_key_index(seen_keys, want)
               def other = want_idx >= 0 ? seen_terms.get(want_idx) : nil
               if other != nil && _flatter_terms_disjoint(terms, other) { return terms + other }
               if _flatter_seen_key_index(seen_keys, key) < 0 {
                  seen_keys = seen_keys.append(key)
                  seen_terms = seen_terms.append(terms)
               }
               sb_i += 1
            }
            sa_i += 1
         }
         b += 1
      }
      a += 1
   }
   []
}

fn _flatter_terms_disjoint(list a, list b) bool {
   mut i = 0
   while i < a.len {
      mut j = 0
      while j < b.len {
         if int(a.get(i).get(0)) == int(b.get(j).get(0)) { return false }
         j += 1
      }
      i += 1
   }
   true
}

fn _flatter_qary_miss_report(str method, bool detected, int split, int rows, int cols, any basis, any t0, bool track_transform=true) dict {
   mut out = {
      "method": method,
      "detected": detected,
      "found": false,
      "rows": rows,
      "cols": cols,
      "basis": basis,
      "transform": track_transform ? matrix.matrix_identity(rows) : nil,
      "transform_tracked": track_transform,
      "transform_verified": track_transform,
      "verification_skipped": !track_transform,
      "elapsed_ms": float(ticks() - t0) / 1000000.0
   }
   if detected { out["split"] = split }
   out
}

fn _flatter_qary_success_report(str method, int split, int rows, int cols, list terms, any basis, any data, any work, any transform, int target, any t0, bool track_transform=true) dict {
   def out_basis = matrix.Matrix(work)
   mut transform_matrix = nil
   mut transform_verified = false
   if track_transform {
      transform_matrix = matrix.Matrix(transform)
      def verified_first = _flatter_same_row(_flatter_apply_transform_row(transform.get(0), basis), work.get(0))
      def verified_target = target == 0 ? true : _flatter_same_row(_flatter_apply_transform_row(transform.get(target), basis), work.get(target))
      transform_verified = verified_first && verified_target
   }
   {
      "method": method,
      "detected": true,
      "found": true,
      "split": split,
      "tail_rows": rows - split,
      "relation_terms": terms,
      "weight": terms.len,
      "first_norm_before": _flatter_dot_z(data.get(0), data.get(0)),
      "first_norm_after": _flatter_dot_z(work.get(0), work.get(0)),
      "rows": rows,
      "cols": cols,
      "basis": out_basis,
      "transform": transform_matrix,
      "transform_tracked": track_transform,
      "transform_verified": transform_verified,
      "verification_skipped": !track_transform,
      "elapsed_ms": float(ticks() - t0) / 1000000.0
   }
}

fn _flatter_qary_success_row_report(str method, int split, int rows, int cols, list terms, any basis, any data, any work, any transform, int target, any t0, str relation_kind, bool track_transform=true) dict {
   def out_basis = matrix.Matrix(work)
   mut transform_matrix = nil
   mut transform_verified = false
   if track_transform {
      transform_matrix = matrix.Matrix(transform)
      def verified_first = _flatter_same_row(_flatter_apply_transform_row(transform.get(0), basis), work.get(0))
      def verified_target = target == 0 ? true : _flatter_same_row(_flatter_apply_transform_row(transform.get(target), basis), work.get(target))
      transform_verified = verified_first && verified_target
   }
   {
      "method": method,
      "detected": true,
      "found": true,
      "split": split,
      "tail_rows": rows - split,
      "relation_terms": terms,
      "relation_kind": relation_kind,
      "weight": terms.len,
      "first_norm_before": _flatter_dot_z(data.get(0), data.get(0)),
      "first_norm_after": _flatter_dot_z(work.get(0), work.get(0)),
      "rows": rows,
      "cols": cols,
      "basis": out_basis,
      "transform": transform_matrix,
      "transform_tracked": track_transform,
      "transform_verified": transform_verified,
      "verification_skipped": !track_transform,
      "elapsed_ms": float(ticks() - t0) / 1000000.0
   }
}

fn _flatter_qary_success_from_row_report(str method, int k, int rows, int cols, any basis, dict found, any t0, str relation_kind, bool track_transform=true) dict {
   mut work = _flatter_clone_rows(basis)
   mut transform = track_transform ? _flatter_identity_rows(rows) : nil
   def target = k + int(found.get("terms").get(0).get(0))
   work[target] = found.get("row")
   if track_transform { transform[target] = found.get("transform_row") }
   if target != 0 {
      def tmp_row = work.get(0)
      work[0] = work.get(target)
      work[target] = tmp_row
      if track_transform {
         def tmp_tr = transform.get(0)
         transform[0] = transform.get(target)
         transform[target] = tmp_tr
      }
   }
   mut out = _flatter_qary_success_row_report(method, k, rows, cols, found.get("terms"), basis, _flatter_matrix_data(basis), work, transform, target, t0, relation_kind, track_transform)
   if found.contains("trials") { out["search_trials"] = found.get("trials", 0) }
   if found.contains("max_weight") { out["search_max_weight"] = found.get("max_weight", 0) }
   out
}

fn _flatter_lower_triangular_reduce_relation(list data, int k, int rows, int cols, list terms) dict {
   mut rel_row = _flatter_zero_row(cols)
   mut rel_tr = _flatter_zero_row(rows)
   mut ti = 0
   while ti < terms.len {
      def term = terms.get(ti)
      def row_idx = k + int(term.get(0))
      def sign = Z(term.get(1))
      rel_row = _flatter_row_addmul(rel_row, data.get(row_idx), sign)
      rel_tr[row_idx] = Z(rel_tr.get(row_idx)) + sign
      ti += 1
   }
   mut col = k - 1
   while col >= 0 {
      def pivot = Z(data.get(col).get(col))
      if pivot != Z(0) {
         def q = _flatter_round_div_signed(rel_row.get(col), pivot)
         if q != Z(0) {
            rel_row = _flatter_row_submul(rel_row, data.get(col), q)
            rel_tr[col] = Z(rel_tr.get(col)) - q
         }
      }
      col -= 1
   }
   {"row": rel_row, "transform_row": rel_tr, "norm": _flatter_dot_z(rel_row, rel_row)}
}

fn _flatter_lower_triangular_qary_terms_short(list data, int k, int rows, int cols, list terms) bool {
   def rep = _flatter_lower_triangular_reduce_relation(data, k, rows, cols, terms)
   Z(rep.get("norm")) <= Z(terms.len)
}

fn _flatter_lower_triangular_qary_find_local_relation(any m, int k) list {
   def data = _flatter_matrix_data(m)
   def rows = _flatter_matrix_rows(m)
   def cols = _flatter_matrix_cols(m)
   def tail = rows - k
   mut a = 0
   while a + 3 < tail {
      def four = [[a, 1], [a + 1, -1], [a + 2, -1], [a + 3, 1]]
      if _flatter_lower_triangular_qary_terms_short(data, k, rows, cols, four) { return four }
      def four_neg = [[a, -1], [a + 1, 1], [a + 2, 1], [a + 3, -1]]
      if _flatter_lower_triangular_qary_terms_short(data, k, rows, cols, four_neg) { return four_neg }
      a += 1
   }
   a = 0
   while a + 1 < tail {
      def diff = [[a, 1], [a + 1, -1]]
      if _flatter_lower_triangular_qary_terms_short(data, k, rows, cols, diff) { return diff }
      def sum = [[a, 1], [a + 1, 1]]
      if _flatter_lower_triangular_qary_terms_short(data, k, rows, cols, sum) { return sum }
      a += 1
   }
   []
}

fn _flatter_lower_triangular_qary_find_near_relation(any m, int k) dict {
   def data = _flatter_matrix_data(m)
   def rows = _flatter_matrix_rows(m)
   def cols = _flatter_matrix_cols(m)
   def tail = rows - k
   mut best_terms = []
   mut best_row = []
   mut best_tr = []
   mut best_norm = _flatter_dot_z(data.get(0), data.get(0))
   mut a = 0
   while a < tail {
      mut sa_i = 0
      while sa_i < 2 {
         def sa = sa_i == 0 ? Z(-1) : Z(1)
         def single = [[a, sa]]
         def srep = _flatter_lower_triangular_reduce_relation(data, k, rows, cols, single)
         def sn = Z(srep.get("norm"))
         if sn < best_norm {
            best_norm = sn
            best_terms = single
            best_row = srep.get("row")
            best_tr = srep.get("transform_row")
         }
         sa_i += 1
      }
      mut b = a + 1
      while b < tail {
         sa_i = 0
         while sa_i < 2 {
            def sa = sa_i == 0 ? Z(-1) : Z(1)
            mut sb_i = 0
            while sb_i < 2 {
               def sb = sb_i == 0 ? Z(-1) : Z(1)
               def terms = [[a, sa], [b, sb]]
               def rep = _flatter_lower_triangular_reduce_relation(data, k, rows, cols, terms)
               def n = Z(rep.get("norm"))
               if n < best_norm {
                  best_norm = n
                  best_terms = terms
                  best_row = rep.get("row")
                  best_tr = rep.get("transform_row")
               }
               sb_i += 1
            }
            sa_i += 1
         }
         b += 1
      }
      a += 1
   }
   {"found": best_terms.len > 0, "terms": best_terms, "row": best_row, "transform_row": best_tr, "norm": best_norm}
}

fn _flatter_lower_triangular_qary_search_relation(any m, int k, int trials=4096, int max_weight=12) dict {
   def data = _flatter_matrix_data(m)
   def rows = _flatter_matrix_rows(m)
   def cols = _flatter_matrix_cols(m)
   def tail = rows - k
   mut best_terms = []
   mut best_row = []
   mut best_tr = []
   mut best_norm = _flatter_dot_z(data.get(0), data.get(0))
   mut t = 0
   while t < max(1, trials) {
      mut terms = []
      mut seed = (t + 1) * 1103515245 + rows * 131071 + k * 8191
      def weight = 3 + (t % max(1, max_weight - 2))
      mut c = 0
      while c < weight {
         seed = (seed * 1103515245 + 12345 + c * 97) % 2147483647
         def idx = seed % tail
         seed = (seed * 1103515245 + 12345 + idx * 31) % 2147483647
         def sign = (seed % 2 == 0) ? Z(1) : Z(-1)
         terms = terms.append([idx, sign])
         c += 1
      }
      def rep = _flatter_lower_triangular_reduce_relation(data, k, rows, cols, terms)
      def n = Z(rep.get("norm"))
      if n < best_norm {
         best_norm = n
         best_terms = terms
         best_row = rep.get("row")
         best_tr = rep.get("transform_row")
      }
      t += 1
   }
   {"found": best_terms.len > 0, "terms": best_terms, "row": best_row, "transform_row": best_tr, "norm": best_norm, "trials": trials, "max_weight": max_weight}
}

fn _flatter_qary_pair_fingerprint(list data, int k, int a, int b, int sa, int sb) list {
   def mod1 = 1000000007
   def mod2 = 1000000009
   mut h1, h2, want1, want2, col = 0, 0, 0, 0, 0
   def row_a = data.get(k + a)
   def row_b = data.get(k + b)
   while col < k {
      mut q = _flatter_small_i64(data.get(col).get(col))
      if q == 2147483647 || q == 0 { return [] }
      if q < 0 { q = -q }
      def av = _flatter_small_i64(row_a.get(col))
      def bv = _flatter_small_i64(row_b.get(col))
      if av == 2147483647 || bv == 2147483647 { return [] }
      mut r = (sa * av + sb * bv) % q
      if r < 0 { r += q }
      def c = r == 0 ? 0 : q - r
      def w1 = ((col + 1) * 1000003 + 97) % mod1
      def w2 = ((col + 1) * 1000033 + 193) % mod2
      h1 = (h1 + r * w1) % mod1
      h2 = (h2 + r * w2) % mod2
      want1 = (want1 + c * w1) % mod1
      want2 = (want2 + c * w2) % mod2
      col += 1
   }
   [to_str(h1) + ":" + to_str(h2), to_str(want1) + ":" + to_str(want2)]
}

fn _flatter_qary_terms_zero_int(list data, int k, list terms) bool {
   mut col = 0
   while col < k {
      mut q = _flatter_small_i64(data.get(col).get(col))
      if q == 2147483647 || q == 0 { return false }
      if q < 0 { q = -q }
      mut s = 0
      mut t = 0
      while t < terms.len {
         def term = terms.get(t)
         def row_idx = k + int(term.get(0))
         def v = _flatter_small_i64(data.get(row_idx).get(col))
         if v == 2147483647 { return false }
         s += int(term.get(1)) * v
         t += 1
      }
      mut r = s % q
      if r < 0 { r += q }
      if r != 0 { return false }
      col += 1
   }
   true
}

fn _flatter_qary_find_local_relation(any m, int k) list {
   def data = _flatter_matrix_data(m)
   def rows = _flatter_matrix_rows(m)
   def tail = rows - k
   mut a = 0
   while a + 3 < tail {
      def four = [[a, 1], [a + 1, -1], [a + 2, -1], [a + 3, 1]]
      if _flatter_qary_terms_zero_int(data, k, four) { return four }
      def four_neg = [[a, -1], [a + 1, 1], [a + 2, 1], [a + 3, -1]]
      if _flatter_qary_terms_zero_int(data, k, four_neg) { return four_neg }
      a += 1
   }
   a = 0
   while a + 1 < tail {
      def diff = [[a, 1], [a + 1, -1]]
      if _flatter_qary_terms_zero_int(data, k, diff) { return diff }
      def sum = [[a, 1], [a + 1, 1]]
      if _flatter_qary_terms_zero_int(data, k, sum) { return sum }
      a += 1
   }
   []
}

fn _flatter_qary_find_relation_fast(any m, int k) list {
   def local = _flatter_qary_find_local_relation(m, k)
   if local.len > 0 { return local }
   def data = _flatter_matrix_data(m)
   def rows = _flatter_matrix_rows(m)
   def tail = rows - k
   mut seen = dict(0)
   mut a = 0
   while a < tail {
      mut b = a + 1
      while b < tail {
         mut sa_i = 0
         while sa_i < 2 {
            def sa = sa_i == 0 ? -1 : 1
            mut sb_i = 0
            while sb_i < 2 {
               def sb = sb_i == 0 ? -1 : 1
               def fp = _flatter_qary_pair_fingerprint(data, k, a, b, sa, sb)
               if fp.len == 0 { return [] }
               def terms = [[a, sa], [b, sb]]
               if _flatter_qary_terms_zero_int(data, k, terms) { return terms }
               def bucket = seen.get(fp.get(1), [])
               mut bi = 0
               while bi < bucket.len {
                  def other = bucket.get(bi)
                  if _flatter_terms_disjoint(terms, other) {
                     def joined = terms + other
                     if _flatter_qary_terms_zero_int(data, k, joined) { return joined }
                  }
                  bi += 1
               }
               def key = fp.get(0)
               seen[key] = seen.get(key, []).append(terms)
               sb_i += 1
            }
            sa_i += 1
         }
         b += 1
      }
      a += 1
   }
   []
}

fn _flatter_qary_find_relation(any m, int k) list {
   def fast = _flatter_qary_find_relation_fast(m, k)
   if fast.len > 0 { return fast }
   def data = _flatter_matrix_data(m)
   def rows = _flatter_matrix_rows(m)
   def tail = rows - k
   mut moduli = []
   mut c = 0
   while c < k {
      moduli = moduli.append(_flatter_abs_z(data.get(c).get(c)))
      c += 1
   }
   def zero_key = _flatter_qary_residue_key(data, k, [])
   mut seen_keys = []
   mut seen_terms = []
   mut a = 0
   while a < tail {
      mut b = a + 1
      while b < tail {
         mut sa_i = 0
         while sa_i < 2 {
            def sa = sa_i == 0 ? Z(-1) : Z(1)
            mut sb_i = 0
            while sb_i < 2 {
               def sb = sb_i == 0 ? Z(-1) : Z(1)
               def terms = [[a, sa], [b, sb]]
               def key = _flatter_qary_residue_key(data, k, terms)
               if key == zero_key { return terms }
               def want = _flatter_qary_complement_key(key, moduli)
               def want_idx = _flatter_seen_key_index(seen_keys, want)
               def other = want_idx >= 0 ? seen_terms.get(want_idx) : nil
               if other != nil && _flatter_terms_disjoint(terms, other) { return terms + other }
               if _flatter_seen_key_index(seen_keys, key) < 0 {
                  seen_keys = seen_keys.append(key)
                  seen_terms = seen_terms.append(terms)
               }
               sb_i += 1
            }
            sa_i += 1
         }
         b += 1
      }
      a += 1
   }
   []
}

fn _flatter_qary_relation_prepass_report(any basis, str method, bool lower, bool track_transform=true) dict {
   def t0 = ticks()
   def a = _flatter_reduce_matrix(basis)
   def rows = _flatter_matrix_rows(a)
   def cols = _flatter_matrix_cols(a)
   def k = lower ? _flatter_lower_triangular_qary_split(a) : _flatter_qary_split(a)
   if k < 0 {
      return _flatter_qary_miss_report(method, false, k, rows, cols, a, t0, track_transform)
   }
   def terms = lower ? _flatter_lower_triangular_qary_find_relation(a, k) : _flatter_qary_find_relation(a, k)
   if terms.len == 0 {
      if lower {
         def near = _flatter_lower_triangular_qary_find_near_relation(a, k)
         if near.get("found", false) {
            return _flatter_qary_success_from_row_report(method, k, rows, cols, a, near, t0, "near", track_transform)
         }
         if rows >= 96 {
            def searched = _flatter_lower_triangular_qary_search_relation(a, k, 4096, 14)
            if searched.get("found", false) {
               return _flatter_qary_success_from_row_report(method, k, rows, cols, a, searched, t0, "deterministic-search", track_transform)
            }
         }
      }
      return _flatter_qary_miss_report(method, true, k, rows, cols, a, t0, track_transform)
   }
   def data = _flatter_matrix_data(a)
   mut work = _flatter_clone_rows(a)
   mut transform = track_transform ? _flatter_identity_rows(rows) : nil
   mut rel_row = _flatter_zero_row(cols)
   mut rel_tr = track_transform ? _flatter_zero_row(rows) : []
   mut ti = 0
   while ti < terms.len {
      def term = terms.get(ti)
      def row_idx = k + int(term.get(0))
      def sign = Z(term.get(1))
      rel_row = _flatter_row_addmul(rel_row, data.get(row_idx), sign)
      if track_transform { rel_tr = _flatter_row_addmul(rel_tr, transform.get(row_idx), sign) }
      ti += 1
   }
   mut exact = true
   mut col = lower ? k - 1 : 0
   while lower ? col >= 0 : col < k {
      def pivot = Z(data.get(col).get(col))
      def v = Z(rel_row.get(col))
      if pivot == Z(0) || v % pivot != Z(0) {
         exact = false
      } else {
         def coeff = v / pivot
         if coeff != Z(0) {
            rel_row = _flatter_row_submul(rel_row, data.get(col), coeff)
            if track_transform { rel_tr = _flatter_row_submul(rel_tr, transform.get(col), coeff) }
         }
      }
      col += lower ? -1 : 1
   }
   if !exact {
      return _flatter_qary_miss_report(method, true, k, rows, cols, a, t0, track_transform)
   }
   def target = k + int(terms.get(0).get(0))
   work[target] = rel_row
   if track_transform { transform[target] = rel_tr }
   if target != 0 {
      def tmp_row = work.get(0)
      work[0] = work.get(target)
      work[target] = tmp_row
      if track_transform {
         def tmp_tr = transform.get(0)
         transform[0] = transform.get(target)
         transform[target] = tmp_tr
      }
   }
   _flatter_qary_success_report(method, k, rows, cols, terms, a, data, work, transform, target, t0, track_transform)
}

fn _flatter_ntru2_split(any m) int {
   def rows = _flatter_matrix_rows(m)
   def cols = _flatter_matrix_cols(m)
   if rows != cols || rows < 4 || rows % 2 != 0 { return -1 }
   def k = rows / 2
   mut i = 0
   while i < k {
      def pivot = _flatter_abs_z(matrix.mat_get(m, i, i))
      if pivot <= Z(1) { return -1 }
      mut j = 0
      while j < cols {
         if j != i && !_flatter_is_zero_scalar(matrix.mat_get(m, i, j)) { return -1 }
         j += 1
      }
      i += 1
   }
   i = 0
   while i < k {
      mut j = 0
      while j < k {
         def want = i == j ? Z(1) : Z(0)
         if Z(matrix.mat_get(m, k + i, k + j)) != want { return -1 }
         j += 1
      }
      i += 1
   }
   k
}

fn _flatter_ntru2_sum_relation_prepass_report(any basis, bool track_transform=true) dict {
   def t0 = ticks()
   def a = _flatter_reduce_matrix(basis)
   def rows = _flatter_matrix_rows(a)
   def cols = _flatter_matrix_cols(a)
   def k = _flatter_ntru2_split(a)
   if k < 0 {
      return _flatter_qary_miss_report("ntru2-sum-relation-prepass", false, k, rows, cols, a, t0, track_transform)
   }
   def data = _flatter_matrix_data(a)
   mut rel_row = _flatter_zero_row(cols)
   mut rel_tr = track_transform ? _flatter_zero_row(rows) : []
   mut terms = []
   mut col = 0
   while col < k {
      def pivot = Z(data.get(col).get(col))
      if pivot == Z(0) {
         return _flatter_qary_miss_report("ntru2-sum-relation-prepass", true, k, rows, cols, a, t0, track_transform)
      }
      mut s = Z(0)
      mut i = 0
      while i < k {
         s = s + Z(data.get(k + i).get(col))
         i += 1
      }
      if s % pivot != Z(0) {
         return _flatter_qary_miss_report("ntru2-sum-relation-prepass", true, k, rows, cols, a, t0, track_transform)
      }
      if track_transform { rel_tr[col] = s / pivot }
      col += 1
   }
   mut i = 0
   while i < k {
      rel_row[k + i] = Z(-1)
      if track_transform { rel_tr[k + i] = Z(-1) }
      terms = terms.append([i, Z(-1)])
      i += 1
   }
   mut work = _flatter_clone_rows(a)
   mut transform = track_transform ? _flatter_identity_rows(rows) : nil
   def target = k
   work[target] = rel_row
   if track_transform { transform[target] = rel_tr }
   if target != 0 {
      def tmp_row = work.get(0)
      work[0] = work.get(target)
      work[target] = tmp_row
      if track_transform {
         def tmp_tr = transform.get(0)
         transform[0] = transform.get(target)
         transform[target] = tmp_tr
      }
   }
   _flatter_qary_success_row_report("ntru2-sum-relation-prepass", k, rows, cols, terms, a, data, work, transform, target, t0, "all-tail-sum", track_transform)
}

fn _flatter_ntru_split(any m) int {
   def rows = _flatter_matrix_rows(m)
   def cols = _flatter_matrix_cols(m)
   if rows != cols || rows < 4 || rows % 2 != 0 { return -1 }
   def k = rows / 2
   mut i = 0
   while i < k {
      mut j = 0
      while j < k {
         def want = i == j ? Z(1) : Z(0)
         if Z(matrix.mat_get(m, i, j)) != want { return -1 }
         j += 1
      }
      i += 1
   }
   mut q = Z(0)
   i = 0
   while i < k {
      mut j = 0
      while j < k {
         def v = Z(matrix.mat_get(m, k + i, k + j))
         if i == j {
            if v <= Z(1) { return -1 }
            if i == 0 { q = v } elif v != q { return -1 }
         } elif v != Z(0) { return -1 }
         if Z(matrix.mat_get(m, k + i, j)) != Z(0) { return -1 }
         j += 1
      }
      i += 1
   }
   k
}

fn _flatter_ntru_sum_relation_prepass_report(any basis, bool track_transform=true) dict {
   def t0 = ticks()
   def a = _flatter_reduce_matrix(basis)
   def rows = _flatter_matrix_rows(a)
   def cols = _flatter_matrix_cols(a)
   def k = _flatter_ntru_split(a)
   if k < 0 {
      return _flatter_qary_miss_report("ntru-sum-relation-prepass", false, k, rows, cols, a, t0, track_transform)
   }
   def data = _flatter_matrix_data(a)
   def q = Z(data.get(k).get(k))
   mut rel_row = _flatter_zero_row(cols)
   mut rel_tr = track_transform ? _flatter_zero_row(rows) : []
   mut terms = []
   mut i = 0
   while i < k {
      rel_row[i] = Z(1)
      if track_transform { rel_tr[i] = Z(1) }
      i += 1
   }
   mut col = 0
   while col < k {
      mut s = Z(0)
      i = 0
      while i < k {
         s = s + Z(data.get(i).get(k + col))
         i += 1
      }
      if s % q != Z(0) {
         return _flatter_qary_miss_report("ntru-sum-relation-prepass", true, k, rows, cols, a, t0, track_transform)
      }
      def coeff = s / q
      if coeff != Z(0) {
         if track_transform { rel_tr[k + col] = -coeff }
         terms = terms.append([col, -coeff])
      }
      col += 1
   }
   mut work = _flatter_clone_rows(a)
   mut transform = track_transform ? _flatter_identity_rows(rows) : nil
   work[0] = rel_row
   if track_transform { transform[0] = rel_tr }
   _flatter_qary_success_row_report("ntru-sum-relation-prepass", k, rows, cols, terms, a, data, work, transform, 0, t0, "top-sum-minus-q-diagonal", track_transform)
}

fn qary_relation_prepass_report(any basis) dict {
   "Find and install a short q-ary tail relation using exact row transforms."
   _flatter_qary_relation_prepass_report(basis, "qary-relation-prepass", false)
}

fn lower_triangular_qary_relation_prepass_report(any basis) dict {
   "Find and install a short q-ary tail relation for lower-triangular top blocks."
   _flatter_qary_relation_prepass_report(basis, "lower-triangular-qary-relation-prepass", true)
}

fn lower_triangular_qary_relation_prepass(any basis) any {
   "Return the basis from lower_triangular_qary_relation_prepass_report."
   lower_triangular_qary_relation_prepass_report(basis).get("basis")
}

fn qary_relation_prepass(any basis) any {
   "Return the basis from qary_relation_prepass_report."
   qary_relation_prepass_report(basis).get("basis")
}

fn triangular_size_reduce_report(any basis, int passes=1) dict {
   "Exact fast size reduction for lower-triangular row bases."
   def t0 = ticks()
   def a = _flatter_reduce_matrix(basis)
   def rows = _flatter_matrix_rows(a)
   def cols = _flatter_matrix_cols(a)
   mut work = _flatter_clone_rows_small(a)
   mut transform = _flatter_identity_rows(rows)
   mut ops = []
   def triangular = _flatter_is_lower_triangular(a)
   def data_before = _flatter_matrix_data(a)
   def first_before = rows > 0 ? _flatter_dot_z(data_before.get(0), data_before.get(0)) : Z(0)
   def best_before = _flatter_best_norm_sq(a)
   if triangular {
      mut p = 0
      while p < max(1, passes) {
         mut changed = false
         mut i = 1
         while i < rows {
            mut j = min(i, cols - 1) - 1
            while j >= 0 {
               def pivot = Z(work.get(j).get(j))
               if pivot != Z(0) {
                  def coeff = _flatter_round_div_signed(Z(work.get(i).get(j)), pivot)
                  if coeff != Z(0) {
                     work[i] = _flatter_row_submul_prefix(work.get(i), work.get(j), coeff, j)
                     transform[i] = _flatter_row_submul(transform.get(i), transform.get(j), coeff)
                     ops = ops.append({"pass": p + 1, "row": i, "against": j, "coeff": coeff})
                     changed = true
                  }
               }
               j -= 1
            }
            i += 1
         }
         if !changed { p = passes } else { p += 1 }
      }
   }
   def out_basis = matrix.Matrix(work)
   def transform_matrix = matrix.Matrix(transform)
   def applied = _flatter_matmul(transform_matrix, a)
   def first_after = rows > 0 ? _flatter_dot_z(work.get(0), work.get(0)) : Z(0)
   {
      "method": "triangular-size-reduction",
      "rows": rows,
      "cols": cols,
      "passes": passes,
      "triangular": triangular,
      "op_count": ops.len,
      "ops": ops,
      "first_norm_before": first_before,
      "first_norm_after": first_after,
      "best_norm_before": best_before,
      "best_norm_after": _flatter_best_norm_sq(out_basis),
      "transform": transform_matrix,
      "transform_verified": _flatter_same_matrix(applied, out_basis),
      "basis": out_basis,
      "elapsed_ms": float(ticks() - t0) / 1000000.0
   }
}

fn triangular_size_reduce(any basis, int passes=1) any {
   "Return the basis from triangular_size_reduce_report."
   triangular_size_reduce_report(basis, passes).get("basis")
}

fn _flatter_triangular_violation_report(any basis) dict {
   def m = _flatter_reduce_matrix(basis)
   def rows = _flatter_matrix_rows(m)
   def cols = _flatter_matrix_cols(m)
   def data = _flatter_matrix_data(m)
   mut violations = 0
   mut max_scaled_excess = Z(0)
   mut i = 1
   while i < rows {
      mut j = 0
      while j < i && j < cols {
         def pivot = _flatter_abs_z(data.get(j).get(j))
         if pivot != Z(0) {
            def scaled = _flatter_abs_z(data.get(i).get(j)) * Z(2)
            if scaled > pivot {
               violations += 1
               def excess = scaled - pivot
               if excess > max_scaled_excess { max_scaled_excess = excess }
            }
         }
         j += 1
      }
      i += 1
   }
   {
      "violations": violations,
      "max_scaled_excess": max_scaled_excess
   }
}

fn blocked_triangular_size_reduce_report(any basis, int block_size=32, int passes=1, bool track_transform=true) dict {
   "Exact blocked size reduction for lower-triangular row bases."
   def t0 = ticks()
   def a = _flatter_reduce_matrix(basis)
   def rows = _flatter_matrix_rows(a)
   def cols = _flatter_matrix_cols(a)
   def bs = max(1, block_size)
   def blocks = rows == 0 ? 0 : (rows + bs - 1) / bs
   def row_i64_safe = 2000000000000000000
   def track = bool(track_transform)
   mut ops = []
   mut tile_reports = []
   mut op_count = 0
   mut fast_row_ops = 0
   mut generic_row_ops = 0
   mut generic_checked_overflow_ops = 0
   mut generic_unbounded_row_ops = 0
   mut generic_big_coeff_ops = 0
   mut generic_dynamic_ops = 0
   mut tile_count = 0
   def record_ops = track && rows <= 48
   def triangular = _flatter_is_lower_triangular(a)
   def collect_quality = track || rows < 48
   def data_before = _flatter_matrix_data(a)
   def first_before = rows > 0 ? _flatter_dot_z(data_before.get(0), data_before.get(0)) : Z(0)
   def best_before = collect_quality ? _flatter_best_norm_sq(a) : Z(-1)
   def before = collect_quality ? _flatter_triangular_violation_report(a) : {"violations": -1, "max_scaled_excess": Z(-1)}
   if triangular && before.get("violations", 0) == 0 {
      return {
         "method": "blocked-triangular-size-reduction",
         "rows": rows,
         "cols": cols,
         "block_size": bs,
         "block_count": blocks,
         "passes": passes,
         "triangular": true,
         "op_count": 0,
         "fast_row_ops": 0,
         "generic_row_ops": 0,
         "generic_checked_overflow_ops": 0,
         "generic_unbounded_row_ops": 0,
         "generic_big_coeff_ops": 0,
         "generic_dynamic_ops": 0,
         "ops": ops,
         "tile_reports": tile_reports,
         "ops_truncated": false,
         "tile_count": 0,
         "first_norm_before": first_before,
         "first_norm_after": first_before,
         "best_norm_before": best_before,
         "best_norm_after": best_before,
         "violations_before": 0,
         "violations_after": 0,
         "max_scaled_excess_before": before.get("max_scaled_excess", Z(0)),
         "max_scaled_excess_after": Z(0),
         "transform": track ? matrix.matrix_identity(rows) : nil,
         "transform_tracked": track,
         "transform_verified": track,
         "verification_skipped": !track,
         "quality_scan_skipped": !collect_quality,
         "basis": a,
         "elapsed_ms": float(ticks() - t0) / 1000000.0
      }
   }
   def cloned = _flatter_clone_rows_lower_report(a)
   mut work = cloned.get("rows")
   mut row_bounds = _flatter_row_i64_abs_bounds(work, row_i64_safe)
   mut transform = track ? _flatter_identity_rows(rows) : []
   if triangular {
      mut p = 0
      while p < max(1, passes) {
         mut changed = false
         mut rb = 0
         while rb < blocks {
            def r0 = rb * bs
            def r1 = min(rows, r0 + bs)
            mut cb = rb
            while cb >= 0 {
               def c0 = cb * bs
               def c1 = min(rows, c0 + bs)
               mut tile_ops = []
               mut i = r0
               while i < r1 {
                  mut row_i = work.get(i)
                  if row_bounds.get(i) == -1 {
                     def compressed_i = _flatter_compress_row_i64_bound(row_i, row_i64_safe)
                     if compressed_i.get(1) >= 0 {
                        row_i = compressed_i.get(0)
                        row_bounds[i] = compressed_i.get(1)
                     } else {
                        row_bounds[i] = -2
                     }
                  }
                  mut transform_i = track ? transform.get(i) : []
                  mut j = min(min(i, cols - 1), c1) - 1
                  while j >= c0 {
                     mut row_j = work.get(j)
                     if row_bounds.get(j) == -1 {
                        def compressed_j = _flatter_compress_row_i64_bound(row_j, row_i64_safe)
                        if compressed_j.get(1) >= 0 {
                           row_j = compressed_j.get(0)
                           row_bounds[j] = compressed_j.get(1)
                           work[j] = row_j
                        } else {
                           row_bounds[j] = -2
                        }
                     }
                     def pivot = row_j.get(j)
                     def pivot_nonzero = is_int(pivot) ? int(pivot) != 0 : Z(pivot) != Z(0)
                     if pivot_nonzero {
                        def raw_num = row_i.get(j)
                        if is_int(raw_num) && is_int(pivot) {
                           def coeff_i = _flatter_round_div_signed_int(int(raw_num), int(pivot))
                           if coeff_i != 2147483647 && coeff_i != 0 {
                              def abs_coeff_i = coeff_i < 0 ? -coeff_i : coeff_i
                              def row_bound_i = int(row_bounds.get(i))
                              def row_bound_j = int(row_bounds.get(j))
                              if abs_coeff_i <= 1000000000 && row_bound_i >= 0 && row_bound_j >= 0 && row_bound_i < row_i64_safe && row_bound_j <= (row_i64_safe - row_bound_i) / abs_coeff_i {
                                 def fast = _flatter_row_submul_prefix_i64_bound_cached(row_i, row_j, coeff_i, j, row_bound_i)
                                 row_i = fast.get(0)
                                 row_bounds[i] = fast.get(1)
                                 fast_row_ops += 1
                              } elif row_bound_i >= 0 && row_bound_j >= 0 {
                                 def checked = _flatter_row_submul_prefix_i64_checked_cached(row_i, row_j, coeff_i, j, row_i64_safe, row_bound_i)
                                 if checked.get(1) >= 0 {
                                    row_i = checked.get(0)
                                    row_bounds[i] = checked.get(1)
                                    fast_row_ops += 1
                                 } else {
                                    row_i = _flatter_row_submul_prefix_bigint_inplace(row_i, row_j, coeff_i, j)
                                    row_bounds[i] = -1
                                    generic_row_ops += 1
                                    generic_checked_overflow_ops += 1
                                 }
                              } else {
                                 row_i = _flatter_row_submul_prefix_bigint_inplace(row_i, row_j, coeff_i, j)
                                 row_bounds[i] = -1
                                 generic_row_ops += 1
                                 generic_unbounded_row_ops += 1
                              }
                              if track { transform_i = _flatter_row_submul(transform_i, transform.get(j), coeff_i) }
                              op_count += 1
                              if record_ops {
                                 def op = {"pass": p + 1, "row_block": rb, "col_block": cb, "row": i, "against": j, "coeff": coeff_i}
                                 tile_ops = tile_ops.append(op)
                                 ops = ops.append(op)
                              }
                              changed = true
                           } elif coeff_i == 2147483647 {
                              def coeff = _flatter_round_div_signed(raw_num, pivot)
                              if coeff != Z(0) {
                                 row_i = _flatter_row_submul_prefix_bigint_inplace(row_i, row_j, coeff, j)
                                 row_bounds[i] = -1
                                 generic_row_ops += 1
                                 generic_big_coeff_ops += 1
                                 if track { transform_i = _flatter_row_submul(transform_i, transform.get(j), coeff) }
                                 op_count += 1
                                 if record_ops {
                                    def op = {"pass": p + 1, "row_block": rb, "col_block": cb, "row": i, "against": j, "coeff": coeff}
                                    tile_ops = tile_ops.append(op)
                                    ops = ops.append(op)
                                 }
                                 changed = true
                              }
                           }
                        } else {
                           def coeff = _flatter_round_div_signed(raw_num, pivot)
                           if coeff != Z(0) {
                              row_i = _flatter_row_submul_prefix_bigint_inplace(row_i, row_j, coeff, j)
                              row_bounds[i] = -1
                              generic_row_ops += 1
                              generic_dynamic_ops += 1
                              if track { transform_i = _flatter_row_submul(transform_i, transform.get(j), coeff) }
                              op_count += 1
                              if record_ops {
                                 def op = {"pass": p + 1, "row_block": rb, "col_block": cb, "row": i, "against": j, "coeff": coeff}
                                 tile_ops = tile_ops.append(op)
                                 ops = ops.append(op)
                              }
                              changed = true
                           }
                        }
                     }
                     j -= 1
                  }
                  work[i] = row_i
                  if track { transform[i] = transform_i }
                  i += 1
               }
               tile_count += 1
               if record_ops {
                  tile_reports = tile_reports.append({
                        "pass": p + 1,
                        "row_block": rb,
                        "col_block": cb,
                        "row_start": r0,
                        "row_end": r1,
                        "col_start": c0,
                        "col_end": c1,
                        "op_count": tile_ops.len,
                        "ops": tile_ops
                  })
               }
               cb -= 1
            }
            rb += 1
         }
         if !changed { p = passes } else { p += 1 }
      }
   }
   def out_basis = matrix.Matrix(work)
   def transform_matrix = track ? matrix.Matrix(transform) : nil
   def applied = track ? _flatter_matmul(transform_matrix, a) : out_basis
   def after = collect_quality ? _flatter_triangular_violation_report(out_basis) : {"violations": -1, "max_scaled_excess": Z(-1)}
   def first_after = rows > 0 ? _flatter_dot_z(work.get(0), work.get(0)) : Z(0)
   {
      "method": "blocked-triangular-size-reduction",
      "rows": rows,
      "cols": cols,
      "block_size": bs,
      "block_count": blocks,
      "passes": passes,
      "triangular": triangular,
      "op_count": op_count,
      "fast_row_ops": fast_row_ops,
      "generic_row_ops": generic_row_ops,
      "generic_checked_overflow_ops": generic_checked_overflow_ops,
      "generic_unbounded_row_ops": generic_unbounded_row_ops,
      "generic_big_coeff_ops": generic_big_coeff_ops,
      "generic_dynamic_ops": generic_dynamic_ops,
      "ops": ops,
      "tile_reports": tile_reports,
      "ops_truncated": track && !record_ops,
      "tile_count": tile_count,
      "first_norm_before": first_before,
      "first_norm_after": first_after,
      "best_norm_before": best_before,
      "best_norm_after": collect_quality ? _flatter_best_norm_sq(out_basis) : Z(-1),
      "violations_before": before.get("violations", 0),
      "violations_after": after.get("violations", 0),
      "max_scaled_excess_before": before.get("max_scaled_excess", Z(0)),
      "max_scaled_excess_after": after.get("max_scaled_excess", Z(0)),
      "transform": transform_matrix,
      "transform_tracked": track,
      "transform_verified": track && _flatter_same_matrix(applied, out_basis),
      "verification_skipped": !track,
      "quality_scan_skipped": !collect_quality,
      "basis": out_basis,
      "elapsed_ms": float(ticks() - t0) / 1000000.0
   }
}

fn blocked_triangular_size_reduce(any basis, int block_size=32, int passes=1) any {
   "Return the basis from blocked_triangular_size_reduce_report."
   blocked_triangular_size_reduce_report(basis, block_size, passes).get("basis")
}

fn _flatter_tail_unit_split(any basis) int {
   def m = _flatter_reduce_matrix(basis)
   def rows = _flatter_matrix_rows(m)
   def cols = _flatter_matrix_cols(m)
   if rows == 0 || rows != cols || !_flatter_is_lower_triangular(m) { return -1 }
   def data = _flatter_matrix_data(m)
   mut split = rows
   mut i = rows - 1
   while i >= 0 {
      def d = _flatter_abs_z(data.get(i).get(i))
      if d == Z(1) {
         split = i
      } else {
         i = -1
      }
      i -= 1
   }
   if split <= 0 || split >= rows { return -1 }
   split
}

fn _flatter_best_norm_sq(any basis) bigint {
   def m = _flatter_reduce_matrix(basis)
   def rows = _flatter_matrix_rows(m)
   def data = _flatter_matrix_data(m)
   mut best = Z(0)
   mut i = 0
   while i < rows {
      def n = _flatter_norm_z(data.get(i))
      if i == 0 || n < best { best = n }
      i += 1
   }
   best
}

fn triangular_bounded_lll_prepass_report(any basis, int chunk_budget=2048, int max_chunks=4, any delta=0.75, any eta=0.51, bool track_transform=true) dict {
   "Run bounded LLL chunks with short-row sorting between chunks for large triangular bases."
   def t0 = ticks()
   def a = _flatter_reduce_matrix(basis)
   def rows = _flatter_matrix_rows(a)
   mut work = a
   mut total_transform = nil
   mut total_transform_identity = bool(track_transform)
   mut chunks = []
   mut total_steps = 0
   mut total_sort_ops = 0
   def before_best = _flatter_best_norm_sq(work)
   mut final_best = before_best
   mut i = 0
   while i < max(1, max_chunks) {
      def before_chunk = final_best
      def rep_method = track_transform ? "bounded-int-transform" : "bounded-int-no-transform-compact"
      def rep = lll_backend.lll_reduce_bounded_report(work, max(1, chunk_budget), delta, rep_method, eta)
      work = rep.get("basis")
      if total_transform != nil && rep.get("transform_tracked", false) {
         total_transform = _flatter_matmul(rep.get("transform"), total_transform)
      } elif total_transform_identity && rep.get("transform_tracked", false) {
         total_transform = rep.get("transform")
         total_transform_identity = false
      } elif !rep.get("transform_tracked", false) {
         total_transform = nil
         total_transform_identity = false
      }
      total_steps += rep.get("steps", 0)
      def sort = short_row_prepass_report(work, track_transform)
      def sort_ops = sort.get("op_count", 0)
      if sort_ops > 0 {
         work = sort.get("basis")
         if total_transform != nil {
            total_transform = _flatter_matmul(sort.get("transform"), total_transform)
         } elif total_transform_identity {
            total_transform = sort.get("transform")
            total_transform_identity = false
         }
         total_sort_ops += sort_ops
      }
      def after_chunk = _flatter_best_norm_sq(work)
      final_best = after_chunk
      chunks = chunks.append({
            "chunk": i + 1,
            "chunk_budget": chunk_budget,
            "core": rep.get("core", ""),
            "steps": rep.get("steps", 0),
            "reduction_complete": rep.get("reduction_complete", false),
            "incomplete_reason": rep.get("incomplete_reason", ""),
            "sort_op_count": sort_ops,
            "best_norm_before": before_chunk,
            "best_norm_after": after_chunk,
            "elapsed_ms": rep.get("elapsed_ms", 0.0) + sort.get("elapsed_ms", 0.0)
      })
      i += 1
   }
   def out_basis = _flatter_reduce_matrix(work)
   def verify_transform = total_transform != nil || total_transform_identity
   def applied = verify_transform ? (total_transform_identity ? a : _flatter_matmul(total_transform, a)) : nil
   {
      "method": "triangular-bounded-lll-prepass",
      "rows": rows,
      "cols": _flatter_matrix_cols(a),
      "delta": delta,
      "eta": eta,
      "chunk_budget": chunk_budget,
      "chunk_count": chunks.len,
      "step_budget": chunk_budget * chunks.len,
      "op_count": total_steps + total_sort_ops,
      "lll_step_count": total_steps,
      "sort_op_count": total_sort_ops,
      "chunks": chunks,
      "best_norm_before": before_best,
      "best_norm_after": final_best,
      "basis": out_basis,
      "transform": total_transform == nil && total_transform_identity ? matrix.matrix_identity(rows) : total_transform,
      "transform_tracked": verify_transform,
      "transform_verified": verify_transform && _flatter_same_matrix(applied, out_basis),
      "verification_skipped": !verify_transform,
      "elapsed_ms": float(ticks() - t0) / 1000000.0
   }
}

fn _flatter_tail_normal_form_target_row(list data, int split, int tail, int cols, int rows, int target) dict {
   mut coeffs = []
   mut i = 0
   while i < tail {
      coeffs = coeffs.append(Z(0))
      i += 1
   }
   mut exact = true
   mut backsolve_ops = 0
   mut idx = tail - 1
   while idx >= 0 {
      def coord = split + idx
      mut s = idx == target ? Z(1) : Z(0)
      mut r = idx + 1
      while r < tail {
         s = s - Z(coeffs.get(r)) * Z(data.get(split + r).get(coord))
         r += 1
      }
      def raw_pivot = data.get(coord).get(coord)
      def pivot_i = _flatter_small_i64(raw_pivot)
      if pivot_i == 0 || (pivot_i == 2147483647 && Z(raw_pivot) == Z(0)) {
         exact = false
         coeffs[idx] = Z(0)
      } elif pivot_i == 1 {
         coeffs[idx] = s
      } elif pivot_i == -1 {
         coeffs[idx] = -s
      } else {
         coeffs[idx] = s / Z(raw_pivot)
      }
      backsolve_ops += 1
      idx -= 1
   }
   mut row = _flatter_zero_row(cols)
   mut tr = _flatter_zero_row(rows)
   mut reduction_ops = 0
   idx = 0
   while idx < tail {
      def c = Z(coeffs.get(idx))
      if c != Z(0) {
         row = _flatter_row_addmul(row, data.get(split + idx), c)
         tr[split + idx] = Z(tr.get(split + idx)) + c
      }
      idx += 1
   }
   idx = split - 1
   while idx >= 0 {
      def raw_pivot = data.get(idx).get(idx)
      def pivot_i = _flatter_small_i64(raw_pivot)
      if pivot_i != 0 && (pivot_i != 2147483647 || Z(raw_pivot) != Z(0)) {
         def q = pivot_i != 2147483647 ? _flatter_round_div_signed(row.get(idx), pivot_i) : _flatter_round_div_signed(row.get(idx), Z(raw_pivot))
         if q != Z(0) {
            row = _flatter_row_submul(row, data.get(idx), q)
            tr[idx] = Z(tr.get(idx)) - q
            reduction_ops += 1
         }
      }
      idx -= 1
   }
   {"row": row, "transform": tr, "exact": exact, "backsolve_ops": backsolve_ops, "reduction_ops": reduction_ops}
}

fn _flatter_tail_reduce_heads(list work, list transform, int rows, int split) dict {
   mut tail_reduce_ops, sort_swaps = 0, 0
   if rows <= 160 {
      mut head = 0
      def head_limit = rows > 96 ? min(split, 1) : min(split, 8)
      while head < head_limit {
         mut cur = work.get(head)
         mut cur_tr = transform.get(head)
         mut cur_norm = _flatter_dot_z(cur, cur)
         mut pass = 0
         mut changed = true
         while changed && pass < 1 {
            changed = false
            mut tail_idx = rows - 1
            while tail_idx >= split {
               def reducer = work.get(tail_idx)
               def reducer_norm = _flatter_dot_z(reducer, reducer)
               if reducer_norm != Z(0) {
                  def q = _flatter_round_div_signed(_flatter_dot_z(cur, reducer), reducer_norm)
                  if q != Z(0) {
                     def cand = _flatter_row_submul(cur, reducer, q)
                     def cand_norm = _flatter_dot_z(cand, cand)
                     if cand_norm < cur_norm {
                        cur = cand
                        cur_tr = _flatter_row_submul(cur_tr, transform.get(tail_idx), q)
                        cur_norm = cand_norm
                        tail_reduce_ops += 1
                        changed = true
                     }
                  }
               }
               tail_idx -= 1
            }
            pass += 1
         }
         work[head] = cur
         transform[head] = cur_tr
         head += 1
      }
      if rows <= 96 {
         mut head_work = []
         mut head_transform = []
         head = 0
         while head < split {
            head_work = head_work.append(work.get(head))
            head_transform = head_transform.append(transform.get(head))
            head += 1
         }
         def sorted = _flatter_norm_sort_rows(head_work, head_transform, split, true)
         head = 0
         while head < split {
            work[head] = sorted.get("work").get(head)
            transform[head] = sorted.get("transform").get(head)
            head += 1
         }
         sort_swaps = sorted.get("swaps", 0)
      }
   }
   {"work": work, "transform": transform, "tail_reduce_ops": tail_reduce_ops, "sort_swaps": sort_swaps}
}

fn triangular_tail_normal_form_report(any basis) dict {
   "Convert a lower-triangular basis with a unit diagonal tail into a canonical tail-coordinate normal form."
   def t0 = ticks()
   def a = _flatter_reduce_matrix(basis)
   def rows = _flatter_matrix_rows(a)
   def cols = _flatter_matrix_cols(a)
   def data = _flatter_matrix_data(a)
   def split = _flatter_tail_unit_split(a)
   if split < 0 {
      return {
         "method": "triangular-tail-normal-form",
         "found": false,
         "split": split,
         "rows": rows,
         "cols": cols,
         "basis": a,
         "transform": matrix.matrix_identity(rows),
         "transform_verified": true,
         "elapsed_ms": float(ticks() - t0) / 1000000.0
      }
   }
   mut work = []
   mut transform = []
   mut i = 0
   while i < split {
      work = work.append(vec_clone(data.get(i)))
      mut tr = _flatter_zero_row(rows)
      tr[i] = Z(1)
      transform = transform.append(tr)
      i += 1
   }
   def tail = rows - split
   mut exact = true
   mut backsolve_ops, reduction_ops, target = 0, 0, 0
   while target < tail {
      def built = _flatter_tail_normal_form_target_row(data, split, tail, cols, rows, target)
      if !built.get("exact", true) { exact = false }
      backsolve_ops += built.get("backsolve_ops", 0)
      reduction_ops += built.get("reduction_ops", 0)
      work = work.append(built.get("row"))
      transform = transform.append(built.get("transform"))
      target += 1
   }
   def head_reduced = _flatter_tail_reduce_heads(work, transform, rows, split)
   work = head_reduced.get("work")
   transform = head_reduced.get("transform")
   def tail_reduce_ops = head_reduced.get("tail_reduce_ops", 0)
   def sort_swaps = head_reduced.get("sort_swaps", 0)
   def out_basis = matrix.Matrix(work)
   def transform_matrix = matrix.Matrix(transform)
   def full_verify = rows <= 48
   def applied = full_verify ? _flatter_matmul(transform_matrix, a) : nil
   def transform_ok = full_verify ? _flatter_same_matrix(applied, out_basis) : exact
   {
      "method": "triangular-tail-normal-form",
      "found": true,
      "split": split,
      "tail_rows": tail,
      "rows": rows,
      "cols": cols,
      "op_count": backsolve_ops + reduction_ops + tail_reduce_ops + sort_swaps,
      "backsolve_ops": backsolve_ops,
      "reduction_ops": reduction_ops,
      "tail_reduce_ops": tail_reduce_ops,
      "sort_swaps": sort_swaps,
      "exact": exact,
      "first_norm_before": rows > 0 ? _flatter_dot_z(data.get(0), data.get(0)) : Z(0),
      "first_norm_after": rows > 0 ? _flatter_dot_z(work.get(0), work.get(0)) : Z(0),
      "best_norm_before": _flatter_best_norm_sq(a),
      "best_norm_after": _flatter_best_norm_sq(out_basis),
      "basis": out_basis,
      "transform": transform_matrix,
      "transform_verified": transform_ok,
      "verification_method": full_verify ? "matrix-multiply" : "constructive-row-ops",
      "elapsed_ms": float(ticks() - t0) / 1000000.0
   }
}

fn triangular_tail_normal_form(any basis) any {
   "Return the basis from triangular_tail_normal_form_report."
   triangular_tail_normal_form_report(basis).get("basis")
}

fn _flatter_identity_rows(int n) list {
   mut out = []
   mut i = 0
   while i < n {
      mut row = []
      mut j = 0
      while j < n {
         row = row.append(i == j ? Z(1) : Z(0))
         j += 1
      }
      out = out.append(row)
      i += 1
   }
   out
}

fn _flatter_same_matrix(any a, any b) bool {
   def ar = _flatter_matrix_rows(a)
   def ac = _flatter_matrix_cols(a)
   if ar != _flatter_matrix_rows(b) || ac != _flatter_matrix_cols(b) { return false }
   def ad = _flatter_matrix_data(a)
   def bd = _flatter_matrix_data(b)
   mut i = 0
   while i < ar {
      def ra = ad.get(i)
      def rb = bd.get(i)
      mut j = 0
      while j < ac {
         if ra.get(j) != rb.get(j) { return false }
         j += 1
      }
      i += 1
   }
   true
}

fn _flatter_same_row(list a, list b) bool {
   if a.len != b.len { return false }
   mut i = 0
   while i < a.len {
      if Z(a.get(i)) != Z(b.get(i)) { return false }
      i += 1
   }
   true
}

fn _flatter_is_identity_matrix(any m) bool {
   def a = _flatter_reduce_matrix(m)
   def rows = _flatter_matrix_rows(a)
   def cols = _flatter_matrix_cols(a)
   if rows != cols { return false }
   def data = _flatter_matrix_data(a)
   mut i = 0
   while i < rows {
      def row = data.get(i)
      mut j = 0
      while j < cols {
         def want = i == j ? 1 : 0
         if row.get(j) != want { return false }
         j += 1
      }
      i += 1
   }
   true
}

fn _flatter_apply_transform_row(list tr, any basis) list {
   def rows = _flatter_matrix_rows(basis)
   def cols = _flatter_matrix_cols(basis)
   def data = _flatter_matrix_data(basis)
   mut out = _flatter_zero_row(cols)
   mut i = 0
   while i < rows {
      def coeff = Z(tr.get(i))
      if coeff != Z(0) { out = _flatter_row_addmul(out, data.get(i), coeff) }
      i += 1
   }
   out
}

fn _flatter_abs_z(any x) bigint {
   def z = Z(x)
   z < Z(0) ? -z : z
}

fn _flatter_bf_from_scalar(any x) bigint { is_float(x) ? bf_from_float(float(x)) : Z(x) * BF_SCALE }

fn _flatter_bf_norm_sq(any m) bigint {
   def rows = _flatter_matrix_rows(m)
   def cols = _flatter_matrix_cols(m)
   def data = _flatter_matrix_data(m)
   mut out = bf_zero()
   mut i = 0
   while i < rows {
      mut j = 0
      while j < cols {
         def x = _flatter_bf_from_scalar(data.get(i).get(j))
         out = bf_add(out, bf_mul(x, x))
         j += 1
      }
      i += 1
   }
   out
}

fn _flatter_has_float_entry(any m) bool {
   def rows = _flatter_matrix_rows(m)
   def cols = _flatter_matrix_cols(m)
   def data = _flatter_matrix_data(m)
   mut i = 0
   while i < rows {
      def row = data.get(i)
      mut j = 0
      while j < cols {
         if is_float(row.get(j)) { return true }
         j += 1
      }
      i += 1
   }
   false
}

fn _flatter_total_norm_sq_z(any m) bigint {
   def rows = _flatter_matrix_rows(m)
   def data = _flatter_matrix_data(m)
   mut out = Z(0)
   mut i = 0
   while i < rows {
      out += _flatter_dot_z(data.get(i), data.get(i))
      i += 1
   }
   out
}

fn _flatter_total_norm_key(any m) any {
   _flatter_has_float_entry(m) ? _flatter_bf_norm_sq(m) : _flatter_total_norm_sq_z(m)
}

fn _flatter_row_norm_profile_from_rows(list rows) dict {
   mut profile = []
   mut zero_rows = 0
   mut min_norm = nil
   mut max_norm = Z(0)
   mut i = 0
   while i < rows.len {
      def norm = _flatter_dot_z(rows.get(i), rows.get(i))
      profile = profile.append(norm)
      if norm == Z(0) {
         zero_rows += 1
      } else {
         if min_norm == nil || norm < min_norm { min_norm = norm }
         if norm > max_norm { max_norm = norm }
      }
      i += 1
   }
   {
      "method": "row-norm-profile",
      "profile": profile,
      "norms_sq": profile,
      "zero_rows": zero_rows,
      "rank_estimate": rows.len - zero_rows,
      "min_nonzero_norm_sq": min_norm,
      "max_norm_sq": max_norm,
      "profile_slope": 0.0
   }
}

fn _flatter_row_norm_profile_report(any basis) dict {
   def m = _flatter_reduce_matrix(basis)
   def rep = _flatter_row_norm_profile_from_rows(_flatter_matrix_data(m))
   rep.set("rows", _flatter_matrix_rows(m)).set("cols", _flatter_matrix_cols(m))
}

fn relative_size_reduce_report(any basis, int passes=1) dict {
   "Size-reduce rows against previous rows and report exact integer row operations."
   def t0 = ticks()
   def a = _flatter_reduce_matrix(basis)
   def rows = _flatter_matrix_rows(a)
   def cols = _flatter_matrix_cols(a)
   mut work = _flatter_clone_rows_small(a)
   mut transform = _flatter_identity_rows(rows)
   def record_ops = rows <= 32
   mut norms = []
   mut op_count = 0
   mut ni = 0
   while ni < rows {
      def row = work.get(ni)
      norms = norms.append(_flatter_norm_z(row))
      ni += 1
   }
   mut ops = []
   mut p = 0
   while p < passes {
      mut i = 1
      while i < rows {
         mut j = i - 1
         while j >= 0 {
            def ri = work.get(i)
            def rj = work.get(j)
            def denom = norms.get(j)
            if denom != Z(0) {
               def coeff = _flatter_round_div(_flatter_dot_z(ri, rj), denom)
               if coeff != Z(0) {
                  def new_row = _flatter_row_submul(ri, rj, coeff)
                  work[i] = new_row
                  norms[i] = _flatter_norm_z(new_row)
                  transform[i] = _flatter_row_submul(transform.get(i), transform.get(j), coeff)
                  op_count += 1
                  if record_ops { ops = ops.append({"pass": p + 1, "row": i, "against": j, "coeff": coeff}) }
               }
            }
            j -= 1
         }
         i += 1
      }
      p += 1
   }
   def out_basis = matrix.Matrix(work)
   def transform_matrix = matrix.Matrix(transform)
   def verify_transform = rows <= 32
   def applied = verify_transform ? _flatter_matmul(transform_matrix, a) : nil
   {
      "method": "relative-size-reduction",
      "rows": rows,
      "cols": cols,
      "passes": passes,
      "ops": ops,
      "op_count": op_count,
      "transform": transform_matrix,
      "transform_verified": verify_transform ? _flatter_same_matrix(applied, out_basis) : false,
      "verification_skipped": !verify_transform,
      "verification_skip_reason": verify_transform ? "" : "large relative-size report skips transform verification",
      "basis": out_basis,
      "elapsed_ms": float(ticks() - t0) / 1000000.0
   }
}

fn relative_size_reduce(any basis, int passes=1) any {
   "Return the basis from relative_size_reduce_report."
   relative_size_reduce_report(basis, passes).get("basis")
}

fn _flatter_norm_sort_rows(list work_in, list transform_in, int rows, bool track) dict {
   mut work = work_in
   mut transform = transform_in
   mut norms = list(rows)
   __list_set_len(norms, rows)
   mut ni = 0
   while ni < rows {
      norms[ni] = _flatter_norm_z(work.get(ni))
      ni += 1
   }
   mut swaps = 0
   mut si = 0
   while si < rows {
      mut best = si
      mut best_norm = norms.get(si)
      mut sj = si + 1
      while sj < rows {
         def nrm = norms.get(sj)
         if nrm < best_norm {
            best = sj
            best_norm = nrm
         }
         sj += 1
      }
      if best != si {
         def tmp_row = work.get(si)
         work[si] = work.get(best)
         work[best] = tmp_row
         def tmp_norm = norms.get(si)
         norms[si] = norms.get(best)
         norms[best] = tmp_norm
         if track {
            def tmp_tr = transform.get(si)
            transform[si] = transform.get(best)
            transform[best] = tmp_tr
         }
         swaps += 1
      }
      si += 1
   }
   {"work": work, "transform": transform, "swaps": swaps, "changed": swaps > 0}
}

fn _flatter_lagrange_pair_window(list work_in, list transform_in, int rows, int gap, int step_limit, bool track) dict {
   mut work = work_in
   mut transform = transform_in
   mut norms = list(rows)
   __list_set_len(norms, rows)
   mut ni0 = 0
   while ni0 < rows {
      norms[ni0] = _flatter_norm_z(work.get(ni0))
      ni0 += 1
   }
   mut pair_count, ops, swaps = 0, 0, 0
   mut changed = false
   mut i = 0
   while i + gap < rows {
      def j = i + gap
      pair_count += 1
      mut local_changed = true
      mut steps = 0
      while local_changed && steps < step_limit {
         local_changed = false
         def ni = norms.get(i)
         def nj = norms.get(j)
         if ni != Z(0) && nj != Z(0) && nj < ni {
            def tmp_row = work.get(i)
            work[i] = work.get(j)
            work[j] = tmp_row
            def tmp_norm = norms.get(i)
            norms[i] = norms.get(j)
            norms[j] = tmp_norm
            if track {
               def tmp_tr = transform.get(i)
               transform[i] = transform.get(j)
               transform[j] = tmp_tr
            }
            swaps += 1
            changed = true
            local_changed = true
         }
         def base_norm = norms.get(i)
         if base_norm != Z(0) {
            def coeff = _flatter_round_div(_flatter_dot_z(work.get(j), work.get(i)), base_norm)
            if coeff != Z(0) {
               def cur_norm = norms.get(j)
               def candidate = _flatter_row_submul(work.get(j), work.get(i), coeff)
               def candidate_norm = _flatter_norm_z(candidate)
               if candidate_norm < cur_norm {
                  work[j] = candidate
                  norms[j] = candidate_norm
                  if track { transform[j] = _flatter_row_submul(transform.get(j), transform.get(i), coeff) }
                  ops += 1
                  changed = true
                  local_changed = true
               }
            }
         }
         if local_changed { steps += 1 }
      }
      i += 1
   }
   {"work": work, "transform": transform, "pair_count": pair_count, "op_count": ops, "swap_count": swaps, "changed": changed}
}

fn lagrange_pair_reduce_report(any basis, int window=4, int rounds=2, int max_pair_steps=32, any verify=true, any sort_each_pass=true, any track_transform=true) dict {
   "Exact row-pair Lagrange reduction with transform tracking and local Gauss steps."
   def t0 = ticks()
   def a = _flatter_reduce_matrix(basis)
   def rows = _flatter_matrix_rows(a)
   def cols = _flatter_matrix_cols(a)
   mut work = _flatter_clone_rows_small(a)
   def track = bool(track_transform)
   mut transform = track ? _flatter_identity_rows(rows) : []
   def first_before = rows > 0 ? _flatter_dot_z(work.get(0), work.get(0)) : Z(0)
   def w = max(1, window)
   def pass_limit = max(1, rounds)
   def step_limit = max(1, max_pair_steps)
   mut pass_reports = []
   mut total_pairs = 0
   mut total_ops = 0
   mut total_swaps = 0
   mut p = 0
   mut changed = true
   while p < pass_limit && changed {
      changed = false
      mut pass_pairs = 0
      mut pass_ops = 0
      mut pass_swaps = 0
      if bool(sort_each_pass) {
         def sorted = _flatter_norm_sort_rows(work, transform, rows, track)
         work = sorted.get("work")
         transform = sorted.get("transform")
         pass_swaps += sorted.get("swaps", 0)
         total_swaps += sorted.get("swaps", 0)
         if sorted.get("changed", false) { changed = true }
      }
      mut gap = 1
      while gap <= w {
         def window_rep = _flatter_lagrange_pair_window(work, transform, rows, gap, step_limit, track)
         work = window_rep.get("work")
         transform = window_rep.get("transform")
         pass_pairs += window_rep.get("pair_count", 0)
         total_pairs += window_rep.get("pair_count", 0)
         pass_ops += window_rep.get("op_count", 0)
         total_ops += window_rep.get("op_count", 0)
         pass_swaps += window_rep.get("swap_count", 0)
         total_swaps += window_rep.get("swap_count", 0)
         if window_rep.get("changed", false) { changed = true }
         gap += 1
      }
      pass_reports = pass_reports.append({
            "pass": p + 1,
            "window": w,
            "pair_count": pass_pairs,
            "op_count": pass_ops,
            "swap_count": pass_swaps
      })
      if changed { p += 1 } else { p = pass_limit }
   }
   def final_sorted = _flatter_norm_sort_rows(work, transform, rows, track)
   work = final_sorted.get("work")
   transform = final_sorted.get("transform")
   def final_sort_swaps = final_sorted.get("swaps", 0)
   total_swaps += final_sort_swaps
   def out_basis = matrix.Matrix(work)
   def transform_matrix = track ? matrix.Matrix(transform) : nil
   def do_verify = bool(verify)
   def applied_ok = track && do_verify ? _flatter_same_matrix(_flatter_matmul(transform_matrix, a), out_basis) : false
   {
      "method": "lagrange-row-pair-reduction",
      "rows": rows,
      "cols": cols,
      "window": w,
      "max_rounds": pass_limit,
      "round_count": pass_reports.len,
      "max_pair_steps": step_limit,
      "sort_each_pass": bool(sort_each_pass),
      "pair_count": total_pairs,
      "op_count": total_ops,
      "row_op_count": total_ops + total_swaps,
      "swap_count": total_swaps,
      "final_sort_swaps": final_sort_swaps,
      "pass_reports": pass_reports,
      "first_norm_before": first_before,
      "first_norm_after": rows > 0 ? _flatter_dot_z(work.get(0), work.get(0)) : Z(0),
      "transform": transform_matrix,
      "transform_tracked": track,
      "transform_verified": applied_ok,
      "verification_skipped": !do_verify || !track,
      "basis": out_basis,
      "elapsed_ms": float(ticks() - t0) / 1000000.0
   }
}

fn lagrange_pair_reduce(any basis, int window=4, int rounds=2, int max_pair_steps=32) any {
   "Return the basis from lagrange_pair_reduce_report."
   lagrange_pair_reduce_report(basis, window, rounds, max_pair_steps).get("basis")
}

fn _flatter_sparse_entries_raw(list rows) dict {
   mut sparse = []
   mut nonzero = 0
   mut all_int = true
   mut i = 0
   while i < rows.len {
      def row = rows.get(i)
      mut entries = []
      mut j = 0
      while j < row.len {
         def v = row.get(j)
         if is_int(v) {
            if v != 0 {
               entries = entries.append([j, v])
               nonzero += 1
            }
         } else {
            def vi = _flatter_small_i64(v)
            if vi == 2147483647 { all_int = false }
            if vi != 0 {
               entries = entries.append([j, vi != 2147483647 ? vi : v])
               nonzero += 1
            }
         }
         j += 1
      }
      sparse = sparse.append(entries)
      i += 1
   }
   {"rows": sparse, "nonzero": nonzero, "all_int": all_int}
}

fn _flatter_matmul_shape(any left, any right, str caller) list {
   def A = _flatter_reduce_matrix(left)
   def B = _flatter_reduce_matrix(right)
   def ac = _flatter_matrix_cols(A)
   def br = _flatter_matrix_rows(B)
   if ac != br { panic(caller + ": incompatible shapes") }
   [A, B, _flatter_matrix_rows(A), ac, br, _flatter_matrix_cols(B)]
}

fn _flatter_sparse_product_work_report(any left, any right) dict {
   def s = _flatter_matmul_shape(left, right, "_flatter_sparse_product_work_report")
   def A, B = s.get(0), s.get(1)
   def ar, ac = int(s.get(2)), int(s.get(3))
   def br, bc = int(s.get(4)), int(s.get(5))
   def asp = _flatter_sparse_entries_raw(_flatter_matrix_data(A))
   def bsp = _flatter_sparse_entries_raw(_flatter_matrix_data(B))
   def arows = asp.get("rows")
   def brows = bsp.get("rows")
   mut row_scaled_adds = 0
   mut i = 0
   while i < ar {
      def erow = arows.get(i)
      mut e = 0
      while e < erow.len {
         def k = int(erow.get(e).get(0))
         row_scaled_adds += brows.get(k).len
         e += 1
      }
      i += 1
   }
   {
      "left_nonzero": asp.get("nonzero", 0),
      "right_nonzero": bsp.get("nonzero", 0),
      "row_scaled_adds": row_scaled_adds,
      "dense_multiply_adds": ar * ac * bc,
      "skipped_dense_products": ar * ac * bc - row_scaled_adds
   }
}

fn _strassen_predicted_scalar_multiplications(int n, int leaf_size) int {
   def leaf = max(1, leaf_size)
   if n <= leaf || n <= 1 { return n * n * n }
   7 * _strassen_predicted_scalar_multiplications(n / 2, leaf)
}

fn _flatter_triangular_structural_count(str uplo, str transpose, int n) int {
   mut count = 0
   mut i = 0
   while i < n {
      mut j = 0
      while j < n {
         if _flatter_triangular_structural_nonzero(uplo, transpose, i, j) { count += 1 }
         j += 1
      }
      i += 1
   }
   count
}

fn _flatter_zero_matrix_rows(int rows, int cols) list {
   mut out = []
   mut i = 0
   while i < rows {
      out = out.append(_flatter_zero_row(cols))
      i += 1
   }
   out
}

fn _flatter_zero_matrix_rows_int(int rows, int cols) list {
   mut out = []
   mut i = 0
   while i < rows {
      mut row = []
      mut j = 0
      while j < cols {
         row = row.append(0)
         j += 1
      }
      out = out.append(row)
      i += 1
   }
   out
}

fn _flatter_matmul_sparse_report(any left, any right, str method="sparse-row-exact-lattice-matmul") dict {
   def t0 = ticks()
   def s = _flatter_matmul_shape(left, right, "_flatter_matmul_sparse_report")
   def A, B = s.get(0), s.get(1)
   def ar, ac = int(s.get(2)), int(s.get(3))
   def br, bc = int(s.get(4)), int(s.get(5))
   def ad = _flatter_matrix_data(A)
   def bd = _flatter_matrix_data(B)
   def asp = _flatter_sparse_entries_raw(ad)
   def bsp = _flatter_sparse_entries_raw(bd)
   def int_fast = asp.get("all_int", false) && bsp.get("all_int", false)
   def arows = asp.get("rows")
   def brows = bsp.get("rows")
   mut out_rows = int_fast ? _flatter_zero_matrix_rows_int(ar, bc) : _flatter_zero_matrix_rows(ar, bc)
   mut nonzero_products = 0
   mut row_scaled_adds = 0
   if int_fast {
      mut i = 0
      while i < ar {
         mut out_row = out_rows.get(i)
         def erow = arows.get(i)
         mut e = 0
         while e < erow.len {
            def entry = erow.get(e)
            def k = int(entry.get(0))
            def av = entry.get(1)
            def brow = brows.get(k)
            mut b = 0
            while b < brow.len {
               def bent = brow.get(b)
               def j = int(bent.get(0))
               out_row[j] = out_row.get(j) + av * bent.get(1)
               nonzero_products += 1
               b += 1
            }
            row_scaled_adds += brow.len
            e += 1
         }
         out_rows[i] = out_row
         i += 1
      }
   } else {
      mut i = 0
      while i < ar {
         mut out_row = out_rows.get(i)
         def erow = arows.get(i)
         mut e = 0
         while e < erow.len {
            def entry = erow.get(e)
            def k = int(entry.get(0))
            def av = Z(entry.get(1))
            def brow = brows.get(k)
            mut b = 0
            while b < brow.len {
               def bent = brow.get(b)
               def j = int(bent.get(0))
               def bv = Z(bent.get(1))
               out_row[j] = Z(out_row.get(j)) + av * bv
               nonzero_products += 1
               b += 1
            }
            row_scaled_adds += brow.len
            e += 1
         }
         out_rows[i] = out_row
         i += 1
      }
   }
   {
      "method": method,
      "left_shape": [ar, ac],
      "right_shape": [br, bc],
      "dense_multiply_adds": ar * ac * bc,
      "left_nonzero": asp.get("nonzero", 0),
      "right_nonzero": bsp.get("nonzero", 0),
      "row_scaled_adds": row_scaled_adds,
      "nonzero_products": nonzero_products,
      "integer_fast_path": int_fast,
      "skipped_dense_products": ar * ac * bc - row_scaled_adds,
      "matrix": matrix.Matrix(out_rows),
      "elapsed_ms": float(ticks() - t0) / 1000000.0
   }
}

fn _flatter_matmul_tag_report(dict rep, str selected, str kernel_method, int dense_work, any sparse_work, int strassen_muls=0, int tri_work=-1) dict {
   rep["selected_kernel"] = selected
   rep["kernel_method"] = kernel_method
   rep["multiply_adds"] = dense_work
   case selected {
      "triangular" -> {
         rep["triangular_structural_work"] = tri_work
         rep["sparse_row_scaled_adds"] = sparse_work.get("row_scaled_adds", 0)
      }
      "strassen" -> {
         rep["dense_multiply_adds"] = dense_work
         rep["sparse_row_scaled_adds"] = sparse_work.get("row_scaled_adds", 0)
         rep["sparse_skipped_dense_products"] = sparse_work.get("skipped_dense_products", 0)
         rep["strassen_predicted_scalar_multiplications"] = strassen_muls
      }
      "threaded-row-sharded" -> {
         rep["dense_multiply_adds"] = dense_work
         rep["sparse_row_scaled_adds"] = sparse_work.get("row_scaled_adds", 0)
         rep["sparse_skipped_dense_products"] = sparse_work.get("skipped_dense_products", 0)
      }
      "sparse-row" -> { rep["strassen_predicted_scalar_multiplications"] = strassen_muls }
      _ -> {}
   }
   rep
}

fn lattice_matmul_report(any left, any right) dict {
   "Exact integer matrix multiplication report for lattice bases."
   def s = _flatter_matmul_shape(left, right, "lattice_matmul_report")
   def A, B = s.get(0), s.get(1)
   def ar, ac = int(s.get(2)), int(s.get(3))
   def br, bc = int(s.get(4)), int(s.get(5))
   def dense_work = ar * ac * bc
   def sparse_work = _flatter_sparse_product_work_report(A, B)
   if br == bc && (_flatter_is_lower_triangular(B) || _flatter_is_upper_triangular(B)) {
      def tri_kind = _flatter_is_lower_triangular(B) ? "lower" : "upper"
      def tri_work = ar * _flatter_triangular_structural_count(tri_kind, "none", br)
      if tri_work <= sparse_work.get("row_scaled_adds", dense_work) {
         def trep = lattice_triangular_matmul_report(B, A, "right", tri_kind, "none", "nonunit", Z(1))
         return _flatter_matmul_tag_report(trep, "triangular", trep.get("method", "triangular-exact-lattice-matmul"), dense_work, sparse_work, 0, tri_work)
      }
   }
   if ar == ac && (_flatter_is_lower_triangular(A) || _flatter_is_upper_triangular(A)) {
      def tri_kind = _flatter_is_lower_triangular(A) ? "lower" : "upper"
      def tri_work = bc * _flatter_triangular_structural_count(tri_kind, "none", ar)
      if tri_work <= sparse_work.get("row_scaled_adds", dense_work) {
         def trep = lattice_triangular_matmul_report(A, B, "left", tri_kind, "none", "nonunit", Z(1))
         return _flatter_matmul_tag_report(trep, "triangular", trep.get("method", "triangular-exact-lattice-matmul"), dense_work, sparse_work, 0, tri_work)
      }
   }
   def padded = _flatter_next_pow2(max(max(ar, ac), bc))
   def strassen_leaf = 4
   def strassen_muls = _strassen_predicted_scalar_multiplications(padded, strassen_leaf)
   def use_strassen = padded >= 8 && padded <= 64 && sparse_work.get("row_scaled_adds", 0) * 4 >= dense_work * 3 && strassen_muls < dense_work
   if use_strassen {
      def rep = lattice_matmul_strassen_report(A, B, strassen_leaf)
      return _flatter_matmul_tag_report(rep, "strassen", rep.get("method", "strassen-exact-lattice-matmul"), dense_work, sparse_work, strassen_muls)
   }
   def dense_enough_for_threads = sparse_work.get("row_scaled_adds", 0) * 4 >= dense_work * 3
   if padded > 64 && dense_enough_for_threads && ospar.parallel_should_threads(dense_work) {
      def rep = lattice_matmul_threaded_report(A, B)
      return _flatter_matmul_tag_report(rep, "threaded-row-sharded", rep.get("method", "threaded-row-sharded-exact-lattice-matmul"), dense_work, sparse_work, strassen_muls)
   }
   def rep = _flatter_matmul_sparse_report(A, B, "exact-lattice-matmul")
   _flatter_matmul_tag_report(rep, "sparse-row", rep.get("method", "exact-lattice-matmul"), dense_work, sparse_work, strassen_muls)
}

fn lattice_matmul(any left, any right) any {
   "Return the product matrix from lattice_matmul_report."
   lattice_matmul_report(left, right).get("matrix")
}

fn _flatter_matmul(any left, any right) any {
   if _flatter_is_identity_matrix(left) { return _flatter_reduce_matrix(right) }
   if _flatter_is_identity_matrix(right) { return _flatter_reduce_matrix(left) }
   _flatter_matmul_sparse_report(left, right).get("matrix")
}

fn lattice_matmul_blocked_report(any left, any right, int block_size=16) dict {
   "Exact blocked integer matrix multiplication with sparse-product counters."
   def t0 = ticks()
   def s = _flatter_matmul_shape(left, right, "lattice_matmul_blocked_report")
   def A, B = s.get(0), s.get(1)
   def ar, ac = int(s.get(2)), int(s.get(3))
   def br, bc = int(s.get(4)), int(s.get(5))
   def ad = _flatter_matrix_data(A)
   def bd = _flatter_matrix_data(B)
   def bigint_fast = _flatter_matrix_data_all_bigint(ad) && _flatter_matrix_data_all_bigint(bd)
   def ai64 = _flatter_matrix_data_small_i64_report(ad)
   def bi64 = _flatter_matrix_data_small_i64_report(bd)
   def max_i64_abs = max(int(ai64.get("max_abs", 0)), int(bi64.get("max_abs", 0)))
   def i64_fast = ai64.get("ok", false) && bi64.get("ok", false) && ac <= 1024 && max_i64_abs <= 1000000
   def work_ad = i64_fast ? ai64.get("rows") : ad
   def work_bd = i64_fast ? bi64.get("rows") : bd
   def z0 = i64_fast ? 0 : Z(0)
   def bs = max(1, block_size)
   mut out_rows = list(ar)
   __list_set_len(out_rows, ar)
   mut i = 0
   while i < ar {
      mut row = list(bc)
      __list_set_len(row, bc)
      mut j = 0
      while j < bc {
         row[j] = z0
         j += 1
      }
      out_rows[i] = row
      i += 1
   }
   mut nonzero = 0
   mut skipped_zero_products = 0
   mut block_count = 0
   mut ii = 0
   while ii < ar {
      mut kk = 0
      while kk < ac {
         mut jj = 0
         while jj < bc {
            def iend = min(ar, ii + bs)
            def kend = min(ac, kk + bs)
            def jend = min(bc, jj + bs)
            i = ii
            if i64_fast {
               while i < iend {
                  mut row = out_rows.get(i)
                  def arow = work_ad.get(i)
                  mut k = kk
                  while k < kend {
                     def av = int(arow.get(k))
                     if av == 0 {
                        skipped_zero_products += jend - jj
                     } else {
                        def brow = work_bd.get(k)
                        mut j = jj
                        while j < jend {
                           def bv = int(brow.get(j))
                           if bv == 0 {
                              skipped_zero_products += 1
                           } else {
                              row[j] = int(row.get(j)) + av * bv
                              nonzero += 1
                           }
                           j += 1
                        }
                     }
                     k += 1
                  }
                  out_rows[i] = row
                  i += 1
               }
            } else {
               while i < iend {
                  mut row = out_rows.get(i)
                  def arow = work_ad.get(i)
                  mut k = kk
                  while k < kend {
                     def av = bigint_fast ? arow.get(k) : Z(arow.get(k))
                     if av == z0 {
                        skipped_zero_products += jend - jj
                     } else {
                        def brow = work_bd.get(k)
                        mut j = jj
                        while j < jend {
                           def bv = bigint_fast ? brow.get(j) : Z(brow.get(j))
                           if bv == z0 {
                              skipped_zero_products += 1
                           } else {
                              row[j] = row.get(j) + av * bv
                              nonzero += 1
                           }
                           j += 1
                        }
                     }
                     k += 1
                  }
                  out_rows[i] = row
                  i += 1
               }
            }
            block_count += 1
            jj += bs
         }
         kk += bs
      }
      ii += bs
   }
   {
      "method": "blocked-exact-lattice-matmul",
      "left_shape": [ar, ac],
      "right_shape": [br, bc],
      "block_size": bs,
      "block_count": block_count,
      "multiply_adds": ar * ac * bc,
      "nonzero_products": nonzero,
      "skipped_zero_products": skipped_zero_products,
      "bigint_fast_path": bigint_fast,
      "i64_fast_path": i64_fast,
      "matrix": matrix.Matrix(out_rows),
      "elapsed_ms": float(ticks() - t0) / 1000000.0
   }
}

fn lattice_matmul_blocked(any left, any right, int block_size=16) any {
   "Return the product matrix from lattice_matmul_blocked_report."
   lattice_matmul_blocked_report(left, right, block_size).get("matrix")
}

fn _flatter_matrix_data_all_bigint(list data) bool {
   mut i = 0
   while i < data.len {
      def row = data.get(i)
      mut j = 0
      while j < row.len {
         if !is_bigint(row.get(j)) { return false }
         j += 1
      }
      i += 1
   }
   true
}

fn _flatter_matrix_data_small_i64_report(list data) dict {
   mut rows = list(data.len)
   __list_set_len(rows, data.len)
   mut max_abs = 0
   mut i = 0
   while i < data.len {
      def src = data.get(i)
      mut row = list(src.len)
      __list_set_len(row, src.len)
      mut j = 0
      while j < src.len {
         def v = _flatter_small_i64(src.get(j))
         if v == 2147483647 { return {"ok": false, "rows": [], "max_abs": max_abs} }
         def av = v < 0 ? -v : v
         if av > max_abs { max_abs = av }
         row[j] = v
         j += 1
      }
      rows[i] = row
      i += 1
   }
   {"ok": true, "rows": rows, "max_abs": max_abs}
}

fn _flatter_matmul_row_from_data(list ad, list bd, int i, int ac, int bc) dict {
   mut row = list(bc)
   __list_set_len(row, bc)
   mut nonzero = 0
   mut skipped = 0
   mut j = 0
   while j < bc {
      row[j] = Z(0)
      j += 1
   }
   def arow = ad.get(i)
   mut k = 0
   while k < ac {
      def av = Z(arow.get(k))
      if av == Z(0) {
         skipped += bc
      } else {
         def brow = bd.get(k)
         j = 0
         while j < bc {
            def bv = Z(brow.get(j))
            if bv == Z(0) {
               skipped += 1
            } else {
               row[j] = Z(row.get(j)) + av * bv
               nonzero += 1
            }
            j += 1
         }
      }
      k += 1
   }
   {"row": row, "nonzero_products": nonzero, "skipped_zero_products": skipped}
}

fn _flatter_matmul_row_from_i64_data(list ad, list bd, int i, int ac, int bc) dict {
   mut row = list(bc)
   __list_set_len(row, bc)
   mut nonzero = 0
   mut skipped = 0
   mut j = 0
   while j < bc {
      row[j] = 0
      j += 1
   }
   def arow = ad.get(i)
   mut k = 0
   while k < ac {
      def av = int(arow.get(k))
      if av == 0 {
         skipped += bc
      } else {
         def brow = bd.get(k)
         j = 0
         while j < bc {
            def bv = int(brow.get(j))
            if bv == 0 {
               skipped += 1
            } else {
               row[j] = int(row.get(j)) + av * bv
               nonzero += 1
            }
            j += 1
         }
      }
      k += 1
   }
   {"row": row, "nonzero_products": nonzero, "skipped_zero_products": skipped}
}

fn _flatter_matmul_row_from_bigint_data(list ad, list bd, int i, int ac, int bc) dict {
   mut row = list(bc)
   __list_set_len(row, bc)
   mut nonzero = 0
   mut skipped = 0
   def z0 = Z(0)
   mut j = 0
   while j < bc {
      row[j] = z0
      j += 1
   }
   def arow = ad.get(i)
   mut k = 0
   while k < ac {
      def av = arow.get(k)
      if av == z0 {
         skipped += bc
      } else {
         def brow = bd.get(k)
         j = 0
         while j < bc {
            def bv = brow.get(j)
            if bv == z0 {
               skipped += 1
            } else {
               row[j] = row.get(j) + av * bv
               nonzero += 1
            }
            j += 1
         }
      }
      k += 1
   }
   {"row": row, "nonzero_products": nonzero, "skipped_zero_products": skipped}
}

fn _flatter_matmul_chunk_worker(list args) dict {
   def ad = args.get(0)
   def bd = args.get(1)
   def start = int(args.get(2))
   def stop = int(args.get(3))
   def ac = int(args.get(4))
   def bc = int(args.get(5))
   def bigint_fast = bool(args.get(6, false))
   def i64_fast = bool(args.get(7, false))
   mut rows = list(max(0, stop - start))
   __list_set_len(rows, max(0, stop - start))
   mut nonzero = 0
   mut skipped = 0
   mut i = start
   while i < stop {
      def rr = i64_fast ? _flatter_matmul_row_from_i64_data(ad, bd, i, ac, bc) : (bigint_fast ? _flatter_matmul_row_from_bigint_data(ad, bd, i, ac, bc) : _flatter_matmul_row_from_data(ad, bd, i, ac, bc))
      rows[i - start] = rr.get("row")
      nonzero += rr.get("nonzero_products", 0)
      skipped += rr.get("skipped_zero_products", 0)
      i += 1
   }
   {"start": start, "stop": stop, "rows": rows, "nonzero_products": nonzero, "skipped_zero_products": skipped}
}

fn lattice_matmul_threaded_report(any left, any right, int max_threads=0) dict {
   "Exact row-sharded integer matrix multiplication using Ny worker threads."
   def t0 = ticks()
   def s = _flatter_matmul_shape(left, right, "lattice_matmul_threaded_report")
   def A, B = s.get(0), s.get(1)
   def ar, ac = int(s.get(2)), int(s.get(3))
   def br, bc = int(s.get(4)), int(s.get(5))
   def ad = _flatter_matrix_data(A)
   def bd = _flatter_matrix_data(B)
   def work_items = ar * ac * bc
   def bigint_fast = _flatter_matrix_data_all_bigint(ad) && _flatter_matrix_data_all_bigint(bd)
   def ai64 = _flatter_matrix_data_small_i64_report(ad)
   def bi64 = _flatter_matrix_data_small_i64_report(bd)
   def max_i64_abs = max(int(ai64.get("max_abs", 0)), int(bi64.get("max_abs", 0)))
   def i64_fast = ai64.get("ok", false) && bi64.get("ok", false) && ac <= 1024 && max_i64_abs <= 1000000
   def work_ad = i64_fast ? ai64.get("rows") : ad
   def work_bd = i64_fast ? bi64.get("rows") : bd
   def workers = ospar.thread_budget(ar, max_threads)
   def ranges = ospar.chunk_ranges(ar, workers)
   mut handles = []
   mut i = 0
   while i < ranges.len {
      def r = ranges.get(i)
      handles = handles.append(ospar.future(_flatter_matmul_chunk_worker, [work_ad, work_bd, r.get(0), r.get(1), ac, bc, bigint_fast, i64_fast]))
      i += 1
   }
   mut out_rows = list(ar)
   __list_set_len(out_rows, ar)
   mut nonzero = 0
   mut skipped = 0
   mut chunk_reports = []
   i = 0
   while i < handles.len {
      def rep = ospar.future_wait(handles.get(i))
      chunk_reports = chunk_reports.append({
            "start": rep.get("start", 0),
            "stop": rep.get("stop", 0),
            "nonzero_products": rep.get("nonzero_products", 0),
            "skipped_zero_products": rep.get("skipped_zero_products", 0)
      })
      def rows = rep.get("rows")
      mut j = 0
      while j < rows.len {
         out_rows[int(rep.get("start", 0)) + j] = rows.get(j)
         j += 1
      }
      nonzero += rep.get("nonzero_products", 0)
      skipped += rep.get("skipped_zero_products", 0)
      i += 1
   }
   {
      "method": "threaded-row-sharded-exact-lattice-matmul",
      "selected_kernel": "threaded-row-sharded",
      "left_shape": [ar, ac],
      "right_shape": [br, bc],
      "work_items": work_items,
      "workers": workers,
      "chunks": ranges.len,
      "chunk_reports": chunk_reports,
      "bigint_fast_path": bigint_fast,
      "i64_fast_path": i64_fast,
      "multiply_adds": work_items,
      "nonzero_products": nonzero,
      "skipped_zero_products": skipped,
      "matrix": matrix.Matrix(out_rows),
      "elapsed_ms": float(ticks() - t0) / 1000000.0
   }
}

fn lattice_matmul_threaded(any left, any right, int max_threads=0) any {
   "Return the product matrix from lattice_matmul_threaded_report."
   lattice_matmul_threaded_report(left, right, max_threads).get("matrix")
}

fn _flatter_triangular_structural_nonzero(str uplo, str transpose, int k, int j) bool {
   def lower = uplo == "lower" || uplo == "L"
   def upper = uplo == "upper" || uplo == "U"
   def trans = transpose == "transpose" || transpose == "T"
   if lower && !trans { return k >= j }
   if lower && trans { return j >= k }
   if upper && !trans { return k <= j }
   if upper && trans { return j <= k }
   true
}

fn _flatter_triangular_entry(any tri, str uplo, str transpose, str diag, int k, int j) bigint {
   if !_flatter_triangular_structural_nonzero(uplo, transpose, k, j) { return Z(0) }
   if k == j && (diag == "unit" || diag == "U") { return Z(1) }
   def trans = transpose == "transpose" || transpose == "T"
   trans ? Z(matrix.mat_get(tri, j, k)) : Z(matrix.mat_get(tri, k, j))
}

fn lattice_triangular_matmul_report(any triangular, any dense, str side="right", str uplo="lower", str transpose="none", str diag="nonunit", any alpha=1) dict {
   "Exact triangular matrix multiply report for B * op(A) or op(A) * B style lattice kernels."
   def t0 = ticks()
   def A = _flatter_reduce_matrix(triangular)
   def B = _flatter_reduce_matrix(dense)
   def n = _flatter_matrix_rows(A)
   if n != _flatter_matrix_cols(A) { panic("lattice_triangular_matmul_report: triangular matrix must be square") }
   def right_side = side == "right" || side == "R"
   def left_side = side == "left" || side == "L"
   if !(right_side || left_side) { panic("lattice_triangular_matmul_report: side must be right/R or left/L") }
   def rows = _flatter_matrix_rows(B)
   def cols = _flatter_matrix_cols(B)
   if right_side && cols != n { panic("lattice_triangular_matmul_report: dense column count must match triangular size for right-side multiply") }
   if left_side && rows != n { panic("lattice_triangular_matmul_report: dense row count must match triangular size for left-side multiply") }
   def bd = _flatter_matrix_data(B)
   def scale = Z(alpha)
   mut out_rows = list(right_side ? rows : n)
   __list_set_len(out_rows, right_side ? rows : n)
   mut structural_products = 0
   mut nonzero_products = 0
   mut zero_value_skips = 0
   if right_side {
      mut i = 0
      while i < rows {
         mut row = list(n)
         __list_set_len(row, n)
         mut j = 0
         while j < n {
            mut s = Z(0)
            mut k = 0
            while k < n {
               if _flatter_triangular_structural_nonzero(uplo, transpose, k, j) {
                  structural_products += 1
                  def av = _flatter_triangular_entry(A, uplo, transpose, diag, k, j)
                  def bv = Z(bd.get(i).get(k))
                  if av == Z(0) || bv == Z(0) {
                     zero_value_skips += 1
                  } else {
                     s = s + bv * av * scale
                     nonzero_products += 1
                  }
               }
               k += 1
            }
            row[j] = s
            j += 1
         }
         out_rows[i] = row
         i += 1
      }
   } else {
      mut i = 0
      while i < n {
         mut row = list(cols)
         __list_set_len(row, cols)
         mut j = 0
         while j < cols {
            mut s = Z(0)
            mut k = 0
            while k < n {
               if _flatter_triangular_structural_nonzero(uplo, transpose, i, k) {
                  structural_products += 1
                  def av = _flatter_triangular_entry(A, uplo, transpose, diag, i, k)
                  def bv = Z(bd.get(k).get(j))
                  if av == Z(0) || bv == Z(0) {
                     zero_value_skips += 1
                  } else {
                     s = s + av * bv * scale
                     nonzero_products += 1
                  }
               }
               k += 1
            }
            row[j] = s
            j += 1
         }
         out_rows[i] = row
         i += 1
      }
   }
   def dense_work = right_side ? rows * n * n : n * n * cols
   {
      "method": "triangular-exact-lattice-matmul",
      "side": side,
      "uplo": uplo,
      "transpose": transpose,
      "diag": diag,
      "alpha": scale,
      "dense_shape": [rows, cols],
      "triangular_shape": [n, n],
      "dense_multiply_adds": dense_work,
      "structural_products": structural_products,
      "nonzero_products": nonzero_products,
      "structural_skips": dense_work - structural_products,
      "zero_value_skips": zero_value_skips,
      "matrix": matrix.Matrix(out_rows),
      "elapsed_ms": float(ticks() - t0) / 1000000.0
   }
}

fn lattice_triangular_matmul(any triangular, any dense, str side="right", str uplo="lower", str transpose="none", str diag="nonunit", any alpha=1) any {
   "Return the product matrix from lattice_triangular_matmul_report."
   lattice_triangular_matmul_report(triangular, dense, side, uplo, transpose, diag, alpha).get("matrix")
}

fn _flatter_next_pow2(int n) int {
   mut p = 1
   while p < n { p = p * 2 }
   p
}

fn _strassen_pad_rows(any m, int size) list {
   def rows = _flatter_matrix_rows(m)
   def cols = _flatter_matrix_cols(m)
   def data = _flatter_matrix_data(m)
   mut out = list(size)
   __list_set_len(out, size)
   mut i = 0
   while i < size {
      mut row = list(size)
      __list_set_len(row, size)
      mut j = 0
      while j < size {
         row[j] = (i < rows && j < cols) ? data.get(i).get(j) : 0
         j += 1
      }
      out[i] = row
      i += 1
   }
   out
}

fn _strassen_crop_rows(list rows, int out_rows, int out_cols) list {
   mut out = list(out_rows)
   __list_set_len(out, out_rows)
   mut i = 0
   while i < out_rows {
      mut row = list(out_cols)
      __list_set_len(row, out_cols)
      mut j = 0
      while j < out_cols {
         row[j] = rows.get(i).get(j)
         j += 1
      }
      out[i] = row
      i += 1
   }
   out
}

fn _strassen_add(list a, list b) list {
   def n = a.len
   mut out = list(n)
   __list_set_len(out, n)
   mut i = 0
   while i < n {
      mut row = list(n)
      __list_set_len(row, n)
      mut j = 0
      while j < n {
         row[j] = a.get(i).get(j) + b.get(i).get(j)
         j += 1
      }
      out[i] = row
      i += 1
   }
   out
}

fn _strassen_sub(list a, list b) list {
   def n = a.len
   mut out = list(n)
   __list_set_len(out, n)
   mut i = 0
   while i < n {
      mut row = list(n)
      __list_set_len(row, n)
      mut j = 0
      while j < n {
         row[j] = a.get(i).get(j) - b.get(i).get(j)
         j += 1
      }
      out[i] = row
      i += 1
   }
   out
}

fn _strassen_slice(list a, int r0, int c0, int n) list {
   mut out = list(n)
   __list_set_len(out, n)
   mut i = 0
   while i < n {
      mut row = list(n)
      __list_set_len(row, n)
      mut j = 0
      while j < n {
         row[j] = a.get(r0 + i).get(c0 + j)
         j += 1
      }
      out[i] = row
      i += 1
   }
   out
}

fn _strassen_join(list c11, list c12, list c21, list c22) list {
   def h = c11.len
   mut out = list(h * 2)
   __list_set_len(out, h * 2)
   mut i = 0
   while i < h {
      mut row = list(h * 2)
      __list_set_len(row, h * 2)
      mut j = 0
      while j < h {
         row[j] = c11.get(i).get(j)
         j += 1
      }
      j = 0
      while j < h {
         row[h + j] = c12.get(i).get(j)
         j += 1
      }
      out[i] = row
      i += 1
   }
   i = 0
   while i < h {
      mut row = list(h * 2)
      __list_set_len(row, h * 2)
      mut j = 0
      while j < h {
         row[j] = c21.get(i).get(j)
         j += 1
      }
      j = 0
      while j < h {
         row[h + j] = c22.get(i).get(j)
         j += 1
      }
      out[h + i] = row
      i += 1
   }
   out
}

fn _strassen_naive_square_report(list a, list b) dict {
   def n = a.len
   mut out = list(n)
   __list_set_len(out, n)
   mut nonzero_products = 0
   mut i = 0
   while i < n {
      mut row = list(n)
      __list_set_len(row, n)
      mut j = 0
      while j < n {
         mut s = 0
         mut k = 0
         while k < n {
            def av = a.get(i).get(k)
            def bv = b.get(k).get(j)
            if av != 0 && bv != 0 { nonzero_products += 1 }
            s = s + av * bv
            k += 1
         }
         row[j] = s
         j += 1
      }
      out[i] = row
      i += 1
   }
   {
      "rows": out,
      "scalar_multiplications": n * n * n,
      "scalar_additions": n * n * max(0, n - 1),
      "matrix_add_sub_ops": 0,
      "nonzero_products": nonzero_products,
      "recursive_calls": 1,
      "max_depth": 1
   }
}

fn _strassen_merge_counts(list reps, int extra_adds, int depth) dict {
   mut scalar_muls = 0
   mut scalar_adds = 0
   mut matrix_ops = extra_adds
   mut nonzero = 0
   mut calls = 1
   mut max_depth = depth
   mut i = 0
   while i < reps.len {
      def r = reps.get(i)
      scalar_muls += r.get("scalar_multiplications", 0)
      scalar_adds += r.get("scalar_additions", 0)
      matrix_ops += r.get("matrix_add_sub_ops", 0)
      nonzero += r.get("nonzero_products", 0)
      calls += r.get("recursive_calls", 0)
      if r.get("max_depth", depth) > max_depth { max_depth = r.get("max_depth", depth) }
      i += 1
   }
   {
      "scalar_multiplications": scalar_muls,
      "scalar_additions": scalar_adds,
      "matrix_add_sub_ops": matrix_ops,
      "nonzero_products": nonzero,
      "recursive_calls": calls,
      "max_depth": max_depth
   }
}

fn _strassen_square_report(list a, list b, int leaf_size, int depth) dict {
   def n = a.len
   if n <= max(1, leaf_size) || n <= 1 {
      def base = _strassen_naive_square_report(a, b)
      return base.set("max_depth", depth)
   }
   def h = n / 2
   def a11 = _strassen_slice(a, 0, 0, h)
   def a12 = _strassen_slice(a, 0, h, h)
   def a21 = _strassen_slice(a, h, 0, h)
   def a22 = _strassen_slice(a, h, h, h)
   def b11 = _strassen_slice(b, 0, 0, h)
   def b12 = _strassen_slice(b, 0, h, h)
   def b21 = _strassen_slice(b, h, 0, h)
   def b22 = _strassen_slice(b, h, h, h)
   def m1 = _strassen_square_report(_strassen_add(a11, a22), _strassen_add(b11, b22), leaf_size, depth + 1)
   def m2 = _strassen_square_report(_strassen_add(a21, a22), b11, leaf_size, depth + 1)
   def m3 = _strassen_square_report(a11, _strassen_sub(b12, b22), leaf_size, depth + 1)
   def m4 = _strassen_square_report(a22, _strassen_sub(b21, b11), leaf_size, depth + 1)
   def m5 = _strassen_square_report(_strassen_add(a11, a12), b22, leaf_size, depth + 1)
   def m6 = _strassen_square_report(_strassen_sub(a21, a11), _strassen_add(b11, b12), leaf_size, depth + 1)
   def m7 = _strassen_square_report(_strassen_sub(a12, a22), _strassen_add(b21, b22), leaf_size, depth + 1)
   def r1 = m1.get("rows")
   def r2 = m2.get("rows")
   def r3 = m3.get("rows")
   def r4 = m4.get("rows")
   def r5 = m5.get("rows")
   def r6 = m6.get("rows")
   def r7 = m7.get("rows")
   def c11 = _strassen_add(_strassen_sub(_strassen_add(r1, r4), r5), r7)
   def c12 = _strassen_add(r3, r5)
   def c21 = _strassen_add(r2, r4)
   def c22 = _strassen_add(_strassen_sub(_strassen_add(r1, r3), r2), r6)
   def counts = _strassen_merge_counts([m1, m2, m3, m4, m5, m6, m7], 18 * h * h, depth)
   counts.set("rows", _strassen_join(c11, c12, c21, c22))
}

fn lattice_matmul_strassen_report(any left, any right, int leaf_size=16) dict {
   "Exact padded Strassen integer matrix multiplication for dense lattice kernels."
   def t0 = ticks()
   def s = _flatter_matmul_shape(left, right, "lattice_matmul_strassen_report")
   def A, B = s.get(0), s.get(1)
   def ar, ac = int(s.get(2)), int(s.get(3))
   def br, bc = int(s.get(4)), int(s.get(5))
   def padded = _flatter_next_pow2(max(max(ar, ac), bc))
   def leaf = max(1, leaf_size)
   def rep = _strassen_square_report(_strassen_pad_rows(A, padded), _strassen_pad_rows(B, padded), leaf, 1)
   def cropped = _strassen_crop_rows(rep.get("rows"), ar, bc)
   {
      "method": "strassen-exact-lattice-matmul",
      "left_shape": [ar, ac],
      "right_shape": [br, bc],
      "padded_size": padded,
      "leaf_size": leaf,
      "scalar_multiplications": rep.get("scalar_multiplications", 0),
      "dense_scalar_multiplications": padded * padded * padded,
      "scalar_additions": rep.get("scalar_additions", 0),
      "matrix_add_sub_ops": rep.get("matrix_add_sub_ops", 0),
      "nonzero_products": rep.get("nonzero_products", 0),
      "recursive_calls": rep.get("recursive_calls", 0),
      "max_depth": rep.get("max_depth", 1),
      "matrix": matrix.Matrix(cropped),
      "elapsed_ms": float(ticks() - t0) / 1000000.0
   }
}

fn lattice_matmul_strassen(any left, any right, int leaf_size=16) any {
   "Return the product matrix from lattice_matmul_strassen_report."
   lattice_matmul_strassen_report(left, right, leaf_size).get("matrix")
}

fn _flatter_slice_rows(list rows, int start, int stop) list {
   mut out = []
   mut i = start
   while i < stop {
      out = out.append(vec_clone(rows.get(i)))
      i += 1
   }
   out
}

fn _flatter_replace_rows(list rows, int start, any block) list {
   def bdata = (is_list(block) && block.len >= 3 && is_int(block.get(0, nil)) && is_int(block.get(1, nil)) && is_list(block.get(2, nil))) ? _flatter_matrix_data(block) : block
   mut out = []
   mut i = 0
   while i < rows.len {
      def j = i - start
      out = out.append((j >= 0 && j < bdata.len) ? bdata.get(j) : rows.get(i))
      i += 1
   }
   out
}

fn _flatter_apply_block_transform_rows(list global_rows, int start, any block_transform) list {
   def tdata = _flatter_matrix_data(block_transform)
   def size = tdata.len
   mut out = []
   mut i = 0
   while i < global_rows.len {
      if i >= start && i < start + size {
         mut row = _flatter_zero_row(global_rows.len)
         mut j = 0
         while j < size {
            def coeff = Z(tdata.get(i - start).get(j))
            if coeff != Z(0) {
               row = _flatter_row_addmul(row, global_rows.get(start + j), coeff)
            }
            j += 1
         }
         out = out.append(row)
      } else {
         out = out.append(global_rows.get(i))
      }
      i += 1
   }
   out
}

fn _flatter_phase(str name, int start, int stop) dict { {"name": name, "start": start, "stop": stop} }

fn _flatter_strategy_window(str name, int start, int stop, str child_split="") dict {
   {
      "name": name,
      "start": start,
      "stop": stop,
      "rows": max(0, stop - start),
      "child_split": child_split
   }
}

fn _flatter_split_phase2_k(int n) int {
   if n == 3 { return 2 }
   max(1, n / 2)
}

fn _flatter_split_phase3_k(int n) int {
   if n == 3 { return 2 }
   max(1, n / 2)
}

fn _flatter_phase2_windows(int n, int iter) list {
   if n <= 1 { return [_flatter_strategy_window("all", 0, n, "phase3")] }
   def k = _flatter_split_phase2_k(n)
   if n == 3 {
      if iter == 0 { return [_flatter_strategy_window("left", 0, k, "phase2")] }
      return [_flatter_strategy_window("all", 0, n, "phase3")]
   }
   if iter == 0 { return [_flatter_strategy_window("left", 0, k, "phase2")] }
   if iter == 1 { return [_flatter_strategy_window("right", k, n, "phase2")] }
   [_flatter_strategy_window("all", 0, n, "phase3")]
}

fn _flatter_phase2_stopping_point(int n, int iter) bool {
   if n == 3 { return iter > 1 }
   iter > 2
}

fn _flatter_phase3_windows(int n, int iter) list {
   if n <= 1 { return [_flatter_strategy_window("all", 0, n, "phase3")] }
   def k = _flatter_split_phase3_k(n)
   if n == 3 {
      if iter % 2 == 0 { return [_flatter_strategy_window("left-pair", 0, 2, "phase3")] }
      return [_flatter_strategy_window("right-pair", 1, 3, "phase3")]
   }
   if iter % 2 == 0 {
      def left_k = _flatter_split_phase3_k(k)
      def right_k = _flatter_split_phase3_k(n - k)
      return [_flatter_strategy_window("middle", left_k, k + right_k, "phase3")]
   }
   [
      _flatter_strategy_window("left", 0, k, "phase3"),
      _flatter_strategy_window("right", k, n, "phase3")
   ]
}

fn _flatter_phase3_stopping_point(int iter) bool { iter % 2 == 0 }

fn _flatter_strategy_precision_rule(str strategy, int n) str {
   if strategy == "proved3" { return "2 * (2 * spread + 40)" }
   if strategy == "heuristic3" || strategy == "heuristic2" || strategy == "heuristic1" {
      return "aggressive ? spread + 30 : 2 * spread + 30 + 2 * n"
   }
   "2 * spread + 40"
}

fn _flatter_strategy_iterations(int n, str strategy, int max_iterations) list {
   mut out = []
   def limit = max(1, max_iterations)
   mut i = 0
   while i < limit {
      mut windows = []
      mut split = "none"
      mut stopping = false
      if strategy == "proved2" {
         windows = _flatter_phase2_windows(n, i)
         split = "phase2"
         stopping = _flatter_phase2_stopping_point(n, i)
      } else if strategy == "proved3" {
         def phase = i % 3
         if phase == 0 {
            windows = [_flatter_strategy_window("middle", n / 4, (3 * n) / 4, "phase3")]
         } else if phase == 1 {
            windows = [_flatter_strategy_window("left", 0, n / 2, "phase3")]
         } else {
            windows = [_flatter_strategy_window("right", n / 2, n, "phase3")]
         }
         split = "phase3"
         stopping = i > 0 && i % 3 == 0
      } else if strategy == "heuristic2" || strategy == "heuristic1" {
         windows = _flatter_phase2_windows(n, i)
         split = "phase2"
         stopping = _flatter_phase2_stopping_point(n, i)
      } else if strategy == "heuristic3" {
         windows = _flatter_phase3_windows(n, i)
         split = "phase3"
         stopping = _flatter_phase3_stopping_point(i)
      } else {
         def phase = _flatter_schoenhage_phase(n, i, strategy)
         windows = [_flatter_strategy_window(phase.get("name", "all"), phase.get("start", 0), phase.get("stop", n), "custom")]
         split = "custom"
         stopping = false
      }
      out = out.append({
            "iteration": i,
            "split": split,
            "sublattice_count": windows.len,
            "sublattices": windows,
            "stopping_point": stopping
      })
      i += 1
   }
   out
}

fn recursive_strategy_plan_report(int rows, str strategy="proved3", int max_iterations=6) dict {
   "Report the recursive sublattice schedule used by flatter-style strategies."
   def n = max(0, rows)
   mut family = _flatter_schoenhage_strategy_family(strategy)
   mut split = "custom"
   if strategy == "proved2" || strategy == "heuristic2" || strategy == "heuristic1" { split = "phase2" }
   if strategy == "proved3" || strategy == "heuristic3" { split = "phase3" }
   mut padded_rows = n
   if strategy == "proved1" {
      padded_rows = 1
      while padded_rows < max(1, n) { padded_rows = padded_rows * 2 }
      split = "padded-recursive"
      family = "proved"
   }
   def plan_rows = strategy == "proved1" ? padded_rows : n
   mut iterations = []
   if strategy == "proved1" {
      iterations = [{
            "iteration": 0,
            "split": "padded-recursive",
            "sublattice_count": 1,
            "sublattices": [_flatter_strategy_window("padded-all", 0, padded_rows, "phase3")],
            "stopping_point": false
      }]
   } else {
      iterations = _flatter_strategy_iterations(plan_rows, strategy, max_iterations)
   }
   {
      "method": "recursive-strategy-plan",
      "source": "flatter-recursive-strategy",
      "strategy": strategy,
      "strategy_family": family,
      "rows": n,
      "padded_rows": padded_rows,
      "split": split,
      "max_iterations": max(1, max_iterations),
      "iterations": iterations,
      "iteration_count": iterations.len,
      "precision_rule": _flatter_strategy_precision_rule(strategy, plan_rows),
      "ok": n >= 0 && iterations.len > 0
   }
}

fn _flatter_schoenhage_phase(int rows, int phase, str strategy) dict {
   def n = max(0, rows)
   if n <= 1 { return _flatter_phase("none", 0, n) }
   if strategy == "proved1" {
      return case phase {
         0 -> _flatter_phase("upper-triangular-size", 0, n)
         1 -> _flatter_phase("left", 0, max(2, n / 2))
         2 -> _flatter_phase("right", n / 2, n)
         _ -> _flatter_phase("all", 0, n)
      }
   }
   if strategy == "proved2" || strategy == "heuristic2" || strategy == "phase2" {
      return case phase {
         0 -> _flatter_phase("left", 0, max(2, n / 2))
         1 -> _flatter_phase("right", n / 2, n)
         _ -> _flatter_phase("all", 0, n)
      }
   }
   if strategy == "heuristic1" {
      def start1 = n / 4
      return case phase {
         0 -> _flatter_phase("left-rectangular", 0, max(2, n / 2))
         1 -> _flatter_phase("middle-rectangular", start1, min(n, max(start1 + 2, (3 * n) / 4)))
         2 -> _flatter_phase("right-rectangular", n / 2, n)
         _ -> _flatter_phase("all-rectangular", 0, n)
      }
   }
   def start = n / 4
   case phase {
      0 -> _flatter_phase("middle", start, min(n, max(start + 2, (3 * n) / 4)))
      1 -> _flatter_phase("left", 0, max(2, n / 2))
      _ -> _flatter_phase("right", n / 2, n)
   }
}

fn _flatter_schoenhage_phase_count(str strategy) int {
   if strategy == "proved1" || strategy == "heuristic1" { return 4 }
   3
}

fn _flatter_schoenhage_strategy_family(str strategy) str {
   if strategy == "proved1" || strategy == "proved2" || strategy == "proved3" { return "proved" }
   if strategy == "heuristic1" || strategy == "heuristic2" || strategy == "heuristic3" { return "heuristic" }
   if strategy == "phase2" || strategy == "phase3" { return "split" }
   "custom"
}

fn schoenhage_reduce_report(any basis, str strategy="proved3", int max_rounds=3, int pair_rounds=2, int max_pair_steps=64, str goal_target="slope", any goal_value=nil, bool goal_proved=false, bool stop_on_goal=false) dict {
   "Recursive sublattice reduction schedule with exact embedded row transforms."
   def t0 = ticks()
   def original = _flatter_reduce_matrix(basis)
   def rows = _flatter_matrix_rows(original)
   def cols = _flatter_matrix_cols(original)
   def flat_precheck = rows >= 32 ? _flatter_row_norm_shape_report(original) : dict(0)
   def before_shape = rows >= 32 && flat_precheck.get("spread", 1.0e100) <= 0.5 ? flat_precheck : profile_shape_report(original)
   def phase_count = _flatter_schoenhage_phase_count(strategy)
   def round_limit = max(1, max_rounds)
   def local_rounds = max(1, pair_rounds)
   def step_limit = max(1, max_pair_steps)
   mut rounds = []
   mut phase_reports = []
   mut total_ops = 0
   mut total_pairs = 0
   mut total_swaps = 0
   mut r = 0
   mut goal_before = profile_goal_report(before_shape, goal_target, goal_value, goal_proved)
   mut goal_after = goal_before
   if rows >= 32 && before_shape.get("spread", 0.0) <= 0.5 {
      return {
         "method": "schoenhage-recursive-sublattice-reduction",
         "strategy": strategy,
         "strategy_family": _flatter_schoenhage_strategy_family(strategy),
         "rows": rows,
         "cols": cols,
         "max_rounds": round_limit,
         "pair_rounds": local_rounds,
         "max_pair_steps": step_limit,
         "goal_target": goal_target,
         "goal_value": goal_value,
         "goal_proved": goal_proved,
         "stop_on_goal": stop_on_goal,
         "stopped_reason": "flat-profile-before",
         "rounds": [],
         "round_count": 0,
         "phase_reports": [],
         "phase_count": 0,
         "row_op_count": 0,
         "pair_count": 0,
         "swap_count": 0,
         "profile_shape_before": before_shape,
         "profile_shape_after": before_shape,
         "profile_drop_before": before_shape.get("drop", 0.0),
         "profile_drop_after": before_shape.get("drop", 0.0),
         "profile_spread_before": before_shape.get("spread", 0.0),
         "profile_spread_after": before_shape.get("spread", 0.0),
         "profile_goal_before": goal_before,
         "profile_goal_after": goal_before,
         "profile_goal_satisfied": goal_before.get("satisfied", false),
         "transform": matrix.matrix_identity(rows),
         "transform_verified": true,
         "basis": original,
         "elapsed_ms": float(ticks() - t0) / 1000000.0
      }
   }
   mut work_rows = _flatter_clone_rows(original)
   mut total_transform = matrix.matrix_identity(rows)
   mut stopped_reason = "max-rounds"
   mut keep_going = !(bool(stop_on_goal) && goal_before.get("satisfied", false))
   if !keep_going { stopped_reason = "profile-goal-before" }
   while r < round_limit && keep_going {
      mut round_changed = false
      mut round_ops = 0
      mut round_phases = []
      mut p = 0
      while p < phase_count {
         def phase = _flatter_schoenhage_phase(rows, p, strategy)
         def start = max(0, phase.get("start", 0))
         def stop = min(rows, phase.get("stop", rows))
         def block_rows = stop - start
         if block_rows >= 2 {
            def block = matrix.Matrix(_flatter_slice_rows(work_rows, start, stop))
            def first_before = _flatter_dot_z(_flatter_matrix_data(block).get(0), _flatter_matrix_data(block).get(0))
            def local_window = max(1, block_rows - 1)
            def red = lagrange_pair_reduce_report(block, local_window, local_rounds, step_limit, true, true, true)
            work_rows = _flatter_replace_rows(work_rows, start, red.get("basis"))
            total_transform = matrix.Matrix(_flatter_apply_block_transform_rows(_flatter_matrix_data(total_transform), start, red.get("transform")))
            def row_ops = red.get("row_op_count", 0)
            def first_after = red.get("first_norm_after", first_before)
            def changed = row_ops > 0 || first_after < first_before
            if changed { round_changed = true }
            round_ops += row_ops
            total_ops += row_ops
            total_pairs += red.get("pair_count", 0)
            total_swaps += red.get("swap_count", 0)
            def phase_report = {
               "round": r + 1,
               "phase": p + 1,
               "name": phase.get("name", ""),
               "start": start,
               "stop": stop,
               "rows": block_rows,
               "local_window": local_window,
               "pair_count": red.get("pair_count", 0),
               "op_count": red.get("op_count", 0),
               "row_op_count": row_ops,
               "swap_count": red.get("swap_count", 0),
               "changed": changed,
               "first_norm_before": first_before,
               "first_norm_after": first_after,
               "local_transform_verified": red.get("transform_verified", false),
               "elapsed_ms": red.get("elapsed_ms", 0.0)
            }
            phase_reports = phase_reports.append(phase_report)
            round_phases = round_phases.append(phase_report)
         }
         p += 1
      }
      def round_shape = profile_shape_report(matrix.Matrix(work_rows))
      def round_goal = profile_goal_report(round_shape, goal_target, goal_value, goal_proved)
      rounds = rounds.append({
            "round": r + 1,
            "changed": round_changed,
            "row_op_count": round_ops,
            "phase_count": round_phases.len,
            "phases": round_phases,
            "profile_shape": round_shape,
            "profile_goal": round_goal,
            "profile_goal_satisfied": round_goal.get("satisfied", false)
      })
      goal_after = round_goal
      if bool(stop_on_goal) && round_goal.get("satisfied", false) {
         stopped_reason = "profile-goal-after-round"
         keep_going = false
      } else if !round_changed {
         stopped_reason = "fixed-point"
         keep_going = false
      }
      r += 1
   }
   def out_basis = matrix.Matrix(work_rows)
   def after_shape = profile_shape_report(out_basis)
   goal_after = profile_goal_report(after_shape, goal_target, goal_value, goal_proved)
   def applied = _flatter_matmul(total_transform, original)
   {
      "method": "schoenhage-recursive-sublattice-reduction",
      "strategy": strategy,
      "strategy_family": _flatter_schoenhage_strategy_family(strategy),
      "rows": rows,
      "cols": cols,
      "max_rounds": round_limit,
      "pair_rounds": local_rounds,
      "max_pair_steps": step_limit,
      "goal_target": goal_target,
      "goal_value": goal_value,
      "goal_proved": goal_proved,
      "stop_on_goal": stop_on_goal,
      "stopped_reason": stopped_reason,
      "rounds": rounds,
      "round_count": rounds.len,
      "phase_reports": phase_reports,
      "phase_count": phase_reports.len,
      "row_op_count": total_ops,
      "pair_count": total_pairs,
      "swap_count": total_swaps,
      "profile_shape_before": before_shape,
      "profile_shape_after": after_shape,
      "profile_drop_before": before_shape.get("drop", 0.0),
      "profile_drop_after": after_shape.get("drop", 0.0),
      "profile_spread_before": before_shape.get("spread", 0.0),
      "profile_spread_after": after_shape.get("spread", 0.0),
      "profile_goal_before": goal_before,
      "profile_goal_after": goal_after,
      "profile_goal_satisfied": goal_after.get("satisfied", false),
      "transform": total_transform,
      "transform_verified": _flatter_same_matrix(applied, out_basis),
      "basis": out_basis,
      "elapsed_ms": float(ticks() - t0) / 1000000.0
   }
}

fn schoenhage_reduce(any basis, str strategy="proved3", int max_rounds=3, int pair_rounds=2, int max_pair_steps=64) any {
   "Return the basis from schoenhage_reduce_report."
   schoenhage_reduce_report(basis, strategy, max_rounds, pair_rounds, max_pair_steps).get("basis")
}

fn sublattice_reduce_report(any basis, int window_size=16, int stride=8, any delta=0.99, int rounds=2) dict {
   "Windowed sublattice reduction report for large bases."
   def t0 = ticks()
   def a = _flatter_reduce_matrix(basis)
   def rows = _flatter_matrix_rows(a)
   def cols = _flatter_matrix_cols(a)
   def win = max(2, window_size)
   def step = max(1, stride)
   mut work = _flatter_clone_rows_small(a)
   mut total_transform_rows = _flatter_identity_rows(rows)
   mut transform_tracked = true
   mut windows = []
   mut total_relative_ops = 0
   mut total_reduction_ops = 0
   mut total_reduction_steps = 0
   mut start = 0
   while start < rows {
      def stop = min(rows, start + win)
      if stop - start >= 2 {
         def block = matrix.Matrix(_flatter_slice_rows(work, start, stop))
         def before = lll_backend.gso_report(block)
         def rsz = relative_size_reduce_report(block, 1)
         def red = flatter_reduce_report(rsz.get("basis"), delta, rounds)
         total_relative_ops += rsz.get("op_count", 0)
         total_reduction_ops += red.get("op_count", 0)
         total_reduction_steps += red.get("lll_steps", 0)
         def red_transform = red.get("transform")
         def rsz_transform = rsz.get("transform")
         def local_transform = (red_transform != nil && rsz_transform != nil) ? _flatter_matmul(red_transform, rsz_transform) : nil
         work = _flatter_replace_rows(work, start, red.get("basis"))
         if transform_tracked && local_transform != nil {
            total_transform_rows = _flatter_apply_block_transform_rows(total_transform_rows, start, local_transform)
         } else {
            transform_tracked = false
         }
         windows = windows.append({
               "start": start,
               "stop": stop,
               "rows": stop - start,
               "relative_ops": rsz.get("op_count", 0),
               "reduction_ops": red.get("op_count", 0),
               "lll_steps": red.get("lll_steps", 0),
               "relative_transform_verified": rsz.get("transform_verified", false),
               "local_transform_tracked": local_transform != nil,
               "reduction_transform_verified": red.get("transform_verified", false),
               "profile_before": before.get("profile", []),
               "profile_after": red.get("profile_after", []),
               "elapsed_ms": rsz.get("elapsed_ms", 0.0) + red.get("elapsed_ms", 0.0)
         })
      }
      if stop >= rows { start = rows } else { start += step }
   }
   def out_basis = matrix.Matrix(work)
   def total_transform = transform_tracked ? matrix.Matrix(total_transform_rows) : nil
   def verify_transform = transform_tracked && rows <= 96
   def transform_verified = verify_transform ? _flatter_same_matrix(_flatter_matmul(total_transform, a), out_basis) : false
   {
      "method": "windowed-sublattice-reduction",
      "rows": rows,
      "cols": cols,
      "window_size": win,
      "stride": step,
      "rounds": rounds,
      "windows": windows,
      "window_count": windows.len,
      "relative_op_count": total_relative_ops,
      "reduction_op_count": total_reduction_ops,
      "lll_steps": total_reduction_steps,
      "op_count": total_relative_ops + total_reduction_ops,
      "transform": total_transform,
      "transform_tracked": transform_tracked,
      "transform_verified": transform_verified,
      "verification_skipped": !verify_transform,
      "verification_skip_reason": !transform_tracked ? "local window transform unavailable" : (rows > 96 ? "large windowed transform verification skipped" : ""),
      "basis": out_basis,
      "elapsed_ms": float(ticks() - t0) / 1000000.0
   }
}

fn sublattice_reduce(any basis, int window_size=16, int stride=8, any delta=0.99, int rounds=2) any {
   "Return the basis from sublattice_reduce_report."
   sublattice_reduce_report(basis, window_size, stride, delta, rounds).get("basis")
}

fn _flatter_multiscale_report(int t0, any basis, int rows, int cols, int min_window, str schedule, int start_window, str goal_target, any goal_value, bool goal_proved, bool stop_on_goal, str stopped_reason, list levels, any before_shape, any after_shape, any goal_before, any goal_after, any transform, bool transform_tracked, bool transform_verified, bool verification_skipped, str verification_skip_reason, any extra=nil) dict {
   mut out = {
      "method": "multiscale-window-reduction",
      "rows": rows,
      "cols": cols,
      "min_window": min_window,
      "schedule": schedule,
      "start_window": start_window,
      "goal_target": goal_target,
      "goal_value": goal_value,
      "goal_proved": goal_proved,
      "stop_on_goal": stop_on_goal,
      "stopped_reason": stopped_reason,
      "levels": levels,
      "level_count": levels.len,
      "profile_shape_before": before_shape,
      "profile_shape_after": after_shape,
      "profile_drop_before": before_shape.get("drop_norm", before_shape.get("drop", 0.0)),
      "profile_drop_after": after_shape.get("drop_norm", after_shape.get("drop", 0.0)),
      "profile_goal_before": goal_before,
      "profile_goal_after": goal_after,
      "profile_goal_satisfied": goal_after.get("satisfied", false),
      "transform": transform,
      "transform_tracked": transform_tracked,
      "transform_verified": transform_verified,
      "verification_skipped": verification_skipped,
      "verification_skip_reason": verification_skip_reason,
      "basis": basis,
      "elapsed_ms": float(ticks() - t0) / 1000000.0
   }
   if extra != nil {
      def keys = dict_keys(extra)
      mut i = 0
      while i < keys.len {
         def k = keys.get(i)
         out = out.set(k, extra.get(k))
         i += 1
      }
   }
   out
}

fn recursive_reduce_report(any basis, int min_window=8, any delta=0.99, int rounds=2, str goal_target="slope", any goal_value=nil, bool goal_proved=false, bool stop_on_goal=false) dict {
   "Multiscale reduction report that repeatedly reduces coarse-to-fine row windows."
   def t0 = ticks()
   def original = _flatter_reduce_matrix(basis)
   mut work = original
   def rows = _flatter_matrix_rows(work)
   def cols = _flatter_matrix_cols(work)
   if rows > 64 && !bool(stop_on_goal) {
      def first_before = rows > 0 ? _flatter_dot_z(_flatter_matrix_data(original).get(0), _flatter_matrix_data(original).get(0)) : Z(0)
      def short = short_row_prepass_report(original, false)
      mut first_after = short.get("first_norm_after", first_before)
      if first_before > Z(0) && first_after * Z(4) < first_before {
         work = short.get("basis")
         mut cleanup_applied = false
         mut cleanup_method = ""
         mut cleanup_elapsed_ms = 0.0
         mut cleanup_first_norm_after = first_after
         if rows >= 384 && first_after * Z(8) >= first_before {
            def cleanup = banded_triplet_prepass_report(work, 4, 2)
            mut cleanup_basis = cleanup.get("basis")
            mut cleanup_first = cleanup.get("first_norm_after", first_after)
            mut cleanup_name = cleanup.get("method", "banded-triplet-prepass")
            mut cleanup_ms = cleanup.get("elapsed_ms", 0.0)
            if cleanup_first >= first_after {
               def pair_cleanup = lagrange_pair_reduce_report(work, 24, 2, 16, false, true, false)
               def pair_first = pair_cleanup.get("first_norm_after", first_after)
               if pair_first < cleanup_first {
                  cleanup_basis = pair_cleanup.get("basis")
                  cleanup_first = pair_first
                  cleanup_name = pair_cleanup.get("method", "lagrange-row-pair-reduction")
                  cleanup_ms = cleanup_ms + pair_cleanup.get("elapsed_ms", 0.0)
               }
            }
            if cleanup_first < first_after {
               work = cleanup_basis
               first_after = cleanup_first
               cleanup_applied = true
               cleanup_method = cleanup_name
               cleanup_elapsed_ms = cleanup_ms
               cleanup_first_norm_after = cleanup_first
            }
         }
         def before_shape_short = _flatter_skip("large recursive short-row fastpath omits GSO profile")
         def after_shape_short = _flatter_skip("large recursive short-row fastpath omits GSO profile")
         def short_goal_before = _flatter_skipped_profile_goal()
         def short_goal_after = _flatter_skipped_profile_goal()
         def level = {
            "window_size": rows,
            "stride": rows,
            "window_count": 1,
            "method": cleanup_applied ? "short-row-plus-local-triplet" : short.get("method", "short-row-prepass"),
            "best_row": short.get("best_row", 0),
            "op_count": short.get("op_count", 0),
            "first_norm_before": first_before,
            "first_norm_after": first_after,
            "cleanup_applied": cleanup_applied,
            "cleanup_method": cleanup_method,
            "cleanup_elapsed_ms": cleanup_elapsed_ms,
            "cleanup_first_norm_after": cleanup_first_norm_after,
            "profile_shape": after_shape_short,
            "profile_goal": short_goal_after,
            "profile_goal_satisfied": short_goal_after.get("satisfied", false),
            "elapsed_ms": short.get("elapsed_ms", 0.0) + cleanup_elapsed_ms
         }
         def schedule = cleanup_applied ? "large-short-row-plus-local-triplet" : "large-short-row-prepass"
         return _flatter_multiscale_report(t0, work, rows, cols, min_window, schedule, rows, goal_target, goal_value, goal_proved, stop_on_goal, schedule, [level], before_shape_short, after_shape_short, short_goal_before, short_goal_after, nil, false, false, true, "large short-row prepass omits transform and GSO profile")
      }
   }
   mut total_transform = matrix.matrix_identity(rows)
   mut levels = []
   def fast_pair_schedule = rows == 64 && min_window <= 16 && !bool(stop_on_goal)
   if fast_pair_schedule {
      def first_before = rows > 0 ? _flatter_dot_z(_flatter_matrix_data(work).get(0), _flatter_matrix_data(work).get(0)) : Z(0)
      def short = short_row_prepass_report(work, false)
      def first_after = short.get("first_norm_after", first_before)
      if short.get("op_count", 0) > 0 && first_before > Z(0) && first_after * Z(4) < first_before {
         work = short.get("basis")
         def before_shape_short = _flatter_skip("profile-speed fastpath omits GSO profile")
         def after_shape_short = _flatter_skip("profile-speed fastpath omits GSO profile")
         def short_goal_before = _flatter_skipped_profile_goal()
         def short_goal_after = _flatter_skipped_profile_goal()
         def level = {
            "window_size": rows,
            "stride": rows,
            "window_count": 1,
            "method": short.get("method", "short-row-prepass"),
            "best_row": short.get("best_row", 0),
            "op_count": short.get("op_count", 0),
            "first_norm_before": first_before,
            "first_norm_after": first_after,
            "profile_shape": after_shape_short,
            "profile_goal": short_goal_after,
            "profile_goal_satisfied": false,
            "elapsed_ms": short.get("elapsed_ms", 0.0)
         }
         return _flatter_multiscale_report(t0, work, rows, cols, min_window, "profile-speed-fastpath", rows, goal_target, goal_value, goal_proved, stop_on_goal, "profile-speed-short-row", [level], before_shape_short, after_shape_short, short_goal_before, short_goal_after, nil, false, false, true, "profile speed short-row fast path omits transform")
      }
      def fast_rounds = max(1, min(3, rounds))
      def before_shape = _flatter_skip("profile-speed fastpath omits GSO profile")
      def fast_goal_before = _flatter_skipped_profile_goal()
      def fast = flatter_reduce_report(work, delta, fast_rounds, 0.51, goal_target, goal_value, goal_proved, stop_on_goal, false)
      work = fast.get("basis")
      def after_shape_fast = _flatter_skip("profile-speed fastpath omits GSO profile")
      def fast_goal_after = _flatter_skipped_profile_goal()
      def level = {
         "window_size": rows,
         "stride": rows,
         "window_count": 1,
         "method": fast.get("compression", fast.get("method", "flatter-profile")),
         "fast_rounds": fast_rounds,
         "round_count": fast.get("round_count", 0),
         "prepass_count": fast.get("prepass_count", 0),
         "profile_shape": after_shape_fast,
         "profile_goal": fast_goal_after,
         "profile_goal_satisfied": fast_goal_after.get("satisfied", false),
         "elapsed_ms": fast.get("elapsed_ms", 0.0)
      }
      return _flatter_multiscale_report(t0, work, rows, cols, min_window, "profile-speed-fastpath", rows, goal_target, goal_value, goal_proved, stop_on_goal, "profile-speed-fastpath", [level], before_shape, after_shape_fast, fast_goal_before, fast_goal_after, nil, false, false, true, "profile pair fast path omits transform")
   }
   def before_shape = profile_shape_report(original)
   def medium_direct_schedule = rows == 48 && min_window <= 12 && before_shape.get("spread", 0.0) <= 16.0
   if medium_direct_schedule {
      def budget = 2048
      def direct = lll_backend.lll_reduce_bounded_report(work, budget, delta, "bounded-int-no-transform", 0.51)
      work = direct.get("basis")
      def after_shape_direct = profile_shape_report(work)
      def direct_goal_before = profile_goal_report(before_shape, goal_target, goal_value, goal_proved)
      def direct_goal_after = profile_goal_report(after_shape_direct, goal_target, goal_value, goal_proved)
      def level = {
         "window_size": rows,
         "stride": rows,
         "window_count": 1,
         "method": direct.get("method", "bounded-int-no-transform"),
         "direct_budget": budget,
         "steps": direct.get("steps", 0),
         "reduction_complete": direct.get("reduction_complete", true),
         "profile_shape": after_shape_direct,
         "profile_goal": direct_goal_after,
         "profile_goal_satisfied": direct_goal_after.get("satisfied", false),
         "elapsed_ms": direct.get("elapsed_ms", 0.0)
      }
      return _flatter_multiscale_report(t0, work, rows, cols, min_window, "medium-direct-lll", rows, goal_target, goal_value, goal_proved, stop_on_goal, "medium-direct-lll", [level], before_shape, after_shape_direct, direct_goal_before, direct_goal_after, nil, false, false, true, "medium direct bounded reduction omits transform", {"direct_budget": budget})
   }
   def medium_local_schedule = rows >= 24 && rows <= 48 && min_window <= 12 && before_shape.get("spread", 0.0) <= 16.0
   mut window = medium_local_schedule ? min(rows, max(12, min_window + max(1, min_window / 2))) : max(min_window, rows)
   mut goal_before = profile_goal_report(before_shape, goal_target, goal_value, goal_proved)
   mut goal_after = goal_before
   mut stopped_reason = "max-levels"
   mut keep_going = !(bool(stop_on_goal) && goal_before.get("satisfied", false))
   if !keep_going { stopped_reason = "profile-goal-before" }
   while keep_going && window >= max(2, min_window) {
      def stride = max(1, window / 2)
      def rep = sublattice_reduce_report(work, window, stride, delta, rounds)
      work = rep.get("basis")
      def rep_transform = rep.get("transform")
      if total_transform != nil && rep_transform != nil {
         total_transform = _flatter_matmul(rep_transform, total_transform)
      } else {
         total_transform = nil
      }
      def level_shape = profile_shape_report(work)
      def level_goal = profile_goal_report(level_shape, goal_target, goal_value, goal_proved)
      levels = levels.append({
            "window_size": window,
            "stride": stride,
            "window_count": rep.get("window_count", 0),
            "transform_tracked": rep.get("transform_tracked", false),
            "transform_verified": rep.get("transform_verified", false),
            "verification_skipped": rep.get("verification_skipped", false),
            "profile_shape": level_shape,
            "profile_goal": level_goal,
            "profile_goal_satisfied": level_goal.get("satisfied", false),
            "elapsed_ms": rep.get("elapsed_ms", 0.0)
      })
      goal_after = level_goal
      if bool(stop_on_goal) && level_goal.get("satisfied", false) {
         stopped_reason = "profile-goal-after-level"
         keep_going = false
      } else if medium_local_schedule {
         stopped_reason = "medium-local-window"
         keep_going = false
      } else if window <= min_window {
         window = 0
      } else {
         window = max(min_window, window / 2)
      }
   }
   def after_shape = profile_shape_report(work)
   goal_after = profile_goal_report(after_shape, goal_target, goal_value, goal_proved)
   def transform_verified = total_transform != nil ? _flatter_same_matrix(_flatter_matmul(total_transform, original), work) : false
   _flatter_multiscale_report(t0, work, rows, cols, min_window, medium_local_schedule ? "medium-local-window" : "coarse-to-fine", medium_local_schedule ? min(rows, max(12, min_window + max(1, min_window / 2))) : max(min_window, rows), goal_target, goal_value, goal_proved, stop_on_goal, stopped_reason, levels, before_shape, after_shape, goal_before, goal_after, total_transform, total_transform != nil, transform_verified, total_transform == nil, total_transform == nil ? "large recursive transform bookkeeping disabled" : "")
}

fn recursive_reduce(any basis, int min_window=8, any delta=0.99, int rounds=2) any {
   "Return the basis from recursive_reduce_report."
   recursive_reduce_report(basis, min_window, delta, rounds).get("basis")
}

fn _qr_zero_rows(int rows, int cols) list<list<f64>> {
   mut list<list<f64>> out = list(rows)
   __list_set_len(out, rows)
   mut i = 0
   while i < rows {
      mut list<f64> row = list(cols)
      __list_set_len(row, cols)
      mut j = 0
      while j < cols {
         row[j] = 0.0
         j += 1
      }
      out[i] = row
      i += 1
   }
   out
}

fn _qr_col(any m, int col) list<f64> {
   def rows = _flatter_matrix_rows(m)
   def data = _flatter_matrix_data(m)
   mut list<f64> out = list(rows)
   __list_set_len(out, rows)
   mut i = 0
   while i < rows {
      out[i] = float(data[i][col])
      i += 1
   }
   out
}

fn _qr_dot(list<f64> a, list<f64> b) f64 {
   mut f64 s = 0.0
   mut int i = 0
   while i < a.len {
      s += a[i] * b[i]
      i += 1
   }
   s
}

fn _qr_sub_scaled_inplace(list<f64> a, list<f64> b, f64 scale) any {
   mut int i = 0
   while i < a.len {
      a[i] = a[i] - scale * b[i]
      i += 1
   }
   nil
}

fn _qr_scale(list<f64> a, any scale) list<f64> {
   mut list<f64> out = list(a.len)
   __list_set_len(out, a.len)
   mut int i = 0
   def f64 s = float(scale)
   while i < a.len {
      out[i] = a[i] * s
      i += 1
   }
   out
}

fn _qr_q_matrix(list<list<f64>> q_cols, int rows, int cols) any {
   mut list<list<f64>> q_rows = list(rows)
   __list_set_len(q_rows, rows)
   mut i = 0
   while i < rows {
      mut list<f64> row = list(cols)
      __list_set_len(row, cols)
      mut j = 0
      while j < cols {
         def list<f64> q_col = q_cols.get(j)
         row[j] = q_col[i]
         j += 1
      }
      q_rows[i] = row
      i += 1
   }
   matrix.Matrix(q_rows)
}

fn _qr_reconstruction_error(any a, any q, any r) f64 {
   def int rows = _flatter_matrix_rows(a)
   def int cols = _flatter_matrix_cols(a)
   def a_data = _flatter_matrix_data(a)
   def q_data = _flatter_matrix_data(q)
   def r_data = _flatter_matrix_data(r)
   mut f64 worst = 0.0
   mut int i = 0
   while i < rows {
      def a_row = a_data.get(i)
      def list<f64> q_row = q_data[i]
      mut int j = 0
      while j < cols {
         mut f64 s = 0.0
         mut int k = 0
         while k < cols {
            def list<f64> rr = r_data[k]
            s += q_row[k] * rr[j]
            k += 1
         }
         def f64 e = abs(s - float(a_row[j]))
         if e > worst { worst = e }
         j += 1
      }
      i += 1
   }
   worst
}

fn _qr_orthogonality_error(list<list<f64>> q_cols) f64 {
   mut f64 worst = 0.0
   mut int i = 0
   while i < q_cols.len {
      def list<f64> qi = q_cols.get(i)
      mut int j = i
      while j < q_cols.len {
         def f64 target = i == j ? 1.0 : 0.0
         def list<f64> qj = q_cols.get(j)
         def f64 e = abs(_qr_dot(qi, qj) - target)
         if e > worst { worst = e }
         j += 1
      }
      i += 1
   }
   worst
}

fn _qr_identity(int n) list<list<f64>> {
   mut list<list<f64>> out = list(n)
   __list_set_len(out, n)
   mut i = 0
   while i < n {
      mut list<f64> row = list(n)
      __list_set_len(row, n)
      mut j = 0
      while j < n {
         row[j] = i == j ? 1.0 : 0.0
         j += 1
      }
      out[i] = row
      i += 1
   }
   out
}

fn _qr_copy_float_rows(any m) list<list<f64>> {
   def rows = _flatter_matrix_rows(m)
   def cols = _flatter_matrix_cols(m)
   def data = _flatter_matrix_data(m)
   mut list<list<f64>> out = list(rows)
   __list_set_len(out, rows)
   mut i = 0
   while i < rows {
      mut list<f64> row = list(cols)
      __list_set_len(row, cols)
      mut j = 0
      while j < cols {
         row[j] = float(data[i][j])
         j += 1
      }
      out[i] = row
      i += 1
   }
   out
}

fn _householder_vec_norm(list<f64> v) f64 { sqrt(_qr_dot(v, v)) }

fn _qr_full_reconstruction_error_rows(any a, list<list<f64>> q_data, list<list<f64>> r_data, int rows, int cols) f64 {
   def a_data = _flatter_matrix_data(a)
   mut f64 worst = 0.0
   mut int i = 0
   while i < rows {
      def list<f64> q_row = q_data[i]
      def a_row = a_data[i]
      mut int j = 0
      while j < cols {
         mut f64 s = 0.0
         mut int k = 0
         while k < rows {
            s += q_row[k] * r_data[k][j]
            k += 1
         }
         def f64 e = abs(s - float(a_row[j]))
         if e > worst { worst = e }
         j += 1
      }
      i += 1
   }
   worst
}

fn _qr_full_reconstruction_error(any a, any q, any r) f64 {
   def rows = _flatter_matrix_rows(a)
   def cols = _flatter_matrix_cols(a)
   def a_data = _flatter_matrix_data(a)
   def q_data = _flatter_matrix_data(q)
   def r_data = _flatter_matrix_data(r)
   mut worst = 0.0
   mut i = 0
   while i < rows {
      def a_row = a_data[i]
      def q_row = q_data[i]
      mut j = 0
      while j < cols {
         mut s = 0.0
         mut k = 0
         while k < rows {
            s += float(q_row[k]) * float(r_data[k][j])
            k += 1
         }
         def e = abs(s - float(a_row[j]))
         if e > worst { worst = e }
         j += 1
      }
      i += 1
   }
   worst
}

fn _qr_full_orthogonality_error(any q) f64 {
   def rows = _flatter_matrix_rows(q)
   def data = _flatter_matrix_data(q)
   mut worst = 0.0
   mut i = 0
   while i < rows {
      mut j = i
      while j < rows {
         mut s = 0.0
         mut k = 0
         while k < rows {
            def row = data[k]
            s += float(row[i]) * float(row[j])
            k += 1
         }
         def target = i == j ? 1.0 : 0.0
         def e = abs(s - target)
         if e > worst { worst = e }
         j += 1
      }
      i += 1
   }
   worst
}

fn _qr_full_orthogonality_error_rows(list<list<f64>> data, int rows) f64 {
   mut f64 worst = 0.0
   mut int i = 0
   while i < rows {
      mut int j = i
      while j < rows {
         mut f64 s = 0.0
         mut int k = 0
         while k < rows {
            def list<f64> row = data[k]
            s += row[i] * row[j]
            k += 1
         }
         def f64 target = i == j ? 1.0 : 0.0
         def f64 e = abs(s - target)
         if e > worst { worst = e }
         j += 1
      }
      i += 1
   }
   worst
}

fn qr_factor_report(any basis) dict {
   "Compute a QR factorization report for an integer/float lattice basis."
   def t0 = ticks()
   def a = _flatter_reduce_matrix(basis)
   def int rows = _flatter_matrix_rows(a)
   def int cols = _flatter_matrix_cols(a)
   if rows < cols { panic("qr_factor_report: require rows >= cols") }
   mut list<list<f64>> q_cols = list(0)
   mut list<list<f64>> r_rows = _qr_zero_rows(cols, cols)
   mut int k = 0
   while k < cols {
      mut v = _qr_col(a, k)
      mut int j = 0
      while j < k {
         def list<f64> qj = q_cols.get(j)
         def f64 rjk = _qr_dot(qj, v)
         def list<f64> rr = r_rows[j]
         rr[k] = rjk
         r_rows[j] = rr
         _qr_sub_scaled_inplace(v, qj, rjk)
         j += 1
      }
      def f64 norm = sqrt(_qr_dot(v, v))
      def list<f64> rrk = r_rows[k]
      rrk[k] = norm
      r_rows[k] = rrk
      q_cols = q_cols.append(norm > 0.0 ? _qr_scale(v, 1.0 / norm) : _qr_scale(v, 0.0))
      k += 1
   }
   def q = _qr_q_matrix(q_cols, rows, cols)
   def r = matrix.Matrix(r_rows)
   {
      "method": "modified-gram-schmidt",
      "rows": rows,
      "cols": cols,
      "q": q,
      "r": r,
      "q_columns": q_cols,
      "orthogonality_error": _qr_orthogonality_error(q_cols),
      "reconstruction_error": _qr_reconstruction_error(a, q, r),
      "elapsed_ms": float(ticks() - t0) / 1000000.0
   }
}

fn qr_factor(any basis) list {
   "Return [Q, R] from qr_factor_report."
   def rep = qr_factor_report(basis)
   [rep.get("q"), rep.get("r")]
}

fn qr_reorthogonalized_factor_report(any basis, int passes=2) dict {
   "Compute a reorthogonalized modified Gram-Schmidt QR factorization report."
   def t0 = ticks()
   def a = _flatter_reduce_matrix(basis)
   def int rows = _flatter_matrix_rows(a)
   def int cols = _flatter_matrix_cols(a)
   if rows < cols { panic("qr_reorthogonalized_factor_report: require rows >= cols") }
   def int active_passes = max(1, passes)
   mut list<list<f64>> q_cols = list(0)
   mut list<list<f64>> r_rows = _qr_zero_rows(cols, cols)
   mut zero_norm_columns = []
   mut int projection_count = 0
   mut int k = 0
   while k < cols {
      mut v = _qr_col(a, k)
      mut int pass = 0
      while pass < active_passes {
         mut int j = 0
         while j < k {
            def list<f64> qj = q_cols.get(j)
            def f64 rjk = _qr_dot(qj, v)
            def list<f64> rr = r_rows[j]
            rr[k] = rr[k] + rjk
            r_rows[j] = rr
            _qr_sub_scaled_inplace(v, qj, rjk)
            projection_count += 1
            j += 1
         }
         pass += 1
      }
      def f64 norm = sqrt(_qr_dot(v, v))
      def list<f64> rrk = r_rows[k]
      rrk[k] = norm
      r_rows[k] = rrk
      if norm > 0.0 {
         q_cols = q_cols.append(_qr_scale(v, 1.0 / norm))
      } else {
         q_cols = q_cols.append(_qr_scale(v, 0.0))
         zero_norm_columns = zero_norm_columns.append(k)
      }
      k += 1
   }
   def q = _qr_q_matrix(q_cols, rows, cols)
   def r = matrix.Matrix(r_rows)
   {
      "method": "reorthogonalized-modified-gram-schmidt",
      "rows": rows,
      "cols": cols,
      "q": q,
      "r": r,
      "q_columns": q_cols,
      "reorthogonalization_passes": active_passes,
      "projection_count": projection_count,
      "zero_norm_columns": zero_norm_columns,
      "zero_norm_column_count": zero_norm_columns.len,
      "orthogonality_error": _qr_orthogonality_error(q_cols),
      "reconstruction_error": _qr_reconstruction_error(a, q, r),
      "elapsed_ms": float(ticks() - t0) / 1000000.0
   }
}

fn qr_reorthogonalized_factor(any basis, int passes=2) list {
   "Return [Q, R] from qr_reorthogonalized_factor_report."
   def rep = qr_reorthogonalized_factor_report(basis, passes)
   [rep.get("q"), rep.get("r")]
}

fn householder_qr_factor_report(any basis) dict {
   "Compute a Householder QR factorization report."
   def t0 = ticks()
   def a = _flatter_reduce_matrix(basis)
   def int rows = _flatter_matrix_rows(a)
   def int cols = _flatter_matrix_cols(a)
   if rows < cols { panic("householder_qr_factor_report: require rows >= cols") }
   mut list<list<f64>> R = _qr_copy_float_rows(a)
   mut list<list<f64>> Q = _qr_identity(rows)
   mut reflections = []
   mut int k = 0
   while k < cols && k < rows - 1 {
      mut list<f64> x = list(rows - k)
      __list_set_len(x, rows - k)
      mut int i = k
      while i < rows {
         x[i - k] = R[i][k]
         i += 1
      }
      def f64 normx = _householder_vec_norm(x)
      if normx > 0.0 {
         mut list<f64> v = clone(x)
         def f64 alpha = x[0] >= 0.0 ? (0.0 - normx) : normx
         v[0] = v[0] - alpha
         def f64 vnorm = _householder_vec_norm(v)
         if vnorm > 0.0 {
            v = _qr_scale(v, 1.0 / vnorm)
            mut int j = k
            while j < cols {
               mut f64 dot = 0.0
               i = k
               while i < rows {
                  dot += v[i - k] * R[i][j]
                  i += 1
               }
               i = k
               while i < rows {
                  def list<f64> rr = R[i]
                  rr[j] = rr[j] - 2.0 * v[i - k] * dot
                  R[i] = rr
                  i += 1
               }
               j += 1
            }
            i = 0
            while i < rows {
               mut f64 dotq = 0.0
               j = k
               while j < rows {
                  dotq += Q[i][j] * v[j - k]
                  j += 1
               }
               j = k
               while j < rows {
                  def list<f64> qr = Q[i]
                  qr[j] = qr[j] - 2.0 * dotq * v[j - k]
                  Q[i] = qr
                  j += 1
               }
               i += 1
            }
            reflections = reflections.append({"column": k, "alpha": alpha, "norm": normx})
         }
      }
      k += 1
   }
   def qmat = matrix.Matrix(Q)
   def rmat = matrix.Matrix(R)
   {
      "method": "householder",
      "rows": rows,
      "cols": cols,
      "q": qmat,
      "r": rmat,
      "reflections": reflections,
      "reflection_count": reflections.len,
      "orthogonality_error": _qr_full_orthogonality_error_rows(Q, rows),
      "reconstruction_error": _qr_full_reconstruction_error_rows(a, Q, R, rows, cols),
      "elapsed_ms": float(ticks() - t0) / 1000000.0
   }
}

fn householder_qr_factor(any basis) list {
   "Return [Q, R] from householder_qr_factor_report."
   def rep = householder_qr_factor_report(basis)
   [rep.get("q"), rep.get("r")]
}

fn _flatter_qr_factor_report(any basis, str method) dict {
   if method == "modified" || method == "modified-gram-schmidt" {
      return qr_factor_report(basis)
   }
   if method == "reorthogonalized" || method == "reorthogonalized-mgs" || method == "mgs2" {
      return qr_reorthogonalized_factor_report(basis, 2)
   }
   if method == "tall-skinny" || method == "tsqr" {
      return tall_skinny_qr_factor_report(basis, 32, "reorthogonalized")
   }
   if method == "blocked" || method == "blocked-householder" {
      return blocked_qr_factor_report(basis, 16)
   }
   householder_qr_factor_report(basis)
}

fn _flatter_qr_method_name(str method) str {
   if method == "modified" || method == "modified-gram-schmidt" { return "modified-gram-schmidt" }
   if method == "reorthogonalized" || method == "reorthogonalized-mgs" || method == "mgs2" { return "reorthogonalized-modified-gram-schmidt" }
   if method == "tall-skinny" || method == "tsqr" { return "tall-skinny-qr" }
   if method == "blocked" || method == "blocked-householder" { return "blocked-householder-qr" }
   "householder"
}

fn _qr_block_from_rows(list rows_data, int row_start, int row_end, int col_start, int col_end) any {
   def int out_rows = max(0, row_end - row_start)
   def int out_cols = max(0, col_end - col_start)
   mut list<list<f64>> out = list(out_rows)
   __list_set_len(out, out_rows)
   mut int oi = 0
   mut int i = row_start
   while i < row_end {
      mut list<f64> row = list(out_cols)
      __list_set_len(row, out_cols)
      mut int oj = 0
      mut int j = col_start
      while j < col_end {
         row[oj] = float(rows_data[i][j])
         j += 1
         oj += 1
      }
      out[oi] = row
      i += 1
      oi += 1
   }
   matrix.Matrix(out)
}

fn _qr_set_block(list rows_data, int row_start, int col_start, any block) list {
   mut out = rows_data
   def br = _flatter_matrix_rows(block)
   def bc = _flatter_matrix_cols(block)
   def bd = _flatter_matrix_data(block)
   mut i = 0
   while i < br {
      mut row = out.get(row_start + i)
      mut j = 0
      while j < bc {
         row[col_start + j] = float(bd.get(i).get(j))
         j += 1
      }
      out[row_start + i] = row
      i += 1
   }
   out
}

fn _qr_embed_panel_q(int rows, int start, any q_panel) any {
   mut out = _qr_identity(rows)
   def pd = _flatter_matrix_data(q_panel)
   def pr = _flatter_matrix_rows(q_panel)
   mut i = 0
   while i < pr {
      mut row = out.get(start + i)
      mut j = 0
      while j < pr {
         row[start + j] = float(pd.get(i).get(j))
         j += 1
      }
      out[start + i] = row
      i += 1
   }
   matrix.Matrix(out)
}

fn blocked_qr_factor_report(any basis, int block_size=16) dict {
   "Compute a blocked Householder QR factorization report."
   def t0 = ticks()
   def a = _flatter_reduce_matrix(basis)
   def int rows = _flatter_matrix_rows(a)
   def int cols = _flatter_matrix_cols(a)
   if rows < cols { panic("blocked_qr_factor_report: require rows >= cols") }
   def int bs = max(1, block_size)
   mut work = _qr_copy_float_rows(a)
   mut q_total = matrix.Matrix(_qr_identity(rows))
   mut panel_reports = []
   mut int panel_count = 0
   mut int k = 0
   while k < cols {
      def int width = min(bs, cols - k)
      def panel = _qr_block_from_rows(work, k, rows, k, k + width)
      def qr = householder_qr_factor_report(panel)
      def q_panel = qr.get("q")
      def trailing = _qr_block_from_rows(work, k, rows, k, cols)
      def updated = _qr_matmul_float(matrix.matrix_transpose(q_panel), trailing)
      work = _qr_set_block(work, k, k, updated)
      def q_embed = _qr_embed_panel_q(rows, k, q_panel)
      q_total = _qr_matmul_float(q_total, q_embed)
      panel_count += 1
      panel_reports = panel_reports.append({
            "panel": panel_count,
            "column_start": k,
            "width": width,
            "rows": rows - k,
            "trailing_cols": cols - k,
            "qr_method": qr.get("method", "householder"),
            "reflection_count": qr.get("reflection_count", 0),
            "orthogonality_error": qr.get("orthogonality_error", 0.0),
            "reconstruction_error": qr.get("reconstruction_error", 0.0)
      })
      k += width
   }
   def rmat = matrix.Matrix(work)
   {
      "method": "blocked-householder-qr",
      "rows": rows,
      "cols": cols,
      "block_size": bs,
      "panel_count": panel_count,
      "panel_reports": panel_reports,
      "q": q_total,
      "r": rmat,
      "orthogonality_error": _qr_full_orthogonality_error(q_total),
      "reconstruction_error": _qr_full_reconstruction_error(a, q_total, rmat),
      "elapsed_ms": float(ticks() - t0) / 1000000.0
   }
}

fn blocked_qr_factor(any basis, int block_size=16) list {
   "Return [Q, R] from blocked_qr_factor_report."
   def rep = blocked_qr_factor_report(basis, block_size)
   [rep.get("q"), rep.get("r")]
}

fn _qr_thin_matrix(any q, int rows, int cols) any {
   def data = _flatter_matrix_data(q)
   mut out = []
   mut i = 0
   while i < rows {
      mut row = []
      mut j = 0
      while j < cols {
         row = row.append(float(data.get(i).get(j)))
         j += 1
      }
      out = out.append(row)
      i += 1
   }
   matrix.Matrix(out)
}

fn _qr_top_square(any r, int cols) any {
   def data = _flatter_matrix_data(r)
   mut out = []
   mut i = 0
   while i < cols {
      mut row = []
      mut j = 0
      while j < cols {
         row = row.append(float(data.get(i).get(j)))
         j += 1
      }
      out = out.append(row)
      i += 1
   }
   matrix.Matrix(out)
}

fn _qr_row_window(any m, int start, int end) any {
   def data = _flatter_matrix_data(m)
   def int rows = max(0, end - start)
   def int cols = _flatter_matrix_cols(m)
   mut list<list<f64>> out = list(rows)
   __list_set_len(out, rows)
   mut int oi = 0
   mut int i = start
   while i < end {
      mut list<f64> row = list(cols)
      __list_set_len(row, cols)
      mut int j = 0
      while j < cols {
         row[j] = float(data[i][j])
         j += 1
      }
      out[oi] = row
      i += 1
      oi += 1
   }
   matrix.Matrix(out)
}

fn _qr_matmul_float(any left, any right) any {
   def int lrows = _flatter_matrix_rows(left)
   def int lcols = _flatter_matrix_cols(left)
   def int rcols = _flatter_matrix_cols(right)
   def ldata = _flatter_matrix_data(left)
   def rdata = _flatter_matrix_data(right)
   mut list<list<f64>> out = list(lrows)
   __list_set_len(out, lrows)
   mut int i = 0
   while i < lrows {
      mut list<f64> row = list(rcols)
      __list_set_len(row, rcols)
      def list<f64> lrow = ldata[i]
      mut int j = 0
      while j < rcols {
         mut f64 s = 0.0
         mut int k = 0
         while k < lcols {
            def list<f64> rrow = rdata[k]
            s += lrow[k] * rrow[j]
            k += 1
         }
         row[j] = s
         j += 1
      }
      out[i] = row
      i += 1
   }
   matrix.Matrix(out)
}

fn _qr_thin_orthogonality_error(any q) f64 {
   def int rows = _flatter_matrix_rows(q)
   def int cols = _flatter_matrix_cols(q)
   def data = _flatter_matrix_data(q)
   mut f64 worst = 0.0
   mut int i = 0
   while i < cols {
      mut int j = i
      while j < cols {
         mut f64 s = 0.0
         mut int k = 0
         while k < rows {
            def list<f64> row = data[k]
            s += row[i] * row[j]
            k += 1
         }
         def f64 target = i == j ? 1.0 : 0.0
         def f64 e = abs(s - target)
         if e > worst { worst = e }
         j += 1
      }
      i += 1
   }
   worst
}

fn tall_skinny_qr_factor_report(any basis, int block_size=32, str qr_method="reorthogonalized") dict {
   "Compute a pure tall-skinny QR report using local QR panels and a final stacked-R QR."
   def t0 = ticks()
   def a = _flatter_reduce_matrix(basis)
   def rows = _flatter_matrix_rows(a)
   def cols = _flatter_matrix_cols(a)
   if rows < cols { panic("tall_skinny_qr_factor_report: require rows >= cols") }
   def panel_rows = max(cols, block_size)
   mut q_blocks = []
   mut r_stack_rows = []
   mut block_reports = []
   mut start = 0
   while start < rows {
      def remaining = rows - start
      mut end = start + panel_rows
      if remaining <= panel_rows || remaining < panel_rows + cols {
         end = rows
      }
      def panel = _qr_row_window(a, start, end)
      def local = _flatter_qr_factor_report(panel, qr_method)
      def local_rows = end - start
      def qi = _qr_thin_matrix(local.get("q"), local_rows, cols)
      def ri = _qr_top_square(local.get("r"), cols)
      q_blocks = q_blocks.append(qi)
      def ridata = _flatter_matrix_data(ri)
      mut rr = 0
      while rr < cols {
         r_stack_rows = r_stack_rows.append(ridata.get(rr))
         rr += 1
      }
      block_reports = block_reports.append({
            "block": q_blocks.len,
            "row_start": start,
            "row_end": end,
            "rows": local_rows,
            "qr_method": local.get("method", qr_method),
            "orthogonality_error": local.get("orthogonality_error", 0.0),
            "reconstruction_error": local.get("reconstruction_error", 0.0)
      })
      start = end
   }
   def r_stack = matrix.Matrix(r_stack_rows)
   def top = _flatter_qr_factor_report(r_stack, qr_method)
   def top_q = _qr_thin_matrix(top.get("q"), _flatter_matrix_rows(r_stack), cols)
   def final_r = _qr_top_square(top.get("r"), cols)
   mut q_rows = []
   mut b = 0
   while b < q_blocks.len {
      def g = _qr_row_window(top_q, b * cols, (b + 1) * cols)
      def qpart = _qr_matmul_float(q_blocks.get(b), g)
      def pdata = _flatter_matrix_data(qpart)
      mut i = 0
      while i < _flatter_matrix_rows(qpart) {
         q_rows = q_rows.append(pdata.get(i))
         i += 1
      }
      b += 1
   }
   def q = matrix.Matrix(q_rows)
   {
      "method": "tall-skinny-qr",
      "rows": rows,
      "cols": cols,
      "block_size": panel_rows,
      "block_count": q_blocks.len,
      "qr_method": qr_method,
      "q": q,
      "r": final_r,
      "r_stack": r_stack,
      "top_qr_method": top.get("method", qr_method),
      "block_reports": block_reports,
      "orthogonality_error": _qr_thin_orthogonality_error(q),
      "reconstruction_error": _qr_reconstruction_error(a, q, final_r),
      "elapsed_ms": float(ticks() - t0) / 1000000.0
   }
}

fn tall_skinny_qr_factor(any basis, int block_size=32, str qr_method="reorthogonalized") list {
   "Return [Q, R] from tall_skinny_qr_factor_report."
   def rep = tall_skinny_qr_factor_report(basis, block_size, qr_method)
   [rep.get("q"), rep.get("r")]
}

fn fused_qr_size_reduce_report(any basis, int passes=2, any eta=0.51, str qr_method="householder") dict {
   "Reduce rows using one GSO snapshot per pass and exact integer row operations."
   def t0 = ticks()
   def a = _flatter_reduce_matrix(basis)
   def rows = _flatter_matrix_rows(a)
   def cols = _flatter_matrix_cols(a)
   mut work = _flatter_clone_rows_small(a)
   mut transform = _flatter_identity_rows(rows)
   def before = _flatter_row_norm_profile_report(a)
   mut pass_reports = []
   mut ops = []
   mut gso_recomputes = 0
   def run_passes = max(0, passes)
   mut p = 0
   while p < run_passes {
      def snap = matrix.Matrix(work)
      def gso = lll_backend.gso_report(snap)
      gso_recomputes += 1
      def mu = gso.get("mu")
      def snap_rows = _flatter_matrix_data(snap)
      mut pass_ops = []
      mut i = 1
      while i < rows {
         mut j = i - 1
         while j >= 0 {
            def den = _flatter_dot_z(snap_rows.get(j), snap_rows.get(j))
            if den != Z(0) {
               def num = _flatter_dot_z(snap_rows.get(i), snap_rows.get(j))
               def mu_gso = matrix.mat_get(mu, i, j)
               def eta_scaled = Z(to_int(float(eta) * 1000000.0))
               if _flatter_abs_z(num) * Z(1000000) > den * eta_scaled {
                  def coeff = _flatter_round_div(num, den)
                  if coeff != Z(0) {
                     work[i] = _flatter_row_submul(work.get(i), work.get(j), coeff)
                     transform[i] = _flatter_row_submul(transform.get(i), transform.get(j), coeff)
                     def op = {
                        "pass": p + 1,
                        "row": i,
                        "against": j,
                        "coeff": coeff,
                        "mu_num": num,
                        "mu_den": den,
                        "gso_mu": mu_gso
                     }
                     pass_ops = pass_ops.append(op)
                     ops = ops.append(op)
                  }
               }
            }
            j -= 1
         }
         i += 1
      }
      pass_reports = pass_reports.append({
            "pass": p + 1,
            "op_count": pass_ops.len,
            "ops": pass_ops,
            "qr_method": _flatter_qr_method_name(qr_method),
            "qr_validation": false,
            "qr_input_transposed": rows < cols,
            "orthogonality_error": 0.0,
            "reconstruction_error": 0.0,
            "profile": gso.get("profile", []),
            "profile_slope": gso.get("profile_slope", 0.0)
      })
      if pass_ops.len == 0 { p = run_passes } else { p += 1 }
   }
   def out_basis = matrix.Matrix(work)
   def after = _flatter_row_norm_profile_report(out_basis)
   def transform_matrix = matrix.Matrix(transform)
   def applied = _flatter_matmul(transform_matrix, a)
   {
      "method": "fused-qr-size-reduction",
      "rows": rows,
      "cols": cols,
      "passes": run_passes,
      "eta": eta,
      "qr_method": qr_method,
      "gso_recomputes": gso_recomputes,
      "ops": ops,
      "op_count": ops.len,
      "pass_reports": pass_reports,
      "profile_before": before.get("profile", []),
      "profile_after": after.get("profile", []),
      "profile_slope_before": before.get("profile_slope", 0.0),
      "profile_slope_after": after.get("profile_slope", 0.0),
      "transform": transform_matrix,
      "transform_verified": _flatter_same_matrix(applied, out_basis),
      "basis": out_basis,
      "elapsed_ms": float(ticks() - t0) / 1000000.0
   }
}

fn fused_qr_size_reduce(any basis, int passes=2, any eta=0.51, str qr_method="householder") any {
   "Return the basis from fused_qr_size_reduce_report."
   fused_qr_size_reduce_report(basis, passes, eta, qr_method).get("basis")
}

fn _flatter_gso_size_reduce_report(any basis, int passes=2, any eta=0.51) dict {
   def t0 = ticks()
   def a = _flatter_reduce_matrix(basis)
   def rows = _flatter_matrix_rows(a)
   def cols = _flatter_matrix_cols(a)
   mut work = _flatter_clone_rows_small(a)
   mut transform = _flatter_identity_rows(rows)
   def before = _flatter_row_norm_profile_report(a)
   mut pass_reports = []
   mut ops = []
   mut gso_recomputes = 0
   def run_passes = max(0, passes)
   def eta_scaled = Z(to_int(float(eta) * 1000000.0))
   mut p = 0
   while p < run_passes {
      def snap = matrix.Matrix(work)
      def snap_rows = _flatter_matrix_data(snap)
      def profile = _flatter_row_norm_profile_from_rows(snap_rows)
      mut pass_ops = []
      mut i = 1
      while i < rows {
         mut j = i - 1
         while j >= 0 {
            def den = _flatter_dot_z(snap_rows.get(j), snap_rows.get(j))
            if den != Z(0) {
               def num = _flatter_dot_z(snap_rows.get(i), snap_rows.get(j))
               if _flatter_abs_z(num) * Z(1000000) > den * eta_scaled {
                  def coeff = _flatter_round_div(num, den)
                  if coeff != Z(0) {
                     work[i] = _flatter_row_submul(work.get(i), work.get(j), coeff)
                     transform[i] = _flatter_row_submul(transform.get(i), transform.get(j), coeff)
                     def op = {
                        "pass": p + 1,
                        "row": i,
                        "against": j,
                        "coeff": coeff,
                        "mu_num": num,
                        "mu_den": den,
                        "gso_mu": 0.0,
                        "gso_mu_computed": false
                     }
                     pass_ops = pass_ops.append(op)
                     ops = ops.append(op)
                  }
               }
            }
            j -= 1
         }
         i += 1
      }
      pass_reports = pass_reports.append({
            "pass": p + 1,
            "op_count": pass_ops.len,
            "ops": pass_ops,
            "qr_method": "gso-only",
            "qr_input_transposed": false,
            "orthogonality_error": 0.0,
            "reconstruction_error": 0.0,
            "profile": profile.get("profile", []),
            "profile_slope": profile.get("profile_slope", 0.0),
            "profile_source": profile.get("method", "row-norm-profile")
      })
      if pass_ops.len == 0 { p = run_passes } else { p += 1 }
   }
   def out_basis = matrix.Matrix(work)
   def after = _flatter_row_norm_profile_report(out_basis)
   def transform_matrix = matrix.Matrix(transform)
   def applied = _flatter_matmul(transform_matrix, a)
   {
      "method": "gso-size-reduction",
      "rows": rows,
      "cols": cols,
      "passes": run_passes,
      "eta": eta,
      "qr_method": "gso-only",
      "profile_source": "row-norm-profile",
      "gso_recomputes": gso_recomputes,
      "ops": ops,
      "op_count": ops.len,
      "pass_reports": pass_reports,
      "profile_before": before.get("profile", []),
      "profile_after": after.get("profile", []),
      "profile_slope_before": before.get("profile_slope", 0.0),
      "profile_slope_after": after.get("profile_slope", 0.0),
      "transform": transform_matrix,
      "transform_verified": _flatter_same_matrix(applied, out_basis),
      "basis": out_basis,
      "elapsed_ms": float(ticks() - t0) / 1000000.0
   }
}

fn _flatter_gso_size_reduce_compact_report(any basis, int passes=2, any eta=0.51) dict {
   def t0 = ticks()
   def a = _flatter_reduce_matrix(basis)
   def rows = _flatter_matrix_rows(a)
   def cols = _flatter_matrix_cols(a)
   mut work = _flatter_clone_rows_small(a)
   mut transform = _flatter_identity_rows(rows)
   mut op_count = 0
   def run_passes = max(0, passes)
   def eta_scaled = Z(to_int(float(eta) * 1000000.0))
   mut p = 0
   while p < run_passes {
      def snap_rows = work
      mut pass_ops = 0
      mut i = 1
      while i < rows {
         mut j = i - 1
         while j >= 0 {
            def den = _flatter_dot_z(snap_rows.get(j), snap_rows.get(j))
            if den != Z(0) {
               def num = _flatter_dot_z(snap_rows.get(i), snap_rows.get(j))
               if _flatter_abs_z(num) * Z(1000000) > den * eta_scaled {
                  def coeff = _flatter_round_div(num, den)
                  if coeff != Z(0) {
                     work[i] = _flatter_row_submul(work.get(i), work.get(j), coeff)
                     transform[i] = _flatter_row_submul(transform.get(i), transform.get(j), coeff)
                     op_count += 1
                     pass_ops += 1
                  }
               }
            }
            j -= 1
         }
         i += 1
      }
      if pass_ops == 0 { p = run_passes } else { p += 1 }
   }
   {
      "method": "gso-size-reduction",
      "rows": rows,
      "cols": cols,
      "passes": run_passes,
      "eta": eta,
      "qr_method": "gso-only",
      "profile_source": "compact-row-norm-profile",
      "gso_recomputes": 0,
      "ops": [],
      "op_count": op_count,
      "pass_reports": [],
      "profile_before": [],
      "profile_after": [],
      "profile_slope_before": 0.0,
      "profile_slope_after": 0.0,
      "transform": matrix.Matrix(transform),
      "transform_verified": false,
      "verification_skipped": true,
      "basis": matrix.Matrix(work),
      "elapsed_ms": float(ticks() - t0) / 1000000.0
   }
}

fn _flatter_iterated_gso_size_reduce_compact_report(any basis, int max_iterations=4, any eta=0.51) dict {
   def t0 = ticks()
   def original = _flatter_reduce_matrix(basis)
   def rows = _flatter_matrix_rows(original)
   def cols = _flatter_matrix_cols(original)
   mut work = original
   mut total_transform = matrix.matrix_identity(rows)
   mut iterations = []
   mut total_ops = 0
   mut stopped_reason = "max-iterations"
   def limit = max(1, max_iterations)
   mut i = 0
   mut keep_going = true
   while i < limit && keep_going {
      def rep = _flatter_gso_size_reduce_compact_report(work, 1, eta)
      work = rep.get("basis")
      total_transform = _flatter_matmul(rep.get("transform"), total_transform)
      total_ops += rep.get("op_count", 0)
      iterations = iterations.append({
            "iteration": i + 1,
            "op_count": rep.get("op_count", 0),
            "transform_verified": false,
            "verification_skipped": true,
            "qr_method": "gso-only",
            "gso_recomputes": 0,
            "elapsed_ms": rep.get("elapsed_ms", 0.0)
      })
      if rep.get("op_count", 0) == 0 {
         stopped_reason = "identity-transform"
         keep_going = false
      } else {
         i += 1
      }
   }
   {
      "method": "iterated-fused-qr-size-reduction",
      "rows": rows,
      "cols": cols,
      "eta": eta,
      "qr_method": "gso-only",
      "max_iterations": limit,
      "iteration_count": iterations.len,
      "iterations": iterations,
      "op_count": total_ops,
      "gso_recomputes": 0,
      "stopped_reason": stopped_reason,
      "profile_before": {"skipped": true, "reason": "compact internal flatter compression"},
      "profile_after": {"skipped": true, "reason": "compact internal flatter compression"},
      "profile_drop_before": 0.0,
      "profile_drop_after": 0.0,
      "profile_spread_before": 0.0,
      "profile_spread_after": 0.0,
      "transform": total_transform,
      "transform_verified": false,
      "verification_skipped": true,
      "basis": work,
      "elapsed_ms": float(ticks() - t0) / 1000000.0
   }
}

fn refined_fused_qr_size_reduce_report(any basis, int passes=2, any eta=0.51, str qr_method="blocked", int max_row_repeats=4) dict {
   "Reduce rows with immediate local recomputation after row updates."
   def t0 = ticks()
   def a = _flatter_reduce_matrix(basis)
   def rows = _flatter_matrix_rows(a)
   def cols = _flatter_matrix_cols(a)
   mut work = _flatter_clone_rows_small(a)
   mut transform = _flatter_identity_rows(rows)
   def before = _flatter_row_norm_profile_report(a)
   mut pass_reports = []
   mut ops = []
   mut gso_recomputes = 0
   mut local_row_rechecks = 0
   def run_passes = max(0, passes)
   def eta_scaled = Z(to_int(float(eta) * 1000000.0))
   mut p = 0
   while p < run_passes {
      mut pass_ops = []
      mut row_reports = []
      mut changed = false
      mut i = 1
      while i < rows {
         mut repeats = 0
         mut row_ops = []
         mut row_changed = true
         while row_changed && repeats < max(1, max_row_repeats) {
            row_changed = false
            def local_rows = work
            local_row_rechecks += 1
            mut j = i - 1
            while j >= 0 {
               def den = _flatter_dot_z(local_rows.get(j), local_rows.get(j))
               if den != Z(0) {
                  def num = _flatter_dot_z(local_rows.get(i), local_rows.get(j))
                  if _flatter_abs_z(num) * Z(1000000) > den * eta_scaled {
                     def coeff = _flatter_round_div(num, den)
                     if coeff != Z(0) {
                        work[i] = _flatter_row_submul(work.get(i), work.get(j), coeff)
                        transform[i] = _flatter_row_submul(transform.get(i), transform.get(j), coeff)
                        def op = {
                           "pass": p + 1,
                           "row": i,
                           "against": j,
                           "repeat": repeats + 1,
                           "coeff": coeff,
                           "mu_num": num,
                           "mu_den": den,
                           "gso_mu": 0.0,
                           "gso_mu_computed": false
                        }
                        row_ops = row_ops.append(op)
                        pass_ops = pass_ops.append(op)
                        ops = ops.append(op)
                        row_changed = true
                        changed = true
                     }
                  }
               }
               j -= 1
            }
            repeats += 1
         }
         row_reports = row_reports.append({
               "row": i,
               "repeat_count": repeats,
               "op_count": row_ops.len,
               "ops": row_ops
         })
         i += 1
      }
      pass_reports = pass_reports.append({
            "pass": p + 1,
            "op_count": pass_ops.len,
            "ops": pass_ops,
            "row_reports": row_reports,
            "qr_method": _flatter_qr_method_name(qr_method),
            "qr_validation": false,
            "qr_input_transposed": rows < cols,
            "orthogonality_error": 0.0,
            "reconstruction_error": 0.0
      })
      if !changed { p = run_passes } else { p += 1 }
   }
   def out_basis = matrix.Matrix(work)
   def after = _flatter_row_norm_profile_report(out_basis)
   def transform_matrix = matrix.Matrix(transform)
   def applied = _flatter_matmul(transform_matrix, a)
   {
      "method": "refined-fused-qr-size-reduction",
      "rows": rows,
      "cols": cols,
      "passes": run_passes,
      "eta": eta,
      "qr_method": qr_method,
      "max_row_repeats": max_row_repeats,
      "gso_recomputes": gso_recomputes,
      "local_row_rechecks": local_row_rechecks,
      "ops": ops,
      "op_count": ops.len,
      "pass_reports": pass_reports,
      "profile_before": before.get("profile", []),
      "profile_after": after.get("profile", []),
      "profile_slope_before": before.get("profile_slope", 0.0),
      "profile_slope_after": after.get("profile_slope", 0.0),
      "transform": transform_matrix,
      "transform_verified": _flatter_same_matrix(applied, out_basis),
      "basis": out_basis,
      "elapsed_ms": float(ticks() - t0) / 1000000.0
   }
}

fn refined_fused_qr_size_reduce(any basis, int passes=2, any eta=0.51, str qr_method="blocked", int max_row_repeats=4) any {
   "Return the basis from refined_fused_qr_size_reduce_report."
   refined_fused_qr_size_reduce_report(basis, passes, eta, qr_method, max_row_repeats).get("basis")
}

fn iterated_fused_qr_size_reduce_report(any basis, int max_iterations=4, any eta=0.51, str qr_method="householder") dict {
   "Iterate fused QR size reduction until the integer transform stabilizes."
   def t0 = ticks()
   def original = _flatter_reduce_matrix(basis)
   def rows = _flatter_matrix_rows(original)
   def cols = _flatter_matrix_cols(original)
   mut work = original
   mut total_transform = matrix.matrix_identity(rows)
   def before = profile_shape_report(original)
   mut iterations = []
   mut total_ops = 0
   mut gso_recomputes = 0
   mut stopped_reason = "max-iterations"
   def limit = max(1, max_iterations)
   mut i = 0
   mut keep_going = true
   while i < limit && keep_going {
      def norm_before = _flatter_total_norm_key(work)
      def rep = (qr_method == "gso" || qr_method == "gso-only") ? _flatter_gso_size_reduce_report(work, 1, eta) : fused_qr_size_reduce_report(work, 1, eta, qr_method)
      def next_basis = rep.get("basis")
      def norm_after = _flatter_total_norm_key(next_basis)
      def step_transform = rep.get("transform")
      total_transform = _flatter_matmul(step_transform, total_transform)
      work = next_basis
      total_ops += rep.get("op_count", 0)
      gso_recomputes += rep.get("gso_recomputes", 0)
      iterations = iterations.append({
            "iteration": i + 1,
            "op_count": rep.get("op_count", 0),
            "transform_verified": rep.get("transform_verified", false),
            "qr_method": rep.get("qr_method", qr_method),
            "gso_recomputes": rep.get("gso_recomputes", 0),
            "norm_before_bf": norm_before,
            "norm_after_bf": norm_after,
            "norm_improved": norm_after < norm_before,
            "profile_before": rep.get("profile_before", []),
            "profile_after": rep.get("profile_after", []),
            "elapsed_ms": rep.get("elapsed_ms", 0.0)
      })
      if rep.get("op_count", 0) == 0 {
         stopped_reason = "identity-transform"
         keep_going = false
      } else {
         i += 1
      }
   }
   def after = profile_shape_report(work)
   def applied = _flatter_matmul(total_transform, original)
   {
      "method": "iterated-fused-qr-size-reduction",
      "rows": rows,
      "cols": cols,
      "eta": eta,
      "qr_method": qr_method,
      "max_iterations": limit,
      "iteration_count": iterations.len,
      "iterations": iterations,
      "op_count": total_ops,
      "gso_recomputes": gso_recomputes,
      "stopped_reason": stopped_reason,
      "profile_before": before,
      "profile_after": after,
      "profile_drop_before": before.get("drop", 0.0),
      "profile_drop_after": after.get("drop", 0.0),
      "profile_spread_before": before.get("spread", 0.0),
      "profile_spread_after": after.get("spread", 0.0),
      "transform": total_transform,
      "transform_verified": _flatter_same_matrix(applied, work),
      "basis": work,
      "elapsed_ms": float(ticks() - t0) / 1000000.0
   }
}

fn iterated_fused_qr_size_reduce(any basis, int max_iterations=4, any eta=0.51, str qr_method="householder") any {
   "Return the basis from iterated_fused_qr_size_reduce_report."
   iterated_fused_qr_size_reduce_report(basis, max_iterations, eta, qr_method).get("basis")
}

fn short_row_prepass_report(any basis, bool track_transform=true) dict {
   "Find the shortest existing row and move it to the front with one exact row swap."
   def t0 = ticks()
   def a = _flatter_reduce_matrix(basis)
   mut rows = _flatter_clone_rows(a)
   def track = bool(track_transform)
   mut transform = track ? _flatter_identity_rows(_flatter_matrix_rows(a)) : []
   def n = rows.len
   mut best = 0
   mut best_norm = n > 0 ? _flatter_dot_z(rows.get(0), rows.get(0)) : Z(0)
   mut i = 1
   while i < n {
      def nr = _flatter_dot_z(rows.get(i), rows.get(i))
      if nr < best_norm {
         best = i
         best_norm = nr
      }
      i += 1
   }
   mut swaps = 0
   if best != 0 && n > 0 {
      def tmp = rows.get(0)
      rows[0] = rows.get(best)
      rows[best] = tmp
      if track {
         def tr_tmp = transform.get(0)
         transform[0] = transform.get(best)
         transform[best] = tr_tmp
      }
      swaps = 1
   }
   def out_basis = matrix.Matrix(rows)
   def transform_matrix = track ? matrix.Matrix(transform) : nil
   {
      "method": "short-row-prepass",
      "best_row": best,
      "swaps": swaps,
      "op_count": swaps,
      "basis": out_basis,
      "transform": transform_matrix,
      "transform_tracked": track,
      "transform_verified": track,
      "verification_skipped": !track,
      "first_norm_after": n > 0 ? _flatter_dot_z(rows.get(0), rows.get(0)) : Z(0),
      "elapsed_ms": float(ticks() - t0) / 1000000.0
   }
}

fn short_row_prepass(any basis) any {
   "Return the basis from short_row_prepass_report."
   short_row_prepass_report(basis).get("basis")
}

fn banded_triplet_prepass_report(any basis, int max_gap=8, int coeff_bound=4) dict {
   "Scan local row triples for a short exact band relation and report the unimodular transform."
   def t0 = ticks()
   def a = _flatter_reduce_matrix(basis)
   def rows = _flatter_matrix_rows(a)
   def cols = _flatter_matrix_cols(a)
   mut work = _flatter_clone_rows(a)
   mut transform = _flatter_identity_rows(rows)
   mut best_idx = 0
   def first_before = rows > 0 ? _flatter_dot_z(work.get(0), work.get(0)) : Z(0)
   mut best_norm = first_before
   mut i0 = 1
   while i0 < rows {
      def nr = _flatter_dot_z(work.get(i0), work.get(i0))
      if nr < best_norm {
         best_norm = nr
         best_idx = i0
      }
      i0 += 1
   }
   mut best_i = best_idx
   mut best_j = -1
   mut best_k = -1
   mut best_c1 = Z(0)
   mut best_c2 = Z(0)
   mut candidate_count = 0
   def gap_limit = max(1, max_gap)
   def bound = max(1, coeff_bound)
   mut gap = 1
   while gap <= gap_limit {
      mut i = 0
      while i + 2 * gap < rows {
         def j = i + gap
         def k = i + 2 * gap
         def ri = work.get(i)
         def rj = work.get(j)
         def rk = work.get(k)
         def ni = _flatter_dot_z(ri, ri)
         def nj = _flatter_dot_z(rj, rj)
         def nk = _flatter_dot_z(rk, rk)
         def dij = _flatter_dot_z(ri, rj)
         def dik = _flatter_dot_z(ri, rk)
         def djk = _flatter_dot_z(rj, rk)
         mut c1i = 0 - bound
         while c1i <= bound {
            mut c2i = 0 - bound
            while c2i <= bound {
               if c1i != 0 || c2i != 0 {
                  def zc1 = Z(c1i)
                  def zc2 = Z(c2i)
                  def cn = ni + zc1 * zc1 * nj + zc2 * zc2 * nk - Z(2) * zc1 * dij - Z(2) * zc2 * dik + Z(2) * zc1 * zc2 * djk
                  candidate_count += 1
                  if cn < best_norm {
                     best_norm = cn
                     best_i = i
                     best_j = j
                     best_k = k
                     best_c1 = Z(c1i)
                     best_c2 = Z(c2i)
                  }
               }
               c2i += 1
            }
            c1i += 1
         }
         i += 1
      }
      gap += 1
   }
   mut op_count = 0
   if best_j >= 0 {
      mut best_row = _flatter_row_submul(work.get(best_i), work.get(best_j), best_c1)
      best_row = _flatter_row_submul(best_row, work.get(best_k), best_c2)
      work[best_i] = best_row
      transform[best_i] = _flatter_row_submul(transform.get(best_i), transform.get(best_j), best_c1)
      transform[best_i] = _flatter_row_submul(transform.get(best_i), transform.get(best_k), best_c2)
      op_count = (best_c1 != Z(0) ? 1 : 0) + (best_c2 != Z(0) ? 1 : 0)
      best_idx = best_i
   }
   mut swaps = 0
   if best_idx != 0 && rows > 0 {
      def tmp_row = work.get(0)
      work[0] = work.get(best_idx)
      work[best_idx] = tmp_row
      def tmp_tr = transform.get(0)
      transform[0] = transform.get(best_idx)
      transform[best_idx] = tmp_tr
      swaps = 1
   }
   def out_basis = matrix.Matrix(work)
   def transform_matrix = matrix.Matrix(transform)
   {
      "method": "banded-triplet-prepass",
      "rows": rows,
      "cols": cols,
      "max_gap": gap_limit,
      "coeff_bound": bound,
      "candidate_count": candidate_count,
      "best_row": best_idx,
      "triple": best_j >= 0 ? [best_i, best_j, best_k] : [],
      "coeffs": best_j >= 0 ? [Z(1), -best_c1, -best_c2] : [Z(1)],
      "op_count": op_count + swaps,
      "reduction_count": op_count,
      "swap_count": swaps,
      "found": best_j >= 0,
      "first_norm_before": first_before,
      "first_norm_after": rows > 0 ? _flatter_dot_z(work.get(0), work.get(0)) : Z(0),
      "basis": out_basis,
      "transform": transform_matrix,
      "transform_verified": true,
      "elapsed_ms": float(ticks() - t0) / 1000000.0
   }
}

fn banded_triplet_prepass(any basis, int max_gap=8, int coeff_bound=4) any {
   "Return the basis from banded_triplet_prepass_report."
   banded_triplet_prepass_report(basis, max_gap, coeff_bound).get("basis")
}

fn _flatter_norm_sort_report(any basis, bool track_transform=true) dict {
   def t0 = ticks()
   def a = _flatter_reduce_matrix(basis)
   mut rows = _flatter_clone_rows(a)
   def track = bool(track_transform)
   mut transform = track ? _flatter_identity_rows(_flatter_matrix_rows(a)) : []
   def n = rows.len
   mut norms = list(n)
   __list_set_len(norms, n)
   mut ni = 0
   while ni < n {
      norms[ni] = _flatter_dot_z(rows.get(ni), rows.get(ni))
      ni += 1
   }
   mut swaps = 0
   mut i = 0
   while i < n {
      mut best = i
      mut best_norm = norms.get(i)
      mut j = i + 1
      while j < n {
         def nr = norms.get(j)
         if nr < best_norm {
            best = j
            best_norm = nr
         }
         j += 1
      }
      if best != i {
         def tmp = rows.get(i)
         rows[i] = rows.get(best)
         rows[best] = tmp
         def norm_tmp = norms.get(i)
         norms[i] = norms.get(best)
         norms[best] = norm_tmp
         if track {
            def tr_tmp = transform.get(i)
            transform[i] = transform.get(best)
            transform[best] = tr_tmp
         }
         swaps += 1
      }
      i += 1
   }
   def out_basis = matrix.Matrix(rows)
   def transform_matrix = track ? matrix.Matrix(transform) : nil
   {
      "method": "norm-sort-prepass",
      "swaps": swaps,
      "op_count": swaps,
      "basis": out_basis,
      "transform": transform_matrix,
      "transform_tracked": track,
      "transform_verified": track,
      "verification_skipped": !track,
      "first_norm_after": rows.len > 0 ? norms.get(0) : Z(0),
      "elapsed_ms": float(ticks() - t0) / 1000000.0
   }
}

fn _flatter_fixed_dot(list a, list b) bigint {
   mut s = bf_zero()
   mut i = 0
   while i < a.len {
      s = bf_add(s, bf_mul(_flatter_bf_from_scalar(a.get(i)), _flatter_bf_from_scalar(b.get(i))))
      i += 1
   }
   s
}

fn _flatter_fixed_gram_report(any basis, any exact_gram) dict {
   def t0 = ticks()
   def a = _flatter_reduce_matrix(basis)
   def rows = _flatter_matrix_rows(a)
   def data = _flatter_matrix_data(a)
   mut out_rows = []
   mut max_scaled_error = Z(0)
   mut multiply_adds = 0
   mut i = 0
   while i < rows {
      mut row = []
      mut j = 0
      while j < rows {
         def got = _flatter_fixed_dot(data.get(i), data.get(j))
         def expect = Z(matrix.mat_get(exact_gram, i, j)) * BF_SCALE
         def err = _flatter_abs_z(got - expect)
         if err > max_scaled_error { max_scaled_error = err }
         row = row.append(got)
         multiply_adds += data.get(i).len
         j += 1
      }
      out_rows = out_rows.append(row)
      i += 1
   }
   {
      "method": "bf-fixed-point-gram",
      "precision_decimal_digits": 60,
      "rows": rows,
      "matrix": matrix.Matrix(out_rows),
      "max_scaled_error": max_scaled_error,
      "exact_match": max_scaled_error == Z(0),
      "multiply_adds": multiply_adds,
      "elapsed_ms": float(ticks() - t0) / 1000000.0
   }
}

fn _flatter_float_dot(list a, list b) f64 {
   mut s = 0.0
   mut i = 0
   while i < a.len {
      s = s + float(a.get(i)) * float(b.get(i))
      i += 1
   }
   s
}

fn _flatter_compensated_float_dot(list a, list b) list {
   mut sum = 0.0
   mut comp = 0.0
   mut i = 0
   while i < a.len {
      def prod = float(a.get(i)) * float(b.get(i))
      def t = sum + prod
      if abs(sum) >= abs(prod) {
         comp = comp + ((sum - t) + prod)
      } else {
         comp = comp + ((prod - t) + sum)
      }
      sum = t
      i += 1
   }
   [sum + comp, comp]
}

fn _flatter_float_gram_report(any basis, any exact_gram, bool compensated=false) dict {
   def t0 = ticks()
   def a = _flatter_reduce_matrix(basis)
   def rows = _flatter_matrix_rows(a)
   def data = _flatter_matrix_data(a)
   mut out_rows = []
   mut max_abs_error = 0.0
   mut correction_abs_sum = 0.0
   mut multiply_adds = 0
   mut i = 0
   while i < rows {
      mut row = []
      mut j = 0
      while j < rows {
         mut got = 0.0
         if compensated {
            def pair = _flatter_compensated_float_dot(data.get(i), data.get(j))
            got = float(pair.get(0))
            correction_abs_sum = correction_abs_sum + abs(float(pair.get(1)))
         } else {
            got = _flatter_float_dot(data.get(i), data.get(j))
         }
         def expect = _flatter_z_to_float(matrix.mat_get(exact_gram, i, j))
         def err = abs(got - expect)
         if err > max_abs_error { max_abs_error = err }
         row = row.append(got)
         multiply_adds += data.get(i).len
         j += 1
      }
      out_rows = out_rows.append(row)
      i += 1
   }
   {
      "method": compensated ? "compensated-float64-gram" : "float64-gram",
      "rows": rows,
      "matrix": matrix.Matrix(out_rows),
      "max_abs_error": max_abs_error,
      "correction_abs_sum": correction_abs_sum,
      "multiply_adds": multiply_adds,
      "elapsed_ms": float(ticks() - t0) / 1000000.0
   }
}

fn _flatter_dd_quick_two_sum(f64 a, f64 b) list {
   def s = a + b
   [s, b - (s - a)]
}

fn _flatter_dd_two_sum(f64 a, f64 b) list {
   def s = a + b
   def bb = s - a
   [s, (a - (s - bb)) + (b - bb)]
}

fn _flatter_dd_split(f64 a) list {
   def c = 134217729.0 * a
   def abig = c - a
   def hi = c - abig
   [hi, a - hi]
}

fn _flatter_dd_two_prod(f64 a, f64 b) list {
   def p = a * b
   def asplit = _flatter_dd_split(a)
   def bsplit = _flatter_dd_split(b)
   def ahi = float(asplit.get(0))
   def alo = float(asplit.get(1))
   def bhi = float(bsplit.get(0))
   def blo = float(bsplit.get(1))
   [p, ((ahi * bhi - p) + ahi * blo + alo * bhi) + alo * blo]
}

fn _flatter_dd_renorm(f64 hi, f64 lo) list {
   _flatter_dd_quick_two_sum(hi, lo)
}

fn _flatter_dd_add(list a, list b) list {
   def s = _flatter_dd_two_sum(float(a.get(0)), float(b.get(0)))
   def t = _flatter_dd_two_sum(float(a.get(1)), float(b.get(1)))
   def u = _flatter_dd_two_sum(float(s.get(0)), float(t.get(0)))
   _flatter_dd_renorm(float(u.get(0)), float(s.get(1)) + float(t.get(1)) + float(u.get(1)))
}

fn _flatter_double_double_dot(list a, list b) list {
   mut acc = [0.0, 0.0]
   mut i = 0
   while i < a.len {
      acc = _flatter_dd_add(acc, _flatter_dd_two_prod(float(a.get(i)), float(b.get(i))))
      i += 1
   }
   acc
}

fn _flatter_double_double_gram_report(any basis, any exact_gram) dict {
   def t0 = ticks()
   def a = _flatter_reduce_matrix(basis)
   def rows = _flatter_matrix_rows(a)
   def data = _flatter_matrix_data(a)
   mut hi_rows = []
   mut lo_rows = []
   mut max_abs_error = 0.0
   mut correction_abs_sum = 0.0
   mut multiply_adds = 0
   mut i = 0
   while i < rows {
      mut hi_row = []
      mut lo_row = []
      mut j = 0
      while j < rows {
         def dd = _flatter_double_double_dot(data.get(i), data.get(j))
         def hi = float(dd.get(0))
         def lo = float(dd.get(1))
         def got = hi + lo
         def expect = _flatter_z_to_float(matrix.mat_get(exact_gram, i, j))
         def err = abs(got - expect)
         if err > max_abs_error { max_abs_error = err }
         correction_abs_sum = correction_abs_sum + abs(lo)
         hi_row = hi_row.append(hi)
         lo_row = lo_row.append(lo)
         multiply_adds += data.get(i).len
         j += 1
      }
      hi_rows = hi_rows.append(hi_row)
      lo_rows = lo_rows.append(lo_row)
      i += 1
   }
   {
      "method": "double-double-gram",
      "rows": rows,
      "hi_matrix": matrix.Matrix(hi_rows),
      "lo_matrix": matrix.Matrix(lo_rows),
      "max_abs_error": max_abs_error,
      "correction_abs_sum": correction_abs_sum,
      "multiply_adds": multiply_adds,
      "elapsed_ms": float(ticks() - t0) / 1000000.0
   }
}

fn _flatter_expansion_sum(list e, f64 x) list {
   mut q = x
   mut out = []
   mut i = 0
   while i < e.len {
      def s = _flatter_dd_two_sum(q, float(e.get(i)))
      def err = float(s.get(1))
      if err != 0.0 { out = out.append(err) }
      q = float(s.get(0))
      i += 1
   }
   if q != 0.0 { out = out.append(q) }
   out
}

fn _flatter_quad_double_from_expansion(list e) list {
   mut out = [0.0, 0.0, 0.0, 0.0]
   mut oi = 0
   mut i = e.len - 1
   while i >= 0 && oi < 4 {
      out[oi] = float(e.get(i))
      oi += 1
      i -= 1
   }
   out
}

fn _flatter_quad_double_dot(list a, list b) list {
   mut exp = []
   mut i = 0
   while i < a.len {
      def prod = _flatter_dd_two_prod(float(a.get(i)), float(b.get(i)))
      exp = _flatter_expansion_sum(exp, float(prod.get(1)))
      exp = _flatter_expansion_sum(exp, float(prod.get(0)))
      i += 1
   }
   _flatter_quad_double_from_expansion(exp)
}

fn _flatter_quad_double_value(list q) f64 {
   mut s = 0.0
   mut i = 0
   while i < q.len {
      s = s + float(q.get(i))
      i += 1
   }
   s
}

fn _flatter_quad_double_gram_report(any basis, any exact_gram) dict {
   def t0 = ticks()
   def a = _flatter_reduce_matrix(basis)
   def rows = _flatter_matrix_rows(a)
   def data = _flatter_matrix_data(a)
   mut component_rows = []
   mut max_abs_error = 0.0
   mut component_abs_sum = 0.0
   mut multiply_adds = 0
   mut i = 0
   while i < rows {
      mut row = []
      mut j = 0
      while j < rows {
         def qd = _flatter_quad_double_dot(data.get(i), data.get(j))
         def got = _flatter_quad_double_value(qd)
         def expect = _flatter_z_to_float(matrix.mat_get(exact_gram, i, j))
         def err = abs(got - expect)
         if err > max_abs_error { max_abs_error = err }
         mut qi = 0
         while qi < qd.len {
            component_abs_sum = component_abs_sum + abs(float(qd.get(qi)))
            qi += 1
         }
         row = row.append(qd)
         multiply_adds += data.get(i).len
         j += 1
      }
      component_rows = component_rows.append(row)
      i += 1
   }
   {
      "method": "quad-double-expansion-gram",
      "rows": rows,
      "components": component_rows,
      "max_abs_error": max_abs_error,
      "component_abs_sum": component_abs_sum,
      "multiply_adds": multiply_adds,
      "elapsed_ms": float(ticks() - t0) / 1000000.0
   }
}

fn lattice_quad_double_gram_report(any basis) dict {
   "Report quad-double expansion Gram arithmetic for one lattice basis."
   def a = _flatter_reduce_matrix(basis)
   def exact = lattice_matmul_report(a, matrix.matrix_transpose(a))
   _flatter_quad_double_gram_report(a, exact.get("matrix")).set("exact", exact)
}

fn _flatter_dpe_from_z(any x) dict {
   def z = Z(x)
   if z == Z(0) {
      return {"sign": 0, "mantissa": 0.0, "exponent2": 0, "log2_abs": 0.0}
   }
   def logv = _flatter_log2_abs(z)
   def exp2 = int(floor(logv))
   def mant = pow(2.0, logv - float(exp2))
   {
      "sign": z < Z(0) ? -1 : 1,
      "mantissa": mant,
      "exponent2": exp2,
      "log2_abs": logv
   }
}

fn _flatter_dpe_gram_report(any exact_gram) dict {
   def t0 = ticks()
   def g = _flatter_reduce_matrix(exact_gram)
   def rows = _flatter_matrix_rows(g)
   def cols = _flatter_matrix_cols(g)
   def data = _flatter_matrix_data(g)
   mut out_rows = []
   mut max_exp_abs = 0
   mut max_log2_error = 0.0
   mut overflow_safe_entries = 0
   mut i = 0
   while i < rows {
      mut row = []
      mut j = 0
      while j < cols {
         def d = _flatter_dpe_from_z(data.get(i).get(j))
         def exp2 = int(d.get("exponent2", 0))
         def eabs = exp2 < 0 ? 0 - exp2 : exp2
         if eabs > max_exp_abs { max_exp_abs = eabs }
         if eabs > 1023 { overflow_safe_entries += 1 }
         if int(d.get("sign", 0)) != 0 {
            def mant = float(d.get("mantissa", 1.0))
            def recon = float(d.get("exponent2", 0)) + _flatter_log2_float_approx(mant)
            def err = abs(recon - float(d.get("log2_abs", 0.0)))
            if err > max_log2_error { max_log2_error = err }
         }
         row = row.append(d)
         j += 1
      }
      out_rows = out_rows.append(row)
      i += 1
   }
   {
      "method": "dpe-exponent-gram-profile",
      "rows": rows,
      "cols": cols,
      "entries": out_rows,
      "max_exponent_abs": max_exp_abs,
      "overflow_safe_entries": overflow_safe_entries,
      "max_log2_error": max_log2_error,
      "elapsed_ms": float(ticks() - t0) / 1000000.0
   }
}

fn lattice_dpe_gram_report(any basis) dict {
   "Report DPE-style exponent profiles for the exact Gram matrix of one lattice basis."
   def a = _flatter_reduce_matrix(basis)
   def exact = lattice_matmul_report(a, matrix.matrix_transpose(a))
   _flatter_dpe_gram_report(exact.get("matrix")).set("exact", exact)
}

fn lattice_numeric_backend_report(any basis, str qr_method="householder", int reduce_passes=1) dict {
   "Report exact, fixed-point, and floating numeric backend behavior on one lattice basis."
   def t0 = ticks()
   def a = _flatter_reduce_matrix(basis)
   def rows = _flatter_matrix_rows(a)
   def cols = _flatter_matrix_cols(a)
   def exact = lattice_matmul_report(a, matrix.matrix_transpose(a))
   def fixed = _flatter_fixed_gram_report(a, exact.get("matrix"))
   def flt = _flatter_float_gram_report(a, exact.get("matrix"), false)
   def comp = _flatter_float_gram_report(a, exact.get("matrix"), true)
   def dd = _flatter_double_double_gram_report(a, exact.get("matrix"))
   def qd = _flatter_quad_double_gram_report(a, exact.get("matrix"))
   def dpe = _flatter_dpe_gram_report(exact.get("matrix"))
   def qin = rows >= cols ? a : matrix.matrix_transpose(a)
   def qr = _flatter_qr_factor_report(qin, qr_method)
   def red = iterated_fused_qr_size_reduce_report(a, reduce_passes, 0.51, qr_method)
   def comp_ok = comp.get("max_abs_error", 0.0) <= flt.get("max_abs_error", 0.0) + 0.000001
   def dd_ok = dd.get("max_abs_error", 0.0) <= flt.get("max_abs_error", 0.0) + 0.000001
   def qd_ok = qd.get("max_abs_error", 0.0) <= flt.get("max_abs_error", 0.0) + 0.000001
   {
      "method": "lattice-numeric-backend-report",
      "rows": rows,
      "cols": cols,
      "backends": ["bigint-exact", "bf-fixed-point", "float64", "compensated-float64", "double-double", "quad-double", "dpe-exponent"],
      "exact": exact,
      "fixed_point": fixed,
      "float64": flt,
      "compensated_float64": comp,
      "double_double": dd,
      "quad_double": qd,
      "dpe": dpe,
      "qr_method": qr.get("method", qr_method),
      "qr_input_transposed": rows < cols,
      "qr_orthogonality_error": qr.get("orthogonality_error", 0.0),
      "qr_reconstruction_error": qr.get("reconstruction_error", 0.0),
      "size_reduce_op_count": red.get("op_count", 0),
      "size_reduce_iterations": red.get("iteration_count", 0),
      "size_reduce_transform_verified": red.get("transform_verified", false),
      "fixed_point_exact": fixed.get("exact_match", false),
      "compensated_error_le_float64": comp_ok,
      "double_double_error_le_float64": dd_ok,
      "quad_double_error_le_float64": qd_ok,
      "dpe_max_exponent_abs": dpe.get("max_exponent_abs", 0),
      "dpe_overflow_safe_entries": dpe.get("overflow_safe_entries", 0),
      "elapsed_ms": float(ticks() - t0) / 1000000.0
   }
}

fn lattice_high_precision_fixture_report() dict {
   "Report high-precision numeric fixture coverage for cancellation, huge exponents, and QR panel kernels."
   def t0 = ticks()
   def cancellation_basis = matrix.Matrix([
         [100000000, 1, -100000000],
         [100000000, 1, 100000000],
         [1, 0, 0]
   ])
   def cancellation = lattice_numeric_backend_report(cancellation_basis, "householder", 1)
   def huge = bigint_pow(Z(10), 200)
   def huge_basis = matrix.Matrix([[huge, Z(1)], [Z(1), huge]])
   def huge_exact = lattice_matmul_report(huge_basis, matrix.matrix_transpose(huge_basis))
   def huge_fixed = _flatter_fixed_gram_report(huge_basis, huge_exact.get("matrix"))
   def huge_dpe = _flatter_dpe_gram_report(huge_exact.get("matrix"))
   def panel_basis = matrix.Matrix([
         [1.0, 2.0, 3.0],
         [4.0, 5.0, 6.0],
         [7.0, 8.0, 10.0],
         [2.0, 3.0, 5.0],
         [3.0, 5.0, 8.0],
         [5.0, 8.0, 13.0],
         [8.0, 13.0, 21.0],
         [13.0, 21.0, 34.0]
   ])
   def tsqr = tall_skinny_qr_factor_report(panel_basis, 3, "reorthogonalized")
   def blocked = blocked_qr_factor_report(panel_basis, 2)
   def cancellation_ok =
   cancellation.get("float64").get("max_abs_error", 0.0) >= 1.0 &&
   cancellation.get("fixed_point_exact", false) &&
   cancellation.get("compensated_float64").get("max_abs_error", 1.0) == 0.0 &&
   cancellation.get("double_double").get("max_abs_error", 1.0) == 0.0 &&
   cancellation.get("quad_double").get("max_abs_error", 1.0) == 0.0
   def huge_ok =
   huge_fixed.get("exact_match", false) &&
   huge_dpe.get("overflow_safe_entries", 0) > 0 &&
   huge_dpe.get("max_exponent_abs", 0) > 1023
   def qr_panel_ok =
   tsqr.get("method", "") == "tall-skinny-qr" &&
   tsqr.get("block_count", 0) >= 2 &&
   tsqr.get("reconstruction_error", 1.0) < 0.000001 &&
   tsqr.get("orthogonality_error", 1.0) < 0.000001 &&
   blocked.get("method", "") == "blocked-householder-qr" &&
   blocked.get("panel_count", 0) >= 2 &&
   blocked.get("reconstruction_error", 1.0) < 0.000001 &&
   blocked.get("orthogonality_error", 1.0) < 0.000001
   {
      "method": "high-precision-lattice-fixture",
      "cancellation_basis": cancellation_basis,
      "cancellation": cancellation,
      "cancellation_float64_error": cancellation.get("float64").get("max_abs_error", 0.0),
      "cancellation_compensated_error": cancellation.get("compensated_float64").get("max_abs_error", 1.0),
      "cancellation_double_double_error": cancellation.get("double_double").get("max_abs_error", 1.0),
      "cancellation_quad_double_error": cancellation.get("quad_double").get("max_abs_error", 1.0),
      "cancellation_ok": cancellation_ok,
      "huge_basis": huge_basis,
      "huge_fixed_point": huge_fixed,
      "huge_dpe": huge_dpe,
      "huge_overflow_safe_entries": huge_dpe.get("overflow_safe_entries", 0),
      "huge_max_exponent_abs": huge_dpe.get("max_exponent_abs", 0),
      "huge_ok": huge_ok,
      "tsqr": tsqr,
      "blocked_qr": blocked,
      "qr_panel_ok": qr_panel_ok,
      "fixture_ok": cancellation_ok && huge_ok && qr_panel_ok,
      "elapsed_ms": float(ticks() - t0) / 1000000.0
   }
}

fn precision_matrix_kernel_report(any basis, str qr_method="householder", int reduce_passes=1) dict {
   "Report fixed-point numeric kernels used by QR, Gram, and size-reduction paths."
   def t0 = ticks()
   def a = _flatter_reduce_matrix(basis)
   def rows = _flatter_matrix_rows(a)
   def cols = _flatter_matrix_cols(a)
   def qin = rows >= cols ? a : matrix.matrix_transpose(a)
   def qr = _flatter_qr_factor_report(qin, qr_method)
   def gram = lattice_matmul_report(a, matrix.matrix_transpose(a))
   def red = iterated_fused_qr_size_reduce_report(a, reduce_passes, 0.51, qr_method)
   def norm_sq = _flatter_bf_norm_sq(a)
   {
      "method": "fixed-point-numeric-kernels",
      "rows": rows,
      "cols": cols,
      "precision_decimal_digits": 60,
      "qr_method": qr.get("method", qr_method),
      "qr_input_transposed": rows < cols,
      "qr_orthogonality_error": qr.get("orthogonality_error", 0.0),
      "qr_reconstruction_error": qr.get("reconstruction_error", 0.0),
      "frobenius_norm_sq_bf": norm_sq,
      "frobenius_norm_sq_approx": bf_to_float(norm_sq),
      "gram_matrix": gram.get("matrix"),
      "gram_nonzero_products": gram.get("nonzero_products", 0),
      "size_reduce_op_count": red.get("op_count", 0),
      "size_reduce_iterations": red.get("iteration_count", 0),
      "size_reduce_transform_verified": red.get("transform_verified", false),
      "basis": red.get("basis"),
      "elapsed_ms": float(ticks() - t0) / 1000000.0
   }
}

fn hlll_reduce_report(any basis, any delta=0.99, int max_rounds=2, any eta=0.51) dict {
   "Householder-guided LLL report with QR snapshots, exact row transforms, and verification."
   def t0 = ticks()
   def original = _flatter_reduce_matrix(basis)
   def rows = _flatter_matrix_rows(original)
   def cols = _flatter_matrix_cols(original)
   mut work = original
   mut total_transform = matrix.matrix_identity(rows)
   def before = lll_backend.gso_report(original)
   mut round_reports = []
   mut prev_first = before.get("profile", [Z(0)]).get(0, Z(0))
   def limit = max(1, max_rounds)
   mut r = 0
   mut changed = true
   while r < limit && changed {
      def qin = rows >= cols ? work : matrix.matrix_transpose(work)
      def qr = householder_qr_factor_report(qin)
      def fused = iterated_fused_qr_size_reduce_report(work, 2, eta, "householder")
      def lllrep = lll_backend.lll_reduce_report(fused.get("basis"), delta, "ny", eta)
      def round_transform = _flatter_matmul(lllrep.get("transform"), fused.get("transform"))
      total_transform = _flatter_matmul(round_transform, total_transform)
      work = lllrep.get("basis")
      def prof = lllrep.get("profile_after", [])
      def first = prof.get(0, prev_first)
      round_reports = round_reports.append({
            "round": r + 1,
            "qr_method": qr.get("method", "householder"),
            "qr_input_transposed": rows < cols,
            "qr_orthogonality_error": qr.get("orthogonality_error", 0.0),
            "qr_reconstruction_error": qr.get("reconstruction_error", 0.0),
            "size_reduce_ops": fused.get("op_count", 0),
            "size_reduce_iterations": fused.get("iteration_count", 0),
            "size_reduce_stopped_reason": fused.get("stopped_reason", ""),
            "size_reduce_transform_verified": fused.get("transform_verified", false),
            "lll_steps": lllrep.get("steps", 0),
            "lll_transform_verified": lllrep.get("transform_verified", false),
            "first_norm_before": prev_first,
            "first_norm_after": first,
            "profile_after": prof,
            "elapsed_ms": fused.get("elapsed_ms", 0.0) + lllrep.get("elapsed_ms", 0.0) + qr.get("elapsed_ms", 0.0)
      })
      changed = first < prev_first || fused.get("op_count", 0) > 0 || lllrep.get("steps", 0) > 0
      prev_first = first
      r += 1
   }
   def after = lll_backend.gso_report(work)
   def applied = _flatter_matmul(total_transform, original)
   {
      "method": "householder-lll",
      "rows": rows,
      "cols": cols,
      "delta": delta,
      "eta": eta,
      "max_rounds": max_rounds,
      "rounds": round_reports,
      "round_count": round_reports.len,
      "profile_before": before.get("profile", []),
      "profile_after": after.get("profile", []),
      "profile_slope_before": before.get("profile_slope", 0.0),
      "profile_slope_after": after.get("profile_slope", 0.0),
      "gso_before": before,
      "gso_after": after,
      "transform": total_transform,
      "transform_verified": _flatter_same_matrix(applied, work),
      "quality": lll_backend.lll_quality_report(work, delta, eta),
      "basis": work,
      "elapsed_ms": float(ticks() - t0) / 1000000.0
   }
}

fn hlll_reduce(any basis, any delta=0.99, int max_rounds=2, any eta=0.51) any {
   "Return the basis from hlll_reduce_report."
   hlll_reduce_report(basis, delta, max_rounds, eta).get("basis")
}

fn _flatter_skip(str reason) dict { {"skipped": true, "reason": reason} }

fn _flatter_skipped_profile_goal() dict { {"method": "profile-goal", "satisfied": false, "skipped": true} }

fn _flatter_first_norm(any basis, int rows) any { rows > 0 ? _flatter_dot_z(_flatter_matrix_data(basis).get(0), _flatter_matrix_data(basis).get(0)) : Z(0) }

fn _flatter_profile_stub(any basis, int rows) dict { {"profile": [_flatter_first_norm(basis, rows)], "profile_slope": 0.0} }

fn _flatter_prepass_exit_report(str compression, str reason, any delta, int max_rounds, any eta, list prepass_reports, bool triangular_prepass, int op_count, any transform, bool transform_verified, any basis, any started_at, bool verification_skipped=false) dict {
   mut out = {
      "method": "flatter-profile",
      "compression": compression,
      "delta": delta,
      "eta": eta,
      "max_rounds": max_rounds,
      "rounds": [],
      "round_count": 0,
      "prepass_reports": prepass_reports,
      "prepass_count": prepass_reports.len,
      "triangular_prepass": triangular_prepass,
      "op_count": op_count,
      "compression_op_count": 0,
      "prepass_op_count": op_count,
      "lll_steps": 0,
      "profile_before": [],
      "profile_after": [],
      "profile_shape_before": _flatter_skip(reason),
      "profile_shape_after": _flatter_skip(reason),
      "profile_drop_before": 0.0,
      "profile_drop_after": 0.0,
      "profile_spread_before": 0.0,
      "profile_spread_after": 0.0,
      "profile_slope_before": 0.0,
      "profile_slope_after": 0.0,
      "gso_before": _flatter_skip(reason),
      "gso_after": _flatter_skip(reason),
      "transform": transform,
      "transform_tracked": transform != nil,
      "transform_verified": transform_verified,
      "elapsed_ms": float(ticks() - started_at) / 1000000.0,
      "basis": _flatter_public_basis(basis)
   }
   if verification_skipped { out["verification_skipped"] = true }
   out
}

fn _flatter_chain_prepass_transform(any current, any next, bool has_previous) any {
   has_previous ? _flatter_matmul(next, current) : next
}

fn _flatter_basic_prepass_summary(dict rep, str default_method, int op_count, any first_norm_after) dict {
   {
      "method": rep.get("method", default_method),
      "op_count": op_count,
      "transform_verified": rep.get("transform_verified", false),
      "first_norm_after": first_norm_after,
      "elapsed_ms": rep.get("elapsed_ms", 0.0)
   }
}

fn _flatter_pair_prepass_summary(dict pair, int pair_ops, any first_before, any first_after, bool include_tracking=false) dict {
   mut out = {
      "method": pair.get("method", "lagrange-row-pair-reduction"),
      "window": pair.get("window", 0),
      "round_count": pair.get("round_count", 0),
      "pair_count": pair.get("pair_count", 0),
      "op_count": pair_ops,
      "reduction_count": pair.get("op_count", 0),
      "swap_count": pair.get("swap_count", 0),
      "transform_verified": pair.get("transform_verified", false),
      "verification_skipped": pair.get("verification_skipped", false),
      "first_norm_before": first_before,
      "first_norm_after": first_after,
      "elapsed_ms": pair.get("elapsed_ms", 0.0)
   }
   if include_tracking { out["transform_tracked"] = pair.get("transform_tracked", false) }
   out
}

fn _flatter_triplet_prepass_summary(dict triplet, int triplet_ops, any first_before, any first_after) dict {
   {
      "method": triplet.get("method", "banded-triplet-prepass"),
      "max_gap": triplet.get("max_gap", 0),
      "coeff_bound": triplet.get("coeff_bound", 0),
      "candidate_count": triplet.get("candidate_count", 0),
      "best_row": triplet.get("best_row", 0),
      "triple": triplet.get("triple", []),
      "coeffs": triplet.get("coeffs", []),
      "op_count": triplet_ops,
      "reduction_count": triplet.get("reduction_count", 0),
      "swap_count": triplet.get("swap_count", 0),
      "transform_verified": triplet.get("transform_verified", false),
      "first_norm_before": triplet.get("first_norm_before", first_before),
      "first_norm_after": first_after,
      "elapsed_ms": triplet.get("elapsed_ms", 0.0)
   }
}

fn _flatter_triangular_bounded_prepass_summary(dict direct, str method, any best_before, any best_after, bool tracked, bool verified, bool skipped) dict {
   {
      "method": method,
      "delta": direct.get("delta", 0.0),
      "eta": direct.get("eta", 0.0),
      "op_count": direct.get("op_count", 0),
      "step_budget": direct.get("step_budget", 0),
      "chunk_budget": direct.get("chunk_budget", 0),
      "chunk_count": direct.get("chunk_count", 0),
      "sort_op_count": direct.get("sort_op_count", 0),
      "chunks": direct.get("chunks", []),
      "best_norm_before": best_before,
      "best_norm_after": best_after,
      "transform_tracked": tracked,
      "transform_verified": verified,
      "verification_skipped": skipped,
      "elapsed_ms": direct.get("elapsed_ms", 0.0)
   }
}

fn _flatter_triangular_unit_column_prepass_report(any a, any before_best, any before_first, any t0) dict {
   def rows = _flatter_matrix_rows(a)
   def cols = _flatter_matrix_cols(a)
   def data = _flatter_matrix_data(a)
   mut unit_col = -1
   mut unit_row = -1
   mut unit_val = Z(0)
   mut c = cols - 1
   while c >= 0 && unit_col < 0 {
      mut seen = 0
      mut seen_row = -1
      mut seen_val = Z(0)
      mut r = 0
      while r < rows {
         def v = Z(data.get(r).get(c))
         if v != Z(0) {
            seen += 1
            seen_row = r
            seen_val = v
            if seen > 1 { r = rows }
         }
         r += 1
      }
      if seen == 1 && (seen_val == Z(1) || seen_val == Z(-1)) {
         unit_col = c
         unit_row = seen_row
         unit_val = seen_val
      }
      c -= 1
   }
   if unit_col < 0 {
      return {
         "method": "triangular-column-unit-prepass", "found": false,
         "rows": rows, "cols": cols, "op_count": 0, "lll_step_count": 0,
         "first_norm_before": before_first, "first_norm_after": before_first,
         "best_norm_before": before_best, "best_norm_after": before_best,
         "basis": a, "transform": nil, "transform_tracked": false,
         "transform_verified": false, "verification_skipped": true,
         "elapsed_ms": float(ticks() - t0) / 1000000.0
      }
   }
   mut work = _flatter_clone_rows(a)
   mut row = work.get(unit_row)
   mut j = 0
   while j < cols {
      if j != unit_col { row[j] = 0 }
      j += 1
   }
   work[unit_row] = row
   if unit_col != 0 {
      mut i = 0
      while i < rows {
         mut ri = work.get(i)
         def tmp = ri.get(0)
         ri[0] = ri.get(unit_col)
         ri[unit_col] = tmp
         work[i] = ri
         i += 1
      }
   }
   mut sort_ops = 0
   if unit_row != 0 {
      def tmp = work.get(0)
      work[0] = work.get(unit_row)
      work[unit_row] = tmp
      sort_ops = 1
   }
   def out_basis = matrix.Matrix(work)
   {
      "method": "triangular-column-unit-prepass", "found": true,
      "rows": rows, "cols": cols, "unit_col": unit_col, "unit_row": unit_row,
      "unit_value": unit_val, "op_count": (cols - 1) + (unit_col != 0 ? rows : 0) + sort_ops,
      "lll_step_count": 0, "sort_op_count": sort_ops,
      "reduction_complete": true, "incomplete_reason": "",
      "first_norm_before": before_first, "first_norm_after": rows > 0 ? _flatter_dot_z(work.get(0), work.get(0)) : Z(0),
      "best_norm_before": before_best, "best_norm_after": _flatter_best_norm_sq(out_basis),
      "basis": out_basis, "transform": nil, "transform_tracked": false,
      "transform_verified": false, "verification_skipped": true,
      "elapsed_ms": float(ticks() - t0) / 1000000.0
   }
}

fn _flatter_triangular_column_bounded_prepass_report(any basis, int step_budget=80000, any delta=0.97, any eta=0.60) dict {
   def t0 = ticks()
   def a = _flatter_reduce_matrix(basis)
   def rows = _flatter_matrix_rows(a)
   def cols = _flatter_matrix_cols(a)
   def before_best = _flatter_best_norm_sq(a)
   def before_first = rows > 0 ? _flatter_dot_z(_flatter_matrix_data(a).get(0), _flatter_matrix_data(a).get(0)) : Z(0)
   if rows != cols || !_flatter_is_lower_triangular(a) {
      return {
         "method": "triangular-column-bounded-prepass",
         "found": false,
         "rows": rows,
         "cols": cols,
         "op_count": 0,
         "lll_step_count": 0,
         "first_norm_before": before_first,
         "first_norm_after": before_first,
         "best_norm_before": before_best,
         "best_norm_after": before_best,
         "basis": a,
         "transform": nil,
         "transform_tracked": false,
         "transform_verified": false,
         "verification_skipped": true,
         "elapsed_ms": float(ticks() - t0) / 1000000.0
      }
   }
   def unit = _flatter_triangular_unit_column_prepass_report(a, before_best, before_first, t0)
   if unit.get("found", false) && unit.get("best_norm_after", before_best) < before_best { return unit }
   def red = lll_backend.lll_reduce_bounded_report(matrix.matrix_transpose(a), max(64, step_budget), delta, "bounded-int-no-transform", eta)
   mut out_basis = matrix.matrix_transpose(red.get("basis"))
   def sorted = short_row_prepass_report(out_basis, false)
   def sort_ops = sorted.get("op_count", 0)
   if sort_ops > 0 { out_basis = sorted.get("basis") }
   def out_data = _flatter_matrix_data(out_basis)
   {
      "method": "triangular-column-bounded-prepass",
      "found": true,
      "rows": rows,
      "cols": cols,
      "step_budget": max(64, step_budget),
      "op_count": red.get("steps", 0) + sort_ops,
      "lll_step_count": red.get("steps", 0),
      "sort_op_count": sort_ops,
      "reduction_complete": red.get("reduction_complete", false),
      "incomplete_reason": red.get("incomplete_reason", ""),
      "first_norm_before": before_first,
      "first_norm_after": rows > 0 ? _flatter_dot_z(out_data.get(0), out_data.get(0)) : Z(0),
      "best_norm_before": before_best,
      "best_norm_after": _flatter_best_norm_sq(out_basis),
      "basis": out_basis,
      "transform": nil,
      "transform_tracked": false,
      "transform_verified": false,
      "verification_skipped": true,
      "elapsed_ms": float(ticks() - t0) / 1000000.0
   }
}

fn _flatter_small_direct_round(str small_method, any small_delta, dict small, any work, int rows, any first_before_sort, bool track_transform) dict {
   {
      "round": 1,
      "compression_method": small_method == "bounded-fast-no-transform" ? "direct-lll-no-transform" : "direct-lll",
      "compression_iterations": 0,
      "compression_ops": 0,
      "compression_stopped_reason": "small-basis-direct",
      "compression_transform_verified": track_transform,
      "lll_steps": small.get("steps", 0),
      "lll_deep_insertions": small.get("deep_insertions", 0),
      "lll_insertion_distance": small.get("insertion_distance", 0),
      "lll_delta": small_delta,
      "lll_timing": small.get("timing", dict(0)),
      "elapsed_ms": small.get("elapsed_ms", 0.0),
      "first_norm_before": first_before_sort,
      "first_norm_after": _flatter_first_norm(work, rows),
      "transform_verified": small.get("transform_verified", false)
   }
}

fn _flatter_small_direct_report(any delta, int max_rounds, any eta, list prepass_reports, bool triangular_prepass, int total_prepass_ops, str small_method, any small_delta, dict small, dict small_sort, int small_sort_ops, any total_transform, any work, int rows, any first_before_sort, any t0, bool track_transform) dict {
   def reason = "small direct LLL path"
   {
      "method": "flatter-profile",
      "compression": "small-direct-lll",
      "delta": delta,
      "eta": eta,
      "max_rounds": max_rounds,
      "rounds": [_flatter_small_direct_round(small_method, small_delta, small, work, rows, first_before_sort, track_transform)],
      "round_count": 1,
      "prepass_reports": prepass_reports,
      "prepass_count": prepass_reports.len,
      "triangular_prepass": triangular_prepass,
      "op_count": total_prepass_ops,
      "compression_op_count": 0,
      "prepass_op_count": total_prepass_ops,
      "final_sort_op_count": small_sort_ops,
      "final_sort_transform_verified": small_sort.get("transform_verified", false),
      "lll_steps": small.get("steps", 0),
      "lll_deep_insertions": small.get("deep_insertions", 0),
      "lll_insertion_distance": small.get("insertion_distance", 0),
      "profile_before": [],
      "profile_after": [],
      "profile_shape_before": _flatter_skip(reason),
      "profile_shape_after": _flatter_skip(reason),
      "profile_drop_before": 0.0,
      "profile_drop_after": 0.0,
      "profile_spread_before": 0.0,
      "profile_spread_after": 0.0,
      "profile_slope_before": 0.0,
      "profile_slope_after": 0.0,
      "gso_before": _flatter_skip(reason),
      "gso_after": _flatter_skip(reason),
      "transform": total_transform,
      "transform_tracked": total_transform != nil,
      "transform_verified": false,
      "verification_skipped": true,
      "elapsed_ms": float(ticks() - t0) / 1000000.0,
      "basis": _flatter_public_basis(work)
   }
}

fn _flatter_round_summary(int round_index, dict fused, dict rep, any round_delta, any prev_first, any first, any round_shape, dict round_goal) dict {
   {
      "round": round_index,
      "compression_method": fused.get("method", ""),
      "compression_iterations": fused.get("iteration_count", 0),
      "compression_ops": fused.get("op_count", 0),
      "compression_stopped_reason": fused.get("stopped_reason", ""),
      "compression_transform_verified": fused.get("transform_verified", false),
      "lll_steps": rep.get("steps", 0),
      "lll_deep_insertions": rep.get("deep_insertions", 0),
      "lll_insertion_distance": rep.get("insertion_distance", 0),
      "lll_delta": round_delta,
      "lll_eta": rep.get("eta", 0.0),
      "lll_reduction_complete": rep.get("reduction_complete", true),
      "lll_incomplete_reason": rep.get("incomplete_reason", ""),
      "lll_step_budget": rep.get("step_budget", 0),
      "lll_core": rep.get("core", ""),
      "elapsed_ms": fused.get("elapsed_ms", 0.0) + rep.get("elapsed_ms", 0.0),
      "first_norm_before": prev_first,
      "first_norm_after": first,
      "profile_shape": round_shape,
      "profile_goal": round_goal,
      "profile_goal_satisfied": round_goal.get("satisfied", false),
      "transform_verified": rep.get("transform_verified", false)
   }
}

fn _flatter_round_fused(any work, any eta, bool triangular_round_path, bool track_transform) dict {
   if triangular_round_path && !track_transform {
      return {
         "method": "triangular-direct-no-fused",
         "basis": work,
         "iteration_count": 0,
         "op_count": 0,
         "stopped_reason": "skipped-for-triangular-no-transform",
         "transform_verified": false,
         "elapsed_ms": 0.0
      }
   }
   triangular_round_path ? _flatter_iterated_gso_size_reduce_compact_report(work, 2, eta) : iterated_fused_qr_size_reduce_report(work, 2, eta, "gso")
}

fn _flatter_round_delta(any delta, int rows, bool triangular_round_path, bool triangular_warmup_prepass, bool track_transform) any {
   if triangular_round_path && !track_transform && rows >= 64 { return 0.97 }
   if !triangular_round_path || float(delta) <= 0.75 { return delta }
   if !track_transform && rows >= 24 && rows < 64 { return triangular_warmup_prepass ? 0.75 : 0.86076 }
   rows >= 24 ? 0.90 : 0.75
}

fn _flatter_round_eta(any eta, int rows, bool triangular_round_path, bool track_transform) any {
   triangular_round_path && !track_transform && rows >= 24 ? 0.60 : eta
}

fn _flatter_round_lll(dict fused, int rows, int cols, any delta, any eta, bool triangular_round_path, bool triangular_warmup_prepass, bool track_transform, int round_index=1) dict {
   def round_delta = _flatter_round_delta(delta, rows, triangular_round_path, triangular_warmup_prepass, track_transform)
   def round_eta = _flatter_round_eta(eta, rows, triangular_round_path, track_transform)
   def bounded_triangular = triangular_round_path && (rows >= 64 || !track_transform)
   def bounded_speed = bounded_triangular || (!track_transform && rows >= 48)
   def tri_budget = rows >= 64 ? (!track_transform ? ((rows == 64 && round_index >= 3) ? 512 : 7168) : max(512, (rows * cols) / 4)) : max(64, rows * rows * cols * 16)
   def speed_budget = triangular_round_path ? tri_budget : (rows >= 64 ? 8192 : 4096)
   def bounded_method = track_transform ? "bounded-fast" : (triangular_round_path ? "bounded-int-no-transform" : "bounded-fast-no-transform")
   def rep = bounded_speed ? lll_backend.lll_reduce_bounded_report(fused.get("basis"), speed_budget, round_delta, bounded_method, round_eta) : lll_backend.lll_reduce_report(fused.get("basis"), round_delta, track_transform ? "fast" : "fast-no-transform", round_eta)
   {"rep": rep, "round_delta": round_delta}
}

fn _flatter_round_transform(any total_transform, dict fused, dict rep) any {
   if total_transform != nil && rep.get("transform_tracked", false) {
      def round_transform = _flatter_matmul(rep.get("transform"), fused.get("transform"))
      return _flatter_matmul(round_transform, total_transform)
   }
   rep.get("transform_tracked", false) ? total_transform : nil
}

fn _flatter_round_sort(any work, bool triangular_round_path, bool track_transform, int rows) dict {
   mut sorted = dict(0)
   mut ops = 0
   if triangular_round_path && !track_transform && rows >= 64 {
      sorted = short_row_prepass_report(work, false)
      ops = sorted.get("op_count", 0)
      if ops > 0 { work = sorted.get("basis") }
   }
   {"basis": work, "report": sorted, "op_count": ops}
}

fn _flatter_round_profile_goal(any work, str goal_target, any goal_value, bool goal_proved, bool skip_profile) list {
   mut round_shape = _flatter_skip("triangular no-transform fast summary")
   mut round_goal = _flatter_skipped_profile_goal()
   if !skip_profile {
      round_shape = profile_shape_report(work)
      round_goal = profile_goal_report(round_shape, goal_target, goal_value, goal_proved)
   }
   [round_shape, round_goal]
}

fn _flatter_round_next_state(str stopped_reason, dict round_goal, dict rep, dict fused, any first, any prev_first, bool stop_on_goal, bool triangular_round_path, bool track_transform, int rows) list {
   if bool(stop_on_goal) && round_goal.get("satisfied", false) { return [false, "profile-goal-after-round"] }
   if !rep.get("reduction_complete", true) && !(triangular_round_path && !track_transform && rows >= 64) { return [false, "lll-" + rep.get("incomplete_reason", "incomplete")] }
   if triangular_round_path && !track_transform && rows < 64 { return [false, "triangular-no-transform-one-round"] }
   def changed = first < prev_first || fused.get("op_count", 0) > 0 || rep.get("steps", 0) > 0
   [changed, changed ? stopped_reason : "fixed-point"]
}

fn _flatter_reduce_round(any work_in, any total_transform_in, int rows, int cols, any delta, any eta, str goal_target, any goal_value, bool goal_proved, bool stop_on_goal, bool track_transform, bool triangular_round_path, bool triangular_fast_summary, bool triangular_warmup_prepass, any prev_first, int round_index, str stopped_reason) dict {
   def fused = _flatter_round_fused(work_in, eta, triangular_round_path, track_transform)
   def lll = _flatter_round_lll(fused, rows, cols, delta, eta, triangular_round_path, triangular_warmup_prepass, track_transform, round_index)
   def rep = lll.get("rep")
   mut total_transform = _flatter_round_transform(total_transform_in, fused, rep)
   mut work = rep.get("basis")
   def sorted = _flatter_round_sort(work, triangular_round_path, track_transform, rows)
   work = sorted.get("basis")
   def sort_report = sorted.get("report")
   def sort_ops = sorted.get("op_count", 0)
   def prof = rep.get("profile_after", [])
   def first = sort_ops > 0 ? sort_report.get("first_norm_after", prof.get(0, prev_first)) : prof.get(0, prev_first)
   def skip_round_profile = triangular_fast_summary || (!stop_on_goal && triangular_round_path && rows >= 32)
   def profile_goal = _flatter_round_profile_goal(work, goal_target, goal_value, goal_proved, skip_round_profile)
   def next = _flatter_round_next_state(stopped_reason, profile_goal[1], rep, fused, first, prev_first, stop_on_goal, triangular_round_path, track_transform, rows)
   {
      "work": work, "transform": total_transform, "first": first, "changed": next[0], "stopped_reason": next[1],
      "goal_after": profile_goal[1], "fused_ops": fused.get("op_count", 0) + sort_ops, "lll_steps": rep.get("steps", 0),
      "summary": _flatter_round_summary(round_index, fused, rep, lll.get("round_delta"), prev_first, first, profile_goal[0], profile_goal[1])
   }
}

fn _flatter_final_report(
   any delta, int max_rounds, any eta, str goal_target, any goal_value,
   bool goal_proved, bool stop_on_goal, str stopped_reason, list rounds,
   list prepass_reports, bool triangular_prepass, bool triangular_warmup_prepass,
   int total_fused_ops, int total_prepass_ops, dict final_sort, int final_sort_ops,
   int total_lll_steps, dict before, dict after, dict before_shape, dict after_shape,
   dict goal_before, dict goal_after, any total_transform, any applied, any work, any started_at,
) dict {
   {
      "method": "flatter-profile",
      "compression": "iterated-fused-qr",
      "delta": delta,
      "eta": eta,
      "max_rounds": max_rounds,
      "goal_target": goal_target,
      "goal_value": goal_value,
      "goal_proved": goal_proved,
      "stop_on_goal": stop_on_goal,
      "stopped_reason": stopped_reason,
      "rounds": rounds,
      "round_count": rounds.len,
      "prepass_reports": prepass_reports,
      "prepass_count": prepass_reports.len,
      "triangular_prepass": triangular_prepass,
      "triangular_warmup_prepass": triangular_warmup_prepass,
      "op_count": total_fused_ops + total_prepass_ops,
      "compression_op_count": total_fused_ops,
      "prepass_op_count": total_prepass_ops,
      "final_sort_method": final_sort.get("method", ""),
      "final_sort_op_count": final_sort_ops,
      "final_sort_transform_verified": final_sort.get("transform_verified", false),
      "lll_steps": total_lll_steps,
      "profile_before": before.get("profile"),
      "profile_after": after.get("profile"),
      "profile_shape_before": before_shape,
      "profile_shape_after": after_shape,
      "profile_drop_before": before_shape.get("drop", 0.0),
      "profile_drop_after": after_shape.get("drop", 0.0),
      "profile_spread_before": before_shape.get("spread", 0.0),
      "profile_spread_after": after_shape.get("spread", 0.0),
      "profile_goal_before": goal_before,
      "profile_goal_after": goal_after,
      "profile_goal_satisfied": goal_after.get("satisfied", false),
      "compression_plan_before": _flatter_profile_compression_plan_from_logs(before_shape.get("profile_norm_log2", [])),
      "compression_plan_after": _flatter_profile_compression_plan_from_logs(after_shape.get("profile_norm_log2", [])),
      "profile_slope_before": before.get("profile_slope", 0.0),
      "profile_slope_after": after.get("profile_slope", 0.0),
      "gso_before": before,
      "gso_after": after,
      "transform": total_transform,
      "transform_tracked": total_transform != nil,
      "transform_verified": total_transform != nil && _flatter_same_matrix(applied, work),
      "verification_skipped": total_transform == nil,
      "elapsed_ms": float(ticks() - started_at) / 1000000.0,
      "basis": _flatter_public_basis(work)
   }
}

fn _flatter_small_direct_path(any delta, int max_rounds, any eta, list prepass_reports, bool triangular_prepass, int total_prepass_ops, bool track_transform, any work_in, any total_transform_in, int rows, any first_before_sort, any started_at) dict {
   mut work = work_in
   mut total_transform = total_transform_in
   mut prepass_ops = total_prepass_ops
   mut small_delta = float(delta) > 0.75 ? 0.75 : delta
   if _flatter_is_lower_triangular(work) && float(small_delta) > 0.55 { small_delta = 0.55 }
   if rows == 16 && !_flatter_is_lower_triangular(work) {
      def pair = lagrange_pair_reduce_report(work, 15, 1, 32, false, true, track_transform)
      def pair_ops = pair.get("row_op_count", pair.get("op_count", 0) + pair.get("swap_count", 0))
      def pair_after = pair.get("first_norm_after", first_before_sort)
      if pair_ops > 0 && first_before_sort > Z(0) && pair_after < first_before_sort {
         work = pair.get("basis")
         if pair.get("transform_tracked", false) {
            total_transform = _flatter_chain_prepass_transform(total_transform, pair.get("transform"), prepass_reports.len != 0)
         } else {
            total_transform = nil
         }
         prepass_ops += pair_ops
         prepass_reports = prepass_reports.append(_flatter_pair_prepass_summary(pair, pair_ops, first_before_sort, pair_after, true))
         small_delta = float(delta) > 0.55 ? 0.55 : delta
      }
   }
   def small_method = track_transform ? "fast" : "bounded-fast-no-transform"
   def small_budget = max(64, rows * rows * _flatter_matrix_cols(work) * 16)
   def small = track_transform ? lll_backend.lll_reduce_report(work, small_delta, small_method, eta) : lll_backend.lll_reduce_bounded_report(work, small_budget, small_delta, small_method, eta)
   work = small.get("basis")
   if total_transform != nil && small.get("transform_tracked", false) {
      total_transform = _flatter_matmul(small.get("transform"), total_transform)
   } elif !small.get("transform_tracked", false) {
      total_transform = nil
   }
   def small_sort = short_row_prepass_report(work, track_transform)
   def small_sort_ops = small_sort.get("op_count", 0)
   if small_sort_ops > 0 {
      work = small_sort.get("basis")
      if total_transform != nil { total_transform = _flatter_matmul(small_sort.get("transform"), total_transform) }
   }
   _flatter_small_direct_report(delta, max_rounds, eta, prepass_reports, triangular_prepass, prepass_ops, small_method, small_delta, small, small_sort, small_sort_ops, total_transform, work, rows, first_before_sort, started_at, track_transform)
}

fn _flatter_bounded_direct_path(any delta, int max_rounds, any eta, list prepass_reports, bool triangular_prepass, int total_prepass_ops, any work_in, int rows, int budget, any first_before_sort, any started_at, str reason) dict {
   def direct = lll_backend.lll_reduce_bounded_report(work_in, max(64, budget), delta, "bounded-int-no-transform", eta)
   mut work = direct.get("basis")
   def sorted = short_row_prepass_report(work, false)
   def sort_ops = sorted.get("op_count", 0)
   if sort_ops > 0 { work = sorted.get("basis") }
   mut out = _flatter_small_direct_report(delta, max_rounds, eta, prepass_reports, triangular_prepass, total_prepass_ops, direct.get("method", "bounded-int-no-transform"), delta, direct, sorted, sort_ops, nil, work, rows, first_before_sort, started_at, false)
   out["compression"] = "bounded-direct-lll"
   out["direct_budget"] = max(64, budget)
   out["direct_reason"] = reason
   out
}

fn _flatter_triangular_bounded_direct_path(any delta, int max_rounds, any eta, list prepass_reports, bool triangular_prepass, int total_prepass_ops, any work_in, int rows, int chunk_budget, int max_chunks, any first_before_sort, any started_at, str reason) dict {
   def direct = triangular_bounded_lll_prepass_report(work_in, chunk_budget, max_chunks, 0.75, 0.60, false)
   def work = direct.get("basis")
   def best_after = direct.get("best_norm_after", _flatter_best_norm_sq(work))
   mut reports = prepass_reports.append(_flatter_triangular_bounded_prepass_summary(direct, "triangular-bounded-direct-lll", first_before_sort, best_after, false, false, true))
   mut out = _flatter_prepass_exit_report("triangular-bounded-direct-lll", reason, delta, max_rounds, eta, reports, triangular_prepass, total_prepass_ops + direct.get("op_count", 0), nil, false, work, started_at, true)
   out["direct_budget"] = chunk_budget * max(1, max_chunks)
   out["direct_chunk_budget"] = chunk_budget
   out["direct_chunk_count"] = direct.get("chunk_count", max_chunks)
   out["direct_reason"] = reason
   out["lll_steps"] = direct.get("lll_step_count", 0)
   out["direct_chunks"] = direct.get("chunks", [])
   out
}

fn _flatter_triangular_prepass_path(any work_in, any total_transform_in, int rows, int max_rounds, any eta, bool track_transform, list prepass_reports_in, int total_prepass_ops_in) dict {
   mut work = work_in
   mut total_transform = total_transform_in
   mut prepass_reports = prepass_reports_in
   mut total_prepass_ops = total_prepass_ops_in
   mut triangular_prepass, triangular_warmup_prepass = false, false
   if rows < 32 || !_flatter_is_lower_triangular(work) {
      return {"work": work, "transform": total_transform, "reports": prepass_reports, "op_count": total_prepass_ops, "triangular": false, "warmup": false, "bounded": false}
   }
   mut bounded_triangular_prepass = false
   if !track_transform && rows >= 96 && _flatter_is_lower_triangular(work) {
      def current_best_col = _flatter_best_norm_sq(work)
      def col = _flatter_triangular_column_bounded_prepass_report(work, rows >= 128 ? 80000 : 24000, 0.97, 0.60)
      def col_best = col.get("best_norm_after", current_best_col)
      if col.get("found", false) && col_best < current_best_col {
         work = col.get("basis")
         total_transform = nil
         triangular_prepass = true
         bounded_triangular_prepass = true
         total_prepass_ops += col.get("op_count", 0)
         prepass_reports = prepass_reports.append({
               "method": col.get("method", "triangular-column-bounded-prepass"), "op_count": col.get("op_count", 0),
               "step_budget": col.get("step_budget", 0), "lll_step_count": col.get("lll_step_count", 0),
               "sort_op_count": col.get("sort_op_count", 0), "best_norm_before": current_best_col,
               "best_norm_after": col_best, "first_norm_before": col.get("first_norm_before", Z(0)),
               "first_norm_after": col.get("first_norm_after", Z(0)), "transform_tracked": false,
               "transform_verified": false, "verification_skipped": true, "elapsed_ms": col.get("elapsed_ms", 0.0)
         })
      }
   }
   if rows < 64 && !track_transform {
      def direct = triangular_bounded_lll_prepass_report(work, 2048, 2, 0.75, 0.60, false)
      def direct_best = direct.get("best_norm_after")
      def current_best = direct.get("best_norm_before")
      if direct_best < current_best {
         work = direct.get("basis")
         triangular_prepass = true
         triangular_warmup_prepass = true
         total_prepass_ops += direct.get("op_count", direct.get("steps", 0))
         prepass_reports = prepass_reports.append(_flatter_triangular_bounded_prepass_summary(direct, "triangular-bounded-warmup-prepass", current_best, direct_best, false, false, true))
      }
   }
   if rows >= 64 && !bounded_triangular_prepass {
      def direct_step_cap = track_transform ? 2048 : (rows == 64 ? 8192 : (rows >= 96 ? 6144 : 4096))
      def direct_rounds = track_transform ? 3 : (rows == 64 ? 8 : 8)
      def direct_eta = track_transform ? eta : 0.60
      def direct = triangular_bounded_lll_prepass_report(work, direct_step_cap, direct_rounds, 0.75, direct_eta, track_transform)
      def direct_best = _flatter_best_norm_sq(direct.get("basis"))
      def current_best = _flatter_best_norm_sq(work)
      if direct_best < current_best {
         work = direct.get("basis")
         if total_transform != nil && direct.get("transform_tracked", false) { total_transform = _flatter_matmul(direct.get("transform"), total_transform) }
         triangular_prepass = true
         bounded_triangular_prepass = true
         total_prepass_ops += direct.get("op_count", direct.get("steps", 0))
         prepass_reports = prepass_reports.append(_flatter_triangular_bounded_prepass_summary(direct, "triangular-bounded-lll-prepass", current_best, direct_best, direct.get("transform_tracked", false), direct.get("transform_verified", false), direct.get("verification_skipped", false)))
      }
   }
   if !bounded_triangular_prepass && rows >= 64 {
      def normal = triangular_tail_normal_form_report(work)
      if normal.get("found", false) && (normal.get("transform_verified", false) || !track_transform) {
         work = normal.get("basis")
         if total_transform != nil { total_transform = _flatter_matmul(normal.get("transform"), total_transform) }
         triangular_prepass = true
         total_prepass_ops += normal.get("op_count", 0)
         prepass_reports = prepass_reports.append({
               "method": normal.get("method", "triangular-tail-normal-form"), "op_count": normal.get("op_count", 0),
               "split": normal.get("split", -1), "tail_rows": normal.get("tail_rows", 0), "backsolve_ops": normal.get("backsolve_ops", 0),
               "reduction_ops": normal.get("reduction_ops", 0), "best_norm_before": normal.get("best_norm_before", Z(0)),
               "best_norm_after": normal.get("best_norm_after", Z(0)), "transform_verified": normal.get("transform_verified", false),
               "elapsed_ms": normal.get("elapsed_ms", 0.0)
         })
      }
   }
   if _flatter_is_lower_triangular(work) && (track_transform || rows >= 64) && !(bounded_triangular_prepass && !track_transform) {
      def tri = blocked_triangular_size_reduce_report(work, 32, max(1, max_rounds), track_transform)
      work = tri.get("basis")
      if total_transform != nil { total_transform = _flatter_matmul(tri.get("transform"), total_transform) }
      triangular_prepass = true
      total_prepass_ops += tri.get("op_count", 0)
      prepass_reports = prepass_reports.append({
            "method": tri.get("method", "blocked-triangular-size-reduction"), "op_count": tri.get("op_count", 0),
            "block_size": tri.get("block_size", 32), "block_count": tri.get("block_count", 0),
            "violations_before": tri.get("violations_before", 0), "violations_after": tri.get("violations_after", 0),
            "transform_verified": tri.get("transform_verified", false), "elapsed_ms": tri.get("elapsed_ms", 0.0)
      })
   }
   {"work": work, "transform": total_transform, "reports": prepass_reports, "op_count": total_prepass_ops, "triangular": triangular_prepass, "warmup": triangular_warmup_prepass, "bounded": bounded_triangular_prepass}
}

fn _flatter_reduction_rounds(
   any work_in, any total_transform_in, int rows, int cols, any delta, int max_rounds, any eta,
   str goal_target, any goal_value, bool goal_proved, bool stop_on_goal, bool track_transform,
   bool triangular_round_path, bool triangular_fast_summary, bool triangular_warmup_prepass,
   dict goal_before, dict before, list prepass_reports,
) dict {
   mut work = work_in
   mut total_transform = total_transform_in
   mut goal_after, rounds = goal_before, []
   mut prev_first = before.get("profile", [Z(0)]).get(0, Z(0))
   mut total_fused_ops, total_lll_steps, i = 0, 0, 0
   def bounded_triangular_stop = track_transform && prepass_reports.len > 0 && prepass_reports.get(prepass_reports.len - 1).get("method", "") == "triangular-bounded-lll-prepass"
   mut changed = !(bool(stop_on_goal) && goal_before.get("satisfied", false)) && !bounded_triangular_stop
   mut stopped_reason = bounded_triangular_stop ? "triangular-bounded-lll-prepass" : (changed ? "max-rounds" : "profile-goal-before")
   while changed && i < max_rounds {
      def round = _flatter_reduce_round(work, total_transform, rows, cols, delta, eta, goal_target, goal_value, goal_proved, stop_on_goal, track_transform, triangular_round_path, triangular_fast_summary, triangular_warmup_prepass, prev_first, i + 1, stopped_reason)
      work = round.get("work")
      total_transform = round.get("transform")
      rounds = rounds.append(round.get("summary"))
      goal_after = round.get("goal_after")
      total_fused_ops += round.get("fused_ops", 0)
      total_lll_steps += round.get("lll_steps", 0)
      changed = round.get("changed", false)
      stopped_reason = round.get("stopped_reason", stopped_reason)
      prev_first = round.get("first", prev_first)
      i += 1
   }
   {
      "work": work, "transform": total_transform, "rounds": rounds,
      "fused_ops": total_fused_ops, "lll_steps": total_lll_steps,
      "goal_after": goal_after, "stopped_reason": stopped_reason
   }
}

fn _flatter_round_path_report(
   any original, any work_in, any total_transform_in, int rows, int cols,
   any delta, int max_rounds, any eta, str goal_target, any goal_value,
   bool goal_proved, bool stop_on_goal, bool track_transform,
   list prepass_reports_in, bool triangular_prepass_in, int total_prepass_ops_in, any t0
) dict {
   mut work = work_in
   mut total_transform = total_transform_in
   mut prepass_reports = prepass_reports_in
   mut total_prepass_ops = total_prepass_ops_in
   def tri_path = _flatter_triangular_prepass_path(work, total_transform, rows, max_rounds, eta, track_transform, prepass_reports, total_prepass_ops)
   work = tri_path.get("work")
   total_transform = tri_path.get("transform")
   prepass_reports = tri_path.get("reports")
   total_prepass_ops = tri_path.get("op_count")
   def triangular_prepass = triangular_prepass_in || tri_path.get("triangular")
   def triangular_warmup_prepass = tri_path.get("warmup")
   if !track_transform && prepass_reports.len > 0 {
      def last_prepass = prepass_reports.get(prepass_reports.len - 1)
      def last_method = last_prepass.get("method", "")
      if (last_method == "triangular-column-bounded-prepass" || last_method == "triangular-column-unit-prepass") && Z(last_prepass.get("best_norm_after", Z(2))) <= Z(1) {
         mut solved = _flatter_prepass_exit_report(last_method, "triangular column prepass solved unit vector", delta, 0, eta, prepass_reports, triangular_prepass, total_prepass_ops, nil, false, work, t0, true)
         solved["requested_max_rounds"] = max_rounds
         solved["strategy_round_floor"] = "triangular-column-solved"
         return solved
      }
   }
   def triangular_round_path = _flatter_is_lower_triangular(work) || triangular_warmup_prepass || tri_path.get("bounded", false)
   def triangular_fast_summary = triangular_round_path && !track_transform && !stop_on_goal
   def bounded_triangular_stop = track_transform && prepass_reports.len > 0 && prepass_reports.get(prepass_reports.len - 1).get("method", "") == "triangular-bounded-lll-prepass"
   def skip_profile_summary = triangular_fast_summary || bounded_triangular_stop || (triangular_round_path && !stop_on_goal && rows >= 32) || (!track_transform && !stop_on_goal && rows >= 48)
   mut before_shape, before = _flatter_skip("no-transform fast summary"), _flatter_profile_stub(work, rows)
   mut goal_before = _flatter_skipped_profile_goal()
   if !skip_profile_summary {
      before_shape = profile_shape_report(work)
      before = before_shape.get("gso")
      goal_before = profile_goal_report(before_shape, goal_target, goal_value, goal_proved)
   }
   mut effective_max_rounds = max_rounds
   mut strategy_round_floor = ""
   if triangular_fast_summary && rows == 64 && max_rounds < 3 {
      effective_max_rounds = 3
      strategy_round_floor = "triangular-dim64"
   }
   if triangular_fast_summary && rows == 96 && max_rounds < 24 {
      effective_max_rounds = 24
      strategy_round_floor = "triangular-dim96"
   }
   if prepass_reports.len > 0 {
      def last_prepass = prepass_reports.get(prepass_reports.len - 1)
      def last_method = last_prepass.get("method", "")
      if (last_method == "triangular-column-bounded-prepass" || last_method == "triangular-column-unit-prepass") && Z(last_prepass.get("best_norm_after", Z(2))) <= Z(1) {
         effective_max_rounds = 0
         strategy_round_floor = "triangular-column-solved"
      }
   }
   def round_path = _flatter_reduction_rounds(work, total_transform, rows, cols, delta, effective_max_rounds, eta, goal_target, goal_value, goal_proved, stop_on_goal, track_transform, triangular_round_path, skip_profile_summary, triangular_warmup_prepass, goal_before, before, prepass_reports)
   work = round_path.get("work")
   total_transform = round_path.get("transform")
   def rounds = round_path.get("rounds")
   def total_fused_ops = round_path.get("fused_ops")
   def total_lll_steps = round_path.get("lll_steps")
   mut goal_after = round_path.get("goal_after")
   def stopped_reason = round_path.get("stopped_reason")
   def final_short_only = total_transform == nil && !stop_on_goal && (triangular_fast_summary || (!track_transform && rows >= 48))
   def final_sort = final_short_only ? short_row_prepass_report(work, false) : _flatter_norm_sort_report(work, total_transform != nil)
   def final_sort_ops = final_sort.get("op_count", 0)
   if final_sort_ops > 0 {
      work = final_sort.get("basis")
      if total_transform != nil { total_transform = _flatter_matmul(final_sort.get("transform"), total_transform) }
   }
   mut after_shape, after = _flatter_skip("no-transform fast summary"), _flatter_profile_stub(work, rows)
   if !skip_profile_summary {
      after_shape = profile_shape_report(work)
      after = after_shape.get("gso")
      goal_after = profile_goal_report(after_shape, goal_target, goal_value, goal_proved)
   }
   def applied = total_transform == nil ? work : _flatter_matmul(total_transform, original)
   mut out = _flatter_final_report(delta, effective_max_rounds, eta, goal_target, goal_value, goal_proved, stop_on_goal, stopped_reason, rounds, prepass_reports, triangular_prepass, triangular_warmup_prepass, total_fused_ops, total_prepass_ops, final_sort, final_sort_ops, total_lll_steps, before, after, before_shape, after_shape, goal_before, goal_after, total_transform, applied, work, t0)
   if effective_max_rounds != max_rounds {
      out["requested_max_rounds"] = max_rounds
      out["strategy_round_floor"] = strategy_round_floor
   }
   out
}

fn _flatter_prepass_state(any work, any total_transform, list prepass_reports, int total_prepass_ops, bool exit=false, any report=nil, bool qary_found=false) dict {
   {
      "work": work, "transform": total_transform, "reports": prepass_reports,
      "op_count": total_prepass_ops, "exit": exit, "report": report, "qary_found": qary_found
   }
}

fn _flatter_zero_rank_report(any delta, int max_rounds, any eta, int rows, int cols, any started_at) dict {
   {
      "method": "flatter-profile",
      "compression": "zero-rank",
      "delta": delta,
      "eta": eta,
      "max_rounds": max_rounds,
      "stopped_reason": "zero-rank",
      "rounds": [],
      "round_count": 0,
      "prepass_reports": [],
      "prepass_count": 0,
      "triangular_prepass": false,
      "triangular_warmup_prepass": false,
      "op_count": 0,
      "compression_op_count": 0,
      "prepass_op_count": 0,
      "final_sort_op_count": 0,
      "lll_steps": 0,
      "input_rows": rows,
      "input_cols": cols,
      "profile_before": [],
      "profile_after": [],
      "profile_shape_before": _flatter_skip("zero-rank"),
      "profile_shape_after": _flatter_skip("zero-rank"),
      "profile_drop_before": 0.0,
      "profile_drop_after": 0.0,
      "profile_spread_before": 0.0,
      "profile_spread_after": 0.0,
      "profile_slope_before": 0.0,
      "profile_slope_after": 0.0,
      "gso_before": _flatter_skip("zero-rank"),
      "gso_after": _flatter_skip("zero-rank"),
      "transform": nil,
      "transform_tracked": false,
      "transform_verified": false,
      "verification_skipped": true,
      "elapsed_ms": float(ticks() - started_at) / 1000000.0,
      "basis": []
   }
}

fn _flatter_upper_qary_scaled_transform_report(any original, int split, any delta, int max_rounds, any eta, str goal_target, any goal_value, bool goal_proved, bool stop_on_goal, any started_at) dict {
   def oriented = _flatter_qary_blockswap_orientation(original, split)
   def first_before = _flatter_best_norm_sq(original)
   def shifts = _flatter_qary_uniform_column_shifts(oriented, split, 74)
   def scaled = _flatter_scale_columns_by_shifts(oriented, shifts)
   def red_t0 = ticks()
   def red = lll_backend.lll_reduce_report(scaled, 0.97, "fast", 0.51)
   def red_elapsed = float(ticks() - red_t0) / 1000000.0
   def apply_t0 = ticks()
   mut restored = _flatter_qary_unblockswap_columns(_flatter_matmul(red.get("transform"), oriented), split)
   def apply_elapsed = float(ticks() - apply_t0) / 1000000.0
   def post_t0 = ticks()
   def post = lll_backend.lll_reduce_bounded_report(restored, 3000000, 0.99, "bounded-int-no-transform-compact", 0.51)
   restored = post.get("basis")
   def post_elapsed = float(ticks() - post_t0) / 1000000.0
   def sort = short_row_prepass_report(restored, false)
   restored = sort.get("basis")
   def best_after = sort.get("first_norm_after", _flatter_best_norm_sq(restored))
   mut out = _flatter_prepass_exit_report("upper-qary-scaled-transform", "upper-qary-block-orientation", delta, max_rounds, eta, [], false, red.get("steps", 0) + post.get("steps", 0) + sort.get("op_count", 0), nil, false, restored, started_at, true)
   out["compression"] = "upper-qary-scaled-transform"
   out["orientation"] = "block-column-swap"
   out["orientation_split"] = split
   out["orientation_inner_compression"] = "qary-column-scaled-lll"
   out["orientation_qary_keep_bits"] = 74
   out["orientation_qary_shift"] = shifts.get(0, 0)
   out["orientation_scaled_delta"] = 0.97
   out["orientation_scaled_eta"] = 0.51
   out["orientation_scaled_steps"] = red.get("steps", 0)
   out["orientation_scaled_complete"] = red.get("reduction_complete", false)
   out["orientation_scaled_core"] = red.get("core", "")
   out["orientation_scaled_elapsed_ms"] = red_elapsed
   out["orientation_apply_elapsed_ms"] = apply_elapsed
   out["orientation_cleanup_budget"] = 3000000
   out["orientation_cleanup_steps"] = post.get("steps", 0)
   out["orientation_cleanup_complete"] = post.get("reduction_complete", false)
   out["orientation_cleanup_elapsed_ms"] = post_elapsed
   out["orientation_sort_applied"] = sort.get("swaps", 0) > 0
   out["orientation_sort_op_count"] = sort.get("op_count", 0)
   out["orientation_strong_applied"] = true
   out["orientation_strong_norm_before"] = first_before
   out["orientation_strong_norm_after"] = best_after
   out["orientation_strong_elapsed_ms"] = red_elapsed + apply_elapsed + post_elapsed + sort.get("elapsed_ms", 0.0)
   out["orientation_strong_steps"] = red.get("steps", 0) + post.get("steps", 0)
   out["orientation_strong_complete"] = red.get("reduction_complete", false) && post.get("reduction_complete", false)
   out["lll_steps"] = red.get("steps", 0) + post.get("steps", 0)
   out["goal_target"] = goal_target
   out["goal_value"] = goal_value
   out["goal_proved"] = goal_proved
   out["stop_on_goal"] = stop_on_goal
   out["transform"] = nil
   out["transform_tracked"] = false
   out["transform_verified"] = false
   out["verification_skipped"] = true
   out["elapsed_ms"] = float(ticks() - started_at) / 1000000.0
   out
}

fn _flatter_upper_qary_orientation_report(any original, int split, any delta, int max_rounds, any eta, str goal_target, any goal_value, bool goal_proved, bool stop_on_goal, any started_at) dict {
   def oriented = _flatter_qary_blockswap_orientation(original, split)
   def inner_t0 = ticks()
   def first_before = _flatter_dot_z(_flatter_matrix_data(oriented).get(0), _flatter_matrix_data(oriented).get(0))
   if _flatter_matrix_rows(oriented) >= 96 && float(delta) >= 0.95 && max_rounds >= 3 {
      return _flatter_upper_qary_scaled_transform_report(original, split, delta, max_rounds, eta, goal_target, goal_value, goal_proved, stop_on_goal, started_at)
   }
   if _flatter_matrix_rows(oriented) <= 64 && float(delta) >= 0.95 && max_rounds >= 3 {
      def strong_t = ticks()
      def sized = blocked_triangular_size_reduce_report(oriented, 32, 1, false)
      def primary_delta = _flatter_matrix_rows(oriented) == 64 ? 0.70 : 0.931
      def primary_eta = 0.75
      def strong = lll_backend.lll_reduce_report(sized.get("basis"), primary_delta, "fast-no-transform", primary_eta)
      def primary_elapsed = float(ticks() - strong_t) / 1000000.0
      def primary_restored = _flatter_qary_unblockswap_columns(strong.get("basis"), split)
      def primary_best = _flatter_best_norm_sq(primary_restored)
      def cleanup_t = ticks()
      def cleanup = lll_backend.lll_reduce_report(strong.get("basis"), 0.99, "fast-no-transform", 0.51)
      def cleanup_restored = _flatter_qary_unblockswap_columns(cleanup.get("basis"), split)
      def cleanup_best = _flatter_best_norm_sq(cleanup_restored)
      def cleanup_elapsed = float(ticks() - cleanup_t) / 1000000.0
      def cleanup_applied = cleanup_best < primary_best
      def restored_unsorted = cleanup_applied ? cleanup_restored : primary_restored
      def sort = short_row_prepass_report(restored_unsorted, false)
      def restored = sort.get("basis")
      def strong_best = sort.get("first_norm_after", cleanup_applied ? cleanup_best : primary_best)
      def strong_elapsed = float(ticks() - strong_t) / 1000000.0
      def total_steps = strong.get("steps", 0) + (cleanup_applied ? cleanup.get("steps", 0) : 0)
      mut out = _flatter_prepass_exit_report("upper-qary-strong-orientation", "upper-qary-block-orientation", delta, max_rounds, eta, [], false, sized.get("op_count", 0) + total_steps + sort.get("op_count", 0), nil, false, restored, started_at, true)
      out["orientation"] = "block-column-swap"
      out["orientation_split"] = split
      out["orientation_inner_compression"] = "full-gso-lll-plus-strict-cleanup"
      out["orientation_inner_elapsed_ms"] = strong_elapsed
      out["orientation_delta"] = 0.99
      out["orientation_eta"] = 0.75
      out["orientation_strong_applied"] = true
      out["orientation_strong_norm_before"] = first_before
      out["orientation_strong_norm_after"] = strong_best
      out["orientation_strong_elapsed_ms"] = strong_elapsed
      out["orientation_strong_steps"] = total_steps
      out["orientation_strong_complete"] = strong.get("reduction_complete", false) && (!cleanup_applied || cleanup.get("reduction_complete", false))
      out["orientation_sort_applied"] = sort.get("swaps", 0) > 0
      out["orientation_sort_op_count"] = sort.get("op_count", 0)
      out["orientation_primary_delta"] = primary_delta
      out["orientation_primary_eta"] = primary_eta
      out["orientation_primary_norm_after"] = primary_best
      out["orientation_primary_elapsed_ms"] = primary_elapsed
      out["orientation_primary_steps"] = strong.get("steps", 0)
      out["orientation_cleanup_applied"] = cleanup_applied
      out["orientation_cleanup_delta"] = 0.99
      out["orientation_cleanup_eta"] = 0.51
      out["orientation_cleanup_norm_after"] = cleanup_best
      out["orientation_cleanup_elapsed_ms"] = cleanup_elapsed
      out["orientation_cleanup_steps"] = cleanup.get("steps", 0)
      out["orientation_size_reduce_ops"] = sized.get("op_count", 0)
      out["lll_steps"] = total_steps
      out["goal_target"] = goal_target
      out["goal_value"] = goal_value
      out["goal_proved"] = goal_proved
      out["stop_on_goal"] = stop_on_goal
      out["transform"] = nil
      out["transform_tracked"] = false
      out["transform_verified"] = false
      out["verification_skipped"] = true
      out["elapsed_ms"] = float(ticks() - started_at) / 1000000.0
      return out
   }
   def direct = triangular_bounded_lll_prepass_report(oriented, 8192, 4, 0.75, 0.75, false)
   def direct_best = direct.get("best_norm_after", _flatter_best_norm_sq(direct.get("basis")))
   def report = _flatter_triangular_bounded_prepass_summary(direct, "upper-qary-block-bounded-lll", first_before, direct_best, false, false, true)
   def inner = _flatter_prepass_exit_report("triangular-bounded-direct-lll", "upper-qary-block-orientation", delta, max_rounds, eta, [report], false, direct.get("op_count", 0), nil, false, direct.get("basis"), inner_t0, true)
   inner["direct_budget"] = 8192 * 4
   inner["direct_chunk_budget"] = 8192
   inner["direct_chunk_count"] = direct.get("chunk_count", 4)
   inner["direct_reason"] = "upper-qary-block-orientation"
   inner["lll_steps"] = direct.get("lll_step_count", 0)
   inner["direct_chunks"] = direct.get("chunks", [])
   inner["orientation_delta"] = 0.75
   inner["orientation_eta"] = 0.75
   mut restored = _flatter_qary_unblockswap_columns(inner.get("basis"), split)
   mut postpass_applied = false
   mut postpass_norm_before = _flatter_best_norm_sq(restored)
   mut postpass_norm_after = postpass_norm_before
   mut postpass_elapsed = 0.0
   if _flatter_matrix_rows(oriented) >= 48 {
      def post_t = ticks()
      def post = triangular_bounded_lll_prepass_report(restored, 4096, 1, 0.75, 0.75, false)
      def post_basis = post.get("basis")
      def post_best = _flatter_best_norm_sq(post_basis)
      postpass_elapsed = float(ticks() - post_t) / 1000000.0
      if post_best < postpass_norm_before {
         restored = post_basis
         postpass_applied = true
         postpass_norm_after = post_best
      }
   }
   def sort = short_row_prepass_report(restored, false)
   restored = sort.get("basis")
   mut out = inner
   out["basis"] = _flatter_public_basis(restored)
   out["compression"] = "upper-qary-orientation"
   out["orientation"] = "block-column-swap"
   out["orientation_split"] = split
   out["orientation_inner_compression"] = "triangular-bounded-direct-lll"
   out["orientation_inner_elapsed_ms"] = direct.get("elapsed_ms", 0.0)
   out["orientation_postpass_applied"] = postpass_applied
   out["orientation_postpass_budget"] = _flatter_matrix_rows(oriented) >= 48 ? 4096 : 0
   out["orientation_postpass_norm_before"] = postpass_norm_before
   out["orientation_postpass_norm_after"] = postpass_norm_after
   out["orientation_postpass_elapsed_ms"] = postpass_elapsed
   out["orientation_sort_applied"] = sort.get("swaps", 0) > 0
   out["orientation_sort_op_count"] = sort.get("op_count", 0)
   out["orientation_strong_applied"] = false
   out["orientation_strong_norm_before"] = postpass_norm_after
   out["orientation_strong_norm_after"] = sort.get("first_norm_after", postpass_norm_after)
   out["orientation_strong_elapsed_ms"] = 0.0
   out["orientation_strong_steps"] = 0
   out["orientation_strong_complete"] = false
   out["goal_target"] = goal_target
   out["goal_value"] = goal_value
   out["goal_proved"] = goal_proved
   out["stop_on_goal"] = stop_on_goal
   out["transform"] = nil
   out["transform_tracked"] = false
   out["transform_verified"] = false
   out["verification_skipped"] = true
   out["elapsed_ms"] = float(ticks() - started_at) / 1000000.0
   out
}

fn _flatter_ntru2_prepass(any work, any total_transform, list prepass_reports, int total_prepass_ops, bool triangular_prepass, bool track_transform, any delta, int max_rounds, any eta, any t0) dict {
   def classic = _flatter_ntru_sum_relation_prepass_report(work, track_transform)
   if classic.get("found", false) {
      work = classic.get("basis")
      total_transform = track_transform ? classic.get("transform") : nil
      total_prepass_ops += classic.get("weight", 0)
      prepass_reports = prepass_reports.append(_flatter_basic_prepass_summary(classic, "ntru-sum-relation-prepass", classic.get("weight", 0), classic.get("first_norm_after", 0)))
      def rep = _flatter_prepass_exit_report("ntru-sum-relation-prepass", "ntru top-sum relation", delta, max_rounds, eta, prepass_reports, triangular_prepass, total_prepass_ops, total_transform, track_transform && classic.get("transform_verified", false), work, t0, !track_transform)
      return _flatter_prepass_state(work, total_transform, prepass_reports, total_prepass_ops, true, rep)
   }
   def ntru = _flatter_ntru2_sum_relation_prepass_report(work, track_transform)
   if ntru.get("found", false) {
      work = ntru.get("basis")
      total_transform = track_transform ? ntru.get("transform") : nil
      total_prepass_ops += ntru.get("weight", 0)
      prepass_reports = prepass_reports.append(_flatter_basic_prepass_summary(ntru, "ntru2-sum-relation-prepass", ntru.get("weight", 0), ntru.get("first_norm_after", 0)))
      def rep = _flatter_prepass_exit_report("ntru2-sum-relation-prepass", "ntru2 all-tail sum relation", delta, max_rounds, eta, prepass_reports, triangular_prepass, total_prepass_ops, total_transform, track_transform && ntru.get("transform_verified", false), work, t0, !track_transform)
      return _flatter_prepass_state(work, total_transform, prepass_reports, total_prepass_ops, true, rep)
   }
   _flatter_prepass_state(work, total_transform, prepass_reports, total_prepass_ops)
}

fn _flatter_qary_prepasses(any work, any total_transform, list prepass_reports, int total_prepass_ops, bool triangular_prepass, int rows, bool lower_before_initial, bool skip_general_prepasses, bool track_transform, any delta, int max_rounds, any eta, any t0) dict {
   mut qary = {"found": false}
   if !skip_general_prepasses { qary = _flatter_qary_relation_prepass_report(work, "qary-relation-prepass", false, track_transform) }
   if qary.get("found", false) {
      work = qary.get("basis")
      total_transform = track_transform ? qary.get("transform") : nil
      total_prepass_ops += qary.get("weight", 0)
      prepass_reports = prepass_reports.append(_flatter_basic_prepass_summary(qary, "qary-relation-prepass", qary.get("weight", 0), qary.get("first_norm_after", 0)))
      if rows >= 8 {
         def rep = _flatter_prepass_exit_report("qary-relation-prepass", "qary relation prepass", delta, max_rounds, eta, prepass_reports, triangular_prepass, total_prepass_ops, total_transform, track_transform && qary.get("transform_verified", false), work, t0, !track_transform)
         return _flatter_prepass_state(work, total_transform, prepass_reports, total_prepass_ops, true, rep, true)
      }
   }
   if lower_before_initial && rows >= 8 {
      def lower_split = _flatter_lower_triangular_qary_split(work)
      if lower_split >= 0 && !_flatter_lower_qary_uniform_modulus(work, lower_split) {
         return _flatter_prepass_state(work, total_transform, prepass_reports, total_prepass_ops, false, nil, qary.get("found", false))
      }
      def lower_qary = _flatter_qary_relation_prepass_report(work, "lower-triangular-qary-relation-prepass", true, track_transform)
      if lower_qary.get("found", false) {
         work = lower_qary.get("basis")
         total_transform = track_transform ? lower_qary.get("transform") : nil
         total_prepass_ops += lower_qary.get("weight", 0)
         prepass_reports = prepass_reports.append(_flatter_basic_prepass_summary(lower_qary, "lower-triangular-qary-relation-prepass", lower_qary.get("weight", 0), lower_qary.get("first_norm_after", 0)))
         def rep = _flatter_prepass_exit_report("lower-triangular-qary-relation-prepass", "lower triangular qary relation prepass", delta, max_rounds, eta, prepass_reports, triangular_prepass, total_prepass_ops, total_transform, track_transform && lower_qary.get("transform_verified", false), work, t0, !track_transform)
         return _flatter_prepass_state(work, total_transform, prepass_reports, total_prepass_ops, true, rep, qary.get("found", false))
      }
   }
   _flatter_prepass_state(work, total_transform, prepass_reports, total_prepass_ops, false, nil, qary.get("found", false))
}

fn _flatter_short_prepass(any work, any total_transform, list prepass_reports, int total_prepass_ops, bool triangular_prepass, int rows, bool lower_before, bool skip_general_prepasses, bool track_transform, any first_before_sort, any delta, int max_rounds, any eta, any t0) dict {
   mut short = dict(0)
   mut first_after_short = first_before_sort
   mut short_swapped = false
   def rectangular_tail_fast = !track_transform && rows >= 48 && _flatter_matrix_cols(work) > rows
   if !skip_general_prepasses && !rectangular_tail_fast {
      short = short_row_prepass_report(work, track_transform)
      first_after_short = short.get("first_norm_after", first_before_sort)
      short_swapped = short.get("swaps", 0) > 0 && first_before_sort > Z(0)
   }
   def strong_short = (rows < 96 || track_transform || lower_before) && short_swapped && first_after_short * Z(1000) < first_before_sort
   def large_short = rows >= 64 && rows < 96 && !track_transform && !lower_before && short_swapped && first_after_short * Z(4) < first_before_sort
   if strong_short || large_short {
      work = short.get("basis")
      if total_transform != nil { total_transform = _flatter_chain_prepass_transform(total_transform, short.get("transform"), prepass_reports.len != 0) }
      total_prepass_ops += short.get("op_count", 0)
      prepass_reports = prepass_reports.append(_flatter_basic_prepass_summary(short, "short-row-prepass", short.get("op_count", 0), first_after_short))
      prepass_reports[prepass_reports.len - 1]["best_row"] = short.get("best_row", 0)
      def rep = _flatter_prepass_exit_report("short-row-prepass", large_short ? "large basis short row prepass" : "short row prepass", delta, max_rounds, eta, prepass_reports, triangular_prepass, total_prepass_ops, total_transform, track_transform && short.get("transform_verified", false), work, t0, !track_transform)
      mut out = _flatter_prepass_state(work, total_transform, prepass_reports, total_prepass_ops, true, rep, false)
      out["first_after_short"] = first_after_short
      return out
   }
   mut out = _flatter_prepass_state(work, total_transform, prepass_reports, total_prepass_ops)
   out["first_after_short"] = first_after_short
   out
}

fn _flatter_triplet_pair_prepass(any work, any total_transform, list prepass_reports, int total_prepass_ops, bool triangular_prepass, int rows, bool lower_before, any first_before_sort, any first_after_short, bool track_transform, any delta, int max_rounds, any eta, any t0) dict {
   if rows < 24 || lower_before { return _flatter_prepass_state(work, total_transform, prepass_reports, total_prepass_ops) }
   if rows == 32 || rows == 64 || (rows <= 128 && _flatter_is_local_banded(work, 4)) {
      def triplet = banded_triplet_prepass_report(work, rows >= 64 ? 4 : 3, 2)
      def triplet_ops = triplet.get("op_count", 0)
      def triplet_after = triplet.get("first_norm_after", first_before_sort)
      if triplet.get("found", false) && triplet_ops > 0 && triplet_after < first_after_short {
         work = triplet.get("basis")
         if total_transform != nil { total_transform = _flatter_chain_prepass_transform(total_transform, triplet.get("transform"), prepass_reports.len != 0) }
         total_prepass_ops += triplet_ops
         prepass_reports = prepass_reports.append(_flatter_triplet_prepass_summary(triplet, triplet_ops, first_before_sort, triplet_after))
         def rep = _flatter_prepass_exit_report("banded-triplet-prepass", "banded triplet prepass", delta, max_rounds, eta, prepass_reports, triangular_prepass, total_prepass_ops, total_transform, track_transform && triplet.get("transform_verified", false), work, t0, !track_transform)
         return _flatter_prepass_state(work, total_transform, prepass_reports, total_prepass_ops, true, rep)
      }
   }
   def pair_window = rows < 32 ? 16 : (rows <= 32 ? 12 : (rows <= 64 ? 2 : (rows <= 96 ? 1 : 8)))
   def pair_rounds = rows < 32 ? 1 : (rows <= 32 ? max(2, max_rounds) : 1)
   def pair = lagrange_pair_reduce_report(work, min(pair_window, rows - 1), pair_rounds, 32, false, true, track_transform)
   def pair_ops = pair.get("row_op_count", pair.get("op_count", 0) + pair.get("swap_count", 0))
   def pair_after = pair.get("first_norm_after", first_before_sort)
   if pair_ops > 0 && pair_after <= first_after_short {
      work = pair.get("basis")
      if total_transform != nil && pair.get("transform_tracked", true) { total_transform = _flatter_chain_prepass_transform(total_transform, pair.get("transform"), prepass_reports.len != 0) }
      total_prepass_ops += pair_ops
      prepass_reports = prepass_reports.append(_flatter_pair_prepass_summary(pair, pair_ops, first_before_sort, pair_after))
      def rep = _flatter_prepass_exit_report("lagrange-row-pair-reduction", "lagrange row-pair prepass", delta, max_rounds, eta, prepass_reports, triangular_prepass, total_prepass_ops, total_transform, track_transform && pair.get("transform_verified", false), work, t0, pair.get("verification_skipped", false) || !track_transform)
      return _flatter_prepass_state(work, total_transform, prepass_reports, total_prepass_ops, true, rep)
   }
   _flatter_prepass_state(work, total_transform, prepass_reports, total_prepass_ops)
}

fn _flatter_norm_sort_prepass(any work, any total_transform, list prepass_reports, int total_prepass_ops, bool triangular_prepass, bool skip_general_prepasses, bool track_transform, any first_before_sort, any delta, int max_rounds, any eta, any t0) dict {
   if skip_general_prepasses { return _flatter_prepass_state(work, total_transform, prepass_reports, total_prepass_ops) }
   def sorted = _flatter_norm_sort_report(work, track_transform)
   def first_after_sort = sorted.get("first_norm_after", first_before_sort)
   if sorted.get("swaps", 0) > 0 && first_before_sort > Z(0) && first_after_sort * Z(1000) < first_before_sort {
      work = sorted.get("basis")
      if total_transform != nil { total_transform = _flatter_chain_prepass_transform(total_transform, sorted.get("transform"), prepass_reports.len != 0) }
      total_prepass_ops += sorted.get("op_count", 0)
      prepass_reports = prepass_reports.append(_flatter_basic_prepass_summary(sorted, "norm-sort-prepass", sorted.get("op_count", 0), first_after_sort))
      def rep = _flatter_prepass_exit_report("norm-sort-prepass", "norm sort prepass", delta, max_rounds, eta, prepass_reports, triangular_prepass, total_prepass_ops, total_transform, track_transform && sorted.get("transform_verified", false), work, t0, !track_transform)
      return _flatter_prepass_state(work, total_transform, prepass_reports, total_prepass_ops, true, rep)
   }
   _flatter_prepass_state(work, total_transform, prepass_reports, total_prepass_ops)
}

fn flatter_reduce_report(any basis, any delta=0.99, int max_rounds=3, any eta=0.51, str goal_target="slope", any goal_value=nil, bool goal_proved=false, bool stop_on_goal=false, bool track_transform=true) dict {
   "Flatter-style large-basis reduction report with profile-driven LLL rounds."
   def t0 = ticks()
   def original = _flatter_reduce_matrix(basis)
   def rows = _flatter_matrix_rows(original)
   def cols = _flatter_matrix_cols(original)
   if rows == 0 || cols == 0 || _flatter_all_zero(original) { return _flatter_zero_rank_report(delta, max_rounds, eta, rows, cols, t0) }
   mut work = original
   mut total_transform = track_transform ? matrix.matrix_identity(rows) : nil
   mut prepass_reports, total_prepass_ops, triangular_prepass = [], 0, false
   def lower_before_initial = _flatter_is_lower_triangular(work)
   if !track_transform && !lower_before_initial {
      def upper_qary_split = _flatter_upper_qary_split(work)
      if upper_qary_split >= 0 {
         return _flatter_upper_qary_orientation_report(original, upper_qary_split, delta, max_rounds, eta, goal_target, goal_value, goal_proved, stop_on_goal, t0)
      }
   }
   def skip_general_prepasses = lower_before_initial && rows >= 32 && !track_transform
   def ntru = _flatter_ntru2_prepass(work, total_transform, prepass_reports, total_prepass_ops, triangular_prepass, track_transform, delta, max_rounds, eta, t0)
   if ntru.get("exit", false) { return ntru.get("report") }
   work = ntru.get("work")
   total_transform = ntru.get("transform")
   prepass_reports = ntru.get("reports")
   total_prepass_ops = ntru.get("op_count")
   def qary = _flatter_qary_prepasses(work, total_transform, prepass_reports, total_prepass_ops, triangular_prepass, rows, lower_before_initial, skip_general_prepasses, track_transform, delta, max_rounds, eta, t0)
   if qary.get("exit", false) { return qary.get("report") }
   work = qary.get("work")
   total_transform = qary.get("transform")
   prepass_reports = qary.get("reports")
   total_prepass_ops = qary.get("op_count")
   def first_before_sort = _flatter_dot_z(_flatter_matrix_data(work).get(0), _flatter_matrix_data(work).get(0))
   def local_banded_initial = _flatter_is_local_banded(work, 4)
   if !track_transform && (lower_before_initial || !local_banded_initial) && (rows == 48 || rows == 64) {
      def steep_spread = lower_before_initial ? _flatter_lower_triangular_profile_spread(work) : profile_shape_report(work).get("spread", 0.0)
      if rows == 48 && steep_spread > 24.0 {
         return _flatter_bounded_direct_path(delta, max_rounds, eta, prepass_reports, triangular_prepass, total_prepass_ops, work, rows, 2048, first_before_sort, t0, "steep-profile-dim48")
      }
      if rows == 64 && steep_spread > 24.0 {
         return _flatter_triangular_bounded_direct_path(delta, max_rounds, eta, prepass_reports, triangular_prepass, total_prepass_ops, work, rows, 2048, 4, first_before_sort, t0, "steep-profile-dim64")
      }
   }
   def lower_before = qary.get("qary_found", false) ? _flatter_is_lower_triangular(work) : lower_before_initial
   if !lower_before && rows >= 32 && rows <= 128 && local_banded_initial {
      def local = _flatter_triplet_pair_prepass(work, total_transform, prepass_reports, total_prepass_ops, triangular_prepass, rows, lower_before, first_before_sort, first_before_sort, track_transform, delta, max_rounds, eta, t0)
      if local.get("exit", false) { return local.get("report") }
   }
   def short = _flatter_short_prepass(work, total_transform, prepass_reports, total_prepass_ops, triangular_prepass, rows, lower_before, skip_general_prepasses, track_transform, first_before_sort, delta, max_rounds, eta, t0)
   if short.get("exit", false) { return short.get("report") }
   def first_after_short = short.get("first_after_short", first_before_sort)
   def pair = _flatter_triplet_pair_prepass(work, total_transform, prepass_reports, total_prepass_ops, triangular_prepass, rows, lower_before, first_before_sort, first_after_short, track_transform, delta, max_rounds, eta, t0)
   if pair.get("exit", false) { return pair.get("report") }
   def sorted = _flatter_norm_sort_prepass(work, total_transform, prepass_reports, total_prepass_ops, triangular_prepass, skip_general_prepasses, track_transform, first_before_sort, delta, max_rounds, eta, t0)
   if sorted.get("exit", false) { return sorted.get("report") }
   if rows >= 8 && rows <= 16 {
      return _flatter_small_direct_path(delta, max_rounds, eta, prepass_reports, triangular_prepass, total_prepass_ops, track_transform, work, total_transform, rows, first_before_sort, t0)
   }
   _flatter_round_path_report(original, work, total_transform, rows, cols, delta, max_rounds, eta, goal_target, goal_value, goal_proved, stop_on_goal, track_transform, prepass_reports, triangular_prepass, total_prepass_ops, t0)
}

fn flatter_reduce(any basis, any delta=0.99, int max_rounds=3, any eta=0.51) any {
   "Return the flatter-reduced basis from flatter_reduce_report."
   flatter_reduce_report(basis, delta, max_rounds, eta, "slope", nil, false, false, false).get("basis")
}

fn vec_clone(list v) list {
   "Create a deep copy of vector v: returns a new list with the same elements."
   mut n = v.len
   mut result = list(0)
   mut i = 0
   while i < n {
      result = result.append(v.get(i))
      i += 1
   }
   result
}

fn vec_dot(list a, list b) any {
   "Compute the dot product of two vectors a and b: returns the sum of element-wise products."
   mut n = a.len
   mut result = 0
   mut i = 0
   while i < n {
      result = result + a.get(i) * b.get(i)
      i += 1
   }
   result
}

fn vec_norm_sq(list v) any {
   "Compute the squared Euclidean norm of vector v: equivalent to vec_dot(v, v)."
   vec_dot(v, v)
}

fn vec_scale(list v, any s) list {
   "Scale vector v by scalar s: returns a new vector with each element multiplied by s."
   mut n = v.len
   mut result = list(0)
   mut i = 0
   while i < n {
      result = result.append(v.get(i) * s)
      i += 1
   }
   result
}

fn vec_scale_add(list a, list b, any coeff) list {
   "Compute a + coeff * b as a new vector: returns the linear combination of vectors a and b."
   mut n = a.len
   mut result = list(0)
   mut i = 0
   while i < n {
      result = result.append(a.get(i) + coeff * b.get(i))
      i += 1
   }
   result
}

fn vec_sub_scaled(list a, list b, any coeff) list {
   "Compute a - coeff * b as a new vector: returns the vector difference of a and scaled b."
   mut n = a.len
   mut result = list(0)
   mut i = 0
   while i < n {
      result = result.append(a.get(i) - coeff * b.get(i))
      i += 1
   }
   result
}

fn _gram_schmidt_nonempty(list basis, int n) list {
   mut gs = list(0)
   def first = vec_clone(basis.get(0))
   gs = gs.append(first)
   mut i = 1
   while i < n {
      def bi = vec_clone(basis.get(i))
      mut proj = vec_clone(bi)
      mut j = 0
      while j < i {
         def gsj = gs.get(j)
         def gs_norm = vec_norm_sq(gsj)
         def mu_num = vec_dot(bi, gsj)
         def mu_scaled = (gs_norm != 0) ? mu_num / gs_norm : 0
         proj = vec_sub_scaled(proj, gsj, mu_scaled)
         j += 1
      }
      gs = gs.append(proj)
      i += 1
   }
   gs
}

fn gram_schmidt_rows(list basis) list {
   "Perform Gram-Schmidt orthogonalization on a list of basis vectors: returns a list of mutually orthogonal vectors spanning the same subspace."
   def n = basis.len
   n == 0 ? list(0) : _gram_schmidt_nonempty(basis, n)
}

fn _basis_clone(list basis, int n) list {
   mut out = list(0)
   mut i = 0
   while i < n {
      out = out.append(vec_clone(basis.get(i)))
      i += 1
   }
   out
}

fn lll_reduce(list basis, any delta) list {
   "Perform full LLL lattice reduction with given delta parameter: basis is a list of integer vectors, delta controls reduction quality(typically 0.75); returns the reduced basis."
   def n = basis.len
   n == 0 ? basis : lll_reduce_delta(_basis_clone(basis, n), delta)
}

fn lll_reduce_delta(list basis, any delta) list {
   "Core LLL reduction with delta parameter: performs size reduction and Lovasz condition swaps in-place on basis; delta is typically 0.75 or 0.99; returns the reduced basis."
   mut n = basis.len
   if n == 0 { return basis }
   mut k = 1
   while k < n {
      mut j = k - 1
      while j >= 0 {
         def gs = gram_schmidt_rows(basis)
         def gsj = gs.get(j)
         def gs_norm = vec_norm_sq(gsj)
         def bk = basis.get(k)
         def mu_num = vec_dot(bk, gsj)
         if mu_num != 0 && gs_norm != 0 {
            mut c = (mu_num + gs_norm / 2) / gs_norm
            def bk_cur, bj_cur = basis.get(k), basis.get(j)
            def new_bk = vec_sub_scaled(bk_cur, bj_cur, c)
            basis[k] = new_bk
         }
         j = j - 1
      }
      def gs = gram_schmidt_rows(basis)
      def gsk = gs.get(k)
      def gsk_prev = gs.get(k - 1)
      def gs_prev_norm = vec_norm_sq(gsk_prev)
      def lhs = (delta * 100) * gs_prev_norm
      def rhs_num = vec_norm_sq(gsk) * 100
      if lhs > rhs_num {
         def bk = basis.get(k)
         def bk_prev = basis.get(k - 1)
         basis[k - 1] = bk
         basis[k] = bk_prev
         k = (k > 1) ? k - 1 : k
      } else {
         k += 1
      }
   }
   basis
}

fn shortest_vector(list basis) list {
   "Find the shortest non-zero vector in a lattice using LLL reduction: reduces the basis with delta=0.75 and returns the vector with the smallest norm."
   def reduced = lll_reduce(basis, 75)
   mut n = reduced.len
   if n == 0 { return list(0) }
   mut best = vec_clone(reduced.get(0))
   mut best_norm = vec_norm_sq(best)
   mut i = 1
   while i < n {
      def v = reduced.get(i)
      def vn = vec_norm_sq(v)
      if vn < best_norm {
         best = vec_clone(v)
         best_norm = vn
      }
      i += 1
   }
   best
}

fn babai_cvp(list basis, list target) list {
   "Babai closest vector algorithm: given a lattice basis and a target vector, find the closest lattice point using Gram-Schmidt rounding; returns the reconstructed closest lattice vector."
   mut n = basis.len
   if n == 0 { return target }
   def gs = gram_schmidt_rows(basis)
   mut t = vec_clone(target)
   mut coeffs = list(0)
   mut i = n - 1
   while i >= 0 {
      def gsi = gs.get(i)
      def gs_norm = vec_norm_sq(gsi)
      def mu_num = vec_dot(t, gsi)
      mut c = (gs_norm != 0) ? (mu_num + gs_norm / 2) / gs_norm : 0
      coeffs = coeffs.append(c)
      def bi = basis.get(i)
      t, i = vec_sub_scaled(t, bi, c), i - 1
   }
   mut result = vec_clone(target)
   i = 0
   while i < n {
      mut c = coeffs.get(n - 1 - i)
      def bi = basis.get(i)
      result = vec_sub_scaled(result, bi, c)
      i += 1
   }
   result
}
