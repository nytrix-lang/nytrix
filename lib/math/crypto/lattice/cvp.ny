;; Keywords: lattice cvp
;; Lattice routines for closest-vector and bounded modular linear solving.
;; Reference:
;; - https://cims.nyu.edu/~regev/teaching/lattices_fall_2004/ln/cvp.pdf
module std.math.crypto.lattice.cvp(build_lattice, solve_inequality, solve_inequality_ex, cvp, cvp_report, cvp_babai, cvp_babai_report, cvp_enumerate, cvp_enumerate_report, cvp_list_gso_count_report, cvp_gso_bound_report, affine_cvp, bounded, bounded_ex, qary_lattice, reduce_mod_p, solve_weighted_bounds, solve_multi_mod_linear, solve_underconstrained_linear, enum_brute, mod_arc_is_inside, mod_arc_has_solution, mod_arc_optf, mod_arc_solve_range)
use std.math.nt
use std.math.big
use std.math.scalar as math
use std.math.matrix as matrix
use std.math.crypto.lattice.flatter
use std.math.crypto.lattice.lll as lllcore
use std.core.str (atof)
use std.os.clock (ticks)
use std.os.prim (env)

fn _cvp_set_fields(dict: out, list: fields): dict {
   mut i = 0
   while(i < fields.len){
      def field = fields.get(i)
      out = out.set(to_str(field.get(0)), field.get(1, nil))
      i += 1
   }
   out
}

fn _z(any: x): bigint { is_bigint(x) ? x : Z(x) }

fn _cvp_elapsed_ms(any: t0): f64 { float(ticks() - t0) / 1000000.0 }

fn _cvp_nodes_per_sec(int: nodes, any: elapsed_ms): f64 {
   def ms = float(elapsed_ms)
   ms <= 0.0 ? 0.0 : float(nodes) * 1000.0 / ms
}

fn _cvp_finish_report(any: out, any: t0): dict {
   def elapsed = _cvp_elapsed_ms(t0)
   out["elapsed_ms"] = elapsed
   out.set("nodes_per_sec", _cvp_nodes_per_sec(int(out.get("nodes", 0)), elapsed))
}

fn _cvp_count_level(list: nodes_by_level, int: idx): list {
   if(idx >= 0 && idx < nodes_by_level.len){ nodes_by_level[idx] = int(nodes_by_level[idx]) + 1 }
   nodes_by_level
}

fn _cvp_enum_report_common(dict: out, bool: reduced, int: coeff_bound, int: max_nodes, int: nodes, list: nodes_by_level, bool: hit_limit, int: radius_levels, any: radius_sq, list: vector, any: distance_sq, bool: verified, list: basis): dict {
   {
      "method": out.get("method", "gso-bounded-enumeration"),
      "ok": true,
      "reduced": reduced,
      "coeff_bound": coeff_bound,
      "max_nodes": max_nodes,
      "nodes": nodes,
      "nodes_by_level": nodes_by_level,
      "hit_limit": hit_limit,
      "radius_levels": radius_levels,
      "radius_sq": radius_sq,
      "vector": vector,
      "distance_sq": distance_sq,
      "verified": verified,
      "basis": basis,
   }
}

fn _cvp_attach_gso_reuse_report(any: out, int: input_gso_builds, int: work_gso_builds, bool: input_gso_reused, str: work_gso_source): dict {
   _cvp_set_fields(out, [
         ["input_gso_builds", input_gso_builds], ["work_gso_builds", work_gso_builds],
         ["input_gso_reused", input_gso_reused], ["work_gso_source", work_gso_source],
   ])
}

fn _cvp_attach_enum_context(any: out, any: reduction_report, str: candidate_mode, any: residual, any: coords, int: input_gso_builds, int: work_gso_builds, bool: input_gso_reused, str: work_gso_source): dict {
   out = _cvp_set_fields(out, [
         ["reduction", reduction_report], ["candidate_mode", candidate_mode],
         ["residualization", residual], ["target_coordinates", coords],
   ])
   _cvp_attach_gso_reuse_report(out, input_gso_builds, work_gso_builds, input_gso_reused, work_gso_source)
}

fn _cvp_enum_context_common(dict: out, bool: reduced, int: coeff_bound, int: max_nodes, int: nodes, list: nodes_by_level, bool: hit_limit, int: radius_levels, any: radius_sq, list: vector, any: distance_sq, bool: verified, list: basis, any: reduction_report, str: candidate_mode, any: residual, any: coords, int: input_gso_builds, int: work_gso_builds, bool: input_gso_reused, str: work_gso_source): dict {
   _cvp_attach_enum_context(
      _cvp_enum_report_common(out, reduced, coeff_bound, max_nodes, nodes, nodes_by_level, hit_limit, radius_levels, radius_sq, vector, distance_sq, verified, basis),
      reduction_report, candidate_mode, residual, coords,
   input_gso_builds, work_gso_builds, input_gso_reused, work_gso_source)
}

fn _cvp_big_float(any: x): f64 {
   def z = Z(x)
   def neg = z < Z(0)
   def abs_z = neg ? Z(0) - z : z
   def s = bigint_to_str(abs_z)
   def n = s.len
   if(n == 0){ return 0.0 }
   mut out = 0.0
   if(n <= 18){
      mut i = 0
      while(i < n){
         out = out * 10.0 + float(load8(s, i) - 48)
         i += 1
      }
   } else {
      mut i = 0
      while(i < 17){
         out = out * 10.0 + float(load8(s, i) - 48)
         i += 1
      }
      out = out * math.pow(10.0, float(n - 17))
   }
   neg ? (0.0 - out) : out
}

fn _cvp_float(any: x): f64 { is_bigint(x) ? _cvp_big_float(x) : float(x) }

fn _at(any: v, int: i, any: fallback): any {
   if(!is_list(v) || i < 0 || i >= v.len){ return fallback }
   v[i]
}

fn _tail_vec(list: vals, int: offset, int: count): list {
   mut out = []
   mut i = 0
   while(i < count){
      out = out.append(_z(_at(vals, offset + i, 0)))
      i += 1
   }
   out
}

fn _abs_z(any: x): bigint {
   def zx = _z(x)
   zx < 0 ? -zx : zx
}

fn _vec_add(list: a, list: b): list {
   mut out = []
   mut i = 0
   while(i < a.len){
      out = out.append(_z(a[i]) + _z(_at(b, i, 0)))
      i += 1
   }
   out
}

fn _vec_sub(list: a, list: b): list {
   mut out = []
   mut i = 0
   while(i < a.len){
      out = out.append(_z(a[i]) - _z(_at(b, i, 0)))
      i += 1
   }
   out
}

fn _vec_equal(list: a, list: b): bool {
   if(a.len != b.len){ return false }
   mut i = 0
   while(i < a.len){
      if(_z(a[i]) != _z(_at(b, i, 0))){ return false }
      i += 1
   }
   true
}

fn _vec_dot(list: a, list: b): bigint {
   mut s, i = Z(0), 0
   while(i < a.len){
      s = s + _z(a[i]) * _z(_at(b, i, 0))
      i += 1
   }
   s
}

fn _vec_add_scaled(list: a, list: b, any: k): list {
   mut out = []
   mut i = 0
   def kk = _z(k)
   while(i < a.len){
      out = out.append(_z(a[i]) + _z(_at(b, i, 0)) * kk)
      i += 1
   }
   out
}

fn _round_div(any: num, any: den): bigint {
   if(den == 0){ return Z(0) }
   def nn = _z(num)
   def dd = _z(den)
   if(nn >= 0){ return(nn + dd / 2) / dd }
   -(((-nn) + dd / 2) / dd)
}

fn _ceil_div(any: num, any: den): bigint {
   if(den == 0){ panic("cvp: ceil_div division by zero") }
   def nn = _z(num)
   def dd = _z(den)
   if(nn >= 0){ return(nn + dd - 1) / dd }
   nn / dd
}

fn _check_rect(list: mat): list {
   if(!is_list(mat) || mat.len == 0){ panic("cvp: matrix must be a non-empty list of rows") }
   def n = len(mat[0])
   if(n == 0){ panic("cvp: matrix must have at least one column") }
   mut i = 1
   while(i < mat.len){
      if(len(mat[i]) != n){ panic("cvp: matrix rows must have equal length") }
      i += 1
   }
   [mat.len, n]
}

fn _scale_cols(list: mat, list: scales): list {
   mut out = []
   mut i = 0
   while(i < mat.len){
      def row = mat[i]
      mut srow = []
      mut j = 0
      while(j < row.len){
         srow = srow.append(_z(row[j]) * _z(_at(scales, j, 1)))
         j += 1
      }
      out = out.append(srow)
      i += 1
   }
   out
}

fn _scale_vec(list: v, list: scales): list {
   mut out = []
   mut i = 0
   while(i < v.len){
      out = out.append(_z(v[i]) * _z(_at(scales, i, 1)))
      i += 1
   }
   out
}

fn _unscale_vec(list: v, list: scales): list {
   mut out = []
   mut i = 0
   while(i < v.len){
      def s, x = _z(_at(scales, i, 1)), _z(v[i])
      out = out.append(s == 0 ? x : x / s)
      i += 1
   }
   out
}

fn _unscale_rows(list: rows, list: scales): list {
   mut out = []
   mut i = 0
   while(i < rows.len){
      out = out.append(_unscale_vec(rows[i], scales))
      i += 1
   }
   out
}

fn _matrix_zero(int: rows, int: cols): list {
   mut out = []
   mut i = 0
   while(i < rows){
      mut row = []
      mut j = 0
      while(j < cols){
         row = row.append(Z(0))
         j += 1
      }
      out = out.append(row)
      i += 1
   }
   out
}

fn _zero_vec(int: n): list {
   mut out = []
   mut i = 0
   while(i < n){
      out = out.append(Z(0))
      i += 1
   }
   out
}

fn _nodes_vec(int: n): list {
   mut out = []
   mut i = 0
   while(i <= n){
      out = out.append(0)
      i += 1
   }
   out
}

fn _cvp_trace_enabled(): bool {
   def v = env("NY_CVP_TRACE")
   is_str(v) && (v == "1" || v == "true" || v == "yes")
}

fn _cvp_trace(str: label, any: value=nil): any {
   if(_cvp_trace_enabled()){
      if(value == nil){ print("[cvp]", label) } else { print("[cvp]", label, value) }
   }
   value
}

fn _cvp_row_norm(list: row): bigint { _vec_dot(row, row) }

fn _cvp_sort_rows_by_norm(list: basis): list {
   mut rows = clone(basis)
   mut out = []
   while(rows.len > 0){
      mut best_i = 0
      mut best_norm = _cvp_row_norm(rows[0])
      mut i = 1
      while(i < rows.len){
         def nrm = _cvp_row_norm(rows[i])
         if(nrm < best_norm){
            best_norm = nrm
            best_i = i
         }
         i += 1
      }
      out = out.append(rows[best_i])
      rows = rows.remove(best_i)
   }
   out
}

fn _cvp_sort_rows_by_norm_desc(list: basis): list {
   mut rows = clone(basis)
   mut out = []
   while(rows.len > 0){
      mut best_i = 0
      mut best_norm = _cvp_row_norm(rows[0])
      mut i = 1
      while(i < rows.len){
         def nrm = _cvp_row_norm(rows[i])
         if(nrm > best_norm){
            best_norm = nrm
            best_i = i
         }
         i += 1
      }
      out = out.append(rows[best_i])
      rows = rows.remove(best_i)
   }
   out
}

fn _cvp_enum_order_basis(list: basis): list {
   def v = env("NY_CVP_ENUM_ORDER")
   if(is_str(v) && (v == "row-norm-desc" || v == "desc")){ return _cvp_sort_rows_by_norm_desc(basis) }
   basis
}

fn _cvp_size_reduce_rows(list: basis, int: rounds=2): list {
   mut rows = clone(basis)
   mut pass = 0
   while(pass < rounds){
      mut changed = false
      mut i = 1
      while(i < rows.len){
         mut j = i - 1
         while(j >= 0){
            def den = _cvp_row_norm(rows[j])
            if(den != 0){
               def q = _round_div(_vec_dot(rows[i], rows[j]), den)
               if(q != 0){
                  def cand = _vec_add_scaled(rows[i], rows[j], -q)
                  if(_cvp_row_norm(cand) < _cvp_row_norm(rows[i])){
                     rows[i] = cand
                     changed = true
                  }
               }
            }
            j -= 1
         }
         i += 1
      }
      rows = _cvp_sort_rows_by_norm(rows)
      if(!changed){ return rows }
      pass += 1
   }
   rows
}

fn _cvp_gso_size_reduce_rows(list: basis, int: rounds=2): list {
   mut rows = clone(basis)
   mut pass = 0
   while(pass < rounds){
      mut changed = false
      def gso = _cvp_gso_mu_profile(rows)
      def mu = gso.get("mu")
      mut i = 1
      while(i < rows.len){
         mut j = i - 1
         while(j >= 0){
            def q = _cvp_round_float(_cvp_mu_get(mu, i, j))
            if(q != 0){
               rows[i] = _vec_add_scaled(rows[i], rows[j], -q)
               changed = true
            }
            j -= 1
         }
         i += 1
      }
      if(!changed){ return rows }
      pass += 1
   }
   rows
}

fn _cvp_lovasz_holds(any: gso, int: k, any: delta): bool {
   def norms = gso.get("norms_sq", [])
   def mu = gso.get("mu")
   def lhs = _cvp_float(_at(norms, k, 0.0))
   def prev = _cvp_float(_at(norms, k - 1, 0.0))
   def m = _cvp_float(_cvp_mu_get(mu, k, k - 1))
   lhs >= (_cvp_float(delta) - m * m) * prev
}

fn _cvp_lll_fast_reduce_rows(list: basis, any: delta=0.75, any: eta=0.51, int: max_steps=20000): list {
   mut rows = clone(basis)
   def n = rows.len
   if(n <= 1){ return rows }
   mut k = 1
   mut steps = 0
   while(k < n && steps < max_steps){
      def gso = _cvp_gso_mu_profile_prefix(rows, k)
      def mu = gso.get("mu")
      mut reduced = false
      mut j = k - 1
      while(j >= 0 && steps < max_steps){
         def mu_kj = _cvp_mu_get(mu, k, j)
         if(_cvp_abs_float(mu_kj) > _cvp_float(eta)){
            def q = _cvp_round_float(mu_kj)
            if(q != 0){
               rows[k] = _vec_add_scaled(rows[k], rows[j], -q)
               steps += 1
               reduced = true
            }
         }
         j -= 1
      }
      def gso2 = reduced ? _cvp_gso_mu_profile_prefix(rows, k) : gso
      if(_cvp_lovasz_holds(gso2, k, delta)){
         k += 1
      } else {
         def cur = rows[k]
         rows[k] = rows[k - 1]
         rows[k - 1] = cur
         k = max(k - 1, 1)
         steps += 1
      }
   }
   def final_rounds = _cvp_env_int("NY_CVP_FINAL_GSO_ROUNDS", 2)
   final_rounds <= 0 ? rows : _cvp_gso_size_reduce_rows(rows, final_rounds)
}

fn _cvp_env_bool(str: name, bool: fallback=false): bool {
   def v = env(name)
   case v {
      "1", "true", "yes" -> true
      "0", "false", "no" -> false
      _ -> fallback
   }
}

fn _cvp_aggressive_reduce_enabled(): bool {
   _cvp_env_bool("NY_CVP_AGGRESSIVE_REDUCE")
}

fn _cvp_embedding_enabled(int: n): bool {
   _cvp_env_bool("NY_CVP_EMBEDDING", n <= 18)
}

fn _cvp_coordinate_cube_enabled(int: n): bool {
   _cvp_env_bool("NY_CVP_COORD_CUBE", n <= 32)
}

fn _cvp_descent_enabled(int: n): bool {
   _cvp_env_bool("NY_CVP_DESCENT", n < 32)
}

fn _cvp_env_int(str: name, int: fallback): int {
   def v = env(name)
   if(is_str(v) && v.len > 0){ return atoi(v) }
   fallback
}

fn _cvp_reduce_steps(int: n): int {
   def fallback = n <= 32 ? max(1200, n * 64) : (n <= 48 ? max(1500, n * 36) : max(1600, n * 24))
   _cvp_env_int("NY_CVP_REDUCE_STEPS", fallback)
}

fn _cvp_reduce_delta(int: n): f64 {
   def v = env("NY_CVP_REDUCE_DELTA")
   if(is_str(v) && v.len > 0){ return atof(v) }
   0.75
}

fn _cvp_reduce_mode(): str {
   def v = env("NY_CVP_REDUCER")
   is_str(v) && v.len > 0 ? v : "lll"
}

fn _cvp_reduce_basis(list: basis): list {
   if(basis.len > 48 && !_cvp_aggressive_reduce_enabled()){
      _cvp_trace("reduce:large-basis-size-pass", {"rows": basis.len, "cols": basis.len > 0 ? basis[0].len : 0})
      return _cvp_gso_size_reduce_rows(_cvp_size_reduce_rows(basis, 2), 2)
   }
   if(basis.len <= 32){
      def m = matrix.Matrix(basis)
      return matrix._matrix_data(lllcore.lll(m, 0.99, "ny"))
   }
   if(basis.len <= 48){
      def mode = _cvp_reduce_mode()
      if(mode == "none" || mode == "off"){
         return basis
      }
      if(mode == "size" || mode == "size-reduce"){
         return _cvp_gso_size_reduce_rows(_cvp_size_reduce_rows(basis, 3), 3)
      }
      if(mode == "gso-size"){
         return _cvp_gso_size_reduce_rows(basis, 4)
      }
      return _cvp_lll_fast_reduce_rows(basis, _cvp_reduce_delta(basis.len), 0.51, _cvp_reduce_steps(basis.len))
   }
   basis
}

fn _cvp_gso_profile(list: basis): dict {
   def gs = gram_schmidt_rows(basis)
   mut norms = []
   mut i = 0
   while(i < gs.len){
      norms = norms.append(_vec_dot(gs[i], gs[i]))
      i += 1
   }
   _cvp_set_fields(dict(6), [
         ["rows", basis.len], ["b_star", gs], ["norms_sq", norms], ["profile", norms],
   ])
}

fn _cvp_mu_get(list: mu, int: i, int: j): any {
   if(i < 0 || i >= mu.len){ return Z(0) }
   def row = mu[i]
   if(!is_list(row) || j < 0 || j >= row.len){ return Z(0) }
   row[j]
}

fn _cvp_float_row(list: row): list<f64> {
   mut out = []
   mut i = 0
   while(i < row.len){
      out = out.append(_cvp_float(row[i]))
      i += 1
   }
   out
}

fn _cvp_float_target_dot(list: a, list: b): f64 {
   mut s = 0.0
   mut i = 0
   while(i < a.len){
      s += _cvp_float(_at(a, i, 0)) * _cvp_float(_at(b, i, 0.0))
      i += 1
   }
   s
}

fn _cvp_f64_dot(list<f64>: a, list<f64>: b): f64 {
   mut s = 0.0
   mut i = 0
   while(i < a.len){
      s += a[i] * b[i]
      i += 1
   }
   s
}

fn _cvp_f64_sub_scaled(list<f64>: a, list<f64>: b, f64: coeff): list<f64> {
   mut out = []
   mut i = 0
   while(i < a.len){
      out = out.append(a[i] - coeff * b[i])
      i += 1
   }
   out
}

fn _cvp_gso_mu_profile(list: basis): dict {
   def n = basis.len
   _cvp_gso_mu_profile_prefix(basis, n - 1)
}

fn _cvp_gso_mu_profile_prefix(list: basis, int: limit): dict {
   def n = min(basis.len, max(0, limit + 1))
   mut bstar = list(0)
   mut mu = []
   mut i = 0
   while(i < n){
      mut v = _cvp_float_row(basis[i])
      mut mu_row = []
      mut j = 0
      while(j < i){
         def list<f64>: bj = bstar.get(j)
         def den = _cvp_f64_dot(bj, bj)
         def den_zero = den == 0.0
         def muij = den_zero ? 0.0 : _cvp_f64_dot(v, bj) / den
         mu_row = mu_row.append(muij)
         if(!den_zero){ v = _cvp_f64_sub_scaled(v, bj, muij) }
         j += 1
      }
      bstar = bstar.append(v)
      mu = mu.append(mu_row)
      i += 1
   }
   mut norms = []
   i = 0
   while(i < bstar.len){
      def list<f64>: row = bstar.get(i)
      norms = norms.append(_cvp_f64_dot(row, row))
      i += 1
   }
   {
      "rows": n,
      "b_star": bstar,
      "mu": mu,
      "norms_sq": norms,
      "profile": norms,
   }
}

fn _cvp_offset_bound_for_level(any: radius_sq, any: norm_sq, int: coeff_bound): int {
   def cap = max(1, int(coeff_bound))
   if(norm_sq == Z(0)){ return cap }
   def ratio = max(0.0, _cvp_float(radius_sq) / max(0.000001, _cvp_float(norm_sq)))
   def b = int(math.ceil(math.sqrt(ratio))) + 1
   max(1, min(cap, b))
}

fn _cvp_gso_offset_bounds(list: norms, any: radius_sq, int: coeff_bound): list {
   mut out = []
   mut i = 0
   while(i < norms.len){
      out = out.append(_cvp_offset_bound_for_level(radius_sq, norms[i], coeff_bound))
      i += 1
   }
   out
}

fn _cvp_bounds_sum(list: bounds): int {
   mut total = 0
   mut i = 0
   while(i < bounds.len){
      total += int(bounds[i])
      i += 1
   }
   total
}

fn _cvp_bounds_product(list: bounds, int: max_nodes): int {
   mut total = 1
   mut i = 0
   while(i < bounds.len){
      total *= 2 * int(bounds[i]) + 1
      if(total > max_nodes){ return max_nodes + 1 }
      i += 1
   }
   total
}

fn cvp_gso_bound_report(list: basis, list: target, bool: reduce=true, int: coeff_bound=2): dict {
   "Report the per-level GSO offset bounds used by CVP bounded enumeration."
   def work = reduce ? _cvp_reduce_basis(basis) : basis
   def seed = _cvp_nearest_seed(work, target)
   def seed_norm = seed.len > 0 ? _cvp_distance_sq(seed, target) : Z(0)
   def gso = _cvp_gso_profile(work)
   def bounds = _cvp_gso_offset_bounds(gso.get("norms_sq", []), seed_norm, coeff_bound)
   _cvp_set_fields(dict(12), [
         ["method", "cvp-gso-offset-bounds"], ["reduced", reduce], ["coeff_bound", coeff_bound],
         ["radius_sq", seed_norm], ["offset_bounds", bounds], ["offset_radius_max", _cvp_bounds_sum(bounds)],
         ["offset_bound_estimate", _cvp_bounds_product(bounds, 200000)], ["gso_profile", gso], ["basis", work],
   ])
}

fn _cvp_target_coords(list: basis, list: target): list {
   if(!is_list(basis) || basis.len == 0){ return [] }
   def gs = gram_schmidt_rows(basis)
   mut coords = []
   mut i = 0
   while(i < basis.len){
      coords = coords.append(Z(0))
      i += 1
   }
   mut diff = clone(target)
   i = basis.len - 1
   while(i >= 0){
      def g = gs[i]
      def den = _vec_dot(g, g)
      def num = _vec_dot(diff, g)
      def k = _round_div(num, den)
      coords[i] = k
      diff = vec_sub_scaled(diff, basis[i], k)
      i -= 1
   }
   coords
}

fn _cvp_coeff_order(any: center, int: bound): list {
   mut out = [center]
   mut d = 1
   while(d <= bound){
      out = out.append(center + d)
      out = out.append(center - d)
      d += 1
   }
   out
}

fn _cvp_offset_order(int: bound): list {
   mut out = [0]
   mut d = 1
   while(d <= bound){
      out = out.append(d)
      out = out.append(-d)
      d += 1
   }
   out
}

fn _cvp_abs_int(int: x): int { x < 0 ? -x : x }

fn _cvp_round_float(any: x): int {
   def f = _cvp_float(x)
   f >= 0.0 ? int(f + 0.5) : int(f - 0.5)
}

fn _cvp_target_coords_solve(list: basis, list: target): list {
   if(!is_list(basis) || basis.len == 0){ return [] }
   def n = basis.len
   if(target.len != n || basis[0].len != n){ return _cvp_target_coords(basis, target) }
   mut a = []
   mut b = []
   mut r = 0
   mut c = 0
   while(r < n){
      mut row = []
      c = 0
      while(c < n){
         row = row.append(_cvp_float(_at(basis[c], r, 0)))
         c += 1
      }
      a = a.append(row)
      b = b.append(_cvp_float(_at(target, r, 0)))
      r += 1
   }
   mut col = 0
   while(col < n){
      mut piv = col
      mut best = _cvp_abs_float(a[col][col])
      r = col + 1
      while(r < n){
         def score = _cvp_abs_float(a[r][col])
         if(score > best){
            best = score
            piv = r
         }
         r += 1
      }
      if(best < 0.000000000001){ return _cvp_target_coords(basis, target) }
      if(piv != col){
         def ar = a[col]
         a[col] = a[piv]
         a[piv] = ar
         def bv = b[col]
         b[col] = b[piv]
         b[piv] = bv
      }
      def pv = a[col][col]
      c = col
      while(c < n){
         a[col][c] = a[col][c] / pv
         c += 1
      }
      b[col] = b[col] / pv
      r = 0
      while(r < n){
         if(r != col){
            def f = a[r][col]
            if(f != 0.0){
               c = col
               while(c < n){
                  a[r][c] = a[r][c] - f * a[col][c]
                  c += 1
               }
               b[r] = b[r] - f * b[col]
            }
         }
         r += 1
      }
      col += 1
   }
   mut out = []
   r = 0
   while(r < n){
      out = out.append(_cvp_round_float(b[r]))
      r += 1
   }
   out
}

fn _cvp_target_coords_float_solve(list: basis, list: target): any {
   if(!is_list(basis) || basis.len == 0){ return nil }
   def n = basis.len
   if(target.len != n || basis[0].len != n){ return nil }
   mut a = []
   mut b = []
   mut r = 0
   mut c = 0
   while(r < n){
      mut row = []
      c = 0
      while(c < n){
         row = row.append(_cvp_float(_at(basis[c], r, 0)))
         c += 1
      }
      a = a.append(row)
      b = b.append(_cvp_float(_at(target, r, 0)))
      r += 1
   }
   mut col = 0
   while(col < n){
      mut piv = col
      mut best = _cvp_abs_float(a[col][col])
      r = col + 1
      while(r < n){
         def score = _cvp_abs_float(a[r][col])
         if(score > best){
            best = score
            piv = r
         }
         r += 1
      }
      if(best < 0.000000000001){ return nil }
      if(piv != col){
         def ar = a[col]
         a[col] = a[piv]
         a[piv] = ar
         def bv = b[col]
         b[col] = b[piv]
         b[piv] = bv
      }
      def pv = a[col][col]
      c = col
      while(c < n){
         a[col][c] = a[col][c] / pv
         c += 1
      }
      b[col] = b[col] / pv
      r = 0
      while(r < n){
         if(r != col){
            def f = a[r][col]
            if(f != 0.0){
               c = col
               while(c < n){
                  a[r][c] = a[r][c] - f * a[col][c]
                  c += 1
               }
               b[r] = b[r] - f * b[col]
            }
         }
         r += 1
      }
      col += 1
   }
   b
}

fn _cvp_floor_int(any: x): int { int(math.floor(_cvp_float(x))) }

fn _cvp_ceil_int(any: x): int { int(math.ceil(_cvp_float(x))) }

fn _cvp_float_coord_options(list: coords): list {
   mut opts = []
   mut i = 0
   while(i < coords.len){
      def lo = _cvp_floor_int(coords[i])
      def hi = _cvp_ceil_int(coords[i])
      opts = opts.append(lo == hi ? [lo] : [lo, hi])
      i += 1
   }
   opts
}

fn _cvp_abs_float(any: x): f64 {
   def f = _cvp_float(x)
   f < 0.0 ? -f : f
}

fn _cvp_distance_sq(list: v, list: target): bigint {
   def diff = _vec_sub(target, v)
   _vec_dot(diff, diff)
}

fn _cvp_core_record(list: vector, any: distance_sq, int: nodes, bool: hit_limit, list: nodes_by_level): dict {
   {
      "vector": vector,
      "distance_sq": distance_sq,
      "nodes": nodes,
      "hit_limit": hit_limit,
      "nodes_by_level": nodes_by_level,
   }
}

fn _cvp_core_record_coeffs(list: vector, any: distance_sq, int: nodes, bool: hit_limit, list: nodes_by_level, list: coeffs): dict {
   {
      "vector": vector,
      "distance_sq": distance_sq,
      "nodes": nodes,
      "hit_limit": hit_limit,
      "nodes_by_level": nodes_by_level,
      "coeffs": coeffs,
   }
}

fn _cvp_babai_coeff_report(list: basis, list: target): dict {
   def n = basis.len
   def gso = _cvp_gso_mu_profile(basis)
   def bstar = gso.get("b_star", [])
   def norms = gso.get("norms_sq", [])
   mut coeffs = _zero_vec(n)
   mut residual = clone(target)
   mut i = n - 1
   while(i >= 0){
      def den = _cvp_float(_at(norms, i, 0.0))
      def c = den == 0.0 ? 0 : _cvp_round_float(_cvp_float_target_dot(residual, _at(bstar, i, [])) / den)
      coeffs[i] = _z(c)
      if(c != 0){ residual = _vec_add_scaled(residual, basis[i], -c) }
      i -= 1
   }
   def vector = _vec_sub(target, residual)
   _cvp_set_fields(dict(12), [
         ["method", "babai-coefficients"], ["coeffs", coeffs], ["vector", vector],
         ["residual_target", residual], ["distance_sq", _cvp_distance_sq(vector, target)], ["gso_profile", gso],
   ])
}

fn _cvp_coeffs_max_abs(list: coeffs): bigint {
   mut best = Z(0)
   mut i = 0
   while(i < coeffs.len){
      def a = _abs_z(_at(coeffs, i, 0))
      if(a > best){ best = a }
      i += 1
   }
   best
}

fn _cvp_babai_residual_report(list: basis, list: target, int: max_loops=256, any: gso=nil): dict {
   mut shift = _zero_vec(target.len)
   mut residual_target = clone(target)
   mut loops = 0
   mut max_coeff = Z(0)
   mut last = nil
   mut stopped = false
   while(loops < max_loops && !stopped){
      def step = _cvp_babai_step_report(basis, residual_target, gso)
      last = step
      def coeffs = step.get("coeffs", [])
      max_coeff = _cvp_coeffs_max_abs(coeffs)
      if(max_coeff <= Z(1)){
         stopped = true
      } else {
         def v = step.get("vector", _zero_vec(target.len))
         if(_cvp_distance_sq(v, _zero_vec(target.len)) == Z(0)){
            stopped = true
         } else {
            shift = _vec_add(shift, v)
            residual_target = _vec_sub(residual_target, v)
            loops += 1
         }
      }
   }
   _cvp_set_fields(dict(14), [
         ["method", "babai-residual-target"], ["shift", shift], ["target", residual_target], ["loops", loops],
         ["max_coeff", max_coeff], ["hit_limit", loops >= max_loops], ["last_babai", last],
         ["changed", !_vec_equal(shift, _zero_vec(target.len))],
   ])
}

fn _cvp_center_vector(list: basis, list: coeffs, int: dim): list {
   mut out = _zero_vec(dim)
   mut i = 0
   while(i < basis.len){
      def c = _z(_at(coeffs, i, 0))
      if(c != Z(0)){ out = _vec_add_scaled(out, basis[i], c) }
      i += 1
   }
   out
}

fn _cvp_projection_seed(list: basis, list: target, int: passes=2): dict {
   mut best = _zero_vec(target.len)
   mut best_norm = _cvp_distance_sq(best, target)
   mut pass = 0
   while(pass < passes){
      mut changed = false
      mut i = 0
      while(i < basis.len){
         def row = basis[i]
         def den = _vec_dot(row, row)
         if(den != Z(0)){
            def diff = _vec_sub(target, best)
            def q = _round_div(_vec_dot(diff, row), den)
            if(q != Z(0)){
               def cand = _vec_add_scaled(best, row, q)
               def cand_norm = _cvp_distance_sq(cand, target)
               if(cand_norm < best_norm){
                  best = cand
                  best_norm = cand_norm
                  changed = true
               }
            }
         }
         i += 1
      }
      if(!changed){ break }
      pass += 1
   }
   {"vector": best, "distance_sq": best_norm}
}

fn _cvp_babai_gso_report_with_profile(list: basis, list: target, any: gso): dict {
   def n = basis.len
   if(!is_dict(gso)){
      _cvp_trace("babai-gso:fallback", {"reason": "non-dict-gso", "rows": n})
      return _cvp_babai_coeff_report(basis, target)
   }
   def bstar = gso.get("b_star", [])
   def mu = gso.get("mu", [])
   def norms = gso.get("norms_sq", [])
   if(!is_list(bstar) || !is_list(mu) || !is_list(norms) || bstar.len != n || mu.len != n || norms.len != n){
      _cvp_trace("babai-gso:fallback", {
            "reason": "profile-shape",
            "rows": n,
            "bstar": is_list(bstar) ? bstar.len : -1,
            "mu": is_list(mu) ? mu.len : -1,
            "norms": is_list(norms) ? norms.len : -1
      })
      return _cvp_babai_coeff_report(basis, target)
   }
   mut coords = _cvp_gso_target_coords(target, gso)
   if(coords.len != n){
      _cvp_trace("babai-gso:fallback", {"reason": "coords-shape", "rows": n, "coords": coords.len})
      return _cvp_babai_coeff_report(basis, target)
   }
   mut coeffs = _zero_vec(n)
   mut i = n - 1
   while(i >= 0){
      def c = _cvp_round_float(_at(coords, i, 0.0))
      coeffs[i] = _z(c)
      mut j = 0
      while(j < i){
         coords[j] = _cvp_float(_at(coords, j, 0.0)) - _cvp_float(_cvp_mu_get(mu, i, j)) * _cvp_float(c)
         j += 1
      }
      i -= 1
   }
   def vector = _cvp_center_vector(basis, coeffs, target.len)
   {
      "method": "gso-backsubstitution-babai",
      "coeffs": coeffs,
      "vector": vector,
      "residual_target": _vec_sub(target, vector),
      "distance_sq": _cvp_distance_sq(vector, target),
      "gso_profile": gso,
   }
}

fn _cvp_babai_gso_report(list: basis, list: target): dict {
   _cvp_babai_gso_report_with_profile(basis, target, _cvp_gso_mu_profile(basis))
}

fn _cvp_nearest_seed_with_profile(list: basis, list: target, any: gso): list {
   if(!is_list(basis) || basis.len == 0 || !is_dict(gso)){ return [] }
   def n = basis.len
   mut coords = _cvp_gso_target_coords(target, gso)
   def mu = gso.get("mu", [])
   mut coeffs = _zero_vec(n)
   mut i = n - 1
   while(i >= 0){
      def c = _cvp_round_float(_at(coords, i, 0.0))
      coeffs[i] = _z(c)
      mut j = 0
      while(j < i){
         coords[j] = _cvp_float(_at(coords, j, 0.0)) - _cvp_float(_cvp_mu_get(mu, i, j)) * _cvp_float(c)
         j += 1
      }
      i -= 1
   }
   _cvp_center_vector(basis, coeffs, target.len)
}

fn _cvp_nearest_seed(list: basis, list: target): list {
   if(!is_list(basis) || basis.len == 0){ return [] }
   _cvp_nearest_seed_with_profile(basis, target, _cvp_gso_mu_profile(basis))
}

fn _cvp_babai_step_report(list: basis, list: target, any: gso=nil): dict {
   gso == nil ? _cvp_babai_gso_report(basis, target) : _cvp_babai_gso_report_with_profile(basis, target, gso)
}

fn _cvp_local_descent_report(list: basis, list: target, list: seed, any: seed_norm, int: max_steps): dict {
   mut best = seed
   mut best_norm = seed_norm
   mut steps = 0
   mut passes = 0
   mut improved = true
   while(improved && steps < max_steps){
      improved = false
      mut i = 0
      while(i < basis.len && steps < max_steps){
         def plus = _vec_add_scaled(best, basis[i], Z(1))
         def plus_norm = _cvp_distance_sq(plus, target)
         steps += 1
         mut chosen = best
         mut chosen_norm = best_norm
         if(plus_norm < chosen_norm){
            chosen = plus
            chosen_norm = plus_norm
         }
         if(steps < max_steps){
            def minus = _vec_add_scaled(best, basis[i], Z(-1))
            def minus_norm = _cvp_distance_sq(minus, target)
            steps += 1
            if(minus_norm < chosen_norm){
               chosen = minus
               chosen_norm = minus_norm
            }
         }
         if(steps < max_steps){
            def row = basis[i]
            def diff = _vec_sub(target, best)
            def den = _vec_dot(row, row)
            def q = den == Z(0) ? Z(0) : _round_div(_vec_dot(diff, row), den)
            mut dq = -1
            while(dq <= 1 && steps < max_steps){
               def k = q + Z(dq)
               if(k != Z(0)){
                  def candidate = _vec_add_scaled(best, row, k)
                  def candidate_norm = _cvp_distance_sq(candidate, target)
                  steps += 1
                  if(candidate_norm < chosen_norm){
                     chosen = candidate
                     chosen_norm = candidate_norm
                  }
               }
               dq += 1
            }
         }
         if(chosen_norm < best_norm){
            best = chosen
            best_norm = chosen_norm
            improved = true
         }
         i += 1
      }
      if(!improved && steps < max_steps){
         mut pi = 0
         while(pi < basis.len && !improved && steps < max_steps){
            mut pj = pi + 1
            while(pj < basis.len && !improved && steps < max_steps){
               mut best_pair = best
               mut best_pair_norm = best_norm
               def u = basis[pi]
               def v = basis[pj]
               def diff = _vec_sub(target, best)
               def uu = _vec_dot(u, u)
               def uv = _vec_dot(u, v)
               def vv = _vec_dot(v, v)
               def du = _vec_dot(diff, u)
               def dv = _vec_dot(diff, v)
               def det = uu * vv - uv * uv
               def qa = det == Z(0) ? Z(0) : _round_div(du * vv - dv * uv, det)
               def qb = det == Z(0) ? Z(0) : _round_div(dv * uu - du * uv, det)
               mut da = -1
               while(da <= 1 && steps < max_steps){
                  mut db = -1
                  while(db <= 1 && steps < max_steps){
                     def ka = qa + Z(da)
                     def kb = qb + Z(db)
                     if(ka != Z(0) || kb != Z(0)){
                        def candidate = _vec_add_scaled(_vec_add_scaled(best, u, ka), v, kb)
                        def candidate_norm = _cvp_distance_sq(candidate, target)
                        steps += 1
                        if(candidate_norm < best_pair_norm){
                           best_pair = candidate
                           best_pair_norm = candidate_norm
                        }
                     }
                     db += 1
                  }
                  da += 1
               }
               if(best_pair_norm < best_norm){
                  best = best_pair
                  best_norm = best_pair_norm
                  improved = true
               }
               pj += 1
            }
            pi += 1
         }
      }
      passes += 1
   }
   _cvp_set_fields(dict(10), [
         ["method", "local-row-descent"], ["vector", best], ["distance_sq", best_norm], ["steps", steps],
         ["passes", passes], ["hit_limit", steps >= max_steps], ["improved", best_norm < seed_norm],
   ])
}

fn _cvp_round_float_coords(list: coords): list {
   mut out = []
   mut i = 0
   while(i < coords.len){
      out = out.append(_z(_cvp_round_float(coords[i])))
      i += 1
   }
   out
}

fn _cvp_gso_target_coords(list: target, any: gso): list {
   def bstar = gso.get("b_star", [])
   def norms = gso.get("norms_sq", [])
   mut out = []
   mut i = 0
   while(i < bstar.len){
      def den = _at(norms, i, Z(0))
      out = out.append(_cvp_float(den) == 0.0 ? 0.0 : _cvp_float_target_dot(target, bstar[i]) / _cvp_float(den))
      i += 1
   }
   out
}

fn _cvp_center_tail(int: idx, list: coeffs, list: coords, any: mu, int: n): f64 {
   mut center = float(coords[idx])
   mut j = idx + 1
   while(j < n){
      def int: cj = coeffs[j]
      if(cj != 0){
         center -= float(cj) * _cvp_float(_cvp_mu_get(mu, j, idx))
      }
      j += 1
   }
   center
}

fn _cvp_mu_flat(list: mu, int: n): list<f64> {
   mut list<f64>: out = list(n * n)
   mut i = 0
   while(i < n){
      mut j = 0
      while(j < n){
         out[i * n + j] = j < i ? _cvp_float(_cvp_mu_get(mu, i, j)) : 0.0
         j += 1
      }
      i += 1
   }
   out
}

fn _cvp_center_tail_flat(int: idx, list: coeffs, list: coords, list<f64>: mu, int: n): f64 {
   mut center = float(coords[idx])
   mut j = idx + 1
   while(j < n){
      def int: cj = coeffs[j]
      if(cj != 0){
         center -= float(cj) * mu[j * n + idx]
      }
      j += 1
   }
   center
}

fn _cvp_search_bound_float(any: best_norm, any: radius_sq): f64 {
   def b = _cvp_float(best_norm)
   def r = _cvp_float(radius_sq)
   r > 0.0 && r < b ? r : b
}

fn _cvp_early_accept_norm(any: seed_norm): any {
   def num_s = env("NY_CVP_EARLY_ACCEPT_NUM")
   def den_s = env("NY_CVP_EARLY_ACCEPT_DEN")
   if(!is_str(num_s) || !is_str(den_s) || num_s.len == 0 || den_s.len == 0){ return nil }
   def num = Z(atoi(num_s))
   def den = Z(atoi(den_s))
   den <= Z(0) ? nil : (Z(seed_norm) * num) / den
}

fn _cvp_gso_cvp_radius_sq(list: norms): any {
   mut s = 0.0
   mut i = 1
   while(i < norms.len){
      s += _cvp_float(norms[i])
      i += 1
   }
   s <= 0.0 ? Z(0) : s
}

fn _cvp_gso_enum_dfs(int: idx, list: basis, list: target, list: coords, any: mu, list: norms, list: coeffs, f64: partial_cost, list: best_v, any: best_norm, any: radius_sq, any: early_norm, list: best_coeffs, int: nodes, int: max_nodes, list: nodes_by_level): dict {
   nodes_by_level
   if(nodes >= max_nodes){ return _cvp_core_record_coeffs(best_v, best_norm, nodes, true, nodes_by_level, best_coeffs) }
   nodes += 1
   if(idx < 0){
      def cur = _cvp_center_vector(basis, coeffs, target.len)
      def norm = _cvp_distance_sq(cur, target)
      if(norm < best_norm){
         def rec = _cvp_core_record_coeffs(cur, norm, nodes, false, nodes_by_level, coeffs)
         if(early_norm != nil && norm <= early_norm){ return rec.set("early_accept", true) }
         return rec
      }
      return _cvp_core_record_coeffs(best_v, best_norm, nodes, false, nodes_by_level, best_coeffs)
   }
   mut state = _cvp_core_record_coeffs(best_v, best_norm, nodes, false, nodes_by_level, best_coeffs)
   def active_bound = _cvp_search_bound_float(state.get("distance_sq", best_norm), radius_sq)
   def remaining = active_bound - partial_cost
   if(remaining < 0.0){ return state }
   def norm_i = max(0.000001, _cvp_float(_at(norms, idx, Z(0))))
   def center = _cvp_center_tail(idx, coeffs, coords, mu, basis.len)
   def bound = int(math.floor(math.sqrt(max(0.0, remaining) / norm_i))) + 1
   def rounded = _cvp_round_float(center)
   mut oi = 0
   def total = bound * 2 + 1
   while(oi < total){
      def c = oi == 0 ? rounded : (rounded + ((oi % 2 == 1) ? ((oi + 1) / 2) : (0 - (oi / 2))))
      def delta = _cvp_float(c) - center
      def next_cost = partial_cost + norm_i * delta * delta
      if(next_cost <= _cvp_search_bound_float(state.get("distance_sq", best_norm), radius_sq)){
         def next_coeffs = coeffs.set(idx, c)
         state = _cvp_gso_enum_dfs(idx - 1, basis, target, coords, mu, norms, next_coeffs, next_cost, state.get("vector"), state.get("distance_sq"), radius_sq, early_norm, state.get("coeffs", best_coeffs), state.get("nodes"), max_nodes, nodes_by_level)
         if(state.get("hit_limit", false) || state.get("early_accept", false)){ return state }
      }
      oi += 1
   }
   state
}

fn _cvp_gso_enum_dfs_mut_flat(int: idx, list: basis, list: target, list: coords, list<f64>: mu, list<f64>: norms, list: coeffs, f64: partial_cost, any: radius_sq, any: early_norm, list: state): bool {
   def nodes = int(state[2])
   if(nodes >= int(state[6])){
      state[3] = true
      return true
   }
   state[2] = nodes + 1
   if(idx < 0){
      def cur = _cvp_center_vector(basis, coeffs, target.len)
      def norm = _cvp_distance_sq(cur, target)
      if(norm < state[1]){
         state[0] = cur
         state[1] = norm
         state[7] = _cvp_search_bound_float(norm, radius_sq)
         state[4] = clone(coeffs)
         if(early_norm != nil && norm <= early_norm){
            state[5] = true
            return true
         }
      }
      return false
   }
   def active_bound = float(state[7])
   def remaining = active_bound - partial_cost
   if(remaining < 0.0){ return false }
   def norm_i = max(0.000001, norms[idx])
   def center = _cvp_center_tail_flat(idx, coeffs, coords, mu, basis.len)
   def bound = int(math.floor(math.sqrt(max(0.0, remaining) / norm_i))) + 1
   def rounded = _cvp_round_float(center)
   mut oi = 0
   def total = bound * 2 + 1
   while(oi < total){
      def c = oi == 0 ? rounded : (rounded + ((oi % 2 == 1) ? ((oi + 1) / 2) : (0 - (oi / 2))))
      def delta = float(c) - center
      def next_cost = partial_cost + norm_i * delta * delta
      if(next_cost <= float(state[7])){
         coeffs[idx] = c
         if(_cvp_gso_enum_dfs_mut_flat(idx - 1, basis, target, coords, mu, norms, coeffs, next_cost, radius_sq, early_norm, state)){ return true }
      }
      oi += 1
   }
   false
}

fn _cvp_gso_enumerate_report(list: basis, list: target, list: seed, any: seed_norm, int: max_nodes, any: gso_override=nil): dict {
   def enum_basis = _cvp_enum_order_basis(basis)
   def reordered = enum_basis.len == basis.len && enum_basis != basis
   def gso = (!reordered && gso_override != nil) ? gso_override : _cvp_gso_mu_profile(enum_basis)
   def coords = _cvp_gso_target_coords(target, gso)
   def coeff_seed = _cvp_target_coords_solve(enum_basis, seed)
   def coeffs = _zero_vec(enum_basis.len)
   def nodes_by_level = _nodes_vec(enum_basis.len)
   def radius_sq = _cvp_gso_cvp_radius_sq(gso.get("norms_sq", []))
   def early_norm = _cvp_early_accept_norm(seed_norm)
   mut state = nil
   if(enum_basis.len >= 16){
      state = [seed, seed_norm, 0, false, coeff_seed, false, max_nodes, _cvp_search_bound_float(seed_norm, radius_sq)]
      def list<f64>: mu_flat = _cvp_mu_flat(gso.get("mu"), enum_basis.len)
      def list<f64>: norms_f = _cvp_list_norms_f64(gso.get("norms_sq", []), enum_basis.len)
      _cvp_gso_enum_dfs_mut_flat(enum_basis.len - 1, enum_basis, target, coords, mu_flat, norms_f, coeffs, 0.0, radius_sq, early_norm, state)
   } else {
      def rec = _cvp_gso_enum_dfs(enum_basis.len - 1, enum_basis, target, coords, gso.get("mu"), gso.get("norms_sq", []), coeffs, 0.0, seed, seed_norm, radius_sq, early_norm, coeff_seed, 0, max_nodes, nodes_by_level)
      state = [rec.get("vector", seed), rec.get("distance_sq", seed_norm), rec.get("nodes", 0), rec.get("hit_limit", false), rec.get("coeffs", coeff_seed), rec.get("early_accept", false), max_nodes]
   }
   mut out = _cvp_set_fields(dict(16), [
         ["method", "gso-centered-enumeration"],
         ["basis_order", reordered ? "row-norm-desc" : "input"],
         ["coefficient_order", "schnorr-euchner-inline"],
         ["search_radius_sq", radius_sq],
         ["target_coordinates_gso", coords],
         ["vector", state[0]],
         ["coeffs", state[4]],
         ["distance_sq", state[1]],
         ["nodes", state[2]],
         ["nodes_by_level", nodes_by_level],
         ["hit_limit", state[3]],
         ["early_accept", state[5]],
         ["early_accept_norm", early_norm],
         ["gso_profile", gso],
   ])
   out
}

fn _cvp_list_default_target(int: n): list {
   mut out = []
   mut i = 0
   while(i < n){
      out = out.append(0.0)
      i += 1
   }
   out
}

fn _cvp_list_target(any: target_coords, int: n): list {
   if(!is_list(target_coords)){ return _cvp_list_default_target(n) }
   mut out = []
   mut i = 0
   while(i < n){
      out = out.append(_cvp_float(_at(target_coords, i, 0.0)))
      i += 1
   }
   out
}

fn _cvp_list_norms_f64(list: norms, int: n): list<f64> {
   mut list<f64>: out = list(n)
   mut i = 0
   while(i < n){
      out[i] = _cvp_float(_at(norms, i, 0.0))
      i += 1
   }
   out
}

fn _cvp_list_count_record(int: count, int: nodes, bool: hit_limit): list {
   [count, nodes, hit_limit]
}

fn _cvp_isqrt_int(int: n): int {
   if(n <= 0){ return 0 }
   if(n <= 9007199254740991){
      mut x = int(math.sqrt(float(n)))
      while((x + 1) <= n / (x + 1)){ x += 1 }
      while(x > n / x){ x -= 1 }
      return x
   }
   mut hi = 1
   while(hi <= n / hi){ hi *= 2 }
   mut lo = hi / 2
   while(lo + 1 < hi){
      def mid = (lo + hi) / 2
      if(mid <= n / mid){ lo = mid } else { hi = mid }
   }
   lo
}

@inline
fn _cvp_round_div_nearest(int: x, int: scale): int {
   if(x >= 0){ return(x + scale / 2) / scale }
   0 - ((0 - x + scale / 2) / scale)
}

fn _cvp_list_gso_dfs_f64(int: idx, list<f64>: target_coords, list<int>: coeffs, any: mu, list<f64>: norms, f64: partial_cost, f64: radius, int: count, int: nodes, int: max_solutions, int: max_nodes): list {
   if(nodes >= max_nodes || count >= max_solutions){ return _cvp_list_count_record(count, nodes, true) }
   nodes += 1
   if(idx < 0){
      if(partial_cost <= radius){ count += 1 }
      return _cvp_list_count_record(count, nodes, count >= max_solutions)
   }
   def rem = radius - partial_cost
   if(rem < 0.0){ return _cvp_list_count_record(count, nodes, false) }
   mut center = float(target_coords[idx])
   mut j = idx + 1
   while(j < coeffs.len){
      def cj = int(coeffs[j])
      if(cj != 0){ center -= float(cj) * _cvp_float(_cvp_mu_get(mu, j, idx)) }
      j += 1
   }
   def norm_i = max(0.000001, float(norms[idx]))
   def width = math.sqrt(max(0.0, rem) / norm_i)
   def lo = int(math.ceil(center - width - 0.000000000001))
   def hi = int(math.floor(center + width + 0.000000000001))
   mut state = _cvp_list_count_record(count, nodes, false)
   mut c = lo
   while(c <= hi){
      coeffs[idx] = c
      def delta = float(c) - center
      state = _cvp_list_gso_dfs_f64(idx - 1, target_coords, coeffs, mu, norms, partial_cost + norm_i * delta * delta, radius, int(state[0]), int(state[1]), max_solutions, max_nodes)
      if(state[2]){ return state }
      c += 1
   }
   coeffs[idx] = 0
   state
}

fn _cvp_list_scaled_int(any: x, int: scale): list {
   def fx = _cvp_float(x) * float(scale)
   def zi = _cvp_round_float(fx)
   [_cvp_abs_float(fx - float(zi)) <= 0.0000001, zi]
}

fn _cvp_i64buf(int: n): ptr {
   def bytes = max(1, n) * 8
   def p = malloc(bytes)
   if(!p){ panic("cvp list count buffer allocation failed") }
   memset(p, 0, bytes)
   p
}

fn _cvp_list_fixed_setup_ptr(list<f64>: target_coords, any: mu, list<f64>: norms, f64: radius, int: scale): list {
   def n = target_coords.len
   def target = _cvp_i64buf(n)
   def norm_buf = _cvp_i64buf(n)
   def dep_counts = _cvp_i64buf(n)
   def dep_idx = _cvp_i64buf(n * n)
   def dep_mu = _cvp_i64buf(n * n)
   mut ok = true
   mut i = 0
   while(i < n){
      def ts = _cvp_list_scaled_int(target_coords[i], scale)
      if(!ts[0]){ ok = false }
      store64(target, int(ts[1]), i * 8)
      def ns = _cvp_list_scaled_int(norms[i], 1)
      if(!ns[0] || int(ns[1]) <= 0){ ok = false }
      store64(norm_buf, int(ns[1]), i * 8)
      mut j = 0
      while(j < i){
         def ms = _cvp_list_scaled_int(_cvp_mu_get(mu, i, j), scale)
         if(!ms[0]){ ok = false }
         def ms_i = int(ms[1])
         if(ms_i != 0){
            def int: c = load64(dep_counts, i * 8)
            store64(dep_idx, j, (i * n + c) * 8)
            store64(dep_mu, ms_i, (i * n + c) * 8)
            store64(dep_counts, c + 1, i * 8)
         }
         j += 1
      }
      i += 1
   }
   def r = _cvp_list_scaled_int(radius, scale * scale)
   if(!r[0]){ ok = false }
   [ok, target, norm_buf, nil, int(r[1]), dep_counts, dep_idx, dep_mu]
}

fn _cvp_list_sub_center_delta(ptr: centers, ptr: dep_counts, ptr: dep_idx, ptr: dep_mu, int: n, int: idx, int: c): any {
   if(c == 0){ return nil }
   def int: dep_count = load64(dep_counts, idx * 8)
   if(dep_count == 1){
      def int: off = load64(dep_idx, idx * n * 8) * 8
      store64(centers, load64(centers, off) - c * load64(dep_mu, idx * n * 8), off)
      return nil
   }
   if(dep_count == 2){
      def base = idx * n * 8
      def int: off0 = load64(dep_idx, base) * 8
      def int: off1 = load64(dep_idx, base + 8) * 8
      store64(centers, load64(centers, off0) - c * load64(dep_mu, base), off0)
      store64(centers, load64(centers, off1) - c * load64(dep_mu, base + 8), off1)
      return nil
   }
   mut k = 0
   while(k < dep_count){
      def int: col = load64(dep_idx, (idx * n + k) * 8)
      def off = col * 8
      store64(centers, load64(centers, off) - c * load64(dep_mu, (idx * n + k) * 8), off)
      k += 1
   }
   nil
}

fn _cvp_list_add_center_delta(ptr: centers, ptr: dep_counts, ptr: dep_idx, ptr: dep_mu, int: n, int: idx, int: c): any {
   if(c == 0){ return nil }
   def int: dep_count = load64(dep_counts, idx * 8)
   if(dep_count == 1){
      def int: off = load64(dep_idx, idx * n * 8) * 8
      store64(centers, load64(centers, off) + c * load64(dep_mu, idx * n * 8), off)
      return nil
   }
   if(dep_count == 2){
      def base = idx * n * 8
      def int: off0 = load64(dep_idx, base) * 8
      def int: off1 = load64(dep_idx, base + 8) * 8
      store64(centers, load64(centers, off0) + c * load64(dep_mu, base), off0)
      store64(centers, load64(centers, off1) + c * load64(dep_mu, base + 8), off1)
      return nil
   }
   mut k = 0
   while(k < dep_count){
      def int: col = load64(dep_idx, (idx * n + k) * 8)
      def off = col * 8
      store64(centers, load64(centers, off) + c * load64(dep_mu, (idx * n + k) * 8), off)
      k += 1
   }
   nil
}

fn _cvp_list_gso_leaf_count_ptr(ptr: centers, ptr: norms, int: scale, int: partial_cost, int: radius_scaled, ptr: counters, int: max_solutions, int: max_nodes): bool {
   def int: nodes0 = load64(counters, 8)
   if(nodes0 >= max_nodes){
      store64(counters, 1, 16)
      return true
   }
   def int: nodes1 = nodes0 + 1
   store64(counters, nodes1, 8)
   def rem = radius_scaled - partial_cost
   if(rem < 0){ return false }
   def int: center = load64(centers, 0)
   def int: norm_raw = load64(norms, 0)
   def int: norm_i = norm_raw > 1 ? norm_raw : 1
   mut int: lo = 0
   mut int: hi = 0
   if(norm_i <= 1000000000 && rem <= norm_i * scale * scale * 64){
      def int: mid = _cvp_round_div_nearest(center, scale)
      lo = mid
      while(true){
         def int: d = lo * scale - center
         if(norm_i * d * d > rem){ break }
         lo -= 1
      }
      lo += 1
      hi = mid + 1
      while(true){
         def int: d = hi * scale - center
         if(norm_i * d * d > rem){ break }
         hi += 1
      }
      hi -= 1
   } else {
      def int: max_delta = _cvp_isqrt_int(rem / norm_i)
      def int: lo_num = center - max_delta
      lo = lo_num / scale
      if(lo_num - lo * scale != 0 && lo_num > 0){ lo += 1 }
      def int: hi_num = center + max_delta
      hi = hi_num / scale
      if(hi_num - hi * scale != 0 && hi_num < 0){ hi -= 1 }
   }
   if(hi < lo){ return false }
   def int: span = hi - lo + 1
   def int: count0 = load64(counters, 0)
   def int: count_room = max_solutions - count0
   def int: node_room = max_nodes - nodes1
   mut int: take = span
   if(count_room < take){ take = count_room }
   if(node_room < take){ take = node_room }
   store64(counters, count0 + take, 0)
   store64(counters, nodes1 + take, 8)
   if(take < span || count0 + take >= max_solutions || nodes1 + take >= max_nodes){
      store64(counters, 1, 16)
      return true
   }
   false
}

fn _cvp_list_count_leaf_span_len(int: center, int: norm_i, int: scale, int: rem): int {
   mut int: lo = 0
   mut int: hi = 0
   if(norm_i <= 1000000000 && rem <= norm_i * scale * scale * 64){
      def int: mid = _cvp_round_div_nearest(center, scale)
      lo = mid
      while(true){
         def int: d = lo * scale - center
         if(norm_i * d * d > rem){ break }
         lo -= 1
      }
      lo += 1
      hi = mid + 1
      while(true){
         def int: d = hi * scale - center
         if(norm_i * d * d > rem){ break }
         hi += 1
      }
      hi -= 1
   } else {
      def int: max_delta = _cvp_isqrt_int(rem / norm_i)
      def int: lo_num = center - max_delta
      lo = lo_num / scale
      if(lo_num - lo * scale != 0 && lo_num > 0){ lo += 1 }
      def int: hi_num = center + max_delta
      hi = hi_num / scale
      if(hi_num - hi * scale != 0 && hi_num < 0){ hi -= 1 }
   }
   hi >= lo ? hi - lo + 1 : 0
}

fn _cvp_list_gso_count_idx1_core(int: lo, int: hi, int: center0_base, int: center1, int: norm1, int: dep_count, int: mu0, ptr: norms, int: scale, int: partial_cost, int: radius_scaled, ptr: counters, int: max_solutions, int: max_nodes): bool {
   def int: norm0_raw = load64(norms, 0)
   def int: norm0 = norm0_raw > 1 ? norm0_raw : 1
   mut int: count_local = load64(counters, 0)
   mut int: nodes_local = load64(counters, 8)
   mut int: c = lo
   while(c <= hi){
      if(nodes_local >= max_nodes){
         store64(counters, count_local, 0)
         store64(counters, nodes_local, 8)
         store64(counters, 1, 16)
         return true
      }
      nodes_local += 1
      def int: center0 = c == 0 || dep_count <= 0 ? center0_base : center0_base - c * mu0
      def int: delta1 = c * scale - center1
      def int: rem0 = radius_scaled - partial_cost - norm1 * delta1 * delta1
      if(rem0 >= 0){
         def int: span = _cvp_list_count_leaf_span_len(center0, norm0, scale, rem0)
         if(span > 0){
            def int: count_room = max_solutions - count_local
            def int: node_room = max_nodes - nodes_local
            mut int: take = span
            if(count_room < take){ take = count_room }
            if(node_room < take){ take = node_room }
            count_local += take
            nodes_local += take
            if(take < span || count_local >= max_solutions || nodes_local >= max_nodes){
               store64(counters, count_local, 0)
               store64(counters, nodes_local, 8)
               store64(counters, 1, 16)
               return true
            }
         }
      }
      c += 1
   }
   store64(counters, count_local, 0)
   store64(counters, nodes_local, 8)
   false
}

fn _cvp_list_gso_count_idx1_ptr(int: lo, int: hi, int: center1, int: norm1, ptr: centers, ptr: dep_counts, ptr: dep_idx, ptr: dep_mu, ptr: norms, int: n, int: scale, int: partial_cost, int: radius_scaled, ptr: counters, int: max_solutions, int: max_nodes): bool {
   def int: dep_count = load64(dep_counts, 8)
   def int: dep_base = n * 8
   def int: mu0 = dep_count > 0 ? load64(dep_mu, dep_base) : 0
   _cvp_list_gso_count_idx1_core(lo, hi, load64(centers, 0), center1, norm1, dep_count, mu0, norms, scale, partial_cost, radius_scaled, counters, max_solutions, max_nodes)
}

fn _cvp_list_gso_count_idx1_state_ptr(ptr: centers, ptr: dep_counts, ptr: dep_idx, ptr: dep_mu, ptr: norms, int: n, int: scale, int: partial_cost, int: radius_scaled, ptr: counters, int: max_solutions, int: max_nodes): bool {
   def int: nodes0 = load64(counters, 8)
   if(nodes0 >= max_nodes){
      store64(counters, 1, 16)
      return true
   }
   store64(counters, nodes0 + 1, 8)
   def rem = radius_scaled - partial_cost
   if(rem < 0){ return false }
   def int: center1 = load64(centers, 8)
   def int: norm_raw = load64(norms, 8)
   def int: norm1 = norm_raw > 1 ? norm_raw : 1
   mut int: lo = 0
   mut int: hi = 0
   if(norm1 <= 1000000000 && rem <= norm1 * scale * scale * 64){
      def int: mid = _cvp_round_div_nearest(center1, scale)
      lo = mid
      while(true){
         def int: d = lo * scale - center1
         if(norm1 * d * d > rem){ break }
         lo -= 1
      }
      lo += 1
      hi = mid + 1
      while(true){
         def int: d = hi * scale - center1
         if(norm1 * d * d > rem){ break }
         hi += 1
      }
      hi -= 1
   } else {
      def int: max_delta = _cvp_isqrt_int(rem / norm1)
      def int: lo_num = center1 - max_delta
      lo = lo_num / scale
      if(lo_num - lo * scale != 0 && lo_num > 0){ lo += 1 }
      def int: hi_num = center1 + max_delta
      hi = hi_num / scale
      if(hi_num - hi * scale != 0 && hi_num < 0){ hi -= 1 }
   }
   _cvp_list_gso_count_idx1_ptr(lo, hi, center1, norm1, centers, dep_counts, dep_idx, dep_mu, norms, n, scale, partial_cost, radius_scaled, counters, max_solutions, max_nodes)
}

fn _cvp_list_gso_count_idx2_ptr(int: lo, int: hi, int: center2, int: norm2, ptr: centers, ptr: dep_counts, ptr: dep_idx, ptr: dep_mu, ptr: norms, int: n, int: scale, int: partial_cost, int: radius_scaled, ptr: counters, int: max_solutions, int: max_nodes): bool {
   def int: dep_count = load64(dep_counts, 16)
   def int: dep_base = 2 * n * 8
   if(dep_count <= 0){
      mut int: c0 = lo
      while(c0 <= hi){
         def int: delta0 = c0 * scale - center2
         if(_cvp_list_gso_count_idx1_state_ptr(centers, dep_counts, dep_idx, dep_mu, norms, n, scale, partial_cost + norm2 * delta0 * delta0, radius_scaled, counters, max_solutions, max_nodes)){ return true }
         c0 += 1
      }
      return false
   }
   if(dep_count == 1){
      def int: off = load64(dep_idx, dep_base) * 8
      def int: mu0 = load64(dep_mu, dep_base)
      mut int: c1 = lo
      while(c1 <= hi){
         if(c1 == 0){
            if(_cvp_list_gso_count_idx1_state_ptr(centers, dep_counts, dep_idx, dep_mu, norms, n, scale, partial_cost + norm2 * center2 * center2, radius_scaled, counters, max_solutions, max_nodes)){ return true }
            c1 += 1
            continue
         }
         store64(centers, load64(centers, off) - c1 * mu0, off)
         def int: delta1 = c1 * scale - center2
         if(_cvp_list_gso_count_idx1_state_ptr(centers, dep_counts, dep_idx, dep_mu, norms, n, scale, partial_cost + norm2 * delta1 * delta1, radius_scaled, counters, max_solutions, max_nodes)){ return true }
         store64(centers, load64(centers, off) + c1 * mu0, off)
         c1 += 1
      }
      return false
   }
   mut int: c = lo
   while(c <= hi){
      if(c == 0){
         if(_cvp_list_gso_count_idx1_state_ptr(centers, dep_counts, dep_idx, dep_mu, norms, n, scale, partial_cost + norm2 * center2 * center2, radius_scaled, counters, max_solutions, max_nodes)){ return true }
         c += 1
         continue
      }
      mut k = 0
      while(k < dep_count){
         def int: pos = dep_base + k * 8
         def int: off = load64(dep_idx, pos) * 8
         store64(centers, load64(centers, off) - c * load64(dep_mu, pos), off)
         k += 1
      }
      def int: delta = c * scale - center2
      if(_cvp_list_gso_count_idx1_state_ptr(centers, dep_counts, dep_idx, dep_mu, norms, n, scale, partial_cost + norm2 * delta * delta, radius_scaled, counters, max_solutions, max_nodes)){ return true }
      k = 0
      while(k < dep_count){
         def int: pos = dep_base + k * 8
         def int: off = load64(dep_idx, pos) * 8
         store64(centers, load64(centers, off) + c * load64(dep_mu, pos), off)
         k += 1
      }
      c += 1
   }
   false
}

fn _cvp_list_gso_dfs_fixed_sparse_ptr(int: idx, ptr: centers, ptr: dep_counts, ptr: dep_idx, ptr: dep_mu, ptr: norms, int: n, int: scale, int: partial_cost, int: radius_scaled, ptr: counters, int: max_solutions, int: max_nodes): bool {
   if(idx == 0){ return _cvp_list_gso_leaf_count_ptr(centers, norms, scale, partial_cost, radius_scaled, counters, max_solutions, max_nodes) }
   def int: nodes0 = load64(counters, 8)
   if(nodes0 >= max_nodes){
      store64(counters, 1, 16)
      return true
   }
   def nodes1 = nodes0 + 1
   store64(counters, nodes1, 8)
   if(idx < 0){
      if(partial_cost <= radius_scaled){
         def int: count0 = load64(counters, 0)
         def count1 = count0 + 1
         store64(counters, count1, 0)
         if(count1 >= max_solutions){
            store64(counters, 1, 16)
            return true
         }
      }
      return false
   }
   def rem = radius_scaled - partial_cost
   if(rem < 0){ return false }
   def int: center = load64(centers, idx * 8)
   def int: norm_raw = load64(norms, idx * 8)
   def int: norm_i = norm_raw > 1 ? norm_raw : 1
   mut int: lo = 0
   mut int: hi = 0
   if(norm_i <= 1000000000 && rem <= norm_i * scale * scale * 64){
      def int: mid = _cvp_round_div_nearest(center, scale)
      lo = mid
      while(true){
         def int: d = lo * scale - center
         if(norm_i * d * d > rem){ break }
         lo -= 1
      }
      lo += 1
      hi = mid + 1
      while(true){
         def int: d = hi * scale - center
         if(norm_i * d * d > rem){ break }
         hi += 1
      }
      hi -= 1
   } else {
      def int: max_delta = _cvp_isqrt_int(rem / norm_i)
      def int: lo_num = center - max_delta
      lo = lo_num / scale
      if(lo_num - lo * scale != 0 && lo_num > 0){ lo += 1 }
      def int: hi_num = center + max_delta
      hi = hi_num / scale
      if(hi_num - hi * scale != 0 && hi_num < 0){ hi -= 1 }
   }
   def int: dep_count = load64(dep_counts, idx * 8)
   def int: dep_base = idx * n * 8
   if(idx == 1){
      return _cvp_list_gso_count_idx1_ptr(lo, hi, center, norm_i, centers, dep_counts, dep_idx, dep_mu, norms, n, scale, partial_cost, radius_scaled, counters, max_solutions, max_nodes)
   }
   if(idx == 2){
      return _cvp_list_gso_count_idx2_ptr(lo, hi, center, norm_i, centers, dep_counts, dep_idx, dep_mu, norms, n, scale, partial_cost, radius_scaled, counters, max_solutions, max_nodes)
   }
   if(dep_count <= 0){
      mut int: c0 = lo
      while(c0 <= hi){
         def int: delta0 = c0 * scale - center
         if(_cvp_list_gso_dfs_fixed_sparse_ptr(idx - 1, centers, dep_counts, dep_idx, dep_mu, norms, n, scale, partial_cost + norm_i * delta0 * delta0, radius_scaled, counters, max_solutions, max_nodes)){ return true }
         c0 += 1
      }
      return false
   }
   if(dep_count == 1){
      def int: off = load64(dep_idx, dep_base) * 8
      def int: mu0 = load64(dep_mu, dep_base)
      mut int: c1 = lo
      while(c1 <= hi){
         if(c1 == 0){
            if(_cvp_list_gso_dfs_fixed_sparse_ptr(idx - 1, centers, dep_counts, dep_idx, dep_mu, norms, n, scale, partial_cost + norm_i * center * center, radius_scaled, counters, max_solutions, max_nodes)){ return true }
            c1 += 1
            continue
         }
         store64(centers, load64(centers, off) - c1 * mu0, off)
         def int: delta1 = c1 * scale - center
         if(_cvp_list_gso_dfs_fixed_sparse_ptr(idx - 1, centers, dep_counts, dep_idx, dep_mu, norms, n, scale, partial_cost + norm_i * delta1 * delta1, radius_scaled, counters, max_solutions, max_nodes)){ return true }
         store64(centers, load64(centers, off) + c1 * mu0, off)
         c1 += 1
      }
      return false
   }
   if(dep_count == 2){
      def int: off0 = load64(dep_idx, dep_base) * 8
      def int: off1 = load64(dep_idx, dep_base + 8) * 8
      def int: mu0 = load64(dep_mu, dep_base)
      def int: mu1 = load64(dep_mu, dep_base + 8)
      mut int: c2 = lo
      while(c2 <= hi){
         if(c2 == 0){
            if(_cvp_list_gso_dfs_fixed_sparse_ptr(idx - 1, centers, dep_counts, dep_idx, dep_mu, norms, n, scale, partial_cost + norm_i * center * center, radius_scaled, counters, max_solutions, max_nodes)){ return true }
            c2 += 1
            continue
         }
         store64(centers, load64(centers, off0) - c2 * mu0, off0)
         store64(centers, load64(centers, off1) - c2 * mu1, off1)
         def int: delta2 = c2 * scale - center
         if(_cvp_list_gso_dfs_fixed_sparse_ptr(idx - 1, centers, dep_counts, dep_idx, dep_mu, norms, n, scale, partial_cost + norm_i * delta2 * delta2, radius_scaled, counters, max_solutions, max_nodes)){ return true }
         store64(centers, load64(centers, off0) + c2 * mu0, off0)
         store64(centers, load64(centers, off1) + c2 * mu1, off1)
         c2 += 1
      }
      return false
   }
   if(dep_count == 3){
      def int: off0 = load64(dep_idx, dep_base) * 8
      def int: off1 = load64(dep_idx, dep_base + 8) * 8
      def int: off2 = load64(dep_idx, dep_base + 16) * 8
      def int: mu0 = load64(dep_mu, dep_base)
      def int: mu1 = load64(dep_mu, dep_base + 8)
      def int: mu2 = load64(dep_mu, dep_base + 16)
      mut int: c3 = lo
      while(c3 <= hi){
         if(c3 == 0){
            if(_cvp_list_gso_dfs_fixed_sparse_ptr(idx - 1, centers, dep_counts, dep_idx, dep_mu, norms, n, scale, partial_cost + norm_i * center * center, radius_scaled, counters, max_solutions, max_nodes)){ return true }
            c3 += 1
            continue
         }
         store64(centers, load64(centers, off0) - c3 * mu0, off0)
         store64(centers, load64(centers, off1) - c3 * mu1, off1)
         store64(centers, load64(centers, off2) - c3 * mu2, off2)
         def int: delta3 = c3 * scale - center
         if(_cvp_list_gso_dfs_fixed_sparse_ptr(idx - 1, centers, dep_counts, dep_idx, dep_mu, norms, n, scale, partial_cost + norm_i * delta3 * delta3, radius_scaled, counters, max_solutions, max_nodes)){ return true }
         store64(centers, load64(centers, off0) + c3 * mu0, off0)
         store64(centers, load64(centers, off1) + c3 * mu1, off1)
         store64(centers, load64(centers, off2) + c3 * mu2, off2)
         c3 += 1
      }
      return false
   }
   mut int: c = lo
   while(c <= hi){
      if(c == 0){
         if(_cvp_list_gso_dfs_fixed_sparse_ptr(idx - 1, centers, dep_counts, dep_idx, dep_mu, norms, n, scale, partial_cost + norm_i * center * center, radius_scaled, counters, max_solutions, max_nodes)){ return true }
         c += 1
         continue
      }
      mut k = 0
      while(k < dep_count){
         def int: pos = dep_base + k * 8
         def int: off = load64(dep_idx, pos) * 8
         store64(centers, load64(centers, off) - c * load64(dep_mu, pos), off)
         k += 1
      }
      def int: delta = c * scale - center
      if(_cvp_list_gso_dfs_fixed_sparse_ptr(idx - 1, centers, dep_counts, dep_idx, dep_mu, norms, n, scale, partial_cost + norm_i * delta * delta, radius_scaled, counters, max_solutions, max_nodes)){ return true }
      k = 0
      while(k < dep_count){
         def int: pos = dep_base + k * 8
         def int: off = load64(dep_idx, pos) * 8
         store64(centers, load64(centers, off) + c * load64(dep_mu, pos), off)
         k += 1
      }
      c += 1
   }
   false
}

fn cvp_list_gso_count_report(list: basis, any: radius_sq=32.5, any: target_coords=nil, int: max_solutions=999999, int: max_nodes=10000000): dict {
   "Count GSO-coordinate CVP enumeration hits inside `radius_sq`, using list-CVP evaluator semantics."
   def t0 = ticks()
   if(!is_list(basis) || basis.len == 0){
      return _cvp_finish_report({"method": "gso-list-cvp-count", "ok": false, "reason": "empty basis"}, t0)
   }
   def n = basis.len
   def gso = _cvp_gso_mu_profile(basis)
   def list<f64>: target = _cvp_list_target(target_coords, n)
   def list<f64>: norms = _cvp_list_norms_f64(gso.get("norms_sq", []), n)
   def fixed = _cvp_list_fixed_setup_ptr(target, gso.get("mu"), norms, _cvp_float(radius_sq), 10000)
   mut state = _cvp_list_count_record(0, 0, false)
   if(fixed[0]){
      def counters = _cvp_i64buf(3)
      _cvp_list_gso_dfs_fixed_sparse_ptr(n - 1, fixed[1], fixed[5], fixed[6], fixed[7], fixed[2], n, 10000, 0, int(fixed[4]), counters, max_solutions, max_nodes)
      def int: final_count = load64(counters, 0)
      def int: final_nodes = load64(counters, 8)
      state = _cvp_list_count_record(final_count, final_nodes, final_count >= max_solutions || final_nodes >= max_nodes)
      free(counters)
   } else {
      def list<int>: coeffs = _zero_vec(n)
      state = _cvp_list_gso_dfs_f64(n - 1, target, coeffs, gso.get("mu"), norms, 0.0, _cvp_float(radius_sq), 0, 0, max_solutions, max_nodes)
   }
   free(fixed[1], fixed[2], fixed[5], fixed[6], fixed[7])
   mut out = _cvp_set_fields(dict(16), [
         ["method", "gso-list-cvp-count"],
         ["ok", !state[2]],
         ["rows", n],
         ["cols", basis[0].len],
         ["radius_sq", radius_sq],
         ["target_coordinates_gso", target],
         ["count", state[0]],
         ["max_solutions", max_solutions],
         ["nodes", state[1]],
         ["max_nodes", max_nodes],
         ["nodes_by_level", _nodes_vec(n)],
         ["hit_limit", state[2]],
         ["coefficient_order", "interval-gso-center"],
         ["numeric_kernel", fixed[0] ? "fixed-point-int-bound-gso-count" : "float-gso-count"],
         ["center_kernel", fixed[0] ? "sparse-fixed-mu" : "dense-float-mu"],
         ["fixed_point_scale", fixed[0] ? 10000 : 0],
         ["gso_profile", gso],
   ])
   _cvp_finish_report(out, t0)
}

fn _cvp_beam_width(): int {
   def v = _cvp_env_int("NY_CVP_BEAM_WIDTH", 192)
   max(8, v)
}

fn _cvp_beam_bound(): int {
   def v = _cvp_env_int("NY_CVP_BEAM_BOUND", 8)
   max(1, v)
}

fn _cvp_beam_enabled(): bool {
   def v = env("NY_CVP_BEAM")
   is_str(v) && (v == "1" || v == "true" || v == "yes")
}

fn _cvp_beam_push(list: states, list: cand, int: width): list {
   if(states.len < width){ return states.append(cand) }
   mut worst_i = 0
   mut worst = _cvp_float(states[0][0])
   mut i = 1
   while(i < states.len){
      def cost = _cvp_float(states[i][0])
      if(cost > worst){
         worst = cost
         worst_i = i
      }
      i += 1
   }
   if(_cvp_float(cand[0]) < worst){ states[worst_i] = cand }
   states
}

fn _cvp_gso_beam_report(list: basis, list: target, list: seed, any: seed_norm, int: max_nodes): dict {
   def n = basis.len
   mut out = dict(14)
   if(n <= 0){
      out = out.set("method", "gso-beam-cvp")
      out = out.set("found", false)
      return out
   }
   def width = _cvp_beam_width()
   def coeff_cap = _cvp_beam_bound()
   def gso = _cvp_gso_mu_profile(basis)
   def coords = _cvp_gso_target_coords(target, gso)
   mut states = [[0.0, _zero_vec(n)]]
   mut nodes = 0
   mut hit_limit = false
   mut idx = n - 1
   while(idx >= 0 && !hit_limit){
      mut next_states = []
      mut si = 0
      while(si < states.len && !hit_limit){
         def st = states[si]
         def coeffs = st[1]
         def partial = _cvp_float(st[0])
         def norm_i = max(0.000001, _cvp_float(_at(gso.get("norms_sq", []), idx, Z(0))))
         def center = _cvp_center_tail(idx, coeffs, coords, gso.get("mu"), n)
         def bound = min(coeff_cap, int(math.floor(math.sqrt(max(0.0, _cvp_float(seed_norm) - partial) / norm_i))) + 1)
         def rounded = _cvp_round_float(center)
         mut oi = 0
         def total = bound * 2 + 1
         while(oi < total && !hit_limit){
            def c = oi == 0 ? rounded : (rounded + ((oi % 2 == 1) ? ((oi + 1) / 2) : (0 - (oi / 2))))
            def delta = _cvp_float(c) - center
            def cost = partial + norm_i * delta * delta
            if(cost <= _cvp_float(seed_norm)){
               def next_coeffs = coeffs.set(idx, c)
               next_states = _cvp_beam_push(next_states, [cost, next_coeffs], width)
               nodes += 1
               if(nodes >= max_nodes){ hit_limit = true }
            }
            oi += 1
         }
         si += 1
      }
      if(next_states.len == 0){ hit_limit = true } else { states = next_states }
      idx -= 1
   }
   mut best_v = seed
   mut best_norm = seed_norm
   mut best_coeffs = _zero_vec(n)
   mut i = 0
   while(i < states.len){
      def coeffs = states[i][1]
      def v = _cvp_center_vector(basis, coeffs, target.len)
      def nr = _cvp_distance_sq(v, target)
      if(nr < best_norm){
         best_v = v
         best_norm = nr
         best_coeffs = coeffs
      }
      i += 1
   }
   out = out.set("method", "gso-beam-cvp")
   out = out.set("found", best_norm < seed_norm)
   out = out.set("vector", best_v)
   out = out.set("distance_sq", best_norm)
   out = out.set("coeffs", best_coeffs)
   out = out.set("nodes", nodes)
   out = out.set("hit_limit", hit_limit)
   out = out.set("beam_width", width)
   out = out.set("coeff_bound", coeff_cap)
   out = out.set("states", states.len)
   out = out.set("target_coordinates_gso", coords)
   out
}

fn _cvp_embedding_report(list: basis, list: target, any: scale=1): dict {
   def dim = target.len
   def m = _z(scale)
   mut rows = []
   mut i = 0
   while(i < basis.len){
      mut row = []
      mut j = 0
      while(j < dim){
         row = row.append(_z(_at(basis[i], j, 0)))
         j += 1
      }
      row = row.append(Z(0))
      rows = rows.append(row)
      i += 1
   }
   mut trow = []
   i = 0
   while(i < dim){
      trow = trow.append(-_z(target[i]))
      i += 1
   }
   trow = trow.append(m)
   rows = rows.append(trow)
   def red = matrix._matrix_data(lllcore.lll(matrix.Matrix(rows), 0.99, "ny"))
   mut best_v = []
   mut best_diff = []
   mut best_norm = nil
   i = 0
   while(i < red.len){
      def row = red[i]
      def last = _z(_at(row, dim, 0))
      if(last == m || last == -m){
         mut diff = []
         mut j = 0
         while(j < dim){
            diff = diff.append(_z(_at(row, j, 0)))
            j += 1
         }
         def cand = last == m ? _vec_add(target, diff) : _vec_sub(target, diff)
         def nr = _cvp_distance_sq(cand, target)
         if(best_norm == nil || nr < best_norm){
            best_norm = nr
            best_v = cand
            best_diff = diff
         }
      }
      i += 1
   }
   mut out = dict(12)
   out = out.set("method", "kannan-embedding-reduction")
   out = out.set("scale", m)
   out = out.set("rows", rows.len)
   out = out.set("cols", dim + 1)
   out = out.set("found", best_norm != nil)
   out = out.set("vector", best_v)
   out = out.set("difference", best_diff)
   out = out.set("distance_sq", best_norm == nil ? nil : best_norm)
   out = out.set("reduced_basis", red)
   out
}

fn _cvp_best_embedding_report(list: basis, list: target, any: current_norm): dict {
   def scales = [1, 2, 4, 8, 16, 32, 64, 128]
   mut best = nil
   mut i = 0
   while(i < scales.len){
      def rep = _cvp_embedding_report(basis, target, scales[i])
      if(rep.get("found", false)){
         def nr = rep.get("distance_sq", current_norm)
         if(best == nil || nr < best.get("distance_sq", current_norm)){ best = rep }
      }
      i += 1
   }
   best == nil ? _cvp_embedding_report(basis, target, 1) : best
}

fn _cvp_enum_dfs(int: idx, list: basis, list: target, list: centers, int: coeff_bound, list: cur, list: best_v, any: best_norm, int: nodes, int: max_nodes, list: nodes_by_level): dict {
   nodes_by_level = _cvp_count_level(nodes_by_level, idx)
   if(nodes >= max_nodes){ return _cvp_core_record(best_v, best_norm, nodes, true, nodes_by_level) }
   if(idx >= basis.len){
      nodes += 1
      def norm = _cvp_distance_sq(cur, target)
      if(norm < best_norm){ return _cvp_core_record(cur, norm, nodes, false, nodes_by_level) }
      return _cvp_core_record(best_v, best_norm, nodes, false, nodes_by_level)
   }
   mut state = _cvp_core_record(best_v, best_norm, nodes, false, nodes_by_level)
   def opts = _cvp_coeff_order(_at(centers, idx, 0), coeff_bound)
   mut oi = 0
   while(oi < opts.len){
      def c = opts[oi]
      def next = (c == 0) ? cur : _vec_add_scaled(cur, basis[idx], c)
      state = _cvp_enum_dfs(idx + 1, basis, target, centers, coeff_bound, next, state.get("vector"), state.get("distance_sq"), state.get("nodes"), max_nodes, nodes_by_level)
      if(state.get("hit_limit", false)){ return state }
      oi += 1
   }
   state
}

fn _cvp_enum_radius_dfs(int: idx, int: radius_left, list: basis, list: target, list: centers, int: coeff_bound, list: cur, list: best_v, any: best_norm, int: nodes, int: max_nodes, list: nodes_by_level): dict {
   nodes_by_level = _cvp_count_level(nodes_by_level, idx)
   if(nodes >= max_nodes){ return _cvp_core_record(best_v, best_norm, nodes, true, nodes_by_level) }
   if(idx >= basis.len){
      if(radius_left != 0){ return _cvp_core_record(best_v, best_norm, nodes, false, nodes_by_level) }
      nodes += 1
      def norm = _cvp_distance_sq(cur, target)
      if(norm < best_norm){ return _cvp_core_record(cur, norm, nodes, false, nodes_by_level) }
      return _cvp_core_record(best_v, best_norm, nodes, false, nodes_by_level)
   }
   mut state = _cvp_core_record(best_v, best_norm, nodes, false, nodes_by_level)
   def offsets = _cvp_offset_order(coeff_bound)
   mut oi = 0
   while(oi < offsets.len){
      def off = int(offsets[oi])
      def cost = _cvp_abs_int(off)
      if(cost <= radius_left){
         def c = _at(centers, idx, 0) + off
         def next = (c == 0) ? cur : _vec_add_scaled(cur, basis[idx], c)
         state = _cvp_enum_radius_dfs(idx + 1, radius_left - cost, basis, target, centers, coeff_bound, next, state.get("vector"), state.get("distance_sq"), state.get("nodes"), max_nodes, nodes_by_level)
         if(state.get("hit_limit", false)){ return state }
      }
      oi += 1
   }
   state
}

fn _cvp_enum_offset_radius_dfs(int: idx, int: radius_left, list: basis, list: target, int: coeff_bound, list: cur, list: best_v, any: best_norm, int: nodes, int: max_nodes, list: nodes_by_level): dict {
   nodes_by_level = _cvp_count_level(nodes_by_level, idx)
   if(nodes >= max_nodes){ return _cvp_core_record(best_v, best_norm, nodes, true, nodes_by_level) }
   if(idx >= basis.len){
      if(radius_left != 0){ return _cvp_core_record(best_v, best_norm, nodes, false, nodes_by_level) }
      nodes += 1
      def norm = _cvp_distance_sq(cur, target)
      if(norm < best_norm){ return _cvp_core_record(cur, norm, nodes, false, nodes_by_level) }
      return _cvp_core_record(best_v, best_norm, nodes, false, nodes_by_level)
   }
   mut state = _cvp_core_record(best_v, best_norm, nodes, false, nodes_by_level)
   def offsets = _cvp_offset_order(coeff_bound)
   mut oi = 0
   while(oi < offsets.len){
      def off = int(offsets[oi])
      def cost = _cvp_abs_int(off)
      if(cost <= radius_left){
         def next = off == 0 ? cur : _vec_add_scaled(cur, basis[idx], off)
         state = _cvp_enum_offset_radius_dfs(idx + 1, radius_left - cost, basis, target, coeff_bound, next, state.get("vector"), state.get("distance_sq"), state.get("nodes"), max_nodes, nodes_by_level)
         if(state.get("hit_limit", false)){ return state }
      }
      oi += 1
   }
   state
}

fn _cvp_enum_gso_offset_radius_dfs(int: idx, int: radius_left, list: basis, list: target, list: offset_bounds, list: cur, list: best_v, any: best_norm, int: nodes, int: max_nodes, list: nodes_by_level): dict {
   nodes_by_level = _cvp_count_level(nodes_by_level, idx)
   if(nodes >= max_nodes){ return _cvp_core_record(best_v, best_norm, nodes, true, nodes_by_level) }
   nodes += 1
   if(idx >= basis.len){
      if(radius_left != 0){ return _cvp_core_record(best_v, best_norm, nodes, false, nodes_by_level) }
      def norm = _cvp_distance_sq(cur, target)
      if(norm < best_norm){ return _cvp_core_record(cur, norm, nodes, false, nodes_by_level) }
      return _cvp_core_record(best_v, best_norm, nodes, false, nodes_by_level)
   }
   mut state = _cvp_core_record(best_v, best_norm, nodes, false, nodes_by_level)
   def bound = min(int(_at(offset_bounds, idx, 1)), radius_left)
   def offsets = _cvp_offset_order(bound)
   mut oi = 0
   while(oi < offsets.len){
      def off = int(offsets[oi])
      def cost = _cvp_abs_int(off)
      if(cost <= radius_left){
         def next = off == 0 ? cur : _vec_add_scaled(cur, basis[idx], off)
         state = _cvp_enum_gso_offset_radius_dfs(idx + 1, radius_left - cost, basis, target, offset_bounds, next, state.get("vector"), state.get("distance_sq"), state.get("nodes"), max_nodes, nodes_by_level)
         if(state.get("hit_limit", false)){ return state }
      }
      oi += 1
   }
   state
}

fn _cvp_coord_cube_dfs(int: idx, list: basis, list: target, list: options, list: cur, list: best_v, any: best_norm, int: nodes, int: max_nodes, list: nodes_by_level): dict {
   nodes_by_level = _cvp_count_level(nodes_by_level, idx)
   if(nodes >= max_nodes){ return _cvp_core_record(best_v, best_norm, nodes, true, nodes_by_level) }
   if(idx >= basis.len){
      nodes += 1
      def norm = _cvp_distance_sq(cur, target)
      if(norm < best_norm){ return _cvp_core_record(cur, norm, nodes, false, nodes_by_level) }
      return _cvp_core_record(best_v, best_norm, nodes, false, nodes_by_level)
   }
   mut state = _cvp_core_record(best_v, best_norm, nodes, false, nodes_by_level)
   def opts = options[idx]
   mut oi = 0
   while(oi < opts.len){
      def c = _z(opts[oi])
      def next = c == Z(0) ? cur : _vec_add_scaled(cur, basis[idx], c)
      state = _cvp_coord_cube_dfs(idx + 1, basis, target, options, next, state.get("vector"), state.get("distance_sq"), state.get("nodes"), max_nodes, nodes_by_level)
      if(state.get("hit_limit", false)){ return state }
      oi += 1
   }
   state
}

fn _cvp_coordinate_cube_report(list: basis, list: target, int: max_nodes): any {
   def coords_f = _cvp_target_coords_float_solve(basis, target)
   if(coords_f == nil){ return nil }
   def options = _cvp_float_coord_options(coords_f)
   def nodes_by_level = _nodes_vec(basis.len)
   def seed_coeffs = _cvp_round_float_coords(coords_f)
   def seed = _cvp_center_vector(basis, seed_coeffs, target.len)
   def seed_norm = _cvp_distance_sq(seed, target)
   def state = _cvp_coord_cube_dfs(0, basis, target, options, _zero_vec(target.len), seed, seed_norm, 0, max_nodes, nodes_by_level)
   {
      "coords_float": coords_f,
      "seed_coeffs": seed_coeffs,
      "options": options,
      "vector": state.get("vector", seed),
      "distance_sq": state.get("distance_sq", seed_norm),
      "nodes": state.get("nodes", 0),
      "hit_limit": state.get("hit_limit", false),
      "nodes_by_level": state.get("nodes_by_level", nodes_by_level),
   }
}

fn build_lattice(list: mat, list: lb, list: ub): list {
   "Build scaled CVP lattice metadata for bounded inequalities.
   Returns [mat, target, scales], where `target` is interval midpoint and
   `scales` are per-column diagonal weights."
   def dims = _check_rect(mat)
   def n = int(dims[1])
   if(!is_list(lb) || !is_list(ub) || lb.len != n || ub.len != n){ panic("build_lattice: bounds must match matrix column count") }
   mut target = []
   mut widths = []
   mut k = Z(0)
   mut i = 0
   while(i < n){
      def lo = _z(lb[i])
      def hi = _z(ub[i])
      if(lo > hi){ panic("build_lattice: lower bound must be <= upper bound") }
      target = target.append((lo + hi) / 2)
      def w = hi - lo
      widths = widths.append(w)
      def aw = _abs_z(w)
      if(aw > k){ k = aw }
      i += 1
   }
   if(k == 0){ k = Z(1) }
   mut scales = []
   i = 0
   while(i < n){
      def w = _abs_z(widths[i])
      mut s = Z(0)
      if(w == 0){ s = k * _z(n) } else { s = k / w }
      if(s <= 0){ s = Z(1) }
      scales = scales.append(s)
      i += 1
   }
   [mat, target, scales]
}

fn cvp(list: basis, list: target, bool: reduce=true): list {
   "Closest vector helper. Uses Babai CVP; optionally LLL-reduces first."
   if(!is_list(basis) || basis.len == 0){ return [] }
   _cvp_trace("cvp:start", {"rows": basis.len, "cols": basis[0].len, "target_dim": target.len, "reduce": reduce})
   def work = reduce ? _cvp_reduce_basis(basis) : basis
   _cvp_trace("cvp:work", {"rows": work.len, "cols": work.len > 0 ? work[0].len : 0})
   def use_projection = work.len > 96 || (work.len > 64 && work[0].len > work.len)
   def seed = use_projection ? _cvp_projection_seed(work, target, 3).get("vector", []) : _cvp_nearest_seed(work, target)
   if(use_projection){ _cvp_trace("cvp:projection", {"seed_dim": seed.len, "distance_sq": seed.len > 0 ? _cvp_distance_sq(seed, target) : nil}) }
   _cvp_trace("cvp:done", {"seed_dim": seed.len, "distance_sq": seed.len > 0 ? _cvp_distance_sq(seed, target) : nil})
   seed
}

fn _cvp_reduction_selection(list: basis, list: target, bool: reduce, any: raw_seed_norm, any: raw_gso): dict {
   mut work = basis
   mut work_gso = nil
   mut input_gso_reused = false
   mut work_gso_source = "none"
   mut reduction_ms = nil
   mut reduction_report = {"selected": "input", "candidate_distance_sq": raw_seed_norm, "input_distance_sq": raw_seed_norm}
   if(reduce){
      _cvp_trace("reduce:start", basis.len)
      def reduce_t0 = ticks()
      def red_basis = _cvp_reduce_basis(basis)
      def red_gso = _cvp_gso_mu_profile(red_basis)
      def red_seed = _cvp_nearest_seed_with_profile(red_basis, target, red_gso)
      def red_seed_norm = _cvp_distance_sq(red_seed, target)
      reduction_ms = _cvp_elapsed_ms(reduce_t0)
      _cvp_trace("reduce:done", {"distance_sq": red_seed_norm, "rows": red_basis.len})
      reduction_report = {"selected": red_seed_norm < raw_seed_norm ? "reduced" : "input", "candidate_distance_sq": red_seed_norm, "input_distance_sq": raw_seed_norm, "candidate_rows": red_basis.len}
      if(red_seed_norm < raw_seed_norm){
         work = red_basis
         work_gso = red_gso
         work_gso_source = "reduced"
      } elif(raw_gso != nil){
         work_gso = raw_gso
         input_gso_reused = true
         work_gso_source = "reused-input"
      }
   } elif(raw_gso != nil){
      work_gso = raw_gso
      input_gso_reused = true
      work_gso_source = "reused-input"
   }
   {"work": work, "work_gso": work_gso, "input_gso_reused": input_gso_reused, "work_gso_source": work_gso_source, "reduction_ms": reduction_ms, "reduction": reduction_report}
}

fn cvp_babai_report(list: basis, list: target, bool: reduce=true): dict {
   "Return a Babai nearest-plane CVP report with coefficients, GSO profile, reduction choice, and exact distance verification."
   def t0 = ticks()
   mut out = dict(18)
   if(!is_list(basis) || basis.len == 0){
      out = _cvp_set_fields(out, [
            ["method", "babai-nearest-plane"],
            ["ok", false],
            ["reason", "empty basis"],
            ["nodes", 0],
            ["hit_limit", false],
      ])
      return _cvp_finish_report(out, t0)
   }
   def raw_gso = _cvp_gso_mu_profile(basis)
   def raw_babai = _cvp_babai_step_report(basis, target, raw_gso)
   def raw_seed_norm = raw_babai.get("distance_sq", Z(0))
   def selection = _cvp_reduction_selection(basis, target, reduce, raw_seed_norm, raw_gso)
   def work = selection.get("work", basis)
   mut work_gso = selection.get("work_gso", nil)
   mut work_gso_builds = 0
   mut work_gso_source = to_str(selection.get("work_gso_source", "none"))
   if(work_gso == nil){
      work_gso = _cvp_gso_mu_profile(work)
      work_gso_builds += 1
      work_gso_source = "work"
   }
   def step = _cvp_babai_step_report(work, target, work_gso)
   def vector = step.get("vector", _zero_vec(target.len))
   def distance_sq = _cvp_distance_sq(vector, target)
   out = _cvp_set_fields(out, [
         ["method", "babai-nearest-plane"],
         ["ok", vector.len > 0],
         ["reduced", reduce],
         ["vector", vector],
         ["distance_sq", distance_sq],
         ["verified", distance_sq == step.get("distance_sq", distance_sq)],
         ["coeffs", step.get("coeffs", [])],
         ["coeff_basis", selection.get("reduction", {"selected": "input"}).get("selected", "input")],
         ["basis", work],
         ["nodes", 0],
         ["hit_limit", false],
         ["target_coordinates", _cvp_target_coords_solve(work, target)],
         ["target_coordinates_gso", _cvp_gso_target_coords(target, work_gso)],
         ["gso_profile", work_gso],
         ["reduction", selection.get("reduction", {"selected": "input"})],
         ["raw_babai", raw_babai],
         ["input_gso_builds", 1],
         ["work_gso_builds", work_gso_builds],
         ["input_gso_reused", bool(selection.get("input_gso_reused", false))],
         ["work_gso_source", work_gso_source],
         ["phase_times", {"reduction_ms": selection.get("reduction_ms", nil)}],
   ])
   _cvp_finish_report(out, t0)
}

fn cvp_babai(list: basis, list: target, bool: reduce=true): list {
   "Return the Babai nearest-plane CVP vector."
   cvp_babai_report(basis, target, reduce).get("vector", [])
}

fn _cvp_direct_hit_finish(dict: out, any: t0, list: basis, list: target, int: direct_i, int: coeff_bound, int: max_nodes): dict {
   out = _cvp_enum_report_common(out.set("method", "gso-bounded-enumeration").set("direct_hit", true).set("direct_row", direct_i), false, coeff_bound, max_nodes, 0, _nodes_vec(basis.len), false, 0, Z(0), basis[direct_i], Z(0), true, basis)
   out = out.set("target_coordinates", _cvp_target_coords_solve(basis, target))
   out = out.set("gso_profile", _cvp_gso_profile(basis))
   _cvp_finish_report(out, t0)
}

fn _cvp_coordinate_cube_finish(dict: out, any: t0, list: basis, list: target, bool: reduce, int: coeff_bound, int: max_nodes, dict: cube, any: raw_seed_norm): dict {
   def vector = cube.get("vector", _zero_vec(target.len))
   def distance_sq = cube.get("distance_sq", raw_seed_norm)
   out = _cvp_enum_report_common({"method": "coordinate-cube-enumeration", "candidate_mode": "floor-ceil-coordinate-cube", "exhaustive_bound": false}, reduce, coeff_bound, max_nodes, cube.get("nodes", 0), cube.get("nodes_by_level", _nodes_vec(basis.len)), false, 0, raw_seed_norm, vector, distance_sq, _cvp_distance_sq(vector, target) == distance_sq, basis)
   _cvp_finish_report(out, t0)
}

fn _cvp_pre_reduce_exact_finish(dict: out, any: t0, list: basis, list: target, bool: reduce, int: coeff_bound, int: max_nodes, list: raw_seed): dict {
   out = _cvp_enum_report_common(out.set("method", "gso-bounded-enumeration").set("pre_reduce_exact", true), reduce, coeff_bound, max_nodes, 0, _nodes_vec(basis.len), false, 0, Z(0), raw_seed, Z(0), true, basis)
   out = out.set("target_coordinates", _cvp_target_coords_solve(basis, target))
   out = out.set("gso_profile", _cvp_gso_profile(basis))
   _cvp_finish_report(out, t0)
}

fn _cvp_initial_refinement(list: work, list: search_target, list: raw_seed, list: shift, any: emb, list: center_vec, any: center_norm, list: seed, any: seed_norm, int: max_nodes, dict: phase_times): dict {
   mut initial_v = seed
   mut initial_norm = seed_norm
   def raw_rel = _vec_sub(raw_seed, shift)
   def raw_rel_norm = _cvp_distance_sq(raw_rel, search_target)
   if(raw_rel_norm < initial_norm){
      initial_v = raw_rel
      initial_norm = raw_rel_norm
   }
   if(emb != nil && emb.get("found", false)){
      def emb_rel = _vec_sub(emb.get("vector", _zero_vec(search_target.len)), shift)
      def emb_rel_norm = _cvp_distance_sq(emb_rel, search_target)
      if(emb_rel_norm < initial_norm){
         initial_v = emb_rel
         initial_norm = emb_rel_norm
      }
   }
   _cvp_trace("residual-embedding:start", initial_norm)
   def residual_embedding_t0 = ticks()
   def residual_emb = _cvp_embedding_enabled(work.len) ? _cvp_best_embedding_report(work, search_target, initial_norm) : nil
   def _residual_embedding_ms = _cvp_elapsed_ms(residual_embedding_t0)
   _residual_embedding_ms
   _cvp_trace("residual-embedding:done", residual_emb == nil ? "nil" : {"found": residual_emb.get("found", false), "distance_sq": residual_emb.get("distance_sq", nil)})
   if(residual_emb != nil && residual_emb.get("found", false) && residual_emb.get("distance_sq", initial_norm) < initial_norm){
      initial_v = residual_emb.get("vector", initial_v)
      initial_norm = residual_emb.get("distance_sq", initial_norm)
   }
   if(center_norm < initial_norm){
      initial_v = center_vec
      initial_norm = center_norm
   }
   def descent_budget = max(work.len * 128, min(int(max_nodes), work.len * 512))
   _cvp_trace("descent:start", descent_budget)
   def descent_t0 = ticks()
   def descent = _cvp_descent_enabled(work.len) ? _cvp_local_descent_report(work, search_target, initial_v, initial_norm, descent_budget) : {"method": "local-row-descent", "skipped": true, "vector": initial_v, "distance_sq": initial_norm, "steps": 0, "passes": 0, "hit_limit": false, "improved": false}
   def _descent_ms = _cvp_elapsed_ms(descent_t0)
   _descent_ms
   _cvp_trace("descent:done", {"steps": descent.get("steps", 0), "distance_sq": descent.get("distance_sq", initial_norm), "hit_limit": descent.get("hit_limit", false)})
   if(descent.get("distance_sq", initial_norm) < initial_norm){
      initial_v = descent.get("vector", initial_v)
      initial_norm = descent.get("distance_sq", initial_norm)
   }
   {"vector": initial_v, "distance_sq": initial_norm, "residual_embedding": residual_emb, "descent": descent, "phase_times": phase_times}
}

fn _cvp_raw_seed_stage(list: basis, list: target, int: max_nodes, dict: phase_times): dict {
   _cvp_trace("coordinate-cube:start", basis.len)
   def cube_t0 = ticks()
   def cube = _cvp_coordinate_cube_enabled(basis.len) ? _cvp_coordinate_cube_report(basis, target, min(int(max_nodes), 4096)) : nil
   def _coordinate_cube_ms = _cvp_elapsed_ms(cube_t0)
   _coordinate_cube_ms
   _cvp_trace("coordinate-cube:done", cube == nil ? "nil" : {"nodes": cube.get("nodes", 0), "hit_limit": cube.get("hit_limit", false), "distance_sq": cube.get("distance_sq", nil)})
   mut raw_gso = nil
   mut input_gso_builds = 0
   mut raw_seed = []
   if(cube != nil){
      raw_seed = cube.get("vector", _zero_vec(target.len))
   } else {
      raw_gso = _cvp_gso_mu_profile(basis)
      input_gso_builds += 1
      raw_seed = _cvp_nearest_seed_with_profile(basis, target, raw_gso)
   }
   {"cube": cube, "raw_gso": raw_gso, "input_gso_builds": input_gso_builds, "raw_seed": raw_seed, "raw_seed_norm": _cvp_distance_sq(raw_seed, target), "phase_times": phase_times}
}

fn _cvp_embedding_seed_stage(list: basis, list: target, list: raw_seed_in, any: raw_seed_norm_in, dict: phase_times): dict {
   mut raw_seed = raw_seed_in
   mut raw_seed_norm = raw_seed_norm_in
   _cvp_trace("embedding:start", raw_seed_norm)
   def embedding_t0 = ticks()
   def emb = _cvp_embedding_enabled(basis.len) ? _cvp_best_embedding_report(basis, target, raw_seed_norm) : nil
   def _embedding_ms = _cvp_elapsed_ms(embedding_t0)
   _embedding_ms
   _cvp_trace("embedding:done", emb == nil ? "nil" : {"found": emb.get("found", false), "distance_sq": emb.get("distance_sq", nil)})
   if(emb != nil && emb.get("found", false) && emb.get("distance_sq", raw_seed_norm) < raw_seed_norm){
      raw_seed = emb.get("vector", raw_seed)
      raw_seed_norm = emb.get("distance_sq", raw_seed_norm)
   }
   {"embedding": emb, "raw_seed": raw_seed, "raw_seed_norm": raw_seed_norm, "phase_times": phase_times}
}

fn _cvp_work_stage(list: basis, list: target, bool: reduce, any: raw_seed_norm, any: raw_gso, dict: phase_times): dict {
   def selection = _cvp_reduction_selection(basis, target, reduce, raw_seed_norm, raw_gso)
   mut work = selection.get("work", basis)
   mut work_gso = selection.get("work_gso", nil)
   mut work_gso_builds = 0
   mut work_gso_source = to_str(selection.get("work_gso_source", "none"))
   if(selection.get("reduction_ms", nil) != nil){ selection.get("reduction_ms") }
   if(work_gso == nil){
      work_gso = _cvp_gso_mu_profile(work)
      work_gso_builds += 1
      work_gso_source = "work"
   }
   {
      "work": work, "work_gso": work_gso,
      "reduction": selection.get("reduction", {"selected": "input"}),
      "input_gso_reused": bool(selection.get("input_gso_reused", false)),
      "work_gso_source": work_gso_source,
      "work_gso_builds": work_gso_builds,
      "phase_times": phase_times
   }
}

fn _cvp_residual_stage(list: work, list: target, any: work_gso, dict: phase_times): dict {
   _cvp_trace("residual:start", work.len)
   def residual_t0 = ticks()
   def residual = _cvp_babai_residual_report(work, target, 256, work_gso)
   def _residual_ms = _cvp_elapsed_ms(residual_t0)
   _residual_ms
   _cvp_trace("residual:done", {"loops": residual.get("loops", 0), "hit_limit": residual.get("hit_limit", false), "max_coeff": residual.get("max_coeff", 0)})
   def search_target = residual.get("target", target)
   def seed = _cvp_nearest_seed_with_profile(work, search_target, work_gso)
   {
      "residual": residual,
      "shift": residual.get("shift", _zero_vec(target.len)),
      "search_target": search_target,
      "seed": seed,
      "seed_norm": _cvp_distance_sq(seed, search_target),
      "coords": _cvp_target_coords_solve(work, search_target),
      "nodes_by_level": _nodes_vec(work.len),
      "phase_times": phase_times
   }
}

fn _cvp_gso_enum_stage(list: work, list: search_target, list: initial_v_in, any: initial_norm_in, int: max_nodes, any: work_gso, dict: phase_times): dict {
   mut initial_v = initial_v_in
   mut initial_norm = initial_norm_in
   _cvp_trace("gso-enum:start", initial_norm)
   def gso_enum_t0 = ticks()
   def gso_enum = _cvp_gso_enumerate_report(work, search_target, initial_v, initial_norm, int(max_nodes), work_gso)
   def _gso_enum_ms = _cvp_elapsed_ms(gso_enum_t0)
   _gso_enum_ms
   _cvp_trace("gso-enum:done", {"nodes": gso_enum.get("nodes", 0), "hit_limit": gso_enum.get("hit_limit", false), "distance_sq": gso_enum.get("distance_sq", initial_norm), "search_radius_sq": gso_enum.get("search_radius_sq", nil)})
   mut improved = false
   if(gso_enum.get("distance_sq", initial_norm) < initial_norm){
      initial_v = gso_enum.get("vector", initial_v)
      initial_norm = gso_enum.get("distance_sq", initial_norm)
      improved = true
   }
   {"vector": initial_v, "distance_sq": initial_norm, "gso_enumeration": gso_enum, "improved": improved, "phase_times": phase_times}
}

fn _cvp_beam_stage(list: work, list: search_target, list: initial_v_in, any: initial_norm_in, int: max_nodes, dict: phase_times): dict {
   mut initial_v = initial_v_in
   mut initial_norm = initial_norm_in
   mut beam = nil
   if(work.len >= 24 && _cvp_beam_enabled()){
      _cvp_trace("beam:start", initial_norm)
      def beam_t0 = ticks()
      beam = _cvp_gso_beam_report(work, search_target, initial_v, initial_norm, min(int(max_nodes), _cvp_env_int("NY_CVP_BEAM_NODES", 80000)))
      def _beam_ms = _cvp_elapsed_ms(beam_t0)
      _beam_ms
      _cvp_trace("beam:done", {"nodes": beam.get("nodes", 0), "distance_sq": beam.get("distance_sq", initial_norm), "hit_limit": beam.get("hit_limit", false)})
      if(beam.get("distance_sq", initial_norm) < initial_norm){
         initial_v = beam.get("vector", initial_v)
         initial_norm = beam.get("distance_sq", initial_norm)
      }
   }
   {"vector": initial_v, "distance_sq": initial_norm, "beam": beam, "phase_times": phase_times}
}

fn _cvp_beam_return_report(
   dict: out, any: t0, bool: reduce, int: coeff_bound, int: max_nodes,
   list: work, list: target, list: shift, list: initial_v, any: initial_norm,
   any: reduction_report, any: residual, any: coords,
   int: input_gso_builds, int: work_gso_builds, bool: input_gso_reused, str: work_gso_source,
   any: descent, any: beam,
): any {
   if(beam == nil){ return nil }
   def beam_return_s = env("NY_CVP_BEAM_RETURN")
   if(!(is_str(beam_return_s) && (beam_return_s == "1" || beam_return_s == "true" || beam_return_s == "yes") && beam.get("found", false))){ return nil }
   def full_beam_v = _vec_add(shift, initial_v)
   def full_beam_dist = _cvp_distance_sq(full_beam_v, target)
   out = _cvp_enum_context_common(out.set("method", "gso-beam-cvp"), reduce, coeff_bound, max_nodes, beam.get("nodes", 0), _nodes_vec(work.len), beam.get("hit_limit", false), 0, initial_norm, full_beam_v, full_beam_dist, full_beam_dist == initial_norm, work, reduction_report, "beam", residual, coords, input_gso_builds, work_gso_builds, input_gso_reused, work_gso_source)
   out = out.set("coeffs", beam.get("coeffs", []))
   out = out.set("local_descent", descent)
   out = out.set("beam", beam)
   _cvp_finish_report(out, t0)
}

fn _cvp_gso_terminal_report(
   dict: out, any: t0, bool: reduce, int: coeff_bound, int: max_nodes,
   list: work, list: target, list: shift, list: initial_v, any: initial_norm,
   any: reduction_report, any: residual, any: coords,
   int: input_gso_builds, int: work_gso_builds, bool: input_gso_reused, str: work_gso_source,
   any: descent, any: beam, any: residual_emb, any: gso_enum, bool: gso_improved, dict: phase_times,
): any {
   if(gso_enum.get("hit_limit", false) && !gso_improved){
      def exhausted_v = _vec_add(shift, initial_v)
      def exhausted_dist = _cvp_distance_sq(exhausted_v, target)
      mut exhausted = _cvp_enum_context_common(dict(22).set("method", "gso-centered-enumeration"), reduce, coeff_bound, max_nodes, gso_enum.get("nodes", 0), gso_enum.get("nodes_by_level", _nodes_vec(work.len)), true, 0, initial_norm, exhausted_v, exhausted_dist, exhausted_dist == initial_norm, work, reduction_report, "budget-exhausted", residual, coords, input_gso_builds, work_gso_builds, input_gso_reused, work_gso_source)
      exhausted = exhausted.set("local_descent", descent)
      exhausted = exhausted.set("beam", beam)
      exhausted = exhausted.set("residual_embedding", residual_emb)
      exhausted = exhausted.set("gso_enumeration", gso_enum)
      exhausted = exhausted.set("phase_times", phase_times)
      return _cvp_finish_report(exhausted, t0)
   }
   if(!gso_enum.get("hit_limit", false) || gso_improved){
      def full_v = _vec_add(shift, initial_v)
      def full_dist = _cvp_distance_sq(full_v, target)
      out = _cvp_enum_context_common(out.set("method", "gso-centered-enumeration"), reduce, coeff_bound, max_nodes, gso_enum.get("nodes", 0), gso_enum.get("nodes_by_level", _nodes_vec(work.len)), gso_enum.get("hit_limit", false), 0, initial_norm, full_v, full_dist, full_dist == initial_norm, work, reduction_report, "projected-distance", residual, coords, input_gso_builds, work_gso_builds, input_gso_reused, work_gso_source)
      out = out.set("target_coordinates_gso", gso_enum.get("target_coordinates_gso", []))
      out = out.set("coeffs", gso_enum.get("coeffs", []))
      out = out.set("local_descent", descent)
      out = out.set("beam", beam)
      out = out.set("residual_embedding", residual_emb)
      out = out.set("gso_profile", gso_enum.get("gso_profile"))
      out = out.set("phase_times", phase_times)
      return _cvp_finish_report(out, t0)
   }
   nil
}

fn _cvp_offset_shell_finish(
   any: t0, bool: reduce, int: coeff_bound, int: max_nodes,
   list: work, list: search_target, list: target, list: shift, list: initial_v, any: initial_norm, any: report_radius_sq,
   list: center_vec, any: center_norm, any: reduction_report, any: residual, any: coords,
   int: input_gso_builds, int: work_gso_builds, bool: input_gso_reused, str: work_gso_source,
   any: descent, any: residual_emb, any: gso_enum, any: gso, dict: phase_times,
): dict {
   mut state = _cvp_core_record(initial_v, initial_norm, 0, false, _nodes_vec(work.len))
   mut radius = 0
   def offset_bounds = _cvp_gso_offset_bounds(gso.get("norms_sq", []), state.get("distance_sq", initial_norm), int(coeff_bound))
   def max_radius = _cvp_bounds_sum(offset_bounds)
   def offset_t0 = ticks()
   while(radius <= max_radius && !state.get("hit_limit", false)){
      if(radius == 0){ _cvp_trace("offset-shell:start", {"max_radius": max_radius, "offset_bound_estimate": _cvp_bounds_product(offset_bounds, int(max_nodes))}) }
      state = _cvp_enum_gso_offset_radius_dfs(0, radius, work, search_target, offset_bounds, center_vec, state.get("vector"), state.get("distance_sq"), state.get("nodes"), int(max_nodes), state.get("nodes_by_level"))
      radius += 1
   }
   def _offset_shell_ms = _cvp_elapsed_ms(offset_t0)
   _offset_shell_ms
   _cvp_trace("offset-shell:done", {"radius_levels": radius, "nodes": state.get("nodes", 0), "hit_limit": state.get("hit_limit", false), "distance_sq": state.get("distance_sq", initial_norm)})
   def full_state_v = _vec_add(shift, state.get("vector", initial_v))
   def full_state_dist = _cvp_distance_sq(full_state_v, target)
   mut offset_out = _cvp_enum_context_common(dict(24).set("method", "gso-offset-bounded-enumeration"), reduce, coeff_bound, max_nodes, state.get("nodes", 0), state.get("nodes_by_level", _nodes_vec(work.len)), state.get("hit_limit", false), radius, report_radius_sq, full_state_v, full_state_dist, full_state_dist == state.get("distance_sq", initial_norm), work, reduction_report, "gso-offset-radius-shell", residual, coords, input_gso_builds, work_gso_builds, input_gso_reused, work_gso_source)
   offset_out = offset_out.set("center_distance_sq", center_norm)
   offset_out = offset_out.set("local_descent", descent)
   offset_out = offset_out.set("residual_embedding", residual_emb)
   offset_out = offset_out.set("gso_enumeration", gso_enum)
   offset_out = offset_out.set("phase_times", phase_times)
   offset_out = offset_out.set("offset_bounds", offset_bounds)
   offset_out = offset_out.set("offset_bound_estimate", _cvp_bounds_product(offset_bounds, int(max_nodes)))
   offset_out = offset_out.set("gso_profile", gso)
   _cvp_finish_report(offset_out, t0)
}

fn cvp_enumerate_report(list: basis, list: target, bool: reduce=true, int: coeff_bound=2, int: max_nodes=200000): dict {
   "GSO-profiled CVP enumeration report.
   The search explores a Babai-centered coefficient window on the optionally
   reduced basis and records radius, target coordinates, node counts per level,
   and budget exhaustion."
   def t0 = ticks()
   _cvp_trace("start", {"rows": basis.len, "target_dim": target.len, "reduce": reduce, "max_nodes": max_nodes})
   mut out = dict(12)
   mut phase_times = dict(12)
   if(!is_list(basis) || basis.len == 0){
      out = out.set("method", "bounded-enumeration")
      out = out.set("ok", false)
      out = out.set("reason", "empty basis")
      return _cvp_finish_report(out, t0)
   }
   mut direct_i = 0
   while(direct_i < basis.len){
      if(_vec_equal(basis[direct_i], target)){
         return _cvp_direct_hit_finish(out, t0, basis, target, direct_i, coeff_bound, max_nodes)
      }
      direct_i += 1
   }
   def raw_stage = _cvp_raw_seed_stage(basis, target, max_nodes, phase_times)
   def cube = raw_stage.get("cube", nil)
   def raw_gso = raw_stage.get("raw_gso", nil)
   def input_gso_builds = int(raw_stage.get("input_gso_builds", 0))
   mut raw_seed = raw_stage.get("raw_seed", [])
   mut raw_seed_norm = raw_stage.get("raw_seed_norm", Z(0))
   phase_times = raw_stage.get("phase_times", phase_times)
   if(cube != nil && !cube.get("hit_limit", false)){
      return _cvp_coordinate_cube_finish(out, t0, basis, target, reduce, coeff_bound, max_nodes, cube, raw_seed_norm)
   }
   def embedded = _cvp_embedding_seed_stage(basis, target, raw_seed, raw_seed_norm, phase_times)
   def emb = embedded.get("embedding", nil)
   raw_seed = embedded.get("raw_seed", raw_seed)
   raw_seed_norm = embedded.get("raw_seed_norm", raw_seed_norm)
   phase_times = embedded.get("phase_times", phase_times)
   if(raw_seed_norm == Z(0)){
      return _cvp_pre_reduce_exact_finish(out, t0, basis, target, reduce, coeff_bound, max_nodes, raw_seed)
   }
   def work_stage = _cvp_work_stage(basis, target, reduce, raw_seed_norm, raw_gso, phase_times)
   def work = work_stage.get("work", basis)
   def work_gso = work_stage.get("work_gso", nil)
   def reduction_report = work_stage.get("reduction", {"selected": "input"})
   def input_gso_reused = bool(work_stage.get("input_gso_reused", false))
   def work_gso_source = to_str(work_stage.get("work_gso_source", "none"))
   def work_gso_builds = int(work_stage.get("work_gso_builds", 0))
   phase_times = work_stage.get("phase_times", phase_times)
   def residual_stage = _cvp_residual_stage(work, target, work_gso, phase_times)
   def residual = residual_stage.get("residual", dict())
   def shift = residual_stage.get("shift", _zero_vec(target.len))
   def search_target = residual_stage.get("search_target", target)
   def seed = residual_stage.get("seed", [])
   def dim = target.len
   def seed_norm = residual_stage.get("seed_norm", Z(0))
   def coords = residual_stage.get("coords", [])
   def nodes_by_level = residual_stage.get("nodes_by_level", _nodes_vec(work.len))
   phase_times = residual_stage.get("phase_times", phase_times)
   if(seed_norm == Z(0)){
      def full_seed = _vec_add(shift, seed)
      out = _cvp_enum_report_common(out.set("method", "gso-bounded-enumeration"), reduce, coeff_bound, max_nodes, 0, nodes_by_level, false, 0, seed_norm, full_seed, _cvp_distance_sq(full_seed, target), true, work)
      out = out.set("reduction", reduction_report)
      out = out.set("residualization", residual)
      out = out.set("target_coordinates", coords)
      out = out.set("gso_profile", work_gso)
      out = _cvp_attach_gso_reuse_report(out, input_gso_builds, work_gso_builds, input_gso_reused, work_gso_source)
      return _cvp_finish_report(out, t0)
   }
   def center_vec = _cvp_center_vector(work, coords, dim)
   def center_norm = _cvp_distance_sq(center_vec, search_target)
   def refinement = _cvp_initial_refinement(work, search_target, raw_seed, shift, emb, center_vec, center_norm, seed, seed_norm, max_nodes, phase_times)
   mut initial_v = refinement.get("vector", seed)
   mut initial_norm = refinement.get("distance_sq", seed_norm)
   def residual_emb = refinement.get("residual_embedding", nil)
   def descent = refinement.get("descent", dict())
   phase_times = refinement.get("phase_times", phase_times)
   def beam_stage = _cvp_beam_stage(work, search_target, initial_v, initial_norm, max_nodes, phase_times)
   initial_v = beam_stage.get("vector", initial_v)
   initial_norm = beam_stage.get("distance_sq", initial_norm)
   def beam = beam_stage.get("beam", nil)
   phase_times = beam_stage.get("phase_times", phase_times)
   def beam_return = _cvp_beam_return_report(out, t0, reduce, coeff_bound, max_nodes, work, target, shift, initial_v, initial_norm, reduction_report, residual, coords, input_gso_builds, work_gso_builds, input_gso_reused, work_gso_source, descent, beam)
   if(beam_return != nil){ return beam_return }
   def gso_stage = _cvp_gso_enum_stage(work, search_target, initial_v, initial_norm, max_nodes, work_gso, phase_times)
   initial_v = gso_stage.get("vector", initial_v)
   initial_norm = gso_stage.get("distance_sq", initial_norm)
   def gso_enum = gso_stage.get("gso_enumeration", dict())
   def gso_improved = bool(gso_stage.get("improved", false))
   phase_times = gso_stage.get("phase_times", phase_times)
   def terminal = _cvp_gso_terminal_report(out, t0, reduce, coeff_bound, max_nodes, work, target, shift, initial_v, initial_norm, reduction_report, residual, coords, input_gso_builds, work_gso_builds, input_gso_reused, work_gso_source, descent, beam, residual_emb, gso_enum, gso_improved, phase_times)
   if(terminal != nil){ return terminal }
   return _cvp_offset_shell_finish(t0, reduce, coeff_bound, max_nodes, work, search_target, target, shift, initial_v, initial_norm, seed_norm, center_vec, center_norm, reduction_report, residual, coords, input_gso_builds, work_gso_builds, input_gso_reused, work_gso_source, descent, residual_emb, gso_enum, work_gso, phase_times)
}

fn cvp_enumerate(list: basis, list: target, bool: reduce=true, int: coeff_bound=2, int: max_nodes=200000): list {
   "Return the bounded-enumeration CVP vector."
   if(!is_list(basis) || basis.len == 0){ return [] }
   def work = reduce ? _cvp_reduce_basis(basis) : basis
   def seed = _cvp_nearest_seed(work, target)
   if(seed.len == 0){ return [] }
   def coords = _cvp_target_coords_solve(work, target)
   def state = _cvp_enum_dfs(0, work, target, coords, int(coeff_bound), _zero_vec(target.len), seed, _cvp_distance_sq(seed, target), 0, int(max_nodes), _nodes_vec(work.len))
   state.get("vector", seed)
}

fn cvp_report(list: basis, list: target, bool: reduce=true, str: method="babai", int: coeff_bound=2, int: max_nodes=200000): dict {
   "Return a CVP report. method=\"babai\" is fast; method=\"enumerate\"
   uses bounded enumeration and records node-budget status."
   if(method == "enumerate" || method == "bounded"){ return cvp_enumerate_report(basis, target, reduce, coeff_bound, max_nodes) }
   cvp_babai_report(basis, target, reduce)
}

fn solve_inequality(list: mat, list: lb, list: ub): list {
   "Solve bounded linear inequality in the lll_cvp style.
   Returns a vector `y` expected to satisfy lb <= y <= ub(unchecked)."
   def b = build_lattice(mat, lb, ub)
   def base = b[0]
   def target = b[1]
   def scales = b[2]
   def scaled_basis = _scale_cols(base, scales)
   def scaled_target = _scale_vec(target, scales)
   _cvp_trace("solve-ineq:start", {"rows": base.len, "cols": base.len > 0 ? base[0].len : 0, "target_dim": target.len})
   def nearest = cvp(scaled_basis, scaled_target, true)
   _cvp_trace("solve-ineq:nearest", {"dim": nearest.len})
   _unscale_vec(nearest, scales)
}

fn solve_inequality_ex(list: mat, list: lb, list: ub): list {
   "Extended bounded inequality solve.
   Returns [solutions, reduced_basis], where solutions is a candidate list."
   def b = build_lattice(mat, lb, ub)
   def base = b[0]
   def target = b[1]
   def scales = b[2]
   def scaled_basis = _scale_cols(base, scales)
   def scaled_target = _scale_vec(target, scales)
   def reduced = _cvp_reduce_basis(scaled_basis)
   def nearest = _cvp_nearest_seed(reduced, scaled_target)
   [[_unscale_vec(nearest, scales)], _unscale_rows(reduced, scales)]
}

fn affine_cvp(list: base, list: lattice_basis, list: target): list {
   "Return `base + CVP(lattice_basis, target - base)`."
   if(!is_list(base) || base.len == 0){ return [] }
   def delta = _vec_sub(target, base)
   def nearest = cvp(lattice_basis, delta, true)
   _vec_add(base, nearest)
}

fn bounded(list: mat, list: lb, list: ub): list {
   "Compact alias for solve_inequality."
   solve_inequality(mat, lb, ub)
}

fn bounded_ex(list: mat, list: lb, list: ub): list {
   "Compact alias for solve_inequality_ex."
   solve_inequality_ex(mat, lb, ub)
}

fn _clone_matrix_rows(list: mat): list {
   mut out = []
   mut i = 0
   while(i < mat.len){
      out = out.append(clone(mat[i]))
      i += 1
   }
   out
}

fn _mat_max_abs(list: mat, int: num_var, int: num_ineq): bigint {
   mut max_element = Z(0)
   mut i = 0
   while(i < num_var){
      def row = mat[i]
      mut j = 0
      while(j < num_ineq){
         def av = _abs_z(row[j])
         if(av > max_element){ max_element = av }
         j += 1
      }
      i += 1
   }
   max_element
}

fn _bounds_max_diff(list: lb, list: ub, int: num_ineq): bigint {
   mut max_diff = Z(0)
   mut i = 0
   while(i < num_ineq){
      def d = _z(ub[i]) - _z(lb[i])
      if(d < 0){ panic("solve_weighted_bounds: lb must be <= ub") }
      if(d > max_diff){ max_diff = d }
      i += 1
   }
   max_diff <= 0 ? Z(1) : max_diff
}

fn _weight_for_interval(any: lo, any: hi, any: w_base, any: max_diff): bigint {
   def d = _z(hi) - _z(lo)
   mut wi = Z(1)
   if(d == 0){ wi = w_base } else { wi = max_diff / d }
   wi <= 0 ? Z(1) : wi
}

fn _midpoint_vec(list: lb, list: ub, int: n): list {
   mut out = []
   mut i = 0
   while(i < n){
      out = out.append((_z(lb[i]) + _z(ub[i])) / 2)
      i += 1
   }
   out
}

fn _unscale_result_checked(list: result_scaled, list: applied_weights, list: scaled_lb, list: scaled_ub, int: num_ineq): list {
   mut ok = true
   mut result = []
   mut i = 0
   while(i < num_ineq){
      def wi = _z(applied_weights[i])
      def rs = _z(result_scaled[i])
      if(rs < _z(scaled_lb[i]) || rs > _z(scaled_ub[i])){ ok = false }
      result = result.append(wi == 0 ? rs : rs / wi)
      i += 1
   }
   [result, ok]
}

fn _recover_fin_square(list: scaled_mat, list: result_scaled, bool: recover_fin, int: num_var, int: num_ineq): list {
   if(!recover_fin || num_var != num_ineq){ return [] }
   def mm = matrix.Matrix(scaled_mat)
   def det = matrix.matrix_det(mm)
   if(det == Z(0)){ return [] }
   def mt = matrix.matrix_transpose(mm)
   matrix.matrix_solve(mt, result_scaled)
}

fn solve_weighted_bounds(list: mat, list: lb, list: ub, any: weight=0, bool: recover_fin=false): list {
   "rkm-style weighted CVP inequality solve.
   - `mat`: rows are variables, cols are inequality expressions
   - `lb`, `ub`: per-inequality bounds
   Returns `[result_unscaled, applied_weights, result_scaled, ok, fin]`.
   `fin` is recovered variable vector when `recover_fin=true` and
   the matrix is square/invertible."
   def dims = _check_rect(mat)
   def num_var = int(dims[0])
   def num_ineq = int(dims[1])
   if(!is_list(lb) || !is_list(ub) || lb.len != num_ineq || ub.len != num_ineq){ panic("solve_weighted_bounds: bounds length must match matrix column count") }
   def max_element = _mat_max_abs(mat, num_var, num_ineq)
   mut w_base = _z(weight)
   if(w_base <= 0){
      w_base = _z(num_ineq) * max_element
      if(w_base <= 0){ w_base = Z(1) }
   }
   def max_diff = _bounds_max_diff(lb, ub, num_ineq)
   mut scaled_mat = _clone_matrix_rows(mat)
   mut scaled_lb = []
   mut scaled_ub = []
   mut i = 0
   while(i < num_ineq){
      scaled_lb, scaled_ub = scaled_lb.append(_z(lb[i])), scaled_ub.append(_z(ub[i]))
      i += 1
   }
   mut applied_weights = []
   i = 0
   while(i < num_ineq){
      def wi = _weight_for_interval(lb[i], ub[i], w_base, max_diff)
      applied_weights = applied_weights.append(wi)
      mut r = 0
      while(r < num_var){
         mut row = scaled_mat[r]
         row[i] = _z(row[i]) * wi
         scaled_mat[r] = row
         r += 1
      }
      scaled_lb[i] = _z(scaled_lb[i]) * wi
      scaled_ub[i] = _z(scaled_ub[i]) * wi
      i += 1
   }
   def target = _midpoint_vec(scaled_lb, scaled_ub, num_ineq)
   def result_scaled = cvp(scaled_mat, target, true)
   def checked = _unscale_result_checked(result_scaled, applied_weights, scaled_lb, scaled_ub, num_ineq)
   def result = checked[0]
   def ok = checked[1]
   def fin = _recover_fin_square(scaled_mat, result_scaled, recover_fin, num_var, num_ineq)
   [result, applied_weights, result_scaled, ok, fin]
}

fn qary_lattice(list: mat, any: q): list {
   "Build q-ary lattice basis from independent row-space generators."
   def dims = _check_rect(mat)
   def nr = int(dims[0])
   def nc = int(dims[1])
   if(nc < nr){ panic("qary_lattice: column count must be >= row count") }
   def rr = matrix.matrix_rref_mod(matrix.Matrix(mat), q)
   def me = rr[0][2]
   def pivots = rr[1]
   if(pivots.len < nr){ panic("qary_lattice: rows must be independent modulo q") }
   mut L = clone(me)
   def zr = _matrix_zero(nc - nr, nc)
   mut i = 0
   while(i < zr.len){
      L = L.append(zr[i])
      i += 1
   }
   mut row = nr
   mut col = 0
   while(col < nc){
      mut is_pivot = false
      i = 0
      while(i < pivots.len){
         if(int(pivots[i]) == col){
            is_pivot = true
            break
         }
         i += 1
      }
      if(!is_pivot){
         if(row >= L.len){ break }
         mut rrow = L[row]
         rrow[col] = _z(q)
         L[row] = rrow
         row += 1
      }
      col += 1
   }
   L
}

fn reduce_mod_p(list: mat, any: p): list {
   "Return an LLL-reduced short basis for matrix rows modulo prime p."
   def L = qary_lattice(mat, p)
   _cvp_reduce_basis(L)
}

fn solve_multi_mod_linear(list: coeff_rows, list: consts, list: mods, list: lb, list: ub): list {
   "Solve bounded linear equations modulo possibly-different moduli.
   Each equation is: `dot(coeff_rows[i], x) + consts[i] == 0(mod mods[i])`.
   Returns bounded `x` candidate vector."
   if(!is_list(coeff_rows) || coeff_rows.len == 0){ return [] }
   def neq = coeff_rows.len
   def nvars = len(coeff_rows[0])
   if(nvars == 0){ return [] }
   if(!is_list(consts) || !is_list(mods) || consts.len != neq || mods.len != neq){ return [] }
   if(!is_list(lb) || !is_list(ub) || lb.len != nvars || ub.len != nvars){ return [] }
   mut i = 1
   while(i < neq){
      if(len(coeff_rows[i]) != nvars){ return [] }
      i += 1
   }
   mut M = []
   i = 0
   while(i < nvars){
      mut row = []
      mut j = 0
      while(j < neq){
         row = row.append(_z(coeff_rows[j][i]))
         j += 1
      }
      M = M.append(row)
      i += 1
   }
   mut L = []
   i = 0
   while(i < nvars){
      mut row = []
      mut j = 0
      while(j < neq){
         row = row.append(_z(M[i][j]))
         j += 1
      }
      j = 0
      while(j < nvars){
         row = row.append(i == j ? Z(1) : Z(0))
         j += 1
      }
      L = L.append(row)
      i += 1
   }
   i = 0
   while(i < neq){
      mut row = []
      mut j = 0
      while(j < neq){
         row = row.append(i == j ? _z(mods[i]) : Z(0))
         j += 1
      }
      j = 0
      while(j < nvars){
         row = row.append(Z(0))
         j += 1
      }
      L = L.append(row)
      i += 1
   }
   mut lbx, ubx = [], []
   i = 0
   while(i < neq){
      def bi = -_z(consts[i])
      lbx, ubx = lbx.append(bi), ubx.append(bi)
      i += 1
   }
   i = 0
   while(i < nvars){
      lbx, ubx = lbx.append(_z(lb[i])), ubx.append(_z(ub[i]))
      i += 1
   }
   def solved = solve_weighted_bounds(L, lbx, ubx)
   def vals = solved[0]
   if(vals.len != neq + nvars){ return [] }
   _tail_vec(vals, neq, nvars)
}

fn solve_underconstrained_linear(list: mat, list: target, list: lb, list: ub): list {
   "Solve underconstrained linear equations via CVP embedding.
   `mat` shape is [num_vars x num_eq], finds bounded vars `x` such that
   `x * mat == target` with `lb <= x <= ub`."
   def dims = _check_rect(mat)
   def num_var = int(dims[0])
   def num_eq = int(dims[1])
   _cvp_trace("underconstrained:start", {"num_var": num_var, "num_eq": num_eq, "target_len": is_list(target) ? target.len : -1, "lb_len": is_list(lb) ? lb.len : -1, "ub_len": is_list(ub) ? ub.len : -1})
   if(!is_list(target) || target.len != num_eq){ return [] }
   if(!is_list(lb) || !is_list(ub) || lb.len != num_var || ub.len != num_var){ return [] }
   if(num_eq >= num_var){ return [] }
   mut L, i = [], 0
   while(i < num_var){
      def row0 = mat[i]
      mut row = []
      mut j = 0
      while(j < num_eq){
         row = row.append(_z(row0[j]))
         j += 1
      }
      j = 0
      while(j < num_var){
         row = row.append(i == j ? Z(1) : Z(0))
         j += 1
      }
      L = L.append(row)
      i += 1
   }
   mut trow = []
   i = 0
   while(i < num_eq){
      trow = trow.append(_z(target[i]))
      i += 1
   }
   i = 0
   while(i < num_var){
      trow = trow.append(Z(0))
      i += 1
   }
   L = L.append(trow)
   mut lbx, ubx = [], []
   i = 0
   while(i < num_eq){
      lbx, ubx = lbx.append(Z(0)), ubx.append(Z(0))
      i += 1
   }
   i = 0
   while(i < num_var){
      lbx, ubx = lbx.append(_z(lb[i])), ubx.append(_z(ub[i]))
      i += 1
   }
   def sol = solve_inequality(L, lbx, ubx)
   _cvp_trace("underconstrained:solution", {"dim": is_list(sol) ? sol.len : -1, "expected": num_eq + num_var})
   if(!is_list(sol) || sol.len != num_eq + num_var){ return [] }
   _tail_vec(sol, num_eq, num_var)
}

fn _enum_check(list: v, any: lb, any: ub): bool {
   mut i = 0
   while(i < v.len){
      def x = _z(v[i])
      if(lb != nil && x < _z(_at(lb, i, x))){ return false }
      if(ub != nil && x > _z(_at(ub, i, x))){ return false }
      i += 1
   }
   true
}

fn _enum_brute_dfs(int: idx, int: k, int: n, list: coeffs, list: base, list: basis, any: lb, any: ub, list: out): list {
   if(idx >= k){
      mut v, i = clone(base), 0
      while(i < k){
         v = _vec_add_scaled(v, basis[i], int(coeffs[i]))
         i += 1
      }
      if(_enum_check(v, lb, ub)){ out = out.append(v) }
      return out
   }
   mut c = -int(n)
   while(c <= int(n)){
      coeffs[idx] = c
      out = _enum_brute_dfs(idx + 1, k, n, coeffs, base, basis, lb, ub, out)
      c += 1
   }
   out
}

fn enum_brute(any: base, list: basis, any: lb=nil, any: ub=nil, int: n=5): list {
   "Enumerate `v = base + c@basis` with integer coefficients in [-n, n].
   Returns all vectors that satisfy optional bounds."
   if(!is_list(basis) || basis.len == 0){ return [] }
   def dim = len(basis[0])
   mut i = 0
   while(i < basis.len){
      if(len(basis[i]) != dim){ return [] }
      i += 1
   }
   def k = basis.len
   mut origin = base
   if(origin == nil){
      origin = []
      i = 0
      while(i < dim){
         origin = origin.append(Z(0))
         i += 1
      }
   }
   mut coeffs = []
   i = 0
   while(i < k){
      coeffs = coeffs.append(0)
      i += 1
   }
   _enum_brute_dfs(0, k, int(n), coeffs, origin, basis, lb, ub, [])
}

fn mod_arc_is_inside(any: L, any: R, any: M, any: val): bool {
   "Return true when `val` lies in modular arc [L, R] over modulus M."
   def mm = _z(M)
   if(mm <= 0){ return false }
   def l, r = mod(_z(L), mm), mod(_z(R), mm)
   def v = mod(_z(val), mm)
   if(l <= r){ return l <= v && v <= r }
   v >= l || v <= r
}

fn mod_arc_has_solution(any: A, any: M, any: L, any: R): bool {
   "Check if modular interval equation L <= A*x(mod M) <= R can have a solution."
   def l, r = _z(L), _z(R)
   if(l == 0 || l > r){ return true }
   def g = gcd(_z(A), _z(M))
   ((l - 1) / g) != (r / g)
}

fn mod_arc_optf(any: A, any: M, any: L, any: R): bigint {
   "Minimum nonnegative x such that L <= A*x(mod M) <= R.
   Assumes the interval does not wrap(L <= R)."
   def az, mz = _z(A), _z(M)
   mut l, r = _z(L), _z(R)
   if(l == 0){ return Z(0) }
   if(az * 2 > mz){
      def l0, r0 = l, r
      l, r = r0, l0
      def az2 = mz - az
      return mod_arc_optf(az2, mz, mz - l, mz - r)
   }
   def c1 = _ceil_div(l, az)
   if(az * c1 <= r){ return c1 }
   def c2 = mod_arc_optf(az - mod(mz, az), az, mod(l, az), mod(r, az))
   _ceil_div(l + mz * c2, az)
}

fn mod_arc_solve_range(any: A, any: M, any: L, any: R, any: S, any: E, int: max_solutions=0): list {
   "Find all x in [S, E] satisfying L <= A*x(mod M) <= R."
   if(!mod_arc_has_solution(A, M, L, R)){ return [] }
   def az, mz = _z(A), _z(M)
   def l, r = mod(_z(L), mz), mod(_z(R), mz)
   mut cur = _z(S) - Z(1)
   def end = _z(E)
   mut ans = []
   while(cur <= end){
      def nl, nr = mod(l - az * (cur + Z(1)), mz), mod(r - az * (cur + Z(1)), mz)
      if(nl > nr){ cur = cur + Z(1) } else {
         def step = mod_arc_optf(az, mz, nl, nr)
         cur = cur + Z(1) + step
      }
      if(cur <= end && mod_arc_is_inside(l, r, mz, az * cur)){
         ans = ans.append(cur)
         if(max_solutions > 0 && ans.len >= int(max_solutions)){ break }
      }
   }
   ans
}
